/* SPDX-License-Identifier: MIT */

#include "common.h"

#include "allocation.h"
#include "block_device.h"
#include "boot_region.h"
#include "directory.h"
#include "endian.h"
#include "resize_internal.h"
#include "shrink.h"
#include "volume.h"

#include <stdlib.h>
#include <string.h>

/* These vectors grow only during preflight. There is no per-cluster owner table. */
struct shrink_array {
	void *items;
	size_t count;
	size_t capacity;
	size_t item_size;
};

struct shrink_stream {
	struct directory_reference owner;
	struct allocation_stream allocation;
};

struct shrink_directory {
	uint32_t original_cluster;
	size_t stream;
};

struct shrink_move {
	uint32_t source;
	uint32_t previous;
	size_t stream;
};

struct shrink_context {
	struct resize_volume *volume;
	struct exfat_resize_geometry target;
	struct resize_allocation allocation;
	struct resize_directory directory;
	struct allocation_observer observer;
	struct shrink_array streams;
	struct shrink_array directories;
	struct shrink_array moves;
	struct shrink_stream observed;
	size_t observed_index;
	size_t bitmap_index;
	uint32_t next_destination;
	uint32_t discarded_bad_clusters;
};

static enum exfat_resize_error append(
    struct shrink_context *context, struct shrink_array *array, const void *value, size_t *index)
{
	struct exfat_resize_allocator *allocator = &context->volume->operation->allocator;
	if (array->count == array->capacity) {
		size_t capacity = array->capacity == 0 ? 16 : array->capacity * 2;
		void *items;
		if (capacity < array->capacity || capacity > SIZE_MAX / array->item_size)
			return EXFAT_RESIZE_ARITHMETIC_OVERFLOW;
		items = allocator->allocate(allocator->context, capacity * array->item_size);
		if (items == NULL)
			return EXFAT_RESIZE_OUT_OF_MEMORY;
		if (array->items != NULL) {
			memcpy(items, array->items, array->count * array->item_size);
			allocator->deallocate(
			    allocator->context, array->items, array->capacity * array->item_size);
		}
		array->items = items;
		array->capacity = capacity;
	}
	if (index != NULL)
		*index = array->count;
	memcpy(
	    (unsigned char *)array->items + array->count * array->item_size, value, array->item_size);
	++array->count;
	return EXFAT_RESIZE_SUCCESS;
}

static struct shrink_stream *stream_at(struct shrink_context *context, size_t index)
{
	return &((struct shrink_stream *)context->streams.items)[index];
}

static uint32_t *model(struct shrink_context *context, uint32_t cluster)
{
	/* All callers use clusters checked during preflight or selected from the model. */
	return &context->allocation.allocation_model[cluster - 2];
}

static enum exfat_resize_error claim(void *opaque, uint32_t cluster)
{
	struct shrink_context *context = opaque;
	return exfat_resize_claim_model_cluster(&context->allocation, cluster);
}

static enum exfat_resize_error record(
    void *opaque, const struct allocation_stream *stream, const struct allocation_link *link)
{
	struct shrink_context *context = opaque;
	struct shrink_move move;
	enum exfat_resize_error error;
	if (!stream->no_fat_chain)
		*model(context, link->cluster) = link->next;
	++context->allocation.used_cluster_count;
	if (link->cluster < context->target.cluster_count + 2)
		return EXFAT_RESIZE_SUCCESS;
	if (context->observed_index == SIZE_MAX) {
		error = append(context, &context->streams, &context->observed, &context->observed_index);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
	}
	move = (struct shrink_move){ link->cluster, link->previous, context->observed_index };
	return append(context, &context->moves, &move, NULL);
}

static enum exfat_resize_error is_claimed(void *opaque, uint32_t cluster, int *claimed)
{
	struct shrink_context *context = opaque;
	*claimed = *model(context, cluster) != 0;
	return EXFAT_RESIZE_SUCCESS;
}

static enum exfat_resize_error bad_cluster(void *opaque, uint32_t cluster)
{
	struct shrink_context *context = opaque;
	*model(context, cluster) = EXFAT_FAT_BAD_CLUSTER;
	++context->allocation.used_cluster_count;
	if (cluster >= context->target.cluster_count + 2)
		++context->discarded_bad_clusters;
	return EXFAT_RESIZE_SUCCESS;
}

static enum exfat_resize_error observe(void *opaque,
    const struct directory_reference *owner,
    const struct allocation_stream *stream,
    int is_directory)
{
	struct shrink_context *context = opaque;
	struct stream_chain_reader chain = exfat_resize_source_chain(&context->allocation);
	enum exfat_resize_error error;
	context->observed = (struct shrink_stream){ *owner, *stream };
	context->observed_index = SIZE_MAX;
	if (is_directory || owner->kind == DIRECTORY_OWNER_BITMAP) {
		error = append(context, &context->streams, &context->observed, &context->observed_index);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
	}
	if (is_directory) {
		struct shrink_directory directory = { stream->first_cluster, context->observed_index };
		error = append(context, &context->directories, &directory, NULL);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
	}
	if (owner->kind == DIRECTORY_OWNER_BITMAP) {
		if (context->bitmap_index != SIZE_MAX)
			return EXFAT_RESIZE_INVALID_FILESYSTEM;
		context->bitmap_index = context->observed_index;
	}
	return exfat_resize_visit_allocation(
	    context->volume, &context->volume->geometry, &chain, stream, &context->observer);
}

static int compare_directories(const void *left, const void *right)
{
	const struct shrink_directory *a = left;
	const struct shrink_directory *b = right;
	return (a->original_cluster > b->original_cluster) -
	    (a->original_cluster < b->original_cluster);
}

static enum exfat_resize_error resolve(void *opaque, uint32_t id, struct allocation_stream *stream)
{
	struct shrink_context *context = opaque;
	const struct shrink_directory key = { .original_cluster = id };
	const struct shrink_directory *directory = bsearch(&key, context->directories.items,
	    context->directories.count, sizeof(key), compare_directories);
	if (directory == NULL)
		return EXFAT_RESIZE_INTERNAL_ERROR;
	*stream = stream_at(context, directory->stream)->allocation;
	return EXFAT_RESIZE_SUCCESS;
}

static enum exfat_resize_error prepare(struct shrink_context *context, uint64_t target_sectors)
{
	struct resize_volume *volume = context->volume;
	struct exfat_resize_device_geometry device = { volume->device->sector_size,
		volume->device->sector_count };
	struct allocation_stream root = { .first_cluster = volume->geometry.root_directory_cluster,
		.root_directory = 1 };
	struct directory_reference root_owner = { .kind = DIRECTORY_OWNER_ROOT };
	struct directory_scan scan = { .geometry = &volume->geometry,
		.chain = exfat_resize_source_chain(&context->allocation),
		.context = context,
		.visit = observe };
	enum exfat_resize_error error;
	uint32_t available = 0;
	uint32_t index;
	error = exfat_resize_plan_shrink(&device, &volume->geometry, target_sectors, &context->target);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = exfat_resize_load_source_fat(&context->allocation);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = exfat_resize_validate_reserved_fat_entries(&context->allocation);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = exfat_resize_allocate_volume_caches(volume);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = exfat_resize_create_allocation_model(&context->allocation);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = observe(context, &root_owner, &root, 1);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = exfat_resize_scan_directory_tree(&context->directory, &root, &scan);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	if (context->bitmap_index == SIZE_MAX)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	error = exfat_resize_reconcile_allocation(volume, &volume->geometry, &scan.chain,
	    &stream_at(context, context->bitmap_index)->allocation, &context->observer);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	for (index = 0; index < context->target.cluster_count; ++index) {
		error = exfat_resize_cluster_step_checkpoint(volume->operation);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		if (context->allocation.allocation_model[index] == 0)
			++available;
	}
	if (context->moves.count > available)
		return EXFAT_RESIZE_INSUFFICIENT_SHRINK_SPACE;
	qsort(context->directories.items, context->directories.count, sizeof(struct shrink_directory),
	    compare_directories);
	exfat_resize_release_source_fat(&context->allocation);
	return EXFAT_RESIZE_SUCCESS;
}

static enum exfat_resize_error synchronize(struct shrink_context *context)
{
	enum exfat_resize_error error =
	    exfat_resize_flush_cache(context->volume, SECTOR_CACHE_TARGET_DIRECTORY_DATA);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	return exfat_resize_block_device_sync(context->volume->device);
}

static enum exfat_resize_error write_fat(
    struct shrink_context *context, uint32_t cluster, uint32_t next)
{
	return exfat_resize_write_fat_entry(context->volume, &context->volume->geometry, cluster, next);
}

static enum exfat_resize_error bitmap_bit(struct shrink_context *context, uint32_t cluster, int set)
{
	struct stream_chain_reader chain = exfat_resize_model_chain(&context->allocation);
	return exfat_resize_set_bitmap_bit(context->volume, &context->volume->geometry, &chain,
	    &stream_at(context, context->bitmap_index)->allocation, cluster, set);
}

static enum exfat_resize_error publish_stream(struct shrink_context *context,
    struct shrink_stream *stream,
    const struct allocation_stream *replacement,
    const struct exfat_resize_geometry *geometry)
{
	struct directory_access access = { .geometry = geometry,
		.chain = exfat_resize_model_chain(&context->allocation),
		.context = context,
		.resolve = resolve };
	enum exfat_resize_error error;
	if (stream->owner.kind == DIRECTORY_OWNER_ROOT) {
		struct exfat_resize_geometry current = *geometry;
		current.root_directory_cluster = replacement->first_cluster;
		error = exfat_resize_write_boot_regions(context->volume->device, &current,
		    context->allocation.used_cluster_count, context->volume->io_buffer,
		    EXFAT_IO_BUFFER_SIZE);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		context->volume->geometry.root_directory_cluster = replacement->first_cluster;
	} else {
		error = exfat_resize_edit_directory_allocation(
		    &context->directory, &access, &stream->owner, &stream->allocation, replacement);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
	}
	stream->allocation = *replacement;
	return synchronize(context);
}

/* Generate bounded FAT batches, reading only partial-sector neighbors. Until
 * the flag is published these FAT entries are ignored. */
static enum exfat_resize_error materialize(
    struct shrink_context *context, struct shrink_stream *stream)
{
	struct resize_volume *volume = context->volume;
	struct allocation_stream replacement = stream->allocation;
	uint32_t count;
	uint32_t cluster = replacement.first_cluster;
	uint64_t end;
	enum exfat_resize_error error;
	if (!replacement.no_fat_chain)
		return EXFAT_RESIZE_SUCCESS;
	error = exfat_resize_stream_cluster_count(volume->cluster_size, &replacement, &count);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	end = (uint64_t)cluster + count;
	while (cluster < end) {
		uint64_t sector = volume->geometry.fat_offset + (uint64_t)cluster * 4 / volume->sector_size;
		size_t offset = (size_t)((uint64_t)cluster * 4 % volume->sector_size);
		uint64_t remaining = (end - cluster) * 4;
		uint64_t sectors = (offset + remaining + volume->sector_size - 1) / volume->sector_size;
		size_t byte_count;
		size_t limit;
		if (sectors > volume->io_sector_capacity)
			sectors = volume->io_sector_capacity;
		byte_count = (size_t)sectors * volume->sector_size;
		limit = remaining < byte_count - offset ? offset + (size_t)remaining : byte_count;
		error = exfat_resize_cancellation_checkpoint(volume->operation);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		if (offset != 0) {
			error = exfat_resize_read_volume(
			    volume, sector, 1, volume->io_buffer, EXFAT_IO_BUFFER_SIZE);
			if (error != EXFAT_RESIZE_SUCCESS)
				return error;
		}
		if (limit % volume->sector_size != 0 && (sectors > 1 || offset == 0)) {
			error = exfat_resize_read_volume(volume, sector + sectors - 1, 1,
			    volume->io_buffer + byte_count - volume->sector_size, volume->sector_size);
			if (error != EXFAT_RESIZE_SUCCESS)
				return error;
		}
		while (offset < limit) {
			uint32_t next = (uint64_t)cluster + 1 == end ? EXFAT_FAT_END_OF_CHAIN : cluster + 1;
			error = exfat_resize_store_le32(volume->io_buffer, byte_count, offset, next);
			if (error != EXFAT_RESIZE_SUCCESS)
				return error;
			*model(context, cluster++) = next;
			offset += 4;
		}
		error = exfat_resize_write_volume(
		    volume, sector, (uint32_t)sectors, volume->io_buffer, EXFAT_IO_BUFFER_SIZE);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
	}
	error = synchronize(context);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	volume->operation->cancellation_deferred = 1;
	replacement.no_fat_chain = 0;
	error = publish_stream(context, stream, &replacement, &volume->geometry);
	volume->operation->cancellation_deferred = 0;
	return error;
}

static enum exfat_resize_error find_destination(
    struct shrink_context *context, uint32_t *destination)
{
	enum exfat_resize_error error;
	while (context->next_destination < context->target.cluster_count + 2 &&
	    *model(context, context->next_destination) != 0) {
		error = exfat_resize_cluster_step_checkpoint(context->volume->operation);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		++context->next_destination;
	}
	if (context->next_destination == context->target.cluster_count + 2)
		return EXFAT_RESIZE_INTERNAL_ERROR;
	*destination = context->next_destination++;
	return EXFAT_RESIZE_SUCCESS;
}

static enum exfat_resize_error move_cluster(
    struct shrink_context *context, const struct shrink_move *move, uint32_t destination)
{
	struct resize_volume *volume = context->volume;
	struct shrink_stream *stream = stream_at(context, move->stream);
	struct allocation_stream replacement = stream->allocation;
	uint32_t next = *model(context, move->source);
	enum exfat_resize_error error;

	/* In particular, copy bitmap clusters AFTER reserving the destination in
	 * their current contents. All preceding directory edits were flushed. */
	error = write_fat(context, destination, next);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = bitmap_bit(context, destination, 1);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = exfat_resize_copy_cluster_run(volume, &volume->geometry, move->source, destination, 1);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = synchronize(context);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	*model(context, destination) = next;

	if (move->previous == 0) {
		replacement.first_cluster = destination;
		error = publish_stream(context, stream, &replacement, &volume->geometry);
	} else {
		error = write_fat(context, move->previous, destination);
		if (error == EXFAT_RESIZE_SUCCESS) {
			*model(context, move->previous) = destination;
			error = synchronize(context);
		}
	}
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	/* Resolve bitmap bits through its now-published chain, including when the
	 * just-moved cluster stores its own allocation bit. */
	error = write_fat(context, move->source, 0);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = bitmap_bit(context, move->source, 0);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	*model(context, move->source) = 0;
	return synchronize(context);
}

static enum exfat_resize_error cancel_cleanly(struct shrink_context *context)
{
	enum exfat_resize_error error;
	/* Materialization can stop while inactive FAT entries await synchronization. */
	error = synchronize(context);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = exfat_resize_set_volume_dirty(
	    context->volume->device, context->volume->io_buffer, EXFAT_IO_BUFFER_SIZE, 0);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	exfat_resize_enter_stage(context->volume->operation, EXFAT_RESIZE_STAGE_SOURCE_READY,
	    context->volume->geometry.volume_sector_count * context->volume->sector_size);
	return EXFAT_RESIZE_CANCELLED;
}

static enum exfat_resize_error clear_newly_reserved_bitmap_bits(
    struct shrink_context *context, uint32_t last_cluster)
{
	struct resize_volume *volume = context->volume;
	uint32_t first_bit = context->target.cluster_count % 8;
	uint32_t end_bit;
	uint64_t offset;
	uint64_t sector;
	unsigned char mask;
	enum exfat_resize_error error;
	if (first_bit == 0 || context->target.cluster_count == volume->geometry.cluster_count)
		return EXFAT_RESIZE_SUCCESS;
	/* Preserve padding that was already reserved in the source bitmap. */
	end_bit = volume->geometry.cluster_count - (context->target.cluster_count - first_bit);
	if (end_bit > 8)
		end_bit = 8;
	mask = (unsigned char)(((1u << end_bit) - 1) & ~((1u << first_bit) - 1));
	/* finish() resolved this cluster through the current bitmap chain. */
	offset = (context->target.cluster_count / 8) % volume->cluster_size;
	error = exfat_resize_cluster_sector(&volume->geometry, last_cluster, &sector);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	sector += offset / volume->sector_size;
	error = exfat_resize_read_volume(volume, sector, 1, volume->io_buffer, EXFAT_IO_BUFFER_SIZE);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	volume->io_buffer[offset % volume->sector_size] &= (unsigned char)~mask;
	return exfat_resize_write_volume(volume, sector, 1, volume->io_buffer, EXFAT_IO_BUFFER_SIZE);
}

static enum exfat_resize_error finish(struct shrink_context *context)
{
	struct resize_volume *volume = context->volume;
	struct shrink_stream *bitmap = stream_at(context, context->bitmap_index);
	struct allocation_stream replacement = bitmap->allocation;
	uint32_t kept_clusters;
	uint32_t last = replacement.first_cluster;
	uint32_t surplus;
	uint32_t index;
	enum exfat_resize_error error;
	replacement.data_length = ((uint64_t)context->target.cluster_count + 7) / 8;
	error = exfat_resize_stream_cluster_count(volume->cluster_size, &replacement, &kept_clusters);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	for (index = 1; index < kept_clusters; ++index)
		last = *model(context, last);
	surplus = *model(context, last);
	/* This starts the non-cancellable geometry transition. The smaller bitmap
	 * is validated against the target, while all I/O still uses current coordinates. */
	error = publish_stream(context, bitmap, &replacement, &context->target);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = write_fat(context, last, EXFAT_FAT_END_OF_CHAIN);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	*model(context, last) = EXFAT_FAT_END_OF_CHAIN;
	while (surplus != EXFAT_FAT_END_OF_CHAIN) {
		uint32_t next = *model(context, surplus);
		error = write_fat(context, surplus, 0);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		error = bitmap_bit(context, surplus, 0);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		*model(context, surplus) = 0;
		--context->allocation.used_cluster_count;
		surplus = next;
	}
	context->allocation.used_cluster_count -= context->discarded_bad_clusters;
	error = clear_newly_reserved_bitmap_bits(context, last);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = synchronize(context);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	context->target.root_directory_cluster = volume->geometry.root_directory_cluster;
	error = exfat_resize_write_boot_regions(volume->device, &context->target,
	    context->allocation.used_cluster_count, volume->io_buffer, EXFAT_IO_BUFFER_SIZE);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	exfat_resize_enter_stage(volume->operation, EXFAT_RESIZE_STAGE_FINALIZING, 0);
	error =
	    exfat_resize_set_volume_dirty(volume->device, volume->io_buffer, EXFAT_IO_BUFFER_SIZE, 0);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	exfat_resize_enter_stage(volume->operation, EXFAT_RESIZE_STAGE_COMPLETED,
	    context->target.volume_sector_count * volume->sector_size);
	return EXFAT_RESIZE_SUCCESS;
}

static enum exfat_resize_error run(struct shrink_context *context)
{
	struct resize_operation *operation = context->volume->operation;
	enum exfat_resize_error error;
	size_t index;
	error = exfat_resize_cancellation_checkpoint(operation);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	exfat_resize_enter_stage(operation, EXFAT_RESIZE_STAGE_PREPARING, 0);
	/* A monitor may request cancellation from the stage event, before any write. */
	error = exfat_resize_cancellation_checkpoint(operation);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = exfat_resize_set_volume_dirty(
	    context->volume->device, context->volume->io_buffer, EXFAT_IO_BUFFER_SIZE, 1);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	exfat_resize_enter_stage(operation, EXFAT_RESIZE_STAGE_RESIZING, 0);
	for (index = context->moves.count; index > 0; --index) {
		const struct shrink_move *move = &((struct shrink_move *)context->moves.items)[index - 1];
		uint32_t destination;
		error = exfat_resize_cancellation_checkpoint(operation);
		if (error == EXFAT_RESIZE_SUCCESS)
			error = materialize(context, stream_at(context, move->stream));
		if (error == EXFAT_RESIZE_SUCCESS)
			error = find_destination(context, &destination);
		if (error == EXFAT_RESIZE_CANCELLED)
			return cancel_cleanly(context);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		operation->cancellation_deferred = 1;
		error = move_cluster(context, move, destination);
		operation->cancellation_deferred = 0;
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
	}
	error = exfat_resize_cancellation_checkpoint(operation);
	if (error == EXFAT_RESIZE_CANCELLED)
		return cancel_cleanly(context);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	operation->cancellation_deferred = 1;
	error = finish(context);
	operation->cancellation_deferred = 0;
	return error;
}

enum exfat_resize_error exfat_resize_shrink(
    struct resize_volume *volume, uint64_t target_sector_count)
{
	struct shrink_context context = { .volume = volume,
		.bitmap_index = SIZE_MAX,
		.next_destination = 2,
		.streams = { .item_size = sizeof(struct shrink_stream) },
		.directories = { .item_size = sizeof(struct shrink_directory) },
		.moves = { .item_size = sizeof(struct shrink_move) } };
	struct shrink_array *arrays[] = { &context.streams, &context.directories, &context.moves };
	enum exfat_resize_error error;
	size_t index;
	context.allocation.volume = volume;
	context.allocation.model_geometry = &volume->geometry;
	context.directory.volume = volume;
	context.observer = (struct allocation_observer){ .context = &context,
		.claim = claim,
		.record = record,
		.is_claimed = is_claimed,
		.bad_cluster = bad_cluster };
	error = prepare(&context, target_sector_count);
	if (error == EXFAT_RESIZE_SUCCESS)
		error = run(&context);
	for (index = 0; index < sizeof(arrays) / sizeof(arrays[0]); ++index) {
		if (arrays[index]->items != NULL)
			volume->operation->allocator.deallocate(volume->operation->allocator.context,
			    arrays[index]->items, arrays[index]->capacity * arrays[index]->item_size);
	}
	exfat_resize_release_source_fat(&context.allocation);
	exfat_resize_release_directory(&context.directory);
	exfat_resize_release_allocation_model(&context.allocation);
	return error;
}
