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

int exfat_resize_fat_value_is_end_of_chain(uint32_t value)
{
	return value == EXFAT_FAT_END_OF_CHAIN;
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

enum exfat_resize_error exfat_resize_model_entry_for_source_cluster(
    struct resize_allocation *context, uint32_t source_cluster, uint32_t **entry)
{
	enum exfat_resize_error error;
	uint32_t target_cluster;

	error = exfat_resize_map_cluster(context, source_cluster, &target_cluster);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	if (target_cluster < 2 || target_cluster > context->target->cluster_count + UINT32_C(1))
		return EXFAT_RESIZE_INTERNAL_ERROR;
	*entry = &context->allocation_model[target_cluster - 2];
	return EXFAT_RESIZE_SUCCESS;
}

enum exfat_resize_error exfat_resize_claim_allocation_stream(
    struct resize_allocation *context, const struct allocation_stream *stream)
{
	enum exfat_resize_error error;
	uint32_t *model_entry;
	uint32_t cluster_count;
	uint32_t cluster;
	uint32_t index;
	uint32_t next;
	uint32_t target_next;

	error =
	    exfat_resize_stream_cluster_count(context->volume->cluster_size, stream, &cluster_count);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	if (cluster_count > context->volume->geometry.cluster_count)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	if (cluster_count == 0)
		return stream->first_cluster == 0 ? EXFAT_RESIZE_SUCCESS : EXFAT_RESIZE_INVALID_FILESYSTEM;
	if (!exfat_resize_cluster_is_valid(&context->volume->geometry, stream->first_cluster))
		return EXFAT_RESIZE_INVALID_FILESYSTEM;

	if (stream->no_fat_chain) {
		int crosses =
		    exfat_resize_mapping_breaks_contiguity(context, stream->first_cluster, cluster_count);

		if ((uint64_t)stream->first_cluster + cluster_count >
		    (uint64_t)context->volume->geometry.cluster_count + 2)
			return EXFAT_RESIZE_INVALID_FILESYSTEM;
		for (index = 0; index < cluster_count; ++index) {
			error = exfat_resize_cluster_step_checkpoint(context->volume->operation);
			if (error != EXFAT_RESIZE_SUCCESS)
				return error;
			error = exfat_resize_model_entry_for_source_cluster(
			    context, stream->first_cluster + index, &model_entry);
			if (error != EXFAT_RESIZE_SUCCESS)
				return error;
			if (*model_entry != 0)
				return EXFAT_RESIZE_INVALID_FILESYSTEM;
			if (!crosses) {
				*model_entry = EXFAT_MODEL_NO_FAT_CHAIN;
			} else if (index + 1 == cluster_count) {
				*model_entry = EXFAT_FAT_END_OF_CHAIN;
			} else {
				error = exfat_resize_map_cluster(
				    context, stream->first_cluster + index + 1, &target_next);
				if (error != EXFAT_RESIZE_SUCCESS)
					return error;
				*model_entry = target_next;
			}
		}
		return EXFAT_RESIZE_SUCCESS;
	}

	cluster = stream->first_cluster;
	for (index = 0; index < cluster_count; ++index) {
		error = exfat_resize_cluster_step_checkpoint(context->volume->operation);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		error = exfat_resize_model_entry_for_source_cluster(context, cluster, &model_entry);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		if (*model_entry != 0)
			return EXFAT_RESIZE_INVALID_FILESYSTEM;
		error = exfat_resize_source_fat_get(context, cluster, &next);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		if (index + 1 == cluster_count) {
			if (!exfat_resize_fat_value_is_end_of_chain(next))
				return EXFAT_RESIZE_INVALID_FILESYSTEM;
			*model_entry = EXFAT_FAT_END_OF_CHAIN;
			return EXFAT_RESIZE_SUCCESS;
		}
		if (!exfat_resize_cluster_is_valid(&context->volume->geometry, next))
			return EXFAT_RESIZE_INVALID_FILESYSTEM;
		error = exfat_resize_map_cluster(context, next, &target_next);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		*model_entry = target_next;
		cluster = next;
	}
	return EXFAT_RESIZE_INTERNAL_ERROR;
}

enum exfat_resize_error exfat_resize_claim_root_directory(
    struct resize_allocation *context, uint32_t first_cluster)
{
	enum exfat_resize_error error;
	uint32_t *model_entry;
	uint32_t cluster = first_cluster;
	uint32_t next;
	uint32_t target_next;
	uint32_t traversed;
	uint64_t maximum_cluster_count = EXFAT_MAX_DIRECTORY_SIZE / context->volume->cluster_size;

	for (traversed = 0; traversed < context->volume->geometry.cluster_count; ++traversed) {
		if (traversed >= maximum_cluster_count)
			return EXFAT_RESIZE_INVALID_FILESYSTEM;
		if (!exfat_resize_cluster_is_valid(&context->volume->geometry, cluster))
			return EXFAT_RESIZE_INVALID_FILESYSTEM;
		error = exfat_resize_model_entry_for_source_cluster(context, cluster, &model_entry);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		if (*model_entry != 0)
			return EXFAT_RESIZE_INVALID_FILESYSTEM;
		error = exfat_resize_source_fat_get(context, cluster, &next);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		if (exfat_resize_fat_value_is_end_of_chain(next)) {
			*model_entry = EXFAT_FAT_END_OF_CHAIN;
			return EXFAT_RESIZE_SUCCESS;
		}
		if (!exfat_resize_cluster_is_valid(&context->volume->geometry, next))
			return EXFAT_RESIZE_INVALID_FILESYSTEM;
		error = exfat_resize_map_cluster(context, next, &target_next);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		*model_entry = target_next;
		cluster = next;
	}
	return EXFAT_RESIZE_INVALID_FILESYSTEM;
}

static enum exfat_resize_error initialize_bitmap_reader(
    struct resize_allocation *context, struct bitmap_reader *reader)
{
	memset(reader, 0, sizeof(*reader));
	return exfat_resize_initialize_stream_cursor(&context->volume->geometry, &context->old_bitmap,
	    SECTOR_CACHE_SOURCE_BITMAP_DATA, STREAM_CHAIN_SOURCE_FAT, &reader->cursor);
}

static enum exfat_resize_error read_old_bitmap_bit(
    struct resize_allocation *context, struct bitmap_reader *reader, int *allocated)
{
	enum exfat_resize_error error;

	if (reader->bit_in_byte == 0) {
		error = exfat_resize_read_stream(
		    context->volume, context, &reader->cursor, &reader->current_byte, 1);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
	}

	*allocated = (reader->current_byte & (1u << reader->bit_in_byte)) != 0;
	++reader->bit_in_byte;
	if (reader->bit_in_byte == 8)
		reader->bit_in_byte = 0;
	return EXFAT_RESIZE_SUCCESS;
}

enum exfat_resize_error exfat_resize_validate_allocation_model(struct resize_allocation *context)
{
	struct bitmap_reader reader;
	enum exfat_resize_error error;
	uint32_t source_index;

	/*
	 * Every recognized owner has already claimed its clusters. A remaining
	 * allocated bit is valid only when the source FAT identifies a bad
	 * cluster. FAT contents for free clusters are otherwise unspecified.
	 */
	error = initialize_bitmap_reader(context, &reader);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	for (source_index = 0; source_index < context->volume->geometry.cluster_count; ++source_index) {
		uint32_t source_cluster = source_index + 2;
		uint32_t *model_entry;
		uint32_t fat_value;
		int allocated;

		error = exfat_resize_model_entry_for_source_cluster(context, source_cluster, &model_entry);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		error = read_old_bitmap_bit(context, &reader, &allocated);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;

		if (*model_entry != 0) {
			if (!allocated)
				return EXFAT_RESIZE_INVALID_FILESYSTEM;
			continue;
		}

		error = exfat_resize_source_fat_get(context, source_cluster, &fat_value);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;
		if (fat_value == EXFAT_FAT_BAD_CLUSTER) {
			if (!allocated)
				return EXFAT_RESIZE_INVALID_FILESYSTEM;
			error = context->mapping.validate_bad_cluster(context->mapping.context, source_cluster);
			if (error != EXFAT_RESIZE_SUCCESS)
				return error;
			*model_entry = EXFAT_FAT_BAD_CLUSTER;
		} else if (allocated) {
			return EXFAT_RESIZE_INVALID_FILESYSTEM;
		}
	}
	return EXFAT_RESIZE_SUCCESS;
}

enum exfat_resize_error exfat_resize_remove_old_bitmap_from_model(struct resize_allocation *context)
{
	enum exfat_resize_error error;
	uint32_t cluster_count;
	uint32_t cluster;
	uint32_t index;
	uint32_t next;

	error = exfat_resize_stream_cluster_count(
	    context->volume->cluster_size, &context->old_bitmap, &cluster_count);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = exfat_resize_map_cluster(context, context->old_bitmap.first_cluster, &cluster);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	for (index = 0; index < cluster_count; ++index) {
		uint32_t *model_entry;

		if (!exfat_resize_cluster_is_valid(context->target, cluster))
			return EXFAT_RESIZE_INTERNAL_ERROR;
		model_entry = &context->allocation_model[cluster - 2];
		if (*model_entry == 0 || *model_entry == EXFAT_MODEL_NO_FAT_CHAIN ||
		    *model_entry == EXFAT_FAT_BAD_CLUSTER)
			return EXFAT_RESIZE_INTERNAL_ERROR;
		/*
		 * exfat_resize_claim_allocation_stream() has already validated the source FAT
		 * chain and stored its mapped links as the canonical model chain.
		 */
		next = *model_entry;
		*model_entry = 0;

		if (index + 1 == cluster_count)
			return next == EXFAT_FAT_END_OF_CHAIN ? EXFAT_RESIZE_SUCCESS
			                                      : EXFAT_RESIZE_INTERNAL_ERROR;
		if (!exfat_resize_cluster_is_valid(context->target, next))
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
		if (!exfat_resize_cluster_is_valid(context->target, cluster))
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
	    context->target->cluster_count, context->volume->sector_size);
	if (fat_sector_count > context->target->fat_length)
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

			if (entry > context->target->cluster_count + UINT64_C(1))
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
		error = exfat_resize_block_device_write(context->volume->device,
		    context->target->fat_offset + fat_sector, sector_count, context->volume->io_buffer,
		    EXFAT_IO_BUFFER_SIZE);
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
	    context->target, context->new_bitmap.first_cluster, &bitmap_sector);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	allocation_sector_count =
	    (context->new_bitmap.data_length + context->volume->cluster_size - 1) /
	    context->volume->cluster_size * context->target->sectors_per_cluster;
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
					if (target_bit >= context->target->cluster_count)
						break;
					if (context->allocation_model[target_bit] != 0) {
						context->volume->io_buffer[byte_index] |= (unsigned char)(1u << bit_index);
						++context->used_cluster_count;
					}
				}
			}
		}
		error =
		    exfat_resize_block_device_write(context->volume->device, bitmap_sector + output_sector,
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

	model_size = (uint64_t)context->target->cluster_count * sizeof(*context->allocation_model);
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

enum exfat_resize_error exfat_resize_map_cluster(
    const struct resize_allocation *context, uint32_t source_cluster, uint32_t *target_cluster)
{
	return context->mapping.map_cluster(context->mapping.context, source_cluster, target_cluster);
}

int exfat_resize_mapping_breaks_contiguity(
    const struct resize_allocation *context, uint32_t first_cluster, uint32_t cluster_count)
{
	return context->mapping.breaks_contiguity(
	    context->mapping.context, first_cluster, cluster_count);
}
