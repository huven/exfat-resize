/* SPDX-License-Identifier: MIT */

#include "common.h"

#include "allocation.h"
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

static enum exfat_resize_error read_directory_entry(struct resize_context *context,
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
		location->sector = cluster_start + cursor->cluster_offset / context->sector_size;
		location->offset = (size_t)(cursor->cluster_offset % context->sector_size);
	}
	return exfat_resize_read_stream(context, cursor, entry, EXFAT_DIRECTORY_ENTRY_SIZE);
}

static enum exfat_resize_error write_directory_entry(struct resize_context *context,
    const struct directory_location *location,
    const unsigned char entry[EXFAT_DIRECTORY_ENTRY_SIZE])
{
	struct sector_cache *cache = &context->caches[SECTOR_CACHE_TARGET_DIRECTORY_DATA];
	unsigned char *destination;
	enum exfat_resize_error error;
	uint32_t cache_sector;

	error =
	    exfat_resize_load_cache(context, SECTOR_CACHE_TARGET_DIRECTORY_DATA, location->sector, 1);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	cache_sector = (uint32_t)(location->sector - cache->first_sector);
	destination = cache->data + (size_t)cache_sector * context->sector_size + location->offset;
	if (memcmp(destination, entry, EXFAT_DIRECTORY_ENTRY_SIZE) == 0)
		return EXFAT_RESIZE_SUCCESS;
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

static enum exfat_resize_error read_and_validate_file_entry_set(struct resize_context *context,
    struct stream_cursor *cursor,
    const unsigned char primary[EXFAT_DIRECTORY_ENTRY_SIZE],
    uint8_t secondary_count,
    struct buffered_directory_entry **secondary_entries)
{
	struct buffered_directory_entry *entries =
	    (struct buffered_directory_entry *)context->io_buffer;
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

/* The caller checks the Stream Extension type and AllocationPossible flag. */
static enum exfat_resize_error rewrite_stream_allocation(struct resize_context *context,
    unsigned char entry[EXFAT_DIRECTORY_ENTRY_SIZE],
    const struct allocation_stream *source_stream,
    struct allocation_stream *target_stream)
{
	enum exfat_resize_error error;
	uint32_t cluster_count;
	uint32_t target_cluster;

	*target_stream = *source_stream;
	if (source_stream->first_cluster < 2 || source_stream->data_length == 0)
		return EXFAT_RESIZE_SUCCESS;

	error = exfat_resize_map_cluster(context, source_stream->first_cluster, &target_cluster);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = exfat_resize_store_le32(
	    entry, EXFAT_DIRECTORY_ENTRY_SIZE, EXFAT_STREAM_FIRST_CLUSTER_OFFSET, target_cluster);
	if (error != EXFAT_RESIZE_SUCCESS)
		return EXFAT_RESIZE_INTERNAL_ERROR;
	target_stream->first_cluster = target_cluster;

	if (!source_stream->no_fat_chain)
		return EXFAT_RESIZE_SUCCESS;
	error = exfat_resize_stream_cluster_count(context, source_stream, &cluster_count);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	if (exfat_resize_stream_crosses_mapping_boundary(
	        context, source_stream->first_cluster, cluster_count)) {
		entry[EXFAT_STREAM_FLAGS_OFFSET] &= (unsigned char)~EXFAT_NO_FAT_CHAIN;
		target_stream->no_fat_chain = 0;
	}
	return EXFAT_RESIZE_SUCCESS;
}

static enum exfat_resize_error push_directory(struct resize_context *context,
    const struct allocation_stream *directory,
    enum directory_scan_mode mode)
{
	struct directory_worklist *worklist = &context->directories;
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
	if (mode == DIRECTORY_SCAN_REWRITE)
		return EXFAT_RESIZE_INTERNAL_ERROR;
	if (worklist->capacity > SIZE_MAX / 2)
		return EXFAT_RESIZE_ARITHMETIC_OVERFLOW;
	capacity = worklist->capacity == 0 ? 1 : worklist->capacity * 2;
	if (capacity > SIZE_MAX / sizeof(*items))
		return EXFAT_RESIZE_ARITHMETIC_OVERFLOW;
	size = capacity * sizeof(*items);
	items = context->allocator.allocate(context->allocator.context, size);
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
		context->allocator.deallocate(
		    context->allocator.context, worklist->items, worklist->capacity * sizeof(*items));
	}
	worklist->items = items;
	worklist->capacity = capacity;
	worklist->items[worklist->count++] = *directory;
	return EXFAT_RESIZE_SUCCESS;
}

static enum exfat_resize_error scan_file_entry_set(struct resize_context *context,
    struct stream_cursor *cursor,
    unsigned char primary[EXFAT_DIRECTORY_ENTRY_SIZE],
    const struct directory_location *primary_location,
    enum directory_scan_mode mode)
{
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
	if (mode == DIRECTORY_SCAN_REWRITE)
		calculated_checksum = checksum_entry(0, primary, 1);

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
			if (mode == DIRECTORY_SCAN_REWRITE) {
				error =
				    rewrite_stream_allocation(context, secondary, &source_stream, &target_stream);
			} else {
				if (is_directory && source_stream.data_length > EXFAT_MAX_DIRECTORY_SIZE)
					return EXFAT_RESIZE_INVALID_FILESYSTEM;
				if (source_stream.no_fat_chain && source_stream.data_length == 0)
					return EXFAT_RESIZE_INVALID_FILESYSTEM;
				error = exfat_resize_claim_allocation_stream(context, &source_stream);
				target_stream = source_stream;
			}
			if (error != EXFAT_RESIZE_SUCCESS)
				return error;
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

		if (mode == DIRECTORY_SCAN_REWRITE) {
			calculated_checksum = checksum_entry(calculated_checksum, secondary, 0);
			error = write_directory_entry(context, &secondary_entries[index].location, secondary);
			if (error != EXFAT_RESIZE_SUCCESS)
				return error;
		}
	}

	if (mode == DIRECTORY_SCAN_REWRITE) {
		error = exfat_resize_store_le16(
		    primary, EXFAT_DIRECTORY_ENTRY_SIZE, EXFAT_FILE_CHECKSUM_OFFSET, calculated_checksum);
		if (error != EXFAT_RESIZE_SUCCESS)
			return EXFAT_RESIZE_INTERNAL_ERROR;
		error = write_directory_entry(context, primary_location, primary);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
	}

	if (is_directory && source_stream.data_length != 0) {
		struct allocation_stream child =
		    mode == DIRECTORY_SCAN_REWRITE ? target_stream : source_stream;

		child.root_directory = 0;
		return push_directory(context, &child, mode);
	}
	return EXFAT_RESIZE_SUCCESS;
}

static enum exfat_resize_error scan_bitmap_entry(struct resize_context *context,
    unsigned char entry[EXFAT_DIRECTORY_ENTRY_SIZE],
    const struct directory_location *location,
    enum directory_scan_mode mode)
{
	enum exfat_resize_error error;
	uint64_t required_length;

	if (entry[EXFAT_BITMAP_FLAGS_OFFSET] != 0)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	if (mode == DIRECTORY_SCAN_REWRITE) {
		error = exfat_resize_store_le32(entry, EXFAT_DIRECTORY_ENTRY_SIZE,
		    EXFAT_BITMAP_FIRST_CLUSTER_OFFSET, context->new_bitmap.first_cluster);
		if (error == EXFAT_RESIZE_SUCCESS)
			error = exfat_resize_store_le64(entry, EXFAT_DIRECTORY_ENTRY_SIZE,
			    EXFAT_BITMAP_DATA_LENGTH_OFFSET, context->new_bitmap.data_length);
		if (error != EXFAT_RESIZE_SUCCESS)
			return EXFAT_RESIZE_INTERNAL_ERROR;
		return write_directory_entry(context, location, entry);
	}
	if (context->old_bitmap.first_cluster != 0)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	error = allocation_from_entry(entry, EXFAT_BITMAP_FLAGS_OFFSET,
	    EXFAT_BITMAP_FIRST_CLUSTER_OFFSET, EXFAT_BITMAP_DATA_LENGTH_OFFSET, &context->old_bitmap);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	required_length = ((uint64_t)context->source.cluster_count + 7) / 8;
	if (context->old_bitmap.data_length < required_length)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	error = exfat_resize_claim_allocation_stream(context, &context->old_bitmap);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	context->bitmap_location = *location;
	return EXFAT_RESIZE_SUCCESS;
}

enum exfat_resize_error exfat_resize_rewrite_identity_bitmap_entry(struct resize_context *context)
{
	struct sector_cache *cache = &context->caches[SECTOR_CACHE_TARGET_DIRECTORY_DATA];
	struct directory_location target_location = context->bitmap_location;
	unsigned char entry[EXFAT_DIRECTORY_ENTRY_SIZE];
	enum exfat_resize_error error;
	uint64_t heap_relative_sector;

	if (exfat_resize_mapping_changes_cluster_numbers(context) ||
	    target_location.sector < context->source.cluster_heap_offset ||
	    target_location.offset > context->sector_size - EXFAT_DIRECTORY_ENTRY_SIZE)
		return EXFAT_RESIZE_INTERNAL_ERROR;
	heap_relative_sector = target_location.sector - context->source.cluster_heap_offset;
	if (heap_relative_sector >=
	    context->target.volume_sector_count - context->target.cluster_heap_offset)
		return EXFAT_RESIZE_INTERNAL_ERROR;
	target_location.sector = context->target.cluster_heap_offset + heap_relative_sector;

	error = exfat_resize_load_cache(
	    context, SECTOR_CACHE_TARGET_DIRECTORY_DATA, target_location.sector, 1);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	memcpy(entry,
	    cache->data +
	        (size_t)(target_location.sector - cache->first_sector) * context->sector_size +
	        target_location.offset,
	    sizeof(entry));
	return scan_bitmap_entry(context, entry, &target_location, DIRECTORY_SCAN_REWRITE);
}

static enum exfat_resize_error scan_upcase_entry(struct resize_context *context,
    unsigned char entry[EXFAT_DIRECTORY_ENTRY_SIZE],
    const struct directory_location *location,
    enum directory_scan_mode mode)
{
	struct allocation_stream stream;
	enum exfat_resize_error error;
	uint32_t target_cluster;

	memset(&stream, 0, sizeof(stream));
	error = exfat_resize_load_le32(entry, EXFAT_DIRECTORY_ENTRY_SIZE,
	    EXFAT_UPCASE_FIRST_CLUSTER_OFFSET, &stream.first_cluster);
	if (error == EXFAT_RESIZE_SUCCESS)
		error = exfat_resize_load_le64(entry, EXFAT_DIRECTORY_ENTRY_SIZE,
		    EXFAT_UPCASE_DATA_LENGTH_OFFSET, &stream.data_length);
	if (error != EXFAT_RESIZE_SUCCESS)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	if (stream.first_cluster < 2 || stream.data_length == 0)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	if (mode == DIRECTORY_SCAN_VALIDATE)
		return exfat_resize_claim_allocation_stream(context, &stream);
	error = exfat_resize_map_cluster(context, stream.first_cluster, &target_cluster);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = exfat_resize_store_le32(
	    entry, EXFAT_DIRECTORY_ENTRY_SIZE, EXFAT_UPCASE_FIRST_CLUSTER_OFFSET, target_cluster);
	if (error != EXFAT_RESIZE_SUCCESS)
		return EXFAT_RESIZE_INTERNAL_ERROR;
	return write_directory_entry(context, location, entry);
}

static enum exfat_resize_error scan_one_directory(struct resize_context *context,
    const struct allocation_stream *directory,
    enum directory_scan_mode mode)
{
	struct directory_location location;
	struct stream_cursor cursor;
	unsigned char entry[EXFAT_DIRECTORY_ENTRY_SIZE];
	enum exfat_resize_error error;

	if (mode == DIRECTORY_SCAN_REWRITE) {
		error = exfat_resize_initialize_stream_cursor(&context->target, directory,
		    SECTOR_CACHE_TARGET_DIRECTORY_DATA, STREAM_CHAIN_TARGET_MODEL, &cursor);
	} else {
		error = exfat_resize_initialize_stream_cursor(&context->source, directory,
		    SECTOR_CACHE_SOURCE_DIRECTORY_DATA, STREAM_CHAIN_SOURCE_FAT, &cursor);
	}
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;

	while (!cursor.exhausted) {
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
			error = scan_bitmap_entry(context, entry, &location, mode);
			break;
		case EXFAT_ENTRY_UPCASE:
			if (!directory->root_directory)
				return EXFAT_RESIZE_INVALID_FILESYSTEM;
			error = scan_upcase_entry(context, entry, &location, mode);
			break;
		case EXFAT_ENTRY_VOLUME_LABEL:
			error =
			    directory->root_directory ? EXFAT_RESIZE_SUCCESS : EXFAT_RESIZE_INVALID_FILESYSTEM;
			break;
		case EXFAT_ENTRY_FILE:
			error = scan_file_entry_set(context, &cursor, entry, &location, mode);
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

enum exfat_resize_error exfat_resize_scan_directory_tree(struct resize_context *context,
    const struct allocation_stream *root,
    enum directory_scan_mode mode)
{
	struct allocation_stream directory;
	enum exfat_resize_error error;

	context->directories.count = 0;
	error = push_directory(context, root, mode);
	while (error == EXFAT_RESIZE_SUCCESS && context->directories.count != 0) {
		directory = context->directories.items[--context->directories.count];
		error = scan_one_directory(context, &directory, mode);
	}
	return error;
}
