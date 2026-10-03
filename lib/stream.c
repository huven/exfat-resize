/* SPDX-License-Identifier: MIT */

#include "common.h"

#include "checked_math.h"
#include "resize_internal.h"
#include "stream.h"
#include "volume.h"

#include <string.h>

int exfat_resize_cluster_is_valid(const struct exfat_resize_geometry *geometry, uint32_t cluster)
{
	return cluster >= 2 && cluster <= geometry->cluster_count + UINT32_C(1);
}

enum exfat_resize_error exfat_resize_stream_cluster_count(
    uint64_t cluster_size, const struct allocation_stream *stream, uint32_t *cluster_count)
{
	uint64_t count;
	enum exfat_resize_error error;

	error = exfat_resize_checked_ceil_divide_u64(stream->data_length, cluster_size, &count);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	if (count > UINT32_MAX)
		return EXFAT_RESIZE_OUT_OF_BOUNDS;
	*cluster_count = (uint32_t)count;
	return EXFAT_RESIZE_SUCCESS;
}

static enum exfat_resize_error next_stream_cluster(struct stream_cursor *cursor)
{
	enum exfat_resize_error error;
	uint32_t next;

	if (cursor->no_fat_chain) {
		next = cursor->current_cluster + 1;
	} else {
		error = cursor->chain.next(cursor->chain.context, cursor->current_cluster, &next);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
	}
	if (!cursor->no_fat_chain) {
		if (next == EXFAT_FAT_END_OF_CHAIN) {
			if (!cursor->root_directory && cursor->remaining_bytes != 0)
				return EXFAT_RESIZE_INVALID_FILESYSTEM;
			cursor->exhausted = 1;
			return EXFAT_RESIZE_SUCCESS;
		}
	}

	++cursor->traversed_clusters;
	if (cursor->traversed_clusters >= cursor->geometry->cluster_count ||
	    !exfat_resize_cluster_is_valid(cursor->geometry, next))
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	cursor->current_cluster = next;
	cursor->cluster_offset = 0;
	return EXFAT_RESIZE_SUCCESS;
}

enum exfat_resize_error exfat_resize_initialize_stream_cursor(
    const struct exfat_resize_geometry *geometry,
    const struct allocation_stream *stream,
    enum sector_cache_index data_cache,
    const struct stream_chain_reader *chain,
    struct stream_cursor *cursor)
{
	if (chain == NULL || (!stream->no_fat_chain && chain->next == NULL))
		return EXFAT_RESIZE_INVALID_ARGUMENT;
	if (!exfat_resize_cluster_is_valid(geometry, stream->first_cluster))
		return EXFAT_RESIZE_INVALID_FILESYSTEM;

	memset(cursor, 0, sizeof(*cursor));
	cursor->geometry = geometry;
	cursor->data_cache = data_cache;
	cursor->chain = *chain;
	cursor->current_cluster = stream->first_cluster;
	cursor->no_fat_chain = stream->no_fat_chain;
	cursor->root_directory = stream->root_directory;
	cursor->remaining_bytes = stream->root_directory ? UINT64_MAX : stream->data_length;
	if (cursor->remaining_bytes == 0)
		cursor->exhausted = 1;
	return EXFAT_RESIZE_SUCCESS;
}

static enum exfat_resize_error advance_stream_cursor(
    struct resize_volume *volume, struct stream_cursor *cursor, size_t count)
{
	enum exfat_resize_error error;

	if (!cursor->root_directory)
		cursor->remaining_bytes -= count;
	cursor->cluster_offset += count;
	if (!cursor->root_directory && cursor->remaining_bytes == 0) {
		cursor->exhausted = 1;
		return EXFAT_RESIZE_SUCCESS;
	}
	if (cursor->cluster_offset == volume->cluster_size) {
		error = next_stream_cluster(cursor);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
	}
	return EXFAT_RESIZE_SUCCESS;
}

static enum exfat_resize_error contiguous_stream_sector_count(struct resize_volume *volume,
    const struct stream_cursor *cursor,
    uint32_t maximum_sector_count,
    uint32_t *sector_count)
{
	struct stream_cursor probe = *cursor;
	enum exfat_resize_error error;
	uint64_t cluster_start;
	uint64_t first_sector = 0;
	uint64_t sector;
	uint32_t count = 0;
	size_t advance;
	size_t sector_offset;

	while (!probe.exhausted && count < maximum_sector_count) {
		error = exfat_resize_cluster_sector(probe.geometry, probe.current_cluster, &cluster_start);
		if (error != EXFAT_RESIZE_SUCCESS)
			return EXFAT_RESIZE_INVALID_FILESYSTEM;
		sector = cluster_start + probe.cluster_offset / volume->sector_size;
		if (count == 0)
			first_sector = sector;
		else if (sector != first_sector + count)
			break;
		++count;

		sector_offset = (size_t)(probe.cluster_offset % volume->sector_size);
		advance = volume->sector_size - sector_offset;
		if (!probe.root_directory && probe.remaining_bytes < advance)
			advance = (size_t)probe.remaining_bytes;
		error = advance_stream_cursor(volume, &probe, advance);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
	}
	if (count == 0)
		return EXFAT_RESIZE_INTERNAL_ERROR;
	*sector_count = count;
	return EXFAT_RESIZE_SUCCESS;
}

enum exfat_resize_error exfat_resize_read_stream(
    struct resize_volume *volume, struct stream_cursor *cursor, void *buffer, size_t count)
{
	struct sector_cache *cache = &volume->caches[cursor->data_cache];
	unsigned char *destination = buffer;
	enum exfat_resize_error error;
	uint64_t cluster_start;
	uint64_t sector;
	uint64_t cache_sector_offset;
	uint64_t consumed_sectors;
	uint32_t maximum_sector_count;
	uint32_t read_sector_count;
	size_t sector_offset;
	size_t available;
	size_t part;

	if (count != 0 && buffer == NULL)
		return EXFAT_RESIZE_INVALID_ARGUMENT;
	if (cursor->exhausted || (!cursor->root_directory && count > cursor->remaining_bytes))
		return EXFAT_RESIZE_OUT_OF_BOUNDS;

	while (count != 0) {
		error =
		    exfat_resize_cluster_sector(cursor->geometry, cursor->current_cluster, &cluster_start);
		if (error != EXFAT_RESIZE_SUCCESS)
			return EXFAT_RESIZE_INVALID_FILESYSTEM;
		sector = cluster_start + cursor->cluster_offset / volume->sector_size;
		sector_offset = (size_t)(cursor->cluster_offset % volume->sector_size);
		available = volume->sector_size - sector_offset;
		part = count < available ? count : available;

		if (!exfat_resize_cache_contains_sector(cache, sector)) {
			maximum_sector_count = cache->sector_capacity;
			if (cursor->data_cache == SECTOR_CACHE_SOURCE_DIRECTORY_DATA ||
			    cursor->data_cache == SECTOR_CACHE_TARGET_DIRECTORY_DATA) {
				/*
				 * For directories, the caller requests one 32-byte entry per call.
				 * Start directories with one sector, then grow read-ahead with
				 * scan progress: uninterrupted fills read 1, 2, 4, 8, ... sectors.
				 * Cache evictions during entry-set rewriting do not advance it.
				 */
				consumed_sectors =
				    (uint64_t)cursor->traversed_clusters * cursor->geometry->sectors_per_cluster +
				    cursor->cluster_offset / volume->sector_size;
				if (consumed_sectors < maximum_sector_count)
					maximum_sector_count = (uint32_t)consumed_sectors + 1;
			}
			error = contiguous_stream_sector_count(
			    volume, cursor, maximum_sector_count, &read_sector_count);
			if (error != EXFAT_RESIZE_SUCCESS)
				return error;
			error = exfat_resize_load_cache(volume, cursor->data_cache, sector, read_sector_count);
			if (error != EXFAT_RESIZE_SUCCESS)
				return error;
		}
		cache_sector_offset = sector - cache->first_sector;
		memcpy(destination,
		    cache->data + (size_t)cache_sector_offset * volume->sector_size + sector_offset, part);
		error = advance_stream_cursor(volume, cursor, part);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		destination += part;
		count -= part;
	}
	return EXFAT_RESIZE_SUCCESS;
}

enum exfat_resize_error exfat_resize_skip_stream(
    struct resize_volume *volume, struct stream_cursor *cursor, uint64_t count)
{
	enum exfat_resize_error error;
	if (!cursor->root_directory && count > cursor->remaining_bytes)
		return EXFAT_RESIZE_OUT_OF_BOUNDS;
	while (count != 0) {
		uint64_t available = volume->cluster_size - cursor->cluster_offset;
		size_t part = (size_t)(count < available ? count : available);
		if (cursor->exhausted)
			return EXFAT_RESIZE_OUT_OF_BOUNDS;
		error = exfat_resize_cluster_step_checkpoint(volume->operation);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		error = advance_stream_cursor(volume, cursor, part);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		count -= part;
	}
	return EXFAT_RESIZE_SUCCESS;
}
