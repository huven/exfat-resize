/* SPDX-License-Identifier: MIT */

#include "exfat_resize.h"

#include "support/exfat_fixture.h"
#include "support/memory_block_device.h"
#include "support/test_allocator.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { TARGET_SECTOR_COUNT = 65536 };
enum { EVENT_CAPACITY = 8 };
enum { BACKUP_BOOT_REGION = 12 };

#define ALLOCATION_MODEL_ZERO_CHUNK_SIZE UINT32_C(67108864)
#define CLUSTER_CHECKPOINT_INTERVAL UINT32_C(1048576)
#define BITMAP_GENERATION_CHUNK_SIZE ((size_t)(CLUSTER_CHECKPOINT_INTERVAL / 8))
#define IO_BUFFER_SIZE ((size_t)UINT32_C(1048576))
#define LARGE_TARGET_SECTOR_COUNT UINT64_C(20000000)
#define TRANSACTION_SCALE_TARGET_SECTOR_COUNT UINT64_C(1100000)

static int failure_count;

enum monitor_operation_trigger {
	MONITOR_TRIGGER_NONE,
	MONITOR_TRIGGER_READ_SECTOR,
	MONITOR_TRIGGER_WRITE_SECTOR,
	MONITOR_TRIGGER_SYNC_COUNT
};

struct monitor_state {
	struct exfat_resize_event events[EVENT_CAPACITY];
	size_t event_count;
	size_t cancellation_calls;
	const struct memory_block_device *operation_memory;
	enum monitor_operation_trigger operation_trigger;
	uint64_t trigger_sector;
	size_t trigger_sync_count;
	enum exfat_resize_stage request_stage;
	int request_on_stage;
	int cancellation_requested;
	int one_shot_cancellation;
};

struct bitmap_generation_state {
	struct test_allocator allocator;
	const struct memory_block_device *memory;
	unsigned char *io_buffer;
	unsigned char *snapshot;
	uint64_t final_fat_sector;
	int saw_generation_start;
	int buffer_changed;
};

#define CHECK(expression) \
	do { \
		if (!(expression)) { \
			fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expression); \
			++failure_count; \
		} \
	} while (0)

static size_t operation_kind_count(
    const struct memory_block_device *memory, enum memory_operation_kind kind)
{
	size_t count = 0;
	size_t index;

	for (index = 0; index < memory->operation_count; ++index) {
		if (memory->operations[index].kind == kind)
			++count;
	}
	return count;
}

static int monitor_operation_triggered(const struct monitor_state *state)
{
	const struct memory_block_device *memory = state->operation_memory;
	const struct memory_operation *operation;

	if (memory == NULL || state->operation_trigger == MONITOR_TRIGGER_NONE)
		return 0;
	if (state->operation_trigger == MONITOR_TRIGGER_SYNC_COUNT)
		return operation_kind_count(memory, MEMORY_OPERATION_SYNC) >= state->trigger_sync_count;
	if (memory->operation_count == 0)
		return 0;
	operation = &memory->operations[memory->operation_count - 1];
	return operation->kind ==
	    (state->operation_trigger == MONITOR_TRIGGER_READ_SECTOR ? MEMORY_OPERATION_READ
	                                                             : MEMORY_OPERATION_WRITE) &&
	    state->trigger_sector >= operation->first_sector &&
	    state->trigger_sector - operation->first_sector < operation->sector_count;
}

static int cancellation_callback(void *context)
{
	struct monitor_state *state = context;
	int requested;

	++state->cancellation_calls;
	requested = state->cancellation_requested || monitor_operation_triggered(state);
	if (requested && state->one_shot_cancellation)
		state->cancellation_requested = 0;
	return requested;
}

static void event_callback(void *context, const struct exfat_resize_event *event)
{
	struct monitor_state *state = context;

	CHECK(event != NULL);
	if (event == NULL)
		return;
	CHECK(state->event_count < EVENT_CAPACITY);
	if (state->event_count < EVENT_CAPACITY)
		state->events[state->event_count++] = *event;
	if (state->request_on_stage && event->code == EXFAT_RESIZE_EVENT_CODE_STAGE_ENTERED &&
	    event->values[0] == (uint64_t)state->request_stage)
		state->cancellation_requested = 1;
}

static struct exfat_resize_monitor resize_monitor(
    struct monitor_state *state, int include_cancellation, int include_events)
{
	struct exfat_resize_monitor monitor = { 0 };

	monitor.context = state;
	if (include_cancellation)
		monitor.cancellation_requested = cancellation_callback;
	if (include_events)
		monitor.report_event = event_callback;
	return monitor;
}

static void check_event_stream(const struct monitor_state *state, size_t expected_count)
{
	size_t index;

	CHECK(state->event_count == expected_count);
	for (index = 0; index < state->event_count; ++index) {
		CHECK(state->events[index].level == EXFAT_RESIZE_EVENT_LEVEL_INFO);
		CHECK(state->events[index].code == EXFAT_RESIZE_EVENT_CODE_STAGE_ENTERED);
		CHECK(state->events[index].values[0] == index);
		if (index != EXFAT_RESIZE_STAGE_COMPLETED)
			CHECK(state->events[index].values[1] == 0);
		CHECK(state->events[index].values[2] == 0);
	}
}

static void check_operation_streams_are_equal(
    const struct memory_block_device *expected, const struct memory_block_device *actual)
{
	size_t index;

	CHECK(expected->operation_count == actual->operation_count);
	if (expected->operation_count != actual->operation_count)
		return;
	for (index = 0; index < expected->operation_count; ++index) {
		CHECK(expected->operations[index].kind == actual->operations[index].kind);
		CHECK(expected->operations[index].first_sector == actual->operations[index].first_sector);
		CHECK(expected->operations[index].sector_count == actual->operations[index].sector_count);
	}
}

static int operation_covers_sector(
    const struct memory_block_device *memory, enum memory_operation_kind kind, uint64_t sector)
{
	size_t index;

	for (index = 0; index < memory->operation_count; ++index) {
		const struct memory_operation *operation = &memory->operations[index];

		if (operation->kind == kind && sector >= operation->first_sector &&
		    sector - operation->first_sector < operation->sector_count)
			return 1;
	}
	return 0;
}

static uint32_t used_fat_sector_count(
    const struct exfat_resize_geometry *geometry, uint32_t sector_size)
{
	uint64_t byte_count = ((uint64_t)geometry->cluster_count + 2) * 4;

	return (uint32_t)((byte_count + sector_size - 1) / sector_size);
}

static void *bitmap_generation_allocate(void *context, size_t size)
{
	struct bitmap_generation_state *state = context;
	void *memory = test_allocator_allocate(&state->allocator, size);

	if (memory != NULL && size == IO_BUFFER_SIZE) {
		CHECK(state->io_buffer == NULL);
		state->io_buffer = memory;
	}
	return memory;
}

static void bitmap_generation_deallocate(void *context, void *memory, size_t size)
{
	struct bitmap_generation_state *state = context;

	if (memory == state->io_buffer)
		CHECK(size == IO_BUFFER_SIZE);
	test_allocator_deallocate(&state->allocator, memory, size);
}

static int cancel_after_first_bitmap_generation_chunk(void *context)
{
	struct bitmap_generation_state *state = context;
	const struct memory_operation *operation;

	if (state->io_buffer == NULL || state->memory->operation_count == 0)
		return 0;
	operation = &state->memory->operations[state->memory->operation_count - 1];
	if (operation->kind != MEMORY_OPERATION_WRITE ||
	    state->final_fat_sector < operation->first_sector ||
	    state->final_fat_sector - operation->first_sector >= operation->sector_count)
		return 0;
	if (!state->saw_generation_start) {
		memcpy(state->snapshot, state->io_buffer, BITMAP_GENERATION_CHUNK_SIZE);
		state->saw_generation_start = 1;
		return 0;
	}
	if (memcmp(state->snapshot, state->io_buffer, BITMAP_GENERATION_CHUNK_SIZE) == 0)
		return 0;
	state->buffer_changed = 1;
	return 1;
}

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
		struct exfat_resize_allocator callbacks = test_allocator_callbacks(&allocator);
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

		error = exfat_fixture_resize(device, TARGET_SECTOR_COUNT, &callbacks, &stage);
		CHECK(error == EXFAT_RESIZE_INVALID_DEVICE);
		CHECK(stage == EXFAT_RESIZE_STAGE_PREFLIGHT);
		CHECK(allocator.allocation_attempts == 0);
		CHECK(allocator.deallocation_calls == 0);
		CHECK(test_allocator_is_clean(&allocator));
		CHECK(memory.operation_count == 0);
		memory_block_device_destroy(&memory);
	}
}

static void test_invalid_allocators(void)
{
	enum invalid_allocator_kind { NULL_ALLOCATOR, MISSING_ALLOCATE, MISSING_DEALLOCATE };
	static const enum invalid_allocator_kind cases[] = {
		NULL_ALLOCATOR,
		MISSING_ALLOCATE,
		MISSING_DEALLOCATE,
	};
	size_t index;

	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
		struct test_allocator allocator = { 0 };
		struct exfat_resize_allocator callbacks = test_allocator_callbacks(&allocator);
		struct memory_block_device memory;
		const struct exfat_resize_allocator *selected_allocator = &callbacks;
		enum exfat_resize_error error;
		enum exfat_resize_stage stage = EXFAT_RESIZE_STAGE_COMPLETED;

		memory_block_device_init(&memory, 512, TARGET_SECTOR_COUNT);
		switch (cases[index]) {
		case NULL_ALLOCATOR:
			selected_allocator = NULL;
			break;
		case MISSING_ALLOCATE:
			callbacks.allocate = NULL;
			break;
		case MISSING_DEALLOCATE:
			callbacks.deallocate = NULL;
			break;
		}

		error =
		    exfat_fixture_resize(&memory.device, TARGET_SECTOR_COUNT, selected_allocator, &stage);
		CHECK(error == EXFAT_RESIZE_INVALID_ARGUMENT);
		CHECK(stage == EXFAT_RESIZE_STAGE_PREFLIGHT);
		CHECK(allocator.allocation_attempts == 0);
		CHECK(allocator.deallocation_calls == 0);
		CHECK(test_allocator_is_clean(&allocator));
		CHECK(memory.operation_count == 0);
		memory_block_device_destroy(&memory);
	}
}

static void test_invalid_arguments_do_not_invoke_monitor(void)
{
	struct test_allocator allocator = { 0 };
	struct exfat_resize_allocator callbacks = test_allocator_callbacks(&allocator);
	struct monitor_state state = { 0 };
	struct exfat_resize_monitor monitor = resize_monitor(&state, 1, 1);
	struct memory_block_device memory;
	enum exfat_resize_error error;
	enum exfat_resize_stage stage;

	memory_block_device_init(&memory, 512, TARGET_SECTOR_COUNT);
	stage = EXFAT_RESIZE_STAGE_COMPLETED;
	error = exfat_resize(
	    NULL, exfat_fixture_target_size(TARGET_SECTOR_COUNT), &callbacks, &monitor, &stage);
	CHECK(error == EXFAT_RESIZE_INVALID_DEVICE);
	CHECK(stage == EXFAT_RESIZE_STAGE_PREFLIGHT);

	stage = EXFAT_RESIZE_STAGE_COMPLETED;
	error = exfat_resize(&memory.device, 0, &callbacks, &monitor, &stage);
	CHECK(error == EXFAT_RESIZE_INVALID_ARGUMENT);
	CHECK(stage == EXFAT_RESIZE_STAGE_PREFLIGHT);

	stage = EXFAT_RESIZE_STAGE_COMPLETED;
	error = exfat_resize(
	    &memory.device, exfat_fixture_target_size(TARGET_SECTOR_COUNT), NULL, &monitor, &stage);
	CHECK(error == EXFAT_RESIZE_INVALID_ARGUMENT);
	CHECK(stage == EXFAT_RESIZE_STAGE_PREFLIGHT);

	CHECK(state.event_count == 0);
	CHECK(state.cancellation_calls == 0);
	CHECK(allocator.allocation_attempts == 0);
	CHECK(memory.operation_count == 0);
	memory_block_device_destroy(&memory);
}

static void test_invalid_targets(void)
{
	enum target_kind {
		ONE_BYTE_TARGET,
		EQUAL_TARGET,
		SMALLER_TARGET,
		INSUFFICIENT_GROWTH_TARGET,
		OVERSIZED_TARGET,
	};
	static const enum target_kind cases[] = {
		ONE_BYTE_TARGET,
		EQUAL_TARGET,
		SMALLER_TARGET,
		INSUFFICIENT_GROWTH_TARGET,
		OVERSIZED_TARGET,
	};
	size_t index;

	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
		struct test_allocator allocator = { 0 };
		struct exfat_resize_allocator callbacks = test_allocator_callbacks(&allocator);
		struct exfat_fixture fixture;
		enum exfat_resize_error error;
		enum exfat_resize_error expected = EXFAT_RESIZE_INTERNAL_ERROR;
		enum exfat_resize_stage stage = EXFAT_RESIZE_STAGE_COMPLETED;
		uint64_t target_size = 0;

		CHECK(exfat_fixture_initialize_with_sectors_per_cluster(&fixture, TARGET_SECTOR_COUNT,
		          cases[index] == INSUFFICIENT_GROWTH_TARGET ? 8 : 1) == 0);
		CHECK(fixture.geometry.fat_length > 1);
		switch (cases[index]) {
		case ONE_BYTE_TARGET:
			target_size = 1;
			expected = EXFAT_RESIZE_INVALID_ARGUMENT;
			break;
		case EQUAL_TARGET:
			target_size = exfat_fixture_target_size(fixture.geometry.volume_sector_count);
			expected = EXFAT_RESIZE_INVALID_ARGUMENT;
			break;
		case SMALLER_TARGET:
			target_size = exfat_fixture_target_size(fixture.geometry.volume_sector_count - 1);
			expected = EXFAT_RESIZE_INVALID_ARGUMENT;
			break;
		case INSUFFICIENT_GROWTH_TARGET:
			target_size = exfat_fixture_target_size(fixture.geometry.volume_sector_count + 1);
			expected = EXFAT_RESIZE_INSUFFICIENT_GROWTH;
			break;
		case OVERSIZED_TARGET:
			target_size = exfat_fixture_target_size(fixture.memory.device.sector_count + 1);
			expected = EXFAT_RESIZE_OUT_OF_BOUNDS;
			break;
		}

		error = exfat_resize(&fixture.memory.device, target_size, &callbacks, NULL, &stage);
		CHECK(error == expected);
		CHECK(stage == EXFAT_RESIZE_STAGE_PREFLIGHT);
		CHECK(allocator.allocation_attempts == 1);
		CHECK(allocator.deallocation_calls == allocator.successful_allocations);
		CHECK(test_allocator_is_clean(&allocator));
		if (cases[index] == OVERSIZED_TARGET)
			CHECK(fixture.memory.operation_count == 1);
		for (size_t operation = 0; operation < fixture.memory.operation_count; ++operation) {
			const struct memory_operation *record = &fixture.memory.operations[operation];

			CHECK(record->kind == MEMORY_OPERATION_READ);
			CHECK(record->first_sector + record->sector_count <= fixture.geometry.fat_offset ||
			    record->first_sector >=
			        (uint64_t)fixture.geometry.fat_offset + fixture.geometry.fat_length);
		}
		exfat_fixture_destroy(&fixture);
	}
}

static void test_preflight_io_error(void)
{
	struct test_allocator allocator = { 0 };
	struct exfat_resize_allocator callbacks = test_allocator_callbacks(&allocator);
	struct exfat_fixture fixture;
	enum exfat_resize_error error;
	enum exfat_resize_stage stage = EXFAT_RESIZE_STAGE_COMPLETED;

	CHECK(exfat_fixture_initialize(&fixture, TARGET_SECTOR_COUNT) == 0);
	memory_block_device_fail_operation(&fixture.memory, 0, 1234);
	error = exfat_fixture_resize(&fixture.memory.device, TARGET_SECTOR_COUNT, &callbacks, &stage);
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
	struct exfat_resize_allocator callbacks = test_allocator_callbacks(&allocator);
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

	error = exfat_resize(&memory.device, memory.device.sector_count * memory.device.sector_size,
	    &callbacks, NULL, &stage);
	CHECK(error == EXFAT_RESIZE_UNSUPPORTED_SECTOR_MAPPING);
	CHECK(stage == EXFAT_RESIZE_STAGE_PREFLIGHT);
	CHECK(allocator.allocation_attempts == 1);
	CHECK(allocator.deallocation_calls == 1);
	CHECK(test_allocator_is_clean(&allocator));
	CHECK(memory.operation_count == 1);
	CHECK(memory.operations[0].kind == MEMORY_OPERATION_READ);
	memory_block_device_destroy(&memory);
}

static void test_monitor_event_stream_and_disabled_behavior(void)
{
	struct test_allocator baseline_allocator = { 0 };
	struct test_allocator empty_allocator = { 0 };
	struct test_allocator reported_allocator = { 0 };
	struct test_allocator polled_allocator = { 0 };
	struct exfat_resize_allocator baseline_callbacks =
	    test_allocator_callbacks(&baseline_allocator);
	struct exfat_resize_allocator empty_callbacks = test_allocator_callbacks(&empty_allocator);
	struct exfat_resize_allocator reported_callbacks =
	    test_allocator_callbacks(&reported_allocator);
	struct exfat_resize_allocator polled_callbacks = test_allocator_callbacks(&polled_allocator);
	struct monitor_state empty_state = { 0 };
	struct monitor_state reported_state = { 0 };
	struct monitor_state polled_state = { 0 };
	struct exfat_resize_monitor empty_monitor = resize_monitor(&empty_state, 0, 0);
	struct exfat_resize_monitor reported_monitor = resize_monitor(&reported_state, 0, 1);
	struct exfat_resize_monitor polled_monitor = resize_monitor(&polled_state, 1, 0);
	struct exfat_fixture baseline;
	struct exfat_fixture empty;
	struct exfat_fixture reported;
	struct exfat_fixture polled;
	enum exfat_resize_error error;
	enum exfat_resize_stage stage;
	uint64_t target_size = exfat_fixture_target_size(TARGET_SECTOR_COUNT);

	CHECK(exfat_fixture_initialize(&baseline, TARGET_SECTOR_COUNT) == 0);
	CHECK(exfat_fixture_initialize(&empty, TARGET_SECTOR_COUNT) == 0);
	CHECK(exfat_fixture_initialize(&reported, TARGET_SECTOR_COUNT) == 0);
	CHECK(exfat_fixture_initialize(&polled, TARGET_SECTOR_COUNT) == 0);
	error = exfat_fixture_resize(
	    &baseline.memory.device, TARGET_SECTOR_COUNT, &baseline_callbacks, NULL);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	error = exfat_fixture_resize_with_monitor(
	    &empty.memory.device, TARGET_SECTOR_COUNT, &empty_callbacks, &empty_monitor, NULL);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	stage = EXFAT_RESIZE_STAGE_PREFLIGHT;
	error = exfat_resize(
	    &reported.memory.device, target_size + 511, &reported_callbacks, &reported_monitor, &stage);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	CHECK(stage == EXFAT_RESIZE_STAGE_COMPLETED);
	error = exfat_fixture_resize_with_monitor(
	    &polled.memory.device, TARGET_SECTOR_COUNT, &polled_callbacks, &polled_monitor, NULL);
	CHECK(error == EXFAT_RESIZE_SUCCESS);

	CHECK(empty_state.event_count == 0);
	CHECK(empty_state.cancellation_calls == 0);
	check_event_stream(&reported_state, 5);
	if (reported_state.event_count == 5)
		CHECK(reported_state.events[4].values[1] == target_size);
	CHECK(reported_state.cancellation_calls == 0);
	CHECK(polled_state.event_count == 0);
	CHECK(polled_state.cancellation_calls != 0);
	check_operation_streams_are_equal(&baseline.memory, &empty.memory);
	check_operation_streams_are_equal(&baseline.memory, &reported.memory);
	check_operation_streams_are_equal(&baseline.memory, &polled.memory);
	CHECK(test_allocator_is_clean(&baseline_allocator));
	CHECK(test_allocator_is_clean(&empty_allocator));
	CHECK(test_allocator_is_clean(&reported_allocator));
	CHECK(test_allocator_is_clean(&polled_allocator));
	exfat_fixture_destroy(&baseline);
	exfat_fixture_destroy(&empty);
	exfat_fixture_destroy(&reported);
	exfat_fixture_destroy(&polled);
}

static void test_cache_window_turnover_cancellation(void)
{
	struct test_allocator allocator = { 0 };
	struct exfat_resize_allocator callbacks = test_allocator_callbacks(&allocator);
	struct exfat_fixture fixture;
	struct monitor_state state = { 0 };
	struct exfat_resize_monitor monitor = resize_monitor(&state, 1, 0);
	enum exfat_resize_error error;
	enum exfat_resize_stage stage = EXFAT_RESIZE_STAGE_COMPLETED;
	uint64_t child_sector;
	uint64_t root_sector;

	CHECK(exfat_fixture_initialize(&fixture, TARGET_SECTOR_COUNT) == 0);
	root_sector =
	    exfat_fixture_cluster_sector(&fixture.geometry, fixture.geometry.root_directory_cluster);
	child_sector = exfat_fixture_cluster_sector(&fixture.geometry, 6);
	state.operation_memory = &fixture.memory;
	state.operation_trigger = MONITOR_TRIGGER_READ_SECTOR;
	state.trigger_sector = root_sector;
	error = exfat_fixture_resize_with_monitor(
	    &fixture.memory.device, TARGET_SECTOR_COUNT, &callbacks, &monitor, &stage);
	CHECK(error == EXFAT_RESIZE_CANCELLED);
	CHECK(stage == EXFAT_RESIZE_STAGE_PREFLIGHT);
	CHECK(operation_covers_sector(&fixture.memory, MEMORY_OPERATION_READ, root_sector));
	CHECK(!operation_covers_sector(&fixture.memory, MEMORY_OPERATION_READ, child_sector));
	CHECK(operation_kind_count(&fixture.memory, MEMORY_OPERATION_WRITE) == 0);
	CHECK(test_allocator_is_clean(&allocator));
	exfat_fixture_destroy(&fixture);
}

static void test_cluster_copy_chunk_boundary_cancellation(void)
{
	struct test_allocator allocator = { 0 };
	struct exfat_resize_allocator callbacks = test_allocator_callbacks(&allocator);
	struct exfat_resize_device_geometry device_geometry;
	struct exfat_resize_geometry target;
	struct exfat_fixture fixture;
	struct monitor_state state = { 0 };
	struct exfat_resize_monitor monitor = resize_monitor(&state, 1, 0);
	enum exfat_resize_error error;
	enum exfat_resize_stage stage = EXFAT_RESIZE_STAGE_COMPLETED;
	uint64_t source_sector;
	uint64_t target_sector;
	uint32_t source_cluster;
	uint32_t target_cluster;

	CHECK(exfat_fixture_initialize(&fixture, TARGET_SECTOR_COUNT) == 0);
	device_geometry.logical_sector_size = fixture.memory.device.sector_size;
	device_geometry.sector_count = fixture.memory.device.sector_count;
	error =
	    exfat_resize_plan_growth(&device_geometry, &fixture.geometry, TARGET_SECTOR_COUNT, &target);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	source_cluster = fixture.crossing_first_cluster;
	error = exfat_resize_map_growth_cluster(
	    &fixture.geometry, &target, source_cluster, &target_cluster);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	source_sector = exfat_fixture_cluster_sector(&fixture.geometry, source_cluster);
	target_sector = exfat_fixture_cluster_sector(&target, target_cluster);
	state.operation_memory = &fixture.memory;
	state.operation_trigger = MONITOR_TRIGGER_WRITE_SECTOR;
	state.trigger_sector = target_sector;
	error = exfat_fixture_resize_with_monitor(
	    &fixture.memory.device, TARGET_SECTOR_COUNT, &callbacks, &monitor, &stage);
	CHECK(error == EXFAT_RESIZE_CANCELLED);
	CHECK(stage == EXFAT_RESIZE_STAGE_PREPARING);
	CHECK(operation_covers_sector(&fixture.memory, MEMORY_OPERATION_READ, source_sector));
	CHECK(operation_covers_sector(&fixture.memory, MEMORY_OPERATION_WRITE, target_sector));
	CHECK(test_allocator_is_clean(&allocator));
	exfat_fixture_destroy(&fixture);
}

static void test_target_fat_buffer_boundary_cancellation(void)
{
	struct test_allocator allocator = { 0 };
	struct exfat_resize_allocator callbacks = test_allocator_callbacks(&allocator);
	struct exfat_resize_device_geometry device_geometry;
	struct exfat_resize_geometry target;
	struct exfat_fixture fixture;
	struct monitor_state state = { 0 };
	struct exfat_resize_monitor monitor = resize_monitor(&state, 1, 0);
	enum exfat_resize_error error;
	enum exfat_resize_stage stage = EXFAT_RESIZE_STAGE_COMPLETED;
	uint32_t fat_sector_capacity;
	uint32_t fat_sector_count;
	int fixture_result;

	fixture_result = exfat_fixture_initialize(&fixture, TRANSACTION_SCALE_TARGET_SECTOR_COUNT);
	CHECK(fixture_result == 0);
	if (fixture_result != 0)
		return;
	device_geometry.logical_sector_size = fixture.memory.device.sector_size;
	device_geometry.sector_count = fixture.memory.device.sector_count;
	error = exfat_resize_plan_growth(&device_geometry, &fixture.geometry,
	    TRANSACTION_SCALE_TARGET_SECTOR_COUNT, &target);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	if (error != EXFAT_RESIZE_SUCCESS)
		goto done;
	fat_sector_capacity = (uint32_t)(IO_BUFFER_SIZE / fixture.memory.device.sector_size);
	fat_sector_count = used_fat_sector_count(&target, fixture.memory.device.sector_size);
	CHECK(fat_sector_count > fat_sector_capacity);
	if (fat_sector_count <= fat_sector_capacity)
		goto done;
	state.operation_memory = &fixture.memory;
	state.operation_trigger = MONITOR_TRIGGER_WRITE_SECTOR;
	state.trigger_sector = target.fat_offset;

	error = exfat_fixture_resize_with_monitor(&fixture.memory.device,
	    TRANSACTION_SCALE_TARGET_SECTOR_COUNT, &callbacks, &monitor, &stage);
	CHECK(error == EXFAT_RESIZE_CANCELLED);
	CHECK(stage == EXFAT_RESIZE_STAGE_RESIZING);
	CHECK(operation_covers_sector(&fixture.memory, MEMORY_OPERATION_WRITE, target.fat_offset));
	CHECK(!operation_covers_sector(
	    &fixture.memory, MEMORY_OPERATION_WRITE, target.fat_offset + fat_sector_capacity));
	CHECK(test_allocator_is_clean(&allocator));

done:
	exfat_fixture_destroy(&fixture);
}

static void test_target_bitmap_generation_boundary_cancellation(void)
{
	struct bitmap_generation_state state = { 0 };
	struct exfat_resize_allocator allocator = {
		.context = &state,
		.allocate = bitmap_generation_allocate,
		.deallocate = bitmap_generation_deallocate,
	};
	struct exfat_resize_monitor monitor = {
		.context = &state,
		.cancellation_requested = cancel_after_first_bitmap_generation_chunk,
	};
	struct exfat_resize_device_geometry device_geometry;
	struct exfat_resize_geometry target;
	struct exfat_fixture fixture;
	enum exfat_resize_error error;
	enum exfat_resize_stage stage = EXFAT_RESIZE_STAGE_COMPLETED;
	uint64_t bitmap_cluster_count;
	uint64_t bitmap_first_sector;
	uint64_t bitmap_length;
	uint64_t cluster_size;
	uint32_t fat_sector_count;
	int fixture_result;

	state.snapshot = malloc(BITMAP_GENERATION_CHUNK_SIZE);
	CHECK(state.snapshot != NULL);
	if (state.snapshot == NULL)
		return;
	fixture_result = exfat_fixture_initialize(&fixture, TRANSACTION_SCALE_TARGET_SECTOR_COUNT);
	CHECK(fixture_result == 0);
	if (fixture_result != 0)
		goto free_snapshot;
	device_geometry.logical_sector_size = fixture.memory.device.sector_size;
	device_geometry.sector_count = fixture.memory.device.sector_count;
	error = exfat_resize_plan_growth(&device_geometry, &fixture.geometry,
	    TRANSACTION_SCALE_TARGET_SECTOR_COUNT, &target);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	if (error != EXFAT_RESIZE_SUCCESS)
		goto destroy_fixture;
	fat_sector_count = used_fat_sector_count(&target, fixture.memory.device.sector_size);
	state.final_fat_sector = target.fat_offset + fat_sector_count - 1;
	state.memory = &fixture.memory;
	bitmap_length = ((uint64_t)target.cluster_count + 7) / 8;
	CHECK(bitmap_length > BITMAP_GENERATION_CHUNK_SIZE);
	if (bitmap_length <= BITMAP_GENERATION_CHUNK_SIZE)
		goto destroy_fixture;
	cluster_size =
	    (uint64_t)fixture.memory.device.sector_size * target.sectors_per_cluster;
	bitmap_cluster_count = (bitmap_length + cluster_size - 1) / cluster_size;
	bitmap_first_sector = target.cluster_heap_offset +
	    (target.cluster_count - bitmap_cluster_count) * target.sectors_per_cluster;

	error = exfat_fixture_resize_with_monitor(&fixture.memory.device,
	    TRANSACTION_SCALE_TARGET_SECTOR_COUNT, &allocator, &monitor, &stage);
	CHECK(error == EXFAT_RESIZE_CANCELLED);
	CHECK(stage == EXFAT_RESIZE_STAGE_RESIZING);
	CHECK(state.saw_generation_start);
	CHECK(state.buffer_changed);
	CHECK(!operation_covers_sector(
	    &fixture.memory, MEMORY_OPERATION_WRITE, bitmap_first_sector));
	CHECK(test_allocator_is_clean(&state.allocator));

destroy_fixture:
	exfat_fixture_destroy(&fixture);
free_snapshot:
	free(state.snapshot);
}

static void test_cancellation_before_boot_region_commit(void)
{
	struct test_allocator allocator = { 0 };
	struct exfat_resize_allocator callbacks = test_allocator_callbacks(&allocator);
	struct exfat_fixture fixture;
	struct monitor_state state = {
		.operation_trigger = MONITOR_TRIGGER_SYNC_COUNT,
		.trigger_sync_count = 2,
	};
	struct exfat_resize_monitor monitor = resize_monitor(&state, 1, 0);
	enum exfat_resize_error error;
	enum exfat_resize_stage stage = EXFAT_RESIZE_STAGE_COMPLETED;

	CHECK(exfat_fixture_initialize(&fixture, TARGET_SECTOR_COUNT) == 0);
	state.operation_memory = &fixture.memory;
	error = exfat_fixture_resize_with_monitor(
	    &fixture.memory.device, TARGET_SECTOR_COUNT, &callbacks, &monitor, &stage);
	CHECK(error == EXFAT_RESIZE_CANCELLED);
	CHECK(stage == EXFAT_RESIZE_STAGE_RESIZING);
	CHECK(operation_kind_count(&fixture.memory, MEMORY_OPERATION_SYNC) == 2);
	CHECK(!operation_covers_sector(&fixture.memory, MEMORY_OPERATION_WRITE, BACKUP_BOOT_REGION));
	CHECK(test_allocator_is_clean(&allocator));
	exfat_fixture_destroy(&fixture);
}

static void test_stage_cancellation(void)
{
	static const struct {
		enum exfat_resize_stage requested_stage;
		enum exfat_resize_error expected_error;
	} cases[] = {
		{ EXFAT_RESIZE_STAGE_PREFLIGHT, EXFAT_RESIZE_CANCELLED },
		{ EXFAT_RESIZE_STAGE_PREPARING, EXFAT_RESIZE_CANCELLED },
		{ EXFAT_RESIZE_STAGE_RESIZING, EXFAT_RESIZE_CANCELLED },
		{ EXFAT_RESIZE_STAGE_FINALIZING, EXFAT_RESIZE_CANCELLED },
		{ EXFAT_RESIZE_STAGE_COMPLETED, EXFAT_RESIZE_SUCCESS },
	};
	size_t index;

	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
		struct test_allocator allocator = { 0 };
		struct exfat_resize_allocator callbacks = test_allocator_callbacks(&allocator);
		struct monitor_state state = {
			.request_stage = cases[index].requested_stage,
			.request_on_stage = 1,
		};
		struct exfat_resize_monitor monitor = resize_monitor(&state, 1, 1);
		struct exfat_fixture fixture;
		enum exfat_resize_error error;
		enum exfat_resize_stage stage = EXFAT_RESIZE_STAGE_COMPLETED;
		size_t operation;

		CHECK(exfat_fixture_initialize(&fixture, TARGET_SECTOR_COUNT) == 0);
		error = exfat_fixture_resize_with_monitor(
		    &fixture.memory.device, TARGET_SECTOR_COUNT, &callbacks, &monitor, &stage);
		CHECK(error == cases[index].expected_error);
		CHECK(stage == cases[index].requested_stage);
		check_event_stream(&state, (size_t)cases[index].requested_stage + 1);
		CHECK(state.cancellation_requested);
		CHECK(state.cancellation_calls != 0);
		if (cases[index].requested_stage == EXFAT_RESIZE_STAGE_PREFLIGHT) {
			CHECK(allocator.allocation_attempts == 0);
			CHECK(fixture.memory.operation_count == 0);
		}
		if (cases[index].requested_stage <= EXFAT_RESIZE_STAGE_PREPARING) {
			for (operation = 0; operation < fixture.memory.operation_count; ++operation)
				CHECK(fixture.memory.operations[operation].kind == MEMORY_OPERATION_READ);
		}
		if (cases[index].requested_stage == EXFAT_RESIZE_STAGE_COMPLETED && state.event_count == 5)
			CHECK(state.events[4].values[1] == exfat_fixture_target_size(TARGET_SECTOR_COUNT));
		CHECK(test_allocator_is_clean(&allocator));
		exfat_fixture_destroy(&fixture);
	}
}

static void test_cancellation_without_event_reporting(void)
{
	struct test_allocator allocator = { 0 };
	struct exfat_resize_allocator callbacks = test_allocator_callbacks(&allocator);
	struct monitor_state state = {
		.cancellation_requested = 1,
		.one_shot_cancellation = 1,
	};
	struct exfat_resize_monitor monitor = resize_monitor(&state, 1, 0);
	struct exfat_fixture fixture;
	enum exfat_resize_error error;
	enum exfat_resize_stage stage = EXFAT_RESIZE_STAGE_COMPLETED;

	CHECK(exfat_fixture_initialize(&fixture, TARGET_SECTOR_COUNT) == 0);
	error = exfat_fixture_resize_with_monitor(
	    &fixture.memory.device, TARGET_SECTOR_COUNT, &callbacks, &monitor, &stage);
	CHECK(error == EXFAT_RESIZE_CANCELLED);
	CHECK(stage == EXFAT_RESIZE_STAGE_PREFLIGHT);
	CHECK(state.cancellation_calls == 1);
	CHECK(!state.cancellation_requested);
	CHECK(state.event_count == 0);
	CHECK(allocator.allocation_attempts == 0);
	CHECK(fixture.memory.operation_count == 0);
	CHECK(test_allocator_is_clean(&allocator));
	exfat_fixture_destroy(&fixture);
}

struct allocation_zero_state {
	struct test_allocator allocator;
	void *allocation_model;
	size_t allocation_model_size;
	int contents_checked;
};

static int buffer_has_value(const unsigned char *buffer, size_t size, unsigned char value)
{
	size_t index;

	for (index = 0; index < size; ++index) {
		if (buffer[index] != value)
			return 0;
	}
	return 1;
}

static void *allocation_zero_allocate(void *context, size_t size)
{
	struct allocation_zero_state *state = context;
	void *memory = test_allocator_allocate(&state->allocator, size);

	if (memory != NULL && size > ALLOCATION_MODEL_ZERO_CHUNK_SIZE) {
		CHECK(state->allocation_model == NULL);
		state->allocation_model = memory;
		state->allocation_model_size = size;
		memset(memory, 0xa5, size);
	}
	return memory;
}

static void allocation_zero_deallocate(void *context, void *memory, size_t size)
{
	struct allocation_zero_state *state = context;

	if (memory == state->allocation_model) {
		const unsigned char *bytes = memory;

		CHECK(size == state->allocation_model_size);
		CHECK(size > ALLOCATION_MODEL_ZERO_CHUNK_SIZE);
		if (size == state->allocation_model_size &&
		    size > ALLOCATION_MODEL_ZERO_CHUNK_SIZE) {
			CHECK(buffer_has_value(bytes, ALLOCATION_MODEL_ZERO_CHUNK_SIZE, 0));
			CHECK(buffer_has_value(bytes + ALLOCATION_MODEL_ZERO_CHUNK_SIZE,
			    size - ALLOCATION_MODEL_ZERO_CHUNK_SIZE, 0xa5));
			state->contents_checked = 1;
		}
	}
	test_allocator_deallocate(&state->allocator, memory, size);
}

static int cancel_after_first_allocation_zero_chunk(void *context)
{
	struct allocation_zero_state *state = context;

	if (state->allocation_model == NULL)
		return 0;
	return *(const unsigned char *)state->allocation_model == 0;
}

static void test_allocation_model_zero_chunk_boundary_cancellation(void)
{
	struct allocation_zero_state state = { 0 };
	struct exfat_resize_allocator allocator = {
		.context = &state,
		.allocate = allocation_zero_allocate,
		.deallocate = allocation_zero_deallocate,
	};
	struct exfat_resize_monitor monitor = {
		.context = &state,
		.cancellation_requested = cancel_after_first_allocation_zero_chunk,
	};
	struct exfat_resize_device_geometry device_geometry;
	struct exfat_resize_geometry target;
	struct exfat_fixture fixture;
	enum exfat_resize_error error;
	enum exfat_resize_stage stage = EXFAT_RESIZE_STAGE_COMPLETED;

	if (SIZE_MAX <= ALLOCATION_MODEL_ZERO_CHUNK_SIZE)
		return;
	CHECK(exfat_fixture_initialize(&fixture, LARGE_TARGET_SECTOR_COUNT) == 0);
	device_geometry.logical_sector_size = fixture.memory.device.sector_size;
	device_geometry.sector_count = fixture.memory.device.sector_count;
	error = exfat_resize_plan_growth(
	    &device_geometry, &fixture.geometry, LARGE_TARGET_SECTOR_COUNT, &target);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	CHECK((uint64_t)target.cluster_count * sizeof(uint32_t) >
	    ALLOCATION_MODEL_ZERO_CHUNK_SIZE);
	CHECK((uint64_t)target.cluster_count * sizeof(uint32_t) <=
	    2 * ALLOCATION_MODEL_ZERO_CHUNK_SIZE);

	error = exfat_fixture_resize_with_monitor(
	    &fixture.memory.device, LARGE_TARGET_SECTOR_COUNT, &allocator, &monitor, &stage);
	CHECK(error == EXFAT_RESIZE_CANCELLED);
	CHECK(stage == EXFAT_RESIZE_STAGE_PREFLIGHT);
	CHECK(state.contents_checked);
	CHECK(operation_kind_count(&fixture.memory, MEMORY_OPERATION_WRITE) == 0);
	CHECK(test_allocator_is_clean(&state.allocator));
	exfat_fixture_destroy(&fixture);
}

enum precedence_failure_kind {
	PRECEDENCE_ALLOCATION_FAILURE,
	PRECEDENCE_READ_FAILURE,
	PRECEDENCE_SOURCE_FAT_READ_FAILURE
};

struct precedence_context {
	struct monitor_state *monitor;
	struct exfat_resize_block_device *device;
	uint64_t failing_read_sector;
	size_t cancellation_calls_at_failure;
	int fail_selected_read;
};

static void *precedence_allocate(void *context, size_t size)
{
	struct precedence_context *precedence = context;

	(void)size;
	precedence->monitor->cancellation_requested = 1;
	return NULL;
}

static void precedence_deallocate(void *context, void *memory, size_t size)
{
	(void)context;
	(void)memory;
	(void)size;
}

static int precedence_read(
    void *context, uint64_t first_sector, uint32_t sector_count, void *buffer)
{
	struct precedence_context *precedence = context;

	if (precedence->fail_selected_read && first_sector != precedence->failing_read_sector)
		return precedence->device->read(
		    precedence->device->context, first_sector, sector_count, buffer);
	precedence->cancellation_calls_at_failure = precedence->monitor->cancellation_calls;
	precedence->monitor->cancellation_requested = 1;
	return 1234;
}

static int precedence_write(
    void *context, uint64_t first_sector, uint32_t sector_count, const void *buffer)
{
	struct precedence_context *precedence = context;

	return precedence->device->write(
	    precedence->device->context, first_sector, sector_count, buffer);
}

static int precedence_sync(void *context)
{
	struct precedence_context *precedence = context;

	return precedence->device->sync(precedence->device->context);
}

static void test_concrete_failures_precede_cancellation(void)
{
	static const enum precedence_failure_kind cases[] = {
		PRECEDENCE_ALLOCATION_FAILURE,
		PRECEDENCE_READ_FAILURE,
		PRECEDENCE_SOURCE_FAT_READ_FAILURE,
	};
	size_t index;

	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
		struct test_allocator allocator = { 0 };
		struct exfat_resize_allocator callbacks = test_allocator_callbacks(&allocator);
		struct monitor_state state = { 0 };
		struct exfat_resize_monitor monitor = resize_monitor(&state, 1, 1);
		struct exfat_fixture fixture;
		struct precedence_context precedence = { 0 };
		struct exfat_resize_block_device device;
		enum exfat_resize_error error;
		enum exfat_resize_error expected_error;
		enum exfat_resize_stage stage = EXFAT_RESIZE_STAGE_COMPLETED;

		CHECK(exfat_fixture_initialize(&fixture, TARGET_SECTOR_COUNT) == 0);
		precedence.monitor = &state;
		precedence.device = &fixture.memory.device;
		device = fixture.memory.device;
		if (cases[index] == PRECEDENCE_ALLOCATION_FAILURE) {
			callbacks.context = &precedence;
			callbacks.allocate = precedence_allocate;
			callbacks.deallocate = precedence_deallocate;
			expected_error = EXFAT_RESIZE_OUT_OF_MEMORY;
		} else {
			if (cases[index] == PRECEDENCE_SOURCE_FAT_READ_FAILURE) {
				precedence.fail_selected_read = 1;
				precedence.failing_read_sector = fixture.geometry.fat_offset;
			}
			device.context = &precedence;
			device.read = precedence_read;
			device.write = precedence_write;
			device.sync = precedence_sync;
			expected_error = EXFAT_RESIZE_IO_ERROR;
		}
		error = exfat_resize(
		    &device, exfat_fixture_target_size(TARGET_SECTOR_COUNT), &callbacks, &monitor, &stage);
		CHECK(error == expected_error);
		CHECK(stage == EXFAT_RESIZE_STAGE_PREFLIGHT);
		CHECK(state.cancellation_requested);
		if (cases[index] == PRECEDENCE_ALLOCATION_FAILURE)
			CHECK(state.cancellation_calls == 1);
		else
			CHECK(state.cancellation_calls == precedence.cancellation_calls_at_failure);
		check_event_stream(&state, 1);
		if (cases[index] != PRECEDENCE_ALLOCATION_FAILURE)
			CHECK(test_allocator_is_clean(&allocator));
		exfat_fixture_destroy(&fixture);
	}
}

int main(void)
{
	test_invalid_devices();
	test_invalid_allocators();
	test_invalid_arguments_do_not_invoke_monitor();
	test_invalid_targets();
	test_preflight_io_error();
	test_unsupported_sector_mapping();
	test_monitor_event_stream_and_disabled_behavior();
	test_cache_window_turnover_cancellation();
	test_cluster_copy_chunk_boundary_cancellation();
	test_target_fat_buffer_boundary_cancellation();
	test_target_bitmap_generation_boundary_cancellation();
	test_cancellation_before_boot_region_commit();
	test_stage_cancellation();
	test_cancellation_without_event_reporting();
	test_allocation_model_zero_chunk_boundary_cancellation();
	test_concrete_failures_precede_cancellation();

	if (failure_count != 0) {
		fprintf(stderr, "%d public API test(s) failed\n", failure_count);
		return 1;
	}
	printf("library public API: passed\n");
	return 0;
}
