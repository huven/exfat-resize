/* SPDX-License-Identifier: MIT */

#include "support/memory_block_device.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int reserve_operations(struct memory_block_device *memory, size_t required)
{
	struct memory_operation *operations;
	size_t capacity = memory->operation_capacity;

	if (required <= capacity)
		return 0;
	if (capacity == 0)
		capacity = 8;
	while (capacity < required) {
		if (capacity > SIZE_MAX / 2)
			return ENOMEM;
		capacity *= 2;
	}
	if (capacity > SIZE_MAX / sizeof(*operations))
		return ENOMEM;
	operations = realloc(memory->operations, capacity * sizeof(*operations));
	if (operations == NULL)
		return ENOMEM;
	memory->operations = operations;
	memory->operation_capacity = capacity;
	return 0;
}

static int reserve_sectors(struct memory_block_device *memory, size_t required)
{
	struct memory_sector *sectors;
	size_t capacity = memory->sector_capacity;

	if (required <= capacity)
		return 0;
	if (capacity == 0)
		capacity = 8;
	while (capacity < required) {
		if (capacity > SIZE_MAX / 2)
			return ENOMEM;
		capacity *= 2;
	}
	if (capacity > SIZE_MAX / sizeof(*sectors))
		return ENOMEM;
	sectors = realloc(memory->sectors, capacity * sizeof(*sectors));
	if (sectors == NULL)
		return ENOMEM;
	memory->sectors = sectors;
	memory->sector_capacity = capacity;
	return 0;
}

static int record_operation(struct memory_block_device *memory,
    enum memory_operation_kind kind,
    uint64_t first_sector,
    uint32_t sector_count,
    size_t *operation_index)
{
	struct memory_operation *operation;
	size_t current_operation;
	int error;

	error = reserve_operations(memory, memory->operation_count + 1);
	if (error != 0)
		return error;
	operation = &memory->operations[memory->operation_count++];
	operation->kind = kind;
	operation->first_sector = first_sector;
	operation->sector_count = sector_count;

	current_operation = memory->operation_index++;
	if (operation_index != NULL)
		*operation_index = current_operation;
	if (memory->failure_enabled && memory->failure_mode == MEMORY_FAILURE_BEFORE &&
	    current_operation == memory->failing_operation)
		return memory->failure_result;
	return 0;
}

static int failure_follows_operation(
    const struct memory_block_device *memory, size_t operation_index)
{
	return memory->failure_enabled && memory->failure_mode == MEMORY_FAILURE_AFTER &&
	    operation_index == memory->failing_operation;
}

static size_t find_sector(const struct memory_block_device *memory, uint64_t sector)
{
	size_t index;

	for (index = 0; index < memory->sector_count; ++index) {
		if (memory->sectors[index].sector == sector)
			return index;
	}
	return SIZE_MAX;
}

static void free_sectors(struct memory_sector *sectors, size_t count)
{
	size_t index;

	for (index = 0; index < count; ++index)
		free(sectors[index].data);
	free(sectors);
}

static int copy_sectors(const struct memory_sector *source,
    size_t source_count,
    size_t sector_size,
    struct memory_sector **destination,
    size_t *destination_count,
    size_t *destination_capacity)
{
	struct memory_sector *copy = NULL;
	size_t index;

	if (source_count != 0) {
		if (source_count > SIZE_MAX / sizeof(*copy))
			return ENOMEM;
		copy = calloc(source_count, sizeof(*copy));
		if (copy == NULL)
			return ENOMEM;
		for (index = 0; index < source_count; ++index) {
			copy[index].sector = source[index].sector;
			copy[index].data = malloc(sector_size);
			if (copy[index].data == NULL) {
				free_sectors(copy, index);
				return ENOMEM;
			}
			memcpy(copy[index].data, source[index].data, sector_size);
		}
	}

	free_sectors(*destination, *destination_count);
	*destination = copy;
	*destination_count = source_count;
	*destination_capacity = source_count;
	return 0;
}

static int memory_read(void *context, uint64_t first_sector, uint32_t sector_count, void *buffer)
{
	struct memory_block_device *memory = context;
	unsigned char *destination = buffer;
	size_t sector_size = memory->device.sector_size;
	size_t operation_index;
	uint32_t completed_sector_count = sector_count;
	uint32_t index;
	int error;

	error = record_operation(
	    memory, MEMORY_OPERATION_READ, first_sector, sector_count, &operation_index);
	if (error != 0)
		return error;
	if (failure_follows_operation(memory, operation_index) &&
	    memory->failure_sector_count < completed_sector_count)
		completed_sector_count = memory->failure_sector_count;

	for (index = 0; index < completed_sector_count; ++index) {
		size_t stored = find_sector(memory, first_sector + index);
		unsigned char *sector = destination + (size_t)index * sector_size;
		if (stored == SIZE_MAX)
			memset(sector, 0, sector_size);
		else
			memcpy(sector, memory->sectors[stored].data, sector_size);
	}
	if (failure_follows_operation(memory, operation_index))
		return memory->failure_result;
	return 0;
}

static int memory_write(
    void *context, uint64_t first_sector, uint32_t sector_count, const void *buffer)
{
	struct memory_block_device *memory = context;
	const unsigned char *source = buffer;
	size_t sector_size = memory->device.sector_size;
	size_t operation_index;
	uint32_t completed_sector_count = sector_count;
	uint32_t index;
	int error;

	error = record_operation(
	    memory, MEMORY_OPERATION_WRITE, first_sector, sector_count, &operation_index);
	if (error != 0)
		return error;
	if (failure_follows_operation(memory, operation_index) &&
	    memory->failure_sector_count < completed_sector_count)
		completed_sector_count = memory->failure_sector_count;

	for (index = 0; index < completed_sector_count; ++index) {
		size_t stored = find_sector(memory, first_sector + index);
		if (stored == SIZE_MAX) {
			error = reserve_sectors(memory, memory->sector_count + 1);
			if (error != 0)
				return error;
			stored = memory->sector_count++;
			memory->sectors[stored].sector = first_sector + index;
			memory->sectors[stored].data = malloc(sector_size);
			if (memory->sectors[stored].data == NULL) {
				--memory->sector_count;
				return ENOMEM;
			}
		}
		memcpy(memory->sectors[stored].data, source + (size_t)index * sector_size, sector_size);
	}
	if (failure_follows_operation(memory, operation_index))
		return memory->failure_result;
	return 0;
}

static int memory_sync(void *context)
{
	struct memory_block_device *memory = context;
	size_t operation_index;
	int error;

	error = record_operation(memory, MEMORY_OPERATION_SYNC, 0, 0, &operation_index);
	if (error != 0)
		return error;
	error = memory_block_device_make_durable(memory);
	if (error != 0)
		return error;
	if (failure_follows_operation(memory, operation_index))
		return memory->failure_result;
	return 0;
}

void memory_block_device_init(
    struct memory_block_device *memory, uint32_t sector_size, uint64_t sector_count)
{
	memset(memory, 0, sizeof(*memory));
	memory->device.context = memory;
	memory->device.sector_size = sector_size;
	memory->device.sector_count = sector_count;
	memory->device.read = memory_read;
	memory->device.write = memory_write;
	memory->device.sync = memory_sync;
}

void memory_block_device_destroy(struct memory_block_device *memory)
{
	free_sectors(memory->sectors, memory->sector_count);
	free_sectors(memory->durable_sectors, memory->durable_sector_count);
	free(memory->operations);
	memset(memory, 0, sizeof(*memory));
}

void memory_block_device_clear_operations(struct memory_block_device *memory)
{
	memory->operation_count = 0;
	memory->operation_index = 0;
}

void memory_block_device_fail_operation(
    struct memory_block_device *memory, size_t operation_index, int failure_result)
{
	memory->failure_enabled = 1;
	memory->failure_mode = MEMORY_FAILURE_BEFORE;
	memory->failing_operation = operation_index;
	memory->failure_sector_count = 0;
	memory->failure_result = failure_result;
}

void memory_block_device_fail_after_operation(struct memory_block_device *memory,
    size_t operation_index,
    uint32_t completed_sector_count,
    int failure_result)
{
	memory->failure_enabled = 1;
	memory->failure_mode = MEMORY_FAILURE_AFTER;
	memory->failing_operation = operation_index;
	memory->failure_sector_count = completed_sector_count;
	memory->failure_result = failure_result;
}

void memory_block_device_clear_failure(struct memory_block_device *memory)
{
	memory->failure_enabled = 0;
	memory->failure_mode = MEMORY_FAILURE_BEFORE;
	memory->failure_sector_count = 0;
	memory->failure_result = 0;
}

int memory_block_device_make_durable(struct memory_block_device *memory)
{
	return copy_sectors(memory->sectors, memory->sector_count, memory->device.sector_size,
	    &memory->durable_sectors, &memory->durable_sector_count, &memory->durable_sector_capacity);
}

int memory_block_device_crash(struct memory_block_device *memory)
{
	return copy_sectors(memory->durable_sectors, memory->durable_sector_count,
	    memory->device.sector_size, &memory->sectors, &memory->sector_count,
	    &memory->sector_capacity);
}
