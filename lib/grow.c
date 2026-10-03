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
	struct allocation_observer observer;
	int keep_no_fat_chain;
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

/* Growth uses its bijective mapping to share target-model storage with the
 * ownership set. Source traversal and validation know only source coordinates. */
static enum exfat_resize_error mapped_model_entry(
    struct grow_context *context, uint32_t source, uint32_t **entry)
{
	uint32_t target;
	enum exfat_resize_error error = map_cluster(&context->plan, source, &target);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	return exfat_resize_model_entry(&context->allocation, target, entry);
}

static enum exfat_resize_error claim_growth_cluster(void *opaque, uint32_t source)
{
	struct grow_context *context = opaque;
	uint32_t target;
	enum exfat_resize_error error = map_cluster(&context->plan, source, &target);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	return exfat_resize_claim_model_cluster(&context->allocation, target);
}

static enum exfat_resize_error record_growth_link(
    void *opaque, const struct allocation_stream *stream, const struct allocation_link *link)
{
	struct grow_context *context = opaque;
	uint32_t *entry;
	enum exfat_resize_error error;
	(void)stream;
	if (context->keep_no_fat_chain)
		return EXFAT_RESIZE_SUCCESS;
	error = mapped_model_entry(context, link->cluster, &entry);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	if (link->next == EXFAT_FAT_END_OF_CHAIN) {
		*entry = EXFAT_FAT_END_OF_CHAIN;
		return EXFAT_RESIZE_SUCCESS;
	}
	return map_cluster(&context->plan, link->next, entry);
}

static enum exfat_resize_error growth_cluster_is_claimed(
    void *opaque, uint32_t cluster, int *claimed)
{
	struct grow_context *context = opaque;
	uint32_t *entry;
	enum exfat_resize_error error = mapped_model_entry(context, cluster, &entry);
	if (error == EXFAT_RESIZE_SUCCESS)
		*claimed = *entry != 0;
	return error;
}

static enum exfat_resize_error record_growth_bad_cluster(void *opaque, uint32_t cluster)
{
	struct grow_context *context = opaque;
	uint32_t *entry;
	enum exfat_resize_error error = validate_bad_cluster(&context->plan, cluster);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = mapped_model_entry(context, cluster, &entry);
	if (error == EXFAT_RESIZE_SUCCESS)
		*entry = EXFAT_FAT_BAD_CLUSTER;
	return error;
}

static enum exfat_resize_error claim_growth_stream(
    struct grow_context *context, const struct allocation_stream *stream)
{
	struct stream_chain_reader chain = exfat_resize_source_chain(&context->allocation);
	uint32_t count;
	enum exfat_resize_error error;
	context->keep_no_fat_chain = 0;
	if (stream->no_fat_chain) {
		error = exfat_resize_stream_cluster_count(context->volume->cluster_size, stream, &count);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		context->keep_no_fat_chain =
		    !mapping_breaks_contiguity(&context->plan, stream->first_cluster, count);
	}
	return exfat_resize_visit_allocation(
	    context->volume, context->plan.source, &chain, stream, &context->observer);
}

static enum exfat_resize_error observe_growth_allocation(void *opaque,
    const struct directory_reference *owner,
    const struct allocation_stream *stream,
    int is_directory)
{
	struct grow_context *context = opaque;
	(void)is_directory;
	if (owner->kind == DIRECTORY_OWNER_BITMAP) {
		if (context->allocation.old_bitmap.first_cluster != 0)
			return EXFAT_RESIZE_INVALID_FILESYSTEM;
		context->allocation.old_bitmap = *stream;
	}
	return claim_growth_stream(context, stream);
}

static enum exfat_resize_error transform_growth_allocation(void *opaque,
    const struct directory_reference *owner,
    const struct allocation_stream *stream,
    int is_directory,
    struct allocation_stream *replacement)
{
	struct grow_context *context = opaque;
	enum exfat_resize_error error;
	uint32_t count;
	(void)is_directory;
	*replacement = *stream;
	if (owner->kind == DIRECTORY_OWNER_BITMAP) {
		*replacement = context->allocation.new_bitmap;
		return EXFAT_RESIZE_SUCCESS;
	}
	if (stream->first_cluster < 2 || stream->data_length == 0)
		return EXFAT_RESIZE_SUCCESS;
	error = map_cluster(&context->plan, stream->first_cluster, &replacement->first_cluster);
	if (error != EXFAT_RESIZE_SUCCESS || !stream->no_fat_chain)
		return error;
	error = exfat_resize_stream_cluster_count(context->volume->cluster_size, stream, &count);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	if (mapping_breaks_contiguity(&context->plan, stream->first_cluster, count))
		replacement->no_fat_chain = 0;
	return EXFAT_RESIZE_SUCCESS;
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
		error = mapped_model_entry(context, source_index + 2, &model_entry);
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
			error = mapped_model_entry(context, source_index + 2, &model_entry);
			if (error != EXFAT_RESIZE_SUCCESS)
				return error;
		} while (*model_entry != 0 && *model_entry != EXFAT_FAT_BAD_CLUSTER);

		source_cluster = run_start + 2;
		error = map_cluster(&context->plan, source_cluster, &target_cluster);
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
	struct allocation_stream mapped_bitmap;
	struct directory_scan scan = { .geometry = context->plan.source,
		.chain = exfat_resize_source_chain(&context->allocation),
		.context = context,
		.visit = observe_growth_allocation };
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
	error = claim_growth_stream(context, &root);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = exfat_resize_scan_directory_tree(&context->directory, &root, &scan);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	if (context->allocation.old_bitmap.first_cluster == 0)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	error = exfat_resize_reconcile_allocation(context->volume, context->plan.source, &scan.chain,
	    &context->allocation.old_bitmap, &context->observer);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	mapped_bitmap = context->allocation.old_bitmap;
	error = map_cluster(&context->plan, mapped_bitmap.first_cluster, &mapped_bitmap.first_cluster);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = exfat_resize_remove_allocation_from_model(&context->allocation, &mapped_bitmap);
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
	return exfat_resize_rewrite_bitmap_entry(
	    &context->directory, &location, &context->allocation.new_bitmap);
}

static enum exfat_resize_error run_transaction(struct grow_context *context)
{
	struct allocation_stream target_root;
	struct directory_scan scan = { .geometry = &context->plan.target,
		.chain = exfat_resize_model_chain(&context->allocation),
		.context = context,
		.transform = transform_growth_allocation };
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
		error = exfat_resize_scan_directory_tree(&context->directory, &target_root, &scan);
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
	context.allocation.model_geometry = &context.plan.target;
	context.directory.volume = volume;
	context.observer = (struct allocation_observer){ .context = &context,
		.claim = claim_growth_cluster,
		.record = record_growth_link,
		.is_claimed = growth_cluster_is_claimed,
		.bad_cluster = record_growth_bad_cluster };

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
