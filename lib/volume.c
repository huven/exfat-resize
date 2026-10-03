/* SPDX-License-Identifier: MIT */

#include "common.h"

#include "block_device.h"
#include "boot_region.h"
#include "resize_internal.h"
#include "volume.h"

_Static_assert(EXFAT_SECTOR_CACHE_SIZE >= EXFAT_RESIZE_MAX_SECTOR_SIZE,
    "a sector cache must hold at least one maximum-sized sector");
_Static_assert(EXFAT_IO_MAX_CHUNK_SIZE >= EXFAT_RESIZE_MAX_SECTOR_SIZE,
    "an I/O chunk must hold at least one maximum-sized sector");

static int ranges_overlap(uint64_t first, uint64_t count, uint64_t other, uint64_t other_count)
{
	if (count == 0 || other_count == 0)
		return 0;
	return first <= other ? other - first < count : first - other < other_count;
}

static enum exfat_resize_error check_range(
    const struct resize_volume *volume, uint64_t first, uint64_t count)
{
	return first > volume->device->sector_count || count > volume->device->sector_count - first
	    ? EXFAT_RESIZE_OUT_OF_BOUNDS
	    : EXFAT_RESIZE_SUCCESS;
}

static enum exfat_resize_error require_clean_range(
    struct resize_volume *volume, uint64_t first, uint64_t count, enum sector_cache_index excluded)
{
	size_t index;
	enum exfat_resize_error error = check_range(volume, first, count);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	for (index = 0; index < SECTOR_CACHE_COUNT; ++index) {
		const struct sector_cache *cache = &volume->caches[index];
		if (index != (size_t)excluded && cache->dirty_first != cache->dirty_end &&
		    ranges_overlap(first, count, cache->first_sector, cache->sector_count))
			return EXFAT_RESIZE_INTERNAL_ERROR;
	}
	return EXFAT_RESIZE_SUCCESS;
}

static enum exfat_resize_error invalidate_aliases(
    struct resize_volume *volume, uint64_t first, uint64_t count, enum sector_cache_index excluded)
{
	size_t index;
	enum exfat_resize_error error = require_clean_range(volume, first, count, excluded);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	for (index = 0; index < SECTOR_CACHE_COUNT; ++index) {
		struct sector_cache *cache = &volume->caches[index];
		if (index != (size_t)excluded &&
		    ranges_overlap(first, count, cache->first_sector, cache->sector_count))
			cache->sector_count = 0;
	}
	return EXFAT_RESIZE_SUCCESS;
}

enum exfat_resize_error exfat_resize_flush_cache(
    struct resize_volume *volume, enum sector_cache_index cache_index)
{
	struct sector_cache *cache = &volume->caches[cache_index];
	enum exfat_resize_error error;
	uint32_t dirty_count;
	size_t buffer_size;

	if (cache->dirty_first > cache->dirty_end || cache->dirty_end > cache->sector_capacity)
		return EXFAT_RESIZE_INTERNAL_ERROR;
	if (cache->dirty_first == cache->dirty_end)
		return EXFAT_RESIZE_SUCCESS;
	dirty_count = cache->dirty_end - cache->dirty_first;
	buffer_size = (size_t)(cache->sector_capacity - cache->dirty_first) * volume->sector_size;
	error = invalidate_aliases(
	    volume, cache->first_sector + cache->dirty_first, dirty_count, cache_index);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = exfat_resize_block_device_write(volume->device,
	    cache->first_sector + cache->dirty_first, dirty_count,
	    cache->data + (size_t)cache->dirty_first * volume->sector_size, buffer_size);
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

enum exfat_resize_error exfat_resize_load_cache(struct resize_volume *volume,
    enum sector_cache_index cache_index,
    uint64_t first_sector,
    uint32_t sector_count)
{
	struct sector_cache *cache = &volume->caches[cache_index];
	enum exfat_resize_error error;

	if (sector_count == 0 || sector_count > cache->sector_capacity)
		return EXFAT_RESIZE_INTERNAL_ERROR;
	if (cache_contains_range(cache, first_sector, sector_count))
		return EXFAT_RESIZE_SUCCESS;

	error = exfat_resize_cancellation_checkpoint(volume->operation);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = exfat_resize_flush_cache(volume, cache_index);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;

	cache->sector_count = 0;
	error = exfat_resize_read_volume(volume, first_sector, sector_count, cache->data,
	    (size_t)cache->sector_capacity * volume->sector_size);
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

enum exfat_resize_error exfat_resize_copy_cluster_run(struct resize_volume *volume,
    const struct exfat_resize_geometry *target,
    uint32_t source_cluster,
    uint32_t target_cluster,
    uint32_t cluster_count)
{
	enum exfat_resize_error error;
	uint64_t source_sector;
	uint64_t target_sector;
	uint64_t copied = 0;
	uint64_t sector_count;

	error = exfat_resize_cluster_sector(&volume->geometry, source_cluster, &source_sector);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = exfat_resize_cluster_sector(target, target_cluster, &target_sector);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	sector_count = (uint64_t)cluster_count * volume->geometry.sectors_per_cluster;
	if (sector_count > volume->geometry.volume_sector_count - source_sector ||
	    sector_count > target->volume_sector_count - target_sector)
		return EXFAT_RESIZE_INTERNAL_ERROR;

	while (copied < sector_count) {
		uint64_t remaining = sector_count - copied;
		uint32_t count = remaining > volume->io_sector_capacity ? volume->io_sector_capacity
		                                                        : (uint32_t)remaining;

		error = exfat_resize_cancellation_checkpoint(volume->operation);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		error = exfat_resize_read_volume(
		    volume, source_sector + copied, count, volume->io_buffer, EXFAT_IO_BUFFER_SIZE);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		error = exfat_resize_write_volume(
		    volume, target_sector + copied, count, volume->io_buffer, EXFAT_IO_BUFFER_SIZE);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		copied += count;
	}
	return EXFAT_RESIZE_SUCCESS;
}

enum exfat_resize_error exfat_resize_open_volume(
    struct resize_volume *volume, const struct exfat_resize_block_device *device)
{
	enum exfat_resize_error error;
	uint32_t filesystem_sector_size;

	volume->io_buffer = volume->operation->allocator.allocate(
	    volume->operation->allocator.context, EXFAT_IO_BUFFER_SIZE);
	if (volume->io_buffer == NULL)
		return EXFAT_RESIZE_OUT_OF_MEMORY;

	error = exfat_resize_probe_sector_size(
	    device, volume->io_buffer, EXFAT_IO_BUFFER_SIZE, &filesystem_sector_size);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error =
	    exfat_resize_adapt_block_device(device, filesystem_sector_size, &volume->sector_adapter);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	volume->device = &volume->sector_adapter.device;
	volume->sector_size = filesystem_sector_size;
	return EXFAT_RESIZE_SUCCESS;
}

enum exfat_resize_error exfat_resize_allocate_volume_caches(struct resize_volume *volume)
{
	size_t cache_index;

	volume->cache_buffer = volume->operation->allocator.allocate(
	    volume->operation->allocator.context, EXFAT_SECTOR_CACHE_BUFFER_SIZE);
	if (volume->cache_buffer == NULL)
		return EXFAT_RESIZE_OUT_OF_MEMORY;
	for (cache_index = 0; cache_index < SECTOR_CACHE_COUNT; ++cache_index) {
		volume->caches[cache_index].data =
		    volume->cache_buffer + cache_index * EXFAT_SECTOR_CACHE_SIZE;
		volume->caches[cache_index].sector_capacity =
		    (uint32_t)(EXFAT_SECTOR_CACHE_SIZE / volume->sector_size);
	}

	return EXFAT_RESIZE_SUCCESS;
}

void exfat_resize_close_volume(struct resize_volume *volume)
{
	if (volume->cache_buffer != NULL) {
		volume->operation->allocator.deallocate(volume->operation->allocator.context,
		    volume->cache_buffer, EXFAT_SECTOR_CACHE_BUFFER_SIZE);
		volume->cache_buffer = NULL;
	}
	if (volume->io_buffer != NULL) {
		volume->operation->allocator.deallocate(
		    volume->operation->allocator.context, volume->io_buffer, EXFAT_IO_BUFFER_SIZE);
		volume->io_buffer = NULL;
	}
}

enum exfat_resize_error exfat_resize_read_volume(struct resize_volume *volume,
    uint64_t first_sector,
    uint32_t sector_count,
    void *buffer,
    size_t buffer_size)
{
	enum exfat_resize_error error =
	    require_clean_range(volume, first_sector, sector_count, SECTOR_CACHE_COUNT);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	return exfat_resize_block_device_read(
	    volume->device, first_sector, sector_count, buffer, buffer_size);
}

enum exfat_resize_error exfat_resize_write_volume(struct resize_volume *volume,
    uint64_t first_sector,
    uint32_t sector_count,
    const void *buffer,
    size_t buffer_size)
{
	enum exfat_resize_error error =
	    exfat_resize_invalidate_volume_range(volume, first_sector, sector_count);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	return exfat_resize_block_device_write(
	    volume->device, first_sector, sector_count, buffer, buffer_size);
}

enum exfat_resize_error exfat_resize_invalidate_volume_range(
    struct resize_volume *volume, uint64_t first_sector, uint64_t sector_count)
{
	return invalidate_aliases(volume, first_sector, sector_count, SECTOR_CACHE_COUNT);
}

enum exfat_resize_error exfat_resize_prepare_cached_write(struct resize_volume *volume,
    enum sector_cache_index cache_index,
    uint64_t first_sector,
    uint32_t sector_count)
{
	if (!cache_contains_range(&volume->caches[cache_index], first_sector, sector_count))
		return EXFAT_RESIZE_INTERNAL_ERROR;
	return invalidate_aliases(volume, first_sector, sector_count, cache_index);
}

enum exfat_resize_error exfat_resize_flush_volume_range(
    struct resize_volume *volume, uint64_t first_sector, uint64_t sector_count)
{
	size_t index;
	enum exfat_resize_error error = check_range(volume, first_sector, sector_count);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	for (index = 0; index < SECTOR_CACHE_COUNT; ++index) {
		struct sector_cache *cache = &volume->caches[index];
		if (ranges_overlap(first_sector, sector_count, cache->first_sector, cache->sector_count)) {
			error = exfat_resize_flush_cache(volume, (enum sector_cache_index)index);
			if (error != EXFAT_RESIZE_SUCCESS)
				return error;
		}
	}
	return EXFAT_RESIZE_SUCCESS;
}
