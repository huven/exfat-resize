/* SPDX-License-Identifier: MIT */

#include "exfat_resize.h"

#include "allocation.h"
#include "endian.h"
#include "geometry.h"
#include "resize_internal.h"
#include "stream.h"
#include "support/exfat_fixture.h"
#include "support/test_allocator.h"
#include "volume.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { SECTOR_SIZE = 512, GROW_SECTORS = 65536, SHRINK_SECTORS = 8000 };

static int failure_count;

#define CHECK(expression) \
	do { \
		if (!(expression)) { \
			fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expression); \
			++failure_count; \
		} \
	} while (0)

struct observation {
	struct exfat_resize_event failure;
	size_t failure_count;
	size_t stage_count;
	size_t cancellation_calls;
	size_t cancellation_calls_at_failure;
	int cancel_on_error;
	int cancelled;
};

struct expected_failure {
	enum exfat_resize_error error;
	uint32_t code;
	uint64_t values[3];
};

static void observe(void *opaque, const struct exfat_resize_event *event)
{
	struct observation *observation = opaque;
	if (event->code == EXFAT_RESIZE_EVENT_CODE_STAGE_ENTERED) {
		CHECK(event->level == EXFAT_RESIZE_EVENT_LEVEL_INFO);
		CHECK(event->values[0] == EXFAT_RESIZE_STAGE_PREFLIGHT);
		++observation->stage_count;
	} else {
		CHECK(event->level == EXFAT_RESIZE_EVENT_LEVEL_ERROR);
		observation->failure = *event;
		++observation->failure_count;
		observation->cancellation_calls_at_failure = observation->cancellation_calls;
		if (observation->cancel_on_error)
			observation->cancelled = 1;
	}
}

static int cancellation_requested(void *opaque)
{
	struct observation *observation = opaque;
	++observation->cancellation_calls;
	return observation->cancelled;
}

static int store_fat(struct exfat_fixture *fixture, uint32_t cluster, uint32_t value)
{
	unsigned char sector[SECTOR_SIZE];
	uint64_t number = fixture->geometry.fat_offset + cluster / 128;
	if (exfat_fixture_read_sector(fixture, number, sector, sizeof(sector)) != 0 ||
	    exfat_resize_store_le32(sector, sizeof(sector), cluster % 128 * 4, value) !=
	        EXFAT_RESIZE_SUCCESS)
		return -1;
	return fixture->memory.device.write(fixture->memory.device.context, number, 1, sector);
}

static int set_bit(struct exfat_fixture *fixture, uint32_t cluster, int allocated)
{
	unsigned char sector[SECTOR_SIZE];
	uint32_t bit = cluster - 2;
	uint32_t bitmap_cluster = fixture->bitmap_clusters[bit / 8 / SECTOR_SIZE];
	uint64_t number = exfat_fixture_cluster_sector(&fixture->geometry, bitmap_cluster);
	unsigned char mask = (unsigned char)(1u << (bit % 8));
	if (exfat_fixture_read_sector(fixture, number, sector, sizeof(sector)) != 0)
		return -1;
	if (allocated)
		sector[bit / 8 % SECTOR_SIZE] |= mask;
	else
		sector[bit / 8 % SECTOR_SIZE] &= (unsigned char)~mask;
	return fixture->memory.device.write(fixture->memory.device.context, number, 1, sector);
}

static uint16_t checksum(const unsigned char *entry)
{
	uint16_t sum = 0;
	size_t index;
	for (index = 0; index < 96; ++index) {
		if (index != 2 && index != 3) {
			uint16_t rot = (uint16_t)(((uint32_t)sum << 15) | ((uint32_t)sum >> 1));
			sum = (uint16_t)(rot + entry[index]);
		}
	}
	return sum;
}

static void update_checksum(unsigned char *entry)
{
	CHECK(exfat_resize_store_le16(entry, 96, 2, checksum(entry)) == EXFAT_RESIZE_SUCCESS);
}

enum mutation {
	UNREFERENCED,
	REFERENCED_FREE,
	BAD_FREE,
	REPEATED_CLUSTER,
	EARLY_END,
	INVALID_LINK,
	MISSING_END,
	EMPTY_WITH_CLUSTER,
	INVALID_FIRST_CLUSTER,
	CONTIGUOUS_RANGE,
	STREAM_TOO_LARGE,
	RESERVED_FAT_ZERO,
	RESERVED_FAT_ONE,
	MISSING_BITMAP,
	DUPLICATE_BITMAP,
	SHORT_BITMAP,
	BITMAP_FLAGS,
	ENTRY_CHECKSUM,
	SECONDARY_COUNT,
	ENTRY_ORDER,
	ENTRY_FLAGS,
	VALID_LENGTH,
	DIRECTORY_LENGTH,
	DIRECTORY_TOO_LARGE,
	EMPTY_NO_FAT_CHAIN,
	MISPLACED_SYSTEM_ENTRY,
	TRUNCATED_SET,
	UNSUPPORTED_ENTRY,
	MUTATION_COUNT
};

static struct expected_failure mutate(struct exfat_fixture *fixture, enum mutation mutation)
{
	struct expected_failure expected = { .error = EXFAT_RESIZE_INVALID_FILESYSTEM };
	unsigned char root[SECTOR_SIZE];
	unsigned char *file = root + 64;
	unsigned char *directory = root + 160;
	uint64_t root_sector = exfat_fixture_cluster_sector(&fixture->geometry, 2);
	uint64_t bitmap_length = ((uint64_t)fixture->geometry.cluster_count + 7) / 8;
	CHECK(exfat_fixture_read_sector(fixture, root_sector, root, sizeof(root)) == 0);

	switch (mutation) {
	case UNREFERENCED:
		CHECK(set_bit(fixture, 9, 1) == 0);
		CHECK(store_fat(fixture, 9, EXFAT_FAT_END_OF_CHAIN) == 0);
		expected.code = EXFAT_RESIZE_EVENT_CODE_UNREFERENCED_CLUSTER;
		expected.values[0] = 9;
		expected.values[1] = EXFAT_FAT_END_OF_CHAIN;
		break;
	case REFERENCED_FREE:
		CHECK(set_bit(fixture, 5, 0) == 0);
		expected.code = EXFAT_RESIZE_EVENT_CODE_REFERENCED_CLUSTER_MARKED_FREE;
		expected.values[0] = 5;
		break;
	case BAD_FREE:
		CHECK(store_fat(fixture, 9, EXFAT_FAT_BAD_CLUSTER) == 0);
		expected.code = EXFAT_RESIZE_EVENT_CODE_BAD_CLUSTER_MARKED_FREE;
		expected.values[0] = 9;
		break;
	case REPEATED_CLUSTER:
		CHECK(store_fat(fixture, 12, 10) == 0);
		expected.code = EXFAT_RESIZE_EVENT_CODE_CLUSTER_ALREADY_REFERENCED;
		expected.values[0] = 10;
		expected.values[1] = 10;
		expected.values[2] = 2;
		break;
	case EARLY_END:
	case INVALID_LINK:
	case MISSING_END:
		expected.code = EXFAT_RESIZE_EVENT_CODE_FAT_CHAIN_INVALID;
		expected.values[0] = mutation == MISSING_END ? 11 : 10;
		expected.values[1] = mutation == EARLY_END ? EXFAT_FAT_END_OF_CHAIN
		    : mutation == INVALID_LINK             ? fixture->geometry.cluster_count + UINT64_C(2)
		                                           : 13;
		expected.values[2] = mutation == EARLY_END ? EXFAT_RESIZE_CHAIN_EARLY_END
		    : mutation == INVALID_LINK             ? EXFAT_RESIZE_CHAIN_INVALID_LINK
		                                           : EXFAT_RESIZE_CHAIN_MISSING_END;
		CHECK(store_fat(fixture, (uint32_t)expected.values[0], (uint32_t)expected.values[1]) == 0);
		break;
	case EMPTY_WITH_CLUSTER:
	case INVALID_FIRST_CLUSTER:
	case CONTIGUOUS_RANGE:
	case STREAM_TOO_LARGE: {
		uint32_t first = mutation == INVALID_FIRST_CLUSTER ? 1
		    : mutation == CONTIGUOUS_RANGE                 ? fixture->geometry.cluster_count + 1
		                                                   : 5;
		uint64_t length = mutation == EMPTY_WITH_CLUSTER ? 0
		    : mutation == CONTIGUOUS_RANGE               ? 1024
		    : mutation == STREAM_TOO_LARGE ? ((uint64_t)fixture->geometry.cluster_count + 1) * 512
		                                   : 512;
		file[33] = mutation == EMPTY_WITH_CLUSTER ? 1 : 3;
		CHECK(exfat_resize_store_le64(file, 96, 40, length) == EXFAT_RESIZE_SUCCESS);
		CHECK(exfat_resize_store_le32(file, 96, 52, first) == EXFAT_RESIZE_SUCCESS);
		CHECK(exfat_resize_store_le64(file, 96, 56, length) == EXFAT_RESIZE_SUCCESS);
		update_checksum(file);
		expected.code = EXFAT_RESIZE_EVENT_CODE_ALLOCATION_STREAM_INVALID;
		expected.values[0] = first;
		expected.values[1] = length;
		expected.values[2] = mutation == EMPTY_WITH_CLUSTER ? EXFAT_RESIZE_STREAM_EMPTY_WITH_CLUSTER
		    : mutation == INVALID_FIRST_CLUSTER ? EXFAT_RESIZE_STREAM_INVALID_FIRST_CLUSTER
		    : mutation == CONTIGUOUS_RANGE      ? EXFAT_RESIZE_STREAM_CONTIGUOUS_RANGE
		                                        : EXFAT_RESIZE_STREAM_LENGTH_EXCEEDS_VOLUME;
		break;
	}
	case RESERVED_FAT_ZERO:
	case RESERVED_FAT_ONE:
		expected.code = EXFAT_RESIZE_EVENT_CODE_RESERVED_FAT_ENTRY_INVALID;
		expected.values[0] = mutation == RESERVED_FAT_ZERO ? 0 : 1;
		expected.values[1] =
		    mutation == RESERVED_FAT_ZERO ? UINT32_C(0xfffef8) : UINT32_C(0xfffffffe);
		CHECK(store_fat(fixture, (uint32_t)expected.values[0], (uint32_t)expected.values[1]) == 0);
		break;
	case MISSING_BITMAP:
		root[0] = 0;
		expected.code = EXFAT_RESIZE_EVENT_CODE_BITMAP_MISSING;
		expected.values[0] = 2;
		break;
	case DUPLICATE_BITMAP:
		memcpy(root + 448, root, 32);
		expected.code = EXFAT_RESIZE_EVENT_CODE_DIRECTORY_ENTRY_INVALID;
		expected.values[0] = 2;
		expected.values[1] = 448;
		expected.values[2] = EXFAT_RESIZE_DIRECTORY_DUPLICATE_BITMAP;
		break;
	case SHORT_BITMAP:
		CHECK(exfat_resize_store_le64(root, 32, 24, bitmap_length - 1) == EXFAT_RESIZE_SUCCESS);
		expected.code = EXFAT_RESIZE_EVENT_CODE_BITMAP_TOO_SHORT;
		expected.values[0] = 3;
		expected.values[1] = bitmap_length - 1;
		expected.values[2] = bitmap_length;
		break;
	case BITMAP_FLAGS:
		root[1] = 1;
		expected.code = EXFAT_RESIZE_EVENT_CODE_DIRECTORY_ENTRY_INVALID;
		expected.values[0] = 2;
		expected.values[2] = EXFAT_RESIZE_DIRECTORY_ENTRY_FLAGS;
		break;
	case ENTRY_CHECKSUM:
		file[5] ^= 1;
		expected.values[2] = EXFAT_RESIZE_DIRECTORY_CHECKSUM;
		break;
	case SECONDARY_COUNT:
		file[1] = 1;
		expected.values[2] = EXFAT_RESIZE_DIRECTORY_SECONDARY_COUNT;
		break;
	case ENTRY_ORDER:
		file[32] = 0xc1;
		update_checksum(file);
		expected.values[2] = EXFAT_RESIZE_DIRECTORY_ENTRY_ORDER;
		break;
	case ENTRY_FLAGS:
		file[33] = 0;
		update_checksum(file);
		expected.values[2] = EXFAT_RESIZE_DIRECTORY_ENTRY_FLAGS;
		break;
	case VALID_LENGTH:
		CHECK(exfat_resize_store_le64(file, 96, 40, 513) == EXFAT_RESIZE_SUCCESS);
		update_checksum(file);
		expected.values[2] = EXFAT_RESIZE_DIRECTORY_VALID_LENGTH;
		break;
	case DIRECTORY_LENGTH:
		CHECK(exfat_resize_store_le64(directory, 96, 40, 511) == EXFAT_RESIZE_SUCCESS);
		update_checksum(directory);
		expected.values[2] = EXFAT_RESIZE_DIRECTORY_LENGTH_MISMATCH;
		break;
	case DIRECTORY_TOO_LARGE:
		CHECK(exfat_resize_store_le64(directory, 96, 40, UINT64_C(256) * 1024 * 1024 + 512) ==
		    EXFAT_RESIZE_SUCCESS);
		CHECK(exfat_resize_store_le64(directory, 96, 56, UINT64_C(256) * 1024 * 1024 + 512) ==
		    EXFAT_RESIZE_SUCCESS);
		update_checksum(directory);
		expected.values[2] = EXFAT_RESIZE_DIRECTORY_TOO_LARGE;
		break;
	case EMPTY_NO_FAT_CHAIN:
		CHECK(exfat_resize_store_le64(file, 96, 40, 0) == EXFAT_RESIZE_SUCCESS);
		CHECK(exfat_resize_store_le64(file, 96, 56, 0) == EXFAT_RESIZE_SUCCESS);
		update_checksum(file);
		expected.values[2] = EXFAT_RESIZE_DIRECTORY_EMPTY_NO_FAT_CHAIN;
		break;
	case MISPLACED_SYSTEM_ENTRY: {
		unsigned char child[SECTOR_SIZE] = { 0 };
		memcpy(child, root, 32);
		CHECK(fixture->memory.device.write(fixture->memory.device.context,
		          exfat_fixture_cluster_sector(&fixture->geometry, 6), 1, child) == 0);
		expected.code = EXFAT_RESIZE_EVENT_CODE_DIRECTORY_ENTRY_INVALID;
		expected.values[0] = 6;
		expected.values[2] = EXFAT_RESIZE_DIRECTORY_MISPLACED_SYSTEM_ENTRY;
		break;
	}
	case TRUNCATED_SET:
		CHECK(exfat_resize_store_le64(directory, 96, 40, 32) == EXFAT_RESIZE_SUCCESS);
		CHECK(exfat_resize_store_le64(directory, 96, 56, 32) == EXFAT_RESIZE_SUCCESS);
		update_checksum(directory);
		expected.code = EXFAT_RESIZE_EVENT_CODE_DIRECTORY_ENTRY_INVALID;
		expected.values[0] = 6;
		expected.values[2] = EXFAT_RESIZE_DIRECTORY_TRUNCATED_SET;
		break;
	case UNSUPPORTED_ENTRY:
		root[448] = 0x84;
		expected.error = EXFAT_RESIZE_UNSUPPORTED_CRITICAL_ENTRY;
		expected.code = EXFAT_RESIZE_EVENT_CODE_DIRECTORY_ENTRY_UNSUPPORTED;
		expected.values[0] = 2;
		expected.values[1] = 448;
		expected.values[2] = 0x84;
		break;
	case MUTATION_COUNT:
		CHECK(0);
		break;
	}
	if (expected.code == 0) {
		expected.code = EXFAT_RESIZE_EVENT_CODE_DIRECTORY_ENTRY_INVALID;
		expected.values[0] = 2;
		expected.values[1] =
		    mutation == DIRECTORY_LENGTH || mutation == DIRECTORY_TOO_LARGE ? 160 : 64;
	}
	CHECK(fixture->memory.device.write(fixture->memory.device.context, root_sector, 1, root) == 0);
	memory_block_device_clear_operations(&fixture->memory);
	return expected;
}

static void check_read_only(const struct memory_block_device *memory)
{
	size_t index;
	for (index = 0; index < memory->operation_count; ++index)
		CHECK(memory->operations[index].kind == MEMORY_OPERATION_READ);
}

static void check_failure(
    const struct observation *observed, const struct expected_failure *expected)
{
	size_t index;
	CHECK(observed->stage_count == 1);
	CHECK(observed->failure_count == 1);
	CHECK(observed->failure.code == expected->code);
	for (index = 0; index < 3; ++index)
		CHECK(observed->failure.values[index] == expected->values[index]);
}

static void test_validation_diagnostics(void)
{
	static const uint64_t targets[] = { GROW_SECTORS, SHRINK_SECTORS };
	size_t target_index;
	int mutation;
	for (target_index = 0; target_index < sizeof(targets) / sizeof(targets[0]); ++target_index) {
		for (mutation = 0; mutation < MUTATION_COUNT; ++mutation) {
			struct exfat_fixture fixture;
			struct test_allocator allocator = { 0 };
			struct exfat_resize_allocator callbacks = test_allocator_callbacks(&allocator);
			struct observation observed = { .cancel_on_error = 1 };
			struct exfat_resize_monitor monitor = { .context = &observed,
				.cancellation_requested = cancellation_requested,
				.report_event = observe };
			struct expected_failure expected;
			enum exfat_resize_stage stage = EXFAT_RESIZE_STAGE_COMPLETED;
			enum exfat_resize_error result;
			int failures_before = failure_count;
			CHECK(exfat_fixture_initialize(&fixture, GROW_SECTORS) == 0);
			expected = mutate(&fixture, (enum mutation)mutation);
			result = exfat_fixture_resize_with_monitor(
			    &fixture.memory.device, targets[target_index], &callbacks, &monitor, &stage);
			CHECK(result == expected.error);
			CHECK(stage == EXFAT_RESIZE_STAGE_PREFLIGHT);
			check_failure(&observed, &expected);
			CHECK(observed.cancelled);
			CHECK(observed.cancellation_calls == observed.cancellation_calls_at_failure);
			check_read_only(&fixture.memory);
			CHECK(test_allocator_is_clean(&allocator));
			CHECK(exfat_fixture_destroy(&fixture) == 0);
			if (failure_count != failures_before)
				fprintf(stderr, "diagnostic mutation %d, target %llu\n", mutation,
				    (unsigned long long)targets[target_index]);
		}
	}
}

static void test_monitor_does_not_change_failure_work(void)
{
	struct exfat_fixture baseline;
	struct exfat_fixture monitored;
	struct test_allocator baseline_allocator = { 0 };
	struct test_allocator monitored_allocator = { 0 };
	struct exfat_resize_allocator baseline_callbacks =
	    test_allocator_callbacks(&baseline_allocator);
	struct exfat_resize_allocator monitored_callbacks =
	    test_allocator_callbacks(&monitored_allocator);
	struct observation observed = { 0 };
	struct exfat_resize_monitor monitor = { .context = &observed, .report_event = observe };
	struct expected_failure expected;
	enum exfat_resize_stage stage;
	size_t index;
	CHECK(exfat_fixture_initialize(&baseline, GROW_SECTORS) == 0);
	CHECK(exfat_fixture_initialize(&monitored, GROW_SECTORS) == 0);
	(void)mutate(&baseline, UNREFERENCED);
	expected = mutate(&monitored, UNREFERENCED);
	CHECK(exfat_fixture_resize(&baseline.memory.device, GROW_SECTORS, &baseline_callbacks,
	          &stage) == EXFAT_RESIZE_INVALID_FILESYSTEM);
	CHECK(exfat_fixture_resize_with_monitor(&monitored.memory.device, GROW_SECTORS,
	          &monitored_callbacks, &monitor, &stage) == EXFAT_RESIZE_INVALID_FILESYSTEM);
	check_failure(&observed, &expected);
	CHECK(baseline.memory.operation_count == monitored.memory.operation_count);
	for (index = 0;
	    index < baseline.memory.operation_count && index < monitored.memory.operation_count;
	    ++index) {
		CHECK(baseline.memory.operations[index].kind == monitored.memory.operations[index].kind);
		CHECK(baseline.memory.operations[index].first_sector ==
		    monitored.memory.operations[index].first_sector);
		CHECK(baseline.memory.operations[index].sector_count ==
		    monitored.memory.operations[index].sector_count);
	}
	CHECK(baseline_allocator.allocation_attempts == monitored_allocator.allocation_attempts);
	CHECK(baseline_allocator.largest_requested_size == monitored_allocator.largest_requested_size);
	CHECK(test_allocator_is_clean(&baseline_allocator));
	CHECK(test_allocator_is_clean(&monitored_allocator));
	CHECK(exfat_fixture_destroy(&baseline) == 0);
	CHECK(exfat_fixture_destroy(&monitored) == 0);
}

/* Read-ahead can find a broken link before the requested directory entry is read.
 * Preserve the chain diagnostic instead of inventing a truncated-entry failure. */
static void test_stream_read_ahead_diagnostic(void)
{
	struct exfat_fixture fixture;
	struct test_allocator allocator = { 0 };
	struct observation observed = { 0 };
	struct resize_operation operation = { .allocator = test_allocator_callbacks(&allocator),
		.monitor = { .context = &observed, .report_event = observe } };
	struct resize_volume volume = { .operation = &operation };
	struct resize_allocation allocation = { .volume = &volume };
	struct allocation_stream stream = { .first_cluster = 6, .root_directory = 1 };
	struct stream_cursor cursor;
	struct stream_chain_reader chain;
	unsigned char entry[32];
	CHECK(exfat_fixture_initialize(&fixture, GROW_SECTORS) == 0);
	CHECK(store_fat(&fixture, 6, 0) == 0);
	CHECK(exfat_resize_open_volume(&volume, &fixture.memory.device) == EXFAT_RESIZE_SUCCESS);
	volume.geometry = fixture.geometry;
	volume.cluster_size = SECTOR_SIZE;
	CHECK(exfat_resize_allocate_volume_caches(&volume) == EXFAT_RESIZE_SUCCESS);
	CHECK(exfat_resize_load_source_fat(&allocation) == EXFAT_RESIZE_SUCCESS);
	chain = exfat_resize_source_chain(&allocation);
	CHECK(exfat_resize_initialize_stream_cursor(&volume.geometry, &stream,
	          SECTOR_CACHE_SOURCE_DIRECTORY_DATA, &chain, &cursor) == EXFAT_RESIZE_SUCCESS);
	memory_block_device_clear_operations(&fixture.memory);
	CHECK(exfat_resize_read_stream(&volume, &cursor, entry, sizeof(entry)) ==
	    EXFAT_RESIZE_INVALID_FILESYSTEM);
	CHECK(observed.failure_count == 1);
	CHECK(observed.failure.code == EXFAT_RESIZE_EVENT_CODE_FAT_CHAIN_INVALID);
	CHECK(observed.failure.values[0] == 6);
	CHECK(observed.failure.values[1] == 0);
	CHECK(observed.failure.values[2] == EXFAT_RESIZE_CHAIN_INVALID_LINK);
	check_read_only(&fixture.memory);
	exfat_resize_release_source_fat(&allocation);
	exfat_resize_close_volume(&volume);
	CHECK(test_allocator_is_clean(&allocator));
	CHECK(exfat_fixture_destroy(&fixture) == 0);
}

static void test_growth_claim_reports_source_cluster(void)
{
	struct exfat_fixture fixture;
	struct exfat_resize_device_geometry device = { .logical_sector_size = SECTOR_SIZE,
		.sector_count = GROW_SECTORS };
	struct exfat_resize_geometry target;
	uint32_t mapped = 0;
	CHECK(exfat_fixture_initialize(&fixture, GROW_SECTORS) == 0);
	CHECK(exfat_resize_plan_growth(&device, &fixture.geometry, GROW_SECTORS, &target) ==
	    EXFAT_RESIZE_SUCCESS);
	CHECK(exfat_resize_map_growth_cluster(&fixture.geometry, &target, 10, &mapped) ==
	    EXFAT_RESIZE_SUCCESS);
	CHECK(mapped != 10);
	/* REPEATED_CLUSTER above asserts 10 rather than this mapped model index. */
	CHECK(exfat_fixture_destroy(&fixture) == 0);
}

int main(void)
{
	test_validation_diagnostics();
	test_monitor_does_not_change_failure_work();
	test_stream_read_ahead_diagnostic();
	test_growth_claim_reports_source_cluster();
	if (failure_count != 0) {
		fprintf(stderr, "%d diagnostic test(s) failed\n", failure_count);
		return 1;
	}
	printf("library diagnostics: passed\n");
	return 0;
}
