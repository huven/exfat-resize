/* SPDX-License-Identifier: MIT */

#ifndef EXFAT_RESIZE_TEST_MEMORY_BLOCK_DEVICE_H
#define EXFAT_RESIZE_TEST_MEMORY_BLOCK_DEVICE_H

#include "block_device.h"

#include <stddef.h>
#include <stdint.h>

enum memory_operation_kind { MEMORY_OPERATION_READ, MEMORY_OPERATION_WRITE, MEMORY_OPERATION_SYNC };
enum memory_failure_mode { MEMORY_FAILURE_BEFORE, MEMORY_FAILURE_AFTER };

struct memory_operation {
	enum memory_operation_kind kind;
	uint64_t first_sector;
	uint32_t sector_count;
};

struct memory_sector {
	uint64_t sector;
	unsigned char *data;
};

struct memory_block_device {
	struct exfat_resize_block_device device;

	struct memory_sector *sectors;
	size_t sector_count;
	size_t sector_capacity;

	struct memory_sector *durable_sectors;
	size_t durable_sector_count;

	struct memory_operation *operations;
	size_t operation_count;
	size_t operation_capacity;
	size_t operation_index;

	int failure_enabled;
	enum memory_failure_mode failure_mode;
	size_t failing_operation;
	uint32_t failure_sector_count;
	int failure_result;
};

void memory_block_device_init(
    struct memory_block_device *memory, uint32_t sector_size, uint64_t sector_count);

void memory_block_device_destroy(struct memory_block_device *memory);
void memory_block_device_clear_operations(struct memory_block_device *memory);

void memory_block_device_fail_operation(
    struct memory_block_device *memory, size_t operation_index, int failure_result);

void memory_block_device_fail_after_operation(struct memory_block_device *memory,
    size_t operation_index,
    uint32_t completed_sector_count,
    int failure_result);

void memory_block_device_clear_failure(struct memory_block_device *memory);

int memory_block_device_make_durable(struct memory_block_device *memory);
int memory_block_device_crash(struct memory_block_device *memory);

#endif
