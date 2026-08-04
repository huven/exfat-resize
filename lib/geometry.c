/* SPDX-License-Identifier: MIT */

#include "common.h"

#include "geometry.h"

#include "block_device.h"

#define EXFAT_MAX_CLUSTER_COUNT (UINT32_MAX - UINT32_C(10))

uint32_t exfat_resize_used_fat_sector_count(uint32_t cluster_count, uint32_t sector_size)
{
	uint64_t byte_count = ((uint64_t)cluster_count + 2) * 4;

	return (uint32_t)((byte_count + sector_size - 1) / sector_size);
}

static int candidate_fits(const struct exfat_resize_device_geometry *device_geometry,
    const struct exfat_resize_geometry *source,
    uint64_t target_volume_sector_count,
    uint32_t candidate_cluster_count,
    uint32_t *fat_length,
    uint32_t *cluster_heap_offset)
{
	const uint32_t sector_size = device_geometry->logical_sector_size;
	const uint64_t sectors_per_cluster = source->sectors_per_cluster;
	uint32_t candidate_fat_length;
	uint64_t candidate_heap_offset;
	uint64_t fat_end;
	uint64_t heap_movement;

	candidate_fat_length = exfat_resize_used_fat_sector_count(candidate_cluster_count, sector_size);
	if (candidate_fat_length < source->fat_length)
		candidate_fat_length = source->fat_length;
	candidate_heap_offset = source->cluster_heap_offset;
	fat_end = (uint64_t)source->fat_offset + candidate_fat_length;
	if (fat_end > candidate_heap_offset) {
		heap_movement = fat_end - candidate_heap_offset;
		heap_movement = (heap_movement + sectors_per_cluster - 1) / sectors_per_cluster;
		candidate_heap_offset += heap_movement * sectors_per_cluster;
	}
	if (candidate_heap_offset > UINT32_MAX || target_volume_sector_count <= candidate_heap_offset ||
	    candidate_cluster_count >
	        (target_volume_sector_count - candidate_heap_offset) / sectors_per_cluster)
		return 0;

	*fat_length = candidate_fat_length;
	*cluster_heap_offset = (uint32_t)candidate_heap_offset;
	return 1;
}

static int target_geometry_is_consistent(const struct exfat_resize_device_geometry *device_geometry,
    const struct exfat_resize_geometry *source,
    const struct exfat_resize_geometry *target)
{
	uint64_t available_clusters;
	uint64_t fat_end;
	uint64_t heap_movement;
	uint32_t minimum_fat_length;

	if (target->cluster_heap_offset < source->cluster_heap_offset ||
	    target->volume_sector_count <= target->cluster_heap_offset)
		return 0;

	heap_movement = (uint64_t)target->cluster_heap_offset - source->cluster_heap_offset;
	if (heap_movement % source->sectors_per_cluster != 0)
		return 0;

	fat_end = (uint64_t)target->fat_offset + target->fat_length;
	if (fat_end > target->cluster_heap_offset)
		return 0;

	minimum_fat_length = exfat_resize_used_fat_sector_count(
	    target->cluster_count, device_geometry->logical_sector_size);
	if (target->fat_length < minimum_fat_length)
		return 0;

	available_clusters =
	    (target->volume_sector_count - target->cluster_heap_offset) / target->sectors_per_cluster;
	if (available_clusters > EXFAT_MAX_CLUSTER_COUNT)
		available_clusters = EXFAT_MAX_CLUSTER_COUNT;
	return target->cluster_count == available_clusters;
}

enum exfat_resize_error exfat_resize_map_growth_cluster(const struct exfat_resize_geometry *source,
    const struct exfat_resize_geometry *target,
    uint32_t source_cluster,
    uint32_t *target_cluster)
{
	uint32_t displaced_cluster_count;
	uint32_t heap_shift_clusters;
	uint32_t mapped_cluster;
	uint32_t remaining_cluster_count;
	uint32_t source_index;
	uint64_t heap_movement;

	if (source == NULL || target == NULL || target_cluster == NULL)
		return EXFAT_RESIZE_INVALID_ARGUMENT;
	if (source->sectors_per_cluster == 0 ||
	    target->sectors_per_cluster != source->sectors_per_cluster ||
	    target->cluster_heap_offset < source->cluster_heap_offset ||
	    target->cluster_count < source->cluster_count ||
	    source->cluster_count > EXFAT_MAX_CLUSTER_COUNT ||
	    target->cluster_count > EXFAT_MAX_CLUSTER_COUNT)
		return EXFAT_RESIZE_INVALID_ARGUMENT;
	if (source_cluster < 2) {
		*target_cluster = source_cluster;
		return EXFAT_RESIZE_SUCCESS;
	}
	if (source_cluster > source->cluster_count + UINT32_C(1))
		return EXFAT_RESIZE_OUT_OF_BOUNDS;

	heap_movement = (uint64_t)target->cluster_heap_offset - source->cluster_heap_offset;
	if (heap_movement % source->sectors_per_cluster != 0)
		return EXFAT_RESIZE_INVALID_ARGUMENT;
	heap_shift_clusters = (uint32_t)(heap_movement / source->sectors_per_cluster);
	displaced_cluster_count = heap_shift_clusters;
	if (displaced_cluster_count > source->cluster_count)
		displaced_cluster_count = source->cluster_count;

	remaining_cluster_count = source->cluster_count - displaced_cluster_count;
	source_index = source_cluster - 2;
	/* Both branches produce a cluster in [2, source->cluster_count + 1]. */
	if (source_index < displaced_cluster_count)
		mapped_cluster = remaining_cluster_count + 2 + source_index;
	else
		mapped_cluster = 2 + source_index - displaced_cluster_count;

	*target_cluster = mapped_cluster;
	return EXFAT_RESIZE_SUCCESS;
}

enum exfat_resize_error exfat_resize_plan_growth(
    const struct exfat_resize_device_geometry *device_geometry,
    const struct exfat_resize_geometry *source,
    uint64_t target_volume_sector_count,
    struct exfat_resize_geometry *target)
{
	struct exfat_resize_geometry result;
	enum exfat_resize_error error;
	uint64_t maximum_cluster_count;
	uint32_t candidate;
	uint32_t fat_length = 0;
	uint32_t high;
	uint32_t low = 0;
	uint32_t cluster_heap_offset = 0;
	uint32_t root_directory_cluster;
	uint64_t adjusted_heap_offset;
	uint64_t usable_cluster_count;

	if (device_geometry == NULL || source == NULL || target == NULL)
		return EXFAT_RESIZE_INVALID_ARGUMENT;
	if (device_geometry->logical_sector_size == 0 || source->sectors_per_cluster == 0 ||
	    source->cluster_count > EXFAT_MAX_CLUSTER_COUNT)
		return EXFAT_RESIZE_INVALID_ARGUMENT;
	if (target_volume_sector_count <= source->volume_sector_count)
		return EXFAT_RESIZE_INVALID_ARGUMENT;
	if (target_volume_sector_count > device_geometry->sector_count)
		return EXFAT_RESIZE_OUT_OF_BOUNDS;

	if (target_volume_sector_count <= source->cluster_heap_offset)
		return EXFAT_RESIZE_INTERNAL_ERROR;
	maximum_cluster_count =
	    (target_volume_sector_count - source->cluster_heap_offset) / source->sectors_per_cluster;
	if (maximum_cluster_count > EXFAT_MAX_CLUSTER_COUNT)
		maximum_cluster_count = EXFAT_MAX_CLUSTER_COUNT;
	high = (uint32_t)maximum_cluster_count;

	while (low < high) {
		candidate = low + (uint32_t)(((uint64_t)high - low + 1) / 2);
		if (candidate_fits(device_geometry, source, target_volume_sector_count, candidate,
		        &fat_length, &cluster_heap_offset))
			low = candidate;
		else
			high = candidate - 1;
	}
	if (low <= source->cluster_count) {
		if (source->cluster_count == EXFAT_MAX_CLUSTER_COUNT)
			return EXFAT_RESIZE_CLUSTER_LIMIT_REACHED;
		return EXFAT_RESIZE_INSUFFICIENT_GROWTH;
	}
	if (!candidate_fits(device_geometry, source, target_volume_sector_count, low, &fat_length,
	        &cluster_heap_offset))
		return EXFAT_RESIZE_INTERNAL_ERROR;

	/*
	 * candidate_fits() places the heap at the earliest FAT-safe offset. If
	 * that leaves room for more clusters than the binary search selected,
	 * move the heap forward by whole clusters until ClusterCount is exact.
	 */
	usable_cluster_count =
	    (target_volume_sector_count - cluster_heap_offset) / source->sectors_per_cluster;
	if (usable_cluster_count > EXFAT_MAX_CLUSTER_COUNT)
		usable_cluster_count = EXFAT_MAX_CLUSTER_COUNT;
	if (usable_cluster_count < low)
		return EXFAT_RESIZE_INTERNAL_ERROR;
	adjusted_heap_offset =
	    cluster_heap_offset + (usable_cluster_count - low) * source->sectors_per_cluster;
	if (adjusted_heap_offset > UINT32_MAX)
		return EXFAT_RESIZE_INTERNAL_ERROR;
	cluster_heap_offset = (uint32_t)adjusted_heap_offset;

	result = *source;
	result.volume_sector_count = target_volume_sector_count;
	result.fat_length = fat_length;
	result.cluster_heap_offset = cluster_heap_offset;
	result.cluster_count = low;
	if (!target_geometry_is_consistent(device_geometry, source, &result))
		return EXFAT_RESIZE_INTERNAL_ERROR;

	error = exfat_resize_map_growth_cluster(
	    source, &result, source->root_directory_cluster, &root_directory_cluster);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	result.root_directory_cluster = root_directory_cluster;

	*target = result;
	return EXFAT_RESIZE_SUCCESS;
}
