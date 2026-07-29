/* SPDX-License-Identifier: MIT */

#include "exfat_resize.h"

#include "support/exfat_fixture.h"
#include "support/memory_block_device.h"
#include "support/test_allocator.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { TARGET_SECTOR_COUNT = 65536 };

static int failure_count;

#define CHECK(expression) \
	do { \
		if (!(expression)) { \
			fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expression); \
			++failure_count; \
		} \
	} while (0)

static void test_invalid_devices(void)
{
	enum invalid_device_kind {
		NULL_DEVICE,
		MISSING_READ,
		MISSING_WRITE,
		MISSING_SYNC,
		INVALID_SECTOR_SIZE,
		EMPTY_DEVICE,
	};
	static const enum invalid_device_kind cases[] = {
		NULL_DEVICE,
		MISSING_READ,
		MISSING_WRITE,
		MISSING_SYNC,
		INVALID_SECTOR_SIZE,
		EMPTY_DEVICE,
	};
	size_t index;

	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
		struct test_allocator allocator = { 0 };
		struct exfat_resize_options options = test_allocator_options(&allocator);
		struct memory_block_device memory;
		struct exfat_resize_block_device invalid;
		const struct exfat_resize_block_device *device;
		enum exfat_resize_error error;
		enum exfat_resize_stage stage = EXFAT_RESIZE_STAGE_COMPLETED;

		memory_block_device_init(&memory, 512, TARGET_SECTOR_COUNT);
		invalid = memory.device;
		device = &invalid;
		switch (cases[index]) {
		case NULL_DEVICE:
			device = NULL;
			break;
		case MISSING_READ:
			invalid.read = NULL;
			break;
		case MISSING_WRITE:
			invalid.write = NULL;
			break;
		case MISSING_SYNC:
			invalid.sync = NULL;
			break;
		case INVALID_SECTOR_SIZE:
			invalid.sector_size = 768;
			break;
		case EMPTY_DEVICE:
			invalid.sector_count = 0;
			break;
		}

		error = exfat_fixture_resize(device, TARGET_SECTOR_COUNT, &options, &stage);
		CHECK(error == EXFAT_RESIZE_INVALID_DEVICE);
		CHECK(stage == EXFAT_RESIZE_STAGE_PREFLIGHT);
		CHECK(allocator.allocation_attempts == 0);
		CHECK(allocator.deallocation_calls == 0);
		CHECK(test_allocator_is_clean(&allocator));
		CHECK(memory.operation_count == 0);
		memory_block_device_destroy(&memory);
	}
}

static void test_invalid_options(void)
{
	enum invalid_options_kind { NULL_OPTIONS, MISSING_ALLOCATE, MISSING_DEALLOCATE };
	static const enum invalid_options_kind cases[] = {
		NULL_OPTIONS,
		MISSING_ALLOCATE,
		MISSING_DEALLOCATE,
	};
	size_t index;

	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
		struct test_allocator allocator = { 0 };
		struct exfat_resize_options options = test_allocator_options(&allocator);
		struct memory_block_device memory;
		const struct exfat_resize_options *selected_options = &options;
		enum exfat_resize_error error;
		enum exfat_resize_stage stage = EXFAT_RESIZE_STAGE_COMPLETED;

		memory_block_device_init(&memory, 512, TARGET_SECTOR_COUNT);
		switch (cases[index]) {
		case NULL_OPTIONS:
			selected_options = NULL;
			break;
		case MISSING_ALLOCATE:
			options.allocator.allocate = NULL;
			break;
		case MISSING_DEALLOCATE:
			options.allocator.deallocate = NULL;
			break;
		}

		error = exfat_fixture_resize(&memory.device, TARGET_SECTOR_COUNT, selected_options, &stage);
		CHECK(error == EXFAT_RESIZE_INVALID_ARGUMENT);
		CHECK(stage == EXFAT_RESIZE_STAGE_PREFLIGHT);
		CHECK(allocator.allocation_attempts == 0);
		CHECK(allocator.deallocation_calls == 0);
		CHECK(test_allocator_is_clean(&allocator));
		CHECK(memory.operation_count == 0);
		memory_block_device_destroy(&memory);
	}
}

static void test_invalid_targets(void)
{
	enum target_kind { EQUAL_TARGET, SMALLER_TARGET, OVERSIZED_TARGET };
	static const enum target_kind cases[] = {
		EQUAL_TARGET,
		SMALLER_TARGET,
		OVERSIZED_TARGET,
	};
	size_t index;

	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
		struct test_allocator allocator = { 0 };
		struct exfat_resize_options options = test_allocator_options(&allocator);
		struct exfat_fixture fixture;
		enum exfat_resize_error error;
		enum exfat_resize_error expected = EXFAT_RESIZE_INTERNAL_ERROR;
		enum exfat_resize_stage stage = EXFAT_RESIZE_STAGE_COMPLETED;
		size_t expected_allocations = 2;
		uint64_t target = 0;

		CHECK(exfat_fixture_initialize(&fixture, TARGET_SECTOR_COUNT) == 0);
		switch (cases[index]) {
		case EQUAL_TARGET:
			target = fixture.geometry.volume_sector_count;
			expected = EXFAT_RESIZE_INVALID_ARGUMENT;
			break;
		case SMALLER_TARGET:
			target = fixture.geometry.volume_sector_count - 1;
			expected = EXFAT_RESIZE_INVALID_ARGUMENT;
			break;
		case OVERSIZED_TARGET:
			target = fixture.memory.device.sector_count + 1;
			expected = EXFAT_RESIZE_OUT_OF_BOUNDS;
			expected_allocations = 1;
			break;
		}

		error = exfat_fixture_resize(&fixture.memory.device, target, &options, &stage);
		CHECK(error == expected);
		CHECK(stage == EXFAT_RESIZE_STAGE_PREFLIGHT);
		CHECK(allocator.allocation_attempts == expected_allocations);
		CHECK(allocator.deallocation_calls == allocator.successful_allocations);
		CHECK(test_allocator_is_clean(&allocator));
		for (size_t operation = 0; operation < fixture.memory.operation_count; ++operation)
			CHECK(fixture.memory.operations[operation].kind == MEMORY_OPERATION_READ);
		exfat_fixture_destroy(&fixture);
	}
}

static void test_preflight_io_error(void)
{
	struct test_allocator allocator = { 0 };
	struct exfat_resize_options options = test_allocator_options(&allocator);
	struct exfat_fixture fixture;
	enum exfat_resize_error error;
	enum exfat_resize_stage stage = EXFAT_RESIZE_STAGE_COMPLETED;

	CHECK(exfat_fixture_initialize(&fixture, TARGET_SECTOR_COUNT) == 0);
	memory_block_device_fail_operation(&fixture.memory, 0, 1234);
	error = exfat_fixture_resize(&fixture.memory.device, TARGET_SECTOR_COUNT, &options, &stage);
	CHECK(error == EXFAT_RESIZE_IO_ERROR);
	CHECK(stage == EXFAT_RESIZE_STAGE_PREFLIGHT);
	CHECK(allocator.allocation_attempts == 1);
	CHECK(allocator.deallocation_calls == 1);
	CHECK(test_allocator_is_clean(&allocator));
	CHECK(fixture.memory.operation_count == 1);
	CHECK(fixture.memory.operations[0].kind == MEMORY_OPERATION_READ);
	exfat_fixture_destroy(&fixture);
}

static void test_unsupported_sector_mapping(void)
{
	struct test_allocator allocator = { 0 };
	struct exfat_resize_options options = test_allocator_options(&allocator);
	struct memory_block_device memory;
	enum exfat_resize_error error;
	enum exfat_resize_stage stage = EXFAT_RESIZE_STAGE_COMPLETED;
	unsigned char sector[4096] = { 0 };

	memory_block_device_init(&memory, 4096, 128);
	sector[0] = 0xeb;
	sector[1] = 0x76;
	sector[2] = 0x90;
	memcpy(sector + 3, "EXFAT   ", 8);
	sector[108] = 9;
	sector[510] = 0x55;
	sector[511] = 0xaa;
	CHECK(memory.device.write(memory.device.context, 0, 1, sector) == 0);
	memory_block_device_clear_operations(&memory);

	error = exfat_resize(
	    &memory.device, memory.device.sector_count * memory.device.sector_size, &options, &stage);
	CHECK(error == EXFAT_RESIZE_UNSUPPORTED_SECTOR_MAPPING);
	CHECK(stage == EXFAT_RESIZE_STAGE_PREFLIGHT);
	CHECK(allocator.allocation_attempts == 1);
	CHECK(allocator.deallocation_calls == 1);
	CHECK(test_allocator_is_clean(&allocator));
	CHECK(memory.operation_count == 1);
	CHECK(memory.operations[0].kind == MEMORY_OPERATION_READ);
	memory_block_device_destroy(&memory);
}

int main(void)
{
	test_invalid_devices();
	test_invalid_options();
	test_invalid_targets();
	test_preflight_io_error();
	test_unsupported_sector_mapping();

	if (failure_count != 0) {
		fprintf(stderr, "%d public API test(s) failed\n", failure_count);
		return 1;
	}
	printf("library public API: passed\n");
	return 0;
}
