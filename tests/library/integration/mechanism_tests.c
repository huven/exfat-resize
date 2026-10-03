/* SPDX-License-Identifier: MIT */

#include "allocation.h"
#include "boot_region.h"
#include "directory.h"
#include "endian.h"
#include "resize_internal.h"
#include "stream.h"
#include "support/exfat_fixture.h"
#include "support/test_allocator.h"
#include "volume.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(expression) \
	do { \
		if (!(expression)) { \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression); \
			++failures; \
		} \
	} while (0)
#define OK(expression) CHECK((expression) == EXFAT_RESIZE_SUCCESS)

struct test_volume {
	struct exfat_fixture fixture;
	struct test_allocator allocator;
	struct resize_operation operation;
	struct resize_volume volume;
	struct resize_allocation allocation;
	struct resize_directory directory;
};

static int initialize(struct test_volume *test)
{
	memset(test, 0, sizeof(*test));
	if (exfat_fixture_initialize(&test->fixture, 65536) != 0)
		return 0;
	test->operation.allocator = test_allocator_callbacks(&test->allocator);
	test->volume.operation = &test->operation;
	OK(exfat_resize_open_volume(&test->volume, &test->fixture.memory.device));
	OK(exfat_resize_read_boot_regions(
	    test->volume.device, test->volume.io_buffer, EXFAT_IO_BUFFER_SIZE, &test->volume.geometry));
	test->volume.cluster_size =
	    test->volume.geometry.sectors_per_cluster * test->volume.sector_size;
	test->volume.io_sector_capacity = (uint32_t)(EXFAT_IO_BUFFER_SIZE / test->volume.sector_size);
	OK(exfat_resize_allocate_volume_caches(&test->volume));
	test->allocation.volume = &test->volume;
	test->allocation.model_geometry = &test->volume.geometry;
	OK(exfat_resize_load_source_fat(&test->allocation));
	OK(exfat_resize_create_allocation_model(&test->allocation));
	test->directory.volume = &test->volume;
	return 1;
}

static void destroy(struct test_volume *test)
{
	exfat_resize_release_source_fat(&test->allocation);
	exfat_resize_release_directory(&test->directory);
	exfat_resize_release_allocation_model(&test->allocation);
	exfat_resize_close_volume(&test->volume);
	CHECK(test_allocator_is_clean(&test->allocator));
	CHECK(exfat_fixture_destroy(&test->fixture) == 0);
}

static uint64_t sector_for(struct test_volume *test, uint32_t cluster)
{
	return exfat_fixture_cluster_sector(&test->volume.geometry, cluster);
}

static void no_publication(const struct memory_block_device *memory)
{
	size_t index;
	for (index = 0; index < memory->operation_count; ++index)
		CHECK(memory->operations[index].kind == MEMORY_OPERATION_READ);
}

struct observed_owner {
	struct directory_reference reference;
	struct allocation_stream stream;
};

struct observation {
	struct test_volume *test;
	struct allocation_observer callbacks;
	struct directory_reference current_owner;
	struct observed_owner owners[16];
	size_t owner_count;
	size_t tail_link_count;
	struct allocation_link first_tail_link;
	struct allocation_link last_tail_link;
	struct allocation_link fragmented_link;
};

static enum exfat_resize_error claim_current(void *context, uint32_t cluster)
{
	struct observation *observation = context;
	return exfat_resize_claim_model_cluster(&observation->test->allocation, cluster);
}

static enum exfat_resize_error record_current(
    void *context, const struct allocation_stream *stream, const struct allocation_link *link)
{
	struct observation *observation = context;
	if (!stream->no_fat_chain)
		observation->test->allocation.allocation_model[link->cluster - 2] = link->next;
	if (link->cluster == 12)
		observation->fragmented_link = *link;
	if (link->cluster >= 300) {
		CHECK(observation->current_owner.kind == DIRECTORY_OWNER_FILE);
		CHECK(observation->current_owner.directory_id == 2);
		CHECK(stream->no_fat_chain);
		CHECK(stream->first_cluster == 100);
		if (observation->tail_link_count == 0)
			observation->first_tail_link = *link;
		observation->last_tail_link = *link;
		++observation->tail_link_count;
	}
	return EXFAT_RESIZE_SUCCESS;
}

static enum exfat_resize_error current_is_claimed(void *context, uint32_t cluster, int *claimed)
{
	struct observation *observation = context;
	uint32_t *entry;
	enum exfat_resize_error error =
	    exfat_resize_model_entry(&observation->test->allocation, cluster, &entry);
	if (error == EXFAT_RESIZE_SUCCESS)
		*claimed = *entry != 0;
	return error;
}

static enum exfat_resize_error record_bad(void *context, uint32_t cluster)
{
	struct observation *observation = context;
	uint32_t *entry;
	enum exfat_resize_error error =
	    exfat_resize_model_entry(&observation->test->allocation, cluster, &entry);
	if (error == EXFAT_RESIZE_SUCCESS)
		*entry = EXFAT_FAT_BAD_CLUSTER;
	return error;
}

static enum exfat_resize_error observe_owner(void *context,
    const struct directory_reference *owner,
    const struct allocation_stream *stream,
    int is_directory)
{
	struct observation *observation = context;
	struct test_volume *test = observation->test;
	struct stream_chain_reader chain = exfat_resize_source_chain(&test->allocation);
	(void)is_directory;
	if (observation->owner_count >= 16)
		return EXFAT_RESIZE_OUT_OF_MEMORY;
	observation->owners[observation->owner_count++] = (struct observed_owner){ *owner, *stream };
	observation->current_owner = *owner;
	if (owner->kind == DIRECTORY_OWNER_BITMAP)
		test->allocation.old_bitmap = *stream;
	return exfat_resize_visit_allocation(
	    &test->volume, &test->volume.geometry, &chain, stream, &observation->callbacks);
}

static void observe(struct test_volume *test, struct observation *observation)
{
	struct allocation_stream root = { .first_cluster = 2, .root_directory = 1 };
	struct directory_scan scan = { .geometry = &test->volume.geometry,
		.chain = exfat_resize_source_chain(&test->allocation),
		.context = observation,
		.visit = observe_owner };
	memset(observation, 0, sizeof(*observation));
	observation->test = test;
	observation->callbacks = (struct allocation_observer){ .context = observation,
		.claim = claim_current,
		.record = record_current,
		.is_claimed = current_is_claimed,
		.bad_cluster = record_bad };
	observation->current_owner = (struct directory_reference){ .kind = DIRECTORY_OWNER_ROOT };
	OK(exfat_resize_visit_allocation(
	    &test->volume, &test->volume.geometry, &scan.chain, &root, &observation->callbacks));
	OK(exfat_resize_scan_directory_tree(&test->directory, &root, &scan));
	OK(exfat_resize_reconcile_allocation(&test->volume, &test->volume.geometry, &scan.chain,
	    &test->allocation.old_bitmap, &observation->callbacks));
}

static struct observed_owner *find_owner(struct observation *observation, uint32_t first_cluster)
{
	size_t index;
	for (index = 0; index < observation->owner_count; ++index) {
		if (observation->owners[index].stream.first_cluster == first_cluster)
			return &observation->owners[index];
	}
	CHECK(0);
	return &observation->owners[0];
}

static void test_current_model_and_tail_observations(void)
{
	struct test_volume test;
	struct observation observation;
	struct stream_chain_reader chain;
	uint32_t next;
	if (!initialize(&test)) {
		CHECK(0);
		return;
	}
	memory_block_device_clear_operations(&test.fixture.memory);
	observe(&test, &observation);
	no_publication(&test.fixture.memory);
	CHECK(test.allocation.allocation_model_size == (size_t)test.volume.geometry.cluster_count * 4);
	CHECK(test.allocation.allocation_model[399 - 2] == EXFAT_MODEL_NO_FAT_CHAIN);
	CHECK(observation.tail_link_count == 100);
	CHECK(observation.first_tail_link.cluster == 300);
	CHECK(observation.first_tail_link.index == 200);
	CHECK(observation.first_tail_link.previous == 299);
	CHECK(observation.last_tail_link.next == EXFAT_FAT_END_OF_CHAIN);
	CHECK(observation.fragmented_link.previous == 10);
	CHECK(observation.fragmented_link.cluster == 12);
	CHECK(observation.fragmented_link.next == 11);
	chain = exfat_resize_model_chain(&test.allocation);
	OK(exfat_resize_reconcile_allocation(&test.volume, &test.volume.geometry, &chain,
	    &test.allocation.old_bitmap, &observation.callbacks));
	OK(chain.next(chain.context, 12, &next));
	CHECK(next == 11);
	CHECK(
	    exfat_resize_claim_model_cluster(&test.allocation, 12) == EXFAT_RESIZE_INVALID_FILESYSTEM);
	destroy(&test);
}

struct directory_resolution {
	uint32_t identity;
	struct allocation_stream current;
};

static enum exfat_resize_error resolve_directory(
    void *context, uint32_t identity, struct allocation_stream *directory)
{
	const struct directory_resolution *resolution = context;
	if (identity != resolution->identity)
		return EXFAT_RESIZE_INVALID_ARGUMENT;
	*directory = resolution->current;
	return EXFAT_RESIZE_SUCCESS;
}

static uint16_t checksum(const unsigned char *bytes, size_t count)
{
	uint16_t value = 0;
	size_t index;
	for (index = 0; index < count; ++index) {
		if (index == 2 || index == 3)
			continue;
		value = (uint16_t)((value >> 1) | ((value & 1) << 15));
		value = (uint16_t)(value + bytes[index]);
	}
	return value;
}

static void test_targeted_edit_after_directory_move(void)
{
	struct test_volume test;
	struct observation observation;
	struct observed_owner *owner;
	struct directory_resolution resolution = { .identity = 6,
		.current = { .first_cluster = 50, .data_length = 512, .no_fat_chain = 1 } };
	struct directory_access access;
	struct allocation_stream replacement;
	unsigned char original[512], expected[512], actual[512];
	size_t attempts;
	if (!initialize(&test)) {
		CHECK(0);
		return;
	}
	observe(&test, &observation);
	owner = find_owner(&observation, 8);
	CHECK(owner->reference.directory_id == 6);
	CHECK(owner->reference.entry_offset == 0);
	CHECK(exfat_fixture_read_sector(&test.fixture, sector_for(&test, 6), original, 512) == 0);
	OK(exfat_resize_copy_cluster_run(&test.volume, &test.volume.geometry, 6, 50, 1));
	access = (struct directory_access){ .geometry = &test.volume.geometry,
		.chain = exfat_resize_model_chain(&test.allocation),
		.context = &resolution,
		.resolve = resolve_directory };
	replacement = owner->stream;
	replacement.first_cluster = 80;
	replacement.no_fat_chain = 0;
	attempts = test.allocator.allocation_attempts;
	{
		struct allocation_stream invalid = replacement;
		invalid.first_cluster = test.volume.geometry.cluster_count + 2;
		memory_block_device_clear_operations(&test.fixture.memory);
		CHECK(exfat_resize_edit_directory_allocation(&test.directory, &access, &owner->reference,
		          &owner->stream, &invalid) == EXFAT_RESIZE_INVALID_ARGUMENT);
		no_publication(&test.fixture.memory);
		invalid = replacement;
		invalid.data_length = 1; /* Relocation preserves both length fields. */
		CHECK(exfat_resize_edit_directory_allocation(&test.directory, &access, &owner->reference,
		          &owner->stream, &invalid) == EXFAT_RESIZE_INVALID_ARGUMENT);
		no_publication(&test.fixture.memory);
	}
	OK(exfat_resize_edit_directory_allocation(
	    &test.directory, &access, &owner->reference, &owner->stream, &replacement));
	OK(exfat_resize_flush_volume_range(&test.volume, sector_for(&test, 50), 1));
	CHECK(test.allocator.allocation_attempts == attempts);
	memcpy(expected, original, 512);
	expected[33] &= (unsigned char)~2;
	OK(exfat_resize_store_le32(expected, 512, 52, 80));
	OK(exfat_resize_store_le16(expected, 512, 2, checksum(expected, 96)));
	CHECK(exfat_fixture_read_sector(&test.fixture, sector_for(&test, 50), actual, 512) == 0);
	CHECK(memcmp(expected, actual, 512) == 0);
	CHECK(exfat_fixture_read_sector(&test.fixture, sector_for(&test, 6), actual, 512) == 0);
	CHECK(memcmp(original, actual, 512) == 0);
	memory_block_device_clear_operations(&test.fixture.memory);
	CHECK(exfat_resize_edit_directory_allocation(&test.directory, &access, &owner->reference,
	          &owner->stream, &replacement) == EXFAT_RESIZE_INVALID_FILESYSTEM);
	no_publication(&test.fixture.memory);
	expected[2] ^= 1;
	OK(exfat_resize_write_volume(&test.volume, sector_for(&test, 50), 1, expected, 512));
	memory_block_device_clear_operations(&test.fixture.memory);
	CHECK(exfat_resize_edit_directory_allocation(&test.directory, &access, &owner->reference,
	          &replacement, &replacement) == EXFAT_RESIZE_INVALID_FILESYSTEM);
	no_publication(&test.fixture.memory);
	destroy(&test);
}

static void test_targeted_entry_set_spans_fragmented_clusters(void)
{
	struct test_volume test;
	struct directory_resolution resolution = { .identity = 2,
		.current = { .first_cluster = 2, .root_directory = 1 } };
	struct directory_reference owner = {
		.directory_id = 2, .entry_offset = 480, .kind = DIRECTORY_OWNER_FILE
	};
	struct allocation_stream expected = {
		.first_cluster = 5, .data_length = 512, .no_fat_chain = 1
	};
	struct allocation_stream replacement = { .first_cluster = 90, .data_length = 512 };
	struct directory_access access;
	unsigned char root[512], first[512], second[512], entries[96], actual[96];
	if (!initialize(&test)) {
		CHECK(0);
		return;
	}
	CHECK(exfat_fixture_read_sector(&test.fixture, sector_for(&test, 2), root, 512) == 0);
	memcpy(entries, root + 64, 96);
	memset(first, 0, 512);
	memset(second, 0, 512);
	memcpy(first + 480, entries, 32);
	memcpy(second, entries + 32, 64);
	OK(exfat_resize_write_volume(&test.volume, sector_for(&test, 2), 1, first, 512));
	OK(exfat_resize_write_volume(&test.volume, sector_for(&test, 80), 1, second, 512));
	test.allocation.allocation_model[0] = 80;
	test.allocation.allocation_model[80 - 2] = EXFAT_FAT_END_OF_CHAIN;
	access = (struct directory_access){ .geometry = &test.volume.geometry,
		.chain = exfat_resize_model_chain(&test.allocation),
		.context = &resolution,
		.resolve = resolve_directory };
	OK(exfat_resize_edit_directory_allocation(
	    &test.directory, &access, &owner, &expected, &replacement));
	OK(exfat_resize_flush_volume_range(&test.volume, 0, test.volume.device->sector_count));
	CHECK(exfat_fixture_read_sector(&test.fixture, sector_for(&test, 2), first, 512) == 0);
	CHECK(exfat_fixture_read_sector(&test.fixture, sector_for(&test, 80), second, 512) == 0);
	memcpy(actual, first + 480, 32);
	memcpy(actual + 32, second, 64);
	entries[33] &= (unsigned char)~2;
	OK(exfat_resize_store_le32(entries, 96, 52, 90));
	OK(exfat_resize_store_le16(entries, 96, 2, checksum(entries, 96)));
	CHECK(memcmp(entries, actual, 96) == 0);
	destroy(&test);
}

static void test_system_entries_after_root_move(void)
{
	struct test_volume test;
	struct observation observation;
	struct directory_resolution resolution = { .identity = 2,
		.current = { .first_cluster = 60, .root_directory = 1 } };
	struct directory_access access;
	unsigned char original[512], expected[512], actual[512];
	uint32_t heads[2];
	size_t index;
	if (!initialize(&test)) {
		CHECK(0);
		return;
	}
	observe(&test, &observation);
	heads[0] = test.allocation.old_bitmap.first_cluster;
	heads[1] = 4;
	CHECK(exfat_fixture_read_sector(&test.fixture, sector_for(&test, 2), original, 512) == 0);
	memcpy(expected, original, 512);
	OK(exfat_resize_copy_cluster_run(&test.volume, &test.volume.geometry, 2, 60, 1));
	test.allocation.allocation_model[60 - 2] = EXFAT_FAT_END_OF_CHAIN;
	access = (struct directory_access){ .geometry = &test.volume.geometry,
		.chain = exfat_resize_model_chain(&test.allocation),
		.context = &resolution,
		.resolve = resolve_directory };
	for (index = 0; index < 2; ++index) {
		struct observed_owner *owner = find_owner(&observation, heads[index]);
		struct allocation_stream replacement = owner->stream;
		replacement.first_cluster = (uint32_t)(70 + index);
		if (owner->reference.kind == DIRECTORY_OWNER_BITMAP)
			++replacement.data_length;
		OK(exfat_resize_edit_directory_allocation(
		    &test.directory, &access, &owner->reference, &owner->stream, &replacement));
		OK(exfat_resize_store_le32(
		    expected, 512, (size_t)owner->reference.entry_offset + 20, replacement.first_cluster));
		if (owner->reference.kind == DIRECTORY_OWNER_BITMAP)
			OK(exfat_resize_store_le64(expected, 512, (size_t)owner->reference.entry_offset + 24,
			    replacement.data_length));
	}
	OK(exfat_resize_flush_volume_range(&test.volume, sector_for(&test, 60), 1));
	CHECK(exfat_fixture_read_sector(&test.fixture, sector_for(&test, 60), actual, 512) == 0);
	CHECK(memcmp(expected, actual, 512) == 0);
	CHECK(exfat_fixture_read_sector(&test.fixture, sector_for(&test, 2), actual, 512) == 0);
	CHECK(memcmp(original, actual, 512) == 0);
	destroy(&test);
}

struct publication_monitor {
	const struct memory_block_device *memory;
	size_t polls;
	size_t syncs_when_cancelled;
	int consumed;
};

static int cancel_publication_after_write(void *context)
{
	struct publication_monitor *monitor = context;
	size_t index;
	size_t writes = 0;
	size_t syncs = 0;
	++monitor->polls;
	for (index = 0; index < monitor->memory->operation_count; ++index) {
		writes += monitor->memory->operations[index].kind == MEMORY_OPERATION_WRITE;
		syncs += monitor->memory->operations[index].kind == MEMORY_OPERATION_SYNC;
	}
	if (writes == 0 || monitor->consumed)
		return 0;
	monitor->consumed = 1; /* A one-shot return must not be consumed mid-batch. */
	monitor->syncs_when_cancelled = syncs;
	return 1;
}

/* Test consumer of the publication interfaces. The operation, rather than an
 * individual editor, owns the cancellation boundary and failure precedence. */
static enum exfat_resize_error publish_allocation(struct test_volume *test,
    const struct directory_access *access,
    const struct directory_reference *owner,
    const struct allocation_stream *expected,
    const struct allocation_stream *replacement)
{
	struct stream_chain_reader chain = exfat_resize_source_chain(&test->allocation);
	struct allocation_stream bitmap = { .first_cluster = test->fixture.bitmap_clusters[0],
		.data_length = ((uint64_t)test->volume.geometry.cluster_count + 7) / 8 };
	enum exfat_resize_error error = exfat_resize_cancellation_checkpoint(&test->operation);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	test->operation.cancellation_deferred = 1;
	/* Force the bitmap seek to reach a cluster checkpoint if deferral is broken. */
	test->operation.cluster_steps_since_checkpoint = EXFAT_CLUSTER_CHECKPOINT_INTERVAL - 1;
	error = exfat_resize_edit_directory_allocation(
	    &test->directory, access, owner, expected, replacement);
	if (error == EXFAT_RESIZE_SUCCESS)
		error = exfat_resize_write_fat_entry(&test->volume, &test->volume.geometry,
		    replacement->first_cluster, EXFAT_FAT_END_OF_CHAIN);
	if (error == EXFAT_RESIZE_SUCCESS)
		error = exfat_resize_set_bitmap_bit(
		    &test->volume, &test->volume.geometry, &chain, &bitmap, replacement->first_cluster, 1);
	if (error == EXFAT_RESIZE_SUCCESS)
		error =
		    exfat_resize_flush_volume_range(&test->volume, 0, test->volume.device->sector_count);
	if (error == EXFAT_RESIZE_SUCCESS)
		error = exfat_resize_block_device_sync(test->volume.device);
	CHECK(test->operation.cluster_steps_since_checkpoint == EXFAT_CLUSTER_CHECKPOINT_INTERVAL - 1);
	test->operation.cancellation_deferred = 0;
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	return exfat_resize_cancellation_checkpoint(&test->operation);
}

static size_t check_publication_cancellation(size_t failing_operation, int fail_after)
{
	struct test_volume test;
	struct directory_resolution resolution = { .identity = 2,
		.current = { .first_cluster = 2, .root_directory = 1 } };
	struct directory_reference owner = {
		.directory_id = 2, .entry_offset = 448, .kind = DIRECTORY_OWNER_FILE
	};
	struct allocation_stream expected = {
		.first_cluster = 5, .data_length = 512, .no_fat_chain = 1
	};
	struct allocation_stream replacement = { .first_cluster = 90, .data_length = 512 };
	struct directory_access access;
	struct publication_monitor monitor = { 0 };
	unsigned char root[512], first[512] = { 0 }, second[512] = { 0 }, entries[96], actual[96];
	enum exfat_resize_error error;
	size_t operation_count;
	if (!initialize(&test)) {
		CHECK(0);
		return 0;
	}
	CHECK(exfat_fixture_read_sector(&test.fixture, sector_for(&test, 2), root, 512) == 0);
	memcpy(entries, root + 64, 96);
	memcpy(first + 448, entries, 64);
	memcpy(second, entries + 64, 32);
	OK(exfat_resize_write_volume(&test.volume, sector_for(&test, 2), 1, first, 512));
	OK(exfat_resize_write_volume(&test.volume, sector_for(&test, 80), 1, second, 512));
	test.allocation.allocation_model[0] = 80;
	test.allocation.allocation_model[80 - 2] = EXFAT_FAT_END_OF_CHAIN;
	access = (struct directory_access){ .geometry = &test.volume.geometry,
		.chain = exfat_resize_model_chain(&test.allocation),
		.context = &resolution,
		.resolve = resolve_directory };
	monitor.memory = &test.fixture.memory;
	test.operation.monitor.context = &monitor;
	test.operation.monitor.cancellation_requested = cancel_publication_after_write;
	memory_block_device_clear_operations(&test.fixture.memory);
	if (failing_operation != SIZE_MAX) {
		if (fail_after)
			memory_block_device_fail_after_operation(
			    &test.fixture.memory, failing_operation, 1, EIO);
		else
			memory_block_device_fail_operation(&test.fixture.memory, failing_operation, EIO);
	}
	error = publish_allocation(&test, &access, &owner, &expected, &replacement);
	operation_count = test.fixture.memory.operation_count;
	CHECK(!test.operation.cancellation_deferred);
	if (failing_operation == SIZE_MAX) {
		CHECK(error == EXFAT_RESIZE_CANCELLED);
		CHECK(monitor.polls == 2);
		CHECK(monitor.consumed);
		CHECK(monitor.syncs_when_cancelled == 1);
		/* The cancellation result follows a durable, checksummed entry update. */
		CHECK(memory_block_device_crash(&test.fixture.memory) == 0);
		CHECK(exfat_fixture_read_sector(&test.fixture, sector_for(&test, 2), first, 512) == 0);
		CHECK(exfat_fixture_read_sector(&test.fixture, sector_for(&test, 80), second, 512) == 0);
		memcpy(actual, first + 448, 64);
		memcpy(actual + 64, second, 32);
		entries[33] &= (unsigned char)~2;
		OK(exfat_resize_store_le32(entries, 96, 52, 90));
		OK(exfat_resize_store_le16(entries, 96, 2, checksum(entries, 96)));
		CHECK(memcmp(entries, actual, 96) == 0);
	} else {
		CHECK(error == EXFAT_RESIZE_IO_ERROR);
		CHECK(monitor.polls == 1);
		CHECK(!monitor.consumed);
		CHECK(test.fixture.memory.operation_index == failing_operation + 1);
		memory_block_device_clear_failure(&test.fixture.memory);
	}
	destroy(&test);
	return operation_count;
}

static void test_cancellation_deferral_covers_publication_and_failures(void)
{
	size_t count = check_publication_cancellation(SIZE_MAX, 0);
	size_t index;
	CHECK(count != 0);
	for (index = 0; index < count; ++index) {
		check_publication_cancellation(index, 0);
		check_publication_cancellation(index, 1);
	}
}

static void test_relocation_rejects_file_directory_and_upcase_length_changes(void)
{
	struct test_volume test;
	struct observation observation;
	struct directory_resolution resolution = { .identity = 2,
		.current = { .first_cluster = 2, .root_directory = 1 } };
	struct directory_access access;
	const uint32_t heads[] = { 5, 6, 4 };
	unsigned char original[512], expected[512], actual[512];
	size_t index;
	if (!initialize(&test)) {
		CHECK(0);
		return;
	}
	CHECK(exfat_fixture_read_sector(&test.fixture, sector_for(&test, 2), original, 512) == 0);
	/* File valid length may differ from its allocation length and must survive relocation. */
	OK(exfat_resize_store_le64(original, 512, 64 + 32 + 8, 128));
	OK(exfat_resize_store_le16(original, 512, 64 + 2, checksum(original + 64, 96)));
	OK(exfat_resize_write_volume(&test.volume, sector_for(&test, 2), 1, original, 512));
	memcpy(expected, original, 512);
	observe(&test, &observation);
	access = (struct directory_access){ .geometry = &test.volume.geometry,
		.chain = exfat_resize_model_chain(&test.allocation),
		.context = &resolution,
		.resolve = resolve_directory };
	for (index = 0; index < sizeof(heads) / sizeof(heads[0]); ++index) {
		struct observed_owner *owner = find_owner(&observation, heads[index]);
		struct allocation_stream replacement = owner->stream;
		int longer;
		for (longer = 0; longer <= 1; ++longer) {
			replacement.data_length =
			    longer ? owner->stream.data_length + 1 : owner->stream.data_length - 1;
			memory_block_device_clear_operations(&test.fixture.memory);
			CHECK(
			    exfat_resize_edit_directory_allocation(&test.directory, &access, &owner->reference,
			        &owner->stream, &replacement) == EXFAT_RESIZE_INVALID_ARGUMENT);
			no_publication(&test.fixture.memory);
		}
	}
	OK(exfat_resize_flush_volume_range(&test.volume, 0, test.volume.device->sector_count));
	no_publication(&test.fixture.memory);
	CHECK(exfat_fixture_read_sector(&test.fixture, sector_for(&test, 2), actual, 512) == 0);
	CHECK(memcmp(original, actual, 512) == 0);
	for (index = 0; index < sizeof(heads) / sizeof(heads[0]); ++index) {
		struct observed_owner *owner = find_owner(&observation, heads[index]);
		struct allocation_stream replacement = owner->stream;
		size_t offset = (size_t)owner->reference.entry_offset;
		replacement.first_cluster = (uint32_t)(50 + index);
		OK(exfat_resize_edit_directory_allocation(
		    &test.directory, &access, &owner->reference, &owner->stream, &replacement));
		if (owner->reference.kind == DIRECTORY_OWNER_FILE) {
			OK(exfat_resize_store_le32(expected, 512, offset + 32 + 20, replacement.first_cluster));
			OK(exfat_resize_store_le16(expected, 512, offset + 2, checksum(expected + offset, 96)));
		} else {
			OK(exfat_resize_store_le32(expected, 512, offset + 20, replacement.first_cluster));
		}
	}
	OK(exfat_resize_flush_volume_range(&test.volume, 0, test.volume.device->sector_count));
	CHECK(exfat_fixture_read_sector(&test.fixture, sector_for(&test, 2), actual, 512) == 0);
	CHECK(memcmp(expected, actual, 512) == 0);
	destroy(&test);
}

static void test_cache_coherence_and_explicit_publication(void)
{
	struct test_volume test;
	struct sector_cache *cache;
	unsigned char data[512];
	uint64_t source, target;
	if (!initialize(&test)) {
		CHECK(0);
		return;
	}
	source = sector_for(&test, 6);
	target = sector_for(&test, 50);
	OK(exfat_resize_load_cache(&test.volume, SECTOR_CACHE_SOURCE_DIRECTORY_DATA, source, 1));
	OK(exfat_resize_load_cache(&test.volume, SECTOR_CACHE_TARGET_DIRECTORY_DATA, source, 1));
	OK(exfat_resize_prepare_cached_write(
	    &test.volume, SECTOR_CACHE_TARGET_DIRECTORY_DATA, source, 1));
	CHECK(test.volume.caches[SECTOR_CACHE_SOURCE_DIRECTORY_DATA].sector_count == 0);
	cache = &test.volume.caches[SECTOR_CACHE_TARGET_DIRECTORY_DATA];
	cache->data[9] ^= 0x5a;
	cache->dirty_first = 0;
	cache->dirty_end = 1;
	memory_block_device_clear_operations(&test.fixture.memory);
	CHECK(exfat_resize_read_volume(&test.volume, source, 1, data, 512) ==
	    EXFAT_RESIZE_INTERNAL_ERROR);
	CHECK(exfat_resize_write_volume(&test.volume, source, 1, data, 512) ==
	    EXFAT_RESIZE_INTERNAL_ERROR);
	CHECK(exfat_resize_copy_cluster_run(&test.volume, &test.volume.geometry, 6, 50, 1) ==
	    EXFAT_RESIZE_INTERNAL_ERROR);
	CHECK(test.fixture.memory.operation_count == 0);
	memcpy(data, cache->data, 512);
	OK(exfat_resize_flush_volume_range(&test.volume, source, 1));
	CHECK(test.fixture.memory.operation_count == 1);
	CHECK(test.fixture.memory.operations[0].kind == MEMORY_OPERATION_WRITE);
	OK(exfat_resize_copy_cluster_run(&test.volume, &test.volume.geometry, 6, 50, 1));
	OK(exfat_resize_load_cache(&test.volume, SECTOR_CACHE_SOURCE_DIRECTORY_DATA, target, 1));
	CHECK(memcmp(data, test.volume.caches[SECTOR_CACHE_SOURCE_DIRECTORY_DATA].data, 512) == 0);
	CHECK(exfat_resize_invalidate_volume_range(&test.volume, UINT64_MAX, 2) ==
	    EXFAT_RESIZE_OUT_OF_BOUNDS);
	destroy(&test);
}

static void test_partial_direct_write_invalidates_cached_aliases(void)
{
	struct test_volume test;
	unsigned char original[1024], replacement[1024], actual[1024];
	uint64_t sector;
	if (!initialize(&test)) {
		CHECK(0);
		return;
	}
	sector = sector_for(&test, 100);
	OK(exfat_resize_read_volume(&test.volume, sector, 2, original, sizeof(original)));
	memset(replacement, 0x5a, sizeof(replacement));
	OK(exfat_resize_load_cache(&test.volume, SECTOR_CACHE_SOURCE_DIRECTORY_DATA, sector, 2));
	OK(exfat_resize_load_cache(&test.volume, SECTOR_CACHE_TARGET_DIRECTORY_DATA, sector, 2));
	memory_block_device_clear_operations(&test.fixture.memory);
	memory_block_device_fail_after_operation(&test.fixture.memory, 0, 1, EIO);
	CHECK(exfat_resize_write_volume(&test.volume, sector, 2, replacement, sizeof(replacement)) ==
	    EXFAT_RESIZE_IO_ERROR);
	CHECK(test.volume.caches[SECTOR_CACHE_SOURCE_DIRECTORY_DATA].sector_count == 0);
	CHECK(test.volume.caches[SECTOR_CACHE_TARGET_DIRECTORY_DATA].sector_count == 0);
	memory_block_device_clear_failure(&test.fixture.memory);
	OK(exfat_resize_load_cache(&test.volume, SECTOR_CACHE_SOURCE_DIRECTORY_DATA, sector, 2));
	memcpy(actual, test.volume.caches[SECTOR_CACHE_SOURCE_DIRECTORY_DATA].data, sizeof(actual));
	CHECK(memcmp(actual, replacement, 512) == 0);
	CHECK(memcmp(actual + 512, original + 512, 512) == 0);
	destroy(&test);
}

static void test_failed_cache_flush_retains_pending_data(void)
{
	struct test_volume test;
	struct sector_cache *cache;
	uint64_t sector;
	unsigned char actual[1024];
	if (!initialize(&test)) {
		CHECK(0);
		return;
	}
	sector = sector_for(&test, 100);
	OK(exfat_resize_load_cache(&test.volume, SECTOR_CACHE_TARGET_DIRECTORY_DATA, sector, 2));
	OK(exfat_resize_prepare_cached_write(
	    &test.volume, SECTOR_CACHE_TARGET_DIRECTORY_DATA, sector, 2));
	cache = &test.volume.caches[SECTOR_CACHE_TARGET_DIRECTORY_DATA];
	memset(cache->data, 0x6b, 1024);
	cache->dirty_first = 0;
	cache->dirty_end = 2;
	memory_block_device_clear_operations(&test.fixture.memory);
	memory_block_device_fail_after_operation(&test.fixture.memory, 0, 1, EIO);
	CHECK(exfat_resize_flush_volume_range(&test.volume, sector, 2) == EXFAT_RESIZE_IO_ERROR);
	CHECK(cache->dirty_first == 0 && cache->dirty_end == 2);
	CHECK(exfat_resize_invalidate_volume_range(&test.volume, sector, 2) ==
	    EXFAT_RESIZE_INTERNAL_ERROR);
	memory_block_device_clear_failure(&test.fixture.memory);
	OK(exfat_resize_flush_volume_range(&test.volume, sector, 2));
	OK(exfat_resize_read_volume(&test.volume, sector, 2, actual, 1024));
	CHECK(memcmp(actual, cache->data, 1024) == 0);
	CHECK(cache->dirty_first == cache->dirty_end);
	destroy(&test);
}

static void test_incremental_fat_and_fragmented_bitmap_updates(void)
{
	struct test_volume test;
	struct stream_chain_reader chain;
	struct allocation_stream bitmap;
	unsigned char before[512], expected[512], actual[512];
	uint64_t sector;
	uint32_t old_value;
	size_t attempts, index;
	if (!initialize(&test)) {
		CHECK(0);
		return;
	}
	chain = exfat_resize_source_chain(&test.allocation);
	attempts = test.allocator.allocation_attempts;
	sector = test.volume.geometry.fat_offset;
	CHECK(exfat_fixture_read_sector(&test.fixture, sector, before, 512) == 0);
	memcpy(expected, before, 512);
	OK(exfat_resize_store_le32(expected, 512, 12 * 4, 80));
	OK(exfat_resize_write_fat_entry(&test.volume, &test.volume.geometry, 12, 80));
	CHECK(exfat_fixture_read_sector(&test.fixture, sector, actual, 512) == 0);
	CHECK(memcmp(expected, actual, 512) == 0);
	OK(chain.next(chain.context, 12, &old_value));
	CHECK(old_value == 11); /* Snapshot and current model are explicitly caller-managed. */
	bitmap = (struct allocation_stream){ .first_cluster = test.fixture.bitmap_clusters[0],
		.data_length = ((uint64_t)test.volume.geometry.cluster_count + 7) / 8 };
	CHECK(test.fixture.bitmap_cluster_count == 3);
	sector = sector_for(&test, test.fixture.bitmap_clusters[2]);
	CHECK(exfat_fixture_read_sector(&test.fixture, sector, before, 512) == 0);
	memcpy(expected, before, 512);
	expected[((9000 - 2) / 8) % 512] |= (unsigned char)(1u << ((9000 - 2) % 8));
	memory_block_device_clear_operations(&test.fixture.memory);
	OK(exfat_resize_set_bitmap_bit(&test.volume, &test.volume.geometry, &chain, &bitmap, 9000, 1));
	CHECK(test.fixture.memory.operation_count == 2);
	CHECK(exfat_fixture_read_sector(&test.fixture, sector, actual, 512) == 0);
	CHECK(memcmp(expected, actual, 512) == 0);
	OK(exfat_resize_set_bitmap_bit(&test.volume, &test.volume.geometry, &chain, &bitmap, 9000, 0));
	CHECK(exfat_fixture_read_sector(&test.fixture, sector, actual, 512) == 0);
	CHECK(memcmp(before, actual, 512) == 0);
	CHECK(test.allocator.allocation_attempts == attempts);
	for (index = 0; index < test.fixture.memory.operation_count; ++index) {
		CHECK(test.fixture.memory.operations[index].kind != MEMORY_OPERATION_SYNC);
		CHECK(test.fixture.memory.operations[index].sector_count == 1);
	}
	CHECK(exfat_resize_write_fat_entry(&test.volume, &test.volume.geometry, 1, 80) ==
	    EXFAT_RESIZE_INVALID_ARGUMENT);
	destroy(&test);
}

static enum exfat_resize_error broken_chain(const void *context, uint32_t cluster, uint32_t *next)
{
	(void)context;
	(void)cluster;
	(void)next;
	return EXFAT_RESIZE_IO_ERROR;
}

static void test_stream_reader_uses_current_model_and_propagates_errors(void)
{
	struct test_volume test;
	struct stream_cursor cursor;
	struct allocation_stream stream = { .first_cluster = 10, .data_length = 1024 };
	struct stream_chain_reader chain;
	unsigned char expected[512], actual[512];
	int pass;
	if (!initialize(&test)) {
		CHECK(0);
		return;
	}
	chain = exfat_resize_model_chain(&test.allocation);
	for (pass = 0; pass < 2; ++pass) {
		uint32_t next = pass == 0 ? 12 : 11;
		test.allocation.allocation_model[10 - 2] = next;
		test.allocation.allocation_model[next - 2] = EXFAT_FAT_END_OF_CHAIN;
		OK(exfat_resize_initialize_stream_cursor(
		    &test.volume.geometry, &stream, SECTOR_CACHE_SOURCE_BITMAP_DATA, &chain, &cursor));
		OK(exfat_resize_skip_stream(&test.volume, &cursor, 512));
		OK(exfat_resize_read_stream(&test.volume, &cursor, actual, 512));
		CHECK(
		    exfat_fixture_read_sector(&test.fixture, sector_for(&test, next), expected, 512) == 0);
		CHECK(memcmp(expected, actual, 512) == 0);
	}
	chain.next = broken_chain;
	OK(exfat_resize_initialize_stream_cursor(
	    &test.volume.geometry, &stream, SECTOR_CACHE_SOURCE_BITMAP_DATA, &chain, &cursor));
	CHECK(exfat_resize_skip_stream(&test.volume, &cursor, 512) == EXFAT_RESIZE_IO_ERROR);
	CHECK(exfat_resize_skip_stream(&test.volume, &cursor, 2048) == EXFAT_RESIZE_OUT_OF_BOUNDS);
	destroy(&test);
}

int main(void)
{
	test_current_model_and_tail_observations();
	test_targeted_edit_after_directory_move();
	test_targeted_entry_set_spans_fragmented_clusters();
	test_system_entries_after_root_move();
	test_cancellation_deferral_covers_publication_and_failures();
	test_relocation_rejects_file_directory_and_upcase_length_changes();
	test_cache_coherence_and_explicit_publication();
	test_partial_direct_write_invalidates_cached_aliases();
	test_failed_cache_flush_retains_pending_data();
	test_incremental_fat_and_fragmented_bitmap_updates();
	test_stream_reader_uses_current_model_and_propagates_errors();
	if (failures != 0)
		fprintf(stderr, "%d mechanism checks failed\n", failures);
	return failures != 0;
}
