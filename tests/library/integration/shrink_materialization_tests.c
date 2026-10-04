/* SPDX-License-Identifier: MIT */

#include "boot_region.h"
#include "endian.h"
#include "stream.h"
#include "support/exfat_fixture.h"
#include "support/test_allocator.h"
#include "volume.h"
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

static uint16_t checksum(const unsigned char *entries)
{
	uint16_t sum = 0;
	size_t i;
	for (i = 0; i < 96; ++i) {
		if (i != 2 && i != 3)
			sum = (uint16_t)(((sum << 15) | (sum >> 1)) + entries[i]);
	}
	return sum;
}

static void file(unsigned char *entry, uint32_t first, uint32_t count, int contiguous)
{
	entry[0] = 0x85;
	entry[1] = 2;
	entry[4] = 0x21;
	entry[32] = 0xc0;
	entry[33] = contiguous ? 3 : 1;
	entry[35] = 1;
	(void)exfat_resize_store_le64(entry, 96, 40, (uint64_t)count * 256);
	(void)exfat_resize_store_le32(entry, 96, 52, first);
	(void)exfat_resize_store_le64(entry, 96, 56, (uint64_t)count * 512);
	entry[64] = 0xc1;
	entry[66] = contiguous ? 'C' : 'N';
	(void)exfat_resize_store_le16(entry, 96, 2, checksum(entry));
}

static uint32_t original_fat(const struct exfat_fixture *f, uint32_t cluster)
{
	uint32_t bitmap_clusters = (f->geometry.cluster_count + 7) / 8;
	bitmap_clusters = (bitmap_clusters + 511) / 512;
	if (cluster == 0)
		return UINT32_C(0xfffffff8);
	if (cluster <= 3 || cluster == f->crossing_first_cluster - 1)
		return EXFAT_FAT_END_OF_CHAIN;
	if (cluster < 4 + bitmap_clusters)
		return cluster + 1 == 4 + bitmap_clusters ? EXFAT_FAT_END_OF_CHAIN : cluster + 1;
	return UINT32_C(0xa5a5a5a5); /* Stale entries, including the NoFatChain stream. */
}

/* Sparse payload storage keeps the million-cluster case small. Only the last
 * file cluster lies above the target; its preceding neighbor is another file. */
static int initialize(struct exfat_fixture *f, uint32_t first, uint32_t count)
{
	unsigned char root[512] = { 0 };
	unsigned char data[512];
	unsigned char *fat = NULL;
	unsigned char *bitmap = NULL;
	uint32_t bitmap_bytes;
	uint32_t bitmap_clusters;
	uint32_t cluster;
	int result = -1;
	memset(f, 0, sizeof(*f));
	f->crossing_first_cluster = first;
	f->crossing_cluster_count = count;
	f->geometry.cluster_count = first + count - 2;
	f->geometry.fat_offset = 24;
	f->geometry.fat_length = exfat_resize_used_fat_sector_count(f->geometry.cluster_count, 512);
	f->geometry.cluster_heap_offset = 24 + f->geometry.fat_length;
	f->geometry.sectors_per_cluster = 1;
	f->geometry.root_directory_cluster = 2;
	f->geometry.volume_sector_count = f->geometry.cluster_heap_offset + f->geometry.cluster_count;
	memory_block_device_init(&f->memory, 512, f->geometry.volume_sector_count);
	bitmap_bytes = (f->geometry.cluster_count + 7) / 8;
	bitmap_clusters = (bitmap_bytes + 511) / 512;
	fat = malloc((size_t)f->geometry.fat_length * 512);
	bitmap = calloc(bitmap_clusters, 512);
	if (fat == NULL || bitmap == NULL)
		goto out;
	for (cluster = 0; cluster < f->geometry.fat_length * 128; ++cluster)
		(void)exfat_resize_store_le32(fat, (size_t)f->geometry.fat_length * 512,
		    (size_t)cluster * 4, original_fat(f, cluster));
	for (cluster = 2; cluster <= f->geometry.cluster_count + 1; ++cluster) {
		if (cluster < 4 + bitmap_clusters || cluster >= first - 1)
			bitmap[(cluster - 2) / 8] |= (unsigned char)(1u << ((cluster - 2) % 8));
	}
	root[0] = 0x81;
	(void)exfat_resize_store_le32(root, sizeof(root), 20, 4);
	(void)exfat_resize_store_le64(root, sizeof(root), 24, bitmap_bytes);
	root[32] = 0x82;
	(void)exfat_resize_store_le32(root, sizeof(root), 52, 3);
	(void)exfat_resize_store_le64(root, sizeof(root), 56, 512);
	file(root + 64, first, count, 1);
	file(root + 160, first - 1, 1, 0);
	if (exfat_fixture_write_boot_regions(f) != 0 ||
	    f->memory.device.write(&f->memory, 24, f->geometry.fat_length, fat) != 0 ||
	    f->memory.device.write(&f->memory, f->geometry.cluster_heap_offset, 1, root) != 0 ||
	    f->memory.device.write(&f->memory, exfat_fixture_cluster_sector(&f->geometry, 4),
	        bitmap_clusters, bitmap) != 0)
		goto out;
	memset(data, 0x77, sizeof(data));
	if (f->memory.device.write(
	        &f->memory, exfat_fixture_cluster_sector(&f->geometry, first - 1), 1, data) != 0)
		goto out;
	for (cluster = 0; cluster < 3; ++cluster) {
		uint32_t index = cluster == 0 ? 0 : cluster == 1 ? count / 2 : count - 1;
		memset(data, 0x11 * (cluster + 1), sizeof(data));
		if (f->memory.device.write(&f->memory,
		        exfat_fixture_cluster_sector(&f->geometry, first + index), 1, data) != 0)
			goto out;
	}
	result = memory_block_device_make_durable(&f->memory);
	memory_block_device_clear_operations(&f->memory);
out:
	free(fat);
	free(bitmap);
	return result;
}

struct monitor_state {
	struct memory_block_device *memory;
	size_t start;
	size_t cancel_at;
};

static int cancelled(void *opaque)
{
	struct monitor_state *state = opaque;
	return state->memory->operation_index >= state->cancel_at;
}

static void event(void *opaque, const struct exfat_resize_event *value)
{
	struct monitor_state *state = opaque;
	if (value->code == EXFAT_RESIZE_EVENT_CODE_STAGE_ENTERED &&
	    value->values[0] == EXFAT_RESIZE_STAGE_RESIZING)
		state->start = state->memory->operation_index;
}

static enum exfat_resize_error resize(
    struct exfat_fixture *f, struct monitor_state *state, enum exfat_resize_stage *stage)
{
	struct test_allocator allocator = { 0 };
	struct exfat_resize_allocator callbacks = test_allocator_callbacks(&allocator);
	struct exfat_resize_monitor monitor = {
		.context = state, .cancellation_requested = cancelled, .report_event = event
	};
	enum exfat_resize_error error = exfat_fixture_resize_with_monitor(
	    &f->memory.device, f->geometry.volume_sector_count - 1, &callbacks, &monitor, stage);
	CHECK(test_allocator_is_clean(&allocator));
	return error;
}

static void verify(struct exfat_fixture *f, int shrunk)
{
	unsigned char root[512];
	unsigned char data[512];
	unsigned char *fat = malloc((size_t)f->geometry.fat_length * 512);
	uint32_t current = f->crossing_first_cluster;
	uint32_t last = current;
	uint32_t first = current;
	uint32_t count = f->crossing_cluster_count;
	uint32_t i;
	uint16_t sum = 0;
	uint64_t length = 0;
	uint32_t value = 0;
	size_t operation;
	CHECK(fat != NULL);
	if (fat == NULL)
		return;
	/* All retained file data must remain untouched. Verify the complete chain
	 * below, and the copied tail plus head/middle markers. Other payload is zero. */
	for (operation = 0; operation < f->memory.operation_count; ++operation) {
		const struct memory_operation *op = &f->memory.operations[operation];
		uint64_t start = exfat_fixture_cluster_sector(&f->geometry, first);
		if (op->kind == MEMORY_OPERATION_WRITE)
			CHECK(
			    op->first_sector >= start + count || op->first_sector + op->sector_count <= start);
	}
	CHECK(exfat_fixture_read_sector(f, 0, data, sizeof(data)) == 0);
	CHECK(exfat_resize_load_le64(data, sizeof(data), 72, &length) == EXFAT_RESIZE_SUCCESS);
	CHECK(length == f->geometry.volume_sector_count - shrunk);
	CHECK(exfat_resize_load_le32(data, sizeof(data), 92, &value) == EXFAT_RESIZE_SUCCESS);
	CHECK(value == f->geometry.cluster_count - shrunk);
	CHECK(exfat_fixture_read_sector(f, f->geometry.cluster_heap_offset, root, sizeof(root)) == 0);
	CHECK(f->memory.device.read(&f->memory, 24, f->geometry.fat_length, fat) == 0);
	CHECK(exfat_resize_load_le32(root, sizeof(root), 116, &value) == EXFAT_RESIZE_SUCCESS);
	CHECK(value == first);
	CHECK(root[97] == 1 || root[97] == 3);
	if (shrunk)
		CHECK(root[97] == 1);
	CHECK(exfat_resize_load_le16(root, sizeof(root), 66, &sum) == EXFAT_RESIZE_SUCCESS);
	CHECK(sum == checksum(root + 64));
	CHECK(exfat_resize_load_le64(root, sizeof(root), 104, &length) == EXFAT_RESIZE_SUCCESS);
	CHECK(length == (uint64_t)count * 256);
	CHECK(exfat_resize_load_le64(root, sizeof(root), 120, &length) == EXFAT_RESIZE_SUCCESS);
	CHECK(length == (uint64_t)count * 512);
	CHECK(exfat_resize_load_le16(root, sizeof(root), 162, &sum) == EXFAT_RESIZE_SUCCESS);
	CHECK(sum == checksum(root + 160));
	CHECK(exfat_fixture_read_sector(
	          f, exfat_fixture_cluster_sector(&f->geometry, first - 1), data, sizeof(data)) == 0);
	for (i = 0; i < sizeof(data); ++i)
		CHECK(data[i] == 0x77);
	for (i = 0; i < count; ++i) {
		uint32_t next = 0;
		CHECK(current >= 2 && current <= f->geometry.cluster_count + 1);
		if (current < 2 || current > f->geometry.cluster_count + 1)
			break;
		if (i + 1 != count)
			CHECK(current == first + i);
		last = current;
		if (i == 0 || i == count / 2 || i + 1 == count) {
			size_t byte;
			unsigned char marker = i == 0 ? 0x11 : i == count / 2 ? 0x22 : 0x33;
			CHECK(exfat_fixture_read_sector(f, exfat_fixture_cluster_sector(&f->geometry, current),
			          data, sizeof(data)) == 0);
			for (byte = 0; byte < sizeof(data); ++byte)
				CHECK(data[byte] == marker);
		}
		if (root[97] & 2)
			current++;
		else {
			CHECK(exfat_resize_load_le32(fat, (size_t)f->geometry.fat_length * 512,
			          (size_t)current * 4, &next) == EXFAT_RESIZE_SUCCESS);
			current = next;
		}
	}
	CHECK(i == count);
	if (!(root[97] & 2))
		CHECK(current == EXFAT_FAT_END_OF_CHAIN);
	for (i = 0; i < f->geometry.fat_length * 128; ++i) {
		uint32_t actual = 0;
		if ((i >= first && i < first + count) || i == last)
			continue;
		CHECK(exfat_resize_load_le32(fat, (size_t)f->geometry.fat_length * 512, (size_t)i * 4,
		          &actual) == EXFAT_RESIZE_SUCCESS);
		CHECK(actual == original_fat(f, i));
	}
	free(fat);
}

static void test_batches(uint32_t first, uint32_t count, size_t reads, size_t writes, int faults)
{
	struct exfat_fixture reference;
	struct monitor_state baseline = { .memory = &reference.memory, .cancel_at = SIZE_MAX };
	enum exfat_resize_stage stage;
	size_t end = 0;
	size_t index;
	size_t actual_reads = 0;
	size_t actual_writes = 0;
	size_t publication_writes = 0;
	size_t first_batch_end = 0;
	unsigned int syncs = 0;
	CHECK(initialize(&reference, first, count) == 0);
	CHECK(resize(&reference, &baseline, &stage) == EXFAT_RESIZE_SUCCESS);
	CHECK(stage == EXFAT_RESIZE_STAGE_COMPLETED);
	for (index = baseline.start; index < reference.memory.operation_count; ++index) {
		const struct memory_operation *op = &reference.memory.operations[index];
		if (op->kind == MEMORY_OPERATION_SYNC) {
			if (++syncs == 2) {
				end = index + 1; /* Include flag/checksum publication and its sync. */
				break;
			}
		} else if (syncs == 0) {
			CHECK(op->first_sector >= 24 &&
			    op->first_sector + op->sector_count <= reference.geometry.cluster_heap_offset);
			CHECK((uint64_t)op->sector_count * 512 <= EXFAT_IO_BUFFER_SIZE);
			if (op->kind == MEMORY_OPERATION_READ) {
				CHECK(op->sector_count == 1);
				++actual_reads;
			} else {
				++actual_writes;
				if (actual_writes == 1)
					first_batch_end = index + 1;
			}
		} else {
			CHECK(op->first_sector == reference.geometry.cluster_heap_offset &&
			    op->sector_count == 1);
			if (op->kind == MEMORY_OPERATION_WRITE)
				++publication_writes;
		}
	}
	CHECK(
	    syncs == 2 && actual_reads == reads && actual_writes == writes && publication_writes == 1);
	CHECK(memory_block_device_crash(&reference.memory) == 0);
	verify(&reference, 1);
	if (faults && actual_writes == writes) {
		for (index = baseline.start; index < end; ++index) {
			const struct memory_operation op = reference.memory.operations[index];
			int mode;
			for (mode = 0; mode < 4; ++mode) {
				struct exfat_fixture f;
				struct monitor_state state = { .memory = &f.memory, .cancel_at = SIZE_MAX };
				enum exfat_resize_error error;
				unsigned char boot[512];
				uint16_t flags = 0;
				if (mode == 2 && (op.kind == MEMORY_OPERATION_SYNC || op.sector_count < 2))
					continue;
				CHECK(initialize(&f, first, count) == 0);
				if (mode == 0)
					memory_block_device_fail_operation(&f.memory, index, EIO);
				else if (mode < 3)
					memory_block_device_fail_after_operation(
					    &f.memory, index, mode == 1 ? op.sector_count : op.sector_count / 2, EIO);
				else
					state.cancel_at = index;
				error = resize(&f, &state, &stage);
				if (mode == 3) {
					CHECK(error == EXFAT_RESIZE_CANCELLED &&
					    stage == EXFAT_RESIZE_STAGE_SOURCE_READY);
				} else {
					CHECK(error == EXFAT_RESIZE_IO_ERROR && stage == EXFAT_RESIZE_STAGE_RESIZING);
					CHECK(f.memory.operation_index == index + 1);
				}
				memory_block_device_clear_failure(&f.memory);
				if (op.kind == MEMORY_OPERATION_WRITE && mode != 3)
					CHECK(memory_block_device_persist_range(&f.memory, op.first_sector, 1) == 0);
				CHECK(memory_block_device_crash(&f.memory) == 0);
				CHECK(exfat_fixture_read_sector(&f, 0, boot, sizeof(boot)) == 0);
				CHECK(exfat_resize_load_le16(boot, sizeof(boot), 106, &flags) ==
				    EXFAT_RESIZE_SUCCESS);
				CHECK(((flags & 2) == 0) == (mode == 3));
				if (mode == 3 && index <= first_batch_end) {
					CHECK(exfat_fixture_read_sector(
					          &f, f.geometry.cluster_heap_offset, boot, sizeof(boot)) == 0);
					CHECK((boot[97] & 2) != 0); /* Cancellation polled before the next batch. */
				}
				verify(&f, 0);
				CHECK(exfat_fixture_destroy(&f) == 0);
			}
		}
	}
	CHECK(exfat_fixture_destroy(&reference) == 0);
}

int main(void)
{
	test_batches(2369, 1000000, 2, 4, 0);
	test_batches(2304, 262144, 0, 1, 0); /* Both boundaries are sector-aligned. */
	test_batches(2304, 262145, 1, 2, 0); /* Only the final sector is partial. */
	test_batches(2369, 262079, 1, 1, 0); /* Only the first sector is partial. */
	test_batches(2369, 4, 1, 1, 0);      /* Both boundaries share one sector. */
	test_batches(2369, 262145, 2, 2, 1);
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
