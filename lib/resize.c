/* SPDX-License-Identifier: MIT */

#include "common.h"

#include "allocation.h"
#include "block_device.h"
#include "boot_region.h"
#include "checked_math.h"
#include "directory.h"
#include "event.h"
#include "exfat_resize.h"
#include "geometry.h"
#include "resize_internal.h"
#include "sector_adapter.h"
#include "stream.h"
#include "volume.h"

static void enter_stage(
    struct resize_context *context, enum exfat_resize_stage stage, uint64_t value)
{
	context->stage = stage;
	exfat_resize_report_event(&context->monitor, EXFAT_RESIZE_EVENT_LEVEL_INFO,
	    EXFAT_RESIZE_EVENT_CODE_STAGE_ENTERED, (uint64_t)stage, value, 0);
}

enum exfat_resize_error exfat_resize_cancellation_checkpoint(struct resize_context *context)
{
	context->cluster_steps_since_checkpoint = 0;
	if (context->monitor.cancellation_requested != NULL &&
	    context->monitor.cancellation_requested(context->monitor.context) != 0)
		return EXFAT_RESIZE_CANCELLED;
	return EXFAT_RESIZE_SUCCESS;
}

enum exfat_resize_error exfat_resize_cluster_step_checkpoint(struct resize_context *context)
{
	if (++context->cluster_steps_since_checkpoint < EXFAT_CLUSTER_CHECKPOINT_INTERVAL)
		return EXFAT_RESIZE_SUCCESS;
	return exfat_resize_cancellation_checkpoint(context);
}

int exfat_resize_mapping_changes_cluster_numbers(const struct resize_context *context)
{
	/* See the cluster-permutation invariant for exfat_resize_map_growth_cluster(). */
	return context->displaced_cluster_count != 0 &&
	    context->displaced_cluster_count != context->source.cluster_count;
}

int exfat_resize_stream_crosses_mapping_boundary(
    const struct resize_context *context, uint32_t first_cluster, uint32_t cluster_count)
{
	uint64_t boundary = (uint64_t)context->displaced_cluster_count + 2;
	uint64_t end = (uint64_t)first_cluster + cluster_count;

	return exfat_resize_mapping_changes_cluster_numbers(context) && first_cluster < boundary &&
	    end > boundary;
}

enum exfat_resize_error exfat_resize_map_cluster(
    const struct resize_context *context, uint32_t source_cluster, uint32_t *target_cluster)
{
	return exfat_resize_map_growth_cluster(
	    &context->source, &context->target, source_cluster, target_cluster);
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

		error = exfat_resize_cluster_step_checkpoint(context);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		error =
		    exfat_resize_model_entry_for_source_cluster(context, source_index + 2, &model_entry);
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
			if (source_index == context->displaced_cluster_count)
				break;
			error = exfat_resize_cluster_step_checkpoint(context);
			if (error != EXFAT_RESIZE_SUCCESS)
				return error;
			error = exfat_resize_model_entry_for_source_cluster(
			    context, source_index + 2, &model_entry);
			if (error != EXFAT_RESIZE_SUCCESS)
				return error;
		} while (*model_entry != 0 && *model_entry != EXFAT_FAT_BAD_CLUSTER);

		source_cluster = run_start + 2;
		error = exfat_resize_map_cluster(context, source_cluster, &target_cluster);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		error = exfat_resize_copy_cluster_run(
		    context, source_cluster, target_cluster, source_index - run_start);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
	}
	return EXFAT_RESIZE_SUCCESS;
}

/*
 * Populate filesystem-derived state after exfat_resize() has initialized the
 * operation-wide allocator, monitor, and recovery stage.
 */
static enum exfat_resize_error prepare_context(struct resize_context *context,
    const struct exfat_resize_block_device *device,
    uint64_t target_size)
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
	device_geometry.logical_sector_size = context->device->sector_size;
	device_geometry.sector_count = context->device->sector_count;
	error = exfat_resize_plan_growth(
	    &device_geometry, &context->source, target_sector_count, &context->target);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;

	error = exfat_resize_load_source_fat(context);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = exfat_resize_validate_reserved_fat_entries(context);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	context->cluster_size = (uint64_t)context->source.sectors_per_cluster * context->sector_size;

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
	if (exfat_resize_mapping_changes_cluster_numbers(context) &&
	    (uint64_t)context->source.cluster_count + 2 + context->displaced_cluster_count >
	        (uint64_t)context->target.cluster_count + 2)
		return EXFAT_RESIZE_INTERNAL_ERROR;

	context->cache_buffer =
	    context->allocator.allocate(context->allocator.context, EXFAT_SECTOR_CACHE_BUFFER_SIZE);
	if (context->cache_buffer == NULL)
		return EXFAT_RESIZE_OUT_OF_MEMORY;
	for (cache_index = 0; cache_index < SECTOR_CACHE_COUNT; ++cache_index) {
		context->caches[cache_index].data =
		    context->cache_buffer + cache_index * EXFAT_SECTOR_CACHE_SIZE;
		context->caches[cache_index].sector_capacity =
		    (uint32_t)(EXFAT_SECTOR_CACHE_SIZE / context->sector_size);
	}

	model_size = (uint64_t)context->target.cluster_count * sizeof(*context->allocation_model);
	if (model_size > SIZE_MAX)
		return EXFAT_RESIZE_ARITHMETIC_OVERFLOW;
	context->allocation_model_size = (size_t)model_size;
	context->allocation_model =
	    context->allocator.allocate(context->allocator.context, context->allocation_model_size);
	if (context->allocation_model == NULL)
		return EXFAT_RESIZE_OUT_OF_MEMORY;
	error = exfat_resize_zero_allocation_model(context);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;

	root.first_cluster = context->source.root_directory_cluster;
	root.data_length = 0;
	root.no_fat_chain = 0;
	root.root_directory = 1;
	error = exfat_resize_claim_root_directory(context, root.first_cluster);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = exfat_resize_scan_directory_tree(context, &root, DIRECTORY_SCAN_VALIDATE);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	if (context->old_bitmap.first_cluster == 0)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	error = exfat_resize_validate_allocation_model(context);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = exfat_resize_remove_old_bitmap_from_model(context);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = exfat_resize_add_new_bitmap_to_model(context);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	exfat_resize_release_source_fat(context);
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
	enter_stage(context, EXFAT_RESIZE_STAGE_PREPARING, 0);
	error = exfat_resize_cancellation_checkpoint(context);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error =
	    exfat_resize_set_volume_dirty(context->device, context->io_buffer, EXFAT_IO_BUFFER_SIZE, 1);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;

	error = move_displaced_clusters(context);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = exfat_resize_write_target_bitmap(context);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	/* Preserve the relocated data before the enlarged FAT overwrites its source. */
	error = exfat_resize_block_device_sync(context->device);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;

	error = exfat_resize_cancellation_checkpoint(context);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	enter_stage(context, EXFAT_RESIZE_STAGE_RESIZING, 0);
	error = exfat_resize_cancellation_checkpoint(context);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = exfat_resize_write_target_fat(context);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;

	if (!exfat_resize_mapping_changes_cluster_numbers(context)) {
		error = exfat_resize_rewrite_identity_bitmap_entry(context);
	} else {
		target_root.first_cluster = context->target.root_directory_cluster;
		target_root.data_length = 0;
		target_root.no_fat_chain = 0;
		target_root.root_directory = 1;
		error = exfat_resize_scan_directory_tree(context, &target_root, DIRECTORY_SCAN_REWRITE);
	}
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = exfat_resize_flush_cache(context, SECTOR_CACHE_TARGET_DIRECTORY_DATA);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = exfat_resize_block_device_sync(context->device);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = exfat_resize_cancellation_checkpoint(context);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;

	error = exfat_resize_write_boot_regions(context->device, &context->target,
	    context->used_cluster_count, context->io_buffer, EXFAT_IO_BUFFER_SIZE);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;

	enter_stage(context, EXFAT_RESIZE_STAGE_FINALIZING, 0);
	error = exfat_resize_cancellation_checkpoint(context);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error =
	    exfat_resize_set_volume_dirty(context->device, context->io_buffer, EXFAT_IO_BUFFER_SIZE, 0);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;

	enter_stage(context, EXFAT_RESIZE_STAGE_COMPLETED,
	    context->target.volume_sector_count * (uint64_t)context->sector_size);
	return EXFAT_RESIZE_SUCCESS;
}

static void release_context(struct resize_context *context)
{
	exfat_resize_release_source_fat(context);
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
		    context->allocator.context, context->cache_buffer, EXFAT_SECTOR_CACHE_BUFFER_SIZE);
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
    const struct exfat_resize_allocator *allocator,
    const struct exfat_resize_monitor *monitor,
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
	if (allocator == NULL || allocator->allocate == NULL || allocator->deallocate == NULL) {
		error = EXFAT_RESIZE_INVALID_ARGUMENT;
		goto out;
	}
	context.allocator = *allocator;
	if (monitor != NULL)
		context.monitor = *monitor;
	/*
	 * The context starts at PREFLIGHT for early-error recovery reporting.
	 * Delay the corresponding event until structural arguments are valid so
	 * rejected calls do not invoke monitor callbacks.
	 */
	enter_stage(&context, EXFAT_RESIZE_STAGE_PREFLIGHT, 0);
	error = exfat_resize_cancellation_checkpoint(&context);
	if (error != EXFAT_RESIZE_SUCCESS)
		goto out;

	error = prepare_context(&context, device, target_size);
	if (error != EXFAT_RESIZE_SUCCESS)
		goto out;
	error = exfat_resize_cancellation_checkpoint(&context);
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
