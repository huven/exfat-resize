/* SPDX-License-Identifier: MIT */
#include "boot_region.h"
#include "endian.h"
#include "exfat_resize.h"
#include "support/shrink_fixture.h"
#include "support/test_allocator.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(x) \
	do { \
		if (!(x)) { \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); \
			++failures; \
		} \
	} while (0)

struct monitor_state {
	struct memory_block_device *memory;
	struct test_allocator *allocator;
	size_t cancel_at;
	size_t allocations_before_writes;
	size_t stage_start[6];
	int validate_only;
	int cancel;
};

static int cancelled(void *opaque)
{
	struct monitor_state *state = opaque;
	return state->cancel || state->memory->operation_index >= state->cancel_at;
}

static void event(void *opaque, const struct exfat_resize_event *value)
{
	struct monitor_state *state = opaque;
	if (value->code != EXFAT_RESIZE_EVENT_CODE_STAGE_ENTERED)
		return;
	CHECK(value->values[0] < 6);
	state->stage_start[value->values[0]] = state->memory->operation_index;
	if (value->values[0] == EXFAT_RESIZE_STAGE_PREPARING) {
		state->allocations_before_writes = state->allocator->allocation_attempts;
		if (state->validate_only)
			state->cancel = 1;
	}
}

static enum exfat_resize_error resize(
    struct exfat_fixture *f, struct monitor_state *state, enum exfat_resize_stage *stage)
{
	struct exfat_resize_allocator callbacks = test_allocator_callbacks(state->allocator);
	struct exfat_resize_monitor monitor = {
		.context = state, .cancellation_requested = cancelled, .report_event = event
	};
	enum exfat_resize_error error = exfat_fixture_resize_with_monitor(
	    &f->memory.device, shrink_fixture_target(f), &callbacks, &monitor, stage);
	CHECK(test_allocator_is_clean(state->allocator));
	if (*stage != EXFAT_RESIZE_STAGE_PREFLIGHT)
		CHECK(state->allocator->allocation_attempts == state->allocations_before_writes);
	return error;
}

static void verify_allocation(struct exfat_fixture *f, uint32_t clusters)
{
	struct test_allocator allocator = { 0 };
	struct exfat_resize_allocator callbacks = test_allocator_callbacks(&allocator);
	struct monitor_state state = {
		.memory = &f->memory, .allocator = &allocator, .cancel_at = SIZE_MAX, .validate_only = 1
	};
	struct exfat_resize_monitor monitor = {
		.context = &state, .cancellation_requested = cancelled, .report_event = event
	};
	enum exfat_resize_stage stage;
	uint64_t target = f->geometry.cluster_heap_offset +
	    (uint64_t)(clusters - 1) * f->geometry.sectors_per_cluster;
	CHECK(exfat_fixture_resize_with_monitor(
	          &f->memory.device, target, &callbacks, &monitor, &stage) == EXFAT_RESIZE_CANCELLED);
	CHECK(stage == EXFAT_RESIZE_STAGE_PREPARING);
	CHECK(test_allocator_is_clean(&allocator));
}

static void test_transactions(uint32_t spc)
{
	struct exfat_fixture reference;
	struct test_allocator allocator = { 0 };
	struct monitor_state baseline = {
		.memory = &reference.memory, .allocator = &allocator, .cancel_at = SIZE_MAX
	};
	enum exfat_resize_stage stage;
	size_t count;
	size_t index;
	int mode;
	int saw_partial = 0;
	int saw_source_ready = 0;
	int saw_commit_completion = 0;
	CHECK(shrink_fixture_initialize(&reference, spc) == 0);
	CHECK(resize(&reference, &baseline, &stage) == EXFAT_RESIZE_SUCCESS);
	count = reference.memory.operation_count;

	/* Every callback may fail before or after side effects. Multi-sector callbacks
	 * also fail after a strict prefix, including directory entry-set publication. */
	for (index = 0; index < count; ++index) {
		const struct memory_operation operation = reference.memory.operations[index];
		for (mode = 0; mode < 3; ++mode) {
			struct exfat_fixture f;
			struct test_allocator a = { 0 };
			struct monitor_state state = {
				.memory = &f.memory, .allocator = &a, .cancel_at = SIZE_MAX
			};
			size_t k;
			unsigned char boot[512];
			uint16_t flags = 0;
			if (mode == 2 &&
			    (operation.kind == MEMORY_OPERATION_SYNC || operation.sector_count < 2))
				continue;
			CHECK(shrink_fixture_initialize(&f, spc) == 0);
			if (mode == 0)
				memory_block_device_fail_operation(&f.memory, index, EIO);
			else
				memory_block_device_fail_after_operation(&f.memory, index,
				    mode == 1 ? operation.sector_count : operation.sector_count / 2, EIO);
			CHECK(resize(&f, &state, &stage) == EXFAT_RESIZE_IO_ERROR);
			CHECK(f.memory.operation_index == index + 1);
			CHECK(
			    stage != EXFAT_RESIZE_STAGE_SOURCE_READY && stage != EXFAT_RESIZE_STAGE_COMPLETED);
			CHECK(stage ==
			    (index < baseline.stage_start[EXFAT_RESIZE_STAGE_PREPARING]
			            ? EXFAT_RESIZE_STAGE_PREFLIGHT
			            : index < baseline.stage_start[EXFAT_RESIZE_STAGE_RESIZING]
			            ? EXFAT_RESIZE_STAGE_PREPARING
			            : index < baseline.stage_start[EXFAT_RESIZE_STAGE_FINALIZING]
			            ? EXFAT_RESIZE_STAGE_RESIZING
			            : EXFAT_RESIZE_STAGE_FINALIZING));
			if (mode == 2 && operation.kind == MEMORY_OPERATION_WRITE)
				saw_partial = 1;
			memory_block_device_clear_failure(&f.memory);
			/* Persist just the last issued write, rather than assuming all unsynced
			 * writes disappear. This includes partially completed failed writes. */
			for (k = f.memory.operation_count; k > 0; --k) {
				const struct memory_operation *op = &f.memory.operations[k - 1];
				if (op->kind == MEMORY_OPERATION_SYNC)
					break;
				if (op->kind == MEMORY_OPERATION_WRITE) {
					CHECK(memory_block_device_persist_range(&f.memory, op->first_sector, 1) == 0);
					break;
				}
			}
			CHECK(memory_block_device_crash(&f.memory) == 0);
			CHECK(exfat_fixture_read_sector(&f, 0, boot, sizeof(boot)) == 0);
			CHECK(exfat_resize_load_le16(boot, sizeof(boot), 106, &flags) == EXFAT_RESIZE_SUCCESS);
			if (stage == EXFAT_RESIZE_STAGE_RESIZING)
				CHECK((flags & 2) != 0);
			if (stage == EXFAT_RESIZE_STAGE_PREFLIGHT)
				CHECK(shrink_fixture_verify(&f, 10000) == 0);
			CHECK(exfat_fixture_destroy(&f) == 0);
		}
	}
	if (spc > 1)
		CHECK(saw_partial);

	/* Request cancellation after each possible callback. Every return must be
	 * either an untouched/clean source or a fully committed clean target. */
	for (index = 0; index <= count; ++index) {
		struct exfat_fixture f;
		struct test_allocator a = { 0 };
		struct monitor_state state = { .memory = &f.memory, .allocator = &a, .cancel_at = index };
		enum exfat_resize_error error;
		CHECK(shrink_fixture_initialize(&f, spc) == 0);
		error = resize(&f, &state, &stage);
		CHECK(memory_block_device_crash(&f.memory) == 0);
		if (error == EXFAT_RESIZE_SUCCESS) {
			CHECK(stage == EXFAT_RESIZE_STAGE_COMPLETED);
			CHECK(shrink_fixture_verify(&f, 2000) == 0);
			verify_allocation(&f, 2000);
			saw_commit_completion = 1;
		} else {
			CHECK(error == EXFAT_RESIZE_CANCELLED);
			CHECK(stage == EXFAT_RESIZE_STAGE_PREFLIGHT || stage == EXFAT_RESIZE_STAGE_PREPARING ||
			    stage == EXFAT_RESIZE_STAGE_SOURCE_READY);
			CHECK(shrink_fixture_verify(&f, 10000) == 0);
			verify_allocation(&f, 10000);
			if (stage == EXFAT_RESIZE_STAGE_SOURCE_READY) {
				if (!saw_source_ready) {
					state.cancel_at = SIZE_MAX;
					CHECK(resize(&f, &state, &stage) == EXFAT_RESIZE_SUCCESS);
					CHECK(shrink_fixture_verify(&f, 2000) == 0);
				}
				saw_source_ready = 1;
			}
		}
		CHECK(exfat_fixture_destroy(&f) == 0);
	}
	CHECK(saw_source_ready && saw_commit_completion);
	CHECK(exfat_fixture_destroy(&reference) == 0);
}

static void test_cancel_cleanup_failure(void)
{
	struct exfat_fixture f;
	struct test_allocator a = { 0 };
	struct monitor_state state = { .memory = &f.memory, .allocator = &a, .cancel_at = SIZE_MAX };
	enum exfat_resize_stage stage;
	size_t first_move_start;
	size_t end;
	size_t i;
	CHECK(shrink_fixture_initialize(&f, 1) == 0);
	CHECK(resize(&f, &state, &stage) == EXFAT_RESIZE_SUCCESS);
	first_move_start = state.stage_start[EXFAT_RESIZE_STAGE_RESIZING];
	CHECK(exfat_fixture_destroy(&f) == 0);
	CHECK(shrink_fixture_initialize(&f, 1) == 0);
	memset(&a, 0, sizeof(a));
	state.cancel_at = first_move_start;
	CHECK(resize(&f, &state, &stage) == EXFAT_RESIZE_CANCELLED);
	CHECK(stage == EXFAT_RESIZE_STAGE_SOURCE_READY);
	end = f.memory.operation_count;
	CHECK(exfat_fixture_destroy(&f) == 0);
	for (i = first_move_start; i < end; ++i) {
		CHECK(shrink_fixture_initialize(&f, 1) == 0);
		memset(&a, 0, sizeof(a));
		memory_block_device_fail_operation(&f.memory, i, EIO);
		CHECK(resize(&f, &state, &stage) == EXFAT_RESIZE_IO_ERROR);
		CHECK(stage == EXFAT_RESIZE_STAGE_RESIZING);
		CHECK(exfat_fixture_destroy(&f) == 0);
	}
}

int main(void)
{
	test_transactions(1);
	test_transactions(4);
	test_cancel_cleanup_failure();
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
