/* SPDX-License-Identifier: MIT */

#include "common.h"

#include "exfat_resize.h"

#include "block_device.h"
#include "boot_region.h"
#include "checked_math.h"
#include "endian.h"
#include "geometry.h"
#include "sector_adapter.h"

#include <string.h>

enum {
	EXFAT_DIRECTORY_ENTRY_SIZE = 32,

	EXFAT_ENTRY_BITMAP = 0x81,
	EXFAT_ENTRY_UPCASE = 0x82,
	EXFAT_ENTRY_VOLUME_LABEL = 0x83,
	EXFAT_ENTRY_FILE = 0x85,
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

#define EXFAT_FAT_BAD_CLUSTER UINT32_C(0xfffffff7)
#define EXFAT_FAT_END_OF_CHAIN UINT32_C(0xffffffff)
#define EXFAT_MAX_DIRECTORY_SIZE (UINT64_C(256) * 1024 * 1024)
#define EXFAT_IO_BUFFER_SIZE ((size_t)UINT32_C(1048576))
/*
 * A nonzero model value means that the cluster is allocated. Values 2 and
 * above are target FAT entries. The otherwise-invalid value 1 represents an
 * allocation whose NoFatChain stream deliberately has no FAT entry.
 */
#define EXFAT_MODEL_NO_FAT_CHAIN UINT32_C(1)

enum {
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

	EXFAT_VENDOR_FLAGS_OFFSET = 1,
	EXFAT_VENDOR_FIRST_CLUSTER_OFFSET = 20
};

struct sector_cache {
	unsigned char *data;
	uint64_t sector;
	int valid;
	int dirty;
};

enum sector_cache_index {
	/* Source directory payload sectors read during preflight validation. */
	SECTOR_CACHE_SOURCE_DIRECTORY_DATA,
	/* Source allocation-bitmap payload sectors read during reconciliation. */
	SECTOR_CACHE_SOURCE_BITMAP_DATA,
	/* Target directory payload sectors read and written during rewrite. */
	SECTOR_CACHE_TARGET_DIRECTORY_DATA,
	/* Number of independently backed sector caches. Must be last! */
	SECTOR_CACHE_COUNT
};

enum stream_chain_source { STREAM_CHAIN_SOURCE_FAT, STREAM_CHAIN_TARGET_MODEL };

struct allocation_stream {
	uint32_t first_cluster;
	uint64_t data_length;
	int no_fat_chain;
	int root_directory;
};

struct stream_cursor {
	struct allocation_stream stream;
	const struct exfat_resize_geometry *geometry;
	enum sector_cache_index data_cache;
	enum stream_chain_source chain_source;
	uint32_t current_cluster;
	uint32_t traversed_clusters;
	uint64_t cluster_offset;
	uint64_t remaining_bytes;
	int exhausted;
};

struct directory_location {
	uint64_t sector;
	size_t offset;
};

struct buffered_directory_entry {
	unsigned char data[EXFAT_DIRECTORY_ENTRY_SIZE];
	struct directory_location location;
};

struct directory_worklist {
	struct allocation_stream *items;
	size_t count;
	size_t capacity;
};

struct resize_context {
	const struct exfat_resize_block_device *device;
	struct exfat_resize_sector_adapter sector_adapter;
	struct exfat_resize_allocator allocator;
	enum exfat_resize_stage stage;
	struct exfat_resize_geometry source;
	struct exfat_resize_geometry target;
	struct allocation_stream old_bitmap;
	struct allocation_stream new_bitmap;
	struct directory_location bitmap_location;
	struct directory_worklist directories;
	struct sector_cache caches[SECTOR_CACHE_COUNT];
	unsigned char *cache_buffer;
	size_t cache_buffer_size;
	unsigned char *io_buffer;
	uint32_t io_sector_capacity;
	size_t sector_size;
	/* Cluster size in bytes. */
	uint64_t cluster_size;
	uint32_t displaced_cluster_count;
	uint32_t used_cluster_count;
	uint32_t fat_entry_zero;
	unsigned char *source_fat;
	size_t source_fat_size;
	/* Indexed by target cluster minus 2; one entry for every target cluster. */
	uint32_t *allocation_model;
	size_t allocation_model_size;
	int found_bitmap;
};

enum directory_scan_mode { DIRECTORY_SCAN_VALIDATE, DIRECTORY_SCAN_REWRITE };

/* Sector I/O */

static enum exfat_resize_error flush_cache(
    struct resize_context *context, enum sector_cache_index cache_index)
{
	struct sector_cache *cache = &context->caches[cache_index];
	enum exfat_resize_error error;

	if (!cache->valid || !cache->dirty)
		return EXFAT_RESIZE_SUCCESS;
	error = exfat_resize_block_device_write(
	    context->device, cache->sector, 1, cache->data, context->sector_size);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	cache->dirty = 0;
	return EXFAT_RESIZE_SUCCESS;
}

static enum exfat_resize_error load_cache(
    struct resize_context *context, enum sector_cache_index cache_index, uint64_t sector)
{
	struct sector_cache *cache = &context->caches[cache_index];
	enum exfat_resize_error error;

	if (cache->valid && cache->sector == sector)
		return EXFAT_RESIZE_SUCCESS;

	error = flush_cache(context, cache_index);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;

	error = exfat_resize_block_device_read(
	    context->device, sector, 1, cache->data, context->sector_size);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	cache->sector = sector;
	cache->valid = 1;
	cache->dirty = 0;
	return EXFAT_RESIZE_SUCCESS;
}

static enum exfat_resize_error cluster_sector(
    const struct exfat_resize_geometry *geometry, uint32_t cluster, uint64_t *sector)
{
	uint64_t cluster_offset;
	uint64_t result;
	enum exfat_resize_error error;

	if (cluster < 2 || cluster > geometry->cluster_count + UINT32_C(1))
		return EXFAT_RESIZE_OUT_OF_BOUNDS;
	error = exfat_resize_checked_multiply_u64(
	    (uint64_t)cluster - 2, geometry->sectors_per_cluster, &cluster_offset);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = exfat_resize_checked_add_u64(geometry->cluster_heap_offset, cluster_offset, &result);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	if (result >= geometry->volume_sector_count)
		return EXFAT_RESIZE_OUT_OF_BOUNDS;
	*sector = result;
	return EXFAT_RESIZE_SUCCESS;
}

static int fat_value_is_end_of_chain(uint32_t value)
{
	return value == EXFAT_FAT_END_OF_CHAIN;
}

static int cluster_is_valid(const struct exfat_resize_geometry *geometry, uint32_t cluster)
{
	return cluster >= 2 && cluster <= geometry->cluster_count + UINT32_C(1);
}

static enum exfat_resize_error load_source_fat(struct resize_context *context)
{
	enum exfat_resize_error error;
	uint64_t byte_count = ((uint64_t)context->source.cluster_count + 2) * 4;
	uint64_t sector_count;
	uint64_t allocation_size;

	error = exfat_resize_checked_ceil_divide_u64(byte_count, context->sector_size, &sector_count);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	if (sector_count > context->source.fat_length)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	error = exfat_resize_checked_multiply_u64(sector_count, context->sector_size, &allocation_size);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	context->source_fat_size = (size_t)allocation_size;
	if (context->source_fat_size != allocation_size)
		return EXFAT_RESIZE_ARITHMETIC_OVERFLOW;
	context->source_fat =
	    context->allocator.allocate(context->allocator.context, context->source_fat_size);
	if (context->source_fat == NULL)
		return EXFAT_RESIZE_OUT_OF_MEMORY;

	return exfat_resize_block_device_read(context->device, context->source.fat_offset,
	    (uint32_t)sector_count, context->source_fat, context->source_fat_size);
}

static enum exfat_resize_error source_fat_get(
    const struct resize_context *context, uint32_t cluster, uint32_t *value)
{
	size_t offset;

	if (cluster > context->source.cluster_count + UINT32_C(1))
		return EXFAT_RESIZE_OUT_OF_BOUNDS;
	offset = (size_t)cluster * 4;
	return exfat_resize_load_le32(context->source_fat, context->source_fat_size, offset, value);
}

static void release_source_fat(struct resize_context *context)
{
	if (context->source_fat == NULL)
		return;
	context->allocator.deallocate(
	    context->allocator.context, context->source_fat, context->source_fat_size);
	context->source_fat = NULL;
	context->source_fat_size = 0;
}

static enum exfat_resize_error validate_reserved_fat_entries(struct resize_context *context)
{
	enum exfat_resize_error error;
	uint32_t entry_one;

	error = source_fat_get(context, 0, &context->fat_entry_zero);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = source_fat_get(context, 1, &entry_one);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;

	/*
	 * FAT entry zero contains the media type in its low byte. Preserve that
	 * byte, but require the reserved upper bytes and entry one to have the
	 * values mandated by the exFAT specification.
	 */
	if ((context->fat_entry_zero & UINT32_C(0xffffff00)) != UINT32_C(0xffffff00) ||
	    entry_one != EXFAT_FAT_END_OF_CHAIN)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	return EXFAT_RESIZE_SUCCESS;
}

static enum exfat_resize_error stream_cluster_count(const struct resize_context *context,
    const struct allocation_stream *stream,
    uint32_t *cluster_count)
{
	uint64_t count;
	enum exfat_resize_error error;

	error =
	    exfat_resize_checked_ceil_divide_u64(stream->data_length, context->cluster_size, &count);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	if (count > UINT32_MAX)
		return EXFAT_RESIZE_OUT_OF_BOUNDS;
	*cluster_count = (uint32_t)count;
	return EXFAT_RESIZE_SUCCESS;
}

static enum exfat_resize_error map_cluster(
    const struct resize_context *context, uint32_t source_cluster, uint32_t *target_cluster);
static int mapping_changes_cluster_numbers(const struct resize_context *context);
static int stream_crosses_mapping_boundary(
    const struct resize_context *context, uint32_t first_cluster, uint32_t cluster_count);

/* Allocation streams */

static enum exfat_resize_error model_entry_for_source_cluster(
    struct resize_context *context, uint32_t source_cluster, uint32_t **entry)
{
	enum exfat_resize_error error;
	uint32_t target_cluster;

	error = map_cluster(context, source_cluster, &target_cluster);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	if (target_cluster < 2 || target_cluster > context->target.cluster_count + UINT32_C(1))
		return EXFAT_RESIZE_INTERNAL_ERROR;
	*entry = &context->allocation_model[target_cluster - 2];
	return EXFAT_RESIZE_SUCCESS;
}

static enum exfat_resize_error claim_allocation_stream(
    struct resize_context *context, const struct allocation_stream *stream)
{
	enum exfat_resize_error error;
	uint32_t *model_entry;
	uint32_t cluster_count;
	uint32_t cluster;
	uint32_t index;
	uint32_t next;
	uint32_t target_next;

	error = stream_cluster_count(context, stream, &cluster_count);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	if (cluster_count > context->source.cluster_count)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	if (cluster_count == 0)
		return stream->first_cluster == 0 ? EXFAT_RESIZE_SUCCESS : EXFAT_RESIZE_INVALID_FILESYSTEM;
	if (!cluster_is_valid(&context->source, stream->first_cluster))
		return EXFAT_RESIZE_INVALID_FILESYSTEM;

	if (stream->no_fat_chain) {
		int crosses =
		    stream_crosses_mapping_boundary(context, stream->first_cluster, cluster_count);

		if ((uint64_t)stream->first_cluster + cluster_count >
		    (uint64_t)context->source.cluster_count + 2)
			return EXFAT_RESIZE_INVALID_FILESYSTEM;
		for (index = 0; index < cluster_count; ++index) {
			error = model_entry_for_source_cluster(
			    context, stream->first_cluster + index, &model_entry);
			if (error != EXFAT_RESIZE_SUCCESS)
				return error;
			if (*model_entry != 0)
				return EXFAT_RESIZE_INVALID_FILESYSTEM;
			if (!crosses) {
				*model_entry = EXFAT_MODEL_NO_FAT_CHAIN;
			} else if (index + 1 == cluster_count) {
				*model_entry = EXFAT_FAT_END_OF_CHAIN;
			} else {
				error = map_cluster(context, stream->first_cluster + index + 1, &target_next);
				if (error != EXFAT_RESIZE_SUCCESS)
					return error;
				*model_entry = target_next;
			}
		}
		return EXFAT_RESIZE_SUCCESS;
	}

	cluster = stream->first_cluster;
	for (index = 0; index < cluster_count; ++index) {
		error = model_entry_for_source_cluster(context, cluster, &model_entry);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		if (*model_entry != 0)
			return EXFAT_RESIZE_INVALID_FILESYSTEM;
		error = source_fat_get(context, cluster, &next);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		if (index + 1 == cluster_count) {
			if (!fat_value_is_end_of_chain(next))
				return EXFAT_RESIZE_INVALID_FILESYSTEM;
			*model_entry = EXFAT_FAT_END_OF_CHAIN;
			return EXFAT_RESIZE_SUCCESS;
		}
		if (!cluster_is_valid(&context->source, next))
			return EXFAT_RESIZE_INVALID_FILESYSTEM;
		error = map_cluster(context, next, &target_next);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		*model_entry = target_next;
		cluster = next;
	}
	return EXFAT_RESIZE_INTERNAL_ERROR;
}

static enum exfat_resize_error claim_root_directory(
    struct resize_context *context, uint32_t first_cluster)
{
	enum exfat_resize_error error;
	uint32_t *model_entry;
	uint32_t cluster = first_cluster;
	uint32_t next;
	uint32_t target_next;
	uint32_t traversed;
	uint64_t maximum_cluster_count = EXFAT_MAX_DIRECTORY_SIZE / context->cluster_size;

	for (traversed = 0; traversed < context->source.cluster_count; ++traversed) {
		if (traversed >= maximum_cluster_count)
			return EXFAT_RESIZE_INVALID_FILESYSTEM;
		if (!cluster_is_valid(&context->source, cluster))
			return EXFAT_RESIZE_INVALID_FILESYSTEM;
		error = model_entry_for_source_cluster(context, cluster, &model_entry);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		if (*model_entry != 0)
			return EXFAT_RESIZE_INVALID_FILESYSTEM;
		error = source_fat_get(context, cluster, &next);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		if (fat_value_is_end_of_chain(next)) {
			*model_entry = EXFAT_FAT_END_OF_CHAIN;
			return EXFAT_RESIZE_SUCCESS;
		}
		if (!cluster_is_valid(&context->source, next))
			return EXFAT_RESIZE_INVALID_FILESYSTEM;
		error = map_cluster(context, next, &target_next);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		*model_entry = target_next;
		cluster = next;
	}
	return EXFAT_RESIZE_INVALID_FILESYSTEM;
}

static enum exfat_resize_error next_stream_cluster(
    struct resize_context *context, struct stream_cursor *cursor)
{
	enum exfat_resize_error error;
	uint32_t next;

	if (cursor->stream.no_fat_chain) {
		next = cursor->current_cluster + 1;
	} else if (cursor->chain_source == STREAM_CHAIN_SOURCE_FAT) {
		error = source_fat_get(context, cursor->current_cluster, &next);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
	} else {
		if (!cluster_is_valid(&context->target, cursor->current_cluster))
			return EXFAT_RESIZE_INTERNAL_ERROR;
		next = context->allocation_model[cursor->current_cluster - 2];
		if (next == 0 || next == EXFAT_MODEL_NO_FAT_CHAIN || next == EXFAT_FAT_BAD_CLUSTER)
			return EXFAT_RESIZE_INTERNAL_ERROR;
	}
	if (!cursor->stream.no_fat_chain) {
		if (fat_value_is_end_of_chain(next)) {
			if (!cursor->stream.root_directory && cursor->remaining_bytes != 0)
				return EXFAT_RESIZE_INVALID_FILESYSTEM;
			cursor->exhausted = 1;
			return EXFAT_RESIZE_SUCCESS;
		}
	}

	++cursor->traversed_clusters;
	if (cursor->traversed_clusters >= cursor->geometry->cluster_count ||
	    !cluster_is_valid(cursor->geometry, next))
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	cursor->current_cluster = next;
	cursor->cluster_offset = 0;
	return EXFAT_RESIZE_SUCCESS;
}

static enum exfat_resize_error initialize_stream_cursor(
    const struct exfat_resize_geometry *geometry,
    const struct allocation_stream *stream,
    enum sector_cache_index data_cache,
    enum stream_chain_source chain_source,
    struct stream_cursor *cursor)
{
	if (!cluster_is_valid(geometry, stream->first_cluster))
		return EXFAT_RESIZE_INVALID_FILESYSTEM;

	memset(cursor, 0, sizeof(*cursor));
	cursor->stream = *stream;
	cursor->geometry = geometry;
	cursor->data_cache = data_cache;
	cursor->chain_source = chain_source;
	cursor->current_cluster = stream->first_cluster;
	cursor->remaining_bytes = stream->root_directory ? UINT64_MAX : stream->data_length;
	if (cursor->remaining_bytes == 0)
		cursor->exhausted = 1;
	return EXFAT_RESIZE_SUCCESS;
}

static enum exfat_resize_error advance_stream_cursor(
    struct resize_context *context, struct stream_cursor *cursor, size_t count)
{
	enum exfat_resize_error error;

	if (!cursor->stream.root_directory)
		cursor->remaining_bytes -= count;
	cursor->cluster_offset += count;
	if (!cursor->stream.root_directory && cursor->remaining_bytes == 0) {
		cursor->exhausted = 1;
		return EXFAT_RESIZE_SUCCESS;
	}
	if (cursor->cluster_offset == context->cluster_size) {
		error = next_stream_cluster(context, cursor);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
	}
	return EXFAT_RESIZE_SUCCESS;
}

static enum exfat_resize_error read_stream(
    struct resize_context *context, struct stream_cursor *cursor, void *buffer, size_t count)
{
	struct sector_cache *cache = &context->caches[cursor->data_cache];
	unsigned char *destination = buffer;
	enum exfat_resize_error error;
	uint64_t cluster_start;
	uint64_t sector;
	size_t sector_offset;
	size_t available;
	size_t part;

	if (count != 0 && buffer == NULL)
		return EXFAT_RESIZE_INVALID_ARGUMENT;
	if (cursor->exhausted || (!cursor->stream.root_directory && count > cursor->remaining_bytes))
		return EXFAT_RESIZE_OUT_OF_BOUNDS;

	while (count != 0) {
		error = cluster_sector(cursor->geometry, cursor->current_cluster, &cluster_start);
		if (error != EXFAT_RESIZE_SUCCESS)
			return EXFAT_RESIZE_INVALID_FILESYSTEM;
		sector = cluster_start + cursor->cluster_offset / context->sector_size;
		sector_offset = (size_t)(cursor->cluster_offset % context->sector_size);
		available = context->sector_size - sector_offset;
		part = count < available ? count : available;

		error = load_cache(context, cursor->data_cache, sector);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		memcpy(destination, cache->data + sector_offset, part);
		error = advance_stream_cursor(context, cursor, part);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		destination += part;
		count -= part;
	}
	return EXFAT_RESIZE_SUCCESS;
}

static enum exfat_resize_error read_directory_entry(struct resize_context *context,
    struct stream_cursor *cursor,
    unsigned char entry[EXFAT_DIRECTORY_ENTRY_SIZE],
    struct directory_location *location)
{
	enum exfat_resize_error error;
	uint64_t cluster_start;

	if (cursor->exhausted ||
	    (!cursor->stream.root_directory && cursor->remaining_bytes < EXFAT_DIRECTORY_ENTRY_SIZE))
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	if (cursor->cluster_offset % EXFAT_DIRECTORY_ENTRY_SIZE != 0 ||
	    context->sector_size % EXFAT_DIRECTORY_ENTRY_SIZE != 0)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;

	error = cluster_sector(cursor->geometry, cursor->current_cluster, &cluster_start);
	if (error != EXFAT_RESIZE_SUCCESS)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	if (location != NULL) {
		location->sector = cluster_start + cursor->cluster_offset / context->sector_size;
		location->offset = (size_t)(cursor->cluster_offset % context->sector_size);
	}
	return read_stream(context, cursor, entry, EXFAT_DIRECTORY_ENTRY_SIZE);
}

static enum exfat_resize_error write_directory_entry(struct resize_context *context,
    const struct directory_location *location,
    const unsigned char entry[EXFAT_DIRECTORY_ENTRY_SIZE])
{
	struct sector_cache *cache = &context->caches[SECTOR_CACHE_TARGET_DIRECTORY_DATA];
	unsigned char *destination;
	enum exfat_resize_error error;

	error = load_cache(context, SECTOR_CACHE_TARGET_DIRECTORY_DATA, location->sector);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	destination = cache->data + location->offset;
	if (memcmp(destination, entry, EXFAT_DIRECTORY_ENTRY_SIZE) == 0)
		return EXFAT_RESIZE_SUCCESS;
	memcpy(destination, entry, EXFAT_DIRECTORY_ENTRY_SIZE);
	cache->dirty = 1;
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

	if ((size_t)secondary_count * sizeof(*entries) >
	    (size_t)context->io_sector_capacity * context->sector_size)
		return EXFAT_RESIZE_INTERNAL_ERROR;
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

static int mapping_changes_cluster_numbers(const struct resize_context *context)
{
	return context->displaced_cluster_count != 0 &&
	    context->displaced_cluster_count != context->source.cluster_count;
}

static int stream_crosses_mapping_boundary(
    const struct resize_context *context, uint32_t first_cluster, uint32_t cluster_count)
{
	uint64_t boundary = (uint64_t)context->displaced_cluster_count + 2;
	uint64_t end = (uint64_t)first_cluster + cluster_count;

	return mapping_changes_cluster_numbers(context) && first_cluster < boundary && end > boundary;
}

static enum exfat_resize_error map_cluster(
    const struct resize_context *context, uint32_t source_cluster, uint32_t *target_cluster)
{
	return exfat_resize_map_growth_cluster(
	    &context->source, &context->target, source_cluster, target_cluster);
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

static enum exfat_resize_error validate_entry_allocation(struct resize_context *context,
    const unsigned char entry[EXFAT_DIRECTORY_ENTRY_SIZE],
    size_t flags_offset,
    size_t first_cluster_offset,
    size_t data_length_offset,
    uint64_t maximum_data_length,
    struct allocation_stream *stream)
{
	enum exfat_resize_error error;

	error = allocation_from_entry(
	    entry, flags_offset, first_cluster_offset, data_length_offset, stream);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	if (stream->data_length > maximum_data_length)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	if ((entry[flags_offset] & EXFAT_ALLOCATION_POSSIBLE) == 0) {
		if (stream->first_cluster != 0 || stream->data_length != 0)
			return EXFAT_RESIZE_INVALID_FILESYSTEM;
		return EXFAT_RESIZE_SUCCESS;
	}
	if (stream->no_fat_chain && stream->data_length == 0)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	return claim_allocation_stream(context, stream);
}

static enum exfat_resize_error rewrite_entry_allocation(struct resize_context *context,
    unsigned char entry[EXFAT_DIRECTORY_ENTRY_SIZE],
    size_t flags_offset,
    size_t first_cluster_offset,
    size_t data_length_offset,
    struct allocation_stream *source_stream,
    struct allocation_stream *target_stream)
{
	enum exfat_resize_error error;
	uint32_t cluster_count;
	uint32_t target_cluster;

	error = allocation_from_entry(
	    entry, flags_offset, first_cluster_offset, data_length_offset, source_stream);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	*target_stream = *source_stream;
	if ((entry[flags_offset] & EXFAT_ALLOCATION_POSSIBLE) == 0 ||
	    source_stream->first_cluster < 2 || source_stream->data_length == 0)
		return EXFAT_RESIZE_SUCCESS;

	error = map_cluster(context, source_stream->first_cluster, &target_cluster);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = exfat_resize_store_le32(
	    entry, EXFAT_DIRECTORY_ENTRY_SIZE, first_cluster_offset, target_cluster);
	if (error != EXFAT_RESIZE_SUCCESS)
		return EXFAT_RESIZE_INTERNAL_ERROR;
	target_stream->first_cluster = target_cluster;

	if (!source_stream->no_fat_chain)
		return EXFAT_RESIZE_SUCCESS;
	error = stream_cluster_count(context, source_stream, &cluster_count);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	if (stream_crosses_mapping_boundary(context, source_stream->first_cluster, cluster_count)) {
		entry[flags_offset] &= (unsigned char)~EXFAT_NO_FAT_CHAIN;
		target_stream->no_fat_chain = 0;
	}
	return EXFAT_RESIZE_SUCCESS;
}

/* Directory tree */

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
	uint16_t calculated_checksum;
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
			if (mode == DIRECTORY_SCAN_REWRITE) {
				error = rewrite_entry_allocation(context, secondary, EXFAT_STREAM_FLAGS_OFFSET,
				    EXFAT_STREAM_FIRST_CLUSTER_OFFSET, EXFAT_STREAM_DATA_LENGTH_OFFSET,
				    &source_stream, &target_stream);
			} else {
				error = validate_entry_allocation(context, secondary, EXFAT_STREAM_FLAGS_OFFSET,
				    EXFAT_STREAM_FIRST_CLUSTER_OFFSET, EXFAT_STREAM_DATA_LENGTH_OFFSET,
				    is_directory ? EXFAT_MAX_DIRECTORY_SIZE : UINT64_MAX, &source_stream);
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
			if ((secondary[EXFAT_VENDOR_FLAGS_OFFSET] & EXFAT_ALLOCATION_POSSIBLE) != 0)
				return EXFAT_RESIZE_INVALID_FILESYSTEM;
		} else {
			if ((secondary[0] & EXFAT_ENTRY_BENIGN) == 0)
				return EXFAT_RESIZE_UNSUPPORTED_CRITICAL_ENTRY;
			if ((secondary[EXFAT_VENDOR_FLAGS_OFFSET] & EXFAT_ALLOCATION_POSSIBLE) != 0) {
				uint32_t first_cluster;

				error = exfat_resize_load_le32(secondary, EXFAT_DIRECTORY_ENTRY_SIZE,
				    EXFAT_VENDOR_FIRST_CLUSTER_OFFSET, &first_cluster);
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
	if (context->found_bitmap)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	error = allocation_from_entry(entry, EXFAT_BITMAP_FLAGS_OFFSET,
	    EXFAT_BITMAP_FIRST_CLUSTER_OFFSET, EXFAT_BITMAP_DATA_LENGTH_OFFSET, &context->old_bitmap);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	required_length = ((uint64_t)context->source.cluster_count + 7) / 8;
	if (context->old_bitmap.data_length < required_length)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	error = claim_allocation_stream(context, &context->old_bitmap);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	context->bitmap_location = *location;
	context->found_bitmap = 1;
	return EXFAT_RESIZE_SUCCESS;
}

static enum exfat_resize_error rewrite_identity_bitmap_entry(struct resize_context *context)
{
	struct sector_cache *cache = &context->caches[SECTOR_CACHE_TARGET_DIRECTORY_DATA];
	struct directory_location target_location = context->bitmap_location;
	unsigned char entry[EXFAT_DIRECTORY_ENTRY_SIZE];
	enum exfat_resize_error error;
	uint64_t heap_relative_sector;

	if (mapping_changes_cluster_numbers(context) ||
	    target_location.sector < context->source.cluster_heap_offset ||
	    target_location.offset > context->sector_size - EXFAT_DIRECTORY_ENTRY_SIZE)
		return EXFAT_RESIZE_INTERNAL_ERROR;
	heap_relative_sector = target_location.sector - context->source.cluster_heap_offset;
	if (heap_relative_sector >=
	    context->target.volume_sector_count - context->target.cluster_heap_offset)
		return EXFAT_RESIZE_INTERNAL_ERROR;
	target_location.sector = context->target.cluster_heap_offset + heap_relative_sector;

	error = load_cache(context, SECTOR_CACHE_TARGET_DIRECTORY_DATA, target_location.sector);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	memcpy(entry, cache->data + target_location.offset, sizeof(entry));
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
	stream.no_fat_chain = 0;

	if (mode == DIRECTORY_SCAN_VALIDATE)
		return claim_allocation_stream(context, &stream);
	error = map_cluster(context, stream.first_cluster, &target_cluster);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = exfat_resize_store_le32(
	    entry, EXFAT_DIRECTORY_ENTRY_SIZE, EXFAT_UPCASE_FIRST_CLUSTER_OFFSET, target_cluster);
	if (error != EXFAT_RESIZE_SUCCESS)
		return EXFAT_RESIZE_INTERNAL_ERROR;
	return write_directory_entry(context, location, entry);
}

static enum exfat_resize_error scan_unknown_primary(
    const unsigned char entry[EXFAT_DIRECTORY_ENTRY_SIZE])
{
	enum exfat_resize_error error;
	uint16_t flags;
	uint32_t first_cluster;

	if ((entry[0] & EXFAT_ENTRY_BENIGN) == 0)
		return EXFAT_RESIZE_UNSUPPORTED_CRITICAL_ENTRY;

	error = exfat_resize_load_le16(entry, EXFAT_DIRECTORY_ENTRY_SIZE, 4, &flags);
	if (error != EXFAT_RESIZE_SUCCESS)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	if ((flags & EXFAT_ALLOCATION_POSSIBLE) == 0)
		return EXFAT_RESIZE_SUCCESS;
	error = exfat_resize_load_le32(entry, EXFAT_DIRECTORY_ENTRY_SIZE, 20, &first_cluster);
	if (error != EXFAT_RESIZE_SUCCESS)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	return first_cluster < 2 ? EXFAT_RESIZE_SUCCESS : EXFAT_RESIZE_UNSUPPORTED_ALLOCATED_ENTRY;
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
		error = initialize_stream_cursor(&context->target, directory,
		    SECTOR_CACHE_TARGET_DIRECTORY_DATA, STREAM_CHAIN_TARGET_MODEL, &cursor);
	} else {
		error = initialize_stream_cursor(&context->source, directory,
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
		default:
			error = scan_unknown_primary(entry);
			break;
		}
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
	}
	return EXFAT_RESIZE_SUCCESS;
}

static enum exfat_resize_error scan_directory_tree(struct resize_context *context,
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

struct bitmap_reader {
	struct stream_cursor cursor;
	unsigned char current_byte;
	unsigned int bit_in_byte;
};

/* Allocation model */

static enum exfat_resize_error initialize_bitmap_reader(
    struct resize_context *context, struct bitmap_reader *reader)
{
	memset(reader, 0, sizeof(*reader));
	return initialize_stream_cursor(&context->source, &context->old_bitmap,
	    SECTOR_CACHE_SOURCE_BITMAP_DATA, STREAM_CHAIN_SOURCE_FAT, &reader->cursor);
}

static enum exfat_resize_error read_old_bitmap_bit(
    struct resize_context *context, struct bitmap_reader *reader, int *allocated)
{
	enum exfat_resize_error error;

	if (reader->bit_in_byte == 0) {
		error = read_stream(context, &reader->cursor, &reader->current_byte, 1);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
	}

	*allocated = (reader->current_byte & (1u << reader->bit_in_byte)) != 0;
	++reader->bit_in_byte;
	if (reader->bit_in_byte == 8)
		reader->bit_in_byte = 0;
	return EXFAT_RESIZE_SUCCESS;
}

static enum exfat_resize_error validate_allocation_model(struct resize_context *context)
{
	struct bitmap_reader reader;
	enum exfat_resize_error error;
	uint32_t source_index;

	/*
	 * Every recognized owner has already claimed its clusters. A remaining
	 * allocated bit is valid only when the source FAT identifies a bad
	 * cluster. FAT contents for free clusters are otherwise unspecified.
	 */
	error = initialize_bitmap_reader(context, &reader);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	for (source_index = 0; source_index < context->source.cluster_count; ++source_index) {
		uint32_t source_cluster = source_index + 2;
		uint32_t *model_entry;
		uint32_t fat_value;
		int allocated;

		error = model_entry_for_source_cluster(context, source_cluster, &model_entry);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		error = read_old_bitmap_bit(context, &reader, &allocated);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;

		if (*model_entry != 0) {
			if (!allocated)
				return EXFAT_RESIZE_INVALID_FILESYSTEM;
			continue;
		}

		error = source_fat_get(context, source_cluster, &fat_value);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		if (fat_value == EXFAT_FAT_BAD_CLUSTER) {
			if (!allocated)
				return EXFAT_RESIZE_INVALID_FILESYSTEM;
			/*
			 * A displaced source cluster's physical sectors become part
			 * of the target FAT. A bad marker describes those sectors and
			 * cannot safely be moved to a different physical location.
			 */
			if (source_index < context->displaced_cluster_count)
				return EXFAT_RESIZE_BAD_CLUSTER_CONFLICT;
			*model_entry = EXFAT_FAT_BAD_CLUSTER;
		} else if (allocated) {
			return EXFAT_RESIZE_INVALID_FILESYSTEM;
		}
	}
	return EXFAT_RESIZE_SUCCESS;
}

static enum exfat_resize_error remove_old_bitmap_from_model(struct resize_context *context)
{
	enum exfat_resize_error error;
	uint32_t cluster_count;
	uint32_t cluster;
	uint32_t index;
	uint32_t next;

	error = stream_cluster_count(context, &context->old_bitmap, &cluster_count);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = map_cluster(context, context->old_bitmap.first_cluster, &cluster);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	for (index = 0; index < cluster_count; ++index) {
		uint32_t *model_entry;

		if (!cluster_is_valid(&context->target, cluster))
			return EXFAT_RESIZE_INTERNAL_ERROR;
		model_entry = &context->allocation_model[cluster - 2];
		if (*model_entry == 0 || *model_entry == EXFAT_MODEL_NO_FAT_CHAIN ||
		    *model_entry == EXFAT_FAT_BAD_CLUSTER)
			return EXFAT_RESIZE_INTERNAL_ERROR;
		/*
		 * claim_allocation_stream() has already validated the source FAT
		 * chain and stored its mapped links as the canonical model chain.
		 */
		next = *model_entry;
		*model_entry = 0;

		if (index + 1 == cluster_count)
			return next == EXFAT_FAT_END_OF_CHAIN ? EXFAT_RESIZE_SUCCESS
			                                      : EXFAT_RESIZE_INTERNAL_ERROR;
		if (!cluster_is_valid(&context->target, next))
			return EXFAT_RESIZE_INTERNAL_ERROR;
		cluster = next;
	}
	return EXFAT_RESIZE_INTERNAL_ERROR;
}

static enum exfat_resize_error add_new_bitmap_to_model(struct resize_context *context)
{
	enum exfat_resize_error error;
	uint32_t cluster_count;
	uint32_t cluster;
	uint32_t index;

	error = stream_cluster_count(context, &context->new_bitmap, &cluster_count);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	for (index = 0; index < cluster_count; ++index) {
		cluster = context->new_bitmap.first_cluster + index;
		if (!cluster_is_valid(&context->target, cluster))
			return EXFAT_RESIZE_INTERNAL_ERROR;
		if (context->allocation_model[cluster - 2] != 0)
			return EXFAT_RESIZE_INTERNAL_ERROR;
		context->allocation_model[cluster - 2] =
		    index + 1 == cluster_count ? EXFAT_FAT_END_OF_CHAIN : cluster + 1;
	}
	return EXFAT_RESIZE_SUCCESS;
}

/* Cluster, FAT, and bitmap transforms */

static enum exfat_resize_error copy_cluster_run(struct resize_context *context,
    uint32_t source_cluster,
    uint32_t target_cluster,
    uint32_t cluster_count)
{
	enum exfat_resize_error error;
	uint64_t source_sector;
	uint64_t target_sector;
	uint64_t copied = 0;
	uint64_t sector_count;

	error = cluster_sector(&context->source, source_cluster, &source_sector);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = cluster_sector(&context->target, target_cluster, &target_sector);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	sector_count = (uint64_t)cluster_count * context->source.sectors_per_cluster;
	if (sector_count > context->source.volume_sector_count - source_sector ||
	    sector_count > context->target.volume_sector_count - target_sector)
		return EXFAT_RESIZE_INTERNAL_ERROR;

	while (copied < sector_count) {
		uint64_t remaining = sector_count - copied;
		uint32_t count = remaining > context->io_sector_capacity ? context->io_sector_capacity
		                                                         : (uint32_t)remaining;

		error = exfat_resize_block_device_read(context->device, source_sector + copied, count,
		    context->io_buffer, (size_t)count * context->sector_size);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		error = exfat_resize_block_device_write(context->device, target_sector + copied, count,
		    context->io_buffer, (size_t)count * context->sector_size);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		copied += count;
	}
	return EXFAT_RESIZE_SUCCESS;
}

static enum exfat_resize_error move_displaced_clusters(struct resize_context *context)
{
	enum exfat_resize_error error;
	uint32_t source_index = 0;

	while (source_index < context->displaced_cluster_count) {
		uint32_t run_start;
		uint32_t source_cluster;
		uint32_t target_cluster;
		uint32_t *model_entry;

		error = model_entry_for_source_cluster(context, source_index + 2, &model_entry);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		if (*model_entry == 0 || *model_entry == EXFAT_FAT_BAD_CLUSTER) {
			++source_index;
			continue;
		}

		/*
		 * Within the displaced prefix, adjacent source clusters map to
		 * adjacent target clusters. Free and bad clusters end a copy run.
		 */
		run_start = source_index;
		do {
			++source_index;
			if (source_index == context->displaced_cluster_count)
				break;
			error = model_entry_for_source_cluster(context, source_index + 2, &model_entry);
			if (error != EXFAT_RESIZE_SUCCESS)
				return error;
		} while (*model_entry != 0 && *model_entry != EXFAT_FAT_BAD_CLUSTER);

		source_cluster = run_start + 2;
		error = map_cluster(context, source_cluster, &target_cluster);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		error = copy_cluster_run(context, source_cluster, target_cluster, source_index - run_start);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
	}
	return EXFAT_RESIZE_SUCCESS;
}

static enum exfat_resize_error target_fat_value(
    const struct resize_context *context, uint32_t cluster, uint32_t *value)
{
	if (cluster == 0) {
		*value = context->fat_entry_zero;
		return EXFAT_RESIZE_SUCCESS;
	}
	if (cluster == 1) {
		*value = EXFAT_FAT_END_OF_CHAIN;
		return EXFAT_RESIZE_SUCCESS;
	}
	if (!cluster_is_valid(&context->target, cluster))
		return EXFAT_RESIZE_OUT_OF_BOUNDS;
	*value = context->allocation_model[cluster - 2];
	if (*value == EXFAT_MODEL_NO_FAT_CHAIN)
		*value = 0;
	return EXFAT_RESIZE_SUCCESS;
}

static enum exfat_resize_error write_target_fat(struct resize_context *context)
{
	enum exfat_resize_error error;
	uint64_t fat_sector = 0;
	uint64_t fat_sector_count;

	fat_sector_count = ((uint64_t)context->target.cluster_count + 2) * 4;
	fat_sector_count = (fat_sector_count + context->sector_size - 1) / context->sector_size;
	if (fat_sector_count > context->target.fat_length)
		return EXFAT_RESIZE_INTERNAL_ERROR;

	while (fat_sector < fat_sector_count) {
		uint64_t remaining = fat_sector_count - fat_sector;
		uint32_t sector_count = remaining > context->io_sector_capacity
		    ? context->io_sector_capacity
		    : (uint32_t)remaining;
		size_t byte_count = (size_t)sector_count * context->sector_size;
		uint64_t first_entry = fat_sector * context->sector_size / 4;
		size_t entry_count = byte_count / 4;
		size_t index;

		memset(context->io_buffer, 0, byte_count);
		for (index = 0; index < entry_count; ++index) {
			uint64_t entry = first_entry + index;
			uint32_t value;

			if (entry > context->target.cluster_count + UINT64_C(1))
				break;
			error = target_fat_value(context, (uint32_t)entry, &value);
			if (error != EXFAT_RESIZE_SUCCESS)
				return error;
			error = exfat_resize_store_le32(context->io_buffer, byte_count, index * 4, value);
			if (error != EXFAT_RESIZE_SUCCESS)
				return EXFAT_RESIZE_INTERNAL_ERROR;
		}
		error = exfat_resize_block_device_write(context->device,
		    context->target.fat_offset + fat_sector, sector_count, context->io_buffer, byte_count);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		fat_sector += sector_count;
	}
	return EXFAT_RESIZE_SUCCESS;
}

static enum exfat_resize_error write_target_bitmap(struct resize_context *context)
{
	enum exfat_resize_error error;
	uint64_t bitmap_sector;
	uint64_t allocation_sector_count;
	uint64_t output_sector = 0;
	uint64_t target_bit = 0;

	error = cluster_sector(&context->target, context->new_bitmap.first_cluster, &bitmap_sector);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	allocation_sector_count = (context->new_bitmap.data_length + context->cluster_size - 1) /
	    context->cluster_size * context->target.sectors_per_cluster;
	context->used_cluster_count = 0;

	while (output_sector < allocation_sector_count) {
		uint64_t remaining = allocation_sector_count - output_sector;
		uint32_t sector_count = remaining > context->io_sector_capacity
		    ? context->io_sector_capacity
		    : (uint32_t)remaining;
		size_t byte_count = (size_t)sector_count * context->sector_size;
		size_t byte_index;

		memset(context->io_buffer, 0, byte_count);
		for (byte_index = 0; byte_index < byte_count; ++byte_index) {
			unsigned int bit_index;

			for (bit_index = 0; bit_index < 8; ++bit_index, ++target_bit) {
				if (target_bit >= context->target.cluster_count)
					break;
				if (context->allocation_model[target_bit] != 0) {
					context->io_buffer[byte_index] |= (unsigned char)(1u << bit_index);
					++context->used_cluster_count;
				}
			}
		}
		error = exfat_resize_block_device_write(context->device, bitmap_sector + output_sector,
		    sector_count, context->io_buffer, byte_count);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		output_sector += sector_count;
	}
	return EXFAT_RESIZE_SUCCESS;
}

/* Resize transaction */

static enum exfat_resize_error prepare_context(struct resize_context *context,
    const struct exfat_resize_block_device *device,
    uint64_t target_size,
    const struct exfat_resize_options *options)
{
	struct allocation_stream root;
	struct exfat_resize_device_geometry device_geometry;
	enum exfat_resize_error error;
	uint32_t filesystem_sector_size;
	uint64_t bitmap_clusters;
	uint64_t heap_movement;
	uint64_t model_size;
	uint64_t target_sector_count;
	size_t cache_index;

	memset(context, 0, sizeof(*context));
	context->allocator = options->allocator;
	context->stage = EXFAT_RESIZE_STAGE_PREFLIGHT;
	context->io_buffer =
	    context->allocator.allocate(context->allocator.context, EXFAT_IO_BUFFER_SIZE);
	if (context->io_buffer == NULL)
		return EXFAT_RESIZE_OUT_OF_MEMORY;

	error = exfat_resize_probe_sector_size(
	    device, context->io_buffer, EXFAT_IO_BUFFER_SIZE, &filesystem_sector_size);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error =
	    exfat_resize_adapt_block_device(device, filesystem_sector_size, &context->sector_adapter);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	context->device = &context->sector_adapter.device;
	context->sector_size = filesystem_sector_size;
	target_sector_count = target_size / filesystem_sector_size;
	if (target_sector_count > context->device->sector_count)
		return EXFAT_RESIZE_OUT_OF_BOUNDS;

	error = exfat_resize_read_boot_regions(
	    context->device, context->io_buffer, EXFAT_IO_BUFFER_SIZE, &context->source);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	context->io_sector_capacity = EXFAT_IO_BUFFER_SIZE / context->sector_size;

	error = load_source_fat(context);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = validate_reserved_fat_entries(context);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = exfat_resize_checked_multiply_u64(
	    context->source.sectors_per_cluster, context->sector_size, &context->cluster_size);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	device_geometry.logical_sector_size = context->device->sector_size;
	device_geometry.sector_count = context->device->sector_count;
	error = exfat_resize_plan_growth(
	    &device_geometry, &context->source, target_sector_count, &context->target);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;

	heap_movement =
	    (uint64_t)context->target.cluster_heap_offset - context->source.cluster_heap_offset;
	if (heap_movement % context->source.sectors_per_cluster != 0)
		return EXFAT_RESIZE_INTERNAL_ERROR;
	heap_movement /= context->source.sectors_per_cluster;
	context->displaced_cluster_count = heap_movement > context->source.cluster_count
	    ? context->source.cluster_count
	    : (uint32_t)heap_movement;

	context->new_bitmap.data_length = ((uint64_t)context->target.cluster_count + 7) / 8;
	error = exfat_resize_checked_ceil_divide_u64(
	    context->new_bitmap.data_length, context->cluster_size, &bitmap_clusters);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	if (bitmap_clusters == 0 || bitmap_clusters > context->target.cluster_count)
		return EXFAT_RESIZE_INTERNAL_ERROR;
	context->new_bitmap.first_cluster =
	    context->target.cluster_count + 2 - (uint32_t)bitmap_clusters;
	if (context->new_bitmap.first_cluster <= context->source.cluster_count + 1)
		return EXFAT_RESIZE_INSUFFICIENT_GROWTH;
	context->new_bitmap.no_fat_chain = 0;

	if (mapping_changes_cluster_numbers(context) &&
	    (uint64_t)context->source.cluster_count + 2 + context->displaced_cluster_count >
	        (uint64_t)context->target.cluster_count + 2)
		return EXFAT_RESIZE_INTERNAL_ERROR;

	if (context->sector_size > SIZE_MAX / (size_t)SECTOR_CACHE_COUNT)
		return EXFAT_RESIZE_ARITHMETIC_OVERFLOW;
	context->cache_buffer_size = (size_t)SECTOR_CACHE_COUNT * context->sector_size;
	context->cache_buffer =
	    context->allocator.allocate(context->allocator.context, context->cache_buffer_size);
	if (context->cache_buffer == NULL)
		return EXFAT_RESIZE_OUT_OF_MEMORY;
	for (cache_index = 0; cache_index < SECTOR_CACHE_COUNT; ++cache_index) {
		context->caches[cache_index].data =
		    context->cache_buffer + cache_index * context->sector_size;
	}

	model_size = (uint64_t)context->target.cluster_count * sizeof(*context->allocation_model);
	context->allocation_model_size = (size_t)model_size;
	if (context->allocation_model_size != model_size)
		return EXFAT_RESIZE_ARITHMETIC_OVERFLOW;
	context->allocation_model =
	    context->allocator.allocate(context->allocator.context, context->allocation_model_size);
	if (context->allocation_model == NULL)
		return EXFAT_RESIZE_OUT_OF_MEMORY;
	memset(context->allocation_model, 0, context->allocation_model_size);

	root.first_cluster = context->source.root_directory_cluster;
	root.data_length = 0;
	root.no_fat_chain = 0;
	root.root_directory = 1;
	error = claim_root_directory(context, root.first_cluster);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = scan_directory_tree(context, &root, DIRECTORY_SCAN_VALIDATE);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	if (!context->found_bitmap)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	error = validate_allocation_model(context);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = remove_old_bitmap_from_model(context);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = add_new_bitmap_to_model(context);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	release_source_fat(context);
	return EXFAT_RESIZE_SUCCESS;
}

static enum exfat_resize_error run_transaction(struct resize_context *context)
{
	struct allocation_stream target_root;
	enum exfat_resize_error error;

	/*
	 * Direct transaction I/O may make the source data caches stale, but they
	 * are reserved for preflight and are never consulted below. The target
	 * directory cache has not yet been populated.
	 */
	context->stage = EXFAT_RESIZE_STAGE_PREPARING;
	error =
	    exfat_resize_set_volume_dirty(context->device, context->io_buffer, EXFAT_IO_BUFFER_SIZE, 1);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;

	error = move_displaced_clusters(context);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;

	context->stage = EXFAT_RESIZE_STAGE_RESIZING;
	error = write_target_fat(context);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = write_target_bitmap(context);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;

	if (!mapping_changes_cluster_numbers(context)) {
		error = rewrite_identity_bitmap_entry(context);
	} else {
		target_root.first_cluster = context->target.root_directory_cluster;
		target_root.data_length = 0;
		target_root.no_fat_chain = 0;
		target_root.root_directory = 1;
		error = scan_directory_tree(context, &target_root, DIRECTORY_SCAN_REWRITE);
	}
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = flush_cache(context, SECTOR_CACHE_TARGET_DIRECTORY_DATA);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = exfat_resize_block_device_sync(context->device);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;

	error = exfat_resize_write_boot_regions(context->device, &context->target,
	    context->used_cluster_count, context->io_buffer, EXFAT_IO_BUFFER_SIZE);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;

	context->stage = EXFAT_RESIZE_STAGE_FINALIZING;
	error =
	    exfat_resize_set_volume_dirty(context->device, context->io_buffer, EXFAT_IO_BUFFER_SIZE, 0);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;

	context->stage = EXFAT_RESIZE_STAGE_COMPLETED;
	return EXFAT_RESIZE_SUCCESS;
}

static void release_context(struct resize_context *context)
{
	release_source_fat(context);
	if (context->directories.items != NULL) {
		context->allocator.deallocate(context->allocator.context, context->directories.items,
		    context->directories.capacity * sizeof(*context->directories.items));
		context->directories.items = NULL;
	}
	if (context->allocation_model != NULL) {
		context->allocator.deallocate(
		    context->allocator.context, context->allocation_model, context->allocation_model_size);
		context->allocation_model = NULL;
	}
	if (context->cache_buffer != NULL) {
		context->allocator.deallocate(
		    context->allocator.context, context->cache_buffer, context->cache_buffer_size);
		context->cache_buffer = NULL;
	}
	if (context->io_buffer != NULL) {
		context->allocator.deallocate(
		    context->allocator.context, context->io_buffer, EXFAT_IO_BUFFER_SIZE);
		context->io_buffer = NULL;
	}
}

enum exfat_resize_error exfat_resize(const struct exfat_resize_block_device *device,
    uint64_t target_size,
    const struct exfat_resize_options *options,
    enum exfat_resize_stage *stage)
{
	struct resize_context context = { .stage = EXFAT_RESIZE_STAGE_PREFLIGHT };
	enum exfat_resize_error error;
	enum exfat_resize_stage final_stage;

	error = exfat_resize_validate_block_device(device);
	if (error != EXFAT_RESIZE_SUCCESS)
		goto out;
	if (target_size == 0) {
		error = EXFAT_RESIZE_INVALID_ARGUMENT;
		goto out;
	}
	if (options == NULL || options->allocator.allocate == NULL ||
	    options->allocator.deallocate == NULL) {
		error = EXFAT_RESIZE_INVALID_ARGUMENT;
		goto out;
	}

	error = prepare_context(&context, device, target_size, options);
	if (error != EXFAT_RESIZE_SUCCESS)
		goto out;

	error = run_transaction(&context);

out:
	final_stage = context.stage;
	release_context(&context);
	if (stage != NULL)
		*stage = final_stage;
	return error;
}
