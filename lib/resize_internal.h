/* SPDX-License-Identifier: MIT */

#ifndef EXFAT_RESIZE_INTERNAL_H
#define EXFAT_RESIZE_INTERNAL_H

#include "directory.h"
#include "geometry.h"
#include "sector_adapter.h"
#include "stream.h"
#include "volume.h"

/*
 * Shared operation state retained during the mechanical module extraction.
 * Grow policy and transaction ordering still belong to resize.c; later steps
 * will separate that policy and narrow the state each module receives.
 */
#define EXFAT_CLUSTER_CHECKPOINT_INTERVAL UINT32_C(1048576)

struct resize_context {
	const struct exfat_resize_block_device *device;
	struct exfat_resize_sector_adapter sector_adapter;
	struct exfat_resize_allocator allocator;
	struct exfat_resize_monitor monitor;
	enum exfat_resize_stage stage;
	struct exfat_resize_geometry source;
	struct exfat_resize_geometry target;
	struct allocation_stream old_bitmap;
	struct allocation_stream new_bitmap;
	struct directory_location bitmap_location;
	struct directory_worklist directories;
	struct sector_cache caches[SECTOR_CACHE_COUNT];
	unsigned char *cache_buffer;
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
	/* Shared by cluster loops; reset at every cancellation checkpoint. */
	uint32_t cluster_steps_since_checkpoint;
};

enum exfat_resize_error exfat_resize_cancellation_checkpoint(struct resize_context *context);

enum exfat_resize_error exfat_resize_cluster_step_checkpoint(struct resize_context *context);

int exfat_resize_mapping_changes_cluster_numbers(const struct resize_context *context);

int exfat_resize_stream_crosses_mapping_boundary(
    const struct resize_context *context, uint32_t first_cluster, uint32_t cluster_count);

enum exfat_resize_error exfat_resize_map_cluster(
    const struct resize_context *context, uint32_t source_cluster, uint32_t *target_cluster);

#endif
