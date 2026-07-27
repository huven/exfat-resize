/* SPDX-License-Identifier: MIT */

#ifndef EXFAT_RESIZE_GEOMETRY_H
#define EXFAT_RESIZE_GEOMETRY_H

#include "block_device.h"

#include <stdint.h>

struct exfat_resize_geometry {
	uint64_t volume_sector_count;
	uint32_t sectors_per_cluster;
	uint32_t fat_offset;
	uint32_t fat_length;
	uint32_t cluster_heap_offset;
	uint32_t cluster_count;
	uint32_t root_directory_cluster;
};

/*
 * Calculates the largest geometry that fits target_volume_sector_count.
 *
 * device_geometry and source must already have been validated. This function
 * plans growth only: the target must be larger than the source volume and no
 * larger than the device. target is modified only on success.
 */
enum exfat_resize_error exfat_resize_plan_growth(
    const struct exfat_resize_device_geometry *device_geometry,
    const struct exfat_resize_geometry *source,
    uint64_t target_volume_sector_count,
    struct exfat_resize_geometry *target);

/*
 * Maps a cluster number from source geometry to its cluster number in target
 * growth geometry. Cluster numbers 0 and 1 are returned unchanged.
 * target_cluster is modified only on success.
 */
enum exfat_resize_error exfat_resize_map_growth_cluster(const struct exfat_resize_geometry *source,
    const struct exfat_resize_geometry *target,
    uint32_t source_cluster,
    uint32_t *target_cluster);

#endif
