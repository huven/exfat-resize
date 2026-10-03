/* SPDX-License-Identifier: MIT */

#include "common.h"

#include "allocation.h"
#include "block_device.h"
#include "boot_region.h"
#include "checked_math.h"
#include "directory.h"
#include "geometry.h"
#include "grow.h"
#include "resize_internal.h"
#include "volume.h"

struct growth_plan {
	const struct exfat_resize_geometry *source;
	struct exfat_resize_geometry target;
	uint32_t displaced_cluster_count;
};

struct grow_context {
	struct resize_volume *volume;
	struct growth_plan plan;
	struct resize_allocation allocation;
	struct resize_directory directory;
};

static int mapping_changes_cluster_numbers(const struct growth_plan *plan)
{
	/* See the cluster-permutation invariant for exfat_resize_map_growth_cluster(). */
	return plan->displaced_cluster_count != 0 &&
	    plan->displaced_cluster_count != plan->source->cluster_count;
}

static int mapping_breaks_contiguity(
    const void *context, uint32_t first_cluster, uint32_t cluster_count)
{
	const struct growth_plan *plan = context;
	uint64_t boundary = (uint64_t)plan->displaced_cluster_count + 2;
	uint64_t end = (uint64_t)first_cluster + cluster_count;

	return mapping_changes_cluster_numbers(plan) && first_cluster < boundary && end > boundary;
}

static enum exfat_resize_error map_cluster(
    const void *context, uint32_t source_cluster, uint32_t *target_cluster)
{
	const struct growth_plan *plan = context;
	return exfat_resize_map_growth_cluster(
	    plan->source, &plan->target, source_cluster, target_cluster);
}

static enum exfat_resize_error validate_bad_cluster(const void *context, uint32_t cluster)
{
	const struct growth_plan *plan = context;
	/*
	 * Displaced physical sectors become part of the enlarged FAT. A bad marker
	 * describes those sectors and cannot move to another physical location.
	 */
	return cluster - 2 < plan->displaced_cluster_count ? EXFAT_RESIZE_BAD_CLUSTER_CONFLICT
	                                                   : EXFAT_RESIZE_SUCCESS;
}

static enum exfat_resize_error move_displaced_clusters(struct grow_context *context)
{
	enum exfat_resize_error error;
	uint32_t source_index = 0;

	while (source_index < context->plan.displaced_cluster_count) {
		uint32_t run_start;
		uint32_t source_cluster;
		uint32_t target_cluster;
		uint32_t *model_entry;

		error = exfat_resize_cluster_step_checkpoint(context->volume->operation);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		error = exfat_resize_model_entry_for_source_cluster(
		    &context->allocation, source_index + 2, &model_entry);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		if (*model_entry == 0 || *model_entry == EXFAT_FAT_BAD_CLUSTER) {
			++source_index;
			continue;
		}

		/*
		 * Within the displaced prefix, adjacent source clusters map to
		 * adjacent target clusters under the mapping invariant documented by
		 * exfat_resize_map_growth_cluster(). Free and bad clusters end a copy
		 * run.
		 */
		run_start = source_index;
		do {
			++source_index;
			if (source_index == context->plan.displaced_cluster_count)
				break;
			error = exfat_resize_cluster_step_checkpoint(context->volume->operation);
			if (error != EXFAT_RESIZE_SUCCESS)
				return error;
			error = exfat_resize_model_entry_for_source_cluster(
			    &context->allocation, source_index + 2, &model_entry);
			if (error != EXFAT_RESIZE_SUCCESS)
				return error;
		} while (*model_entry != 0 && *model_entry != EXFAT_FAT_BAD_CLUSTER);

		source_cluster = run_start + 2;
		error = exfat_resize_map_cluster(&context->allocation, source_cluster, &target_cluster);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		error = exfat_resize_copy_cluster_run(context->volume, &context->plan.target,
		    source_cluster, target_cluster, source_index - run_start);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
	}
	return EXFAT_RESIZE_SUCCESS;
}

static enum exfat_resize_error prepare_growth(
    struct grow_context *context, uint64_t target_sector_count)
{
	struct allocation_stream root;
	struct exfat_resize_device_geometry device_geometry;
	enum exfat_resize_error error;
	uint64_t bitmap_clusters;
	uint64_t heap_movement;

	device_geometry.logical_sector_size = context->volume->device->sector_size;
	device_geometry.sector_count = context->volume->device->sector_count;
	error = exfat_resize_plan_growth(
	    &device_geometry, &context->volume->geometry, target_sector_count, &context->plan.target);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;

	error = exfat_resize_load_source_fat(&context->allocation);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = exfat_resize_validate_reserved_fat_entries(&context->allocation);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;

	heap_movement = (uint64_t)context->plan.target.cluster_heap_offset -
	    context->volume->geometry.cluster_heap_offset;
	if (heap_movement % context->volume->geometry.sectors_per_cluster != 0)
		return EXFAT_RESIZE_INTERNAL_ERROR;
	heap_movement /= context->volume->geometry.sectors_per_cluster;
	context->plan.displaced_cluster_count = heap_movement > context->volume->geometry.cluster_count
	    ? context->volume->geometry.cluster_count
	    : (uint32_t)heap_movement;

	context->allocation.new_bitmap.data_length =
	    ((uint64_t)context->plan.target.cluster_count + 7) / 8;
	error = exfat_resize_checked_ceil_divide_u64(context->allocation.new_bitmap.data_length,
	    context->volume->cluster_size, &bitmap_clusters);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	if (bitmap_clusters == 0 || bitmap_clusters > context->plan.target.cluster_count)
		return EXFAT_RESIZE_INTERNAL_ERROR;
	context->allocation.new_bitmap.first_cluster =
	    context->plan.target.cluster_count + 2 - (uint32_t)bitmap_clusters;
	if (context->allocation.new_bitmap.first_cluster <= context->volume->geometry.cluster_count + 1)
		return EXFAT_RESIZE_INSUFFICIENT_GROWTH;
	if (mapping_changes_cluster_numbers(&context->plan) &&
	    (uint64_t)context->volume->geometry.cluster_count + 2 +
	            context->plan.displaced_cluster_count >
	        (uint64_t)context->plan.target.cluster_count + 2)
		return EXFAT_RESIZE_INTERNAL_ERROR;

	error = exfat_resize_allocate_volume_caches(context->volume);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = exfat_resize_create_allocation_model(&context->allocation);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;

	root.first_cluster = context->volume->geometry.root_directory_cluster;
	root.data_length = 0;
	root.no_fat_chain = 0;
	root.root_directory = 1;
	error = exfat_resize_claim_root_directory(&context->allocation, root.first_cluster);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = exfat_resize_scan_directory_tree(&context->directory, &root, DIRECTORY_SCAN_VALIDATE);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	if (context->allocation.old_bitmap.first_cluster == 0)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	error = exfat_resize_validate_allocation_model(&context->allocation);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = exfat_resize_remove_old_bitmap_from_model(&context->allocation);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = exfat_resize_add_new_bitmap_to_model(&context->allocation);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	exfat_resize_release_source_fat(&context->allocation);
	return EXFAT_RESIZE_SUCCESS;
}

static enum exfat_resize_error rewrite_identity_bitmap_entry(struct grow_context *context)
{
	const struct growth_plan *plan = &context->plan;
	struct directory_location location = context->directory.bitmap_location;
	uint64_t heap_relative_sector;

	if (mapping_changes_cluster_numbers(plan) ||
	    location.sector < plan->source->cluster_heap_offset)
		return EXFAT_RESIZE_INTERNAL_ERROR;
	heap_relative_sector = location.sector - plan->source->cluster_heap_offset;
	if (heap_relative_sector >= plan->target.volume_sector_count - plan->target.cluster_heap_offset)
		return EXFAT_RESIZE_INTERNAL_ERROR;
	location.sector = plan->target.cluster_heap_offset + heap_relative_sector;
	return exfat_resize_rewrite_bitmap_entry(&context->directory, &location);
}

static enum exfat_resize_error run_transaction(struct grow_context *context)
{
	struct allocation_stream target_root;
	enum exfat_resize_error error;

	/*
	 * Direct transaction I/O may make the source data caches stale, but they
	 * are reserved for preflight and are never consulted below. The target
	 * directory cache has not yet been populated.
	 */
	exfat_resize_enter_stage(context->volume->operation, EXFAT_RESIZE_STAGE_PREPARING, 0);
	error = exfat_resize_cancellation_checkpoint(context->volume->operation);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = exfat_resize_set_volume_dirty(
	    context->volume->device, context->volume->io_buffer, EXFAT_IO_BUFFER_SIZE, 1);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;

	error = move_displaced_clusters(context);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = exfat_resize_write_target_bitmap(&context->allocation);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	/* Preserve the relocated data before the enlarged FAT overwrites its source. */
	error = exfat_resize_block_device_sync(context->volume->device);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;

	error = exfat_resize_cancellation_checkpoint(context->volume->operation);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	exfat_resize_enter_stage(context->volume->operation, EXFAT_RESIZE_STAGE_RESIZING, 0);
	error = exfat_resize_cancellation_checkpoint(context->volume->operation);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = exfat_resize_write_target_fat(&context->allocation);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;

	if (!mapping_changes_cluster_numbers(&context->plan)) {
		error = rewrite_identity_bitmap_entry(context);
	} else {
		target_root.first_cluster = context->plan.target.root_directory_cluster;
		target_root.data_length = 0;
		target_root.no_fat_chain = 0;
		target_root.root_directory = 1;
		error = exfat_resize_scan_directory_tree(
		    &context->directory, &target_root, DIRECTORY_SCAN_REWRITE);
	}
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = exfat_resize_flush_cache(context->volume, SECTOR_CACHE_TARGET_DIRECTORY_DATA);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = exfat_resize_block_device_sync(context->volume->device);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = exfat_resize_cancellation_checkpoint(context->volume->operation);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;

	error = exfat_resize_write_boot_regions(context->volume->device, &context->plan.target,
	    context->allocation.used_cluster_count, context->volume->io_buffer, EXFAT_IO_BUFFER_SIZE);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;

	exfat_resize_enter_stage(context->volume->operation, EXFAT_RESIZE_STAGE_FINALIZING, 0);
	error = exfat_resize_cancellation_checkpoint(context->volume->operation);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = exfat_resize_set_volume_dirty(
	    context->volume->device, context->volume->io_buffer, EXFAT_IO_BUFFER_SIZE, 0);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;

	exfat_resize_enter_stage(context->volume->operation, EXFAT_RESIZE_STAGE_COMPLETED,
	    context->plan.target.volume_sector_count * (uint64_t)context->volume->sector_size);
	return EXFAT_RESIZE_SUCCESS;
}

enum exfat_resize_error exfat_resize_grow(
    struct resize_volume *volume, uint64_t target_sector_count)
{
	struct grow_context context = { .volume = volume };
	enum exfat_resize_error error;

	context.plan.source = &volume->geometry;
	context.allocation.volume = volume;
	context.allocation.target = &context.plan.target;
	context.allocation.mapping.context = &context.plan;
	context.allocation.mapping.map_cluster = map_cluster;
	context.allocation.mapping.breaks_contiguity = mapping_breaks_contiguity;
	context.allocation.mapping.validate_bad_cluster = validate_bad_cluster;
	context.directory.volume = volume;
	context.directory.allocation = &context.allocation;

	error = prepare_growth(&context, target_sector_count);
	if (error != EXFAT_RESIZE_SUCCESS)
		goto out;
	error = exfat_resize_cancellation_checkpoint(volume->operation);
	if (error != EXFAT_RESIZE_SUCCESS)
		goto out;
	error = run_transaction(&context);

out:
	exfat_resize_release_source_fat(&context.allocation);
	exfat_resize_release_directory(&context.directory);
	exfat_resize_release_allocation_model(&context.allocation);
	return error;
}
