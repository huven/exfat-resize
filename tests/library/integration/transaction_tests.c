/* SPDX-License-Identifier: MIT */

#include "exfat_resize.h"

#include "endian.h"
#include "geometry.h"
#include "support/exfat_fixture.h"
#include "support/memory_block_device.h"
#include "support/test_allocator.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
	SECTOR_SIZE = 512,
	VOLUME_LENGTH_OFFSET = 72,
	VOLUME_FLAGS_OFFSET = 106,
	BACKUP_BOOT_REGION = 12,
};

static int failure_count;
static int saw_multi_sector_read;
static int saw_multi_sector_write;

struct boot_state {
	uint64_t volume_sector_count;
	uint16_t volume_flags;
};

#define CHECK(expression) \
	do { \
		if (!(expression)) { \
			fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expression); \
			++failure_count; \
		} \
	} while (0)

static enum exfat_resize_error plan_growth(const struct exfat_fixture *fixture,
    uint64_t target_sector_count,
    struct exfat_resize_geometry *target)
{
	struct exfat_resize_device_geometry device;

	device.logical_sector_size = fixture->memory.device.sector_size;
	device.sector_count = fixture->memory.device.sector_count;
	return exfat_resize_plan_growth(&device, &fixture->geometry, target_sector_count, target);
}

static int initialize_durable_fixture(struct exfat_fixture *fixture, uint64_t target_sector_count)
{
	if (exfat_fixture_initialize(fixture, target_sector_count) != 0)
		return -1;
	if (memory_block_device_make_durable(&fixture->memory) != 0) {
		exfat_fixture_destroy(fixture);
		return -1;
	}
	memory_block_device_clear_operations(&fixture->memory);
	return 0;
}

static struct boot_state read_boot_state(struct exfat_fixture *fixture, uint64_t first_sector)
{
	struct boot_state state = { 0 };
	unsigned char sector[SECTOR_SIZE];

	CHECK(exfat_fixture_read_sector(fixture, first_sector, sector, sizeof(sector)) == 0);
	CHECK(exfat_resize_load_le64(sector, sizeof(sector), VOLUME_LENGTH_OFFSET,
	          &state.volume_sector_count) == EXFAT_RESIZE_SUCCESS);
	CHECK(exfat_resize_load_le16(sector, sizeof(sector), VOLUME_FLAGS_OFFSET,
	          &state.volume_flags) == EXFAT_RESIZE_SUCCESS);
	return state;
}

static int durable_image_matches_except_dirty(
    const struct memory_block_device *actual, const struct memory_block_device *expected)
{
	size_t sector_index;
	size_t byte_index;

	if (actual->sector_count != expected->durable_sector_count)
		return 0;
	for (sector_index = 0; sector_index < actual->sector_count; ++sector_index) {
		const struct memory_sector *actual_sector = &actual->sectors[sector_index];
		const struct memory_sector *expected_sector = &expected->durable_sectors[sector_index];

		if (actual_sector->sector != expected_sector->sector)
			return 0;
		for (byte_index = 0; byte_index < actual->device.sector_size; ++byte_index) {
			unsigned char actual_byte = actual_sector->data[byte_index];
			unsigned char expected_byte = expected_sector->data[byte_index];

			if (actual_sector->sector == 0 && byte_index == VOLUME_FLAGS_OFFSET) {
				actual_byte &= (unsigned char)~UINT8_C(0x02);
				expected_byte &= (unsigned char)~UINT8_C(0x02);
			}
			if (actual_byte != expected_byte)
				return 0;
		}
	}
	return 1;
}

static void check_durable_boundary(struct exfat_fixture *fixture,
    const struct exfat_fixture *completed_fixture,
    const struct exfat_resize_geometry *target,
    size_t completed_sync_count)
{
	struct boot_state main;
	struct boot_state backup;
	uint64_t expected_main_volume;
	uint64_t expected_backup_volume;
	int expected_dirty;

	CHECK(completed_sync_count <= 5);
	CHECK(memory_block_device_crash(&fixture->memory) == 0);
	main = read_boot_state(fixture, 0);
	backup = read_boot_state(fixture, BACKUP_BOOT_REGION);

	expected_main_volume = completed_sync_count >= 4 ? target->volume_sector_count
	                                                 : fixture->geometry.volume_sector_count;
	expected_backup_volume = completed_sync_count >= 3 ? target->volume_sector_count
	                                                   : fixture->geometry.volume_sector_count;
	expected_dirty = completed_sync_count >= 1 && completed_sync_count < 5;
	CHECK(main.volume_sector_count == expected_main_volume);
	CHECK(backup.volume_sector_count == expected_backup_volume);
	CHECK(((main.volume_flags & UINT16_C(0x0002)) != 0) == expected_dirty);
	if (completed_sync_count >= 4)
		CHECK(durable_image_matches_except_dirty(&fixture->memory, &completed_fixture->memory));
}

static size_t count_syncs_before(const struct memory_operation *operations, size_t operation_index)
{
	size_t count = 0;
	size_t index;

	for (index = 0; index < operation_index; ++index) {
		if (operations[index].kind == MEMORY_OPERATION_SYNC)
			++count;
	}
	return count;
}

static void check_preparing_write_ranges(const struct memory_operation *operations,
    size_t first_transaction_operation,
    size_t first_fat_write,
    const struct exfat_resize_geometry *source)
{
	uint64_t source_heap_end =
	    source->cluster_heap_offset + (uint64_t)source->cluster_count * source->sectors_per_cluster;
	size_t index;

	for (index = first_transaction_operation; index < first_fat_write; ++index) {
		const struct memory_operation *operation = &operations[index];

		if (operation->kind != MEMORY_OPERATION_WRITE)
			continue;
		if (operation->first_sector == 0) {
			CHECK(operation->sector_count == 1);
			continue;
		}
		CHECK(operation->first_sector >= source_heap_end);
	}
}

static enum exfat_resize_stage expected_stage(const struct memory_operation *operations,
    size_t operation_index,
    size_t first_transaction_operation,
    size_t first_fat_write)
{
	size_t completed_sync_count = count_syncs_before(operations, operation_index);

	if (operation_index < first_transaction_operation)
		return EXFAT_RESIZE_STAGE_PREFLIGHT;
	if (operations[operation_index].kind == MEMORY_OPERATION_SYNC) {
		switch (completed_sync_count + 1) {
		case 1:
			return EXFAT_RESIZE_STAGE_PREPARING;
		case 2:
		case 3:
		case 4:
			return EXFAT_RESIZE_STAGE_RESIZING;
		case 5:
			return EXFAT_RESIZE_STAGE_FINALIZING;
		}
	}
	if (completed_sync_count == 0 || operation_index < first_fat_write)
		return EXFAT_RESIZE_STAGE_PREPARING;
	if (completed_sync_count < 4)
		return EXFAT_RESIZE_STAGE_RESIZING;
	return EXFAT_RESIZE_STAGE_FINALIZING;
}

static void check_transaction_order(const struct memory_operation *operations,
    size_t operation_count,
    uint32_t fat_offset,
    size_t syncs[5],
    size_t *first_transaction_operation,
    size_t *first_fat_write)
{
	size_t sync_count = 0;
	size_t index;
	int backup_sector_written = 0;
	int backup_checksum_written = 0;
	int main_sector_written = 0;
	int main_checksum_written = 0;

	*first_transaction_operation = SIZE_MAX;
	*first_fat_write = SIZE_MAX;
	for (index = 0; index < operation_count; ++index) {
		if (operations[index].kind == MEMORY_OPERATION_SYNC) {
			CHECK(sync_count < 5);
			if (sync_count < 5)
				syncs[sync_count++] = index;
		}
		if (operations[index].kind == MEMORY_OPERATION_WRITE &&
		    operations[index].first_sector == fat_offset && *first_fat_write == SIZE_MAX)
			*first_fat_write = index;
	}
	CHECK(sync_count == 5);
	CHECK(*first_fat_write != SIZE_MAX);
	if (sync_count != 5 || *first_fat_write == SIZE_MAX)
		return;

	CHECK(syncs[0] != 0);
	CHECK(operations[syncs[0] - 1].kind == MEMORY_OPERATION_WRITE);
	CHECK(operations[syncs[0] - 1].first_sector == 0);
	CHECK(syncs[0] >= 2);
	if (syncs[0] < 2)
		return;
	*first_transaction_operation = syncs[0] - 2;
	CHECK(operations[*first_transaction_operation].kind == MEMORY_OPERATION_READ);
	CHECK(operations[*first_transaction_operation].first_sector == 0);
	CHECK(*first_fat_write > syncs[0]);
	CHECK(*first_fat_write < syncs[1]);
	CHECK(syncs[4] + 1 == operation_count);

	for (index = syncs[1] + 1; index < syncs[2]; ++index) {
		if (operations[index].kind == MEMORY_OPERATION_WRITE &&
		    operations[index].first_sector == BACKUP_BOOT_REGION)
			backup_sector_written = 1;
		if (operations[index].kind == MEMORY_OPERATION_WRITE &&
		    operations[index].first_sector == BACKUP_BOOT_REGION + 11)
			backup_checksum_written = 1;
	}
	for (index = syncs[2] + 1; index < syncs[3]; ++index) {
		if (operations[index].kind == MEMORY_OPERATION_WRITE && operations[index].first_sector == 0)
			main_sector_written = 1;
		if (operations[index].kind == MEMORY_OPERATION_WRITE &&
		    operations[index].first_sector == 11)
			main_checksum_written = 1;
	}
	CHECK(backup_sector_written);
	CHECK(backup_checksum_written);
	CHECK(main_sector_written);
	CHECK(main_checksum_written);
	for (index = syncs[3] + 1; index < syncs[4]; ++index) {
		if (operations[index].kind == MEMORY_OPERATION_WRITE) {
			CHECK(operations[index].first_sector == 0);
			CHECK(operations[index].sector_count == 1);
		}
	}
	CHECK(operations[syncs[4] - 1].kind == MEMORY_OPERATION_WRITE);
	CHECK(operations[syncs[4] - 1].first_sector == 0);
}

static void run_fault_case(const struct memory_operation *baseline,
    const struct exfat_fixture *completed_fixture,
    size_t operation_index,
    size_t first_transaction_operation,
    size_t first_fat_write,
    int fail_after,
    uint32_t completed_sector_count,
    uint64_t target_sector_count,
    const struct exfat_resize_geometry *target)
{
	struct test_allocator allocator = { 0 };
	struct exfat_resize_allocator callbacks = test_allocator_callbacks(&allocator);
	struct exfat_fixture fixture;
	enum exfat_resize_error error;
	enum exfat_resize_stage stage = EXFAT_RESIZE_STAGE_COMPLETED;
	size_t completed_sync_count = count_syncs_before(baseline, operation_index);

	CHECK(initialize_durable_fixture(&fixture, target_sector_count) == 0);
	if (fail_after) {
		memory_block_device_fail_after_operation(
		    &fixture.memory, operation_index, completed_sector_count, 1234);
	} else {
		memory_block_device_fail_operation(&fixture.memory, operation_index, 1234);
	}
	error = exfat_fixture_resize(&fixture.memory.device, target_sector_count, &callbacks, &stage);
	CHECK(error == EXFAT_RESIZE_IO_ERROR);
	CHECK(stage ==
	    expected_stage(baseline, operation_index, first_transaction_operation, first_fat_write));
	CHECK(fixture.memory.operation_count == operation_index + 1);
	CHECK(allocator.successful_allocations == allocator.deallocation_calls);
	CHECK(test_allocator_is_clean(&allocator));

	if (baseline[operation_index].kind == MEMORY_OPERATION_SYNC && fail_after)
		++completed_sync_count;
	memory_block_device_clear_failure(&fixture.memory);
	check_durable_boundary(&fixture, completed_fixture, target, completed_sync_count);
	exfat_fixture_destroy(&fixture);
}

static void test_transaction_failures(uint64_t target_sector_count)
{
	struct test_allocator allocator = { 0 };
	struct exfat_resize_geometry target;
	struct exfat_resize_allocator callbacks = test_allocator_callbacks(&allocator);
	struct exfat_fixture fixture;
	struct memory_operation *baseline = NULL;
	enum exfat_resize_error error;
	enum exfat_resize_stage stage = EXFAT_RESIZE_STAGE_PREFLIGHT;
	size_t first_transaction_operation = SIZE_MAX;
	size_t first_fat_write = SIZE_MAX;
	size_t operation_count;
	size_t operation_index;
	size_t syncs[5];
	int saw_preflight_read = 0;
	int saw_preparing_read = 0;
	int saw_resizing_read = 0;
	int saw_finalizing_read = 0;

	CHECK(initialize_durable_fixture(&fixture, target_sector_count) == 0);
	error = plan_growth(&fixture, target_sector_count, &target);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	error = exfat_fixture_resize(&fixture.memory.device, target_sector_count, &callbacks, &stage);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	CHECK(stage == EXFAT_RESIZE_STAGE_COMPLETED);
	CHECK(allocator.successful_allocations == allocator.deallocation_calls);
	CHECK(test_allocator_is_clean(&allocator));
	operation_count = fixture.memory.operation_count;
	if (operation_count != 0) {
		baseline = malloc(operation_count * sizeof(*baseline));
		CHECK(baseline != NULL);
		if (baseline != NULL)
			memcpy(baseline, fixture.memory.operations, operation_count * sizeof(*baseline));
	}
	if (baseline != NULL)
		check_transaction_order(baseline, operation_count, target.fat_offset, syncs,
		    &first_transaction_operation, &first_fat_write);
	if (baseline == NULL || first_transaction_operation == SIZE_MAX ||
	    first_fat_write == SIZE_MAX) {
		exfat_fixture_destroy(&fixture);
		free(baseline);
		return;
	}
	check_preparing_write_ranges(
	    baseline, first_transaction_operation, first_fat_write, &fixture.geometry);

	for (operation_index = 0; operation_index < operation_count; ++operation_index) {
		const struct memory_operation *operation = &baseline[operation_index];
		enum exfat_resize_stage operation_stage =
		    expected_stage(baseline, operation_index, first_transaction_operation, first_fat_write);

		run_fault_case(baseline, &fixture, operation_index, first_transaction_operation,
		    first_fat_write, 0, 0, target_sector_count, &target);
		run_fault_case(baseline, &fixture, operation_index, first_transaction_operation,
		    first_fat_write, 1, operation->sector_count, target_sector_count, &target);
		if (operation->sector_count > 1) {
			run_fault_case(baseline, &fixture, operation_index, first_transaction_operation,
			    first_fat_write, 1, 0, target_sector_count, &target);
			run_fault_case(baseline, &fixture, operation_index, first_transaction_operation,
			    first_fat_write, 1, 1, target_sector_count, &target);
		}

		if (operation->kind == MEMORY_OPERATION_READ) {
			saw_preflight_read |= operation_stage == EXFAT_RESIZE_STAGE_PREFLIGHT;
			saw_preparing_read |= operation_stage == EXFAT_RESIZE_STAGE_PREPARING;
			saw_resizing_read |= operation_stage == EXFAT_RESIZE_STAGE_RESIZING;
			saw_finalizing_read |= operation_stage == EXFAT_RESIZE_STAGE_FINALIZING;
			saw_multi_sector_read |= operation->sector_count > 1;
		} else if (operation->kind == MEMORY_OPERATION_WRITE) {
			saw_multi_sector_write |= operation->sector_count > 1;
		}
	}

	CHECK(saw_preflight_read);
	CHECK(saw_preparing_read);
	CHECK(saw_resizing_read);
	CHECK(saw_finalizing_read);
	exfat_fixture_destroy(&fixture);
	free(baseline);
}

int main(void)
{
	static const uint64_t target_sector_counts[] = {
		12003,   /* No heap shift. */
		29951,   /* Overlapping source and target heaps. */
		1544927, /* Shift greater than the source cluster count. */
	};
	size_t index;

	for (index = 0; index < sizeof(target_sector_counts) / sizeof(target_sector_counts[0]); ++index)
		test_transaction_failures(target_sector_counts[index]);

	CHECK(saw_multi_sector_read);
	CHECK(saw_multi_sector_write);
	if (failure_count != 0) {
		fprintf(stderr, "%d transaction test(s) failed\n", failure_count);
		return 1;
	}
	printf("library transaction: passed\n");
	return 0;
}
