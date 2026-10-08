/* SPDX-License-Identifier: MIT */
#include "boot_region.h"
#include "endian.h"
#include "exfat_resize.h"
#include "stream.h"
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
	uint32_t target_clusters;
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
	enum exfat_resize_error error = exfat_fixture_resize_with_monitor(&f->memory.device,
	    f->geometry.cluster_heap_offset +
	        (uint64_t)state->target_clusters * f->geometry.sectors_per_cluster,
	    &callbacks, &monitor, stage);
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

static void initialize_transaction_fixture(struct exfat_fixture *f, uint32_t spc, uint32_t target)
{
	CHECK(shrink_fixture_initialize(f, spc) == 0);
	if (target % 8 != 0) {
		/* This allocated bad-cluster bit becomes reserved only during final commit.
		 * Original-size validation after cancellation must still find it set. */
		CHECK(shrink_fixture_set_fat(f, 2005, EXFAT_FAT_BAD_CLUSTER) == 0);
		CHECK(shrink_fixture_set_bit(f, 2005, 1) == 0);
		CHECK(memory_block_device_make_durable(&f->memory) == 0);
		memory_block_device_clear_operations(&f->memory);
	}
}

static void read_durable_sector(
    const struct memory_block_device *memory, uint64_t sector, unsigned char buffer[512])
{
	size_t i;
	memset(buffer, 0, 512);
	for (i = 0; i < memory->durable_sector_count; ++i) {
		if (memory->durable_sectors[i].sector == sector) {
			memcpy(buffer, memory->durable_sectors[i].data, 512);
			return;
		}
	}
}

static int verify_move_durability(const struct exfat_fixture *f, uint32_t target)
{
	unsigned char sector[512];
	uint32_t first = 0;
	uint32_t next = 0;
	uint32_t size = f->geometry.sectors_per_cluster * 512;
	uint32_t byte = (8900 - 2) / 8;
	uint32_t i;
	size_t j;
	/* The fixture's nested file moves from 8900 into the retained heap.
	 * Its stream entry starts in the second cluster of directory 8800. */
	read_durable_sector(&f->memory, exfat_fixture_cluster_sector(&f->geometry, 8801), sector);
	CHECK(exfat_resize_load_le32(sector, sizeof(sector), 20, &first) == EXFAT_RESIZE_SUCCESS);
	read_durable_sector(&f->memory,
	    exfat_fixture_cluster_sector(&f->geometry, f->bitmap_clusters[byte / size]) +
	        byte % size / 512,
	    sector);
	/* Freeing the source requires the replacement reference to be durable. */
	CHECK((sector[byte % 512] & (1u << ((8900 - 2) % 8))) != 0 || first != 8900);
	if (first == 8900)
		return 0;
	CHECK(first >= 2 && first < target + 2);
	if (first < 2 || first >= target + 2)
		return 0;
	/* Publishing the replacement requires its reservation and complete payload. */
	read_durable_sector(&f->memory, f->geometry.fat_offset + first / 128, sector);
	CHECK(exfat_resize_load_le32(sector, sizeof(sector), first % 128 * 4, &next) ==
	    EXFAT_RESIZE_SUCCESS);
	CHECK(next == EXFAT_FAT_END_OF_CHAIN);
	byte = (first - 2) / 8;
	read_durable_sector(&f->memory,
	    exfat_fixture_cluster_sector(&f->geometry, f->bitmap_clusters[byte / size]) +
	        byte % size / 512,
	    sector);
	CHECK((sector[byte % 512] & (1u << ((first - 2) % 8))) != 0);
	for (i = 0; i < f->geometry.sectors_per_cluster; ++i) {
		read_durable_sector(
		    &f->memory, exfat_fixture_cluster_sector(&f->geometry, first) + i, sector);
		for (j = 0; j < sizeof(sector); ++j)
			CHECK(sector[j] == 0x88);
	}
	return 1;
}

static int durable_target_matches(
    const struct exfat_fixture *f, const struct exfat_fixture *reference, uint32_t target)
{
	unsigned char actual[512];
	unsigned char expected[512];
	uint64_t length =
	    f->geometry.cluster_heap_offset + (uint64_t)target * f->geometry.sectors_per_cluster;
	uint64_t sector;
	/* FINALIZING promises the complete target, including payload and allocation
	 * metadata. Only clearing the main dirty flag may still be incomplete. */
	for (sector = 0; sector < length; ++sector) {
		read_durable_sector(&f->memory, sector, actual);
		read_durable_sector(&reference->memory, sector, expected);
		if (sector == 0) {
			actual[106] &= (unsigned char)~2u;
			expected[106] &= (unsigned char)~2u;
		}
		if (memcmp(actual, expected, sizeof(actual)) != 0)
			return 0;
	}
	return 1;
}

static void test_transactions(uint32_t spc, uint32_t target)
{
	struct exfat_fixture reference;
	struct test_allocator allocator = { 0 };
	struct monitor_state baseline = { .memory = &reference.memory,
		.allocator = &allocator,
		.cancel_at = SIZE_MAX,
		.target_clusters = target };
	enum exfat_resize_stage stage;
	size_t count;
	size_t index;
	int mode;
	int saw_partial = 0;
	int saw_published_move = 0;
	int saw_source_ready = 0;
	int saw_commit_completion = 0;
	initialize_transaction_fixture(&reference, spc, target);
	CHECK(resize(&reference, &baseline, &stage) == EXFAT_RESIZE_SUCCESS);
	count = reference.memory.operation_count;
	CHECK(shrink_fixture_verify(&reference, target) == 0);
	verify_allocation(&reference, target);

	/* Every callback may fail before or after side effects. Multi-sector callbacks
	 * also fail after a strict prefix, including directory entry-set publication. */
	for (index = 0; index < count; ++index) {
		const struct memory_operation operation = reference.memory.operations[index];
		for (mode = 0; mode < 3; ++mode) {
			struct exfat_fixture f;
			struct test_allocator a = { 0 };
			struct monitor_state state = { .memory = &f.memory,
				.allocator = &a,
				.cancel_at = SIZE_MAX,
				.target_clusters = target };
			size_t k;
			unsigned char boot[512];
			uint16_t flags = 0;
			if (mode == 2 &&
			    (operation.kind == MEMORY_OPERATION_SYNC || operation.sector_count < 2))
				continue;
			initialize_transaction_fixture(&f, spc, target);
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
			/* Persist the last issued write since the last successful sync, including
			 * all publication sectors and partially completed failed writes. */
			for (k = f.memory.operation_count; k > 0; --k) {
				const struct memory_operation *op = &f.memory.operations[k - 1];
				if (op->kind == MEMORY_OPERATION_SYNC && (k - 1 != index || mode != 0))
					break;
				if (op->kind == MEMORY_OPERATION_WRITE) {
					CHECK(memory_block_device_persist_range(
					          &f.memory, op->first_sector, op->sector_count) == 0);
					break;
				}
			}
			CHECK(memory_block_device_crash(&f.memory) == 0);
			CHECK(exfat_fixture_read_sector(&f, 0, boot, sizeof(boot)) == 0);
			CHECK(exfat_resize_load_le16(boot, sizeof(boot), 106, &flags) == EXFAT_RESIZE_SUCCESS);
			if (stage == EXFAT_RESIZE_STAGE_RESIZING) {
				CHECK((flags & 2) != 0);
				saw_published_move |= verify_move_durability(&f, target);
			}
			if (stage == EXFAT_RESIZE_STAGE_FINALIZING)
				CHECK(durable_target_matches(&f, &reference, target));
			if (stage == EXFAT_RESIZE_STAGE_PREFLIGHT)
				CHECK(shrink_fixture_verify(&f, 10000) == 0);
			CHECK(exfat_fixture_destroy(&f) == 0);
		}
	}
	if (spc > 1)
		CHECK(saw_partial);
	CHECK(saw_published_move);

	/* Request cancellation after each possible callback. Every return must be
	 * either an untouched/clean source or a fully committed clean target. */
	for (index = 0; index <= count; ++index) {
		struct exfat_fixture f;
		struct test_allocator a = { 0 };
		struct monitor_state state = {
			.memory = &f.memory, .allocator = &a, .cancel_at = index, .target_clusters = target
		};
		enum exfat_resize_error error;
		initialize_transaction_fixture(&f, spc, target);
		error = resize(&f, &state, &stage);
		CHECK(memory_block_device_crash(&f.memory) == 0);
		if (error == EXFAT_RESIZE_SUCCESS) {
			CHECK(stage == EXFAT_RESIZE_STAGE_COMPLETED);
			CHECK(shrink_fixture_verify(&f, target) == 0);
			verify_allocation(&f, target);
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
					CHECK(shrink_fixture_verify(&f, target) == 0);
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
	struct monitor_state state = {
		.memory = &f.memory, .allocator = &a, .cancel_at = SIZE_MAX, .target_clusters = 2000
	};
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
	test_transactions(1, 2000);
	test_transactions(4, 2000);
	test_transactions(1, 2001);
	test_cancel_cleanup_failure();
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
