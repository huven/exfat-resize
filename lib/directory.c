/* SPDX-License-Identifier: MIT */

#include "common.h"

#include "directory.h"
#include "endian.h"
#include "resize_internal.h"
#include "stream.h"
#include "volume.h"

#include <string.h>

enum {
	EXFAT_DIRECTORY_ENTRY_SIZE = 32,

	EXFAT_ENTRY_BITMAP = 0x81,
	EXFAT_ENTRY_UPCASE = 0x82,
	EXFAT_ENTRY_VOLUME_LABEL = 0x83,
	EXFAT_ENTRY_FILE = 0x85,
	EXFAT_ENTRY_VOLUME_GUID = 0xa0,
	EXFAT_ENTRY_STREAM = 0xc0,
	EXFAT_ENTRY_FILE_NAME = 0xc1,
	EXFAT_ENTRY_VENDOR_EXTENSION = 0xe0,
	EXFAT_ENTRY_VENDOR_ALLOCATION = 0xe1,

	EXFAT_ENTRY_IN_USE = 0x80,
	EXFAT_ENTRY_SECONDARY = 0x40,
	EXFAT_ENTRY_BENIGN = 0x20,

	EXFAT_ALLOCATION_POSSIBLE = 0x01,
	EXFAT_NO_FAT_CHAIN = 0x02,
	EXFAT_DIRECTORY_ATTRIBUTE = 0x10
};

enum {
	EXFAT_PRIMARY_SECONDARY_COUNT_OFFSET = 1,
	EXFAT_PRIMARY_FLAGS_OFFSET = 4,

	EXFAT_FILE_SECONDARY_COUNT_OFFSET = 1,
	EXFAT_FILE_CHECKSUM_OFFSET = 2,
	EXFAT_FILE_ATTRIBUTES_OFFSET = 4,

	EXFAT_STREAM_FLAGS_OFFSET = 1,
	EXFAT_STREAM_VALID_LENGTH_OFFSET = 8,
	EXFAT_STREAM_FIRST_CLUSTER_OFFSET = 20,
	EXFAT_STREAM_DATA_LENGTH_OFFSET = 24,

	EXFAT_BITMAP_FLAGS_OFFSET = 1,
	EXFAT_BITMAP_FIRST_CLUSTER_OFFSET = 20,
	EXFAT_BITMAP_DATA_LENGTH_OFFSET = 24,

	EXFAT_UPCASE_FIRST_CLUSTER_OFFSET = 20,
	EXFAT_UPCASE_DATA_LENGTH_OFFSET = 24,

	EXFAT_SECONDARY_FLAGS_OFFSET = 1,
	EXFAT_SECONDARY_FIRST_CLUSTER_OFFSET = 20
};

struct buffered_directory_entry {
	unsigned char data[EXFAT_DIRECTORY_ENTRY_SIZE];
	struct directory_location location;
};

_Static_assert(UINT8_MAX * sizeof(struct buffered_directory_entry) <= EXFAT_IO_BUFFER_SIZE,
    "the I/O buffer must hold the maximum file secondary-entry set");

static enum exfat_resize_error read_directory_entry(struct resize_directory *context,
    struct stream_cursor *cursor,
    unsigned char entry[EXFAT_DIRECTORY_ENTRY_SIZE],
    struct directory_location *location)
{
	enum exfat_resize_error error;
	uint64_t cluster_start;

	if (cursor->exhausted ||
	    (!cursor->root_directory && cursor->remaining_bytes < EXFAT_DIRECTORY_ENTRY_SIZE))
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	if (cursor->cluster_offset % EXFAT_DIRECTORY_ENTRY_SIZE != 0)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;

	error = exfat_resize_cluster_sector(cursor->geometry, cursor->current_cluster, &cluster_start);
	if (error != EXFAT_RESIZE_SUCCESS)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	if (location != NULL) {
		location->sector = cluster_start + cursor->cluster_offset / context->volume->sector_size;
		location->offset = (size_t)(cursor->cluster_offset % context->volume->sector_size);
	}
	return exfat_resize_read_stream(context->volume, cursor, entry, EXFAT_DIRECTORY_ENTRY_SIZE);
}

static enum exfat_resize_error write_directory_entry(struct resize_directory *context,
    const struct directory_location *location,
    const unsigned char entry[EXFAT_DIRECTORY_ENTRY_SIZE])
{
	struct sector_cache *cache = &context->volume->caches[SECTOR_CACHE_TARGET_DIRECTORY_DATA];
	unsigned char *destination;
	enum exfat_resize_error error;
	uint32_t cache_sector;

	error = exfat_resize_load_cache(
	    context->volume, SECTOR_CACHE_TARGET_DIRECTORY_DATA, location->sector, 1);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	cache_sector = (uint32_t)(location->sector - cache->first_sector);
	destination =
	    cache->data + (size_t)cache_sector * context->volume->sector_size + location->offset;
	if (memcmp(destination, entry, EXFAT_DIRECTORY_ENTRY_SIZE) == 0)
		return EXFAT_RESIZE_SUCCESS;
	error = exfat_resize_prepare_cached_write(
	    context->volume, SECTOR_CACHE_TARGET_DIRECTORY_DATA, location->sector, 1);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	memcpy(destination, entry, EXFAT_DIRECTORY_ENTRY_SIZE);
	if (cache->dirty_first == cache->dirty_end) {
		cache->dirty_first = cache_sector;
		cache->dirty_end = cache_sector + 1;
	} else {
		if (cache_sector < cache->dirty_first)
			cache->dirty_first = cache_sector;
		if (cache_sector >= cache->dirty_end)
			cache->dirty_end = cache_sector + 1;
	}
	return EXFAT_RESIZE_SUCCESS;
}

static uint16_t update_entry_checksum(uint16_t checksum, unsigned char value)
{
	uint16_t rotated = (uint16_t)(((uint32_t)checksum << 15) | ((uint32_t)checksum >> 1));
	return (uint16_t)(rotated + value);
}

static uint16_t checksum_entry(
    uint16_t checksum, const unsigned char entry[EXFAT_DIRECTORY_ENTRY_SIZE], int primary)
{
	size_t index;

	for (index = 0; index < EXFAT_DIRECTORY_ENTRY_SIZE; ++index) {
		if (!primary ||
		    (index != EXFAT_FILE_CHECKSUM_OFFSET && index != EXFAT_FILE_CHECKSUM_OFFSET + 1))
			checksum = update_entry_checksum(checksum, entry[index]);
	}
	return checksum;
}

static enum exfat_resize_error read_and_validate_file_entry_set(struct resize_directory *context,
    struct stream_cursor *cursor,
    const unsigned char primary[EXFAT_DIRECTORY_ENTRY_SIZE],
    uint8_t secondary_count,
    struct buffered_directory_entry **secondary_entries)
{
	struct buffered_directory_entry *entries =
	    (struct buffered_directory_entry *)context->volume->io_buffer;
	enum exfat_resize_error error;
	uint16_t calculated_checksum;
	uint16_t stored_checksum;
	uint32_t index;

	error = exfat_resize_load_le16(
	    primary, EXFAT_DIRECTORY_ENTRY_SIZE, EXFAT_FILE_CHECKSUM_OFFSET, &stored_checksum);
	if (error != EXFAT_RESIZE_SUCCESS)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	calculated_checksum = checksum_entry(0, primary, 1);
	for (index = 0; index < secondary_count; ++index) {
		error =
		    read_directory_entry(context, cursor, entries[index].data, &entries[index].location);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		calculated_checksum = checksum_entry(calculated_checksum, entries[index].data, 0);
	}
	if (calculated_checksum != stored_checksum)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	*secondary_entries = entries;
	return EXFAT_RESIZE_SUCCESS;
}

static enum exfat_resize_error allocation_from_entry(
    const unsigned char entry[EXFAT_DIRECTORY_ENTRY_SIZE],
    size_t flags_offset,
    size_t first_cluster_offset,
    size_t data_length_offset,
    struct allocation_stream *stream)
{
	enum exfat_resize_error error;

	memset(stream, 0, sizeof(*stream));
	stream->no_fat_chain = (entry[flags_offset] & EXFAT_NO_FAT_CHAIN) != 0;
	error = exfat_resize_load_le32(
	    entry, EXFAT_DIRECTORY_ENTRY_SIZE, first_cluster_offset, &stream->first_cluster);
	if (error != EXFAT_RESIZE_SUCCESS)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	error = exfat_resize_load_le64(
	    entry, EXFAT_DIRECTORY_ENTRY_SIZE, data_length_offset, &stream->data_length);
	if (error != EXFAT_RESIZE_SUCCESS)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	return EXFAT_RESIZE_SUCCESS;
}

static enum exfat_resize_error validate_replacement(struct resize_volume *volume,
    const struct exfat_resize_geometry *geometry,
    const struct allocation_stream *stream)
{
	uint32_t count;
	enum exfat_resize_error error;

	if (stream->root_directory)
		return EXFAT_RESIZE_INVALID_ARGUMENT;
	error = exfat_resize_stream_cluster_count(volume->cluster_size, stream, &count);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	if (count == 0)
		return stream->first_cluster == 0 && !stream->no_fat_chain ? EXFAT_RESIZE_SUCCESS
		                                                           : EXFAT_RESIZE_INVALID_ARGUMENT;
	if (count > geometry->cluster_count ||
	    !exfat_resize_cluster_is_valid(geometry, stream->first_cluster) ||
	    (stream->no_fat_chain &&
	        (uint64_t)stream->first_cluster + count > (uint64_t)geometry->cluster_count + 2))
		return EXFAT_RESIZE_INVALID_ARGUMENT;
	return EXFAT_RESIZE_SUCCESS;
}

static enum exfat_resize_error store_stream_allocation(
    unsigned char entry[EXFAT_DIRECTORY_ENTRY_SIZE], const struct allocation_stream *stream)
{
	enum exfat_resize_error error;
	/* The caller has validated the replacement and required an unchanged length. */
	error = exfat_resize_store_le32(entry, EXFAT_DIRECTORY_ENTRY_SIZE,
	    EXFAT_STREAM_FIRST_CLUSTER_OFFSET, stream->first_cluster);
	if (stream->no_fat_chain)
		entry[EXFAT_STREAM_FLAGS_OFFSET] |= EXFAT_NO_FAT_CHAIN;
	else
		entry[EXFAT_STREAM_FLAGS_OFFSET] &= (unsigned char)~EXFAT_NO_FAT_CHAIN;
	return error;
}

static enum exfat_resize_error push_directory(
    struct resize_directory *context, const struct allocation_stream *directory, int rewriting)
{
	struct directory_worklist *worklist = &context->worklist;
	struct allocation_stream *items;
	size_t capacity;
	size_t size;

	if (worklist->count < worklist->capacity) {
		worklist->items[worklist->count++] = *directory;
		return EXFAT_RESIZE_SUCCESS;
	}
	/*
	 * Rewrite follows the same tree and worklist order as validation, so its
	 * preflight-established capacity must already be sufficient.
	 */
	if (rewriting)
		return EXFAT_RESIZE_INTERNAL_ERROR;
	if (worklist->capacity > SIZE_MAX / 2)
		return EXFAT_RESIZE_ARITHMETIC_OVERFLOW;
	capacity = worklist->capacity == 0 ? 1 : worklist->capacity * 2;
	if (capacity > SIZE_MAX / sizeof(*items))
		return EXFAT_RESIZE_ARITHMETIC_OVERFLOW;
	size = capacity * sizeof(*items);
	items = context->volume->operation->allocator.allocate(
	    context->volume->operation->allocator.context, size);
	if (items == NULL)
		return EXFAT_RESIZE_OUT_OF_MEMORY;
	/*
	 * Keep this preflight-only growth as one copy. Directory traversal owns
	 * cancellation checkpoints; chunking would only benefit worklists with
	 * millions of pending directories.
	 */
	if (worklist->count != 0)
		memcpy(items, worklist->items, worklist->count * sizeof(*items));
	if (worklist->items != NULL) {
		context->volume->operation->allocator.deallocate(
		    context->volume->operation->allocator.context, worklist->items,
		    worklist->capacity * sizeof(*items));
	}
	worklist->items = items;
	worklist->capacity = capacity;
	worklist->items[worklist->count++] = *directory;
	return EXFAT_RESIZE_SUCCESS;
}

static enum exfat_resize_error scan_file_entry_set(struct resize_directory *context,
    struct stream_cursor *cursor,
    unsigned char primary[EXFAT_DIRECTORY_ENTRY_SIZE],
    const struct directory_location *primary_location,
    const struct directory_scan *scan,
    const struct directory_reference *owner,
    int queue_children)
{
	int rewriting = scan->transform != NULL;
	struct allocation_stream source_stream;
	struct allocation_stream target_stream;
	struct buffered_directory_entry *secondary_entries;
	unsigned char *secondary;
	enum exfat_resize_error error;
	uint64_t valid_data_length;
	uint16_t attributes;
	uint16_t calculated_checksum = 0;
	uint8_t secondary_count;
	uint32_t index;
	int is_directory;

	secondary_count = primary[EXFAT_FILE_SECONDARY_COUNT_OFFSET];
	if (secondary_count < 2)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	error = read_and_validate_file_entry_set(
	    context, cursor, primary, secondary_count, &secondary_entries);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = exfat_resize_load_le16(
	    primary, EXFAT_DIRECTORY_ENTRY_SIZE, EXFAT_FILE_ATTRIBUTES_OFFSET, &attributes);
	if (error != EXFAT_RESIZE_SUCCESS)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	is_directory = (attributes & EXFAT_DIRECTORY_ATTRIBUTE) != 0;

	for (index = 0; index < secondary_count; ++index) {
		secondary = secondary_entries[index].data;
		if ((secondary[0] & (EXFAT_ENTRY_IN_USE | EXFAT_ENTRY_SECONDARY)) !=
		    (EXFAT_ENTRY_IN_USE | EXFAT_ENTRY_SECONDARY))
			return EXFAT_RESIZE_INVALID_FILESYSTEM;

		if (index == 0 && secondary[0] != EXFAT_ENTRY_STREAM)
			return EXFAT_RESIZE_INVALID_FILESYSTEM;
		if (secondary[0] == EXFAT_ENTRY_STREAM) {
			if (index != 0)
				return EXFAT_RESIZE_INVALID_FILESYSTEM;
			if ((secondary[EXFAT_STREAM_FLAGS_OFFSET] & EXFAT_ALLOCATION_POSSIBLE) == 0)
				return EXFAT_RESIZE_INVALID_FILESYSTEM;
			error = exfat_resize_load_le64(secondary, EXFAT_DIRECTORY_ENTRY_SIZE,
			    EXFAT_STREAM_VALID_LENGTH_OFFSET, &valid_data_length);
			if (error != EXFAT_RESIZE_SUCCESS)
				return EXFAT_RESIZE_INVALID_FILESYSTEM;
			error = allocation_from_entry(secondary, EXFAT_STREAM_FLAGS_OFFSET,
			    EXFAT_STREAM_FIRST_CLUSTER_OFFSET, EXFAT_STREAM_DATA_LENGTH_OFFSET, &source_stream);
			if (error != EXFAT_RESIZE_SUCCESS)
				return error;

			if (is_directory && source_stream.data_length > EXFAT_MAX_DIRECTORY_SIZE)
				return EXFAT_RESIZE_INVALID_FILESYSTEM;
			if (source_stream.no_fat_chain && source_stream.data_length == 0)
				return EXFAT_RESIZE_INVALID_FILESYSTEM;
			if (valid_data_length > source_stream.data_length ||
			    (is_directory && valid_data_length != source_stream.data_length))
				return EXFAT_RESIZE_INVALID_FILESYSTEM;
		} else if (secondary[0] == EXFAT_ENTRY_VENDOR_ALLOCATION) {
			return EXFAT_RESIZE_UNSUPPORTED_VENDOR_ALLOCATION;
		} else if (secondary[0] == EXFAT_ENTRY_FILE_NAME ||
		    secondary[0] == EXFAT_ENTRY_VENDOR_EXTENSION) {
			if ((secondary[EXFAT_SECONDARY_FLAGS_OFFSET] & EXFAT_ALLOCATION_POSSIBLE) != 0)
				return EXFAT_RESIZE_INVALID_FILESYSTEM;
		} else {
			if ((secondary[0] & EXFAT_ENTRY_BENIGN) == 0)
				return EXFAT_RESIZE_UNSUPPORTED_CRITICAL_ENTRY;
			if ((secondary[EXFAT_SECONDARY_FLAGS_OFFSET] & EXFAT_ALLOCATION_POSSIBLE) != 0) {
				uint32_t first_cluster;

				error = exfat_resize_load_le32(secondary, EXFAT_DIRECTORY_ENTRY_SIZE,
				    EXFAT_SECONDARY_FIRST_CLUSTER_OFFSET, &first_cluster);
				if (error != EXFAT_RESIZE_SUCCESS)
					return EXFAT_RESIZE_INVALID_FILESYSTEM;
				if (first_cluster >= 2)
					return EXFAT_RESIZE_UNSUPPORTED_ALLOCATED_ENTRY;
			}
		}
	}

	target_stream = source_stream;
	if (rewriting) {
		error = scan->transform(scan->context, owner, &source_stream, is_directory, &target_stream);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		if (target_stream.data_length != source_stream.data_length)
			return EXFAT_RESIZE_INVALID_ARGUMENT;
		error = validate_replacement(context->volume, scan->geometry, &target_stream);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		error = store_stream_allocation(secondary_entries[0].data, &target_stream);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		calculated_checksum = checksum_entry(0, primary, 1);
		for (index = 0; index < secondary_count; ++index) {
			secondary = secondary_entries[index].data;
			calculated_checksum = checksum_entry(calculated_checksum, secondary, 0);
			error = write_directory_entry(context, &secondary_entries[index].location, secondary);
			if (error != EXFAT_RESIZE_SUCCESS)
				return error;
		}
		error = exfat_resize_store_le16(
		    primary, EXFAT_DIRECTORY_ENTRY_SIZE, EXFAT_FILE_CHECKSUM_OFFSET, calculated_checksum);
		if (error != EXFAT_RESIZE_SUCCESS)
			return EXFAT_RESIZE_INTERNAL_ERROR;
		error = write_directory_entry(context, primary_location, primary);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
	} else if (scan->visit != NULL) {
		error = scan->visit(scan->context, owner, &source_stream, is_directory);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
	}

	if (queue_children && is_directory && target_stream.data_length != 0) {
		struct allocation_stream child = rewriting ? target_stream : source_stream;

		child.root_directory = 0;
		return push_directory(context, &child, rewriting);
	}
	return EXFAT_RESIZE_SUCCESS;
}

static enum exfat_resize_error store_system_allocation(struct resize_directory *context,
    unsigned char entry[EXFAT_DIRECTORY_ENTRY_SIZE],
    const struct directory_location *location,
    const struct allocation_stream *replacement)
{
	enum exfat_resize_error error;
	if (replacement->root_directory || replacement->no_fat_chain ||
	    replacement->first_cluster < 2 || replacement->data_length == 0)
		return EXFAT_RESIZE_INVALID_ARGUMENT;
	error = exfat_resize_store_le32(entry, EXFAT_DIRECTORY_ENTRY_SIZE,
	    EXFAT_BITMAP_FIRST_CLUSTER_OFFSET, replacement->first_cluster);
	if (error == EXFAT_RESIZE_SUCCESS && entry[0] == EXFAT_ENTRY_BITMAP)
		error = exfat_resize_store_le64(entry, EXFAT_DIRECTORY_ENTRY_SIZE,
		    EXFAT_BITMAP_DATA_LENGTH_OFFSET, replacement->data_length);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	return write_directory_entry(context, location, entry);
}

static enum exfat_resize_error scan_system_entry(struct resize_directory *context,
    unsigned char entry[EXFAT_DIRECTORY_ENTRY_SIZE],
    const struct directory_location *location,
    const struct directory_scan *scan,
    const struct directory_reference *owner)
{
	struct allocation_stream stream = { 0 };
	struct allocation_stream replacement;
	enum exfat_resize_error error;
	if (owner->kind == DIRECTORY_OWNER_BITMAP && entry[EXFAT_BITMAP_FLAGS_OFFSET] != 0)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	error = exfat_resize_load_le32(entry, EXFAT_DIRECTORY_ENTRY_SIZE,
	    EXFAT_BITMAP_FIRST_CLUSTER_OFFSET, &stream.first_cluster);
	if (error == EXFAT_RESIZE_SUCCESS)
		error = exfat_resize_load_le64(entry, EXFAT_DIRECTORY_ENTRY_SIZE,
		    EXFAT_BITMAP_DATA_LENGTH_OFFSET, &stream.data_length);
	if (error != EXFAT_RESIZE_SUCCESS || stream.first_cluster < 2 || stream.data_length == 0)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	if (scan->transform != NULL) {
		replacement = stream;
		error = scan->transform(scan->context, owner, &stream, 0, &replacement);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		if (owner->kind == DIRECTORY_OWNER_UPCASE && replacement.data_length != stream.data_length)
			return EXFAT_RESIZE_INVALID_ARGUMENT;
		error = validate_replacement(context->volume, scan->geometry, &replacement);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		if (owner->kind == DIRECTORY_OWNER_BITMAP &&
		    replacement.data_length < ((uint64_t)scan->geometry->cluster_count + 7) / 8)
			return EXFAT_RESIZE_INVALID_ARGUMENT;
		return store_system_allocation(context, entry, location, &replacement);
	}
	if (owner->kind == DIRECTORY_OWNER_BITMAP &&
	    stream.data_length < ((uint64_t)scan->geometry->cluster_count + 7) / 8)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	if (scan->visit != NULL) {
		error = scan->visit(scan->context, owner, &stream, 0);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
	}
	if (owner->kind == DIRECTORY_OWNER_BITMAP)
		context->bitmap_location = *location;
	return EXFAT_RESIZE_SUCCESS;
}

enum exfat_resize_error exfat_resize_rewrite_bitmap_entry(struct resize_directory *context,
    const struct directory_location *location,
    const struct allocation_stream *replacement)
{
	struct sector_cache *cache = &context->volume->caches[SECTOR_CACHE_TARGET_DIRECTORY_DATA];
	unsigned char entry[EXFAT_DIRECTORY_ENTRY_SIZE];
	enum exfat_resize_error error;
	if (location->offset > context->volume->sector_size - EXFAT_DIRECTORY_ENTRY_SIZE)
		return EXFAT_RESIZE_INTERNAL_ERROR;
	error = exfat_resize_load_cache(
	    context->volume, SECTOR_CACHE_TARGET_DIRECTORY_DATA, location->sector, 1);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	memcpy(entry,
	    cache->data +
	        (size_t)(location->sector - cache->first_sector) * context->volume->sector_size +
	        location->offset,
	    sizeof(entry));
	if (entry[0] != EXFAT_ENTRY_BITMAP || entry[EXFAT_BITMAP_FLAGS_OFFSET] != 0)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	return store_system_allocation(context, entry, location, replacement);
}

static enum exfat_resize_error scan_one_directory(struct resize_directory *context,
    const struct allocation_stream *directory,
    const struct directory_scan *scan)
{
	struct directory_location location;
	struct stream_cursor cursor;
	unsigned char entry[EXFAT_DIRECTORY_ENTRY_SIZE];
	enum exfat_resize_error error;

	struct directory_reference owner = { .directory_id = directory->first_cluster };
	error = exfat_resize_initialize_stream_cursor(scan->geometry, directory,
	    scan->transform != NULL ? SECTOR_CACHE_TARGET_DIRECTORY_DATA
	                            : SECTOR_CACHE_SOURCE_DIRECTORY_DATA,
	    &scan->chain, &cursor);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;

	while (!cursor.exhausted) {
		owner.entry_offset = (uint64_t)cursor.traversed_clusters * context->volume->cluster_size +
		    cursor.cluster_offset;
		error = read_directory_entry(context, &cursor, entry, &location);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		if (entry[0] == 0)
			break;
		if ((entry[0] & EXFAT_ENTRY_IN_USE) == 0)
			continue;
		if ((entry[0] & EXFAT_ENTRY_SECONDARY) != 0)
			return EXFAT_RESIZE_INVALID_FILESYSTEM;

		switch (entry[0]) {
		case EXFAT_ENTRY_BITMAP:
			if (!directory->root_directory)
				return EXFAT_RESIZE_INVALID_FILESYSTEM;
			owner.kind = DIRECTORY_OWNER_BITMAP;
			error = scan_system_entry(context, entry, &location, scan, &owner);
			break;
		case EXFAT_ENTRY_UPCASE:
			if (!directory->root_directory)
				return EXFAT_RESIZE_INVALID_FILESYSTEM;
			owner.kind = DIRECTORY_OWNER_UPCASE;
			error = scan_system_entry(context, entry, &location, scan, &owner);
			break;
		case EXFAT_ENTRY_VOLUME_LABEL:
			error =
			    directory->root_directory ? EXFAT_RESIZE_SUCCESS : EXFAT_RESIZE_INVALID_FILESYSTEM;
			break;
		case EXFAT_ENTRY_FILE:
			owner.kind = DIRECTORY_OWNER_FILE;
			error = scan_file_entry_set(context, &cursor, entry, &location, scan, &owner, 1);
			break;
		case EXFAT_ENTRY_VOLUME_GUID:
			if (!directory->root_directory || entry[EXFAT_PRIMARY_SECONDARY_COUNT_OFFSET] != 0 ||
			    (entry[EXFAT_PRIMARY_FLAGS_OFFSET] &
			        (EXFAT_ALLOCATION_POSSIBLE | EXFAT_NO_FAT_CHAIN)) != 0)
				return EXFAT_RESIZE_INVALID_FILESYSTEM;
			error = EXFAT_RESIZE_SUCCESS;
			break;
		default:
			/*
			 * Revision 1.00 defines no other primary directory-entry types.
			 * Revisit this rejection before accepting later minor revisions:
			 * future benign primaries may declare checksummed secondary entry
			 * sets that must be consumed and preserved as a unit.
			 */
			error = (entry[0] & EXFAT_ENTRY_BENIGN) == 0 ? EXFAT_RESIZE_UNSUPPORTED_CRITICAL_ENTRY
			                                             : EXFAT_RESIZE_INVALID_FILESYSTEM;
			break;
		}
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
	}
	return EXFAT_RESIZE_SUCCESS;
}

enum exfat_resize_error exfat_resize_scan_directory_tree(struct resize_directory *context,
    const struct allocation_stream *root,
    const struct directory_scan *scan)
{
	struct allocation_stream directory;
	enum exfat_resize_error error;

	context->worklist.count = 0;
	error = push_directory(context, root, scan->transform != NULL);
	while (error == EXFAT_RESIZE_SUCCESS && context->worklist.count != 0) {
		directory = context->worklist.items[--context->worklist.count];
		error = scan_one_directory(context, &directory, scan);
	}
	return error;
}

void exfat_resize_release_directory(struct resize_directory *context)
{
	if (context->worklist.items != NULL) {
		context->volume->operation->allocator.deallocate(
		    context->volume->operation->allocator.context, context->worklist.items,
		    context->worklist.capacity * sizeof(*context->worklist.items));
		context->worklist.items = NULL;
	}
}

struct targeted_edit {
	const struct allocation_stream *expected;
	const struct allocation_stream *replacement;
};

static enum exfat_resize_error replace_observed_allocation(void *context,
    const struct directory_reference *owner,
    const struct allocation_stream *stream,
    int is_directory,
    struct allocation_stream *replacement)
{
	const struct targeted_edit *edit = context;
	(void)owner;
	(void)is_directory;
	if (stream->first_cluster != edit->expected->first_cluster ||
	    stream->data_length != edit->expected->data_length ||
	    stream->no_fat_chain != edit->expected->no_fat_chain ||
	    stream->root_directory != edit->expected->root_directory)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	*replacement = *edit->replacement;
	return EXFAT_RESIZE_SUCCESS;
}

enum exfat_resize_error exfat_resize_edit_directory_allocation(struct resize_directory *context,
    const struct directory_access *access,
    const struct directory_reference *owner,
    const struct allocation_stream *expected,
    const struct allocation_stream *replacement)
{
	struct targeted_edit edit = { .expected = expected, .replacement = replacement };
	struct directory_scan scan = { .geometry = access->geometry,
		.chain = access->chain,
		.context = &edit,
		.transform = replace_observed_allocation };
	struct allocation_stream directory;
	struct stream_cursor cursor;
	struct directory_location location;
	unsigned char entry[EXFAT_DIRECTORY_ENTRY_SIZE];
	enum exfat_resize_error error;

	if (access->resolve == NULL || owner->kind == DIRECTORY_OWNER_ROOT ||
	    owner->entry_offset % EXFAT_DIRECTORY_ENTRY_SIZE != 0 ||
	    owner->entry_offset >= EXFAT_MAX_DIRECTORY_SIZE)
		return EXFAT_RESIZE_INVALID_ARGUMENT;
	error = access->resolve(access->context, owner->directory_id, &directory);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	if (owner->kind != DIRECTORY_OWNER_FILE && !directory.root_directory)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	error = exfat_resize_initialize_stream_cursor(
	    access->geometry, &directory, SECTOR_CACHE_TARGET_DIRECTORY_DATA, &access->chain, &cursor);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = exfat_resize_skip_stream(context->volume, &cursor, owner->entry_offset);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = read_directory_entry(context, &cursor, entry, &location);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	if (owner->kind == DIRECTORY_OWNER_FILE && entry[0] == EXFAT_ENTRY_FILE)
		return scan_file_entry_set(context, &cursor, entry, &location, &scan, owner, 0);
	if ((owner->kind == DIRECTORY_OWNER_BITMAP && entry[0] == EXFAT_ENTRY_BITMAP) ||
	    (owner->kind == DIRECTORY_OWNER_UPCASE && entry[0] == EXFAT_ENTRY_UPCASE))
		return scan_system_entry(context, entry, &location, &scan, owner);
	return EXFAT_RESIZE_INVALID_FILESYSTEM;
}
