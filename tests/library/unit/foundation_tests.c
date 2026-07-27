/* SPDX-License-Identifier: MIT */

#include "block_device.h"
#include "checked_math.h"
#include "endian.h"
#include "sector_adapter.h"
#include "support/memory_block_device.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failure_count;

#define CHECK(expression) \
	do { \
		if (!(expression)) { \
			fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expression); \
			++failure_count; \
		} \
	} while (0)

static void test_checked_math(void)
{
	enum exfat_resize_error error;
	uint64_t result;

	error = exfat_resize_checked_add_u64(20, 22, &result);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	CHECK(result == 42);

	error = exfat_resize_checked_add_u64(UINT64_MAX, 1, &result);
	CHECK(error == EXFAT_RESIZE_ARITHMETIC_OVERFLOW);

	error = exfat_resize_checked_multiply_u64(6, 7, &result);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	CHECK(result == 42);

	error = exfat_resize_checked_multiply_u64(UINT64_MAX, 2, &result);
	CHECK(error == EXFAT_RESIZE_ARITHMETIC_OVERFLOW);

	error = exfat_resize_checked_ceil_divide_u64(9, 4, &result);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	CHECK(result == 3);

	error = exfat_resize_checked_ceil_divide_u64(UINT64_MAX, 1, &result);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	CHECK(result == UINT64_MAX);

	error = exfat_resize_checked_ceil_divide_u64(0, 4, &result);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	CHECK(result == 0);

	error = exfat_resize_checked_ceil_divide_u64(1, 0, &result);
	CHECK(error == EXFAT_RESIZE_INVALID_ARGUMENT);

	error = exfat_resize_checked_add_u64(1, 2, NULL);
	CHECK(error == EXFAT_RESIZE_INVALID_ARGUMENT);
}

static void test_endian_access(void)
{
	unsigned char buffer[20] = { 0 };
	enum exfat_resize_error error;
	uint16_t value16;
	uint32_t value32;
	uint64_t value64;

	error = exfat_resize_store_le16(buffer, sizeof(buffer), 1, UINT16_C(0x1234));
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	error = exfat_resize_store_le32(buffer, sizeof(buffer), 3, UINT32_C(0x89abcdef));
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	error = exfat_resize_store_le64(buffer, sizeof(buffer), 7, UINT64_C(0x0123456789abcdef));
	CHECK(error == EXFAT_RESIZE_SUCCESS);

	error = exfat_resize_load_le16(buffer, sizeof(buffer), 1, &value16);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	CHECK(value16 == UINT16_C(0x1234));
	error = exfat_resize_load_le32(buffer, sizeof(buffer), 3, &value32);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	CHECK(value32 == UINT32_C(0x89abcdef));
	error = exfat_resize_load_le64(buffer, sizeof(buffer), 7, &value64);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	CHECK(value64 == UINT64_C(0x0123456789abcdef));

	error = exfat_resize_load_le64(buffer, sizeof(buffer), sizeof(buffer) - 7, &value64);
	CHECK(error == EXFAT_RESIZE_OUT_OF_BOUNDS);
	error = exfat_resize_store_le32(buffer, sizeof(buffer), sizeof(buffer) - 3, 0);
	CHECK(error == EXFAT_RESIZE_OUT_OF_BOUNDS);
	error = exfat_resize_load_le16(buffer, sizeof(buffer), 0, NULL);
	CHECK(error == EXFAT_RESIZE_INVALID_ARGUMENT);
}

static void test_device_geometry(void)
{
	static const uint32_t valid_sizes[] = { 512, 1024, 2048, 4096 };
	static const uint32_t invalid_sizes[] = { 0, 256, 768, 8192 };
	struct memory_block_device memory;
	struct exfat_resize_block_device device;
	enum exfat_resize_error error;
	size_t index;

	memory_block_device_init(&memory, 512, 128);
	device = memory.device;
	for (index = 0; index < sizeof(valid_sizes) / sizeof(valid_sizes[0]); ++index) {
		device.sector_size = valid_sizes[index];
		error = exfat_resize_validate_block_device(&device);
		CHECK(error == EXFAT_RESIZE_SUCCESS);
	}

	for (index = 0; index < sizeof(invalid_sizes) / sizeof(invalid_sizes[0]); ++index) {
		device.sector_size = invalid_sizes[index];
		error = exfat_resize_validate_block_device(&device);
		CHECK(error == EXFAT_RESIZE_INVALID_DEVICE);
	}

	device.sector_size = 512;
	device.sector_count = 0;
	error = exfat_resize_validate_block_device(&device);
	CHECK(error == EXFAT_RESIZE_INVALID_DEVICE);

	device.sector_size = 4096;
	device.sector_count = UINT64_MAX;
	error = exfat_resize_validate_block_device(&device);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	memory_block_device_destroy(&memory);
}

static void test_memory_block_device(void)
{
	struct memory_block_device memory;
	struct exfat_resize_block_device invalid_device;
	enum exfat_resize_error error;
	unsigned char written[1024];
	unsigned char read_back[1024];
	unsigned char zero_sector[512];
	unsigned char partial_sector[513];
	size_t operation_count;
	size_t index;

	memory_block_device_init(&memory, 512, 8);
	error = exfat_resize_validate_block_device(&memory.device);
	CHECK(error == EXFAT_RESIZE_SUCCESS);

	invalid_device = memory.device;
	invalid_device.read = NULL;
	error = exfat_resize_validate_block_device(&invalid_device);
	CHECK(error == EXFAT_RESIZE_INVALID_DEVICE);
	invalid_device = memory.device;
	invalid_device.write = NULL;
	error = exfat_resize_validate_block_device(&invalid_device);
	CHECK(error == EXFAT_RESIZE_INVALID_DEVICE);
	invalid_device = memory.device;
	invalid_device.sync = NULL;
	error = exfat_resize_validate_block_device(&invalid_device);
	CHECK(error == EXFAT_RESIZE_INVALID_DEVICE);

	memset(written, 0x5a, sizeof(written));

	error = exfat_resize_block_device_write(&memory.device, 2, 2, written, sizeof(written));
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	CHECK(memory.sector_count == 2);

	memset(read_back, 0, sizeof(read_back));
	error = exfat_resize_block_device_read(&memory.device, 2, 2, read_back, sizeof(read_back));
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	CHECK(memcmp(read_back, written, sizeof(read_back)) == 0);

	memset(zero_sector, 0xff, sizeof(zero_sector));
	error = exfat_resize_block_device_read(&memory.device, 0, 1, zero_sector, sizeof(zero_sector));
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	for (index = 0; index < sizeof(zero_sector); ++index)
		CHECK(zero_sector[index] == 0);

	CHECK(memory.operation_count == 3);
	CHECK(memory.operations[0].kind == MEMORY_OPERATION_WRITE);
	CHECK(memory.operations[0].first_sector == 2);
	CHECK(memory.operations[0].sector_count == 2);

	operation_count = memory.operation_count;
	memset(partial_sector, 0xff, sizeof(partial_sector));
	error = exfat_resize_block_device_read(
	    &memory.device, 0, 1, partial_sector, sizeof(partial_sector));
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	for (index = 0; index < sizeof(zero_sector); ++index)
		CHECK(partial_sector[index] == 0);
	CHECK(partial_sector[sizeof(partial_sector) - 1] == 0xff);
	CHECK(memory.operation_count == operation_count + 1);

	operation_count = memory.operation_count;
	error = exfat_resize_block_device_read(
	    &memory.device, 0, 1, partial_sector, sizeof(zero_sector) - 1);
	CHECK(error == EXFAT_RESIZE_INVALID_ARGUMENT);
	CHECK(memory.operation_count == operation_count);

	error = exfat_resize_block_device_read(&memory.device, 7, 2, read_back, sizeof(read_back));
	CHECK(error == EXFAT_RESIZE_OUT_OF_BOUNDS);
	CHECK(memory.operation_count == operation_count);

	error = exfat_resize_block_device_read(&memory.device, memory.device.sector_count, 0, NULL, 0);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	CHECK(memory.operation_count == operation_count);

	error = exfat_resize_block_device_write(&memory.device, memory.device.sector_count, 0, NULL, 0);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	CHECK(memory.operation_count == operation_count);

	error =
	    exfat_resize_block_device_read(&memory.device, memory.device.sector_count + 1, 0, NULL, 0);
	CHECK(error == EXFAT_RESIZE_OUT_OF_BOUNDS);
	CHECK(memory.operation_count == operation_count);

	memory_block_device_clear_operations(&memory);
	memory_block_device_fail_operation(&memory, 0, 1234);
	error = exfat_resize_block_device_read(&memory.device, 0, 1, zero_sector, sizeof(zero_sector));
	CHECK(error == EXFAT_RESIZE_IO_ERROR);
	CHECK(memory.operation_count == 1);
	CHECK(memory.operations[0].kind == MEMORY_OPERATION_READ);

	memory_block_device_clear_failure(&memory);
	memory_block_device_clear_operations(&memory);
	memory_block_device_fail_operation(&memory, 0, 1234);
	error = exfat_resize_block_device_write(&memory.device, 0, 1, zero_sector, sizeof(zero_sector));
	CHECK(error == EXFAT_RESIZE_IO_ERROR);
	CHECK(memory.operation_count == 1);
	CHECK(memory.operations[0].kind == MEMORY_OPERATION_WRITE);

	memory_block_device_clear_failure(&memory);
	memory_block_device_clear_operations(&memory);
	memory_block_device_fail_operation(&memory, 0, 1234);
	error = exfat_resize_block_device_sync(&memory.device);
	CHECK(error == EXFAT_RESIZE_IO_ERROR);
	CHECK(memory.operation_count == 1);
	CHECK(memory.operations[0].kind == MEMORY_OPERATION_SYNC);

	memory_block_device_clear_failure(&memory);
	memory_block_device_clear_operations(&memory);
	error = exfat_resize_block_device_sync(&memory.device);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	CHECK(memory.operations[memory.operation_count - 1].kind == MEMORY_OPERATION_SYNC);

	memory_block_device_destroy(&memory);
}

static void test_sector_adapter(void)
{
	struct exfat_resize_sector_adapter adapter;
	struct memory_block_device memory;
	enum exfat_resize_error error;
	unsigned char written[4096];
	unsigned char read_back[4096];

	memory_block_device_init(&memory, 512, 66);
	memset(written, 0x5a, sizeof(written));

	error = exfat_resize_adapt_block_device(&memory.device, 4096, &adapter);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	CHECK(adapter.device.sector_size == 4096);
	CHECK(adapter.device.sector_count == 8);

	error = exfat_resize_block_device_write(&adapter.device, 2, 1, written, sizeof(written));
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	CHECK(memory.operation_count == 1);
	CHECK(memory.operations[0].kind == MEMORY_OPERATION_WRITE);
	CHECK(memory.operations[0].first_sector == 16);
	CHECK(memory.operations[0].sector_count == 8);

	memset(read_back, 0, sizeof(read_back));
	error = exfat_resize_block_device_read(&adapter.device, 2, 1, read_back, sizeof(read_back));
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	CHECK(memcmp(read_back, written, sizeof(read_back)) == 0);
	CHECK(memory.operations[1].kind == MEMORY_OPERATION_READ);
	CHECK(memory.operations[1].first_sector == 16);
	CHECK(memory.operations[1].sector_count == 8);

	error = exfat_resize_block_device_read(&adapter.device, 8, 1, read_back, sizeof(read_back));
	CHECK(error == EXFAT_RESIZE_OUT_OF_BOUNDS);
	CHECK(memory.operation_count == 2);

	error = exfat_resize_adapt_block_device(&memory.device, 1024, &adapter);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	CHECK(adapter.device.sector_count == 33);
	error = exfat_resize_adapt_block_device(&memory.device, 768, &adapter);
	CHECK(error == EXFAT_RESIZE_UNSUPPORTED_SECTOR_MAPPING);
	error = exfat_resize_adapt_block_device(&memory.device, 4096, NULL);
	CHECK(error == EXFAT_RESIZE_INVALID_ARGUMENT);

	memory.device.sector_size = 4096;
	memory.device.sector_count = 8;
	error = exfat_resize_adapt_block_device(&memory.device, 512, &adapter);
	CHECK(error == EXFAT_RESIZE_UNSUPPORTED_SECTOR_MAPPING);
	memory_block_device_destroy(&memory);
}

static void test_memory_block_device_durability(void)
{
	struct memory_block_device memory;
	enum exfat_resize_error error;
	unsigned char original[1024];
	unsigned char replacement[1024];
	unsigned char read_back[1024];

	memset(original, 0x11, 512);
	memset(original + 512, 0x22, 512);
	memset(replacement, 0x33, 512);
	memset(replacement + 512, 0x44, 512);

	memory_block_device_init(&memory, 512, 4);
	error = exfat_resize_block_device_write(&memory.device, 0, 2, original, sizeof(original));
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	CHECK(memory_block_device_make_durable(&memory) == 0);
	memory_block_device_clear_operations(&memory);

	memory_block_device_fail_after_operation(&memory, 0, 1, 1234);
	error = exfat_resize_block_device_write(&memory.device, 0, 2, replacement, sizeof(replacement));
	CHECK(error == EXFAT_RESIZE_IO_ERROR);
	memory_block_device_clear_failure(&memory);
	error = exfat_resize_block_device_read(&memory.device, 0, 2, read_back, sizeof(read_back));
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	CHECK(memcmp(read_back, replacement, 512) == 0);
	CHECK(memcmp(read_back + 512, original + 512, 512) == 0);

	CHECK(memory_block_device_crash(&memory) == 0);
	error = exfat_resize_block_device_read(&memory.device, 0, 2, read_back, sizeof(read_back));
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	CHECK(memcmp(read_back, original, sizeof(read_back)) == 0);

	memory_block_device_clear_operations(&memory);
	error = exfat_resize_block_device_write(&memory.device, 0, 2, replacement, sizeof(replacement));
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	memory_block_device_fail_after_operation(&memory, 1, 0, 1234);
	error = exfat_resize_block_device_sync(&memory.device);
	CHECK(error == EXFAT_RESIZE_IO_ERROR);
	memory_block_device_clear_failure(&memory);
	CHECK(memory_block_device_crash(&memory) == 0);
	error = exfat_resize_block_device_read(&memory.device, 0, 2, read_back, sizeof(read_back));
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	CHECK(memcmp(read_back, replacement, sizeof(read_back)) == 0);

	memory_block_device_destroy(&memory);
}

static void test_sparse_large_device(void)
{
	static const uint64_t four_tibibytes = UINT64_C(4) * 1024 * 1024 * 1024 * 1024;
	static const uint32_t sector_size = 4096;
	const uint64_t sector_count = four_tibibytes / sector_size;
	struct memory_block_device memory;
	enum exfat_resize_error error;
	unsigned char sector[4096];
	unsigned char read_back[4096];

	memory_block_device_init(&memory, sector_size, sector_count);
	memset(sector, 0xa5, sizeof(sector));

	error = exfat_resize_block_device_write(
	    &memory.device, sector_count - 1, 1, sector, sizeof(sector));
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	CHECK(memory.sector_count == 1);

	error = exfat_resize_block_device_read(
	    &memory.device, sector_count - 1, 1, read_back, sizeof(read_back));
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	CHECK(memcmp(read_back, sector, sizeof(read_back)) == 0);

	error = exfat_resize_block_device_read(
	    &memory.device, sector_count, 1, read_back, sizeof(read_back));
	CHECK(error == EXFAT_RESIZE_OUT_OF_BOUNDS);

	memory_block_device_destroy(&memory);
}

int main(void)
{
	test_checked_math();
	test_endian_access();
	test_device_geometry();
	test_memory_block_device();
	test_sector_adapter();
	test_memory_block_device_durability();
	test_sparse_large_device();

	if (failure_count != 0) {
		fprintf(stderr, "%d foundation test(s) failed\n", failure_count);
		return 1;
	}

	printf("library foundation: passed\n");
	return 0;
}
