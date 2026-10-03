/* SPDX-License-Identifier: MIT */

#include "common.h"

#include "block_device.h"
#include "resize_internal.h"
#include "volume.h"

_Static_assert(EXFAT_SECTOR_CACHE_SIZE >= EXFAT_RESIZE_MAX_SECTOR_SIZE,
    "a sector cache must hold at least one maximum-sized sector");
_Static_assert(EXFAT_IO_MAX_CHUNK_SIZE >= EXFAT_RESIZE_MAX_SECTOR_SIZE,
    "an I/O chunk must hold at least one maximum-sized sector");

enum exfat_resize_error exfat_resize_flush_cache(
    struct resize_context *context, enum sector_cache_index cache_index)
{
	struct sector_cache *cache = &context->caches[cache_index];
	enum exfat_resize_error error;
	uint32_t dirty_count;
	size_t buffer_size;

	if (cache->dirty_first > cache->dirty_end || cache->dirty_end > cache->sector_capacity)
		return EXFAT_RESIZE_INTERNAL_ERROR;
	if (cache->dirty_first == cache->dirty_end)
		return EXFAT_RESIZE_SUCCESS;
	dirty_count = cache->dirty_end - cache->dirty_first;
	buffer_size = (size_t)(cache->sector_capacity - cache->dirty_first) * context->sector_size;
	error = exfat_resize_block_device_write(context->device,
	    cache->first_sector + cache->dirty_first, dirty_count,
	    cache->data + (size_t)cache->dirty_first * context->sector_size, buffer_size);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	cache->dirty_first = 0;
	cache->dirty_end = 0;
	return EXFAT_RESIZE_SUCCESS;
}

int exfat_resize_cache_contains_sector(const struct sector_cache *cache, uint64_t sector)
{
	return sector >= cache->first_sector && sector - cache->first_sector < cache->sector_count;
}

static int cache_contains_range(
    const struct sector_cache *cache, uint64_t first_sector, uint32_t sector_count)
{
	if (first_sector < cache->first_sector || sector_count > cache->sector_count)
		return 0;
	return first_sector - cache->first_sector <= cache->sector_count - sector_count;
}

enum exfat_resize_error exfat_resize_load_cache(struct resize_context *context,
    enum sector_cache_index cache_index,
    uint64_t first_sector,
    uint32_t sector_count)
{
	struct sector_cache *cache = &context->caches[cache_index];
	enum exfat_resize_error error;

	if (sector_count == 0 || sector_count > cache->sector_capacity)
		return EXFAT_RESIZE_INTERNAL_ERROR;
	if (cache_contains_range(cache, first_sector, sector_count))
		return EXFAT_RESIZE_SUCCESS;

	error = exfat_resize_cancellation_checkpoint(context);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = exfat_resize_flush_cache(context, cache_index);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;

	cache->sector_count = 0;
	error = exfat_resize_block_device_read(context->device, first_sector, sector_count, cache->data,
	    (size_t)cache->sector_capacity * context->sector_size);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	cache->first_sector = first_sector;
	cache->sector_count = sector_count;
	cache->dirty_first = 0;
	cache->dirty_end = 0;
	return EXFAT_RESIZE_SUCCESS;
}

enum exfat_resize_error exfat_resize_cluster_sector(
    const struct exfat_resize_geometry *geometry, uint32_t cluster, uint64_t *sector)
{
	uint64_t cluster_offset;
	uint64_t result;

	if (cluster < 2 || cluster > geometry->cluster_count + UINT32_C(1))
		return EXFAT_RESIZE_OUT_OF_BOUNDS;
	cluster_offset = ((uint64_t)cluster - 2) * geometry->sectors_per_cluster;
	result = geometry->cluster_heap_offset + cluster_offset;
	if (result >= geometry->volume_sector_count)
		return EXFAT_RESIZE_OUT_OF_BOUNDS;
	*sector = result;
	return EXFAT_RESIZE_SUCCESS;
}

enum exfat_resize_error exfat_resize_copy_cluster_run(struct resize_context *context,
    uint32_t source_cluster,
    uint32_t target_cluster,
    uint32_t cluster_count)
{
	enum exfat_resize_error error;
	uint64_t source_sector;
	uint64_t target_sector;
	uint64_t copied = 0;
	uint64_t sector_count;

	error = exfat_resize_cluster_sector(&context->source, source_cluster, &source_sector);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = exfat_resize_cluster_sector(&context->target, target_cluster, &target_sector);
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

		error = exfat_resize_cancellation_checkpoint(context);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		error = exfat_resize_block_device_read(context->device, source_sector + copied, count,
		    context->io_buffer, EXFAT_IO_BUFFER_SIZE);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		error = exfat_resize_block_device_write(context->device, target_sector + copied, count,
		    context->io_buffer, EXFAT_IO_BUFFER_SIZE);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		copied += count;
	}
	return EXFAT_RESIZE_SUCCESS;
}
