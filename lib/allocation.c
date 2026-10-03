/* SPDX-License-Identifier: MIT */

#include "common.h"

#include "allocation.h"
#include "block_device.h"
#include "directory.h"
#include "endian.h"
#include "geometry.h"
#include "resize_internal.h"
#include "stream.h"
#include "volume.h"

#include <string.h>

#define EXFAT_MEMORY_MAX_CHUNK_SIZE UINT32_C(67108864)

struct bitmap_reader {
	struct stream_cursor cursor;
	unsigned char current_byte;
	unsigned int bit_in_byte;
};

static enum exfat_resize_error zero_allocation_model(struct resize_allocation *context)
{
	enum exfat_resize_error error;
	size_t byte_offset = 0;

	while (byte_offset < context->allocation_model_size) {
		size_t remaining = context->allocation_model_size - byte_offset;
		size_t byte_count = remaining > EXFAT_MEMORY_MAX_CHUNK_SIZE
		    ? (size_t)EXFAT_MEMORY_MAX_CHUNK_SIZE
		    : remaining;

		error = exfat_resize_cancellation_checkpoint(context->volume->operation);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		memset((unsigned char *)context->allocation_model + byte_offset, 0, byte_count);
		byte_offset += byte_count;
	}
	return EXFAT_RESIZE_SUCCESS;
}

enum exfat_resize_error exfat_resize_load_source_fat(struct resize_allocation *context)
{
	enum exfat_resize_error error;
	uint64_t allocation_size;
	uint32_t loaded_sector_count = 0;
	uint32_t read_sector_capacity;
	uint32_t sector_count;

	sector_count = exfat_resize_used_fat_sector_count(
	    context->volume->geometry.cluster_count, context->volume->sector_size);
	if (sector_count > context->volume->geometry.fat_length)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	allocation_size = (uint64_t)sector_count * context->volume->sector_size;
	context->source_fat_size = (size_t)allocation_size;
	if (context->source_fat_size != allocation_size)
		return EXFAT_RESIZE_ARITHMETIC_OVERFLOW;
	context->source_fat = context->volume->operation->allocator.allocate(
	    context->volume->operation->allocator.context, context->source_fat_size);
	if (context->source_fat == NULL)
		return EXFAT_RESIZE_OUT_OF_MEMORY;

	read_sector_capacity = (uint32_t)(EXFAT_IO_MAX_CHUNK_SIZE / context->volume->sector_size);
	while (loaded_sector_count < sector_count) {
		uint32_t remaining_sector_count = sector_count - loaded_sector_count;
		uint32_t read_sector_count = remaining_sector_count > read_sector_capacity
		    ? read_sector_capacity
		    : remaining_sector_count;
		size_t byte_offset = (size_t)loaded_sector_count * context->volume->sector_size;

		error = exfat_resize_cancellation_checkpoint(context->volume->operation);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		error = exfat_resize_block_device_read(context->volume->device,
		    context->volume->geometry.fat_offset + loaded_sector_count, read_sector_count,
		    context->source_fat + byte_offset, context->source_fat_size - byte_offset);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		loaded_sector_count += read_sector_count;
	}
	return EXFAT_RESIZE_SUCCESS;
}

enum exfat_resize_error exfat_resize_source_fat_get(
    const struct resize_allocation *context, uint32_t cluster, uint32_t *value)
{
	size_t offset;

	if (cluster > context->volume->geometry.cluster_count + UINT32_C(1))
		return EXFAT_RESIZE_OUT_OF_BOUNDS;
	offset = (size_t)cluster * 4;
	return exfat_resize_load_le32(context->source_fat, context->source_fat_size, offset, value);
}

void exfat_resize_release_source_fat(struct resize_allocation *context)
{
	if (context->source_fat == NULL)
		return;
	context->volume->operation->allocator.deallocate(context->volume->operation->allocator.context,
	    context->source_fat, context->source_fat_size);
	context->source_fat = NULL;
	context->source_fat_size = 0;
}

enum exfat_resize_error exfat_resize_validate_reserved_fat_entries(
    struct resize_allocation *context)
{
	enum exfat_resize_error error;
	uint32_t entry_one;

	error = exfat_resize_source_fat_get(context, 0, &context->fat_entry_zero);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = exfat_resize_source_fat_get(context, 1, &entry_one);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;

	/*
	 * FAT entry zero contains the media type in its low byte. Preserve that
	 * byte, but require the reserved upper bytes and entry one to have the
	 * values mandated by the exFAT specification.
	 */
	if ((context->fat_entry_zero & UINT32_C(0xffffff00)) != UINT32_C(0xffffff00) ||
	    entry_one != EXFAT_FAT_END_OF_CHAIN)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	return EXFAT_RESIZE_SUCCESS;
}

enum exfat_resize_error exfat_resize_model_entry(
    struct resize_allocation *context, uint32_t cluster, uint32_t **entry)
{
	if (!exfat_resize_cluster_is_valid(context->model_geometry, cluster))
		return EXFAT_RESIZE_INTERNAL_ERROR;
	*entry = &context->allocation_model[cluster - 2];
	return EXFAT_RESIZE_SUCCESS;
}

enum exfat_resize_error exfat_resize_claim_model_cluster(
    struct resize_allocation *context, uint32_t cluster)
{
	uint32_t *entry;
	enum exfat_resize_error error = exfat_resize_model_entry(context, cluster, &entry);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	if (*entry != 0)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	*entry = EXFAT_MODEL_NO_FAT_CHAIN;
	return EXFAT_RESIZE_SUCCESS;
}

enum exfat_resize_error exfat_resize_visit_allocation(struct resize_volume *volume,
    const struct exfat_resize_geometry *geometry,
    const struct stream_chain_reader *chain,
    const struct allocation_stream *stream,
    const struct allocation_observer *observer)
{
	struct allocation_link link = { .cluster = stream->first_cluster };
	uint32_t cluster_count;
	enum exfat_resize_error error;

	if (observer == NULL || observer->claim == NULL || chain == NULL ||
	    (!stream->no_fat_chain && chain->next == NULL))
		return EXFAT_RESIZE_INVALID_ARGUMENT;
	if (stream->root_directory) {
		uint64_t maximum = EXFAT_MAX_DIRECTORY_SIZE / volume->cluster_size;
		if (stream->no_fat_chain)
			return EXFAT_RESIZE_INVALID_FILESYSTEM;
		cluster_count =
		    maximum < geometry->cluster_count ? (uint32_t)maximum : geometry->cluster_count;
	} else {
		error = exfat_resize_stream_cluster_count(volume->cluster_size, stream, &cluster_count);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		if (cluster_count > geometry->cluster_count)
			return EXFAT_RESIZE_INVALID_FILESYSTEM;
		if (cluster_count == 0)
			return stream->first_cluster == 0 ? EXFAT_RESIZE_SUCCESS
			                                  : EXFAT_RESIZE_INVALID_FILESYSTEM;
	}
	if (!exfat_resize_cluster_is_valid(geometry, stream->first_cluster) ||
	    (stream->no_fat_chain &&
	        (uint64_t)stream->first_cluster + cluster_count >
	            (uint64_t)geometry->cluster_count + 2))
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	for (link.index = 0; link.index < cluster_count; ++link.index) {
		if (!stream->root_directory) {
			error = exfat_resize_cluster_step_checkpoint(volume->operation);
			if (error != EXFAT_RESIZE_SUCCESS)
				return error;
		}
		error = observer->claim(observer->context, link.cluster);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		if (stream->no_fat_chain) {
			link.next = link.index + 1 == cluster_count ? EXFAT_FAT_END_OF_CHAIN : link.cluster + 1;
		} else {
			error = chain->next(chain->context, link.cluster, &link.next);
			if (error != EXFAT_RESIZE_SUCCESS)
				return error;
		}
		if (!stream->root_directory && link.index + 1 == cluster_count) {
			if (link.next != EXFAT_FAT_END_OF_CHAIN)
				return EXFAT_RESIZE_INVALID_FILESYSTEM;
		} else if (link.next != EXFAT_FAT_END_OF_CHAIN || !stream->root_directory) {
			if (!exfat_resize_cluster_is_valid(geometry, link.next))
				return EXFAT_RESIZE_INVALID_FILESYSTEM;
		}
		if (observer->record != NULL) {
			error = observer->record(observer->context, stream, &link);
			if (error != EXFAT_RESIZE_SUCCESS)
				return error;
		}
		if (link.next == EXFAT_FAT_END_OF_CHAIN)
			return EXFAT_RESIZE_SUCCESS;
		link.previous = link.cluster;
		link.cluster = link.next;
	}
	return EXFAT_RESIZE_INVALID_FILESYSTEM;
}

enum exfat_resize_error exfat_resize_reconcile_allocation(struct resize_volume *volume,
    const struct exfat_resize_geometry *geometry,
    const struct stream_chain_reader *chain,
    const struct allocation_stream *bitmap,
    const struct allocation_observer *observer)
{
	struct bitmap_reader reader = { 0 };
	enum exfat_resize_error error;
	uint32_t source_index;

	if (observer == NULL || observer->is_claimed == NULL || observer->bad_cluster == NULL ||
	    chain == NULL || chain->next == NULL)
		return EXFAT_RESIZE_INVALID_ARGUMENT;
	error = exfat_resize_initialize_stream_cursor(
	    geometry, bitmap, SECTOR_CACHE_SOURCE_BITMAP_DATA, chain, &reader.cursor);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	/* Recognized owners claim first; remaining allocated bits must be bad clusters.
	 * FAT contents for free clusters are otherwise unspecified. */
	for (source_index = 0; source_index < geometry->cluster_count; ++source_index) {
		uint32_t cluster = source_index + 2;
		uint32_t fat_value;
		int claimed;
		int allocated;
		error = observer->is_claimed(observer->context, cluster, &claimed);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		if (reader.bit_in_byte == 0) {
			error = exfat_resize_read_stream(volume, &reader.cursor, &reader.current_byte, 1);
			if (error != EXFAT_RESIZE_SUCCESS)
				return error;
		}
		allocated = (reader.current_byte & (1u << reader.bit_in_byte)) != 0;
		reader.bit_in_byte = (reader.bit_in_byte + 1) % 8;
		if (claimed) {
			if (!allocated)
				return EXFAT_RESIZE_INVALID_FILESYSTEM;
			continue;
		}
		error = chain->next(chain->context, cluster, &fat_value);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		if (fat_value == EXFAT_FAT_BAD_CLUSTER) {
			if (!allocated)
				return EXFAT_RESIZE_INVALID_FILESYSTEM;
			error = observer->bad_cluster(observer->context, cluster);
			if (error != EXFAT_RESIZE_SUCCESS)
				return error;
		} else if (allocated) {
			return EXFAT_RESIZE_INVALID_FILESYSTEM;
		}
	}
	return EXFAT_RESIZE_SUCCESS;
}

enum exfat_resize_error exfat_resize_remove_allocation_from_model(
    struct resize_allocation *context, const struct allocation_stream *stream)
{
	enum exfat_resize_error error;
	uint32_t cluster_count;
	uint32_t cluster;
	uint32_t index;
	uint32_t next;

	error =
	    exfat_resize_stream_cluster_count(context->volume->cluster_size, stream, &cluster_count);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	cluster = stream->first_cluster;
	for (index = 0; index < cluster_count; ++index) {
		uint32_t *model_entry;

		if (!exfat_resize_cluster_is_valid(context->model_geometry, cluster))
			return EXFAT_RESIZE_INTERNAL_ERROR;
		model_entry = &context->allocation_model[cluster - 2];
		if (*model_entry == 0 || *model_entry == EXFAT_MODEL_NO_FAT_CHAIN ||
		    *model_entry == EXFAT_FAT_BAD_CLUSTER)
			return EXFAT_RESIZE_INTERNAL_ERROR;
		/*
		 * The caller has already validated this allocation and populated the
		 * model chain in model_geometry coordinates.
		 */
		next = *model_entry;
		*model_entry = 0;

		if (index + 1 == cluster_count)
			return next == EXFAT_FAT_END_OF_CHAIN ? EXFAT_RESIZE_SUCCESS
			                                      : EXFAT_RESIZE_INTERNAL_ERROR;
		if (!exfat_resize_cluster_is_valid(context->model_geometry, next))
			return EXFAT_RESIZE_INTERNAL_ERROR;
		cluster = next;
	}
	return EXFAT_RESIZE_INTERNAL_ERROR;
}

enum exfat_resize_error exfat_resize_add_new_bitmap_to_model(struct resize_allocation *context)
{
	enum exfat_resize_error error;
	uint32_t cluster_count;
	uint32_t cluster;
	uint32_t index;

	error = exfat_resize_stream_cluster_count(
	    context->volume->cluster_size, &context->new_bitmap, &cluster_count);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	for (index = 0; index < cluster_count; ++index) {
		cluster = context->new_bitmap.first_cluster + index;
		if (!exfat_resize_cluster_is_valid(context->model_geometry, cluster))
			return EXFAT_RESIZE_INTERNAL_ERROR;
		if (context->allocation_model[cluster - 2] != 0)
			return EXFAT_RESIZE_INTERNAL_ERROR;
		context->allocation_model[cluster - 2] =
		    index + 1 == cluster_count ? EXFAT_FAT_END_OF_CHAIN : cluster + 1;
	}
	return EXFAT_RESIZE_SUCCESS;
}

enum exfat_resize_error exfat_resize_write_target_fat(struct resize_allocation *context)
{
	enum exfat_resize_error error;
	uint64_t fat_sector = 0;
	uint32_t fat_sector_count;

	fat_sector_count = exfat_resize_used_fat_sector_count(
	    context->model_geometry->cluster_count, context->volume->sector_size);
	if (fat_sector_count > context->model_geometry->fat_length)
		return EXFAT_RESIZE_INTERNAL_ERROR;

	while (fat_sector < fat_sector_count) {
		uint64_t remaining = fat_sector_count - fat_sector;
		uint32_t sector_count = remaining > context->volume->io_sector_capacity
		    ? context->volume->io_sector_capacity
		    : (uint32_t)remaining;
		size_t byte_count = (size_t)sector_count * context->volume->sector_size;
		uint64_t first_entry = fat_sector * context->volume->sector_size / 4;
		size_t entry_count = byte_count / 4;
		size_t index;

		/* The caller checks for cancellation before the first output buffer. */
		if (fat_sector != 0) {
			error = exfat_resize_cancellation_checkpoint(context->volume->operation);
			if (error != EXFAT_RESIZE_SUCCESS)
				return error;
		}
		memset(context->volume->io_buffer, 0, byte_count);
		for (index = 0; index < entry_count; ++index) {
			uint64_t entry = first_entry + index;
			uint32_t value;

			if (entry > context->model_geometry->cluster_count + UINT64_C(1))
				break;
			if (entry == 0) {
				value = context->fat_entry_zero;
			} else if (entry == 1) {
				value = EXFAT_FAT_END_OF_CHAIN;
			} else {
				value = context->allocation_model[entry - 2];
				if (value == EXFAT_MODEL_NO_FAT_CHAIN)
					value = 0;
			}
			error =
			    exfat_resize_store_le32(context->volume->io_buffer, byte_count, index * 4, value);
			if (error != EXFAT_RESIZE_SUCCESS)
				return EXFAT_RESIZE_INTERNAL_ERROR;
		}
		error = exfat_resize_write_volume(context->volume,
		    context->model_geometry->fat_offset + fat_sector, sector_count,
		    context->volume->io_buffer, EXFAT_IO_BUFFER_SIZE);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		fat_sector += sector_count;
	}
	return EXFAT_RESIZE_SUCCESS;
}

enum exfat_resize_error exfat_resize_write_target_bitmap(struct resize_allocation *context)
{
	enum exfat_resize_error error;
	const size_t generation_byte_capacity = EXFAT_CLUSTER_CHECKPOINT_INTERVAL / 8;
	uint64_t bitmap_sector;
	uint64_t allocation_sector_count;
	uint64_t output_sector = 0;
	uint64_t target_bit = 0;

	error = exfat_resize_cluster_sector(
	    context->model_geometry, context->new_bitmap.first_cluster, &bitmap_sector);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	allocation_sector_count =
	    (context->new_bitmap.data_length + context->volume->cluster_size - 1) /
	    context->volume->cluster_size * context->model_geometry->sectors_per_cluster;
	context->used_cluster_count = 0;

	while (output_sector < allocation_sector_count) {
		uint64_t remaining = allocation_sector_count - output_sector;
		uint32_t sector_count = remaining > context->volume->io_sector_capacity
		    ? context->volume->io_sector_capacity
		    : (uint32_t)remaining;
		size_t byte_count = (size_t)sector_count * context->volume->sector_size;
		size_t byte_index = 0;

		while (byte_index < byte_count) {
			size_t remaining_byte_count = byte_count - byte_index;
			size_t generation_byte_count = remaining_byte_count > generation_byte_capacity
			    ? generation_byte_capacity
			    : remaining_byte_count;
			size_t generation_end = byte_index + generation_byte_count;

			error = exfat_resize_cancellation_checkpoint(context->volume->operation);
			if (error != EXFAT_RESIZE_SUCCESS)
				return error;
			memset(context->volume->io_buffer + byte_index, 0, generation_byte_count);
			for (; byte_index < generation_end; ++byte_index) {
				unsigned int bit_index;

				for (bit_index = 0; bit_index < 8; ++bit_index, ++target_bit) {
					if (target_bit >= context->model_geometry->cluster_count)
						break;
					if (context->allocation_model[target_bit] != 0) {
						context->volume->io_buffer[byte_index] |= (unsigned char)(1u << bit_index);
						++context->used_cluster_count;
					}
				}
			}
		}
		error = exfat_resize_write_volume(context->volume, bitmap_sector + output_sector,
		    sector_count, context->volume->io_buffer, EXFAT_IO_BUFFER_SIZE);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		output_sector += sector_count;
	}
	return EXFAT_RESIZE_SUCCESS;
}

enum exfat_resize_error exfat_resize_create_allocation_model(struct resize_allocation *context)
{
	uint64_t model_size;
	enum exfat_resize_error error;

	model_size =
	    (uint64_t)context->model_geometry->cluster_count * sizeof(*context->allocation_model);
	if (model_size > SIZE_MAX)
		return EXFAT_RESIZE_ARITHMETIC_OVERFLOW;
	context->allocation_model_size = (size_t)model_size;
	context->allocation_model = context->volume->operation->allocator.allocate(
	    context->volume->operation->allocator.context, context->allocation_model_size);
	if (context->allocation_model == NULL)
		return EXFAT_RESIZE_OUT_OF_MEMORY;
	error = zero_allocation_model(context);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;

	return EXFAT_RESIZE_SUCCESS;
}

void exfat_resize_release_allocation_model(struct resize_allocation *context)
{
	if (context->allocation_model != NULL) {
		context->volume->operation->allocator.deallocate(
		    context->volume->operation->allocator.context, context->allocation_model,
		    context->allocation_model_size);
		context->allocation_model = NULL;
	}
}

static enum exfat_resize_error read_source_link(
    const void *context, uint32_t cluster, uint32_t *next)
{
	return exfat_resize_source_fat_get(context, cluster, next);
}

static enum exfat_resize_error read_model_link(
    const void *context, uint32_t cluster, uint32_t *next)
{
	const struct resize_allocation *allocation = context;
	if (!exfat_resize_cluster_is_valid(allocation->model_geometry, cluster))
		return EXFAT_RESIZE_INTERNAL_ERROR;
	*next = allocation->allocation_model[cluster - 2];
	if (*next == EXFAT_MODEL_NO_FAT_CHAIN)
		*next = 0;
	return EXFAT_RESIZE_SUCCESS;
}

struct stream_chain_reader exfat_resize_source_chain(const struct resize_allocation *allocation)
{
	return (struct stream_chain_reader){ .context = allocation, .next = read_source_link };
}

struct stream_chain_reader exfat_resize_model_chain(const struct resize_allocation *allocation)
{
	return (struct stream_chain_reader){ .context = allocation, .next = read_model_link };
}

enum exfat_resize_error exfat_resize_write_fat_entry(struct resize_volume *volume,
    const struct exfat_resize_geometry *geometry,
    uint32_t cluster,
    uint32_t value)
{
	uint64_t offset = (uint64_t)cluster * 4;
	uint64_t sector = offset / volume->sector_size;
	enum exfat_resize_error error;
	if (!exfat_resize_cluster_is_valid(geometry, cluster) ||
	    (value != 0 && value != EXFAT_FAT_END_OF_CHAIN && value != EXFAT_FAT_BAD_CLUSTER &&
	        !exfat_resize_cluster_is_valid(geometry, value)))
		return EXFAT_RESIZE_INVALID_ARGUMENT;
	if (sector >= geometry->fat_length)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	sector += geometry->fat_offset;
	error = exfat_resize_read_volume(volume, sector, 1, volume->io_buffer, EXFAT_IO_BUFFER_SIZE);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = exfat_resize_store_le32(
	    volume->io_buffer, volume->sector_size, (size_t)(offset % volume->sector_size), value);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	return exfat_resize_write_volume(volume, sector, 1, volume->io_buffer, EXFAT_IO_BUFFER_SIZE);
}

enum exfat_resize_error exfat_resize_set_bitmap_bit(struct resize_volume *volume,
    const struct exfat_resize_geometry *geometry,
    const struct stream_chain_reader *chain,
    const struct allocation_stream *bitmap,
    uint32_t cluster,
    int allocated)
{
	struct stream_cursor cursor;
	uint64_t sector;
	size_t offset;
	unsigned char mask;
	enum exfat_resize_error error;
	if (!exfat_resize_cluster_is_valid(geometry, cluster) || bitmap->root_directory)
		return EXFAT_RESIZE_INVALID_ARGUMENT;
	error = exfat_resize_initialize_stream_cursor(
	    geometry, bitmap, SECTOR_CACHE_SOURCE_BITMAP_DATA, chain, &cursor);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = exfat_resize_skip_stream(volume, &cursor, ((uint64_t)cluster - 2) / 8);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	if (cursor.exhausted)
		return EXFAT_RESIZE_OUT_OF_BOUNDS;
	error = exfat_resize_cluster_sector(geometry, cursor.current_cluster, &sector);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	sector += cursor.cluster_offset / volume->sector_size;
	offset = (size_t)(cursor.cluster_offset % volume->sector_size);
	error = exfat_resize_read_volume(volume, sector, 1, volume->io_buffer, EXFAT_IO_BUFFER_SIZE);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	mask = (unsigned char)(1u << ((cluster - 2) % 8));
	if (allocated)
		volume->io_buffer[offset] |= mask;
	else
		volume->io_buffer[offset] &= (unsigned char)~mask;
	return exfat_resize_write_volume(volume, sector, 1, volume->io_buffer, EXFAT_IO_BUFFER_SIZE);
}
