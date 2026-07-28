/* SPDX-License-Identifier: MIT */

#include "exfat_resize.h"

#include "boot_region.h"
#include "endian.h"
#include "geometry.h"
#include "support/exfat_fixture.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
	SECTOR_SIZE = 512,
	TARGET_SECTOR_COUNT = 65536,
	WORK_BUFFER_SIZE = 1024 * 1024,
	VOLUME_FLAGS_OFFSET = 106,
	ENTRY_BITMAP = 0x81,
	ENTRY_FILE = 0x85,
	ENTRY_STREAM = 0xc0,
	ENTRY_FILE_NAME = 0xc1,
	ENTRY_VENDOR_EXTENSION = 0xe0,
	ENTRY_VENDOR_ALLOCATION = 0xe1,
	ALLOCATION_POSSIBLE = 0x01,
	NO_FAT_CHAIN = 0x02
};

#define FAT_BAD_CLUSTER UINT32_C(0xfffffff7)
#define MAX_DIRECTORY_SIZE (UINT64_C(256) * 1024 * 1024)

static int failure_count;

struct allocator_state {
	struct memory_block_device *device;
	size_t allocation_count;
	size_t deallocation_count;
	size_t largest_allocation_size;
	size_t live_size;
	size_t fail_on_allocation;
	int allocation_after_write;
};

#define CHECK(expression) \
	do { \
		if (!(expression)) { \
			fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expression); \
			++failure_count; \
		} \
	} while (0)

static void *allocate_memory(void *context, size_t size)
{
	(void)context;
	return malloc(size);
}

static void deallocate_memory(void *context, void *memory, size_t size)
{
	(void)context;
	(void)size;
	free(memory);
}

static struct exfat_resize_options resize_options(void)
{
	struct exfat_resize_options options;

	options.allocator.context = NULL;
	options.allocator.allocate = allocate_memory;
	options.allocator.deallocate = deallocate_memory;

	return options;
}

static enum exfat_resize_error plan_fixture_growth(struct exfat_fixture *fixture,
    uint64_t target_sector_count,
    struct exfat_resize_geometry *target)
{
	struct exfat_resize_device_geometry device_geometry;

	device_geometry.logical_sector_size = fixture->memory.device.sector_size;
	device_geometry.sector_count = fixture->memory.device.sector_count;
	return exfat_resize_plan_growth(
	    &device_geometry, &fixture->geometry, target_sector_count, target);
}

static void *tracked_allocate(void *context, size_t size)
{
	struct allocator_state *state = context;
	void *memory;
	size_t index;

	CHECK(size != 0);
	++state->allocation_count;
	if (size > state->largest_allocation_size)
		state->largest_allocation_size = size;
	if (state->device != NULL) {
		for (index = 0; index < state->device->operation_count; ++index) {
			if (state->device->operations[index].kind == MEMORY_OPERATION_WRITE)
				state->allocation_after_write = 1;
		}
	}
	if (state->allocation_count == state->fail_on_allocation)
		return NULL;
	memory = malloc(size);
	if (memory != NULL)
		state->live_size += size;
	return memory;
}

static void tracked_deallocate(void *context, void *memory, size_t size)
{
	struct allocator_state *state = context;

	CHECK(memory != NULL);
	CHECK(size != 0);
	CHECK(size <= state->live_size);
	if (size <= state->live_size)
		state->live_size -= size;
	++state->deallocation_count;
	free(memory);
}

static uint32_t load_fat_entry(
    struct exfat_fixture *fixture, const struct exfat_resize_geometry *geometry, uint32_t cluster)
{
	unsigned char sector[SECTOR_SIZE];
	uint64_t byte_offset = (uint64_t)cluster * 4;
	uint32_t value = 0;

	CHECK(exfat_fixture_read_sector(fixture, geometry->fat_offset + byte_offset / SECTOR_SIZE,
	          sector, sizeof(sector)) == 0);
	CHECK(exfat_resize_load_le32(sector, sizeof(sector), (size_t)(byte_offset % SECTOR_SIZE),
	          &value) == EXFAT_RESIZE_SUCCESS);
	return value;
}

static int store_fat_entry(struct exfat_fixture *fixture,
    const struct exfat_resize_geometry *geometry,
    uint32_t cluster,
    uint32_t value)
{
	unsigned char sector[SECTOR_SIZE];
	uint64_t byte_offset = (uint64_t)cluster * 4;
	uint64_t sector_number = geometry->fat_offset + byte_offset / SECTOR_SIZE;

	if (exfat_fixture_read_sector(fixture, sector_number, sector, sizeof(sector)) != 0)
		return -1;
	if (exfat_resize_store_le32(sector, sizeof(sector), (size_t)(byte_offset % SECTOR_SIZE),
	        value) != EXFAT_RESIZE_SUCCESS)
		return -1;
	return fixture->memory.device.write(fixture->memory.device.context, sector_number, 1, sector);
}

static int set_bitmap_cluster(struct exfat_fixture *fixture, uint32_t cluster, int allocated)
{
	unsigned char sector[SECTOR_SIZE];
	uint32_t bit = cluster - 2;
	uint64_t cluster_size = (uint64_t)fixture->geometry.sectors_per_cluster * SECTOR_SIZE;
	uint32_t bitmap_cluster = (uint32_t)((bit / 8) / cluster_size);
	uint32_t bitmap_sector = (uint32_t)(((bit / 8) % cluster_size) / SECTOR_SIZE);
	uint64_t sector_number;
	unsigned char mask = (unsigned char)(1u << (bit % 8));

	if (bitmap_cluster >= fixture->bitmap_cluster_count)
		return -1;
	sector_number =
	    exfat_fixture_cluster_sector(&fixture->geometry, fixture->bitmap_clusters[bitmap_cluster]) +
	    bitmap_sector;
	if (exfat_fixture_read_sector(fixture, sector_number, sector, sizeof(sector)) != 0)
		return -1;
	if (allocated)
		sector[bit / 8 % SECTOR_SIZE] |= mask;
	else
		sector[bit / 8 % SECTOR_SIZE] &= (unsigned char)~mask;
	return fixture->memory.device.write(fixture->memory.device.context, sector_number, 1, sector);
}

static int mark_bad_cluster(struct exfat_fixture *fixture, uint32_t cluster)
{
	if (store_fat_entry(fixture, &fixture->geometry, cluster, FAT_BAD_CLUSTER) != 0)
		return -1;
	return set_bitmap_cluster(fixture, cluster, 1);
}

static void check_operations_are_read_only(const struct exfat_fixture *fixture)
{
	size_t index;

	for (index = 0; index < fixture->memory.operation_count; ++index)
		CHECK(fixture->memory.operations[index].kind == MEMORY_OPERATION_READ);
}

static void check_sector_is_not_written(const struct exfat_fixture *fixture, uint64_t sector)
{
	size_t index;

	for (index = 0; index < fixture->memory.operation_count; ++index) {
		const struct memory_operation *operation = &fixture->memory.operations[index];

		if (operation->kind == MEMORY_OPERATION_WRITE && sector >= operation->first_sector)
			CHECK(sector - operation->first_sector >= operation->sector_count);
	}
}

static void check_sector_is_not_read(const struct exfat_fixture *fixture, uint64_t sector)
{
	size_t index;

	for (index = 0; index < fixture->memory.operation_count; ++index) {
		const struct memory_operation *operation = &fixture->memory.operations[index];

		if (operation->kind == MEMORY_OPERATION_READ && sector >= operation->first_sector)
			CHECK(sector - operation->first_sector >= operation->sector_count);
	}
}

static size_t sector_operation_count(
    const struct exfat_fixture *fixture, enum memory_operation_kind kind, uint64_t sector)
{
	size_t count = 0;
	size_t index;

	for (index = 0; index < fixture->memory.operation_count; ++index) {
		const struct memory_operation *operation = &fixture->memory.operations[index];

		if (operation->kind == kind && sector >= operation->first_sector &&
		    sector - operation->first_sector < operation->sector_count)
			++count;
	}
	return count;
}

static uint16_t entry_checksum_byte(uint16_t checksum, unsigned char value)
{
	uint16_t rotated = (uint16_t)(((uint32_t)checksum << 15) | ((uint32_t)checksum >> 1));

	return (uint16_t)(rotated + value);
}

static uint16_t entry_set_checksum_bytes(const unsigned char *entries, size_t byte_count)
{
	uint16_t checksum = 0;
	size_t index;

	for (index = 0; index < byte_count; ++index) {
		if (index != 2 && index != 3)
			checksum = entry_checksum_byte(checksum, entries[index]);
	}
	return checksum;
}

static uint16_t entry_set_checksum_count(const unsigned char *entries, size_t entry_count)
{
	return entry_set_checksum_bytes(entries, 32 * entry_count);
}

static uint16_t entry_set_checksum(const unsigned char entries[32 * 3])
{
	return entry_set_checksum_count(entries, 3);
}

static int configure_checksum_wrap_entry_set(struct exfat_fixture *fixture)
{
	static const unsigned char wrap_prefix[] = { 1, 83, 255, 255, 255, 255 };
	unsigned char child[SECTOR_SIZE];
	unsigned char *vendor;
	uint64_t child_sector = exfat_fixture_cluster_sector(&fixture->geometry, 6);
	uint16_t checksum;

	if (exfat_fixture_read_sector(fixture, child_sector, child, sizeof(child)) != 0)
		return -1;
	child[1] = 3;
	vendor = child + 32 * 3;
	memset(vendor, 0, 32);
	vendor[0] = ENTRY_VENDOR_EXTENSION;
	memcpy(vendor + 2, wrap_prefix, sizeof(wrap_prefix));
	vendor[2 + sizeof(wrap_prefix)] = 1;
	if (entry_set_checksum_bytes(child, 32 * 3 + 2 + sizeof(wrap_prefix)) != UINT16_MAX)
		return -1;

	checksum = entry_set_checksum_count(child, 4);
	if (exfat_resize_store_le16(child, 32, 2, checksum) != EXFAT_RESIZE_SUCCESS)
		return -1;
	if (fixture->memory.device.write(fixture->memory.device.context, child_sector, 1, child) != 0)
		return -1;
	memory_block_device_clear_operations(&fixture->memory);
	return 0;
}

static int configure_crossing_file_entry_set(
    struct exfat_fixture *fixture, uint32_t continuation_cluster, uint32_t file_cluster)
{
	unsigned char continuation[SECTOR_SIZE] = { 0 };
	unsigned char entry_set[32 * 3] = { 0 };
	unsigned char root[SECTOR_SIZE];
	uint64_t continuation_sector;
	uint64_t root_sector;
	uint16_t checksum;
	size_t offset;

	root_sector = exfat_fixture_cluster_sector(&fixture->geometry, 2);
	continuation_sector = exfat_fixture_cluster_sector(&fixture->geometry, continuation_cluster);
	if (exfat_fixture_read_sector(fixture, root_sector, root, sizeof(root)) != 0)
		return -1;

	/*
	 * Keep the existing entries, fill the gap with unused entries, and put
	 * the primary entry at the end of the first root-directory cluster.
	 */
	for (offset = 0; offset < SECTOR_SIZE - 32; offset += 32) {
		if (root[offset] == 0)
			root[offset] = 1;
	}

	entry_set[0] = ENTRY_FILE;
	entry_set[1] = 2;
	entry_set[32] = ENTRY_STREAM;
	entry_set[33] = ALLOCATION_POSSIBLE | NO_FAT_CHAIN;
	entry_set[35] = 1;
	entry_set[64] = ENTRY_FILE_NAME;
	if (exfat_resize_store_le64(entry_set + 32, 32, 8, SECTOR_SIZE) != EXFAT_RESIZE_SUCCESS ||
	    exfat_resize_store_le32(entry_set + 32, 32, 20, file_cluster) != EXFAT_RESIZE_SUCCESS ||
	    exfat_resize_store_le64(entry_set + 32, 32, 24, SECTOR_SIZE) != EXFAT_RESIZE_SUCCESS ||
	    exfat_resize_store_le16(entry_set + 64, 32, 2, 'I') != EXFAT_RESIZE_SUCCESS)
		return -1;
	checksum = entry_set_checksum(entry_set);
	if (exfat_resize_store_le16(entry_set, 32, 2, checksum) != EXFAT_RESIZE_SUCCESS)
		return -1;

	memcpy(root + SECTOR_SIZE - 32, entry_set, 32);
	memcpy(continuation, entry_set + 32, 64);
	if (fixture->memory.device.write(fixture->memory.device.context, root_sector, 1, root) != 0 ||
	    fixture->memory.device.write(
	        fixture->memory.device.context, continuation_sector, 1, continuation) != 0 ||
	    store_fat_entry(fixture, &fixture->geometry, 2, continuation_cluster) != 0 ||
	    store_fat_entry(fixture, &fixture->geometry, continuation_cluster, UINT32_C(0xffffffff)) !=
	        0 ||
	    set_bitmap_cluster(fixture, continuation_cluster, 1) != 0 ||
	    set_bitmap_cluster(fixture, file_cluster, 1) != 0)
		return -1;

	memory_block_device_clear_operations(&fixture->memory);
	return 0;
}

static int operation_contains_sector(const struct memory_operation *operation, uint64_t sector)
{
	return sector >= operation->first_sector &&
	    sector - operation->first_sector < operation->sector_count;
}

static int bitmap_cluster_is_set(struct exfat_fixture *fixture,
    const struct exfat_resize_geometry *geometry,
    uint32_t bitmap_cluster,
    uint32_t cluster)
{
	unsigned char sector[SECTOR_SIZE];
	uint64_t byte_index = ((uint64_t)cluster - 2) / 8;
	uint64_t first_sector = exfat_fixture_cluster_sector(geometry, bitmap_cluster);

	CHECK(exfat_fixture_read_sector(
	          fixture, first_sector + byte_index / SECTOR_SIZE, sector, sizeof(sector)) == 0);
	return (sector[byte_index % SECTOR_SIZE] & (1u << ((cluster - 2) % 8))) != 0;
}

static void check_cluster_marker(struct exfat_fixture *fixture,
    const struct exfat_resize_geometry *geometry,
    uint32_t cluster,
    unsigned char marker)
{
	unsigned char sector[SECTOR_SIZE];
	size_t index;

	CHECK(exfat_fixture_read_sector(fixture, exfat_fixture_cluster_sector(geometry, cluster),
	          sector, sizeof(sector)) == 0);
	for (index = 0; index < sizeof(sector); ++index)
		CHECK(sector[index] == marker);
}

static unsigned char expected_cluster_byte(uint32_t source_cluster, uint64_t cluster_byte_offset)
{
	return (unsigned char)(((uint64_t)source_cluster * 131 + cluster_byte_offset * 29) % 251 + 1);
}

static void check_cluster_pattern(struct exfat_fixture *fixture,
    const struct exfat_resize_geometry *geometry,
    uint32_t source_cluster,
    uint32_t target_cluster)
{
	unsigned char sector[SECTOR_SIZE];
	uint32_t sector_index;
	size_t byte_index;

	for (sector_index = 0; sector_index < geometry->sectors_per_cluster; ++sector_index) {
		CHECK(exfat_fixture_read_sector(fixture,
		          exfat_fixture_cluster_sector(geometry, target_cluster) + sector_index, sector,
		          sizeof(sector)) == 0);
		for (byte_index = 0; byte_index < sizeof(sector); ++byte_index) {
			uint64_t cluster_byte_offset = (uint64_t)sector_index * SECTOR_SIZE + byte_index;

			CHECK(sector[byte_index] == expected_cluster_byte(source_cluster, cluster_byte_offset));
		}
	}
}

static void test_resize(void)
{
	struct allocator_state allocator = { 0 };
	struct exfat_resize_geometry target;
	struct exfat_resize_geometry read_back;
	struct exfat_resize_options options;
	struct exfat_fixture fixture;
	unsigned char workspace[SECTOR_SIZE * 2 + 13];
	unsigned char root[SECTOR_SIZE];
	enum exfat_resize_error error;
	uint32_t mapped;
	uint32_t bitmap_cluster;
	uint64_t bitmap_length;
	uint32_t index;
	uint32_t next_source_cluster;
	uint32_t source_cluster;
	uint32_t target_cluster;
	uint32_t next_cluster;

	CHECK(exfat_fixture_initialize(&fixture, TARGET_SECTOR_COUNT + 1) == 0);
	error = plan_fixture_growth(&fixture, TARGET_SECTOR_COUNT, &target);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	CHECK(target.cluster_heap_offset > fixture.geometry.cluster_heap_offset);

	options = resize_options();
	options.allocator.context = &allocator;
	options.allocator.allocate = tracked_allocate;
	options.allocator.deallocate = tracked_deallocate;
	error = exfat_resize(&fixture.memory.device,
	    exfat_fixture_target_size(TARGET_SECTOR_COUNT) + SECTOR_SIZE - 1, &options, NULL);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	CHECK(allocator.allocation_count == 3);
	CHECK(allocator.deallocation_count == 3);
	CHECK(allocator.largest_allocation_size == WORK_BUFFER_SIZE);
	CHECK(allocator.live_size == 0);
	if (error != EXFAT_RESIZE_SUCCESS) {
		exfat_fixture_destroy(&fixture);
		return;
	}

	error = exfat_resize_read_boot_regions(
	    &fixture.memory.device, workspace, sizeof(workspace), &read_back);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	CHECK(memcmp(&read_back, &target, sizeof(target)) == 0);

	error = exfat_resize_map_growth_cluster(&fixture.geometry, &target, 2, &mapped);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	CHECK(mapped == target.root_directory_cluster);
	CHECK(exfat_fixture_read_sector(
	          &fixture, exfat_fixture_cluster_sector(&target, mapped), root, sizeof(root)) == 0);
	CHECK(root[0] == 0x81);
	CHECK(exfat_resize_load_le32(root, sizeof(root), 20, &bitmap_cluster) == EXFAT_RESIZE_SUCCESS);
	CHECK(exfat_resize_load_le64(root, sizeof(root), 24, &bitmap_length) == EXFAT_RESIZE_SUCCESS);
	CHECK(bitmap_length == ((uint64_t)target.cluster_count + 7) / 8);
	CHECK((root[32 * 9 + 1] & 0x02) == 0);
	CHECK(
	    exfat_resize_load_le32(root + 32, sizeof(root) - 32, 20, &mapped) == EXFAT_RESIZE_SUCCESS);
	check_cluster_marker(&fixture, &target, mapped, 0x44);

	error = exfat_resize_map_growth_cluster(&fixture.geometry, &target, 5, &mapped);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	check_cluster_marker(&fixture, &target, mapped, 0x55);
	error = exfat_resize_map_growth_cluster(&fixture.geometry, &target, 8, &mapped);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	check_cluster_marker(&fixture, &target, mapped, 0x88);

	for (index = 0; index < fixture.crossing_cluster_count; ++index) {
		source_cluster = fixture.crossing_first_cluster + index;
		error = exfat_resize_map_growth_cluster(
		    &fixture.geometry, &target, source_cluster, &target_cluster);
		CHECK(error == EXFAT_RESIZE_SUCCESS);
		check_cluster_pattern(&fixture, &target, source_cluster, target_cluster);
		next_cluster = load_fat_entry(&fixture, &target, target_cluster);
		if (index + 1 == fixture.crossing_cluster_count) {
			CHECK(next_cluster == UINT32_C(0xffffffff));
		} else {
			error = exfat_resize_map_growth_cluster(
			    &fixture.geometry, &target, source_cluster + 1, &mapped);
			CHECK(error == EXFAT_RESIZE_SUCCESS);
			CHECK(next_cluster == mapped);
		}
	}

	for (index = 0; index < 3; ++index) {
		source_cluster = fixture.fragmented_clusters[index];
		error = exfat_resize_map_growth_cluster(
		    &fixture.geometry, &target, source_cluster, &target_cluster);
		CHECK(error == EXFAT_RESIZE_SUCCESS);
		check_cluster_pattern(&fixture, &target, source_cluster, target_cluster);
		next_cluster = load_fat_entry(&fixture, &target, target_cluster);
		if (index + 1 == 3) {
			CHECK(next_cluster == UINT32_C(0xffffffff));
		} else {
			next_source_cluster = fixture.fragmented_clusters[index + 1];
			error = exfat_resize_map_growth_cluster(
			    &fixture.geometry, &target, next_source_cluster, &mapped);
			CHECK(error == EXFAT_RESIZE_SUCCESS);
			CHECK(next_cluster == mapped);
		}
	}

	for (index = 0; index < fixture.bitmap_cluster_count; ++index) {
		error = exfat_resize_map_growth_cluster(
		    &fixture.geometry, &target, fixture.bitmap_clusters[index], &mapped);
		CHECK(error == EXFAT_RESIZE_SUCCESS);
		CHECK(load_fat_entry(&fixture, &target, mapped) == 0);
		CHECK(!bitmap_cluster_is_set(&fixture, &target, bitmap_cluster, mapped));
	}
	CHECK(bitmap_cluster_is_set(&fixture, &target, bitmap_cluster, bitmap_cluster));
	CHECK(load_fat_entry(&fixture, &target, bitmap_cluster) != 0);

	exfat_fixture_destroy(&fixture);
}

static void test_entry_checksum_unsigned_wrap(void)
{
	struct exfat_resize_geometry target;
	struct exfat_resize_options options = resize_options();
	struct exfat_fixture fixture;
	unsigned char child[SECTOR_SIZE];
	enum exfat_resize_error error;
	uint16_t stored_checksum = 0;
	uint32_t mapped_child = 0;

	CHECK(exfat_fixture_initialize(&fixture, TARGET_SECTOR_COUNT + 1) == 0);
	if (configure_checksum_wrap_entry_set(&fixture) != 0) {
		CHECK(0);
		exfat_fixture_destroy(&fixture);
		return;
	}
	error = plan_fixture_growth(&fixture, TARGET_SECTOR_COUNT, &target);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	error = exfat_fixture_resize(&fixture.memory.device, TARGET_SECTOR_COUNT, &options, NULL);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	if (error != EXFAT_RESIZE_SUCCESS) {
		exfat_fixture_destroy(&fixture);
		return;
	}

	error = exfat_resize_map_growth_cluster(&fixture.geometry, &target, 6, &mapped_child);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	CHECK(exfat_fixture_read_sector(&fixture, exfat_fixture_cluster_sector(&target, mapped_child),
	          child, sizeof(child)) == 0);
	CHECK(child[1] == 3);
	CHECK(child[32 * 3] == ENTRY_VENDOR_EXTENSION);
	CHECK(
	    exfat_resize_load_le16(child, sizeof(child), 2, &stored_checksum) == EXFAT_RESIZE_SUCCESS);
	CHECK(stored_checksum == entry_set_checksum_count(child, 4));
	exfat_fixture_destroy(&fixture);
}

static void test_fat_boundary_geometry(void)
{
	const uint64_t target_sector_count = 29951;
	struct exfat_resize_geometry target;
	struct exfat_resize_geometry read_back;
	struct exfat_resize_options options;
	struct exfat_fixture fixture;
	unsigned char workspace[SECTOR_SIZE * 2];
	enum exfat_resize_error error;
	uint32_t mapped;

	CHECK(exfat_fixture_initialize(&fixture, target_sector_count) == 0);
	error = plan_fixture_growth(&fixture, target_sector_count, &target);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	if (error != EXFAT_RESIZE_SUCCESS) {
		exfat_fixture_destroy(&fixture);
		return;
	}
	CHECK(target.fat_length == 232);
	CHECK(target.cluster_heap_offset == 257);
	CHECK(target.cluster_count == 29694);
	CHECK(target.cluster_count ==
	    (target.volume_sector_count - target.cluster_heap_offset) / target.sectors_per_cluster);

	options = resize_options();
	error = exfat_fixture_resize(&fixture.memory.device, target_sector_count, &options, NULL);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	if (error == EXFAT_RESIZE_SUCCESS) {
		error = exfat_resize_read_boot_regions(
		    &fixture.memory.device, workspace, sizeof(workspace), &read_back);
		CHECK(error == EXFAT_RESIZE_SUCCESS);
		CHECK(memcmp(&read_back, &target, sizeof(target)) == 0);

		error = exfat_resize_map_growth_cluster(&fixture.geometry, &target, 5, &mapped);
		CHECK(error == EXFAT_RESIZE_SUCCESS);
		check_cluster_marker(&fixture, &target, mapped, 0x55);
	}
	exfat_fixture_destroy(&fixture);
}

static void test_fat_padding_is_not_written(void)
{
	const uint64_t target_sector_count = 12100;
	struct exfat_resize_geometry target;
	struct exfat_resize_geometry read_back;
	struct exfat_resize_options options;
	struct exfat_fixture fixture;
	unsigned char workspace[SECTOR_SIZE * 2];
	enum exfat_resize_error error;
	uint64_t required_fat_sectors;
	uint64_t sector;

	CHECK(exfat_fixture_initialize(&fixture, target_sector_count) == 0);
	fixture.geometry.fat_length = 200;
	CHECK(exfat_fixture_write_boot_regions(&fixture) == 0);
	error = plan_fixture_growth(&fixture, target_sector_count, &target);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	required_fat_sectors = ((uint64_t)target.cluster_count + 2) * 4;
	required_fat_sectors = (required_fat_sectors + SECTOR_SIZE - 1) / SECTOR_SIZE;
	CHECK(required_fat_sectors < target.fat_length);
	CHECK(target.fat_length == fixture.geometry.fat_length);
	memory_block_device_clear_operations(&fixture.memory);

	options = resize_options();
	error = exfat_fixture_resize(&fixture.memory.device, target_sector_count, &options, NULL);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	if (error == EXFAT_RESIZE_SUCCESS) {
		CHECK(load_fat_entry(&fixture, &target, target.cluster_count + 1) == UINT32_C(0xffffffff));
		error = exfat_resize_read_boot_regions(
		    &fixture.memory.device, workspace, sizeof(workspace), &read_back);
		CHECK(error == EXFAT_RESIZE_SUCCESS);
		CHECK(read_back.fat_length == fixture.geometry.fat_length);
	}
	for (sector = required_fat_sectors; sector < target.fat_length; ++sector)
		check_sector_is_not_written(&fixture, (uint64_t)target.fat_offset + sector);
	exfat_fixture_destroy(&fixture);
}

static void test_preflight_is_read_only(void)
{
	struct exfat_resize_options options;
	struct exfat_fixture fixture;
	unsigned char root[SECTOR_SIZE];
	enum exfat_resize_error error;
	enum exfat_resize_stage stage;

	CHECK(exfat_fixture_initialize(&fixture, TARGET_SECTOR_COUNT) == 0);
	CHECK(exfat_fixture_read_sector(&fixture, exfat_fixture_cluster_sector(&fixture.geometry, 2),
	          root, sizeof(root)) == 0);
	root[32 * 3 + 5] ^= 1;
	CHECK(fixture.memory.device.write(fixture.memory.device.context,
	          exfat_fixture_cluster_sector(&fixture.geometry, 2), 1, root) == 0);
	memory_block_device_clear_operations(&fixture.memory);

	options = resize_options();
	error = exfat_fixture_resize(&fixture.memory.device, TARGET_SECTOR_COUNT, &options, &stage);
	CHECK(error == EXFAT_RESIZE_INVALID_FILESYSTEM);
	CHECK(stage == EXFAT_RESIZE_STAGE_PREFLIGHT);
	check_operations_are_read_only(&fixture);
	exfat_fixture_destroy(&fixture);
}

static void test_invalid_entry_checksum_does_not_direct_fat_reads(void)
{
	const uint32_t untrusted_cluster = 200;
	struct exfat_resize_options options = resize_options();
	struct exfat_fixture fixture;
	unsigned char root[SECTOR_SIZE];
	unsigned char *entry_set = root + 32 * 2;
	unsigned char *stream = entry_set + 32;
	enum exfat_resize_error error;
	enum exfat_resize_stage stage = EXFAT_RESIZE_STAGE_COMPLETED;
	uint64_t root_sector;
	uint64_t untrusted_fat_sector;
	uint16_t stored_checksum = 0;

	CHECK(exfat_fixture_initialize(&fixture, TARGET_SECTOR_COUNT) == 0);
	root_sector = exfat_fixture_cluster_sector(&fixture.geometry, 2);
	untrusted_fat_sector =
	    fixture.geometry.fat_offset + (uint64_t)untrusted_cluster * 4 / SECTOR_SIZE;
	CHECK(exfat_fixture_read_sector(&fixture, root_sector, root, sizeof(root)) == 0);
	stream[1] = ALLOCATION_POSSIBLE;
	CHECK(exfat_resize_store_le32(stream, 32, 20, untrusted_cluster) == EXFAT_RESIZE_SUCCESS);
	CHECK(exfat_resize_load_le16(entry_set, 32, 2, &stored_checksum) == EXFAT_RESIZE_SUCCESS);
	CHECK(stored_checksum != entry_set_checksum(entry_set));
	CHECK(fixture.memory.device.write(fixture.memory.device.context, root_sector, 1, root) == 0);
	CHECK(
	    store_fat_entry(&fixture, &fixture.geometry, untrusted_cluster, UINT32_C(0xffffffff)) == 0);
	memory_block_device_clear_operations(&fixture.memory);

	error = exfat_fixture_resize(&fixture.memory.device, TARGET_SECTOR_COUNT, &options, &stage);
	CHECK(error == EXFAT_RESIZE_INVALID_FILESYSTEM);
	CHECK(stage == EXFAT_RESIZE_STAGE_PREFLIGHT);
	check_operations_are_read_only(&fixture);
	check_sector_is_not_read(&fixture, untrusted_fat_sector);
	exfat_fixture_destroy(&fixture);
}

static void test_stream_extension_structure_is_validated(void)
{
	static const struct {
		size_t secondary_index;
		unsigned char replacement_type;
	} cases[] = {
		{ 0, ENTRY_FILE_NAME },
		{ 1, ENTRY_STREAM },
	};
	struct exfat_resize_options options;
	size_t index;

	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
		struct exfat_fixture fixture;
		unsigned char root[SECTOR_SIZE];
		unsigned char *entry_set = root + 32 * 2;
		enum exfat_resize_error error;
		uint16_t checksum;
		uint64_t root_sector;

		CHECK(exfat_fixture_initialize(&fixture, TARGET_SECTOR_COUNT) == 0);
		root_sector = exfat_fixture_cluster_sector(&fixture.geometry, 2);
		CHECK(exfat_fixture_read_sector(&fixture, root_sector, root, sizeof(root)) == 0);
		entry_set[32 * (cases[index].secondary_index + 1)] = cases[index].replacement_type;
		checksum = entry_set_checksum(entry_set);
		CHECK(exfat_resize_store_le16(entry_set, 32, 2, checksum) == EXFAT_RESIZE_SUCCESS);
		CHECK(
		    fixture.memory.device.write(fixture.memory.device.context, root_sector, 1, root) == 0);
		memory_block_device_clear_operations(&fixture.memory);

		options = resize_options();
		error = exfat_fixture_resize(&fixture.memory.device, TARGET_SECTOR_COUNT, &options, NULL);
		CHECK(error == EXFAT_RESIZE_INVALID_FILESYSTEM);
		check_operations_are_read_only(&fixture);
		exfat_fixture_destroy(&fixture);
	}
}

static void test_unsupported_directory_entries(void)
{
	static const struct {
		unsigned char entry_type;
		size_t entry_index;
		int allocated;
		uint64_t target_sector_count;
		enum exfat_resize_error expected;
	} cases[] = {
		{ ENTRY_VENDOR_ALLOCATION, 4, 1, TARGET_SECTOR_COUNT,
		    EXFAT_RESIZE_UNSUPPORTED_VENDOR_ALLOCATION },
		{ 0xc2, 4, 0, TARGET_SECTOR_COUNT, EXFAT_RESIZE_UNSUPPORTED_CRITICAL_ENTRY },
		{ 0xe2, 4, 1, 12100, EXFAT_RESIZE_UNSUPPORTED_ALLOCATED_ENTRY },
		{ 0x84, 14, 0, TARGET_SECTOR_COUNT, EXFAT_RESIZE_UNSUPPORTED_CRITICAL_ENTRY },
		{ 0xa4, 14, 1, 12100, EXFAT_RESIZE_UNSUPPORTED_ALLOCATED_ENTRY },
	};
	struct exfat_resize_options options = resize_options();
	size_t index;

	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
		struct exfat_fixture fixture;
		unsigned char root[SECTOR_SIZE];
		unsigned char *entry;
		enum exfat_resize_error error;
		enum exfat_resize_stage stage = EXFAT_RESIZE_STAGE_COMPLETED;
		uint64_t root_sector;

		CHECK(exfat_fixture_initialize(&fixture, cases[index].target_sector_count) == 0);
		root_sector = exfat_fixture_cluster_sector(&fixture.geometry, 2);
		CHECK(exfat_fixture_read_sector(&fixture, root_sector, root, sizeof(root)) == 0);
		entry = root + 32 * cases[index].entry_index;
		memset(entry, 0, 32);
		entry[0] = cases[index].entry_type;
		if (cases[index].allocated) {
			entry[1] = ALLOCATION_POSSIBLE;
			CHECK(
			    exfat_resize_store_le16(entry, 32, 4, ALLOCATION_POSSIBLE) == EXFAT_RESIZE_SUCCESS);
			CHECK(exfat_resize_store_le32(entry, 32, 20, 5) == EXFAT_RESIZE_SUCCESS);
			CHECK(exfat_resize_store_le64(entry, 32, 24, SECTOR_SIZE) == EXFAT_RESIZE_SUCCESS);
		}
		if (cases[index].entry_index == 4) {
			unsigned char *entry_set = root + 32 * 2;
			uint16_t checksum = entry_set_checksum(entry_set);

			CHECK(exfat_resize_store_le16(entry_set, 32, 2, checksum) == EXFAT_RESIZE_SUCCESS);
		}
		CHECK(
		    fixture.memory.device.write(fixture.memory.device.context, root_sector, 1, root) == 0);
		memory_block_device_clear_operations(&fixture.memory);

		error = exfat_fixture_resize(
		    &fixture.memory.device, cases[index].target_sector_count, &options, &stage);
		CHECK(error == cases[index].expected);
		CHECK(stage == EXFAT_RESIZE_STAGE_PREFLIGHT);
		check_operations_are_read_only(&fixture);
		exfat_fixture_destroy(&fixture);
	}
}

static void test_insufficient_growth_is_rejected(void)
{
	const uint64_t target_sector_count = 12001;
	struct exfat_resize_options options = resize_options();
	struct exfat_fixture fixture;
	enum exfat_resize_error error;
	enum exfat_resize_stage stage = EXFAT_RESIZE_STAGE_COMPLETED;

	CHECK(exfat_fixture_initialize(&fixture, target_sector_count) == 0);
	error = exfat_fixture_resize(&fixture.memory.device, target_sector_count, &options, &stage);
	CHECK(error == EXFAT_RESIZE_INSUFFICIENT_GROWTH);
	CHECK(stage == EXFAT_RESIZE_STAGE_PREFLIGHT);
	check_operations_are_read_only(&fixture);
	exfat_fixture_destroy(&fixture);
}

static void test_bitmap_entry_rejections(void)
{
	enum bitmap_mutation { BITMAP_MISSING, BITMAP_DUPLICATE, BITMAP_FLAGS, BITMAP_SHORT };
	static const enum bitmap_mutation mutations[] = {
		BITMAP_MISSING,
		BITMAP_DUPLICATE,
		BITMAP_FLAGS,
		BITMAP_SHORT,
	};
	struct exfat_resize_options options = resize_options();
	size_t index;

	for (index = 0; index < sizeof(mutations) / sizeof(mutations[0]); ++index) {
		struct exfat_fixture fixture;
		unsigned char root[SECTOR_SIZE];
		enum exfat_resize_error error;
		enum exfat_resize_stage stage = EXFAT_RESIZE_STAGE_COMPLETED;
		uint64_t required_length;
		uint64_t root_sector;

		CHECK(exfat_fixture_initialize(&fixture, TARGET_SECTOR_COUNT) == 0);
		root_sector = exfat_fixture_cluster_sector(&fixture.geometry, 2);
		CHECK(exfat_fixture_read_sector(&fixture, root_sector, root, sizeof(root)) == 0);
		switch (mutations[index]) {
		case BITMAP_MISSING:
			root[0] = 0;
			break;
		case BITMAP_DUPLICATE:
			memcpy(root + 32 * 14, root, 32);
			break;
		case BITMAP_FLAGS:
			root[1] = 1;
			break;
		case BITMAP_SHORT:
			required_length = ((uint64_t)fixture.geometry.cluster_count + 7) / 8;
			CHECK(required_length != 0);
			CHECK(
			    exfat_resize_store_le64(root, 32, 24, required_length - 1) == EXFAT_RESIZE_SUCCESS);
			break;
		}
		CHECK(
		    fixture.memory.device.write(fixture.memory.device.context, root_sector, 1, root) == 0);
		memory_block_device_clear_operations(&fixture.memory);

		error = exfat_fixture_resize(&fixture.memory.device, TARGET_SECTOR_COUNT, &options, &stage);
		CHECK(error == EXFAT_RESIZE_INVALID_FILESYSTEM);
		CHECK(stage == EXFAT_RESIZE_STAGE_PREFLIGHT);
		check_operations_are_read_only(&fixture);
		exfat_fixture_destroy(&fixture);
	}
}

static void test_malformed_bitmap_fat_chain_is_rejected(void)
{
	enum bitmap_chain_mutation {
		BITMAP_CHAIN_ENDS_EARLY,
		BITMAP_CHAIN_LINK_OUT_OF_RANGE,
		BITMAP_CHAIN_MISSING_END_OF_CHAIN
	};
	static const enum bitmap_chain_mutation mutations[] = {
		BITMAP_CHAIN_ENDS_EARLY,
		BITMAP_CHAIN_LINK_OUT_OF_RANGE,
		BITMAP_CHAIN_MISSING_END_OF_CHAIN,
	};
	struct exfat_resize_options options = resize_options();
	size_t index;

	for (index = 0; index < sizeof(mutations) / sizeof(mutations[0]); ++index) {
		struct exfat_fixture fixture;
		enum exfat_resize_error error;
		enum exfat_resize_stage stage = EXFAT_RESIZE_STAGE_COMPLETED;
		uint32_t cluster;
		uint32_t next;

		CHECK(exfat_fixture_initialize(&fixture, TARGET_SECTOR_COUNT) == 0);
		switch (mutations[index]) {
		case BITMAP_CHAIN_ENDS_EARLY:
			cluster = fixture.bitmap_clusters[0];
			next = UINT32_C(0xffffffff);
			break;
		case BITMAP_CHAIN_LINK_OUT_OF_RANGE:
			cluster = fixture.bitmap_clusters[0];
			next = fixture.geometry.cluster_count + 2;
			break;
		case BITMAP_CHAIN_MISSING_END_OF_CHAIN:
			cluster = fixture.bitmap_clusters[fixture.bitmap_cluster_count - 1];
			next = 9;
			break;
		}
		CHECK(store_fat_entry(&fixture, &fixture.geometry, cluster, next) == 0);
		memory_block_device_clear_operations(&fixture.memory);

		error = exfat_fixture_resize(&fixture.memory.device, TARGET_SECTOR_COUNT, &options, &stage);
		CHECK(error == EXFAT_RESIZE_INVALID_FILESYSTEM);
		CHECK(stage == EXFAT_RESIZE_STAGE_PREFLIGHT);
		check_operations_are_read_only(&fixture);
		exfat_fixture_destroy(&fixture);
	}
}

static void test_unallocated_benign_entries_are_preserved(void)
{
	struct exfat_resize_options options = resize_options();
	size_t secondary_case;

	for (secondary_case = 0; secondary_case < 2; ++secondary_case) {
		struct exfat_resize_geometry target;
		struct exfat_fixture fixture;
		unsigned char expected[32] = { 0 };
		unsigned char root[SECTOR_SIZE];
		unsigned char *entry;
		enum exfat_resize_error error;
		uint16_t checksum;
		uint64_t root_sector;
		uint32_t mapped_root;

		CHECK(exfat_fixture_initialize(&fixture, TARGET_SECTOR_COUNT) == 0);
		error = plan_fixture_growth(&fixture, TARGET_SECTOR_COUNT, &target);
		CHECK(error == EXFAT_RESIZE_SUCCESS);
		root_sector = exfat_fixture_cluster_sector(&fixture.geometry, 2);
		CHECK(exfat_fixture_read_sector(&fixture, root_sector, root, sizeof(root)) == 0);
		entry = root + 32 * 14;
		entry[0] = secondary_case ? 0xe2 : 0xa4;
		memcpy(entry + 8, "benign-entry", 12);
		memcpy(expected, entry, sizeof(expected));
		if (secondary_case) {
			unsigned char *entry_set = root + 32 * 11;

			entry_set[1] = 3;
			checksum = entry_set_checksum_count(entry_set, 4);
			CHECK(exfat_resize_store_le16(entry_set, 32, 2, checksum) == EXFAT_RESIZE_SUCCESS);
		}
		CHECK(
		    fixture.memory.device.write(fixture.memory.device.context, root_sector, 1, root) == 0);
		memory_block_device_clear_operations(&fixture.memory);

		error = exfat_fixture_resize(&fixture.memory.device, TARGET_SECTOR_COUNT, &options, NULL);
		CHECK(error == EXFAT_RESIZE_SUCCESS);
		error = exfat_resize_map_growth_cluster(
		    &fixture.geometry, &target, fixture.geometry.root_directory_cluster, &mapped_root);
		CHECK(error == EXFAT_RESIZE_SUCCESS);
		CHECK(exfat_fixture_read_sector(&fixture,
		          exfat_fixture_cluster_sector(&target, mapped_root), root, sizeof(root)) == 0);
		CHECK(memcmp(root + 32 * 14, expected, sizeof(expected)) == 0);
		exfat_fixture_destroy(&fixture);
	}
}

static void test_malformed_fat_streams_are_rejected(void)
{
	static const struct {
		size_t cluster_index;
		uint32_t value;
	} cases[] = {
		{ 1, 10 },
		{ 1, UINT32_C(0xffffffff) },
		{ 2, 13 },
	};
	struct exfat_resize_options options = resize_options();
	size_t index;

	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
		struct exfat_fixture fixture;
		enum exfat_resize_error error;
		enum exfat_resize_stage stage = EXFAT_RESIZE_STAGE_COMPLETED;

		CHECK(exfat_fixture_initialize(&fixture, TARGET_SECTOR_COUNT) == 0);
		CHECK(
		    store_fat_entry(&fixture, &fixture.geometry,
		        fixture.fragmented_clusters[cases[index].cluster_index], cases[index].value) == 0);
		memory_block_device_clear_operations(&fixture.memory);

		error = exfat_fixture_resize(&fixture.memory.device, TARGET_SECTOR_COUNT, &options, &stage);
		CHECK(error == EXFAT_RESIZE_INVALID_FILESYSTEM);
		CHECK(stage == EXFAT_RESIZE_STAGE_PREFLIGHT);
		check_operations_are_read_only(&fixture);
		exfat_fixture_destroy(&fixture);
	}
}

static void test_misplaced_system_entry_is_rejected(void)
{
	struct exfat_resize_options options = resize_options();
	struct exfat_fixture fixture;
	unsigned char child[SECTOR_SIZE];
	enum exfat_resize_error error;
	enum exfat_resize_stage stage = EXFAT_RESIZE_STAGE_COMPLETED;
	uint64_t child_sector;

	CHECK(exfat_fixture_initialize(&fixture, TARGET_SECTOR_COUNT) == 0);
	child_sector = exfat_fixture_cluster_sector(&fixture.geometry, 6);
	CHECK(exfat_fixture_read_sector(&fixture, child_sector, child, sizeof(child)) == 0);
	child[32 * 3] = ENTRY_BITMAP;
	CHECK(fixture.memory.device.write(fixture.memory.device.context, child_sector, 1, child) == 0);
	memory_block_device_clear_operations(&fixture.memory);

	error = exfat_fixture_resize(&fixture.memory.device, TARGET_SECTOR_COUNT, &options, &stage);
	CHECK(error == EXFAT_RESIZE_INVALID_FILESYSTEM);
	CHECK(stage == EXFAT_RESIZE_STAGE_PREFLIGHT);
	check_operations_are_read_only(&fixture);
	exfat_fixture_destroy(&fixture);
}

static void test_truncated_entry_set_is_rejected(void)
{
	struct exfat_resize_options options = resize_options();
	struct exfat_fixture fixture;
	unsigned char root[SECTOR_SIZE];
	enum exfat_resize_error error;
	enum exfat_resize_stage stage = EXFAT_RESIZE_STAGE_COMPLETED;
	uint64_t root_sector;

	CHECK(exfat_fixture_initialize(&fixture, TARGET_SECTOR_COUNT) == 0);
	root_sector = exfat_fixture_cluster_sector(&fixture.geometry, 2);
	CHECK(exfat_fixture_read_sector(&fixture, root_sector, root, sizeof(root)) == 0);
	root[32 * 11 + 1] = 4;
	CHECK(fixture.memory.device.write(fixture.memory.device.context, root_sector, 1, root) == 0);
	memory_block_device_clear_operations(&fixture.memory);

	error = exfat_fixture_resize(&fixture.memory.device, TARGET_SECTOR_COUNT, &options, &stage);
	CHECK(error == EXFAT_RESIZE_INVALID_FILESYSTEM);
	CHECK(stage == EXFAT_RESIZE_STAGE_PREFLIGHT);
	check_operations_are_read_only(&fixture);
	exfat_fixture_destroy(&fixture);
}

static void test_secondary_entry_io_errors_are_preserved(void)
{
	const uint32_t continuation_cluster = 9;
	const uint32_t file_cluster = 13;
	struct exfat_resize_geometry target;
	struct exfat_resize_options options;
	struct exfat_fixture fixture;
	unsigned char boot_sector[SECTOR_SIZE];
	enum exfat_resize_error error;
	enum exfat_resize_stage stage;
	uint64_t source_secondary_sector;
	uint64_t target_secondary_sector;
	uint16_t volume_flags;
	uint32_t mapped_cluster;
	size_t preflight_operation = SIZE_MAX;
	size_t rewrite_operation = SIZE_MAX;
	size_t index;

	CHECK(exfat_fixture_initialize(&fixture, TARGET_SECTOR_COUNT) == 0);
	CHECK(configure_crossing_file_entry_set(&fixture, continuation_cluster, file_cluster) == 0);
	error = plan_fixture_growth(&fixture, TARGET_SECTOR_COUNT, &target);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	error = exfat_resize_map_growth_cluster(
	    &fixture.geometry, &target, continuation_cluster, &mapped_cluster);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	source_secondary_sector = exfat_fixture_cluster_sector(&fixture.geometry, continuation_cluster);
	target_secondary_sector = exfat_fixture_cluster_sector(&target, mapped_cluster);

	options = resize_options();
	error = exfat_fixture_resize(&fixture.memory.device, TARGET_SECTOR_COUNT, &options, NULL);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	for (index = 0; index < fixture.memory.operation_count; ++index) {
		const struct memory_operation *operation = &fixture.memory.operations[index];

		if (operation->kind == MEMORY_OPERATION_READ &&
		    operation_contains_sector(operation, source_secondary_sector) &&
		    preflight_operation == SIZE_MAX)
			preflight_operation = index;
		if (operation->kind == MEMORY_OPERATION_READ &&
		    operation_contains_sector(operation, target_secondary_sector))
			rewrite_operation = index;
	}
	CHECK(preflight_operation != SIZE_MAX);
	CHECK(rewrite_operation != SIZE_MAX);
	exfat_fixture_destroy(&fixture);

	if (preflight_operation != SIZE_MAX) {
		CHECK(exfat_fixture_initialize(&fixture, TARGET_SECTOR_COUNT) == 0);
		CHECK(configure_crossing_file_entry_set(&fixture, continuation_cluster, file_cluster) == 0);
		memory_block_device_fail_operation(&fixture.memory, preflight_operation, 1);
		stage = EXFAT_RESIZE_STAGE_COMPLETED;
		error = exfat_fixture_resize(&fixture.memory.device, TARGET_SECTOR_COUNT, &options, &stage);
		CHECK(error == EXFAT_RESIZE_IO_ERROR);
		CHECK(stage == EXFAT_RESIZE_STAGE_PREFLIGHT);
		check_operations_are_read_only(&fixture);
		exfat_fixture_destroy(&fixture);
	}

	if (rewrite_operation != SIZE_MAX) {
		CHECK(exfat_fixture_initialize(&fixture, TARGET_SECTOR_COUNT) == 0);
		CHECK(configure_crossing_file_entry_set(&fixture, continuation_cluster, file_cluster) == 0);
		memory_block_device_fail_operation(&fixture.memory, rewrite_operation, 1);
		stage = EXFAT_RESIZE_STAGE_COMPLETED;
		error = exfat_fixture_resize(&fixture.memory.device, TARGET_SECTOR_COUNT, &options, &stage);
		CHECK(error == EXFAT_RESIZE_IO_ERROR);
		CHECK(stage == EXFAT_RESIZE_STAGE_RESIZING);
		memory_block_device_clear_failure(&fixture.memory);
		CHECK(exfat_fixture_read_sector(&fixture, 0, boot_sector, sizeof(boot_sector)) == 0);
		CHECK(exfat_resize_load_le16(boot_sector, sizeof(boot_sector), VOLUME_FLAGS_OFFSET,
		          &volume_flags) == EXFAT_RESIZE_SUCCESS);
		CHECK((volume_flags & 0x0002) != 0);
		exfat_fixture_destroy(&fixture);
	}
}

static void test_oversized_child_directory_is_rejected(void)
{
	struct exfat_resize_options options;
	struct exfat_fixture fixture;
	unsigned char root[SECTOR_SIZE];
	unsigned char child_entry_set[32 * 3];
	enum exfat_resize_error error;
	uint64_t child_fat_sector;
	uint64_t root_sector;
	uint64_t target_sector_count;
	uint16_t checksum;

	CHECK(exfat_fixture_initialize(&fixture, TARGET_SECTOR_COUNT) == 0);
	CHECK(exfat_fixture_read_sector(&fixture, exfat_fixture_cluster_sector(&fixture.geometry, 2),
	          root, sizeof(root)) == 0);
	/*
	 * Move the fixture's child directory entry set ahead of the system
	 * entries so its size is the first metadata checked in the larger
	 * synthetic geometry.
	 */
	memcpy(child_entry_set, root + 32 * 5, sizeof(child_entry_set));
	memset(root, 0, sizeof(root));
	memcpy(root, child_entry_set, sizeof(child_entry_set));
	root[32 + 1] = 1;
	CHECK(exfat_resize_store_le64(root + 32, 32, 8, MAX_DIRECTORY_SIZE + SECTOR_SIZE) ==
	    EXFAT_RESIZE_SUCCESS);
	CHECK(exfat_resize_store_le32(root + 32, 32, 20, 200) == EXFAT_RESIZE_SUCCESS);
	CHECK(exfat_resize_store_le64(root + 32, 32, 24, MAX_DIRECTORY_SIZE + SECTOR_SIZE) ==
	    EXFAT_RESIZE_SUCCESS);
	checksum = entry_set_checksum(root);
	CHECK(exfat_resize_store_le16(root, 32, 2, checksum) == EXFAT_RESIZE_SUCCESS);

	fixture.geometry.fat_length = 5000;
	fixture.geometry.cluster_heap_offset =
	    fixture.geometry.fat_offset + fixture.geometry.fat_length;
	fixture.geometry.cluster_count = 600000;
	fixture.geometry.volume_sector_count =
	    fixture.geometry.cluster_heap_offset + fixture.geometry.cluster_count;
	target_sector_count = fixture.geometry.volume_sector_count + fixture.geometry.cluster_count;
	fixture.memory.device.sector_count = target_sector_count;
	CHECK(exfat_fixture_write_boot_regions(&fixture) == 0);
	root_sector = exfat_fixture_cluster_sector(&fixture.geometry, 2);
	CHECK(fixture.memory.device.write(fixture.memory.device.context, root_sector, 1, root) == 0);
	child_fat_sector = fixture.geometry.fat_offset + (uint64_t)200 * 4 / SECTOR_SIZE;
	memory_block_device_clear_operations(&fixture.memory);

	options = resize_options();
	error = exfat_fixture_resize(&fixture.memory.device, target_sector_count, &options, NULL);
	CHECK(error == EXFAT_RESIZE_INVALID_FILESYSTEM);
	check_operations_are_read_only(&fixture);
	check_sector_is_not_read(&fixture, child_fat_sector);
	exfat_fixture_destroy(&fixture);
}

static void test_oversized_root_directory_is_rejected(void)
{
	struct exfat_resize_options options;
	struct exfat_fixture fixture;
	enum exfat_resize_error error;
	uint64_t root_sector;
	uint64_t target_sector_count;
	uint32_t cluster;

	/* At the maximum 32 MiB cluster size, a directory may use eight clusters. */
	CHECK(exfat_fixture_initialize(&fixture, TARGET_SECTOR_COUNT) == 0);
	fixture.geometry.sectors_per_cluster = 65536;
	fixture.geometry.cluster_count = 1000;
	fixture.geometry.volume_sector_count = fixture.geometry.cluster_heap_offset +
	    (uint64_t)fixture.geometry.cluster_count * fixture.geometry.sectors_per_cluster;
	target_sector_count =
	    fixture.geometry.volume_sector_count + fixture.geometry.sectors_per_cluster;
	fixture.memory.device.sector_count = target_sector_count;
	CHECK(exfat_fixture_write_boot_regions(&fixture) == 0);
	for (cluster = 2; cluster < 10; ++cluster)
		CHECK(store_fat_entry(&fixture, &fixture.geometry, cluster, cluster + 1) == 0);
	CHECK(store_fat_entry(&fixture, &fixture.geometry, 10, UINT32_C(0xffffffff)) == 0);
	root_sector = exfat_fixture_cluster_sector(&fixture.geometry, 2);
	memory_block_device_clear_operations(&fixture.memory);

	options = resize_options();
	error = exfat_fixture_resize(&fixture.memory.device, target_sector_count, &options, NULL);
	CHECK(error == EXFAT_RESIZE_INVALID_FILESYSTEM);
	check_operations_are_read_only(&fixture);
	check_sector_is_not_read(&fixture, root_sector);
	exfat_fixture_destroy(&fixture);
}

static void test_reserved_fat_entries(void)
{
	struct exfat_resize_geometry target;
	struct exfat_resize_options options;
	struct exfat_fixture fixture;
	enum exfat_resize_error error;

	CHECK(exfat_fixture_initialize(&fixture, TARGET_SECTOR_COUNT) == 0);
	error = plan_fixture_growth(&fixture, TARGET_SECTOR_COUNT, &target);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	CHECK(store_fat_entry(&fixture, &fixture.geometry, 0, UINT32_C(0xfffffff0)) == 0);
	memory_block_device_clear_operations(&fixture.memory);

	options = resize_options();
	error = exfat_fixture_resize(&fixture.memory.device, TARGET_SECTOR_COUNT, &options, NULL);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	if (error == EXFAT_RESIZE_SUCCESS)
		CHECK(load_fat_entry(&fixture, &target, 0) == UINT32_C(0xfffffff0));
	exfat_fixture_destroy(&fixture);

	CHECK(exfat_fixture_initialize(&fixture, TARGET_SECTOR_COUNT) == 0);
	CHECK(store_fat_entry(&fixture, &fixture.geometry, 0, UINT32_C(0xfffefff8)) == 0);
	memory_block_device_clear_operations(&fixture.memory);

	options = resize_options();
	error = exfat_fixture_resize(&fixture.memory.device, TARGET_SECTOR_COUNT, &options, NULL);
	CHECK(error == EXFAT_RESIZE_INVALID_FILESYSTEM);
	check_operations_are_read_only(&fixture);
	exfat_fixture_destroy(&fixture);

	CHECK(exfat_fixture_initialize(&fixture, TARGET_SECTOR_COUNT) == 0);
	CHECK(store_fat_entry(&fixture, &fixture.geometry, 1, UINT32_C(0xfffffffe)) == 0);
	memory_block_device_clear_operations(&fixture.memory);

	options = resize_options();
	error = exfat_fixture_resize(&fixture.memory.device, TARGET_SECTOR_COUNT, &options, NULL);
	CHECK(error == EXFAT_RESIZE_INVALID_FILESYSTEM);
	check_operations_are_read_only(&fixture);
	exfat_fixture_destroy(&fixture);
}

static void test_allocation_model_validates_bitmap(void)
{
	struct allocator_state allocator = { 0 };
	struct exfat_resize_options options;
	struct exfat_fixture fixture;
	enum exfat_resize_error error;

	CHECK(exfat_fixture_initialize(&fixture, TARGET_SECTOR_COUNT) == 0);
	CHECK(set_bitmap_cluster(&fixture, 5, 0) == 0);
	memory_block_device_clear_operations(&fixture.memory);
	options = resize_options();
	options.allocator.context = &allocator;
	options.allocator.allocate = tracked_allocate;
	options.allocator.deallocate = tracked_deallocate;
	error = exfat_fixture_resize(&fixture.memory.device, TARGET_SECTOR_COUNT, &options, NULL);
	CHECK(error == EXFAT_RESIZE_INVALID_FILESYSTEM);
	CHECK(allocator.allocation_count == 3);
	CHECK(allocator.deallocation_count == 3);
	CHECK(allocator.largest_allocation_size == WORK_BUFFER_SIZE);
	CHECK(allocator.live_size == 0);
	check_operations_are_read_only(&fixture);
	exfat_fixture_destroy(&fixture);

	CHECK(exfat_fixture_initialize(&fixture, TARGET_SECTOR_COUNT) == 0);
	CHECK(set_bitmap_cluster(&fixture, 9, 1) == 0);
	memory_block_device_clear_operations(&fixture.memory);
	options = resize_options();
	error = exfat_fixture_resize(&fixture.memory.device, TARGET_SECTOR_COUNT, &options, NULL);
	CHECK(error == EXFAT_RESIZE_INVALID_FILESYSTEM);
	check_operations_are_read_only(&fixture);
	exfat_fixture_destroy(&fixture);
}

static void test_allocation_model_rejects_shared_directory(void)
{
	struct exfat_resize_options options;
	struct exfat_fixture fixture;
	unsigned char root[SECTOR_SIZE];
	enum exfat_resize_error error;

	CHECK(exfat_fixture_initialize(&fixture, TARGET_SECTOR_COUNT) == 0);
	CHECK(exfat_fixture_read_sector(&fixture, exfat_fixture_cluster_sector(&fixture.geometry, 2),
	          root, sizeof(root)) == 0);
	memcpy(root + 32 * 11, root + 32 * 5, 32 * 3);
	CHECK(fixture.memory.device.write(fixture.memory.device.context,
	          exfat_fixture_cluster_sector(&fixture.geometry, 2), 1, root) == 0);
	memory_block_device_clear_operations(&fixture.memory);

	options = resize_options();
	error = exfat_fixture_resize(&fixture.memory.device, TARGET_SECTOR_COUNT, &options, NULL);
	CHECK(error == EXFAT_RESIZE_INVALID_FILESYSTEM);
	check_operations_are_read_only(&fixture);
	exfat_fixture_destroy(&fixture);
}

static void test_displaced_bad_cluster_is_rejected(void)
{
	static const struct {
		uint64_t target_sector_count;
		uint32_t bad_cluster;
	} cases[] = {
		{ TARGET_SECTOR_COUNT, 50 },
		{ UINT64_C(1544926), 500 },
		{ UINT64_C(1600000), 500 },
	};
	struct allocator_state allocator;
	struct exfat_resize_geometry target;
	struct exfat_resize_options options;
	struct exfat_fixture fixture;
	unsigned char boot_sector[SECTOR_SIZE];
	enum exfat_resize_error error;
	uint64_t bad_sector;
	uint64_t fat_end;
	uint64_t heap_shift;
	uint16_t volume_flags;
	size_t index;

	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
		memset(&allocator, 0, sizeof(allocator));
		CHECK(exfat_fixture_initialize(&fixture, cases[index].target_sector_count) == 0);
		error = plan_fixture_growth(&fixture, cases[index].target_sector_count, &target);
		CHECK(error == EXFAT_RESIZE_SUCCESS);
		heap_shift = (target.cluster_heap_offset - fixture.geometry.cluster_heap_offset) /
		    fixture.geometry.sectors_per_cluster;
		if (index == 0) {
			CHECK(heap_shift > 0);
			CHECK(heap_shift < fixture.geometry.cluster_count);
		} else if (index == 1) {
			CHECK(heap_shift == fixture.geometry.cluster_count);
		} else {
			CHECK(heap_shift > fixture.geometry.cluster_count);
		}
		bad_sector = exfat_fixture_cluster_sector(&fixture.geometry, cases[index].bad_cluster);
		fat_end = (uint64_t)target.fat_offset + target.fat_length;
		CHECK(bad_sector >= target.fat_offset);
		CHECK(bad_sector < fat_end);
		CHECK(mark_bad_cluster(&fixture, cases[index].bad_cluster) == 0);
		memory_block_device_clear_operations(&fixture.memory);

		options = resize_options();
		options.allocator.context = &allocator;
		options.allocator.allocate = tracked_allocate;
		options.allocator.deallocate = tracked_deallocate;
		error = exfat_fixture_resize(
		    &fixture.memory.device, cases[index].target_sector_count, &options, NULL);
		CHECK(error == EXFAT_RESIZE_BAD_CLUSTER_CONFLICT);
		CHECK(allocator.allocation_count == 3);
		CHECK(allocator.deallocation_count == 3);
		CHECK(allocator.live_size == 0);
		CHECK(exfat_fixture_read_sector(&fixture, 0, boot_sector, sizeof(boot_sector)) == 0);
		CHECK(exfat_resize_load_le16(boot_sector, sizeof(boot_sector), VOLUME_FLAGS_OFFSET,
		          &volume_flags) == EXFAT_RESIZE_SUCCESS);
		CHECK((volume_flags & UINT16_C(0x0002)) == 0);
		check_operations_are_read_only(&fixture);
		check_sector_is_not_written(&fixture, bad_sector);
		exfat_fixture_destroy(&fixture);
	}
}

static void test_non_displaced_bad_cluster_is_preserved(void)
{
	struct exfat_resize_geometry target;
	struct exfat_resize_options options;
	struct exfat_fixture fixture;
	unsigned char root[SECTOR_SIZE];
	enum exfat_resize_error error;
	uint64_t source_sector;
	uint64_t target_sector;
	uint32_t bad_cluster = 500;
	uint32_t bitmap_cluster;
	uint32_t mapped_cluster;

	CHECK(exfat_fixture_initialize(&fixture, TARGET_SECTOR_COUNT) == 0);
	error = plan_fixture_growth(&fixture, TARGET_SECTOR_COUNT, &target);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	source_sector = exfat_fixture_cluster_sector(&fixture.geometry, bad_cluster);
	CHECK(source_sector >= (uint64_t)target.fat_offset + target.fat_length);
	CHECK(mark_bad_cluster(&fixture, bad_cluster) == 0);
	memory_block_device_clear_operations(&fixture.memory);

	options = resize_options();
	error = exfat_fixture_resize(&fixture.memory.device, TARGET_SECTOR_COUNT, &options, NULL);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	if (error != EXFAT_RESIZE_SUCCESS) {
		exfat_fixture_destroy(&fixture);
		return;
	}

	error =
	    exfat_resize_map_growth_cluster(&fixture.geometry, &target, bad_cluster, &mapped_cluster);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	target_sector = exfat_fixture_cluster_sector(&target, mapped_cluster);
	CHECK(target_sector == source_sector);
	CHECK(load_fat_entry(&fixture, &target, mapped_cluster) == FAT_BAD_CLUSTER);
	CHECK(exfat_fixture_read_sector(&fixture,
	          exfat_fixture_cluster_sector(&target, target.root_directory_cluster), root,
	          sizeof(root)) == 0);
	CHECK(exfat_resize_load_le32(root, sizeof(root), 20, &bitmap_cluster) == EXFAT_RESIZE_SUCCESS);
	CHECK(bitmap_cluster_is_set(&fixture, &target, bitmap_cluster, mapped_cluster));
	check_sector_is_not_written(&fixture, source_sector);
	exfat_fixture_destroy(&fixture);
}

static void test_allocator_failure_is_read_only(void)
{
	size_t failing_allocation;

	for (failing_allocation = 1; failing_allocation <= 2; ++failing_allocation) {
		struct allocator_state allocator = { .fail_on_allocation = failing_allocation };
		struct exfat_resize_options options;
		struct exfat_fixture fixture;
		enum exfat_resize_error error;

		CHECK(exfat_fixture_initialize(&fixture, TARGET_SECTOR_COUNT) == 0);
		options = resize_options();
		options.allocator.context = &allocator;
		options.allocator.allocate = tracked_allocate;
		options.allocator.deallocate = tracked_deallocate;
		error = exfat_fixture_resize(&fixture.memory.device, TARGET_SECTOR_COUNT, &options, NULL);
		CHECK(error == EXFAT_RESIZE_OUT_OF_MEMORY);
		CHECK(allocator.allocation_count == failing_allocation);
		CHECK(allocator.deallocation_count + 1 == failing_allocation);
		CHECK(allocator.largest_allocation_size == WORK_BUFFER_SIZE);
		CHECK(allocator.live_size == 0);
		check_operations_are_read_only(&fixture);
		exfat_fixture_destroy(&fixture);
	}
}

static void test_directory_worklist_growth(void)
{
	struct allocator_state allocator = { 0 };
	struct exfat_resize_options options;
	struct exfat_fixture fixture;
	enum exfat_resize_error error;

	CHECK(exfat_fixture_initialize(&fixture, TARGET_SECTOR_COUNT) == 0);
	CHECK(exfat_fixture_add_child_directories(&fixture, 500, 4) == 0);
	memory_block_device_clear_operations(&fixture.memory);
	allocator.device = &fixture.memory;
	options = resize_options();
	options.allocator.context = &allocator;
	options.allocator.allocate = tracked_allocate;
	options.allocator.deallocate = tracked_deallocate;
	error = exfat_fixture_resize(&fixture.memory.device, TARGET_SECTOR_COUNT, &options, NULL);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	CHECK(allocator.allocation_count > 3);
	CHECK(allocator.deallocation_count == allocator.allocation_count);
	CHECK(allocator.live_size == 0);
	CHECK(!allocator.allocation_after_write);
	exfat_fixture_destroy(&fixture);
}

static void test_directory_worklist_allocation_failure(void)
{
	size_t failing_allocation;

	for (failing_allocation = 3; failing_allocation <= 5; ++failing_allocation) {
		struct allocator_state allocator = { .fail_on_allocation = failing_allocation };
		struct exfat_resize_options options;
		struct exfat_fixture fixture;
		enum exfat_resize_error error;

		CHECK(exfat_fixture_initialize(&fixture, TARGET_SECTOR_COUNT) == 0);
		CHECK(exfat_fixture_add_child_directories(&fixture, 500, 4) == 0);
		memory_block_device_clear_operations(&fixture.memory);
		allocator.device = &fixture.memory;
		options = resize_options();
		options.allocator.context = &allocator;
		options.allocator.allocate = tracked_allocate;
		options.allocator.deallocate = tracked_deallocate;
		error = exfat_fixture_resize(&fixture.memory.device, TARGET_SECTOR_COUNT, &options, NULL);
		CHECK(error == EXFAT_RESIZE_OUT_OF_MEMORY);
		CHECK(allocator.allocation_count == failing_allocation);
		CHECK(allocator.deallocation_count + 1 == failing_allocation);
		CHECK(allocator.live_size == 0);
		CHECK(!allocator.allocation_after_write);
		check_operations_are_read_only(&fixture);
		exfat_fixture_destroy(&fixture);
	}
}

static void test_deep_directory_tree(void)
{
	struct exfat_resize_options options;
	struct exfat_fixture fixture;
	enum exfat_resize_error error;

	CHECK(exfat_fixture_initialize(&fixture, TARGET_SECTOR_COUNT) == 0);
	CHECK(exfat_fixture_add_directory_chain(&fixture, 500, 2048) == 0);
	memory_block_device_clear_operations(&fixture.memory);
	options = resize_options();
	error = exfat_fixture_resize(&fixture.memory.device, TARGET_SECTOR_COUNT, &options, NULL);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	exfat_fixture_destroy(&fixture);
}

static void test_identity_mapping_rewrites_only_bitmap(void)
{
	static const struct {
		uint64_t target_sector_count;
		size_t expected_directory_reads;
		size_t expected_directory_writes;
	} cases[] = {
		{ 12003, 1, 0 },
		{ 1544926, 2, 1 },
	};
	struct exfat_resize_geometry target;
	struct exfat_resize_options options = resize_options();
	struct exfat_fixture fixture;
	enum exfat_resize_error error;
	uint64_t heap_shift;
	uint64_t source_sector;
	uint64_t target_sector;
	uint32_t cluster;
	size_t index;

	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
		CHECK(exfat_fixture_initialize(&fixture, cases[index].target_sector_count) == 0);
		CHECK(exfat_fixture_add_directory_chain(&fixture, 500, 64) == 0);
		error = plan_fixture_growth(&fixture, cases[index].target_sector_count, &target);
		CHECK(error == EXFAT_RESIZE_SUCCESS);
		heap_shift = (target.cluster_heap_offset - fixture.geometry.cluster_heap_offset) /
		    fixture.geometry.sectors_per_cluster;
		CHECK(heap_shift == (index == 0 ? 0 : fixture.geometry.cluster_count));
		memory_block_device_clear_operations(&fixture.memory);

		error = exfat_fixture_resize(
		    &fixture.memory.device, cases[index].target_sector_count, &options, NULL);
		CHECK(error == EXFAT_RESIZE_SUCCESS);
		for (cluster = 500; cluster < 564; ++cluster) {
			source_sector = exfat_fixture_cluster_sector(&fixture.geometry, cluster);
			target_sector = exfat_fixture_cluster_sector(&target, cluster);
			CHECK(sector_operation_count(&fixture, MEMORY_OPERATION_READ, source_sector) ==
			    cases[index].expected_directory_reads);
			if (target_sector != source_sector)
				CHECK(sector_operation_count(&fixture, MEMORY_OPERATION_READ, target_sector) == 0);
			CHECK(sector_operation_count(&fixture, MEMORY_OPERATION_WRITE, target_sector) ==
			    cases[index].expected_directory_writes);
		}
		exfat_fixture_destroy(&fixture);
	}
}

static void test_no_fat_chain_ignores_stale_fat(void)
{
	struct exfat_resize_geometry target;
	struct exfat_resize_options options;
	struct exfat_fixture fixture;
	unsigned char directory[SECTOR_SIZE];
	unsigned char root[SECTOR_SIZE];
	enum exfat_resize_error error;
	uint32_t mapped_cluster;
	uint32_t mapped_root;

	CHECK(exfat_fixture_initialize(&fixture, TARGET_SECTOR_COUNT) == 0);
	CHECK(store_fat_entry(&fixture, &fixture.geometry, 5, FAT_BAD_CLUSTER) == 0);
	CHECK(store_fat_entry(&fixture, &fixture.geometry, 6, FAT_BAD_CLUSTER) == 0);
	CHECK(store_fat_entry(&fixture, &fixture.geometry, 9, UINT32_C(0x12345678)) == 0);
	CHECK(store_fat_entry(
	          &fixture, &fixture.geometry, fixture.crossing_first_cluster, FAT_BAD_CLUSTER) == 0);
	memory_block_device_clear_operations(&fixture.memory);

	error = plan_fixture_growth(&fixture, TARGET_SECTOR_COUNT, &target);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	options = resize_options();
	error = exfat_fixture_resize(&fixture.memory.device, TARGET_SECTOR_COUNT, &options, NULL);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	if (error != EXFAT_RESIZE_SUCCESS) {
		exfat_fixture_destroy(&fixture);
		return;
	}

	error = exfat_resize_map_growth_cluster(&fixture.geometry, &target, 5, &mapped_cluster);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	check_cluster_marker(&fixture, &target, mapped_cluster, 0x55);

	error = exfat_resize_map_growth_cluster(&fixture.geometry, &target, 6, &mapped_cluster);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	CHECK(exfat_fixture_read_sector(&fixture, exfat_fixture_cluster_sector(&target, mapped_cluster),
	          directory, sizeof(directory)) == 0);
	CHECK(directory[0] == 0x85);

	error = exfat_resize_map_growth_cluster(
	    &fixture.geometry, &target, fixture.crossing_first_cluster, &mapped_cluster);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	check_cluster_pattern(&fixture, &target, fixture.crossing_first_cluster, mapped_cluster);

	error = exfat_resize_map_growth_cluster(
	    &fixture.geometry, &target, fixture.geometry.root_directory_cluster, &mapped_root);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	CHECK(exfat_fixture_read_sector(&fixture, exfat_fixture_cluster_sector(&target, mapped_root),
	          root, sizeof(root)) == 0);
	CHECK((root[32 * 3 + 1] & 0x02) != 0);

	error = exfat_resize_map_growth_cluster(&fixture.geometry, &target, 9, &mapped_cluster);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	CHECK(load_fat_entry(&fixture, &target, mapped_cluster) == 0);

	exfat_fixture_destroy(&fixture);
}

static void test_failure_leaves_volume_dirty(void)
{
	struct exfat_resize_options options;
	struct exfat_fixture fixture;
	unsigned char boot_sector[SECTOR_SIZE];
	enum exfat_resize_error error;
	enum exfat_resize_stage stage;
	uint16_t volume_flags;
	size_t failing_operation = SIZE_MAX;
	size_t index;
	unsigned int write_count = 0;

	CHECK(exfat_fixture_initialize(&fixture, TARGET_SECTOR_COUNT) == 0);
	options = resize_options();
	error = exfat_fixture_resize(&fixture.memory.device, TARGET_SECTOR_COUNT, &options, NULL);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	for (index = 0; index < fixture.memory.operation_count; ++index) {
		if (fixture.memory.operations[index].kind == MEMORY_OPERATION_WRITE && ++write_count == 2) {
			failing_operation = index;
			break;
		}
	}
	CHECK(failing_operation != SIZE_MAX);
	exfat_fixture_destroy(&fixture);

	CHECK(exfat_fixture_initialize(&fixture, TARGET_SECTOR_COUNT) == 0);
	memory_block_device_fail_operation(&fixture.memory, failing_operation, 1);
	error = exfat_fixture_resize(&fixture.memory.device, TARGET_SECTOR_COUNT, &options, &stage);
	CHECK(error == EXFAT_RESIZE_IO_ERROR);
	CHECK(stage == EXFAT_RESIZE_STAGE_PREPARING);
	memory_block_device_clear_failure(&fixture.memory);
	CHECK(exfat_fixture_read_sector(&fixture, 0, boot_sector, sizeof(boot_sector)) == 0);
	CHECK(exfat_resize_load_le16(boot_sector, sizeof(boot_sector), VOLUME_FLAGS_OFFSET,
	          &volume_flags) == EXFAT_RESIZE_SUCCESS);
	CHECK((volume_flags & 0x0002) != 0);
	exfat_fixture_destroy(&fixture);
}

static void test_resize_stages(void)
{
	struct exfat_resize_options options;
	struct exfat_resize_stage_case {
		size_t operation;
		enum exfat_resize_stage expected_stage;
		int expected_dirty;
	} cases[4];
	struct exfat_fixture fixture;
	unsigned char boot_sector[SECTOR_SIZE];
	enum exfat_resize_error error;
	enum exfat_resize_stage stage;
	uint16_t volume_flags;
	size_t case_count = 0;
	size_t final_write_operation = SIZE_MAX;
	size_t index;

	CHECK(exfat_fixture_initialize(&fixture, TARGET_SECTOR_COUNT) == 0);
	options = resize_options();
	stage = EXFAT_RESIZE_STAGE_COMPLETED;
	error = exfat_fixture_resize(&fixture.memory.device, TARGET_SECTOR_COUNT, &options, &stage);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	CHECK(stage == EXFAT_RESIZE_STAGE_COMPLETED);

	for (index = 0; index < fixture.memory.operation_count; ++index) {
		const struct memory_operation *operation = &fixture.memory.operations[index];

		if (operation->kind == MEMORY_OPERATION_WRITE && case_count == 0) {
			cases[case_count].operation = index;
			cases[case_count].expected_stage = EXFAT_RESIZE_STAGE_PREPARING;
			cases[case_count++].expected_dirty = -1;
		}
		if (operation->kind == MEMORY_OPERATION_WRITE &&
		    operation->first_sector == fixture.geometry.fat_offset && case_count == 1) {
			cases[case_count].operation = index;
			cases[case_count].expected_stage = EXFAT_RESIZE_STAGE_RESIZING;
			cases[case_count++].expected_dirty = -1;
		}
		if (operation->kind == MEMORY_OPERATION_WRITE && operation->first_sector == 0 &&
		    operation->sector_count == 1)
			final_write_operation = index;
	}
	CHECK(case_count == 2);
	CHECK(final_write_operation != SIZE_MAX);
	cases[case_count].operation = final_write_operation;
	cases[case_count].expected_stage = EXFAT_RESIZE_STAGE_FINALIZING;
	cases[case_count++].expected_dirty = 1;
	CHECK(fixture.memory.operation_count != 0);
	CHECK(fixture.memory.operations[fixture.memory.operation_count - 1].kind ==
	    MEMORY_OPERATION_SYNC);
	cases[case_count].operation = fixture.memory.operation_count - 1;
	cases[case_count].expected_stage = EXFAT_RESIZE_STAGE_FINALIZING;
	cases[case_count++].expected_dirty = 0;
	exfat_fixture_destroy(&fixture);

	for (index = 0; index < case_count; ++index) {
		CHECK(exfat_fixture_initialize(&fixture, TARGET_SECTOR_COUNT) == 0);
		memory_block_device_fail_operation(&fixture.memory, cases[index].operation, 1);
		stage = EXFAT_RESIZE_STAGE_COMPLETED;
		error = exfat_fixture_resize(&fixture.memory.device, TARGET_SECTOR_COUNT, &options, &stage);
		CHECK(error == EXFAT_RESIZE_IO_ERROR);
		CHECK(stage == cases[index].expected_stage);
		if (cases[index].expected_dirty >= 0) {
			CHECK(exfat_fixture_read_sector(&fixture, 0, boot_sector, sizeof(boot_sector)) == 0);
			CHECK(exfat_resize_load_le16(boot_sector, sizeof(boot_sector), VOLUME_FLAGS_OFFSET,
			          &volume_flags) == EXFAT_RESIZE_SUCCESS);
			CHECK(((volume_flags & 0x0002) != 0) == cases[index].expected_dirty);
		}
		exfat_fixture_destroy(&fixture);
	}
}

static void test_options_validation(void)
{
	struct exfat_resize_options options;
	struct exfat_fixture fixture;
	enum exfat_resize_error error;

	CHECK(exfat_fixture_initialize(&fixture, TARGET_SECTOR_COUNT) == 0);
	error = exfat_fixture_resize(&fixture.memory.device, TARGET_SECTOR_COUNT, NULL, NULL);
	CHECK(error == EXFAT_RESIZE_INVALID_ARGUMENT);
	CHECK(fixture.memory.operation_count == 0);

	options = resize_options();
	options.allocator.allocate = NULL;
	error = exfat_fixture_resize(&fixture.memory.device, TARGET_SECTOR_COUNT, &options, NULL);
	CHECK(error == EXFAT_RESIZE_INVALID_ARGUMENT);
	CHECK(fixture.memory.operation_count == 0);

	options = resize_options();
	options.allocator.deallocate = NULL;
	error = exfat_fixture_resize(&fixture.memory.device, TARGET_SECTOR_COUNT, &options, NULL);
	CHECK(error == EXFAT_RESIZE_INVALID_ARGUMENT);
	CHECK(fixture.memory.operation_count == 0);
	exfat_fixture_destroy(&fixture);
}

static void test_mapping_extremes(void)
{
	/* The last three cases shift by one less than, exactly, and one more than the old heap. */
	static const struct {
		uint64_t target_sector_count;
		uint32_t expected_heap_shift;
	} cases[] = {
		{ 12003, 0 },
		{ 1544669, 11743 },
		{ 1544926, 11744 },
		{ 1544927, 11745 },
	};
	struct exfat_resize_geometry target;
	struct exfat_resize_geometry read_back;
	struct exfat_resize_options options;
	struct exfat_fixture fixture;
	unsigned char workspace[SECTOR_SIZE * 2];
	unsigned char root[SECTOR_SIZE];
	enum exfat_resize_error error;
	uint64_t heap_shift;
	uint32_t source_cluster;
	uint32_t mapped;
	uint32_t pattern_index;
	size_t index;

	options = resize_options();
	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
		CHECK(exfat_fixture_initialize(&fixture, cases[index].target_sector_count) == 0);
		error = plan_fixture_growth(&fixture, cases[index].target_sector_count, &target);
		CHECK(error == EXFAT_RESIZE_SUCCESS);
		heap_shift = (target.cluster_heap_offset - fixture.geometry.cluster_heap_offset) /
		    fixture.geometry.sectors_per_cluster;
		CHECK(heap_shift == cases[index].expected_heap_shift);
		if (index == 0) {
			uint64_t bitmap_length = ((uint64_t)fixture.geometry.cluster_count + 7) / 8 + 16;

			CHECK(exfat_fixture_read_sector(&fixture,
			          exfat_fixture_cluster_sector(&fixture.geometry, 2), root, sizeof(root)) == 0);
			CHECK(exfat_resize_store_le64(root, sizeof(root), 24, bitmap_length) ==
			    EXFAT_RESIZE_SUCCESS);
			CHECK(fixture.memory.device.write(fixture.memory.device.context,
			          exfat_fixture_cluster_sector(&fixture.geometry, 2), 1, root) == 0);
			memory_block_device_clear_operations(&fixture.memory);
		}

		error = exfat_fixture_resize(
		    &fixture.memory.device, cases[index].target_sector_count, &options, NULL);
		CHECK(error == EXFAT_RESIZE_SUCCESS);
		if (error == EXFAT_RESIZE_SUCCESS) {
			error = exfat_resize_read_boot_regions(
			    &fixture.memory.device, workspace, sizeof(workspace), &read_back);
			CHECK(error == EXFAT_RESIZE_SUCCESS);
			CHECK(memcmp(&read_back, &target, sizeof(target)) == 0);
			error = exfat_resize_map_growth_cluster(&fixture.geometry, &target, 5, &mapped);
			CHECK(error == EXFAT_RESIZE_SUCCESS);
			check_cluster_marker(&fixture, &target, mapped, 0x55);
			for (pattern_index = 0; pattern_index < fixture.crossing_cluster_count;
			    ++pattern_index) {
				source_cluster = fixture.crossing_first_cluster + pattern_index;
				error = exfat_resize_map_growth_cluster(
				    &fixture.geometry, &target, source_cluster, &mapped);
				CHECK(error == EXFAT_RESIZE_SUCCESS);
				check_cluster_pattern(&fixture, &target, source_cluster, mapped);
			}
			if (index >= 2) {
				error = exfat_resize_map_growth_cluster(&fixture.geometry, &target, 5, &mapped);
				CHECK(error == EXFAT_RESIZE_SUCCESS);
				CHECK(mapped == 5);
				CHECK(exfat_fixture_read_sector(&fixture,
				          exfat_fixture_cluster_sector(&target, target.root_directory_cluster),
				          root, sizeof(root)) == 0);
				CHECK((root[32 * 9 + 1] & 0x02) != 0);
			}
		}
		exfat_fixture_destroy(&fixture);
	}
}

static void test_contiguous_relocation_is_batched(void)
{
	static const uint64_t target_sector_counts[] = {
		TARGET_SECTOR_COUNT,
		UINT64_C(1544926),
		UINT64_C(1600000),
	};
	struct exfat_resize_geometry target;
	struct exfat_resize_options options = resize_options();
	struct exfat_fixture fixture;
	enum exfat_resize_error error;
	uint64_t source_sector;
	uint64_t target_sector;
	uint64_t heap_shift;
	uint32_t displaced_cluster_count;
	uint32_t expected_cluster_count;
	uint32_t mapped_cluster;
	size_t case_index;
	size_t operation_index;
	int saw_batched_read;
	int saw_batched_write;

	for (case_index = 0;
	    case_index < sizeof(target_sector_counts) / sizeof(target_sector_counts[0]); ++case_index) {
		CHECK(exfat_fixture_initialize(&fixture, target_sector_counts[case_index]) == 0);
		error = plan_fixture_growth(&fixture, target_sector_counts[case_index], &target);
		CHECK(error == EXFAT_RESIZE_SUCCESS);
		heap_shift = (target.cluster_heap_offset - fixture.geometry.cluster_heap_offset) /
		    fixture.geometry.sectors_per_cluster;
		displaced_cluster_count = heap_shift > fixture.geometry.cluster_count
		    ? fixture.geometry.cluster_count
		    : (uint32_t)heap_shift;
		expected_cluster_count = displaced_cluster_count + 2 - fixture.crossing_first_cluster;
		if (expected_cluster_count > fixture.crossing_cluster_count)
			expected_cluster_count = fixture.crossing_cluster_count;
		CHECK(expected_cluster_count > 1);
		error = exfat_resize_map_growth_cluster(
		    &fixture.geometry, &target, fixture.crossing_first_cluster, &mapped_cluster);
		CHECK(error == EXFAT_RESIZE_SUCCESS);
		source_sector =
		    exfat_fixture_cluster_sector(&fixture.geometry, fixture.crossing_first_cluster);
		target_sector = exfat_fixture_cluster_sector(&target, mapped_cluster);
		memory_block_device_clear_operations(&fixture.memory);

		error = exfat_fixture_resize(
		    &fixture.memory.device, target_sector_counts[case_index], &options, NULL);
		CHECK(error == EXFAT_RESIZE_SUCCESS);
		saw_batched_read = 0;
		saw_batched_write = 0;
		for (operation_index = 0; operation_index < fixture.memory.operation_count;
		    ++operation_index) {
			const struct memory_operation *operation = &fixture.memory.operations[operation_index];

			if (operation->kind == MEMORY_OPERATION_READ &&
			    operation->first_sector == source_sector &&
			    operation->sector_count == expected_cluster_count)
				saw_batched_read = 1;
			if (operation->kind == MEMORY_OPERATION_WRITE &&
			    operation->first_sector == target_sector &&
			    operation->sector_count == expected_cluster_count)
				saw_batched_write = 1;
		}
		CHECK(saw_batched_read);
		CHECK(saw_batched_write);
		exfat_fixture_destroy(&fixture);
	}
}

static void test_multi_sector_cluster_copy(void)
{
	const uint64_t target_sector_count = 262144;
	struct allocator_state allocator = { 0 };
	struct exfat_resize_geometry target;
	struct exfat_resize_options options;
	struct exfat_fixture fixture;
	unsigned char sector[SECTOR_SIZE];
	enum exfat_resize_error error;
	uint64_t source_root_sector;
	uint64_t target_root_sector;
	uint32_t mapped_data;
	uint32_t mapped_root;
	uint32_t sector_index;
	uint32_t source_data;
	size_t byte_index;
	size_t operation_index;
	int saw_multi_sector_read = 0;
	int saw_multi_sector_write = 0;

	CHECK(exfat_fixture_initialize_with_sectors_per_cluster(&fixture, target_sector_count, 8) == 0);
	error = plan_fixture_growth(&fixture, target_sector_count, &target);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	CHECK((target.cluster_heap_offset - fixture.geometry.cluster_heap_offset) /
	        fixture.geometry.sectors_per_cluster >
	    0);
	CHECK((target.cluster_heap_offset - fixture.geometry.cluster_heap_offset) /
	        fixture.geometry.sectors_per_cluster <
	    fixture.geometry.cluster_count);

	source_root_sector =
	    exfat_fixture_cluster_sector(&fixture.geometry, fixture.geometry.root_directory_cluster);
	for (sector_index = 1; sector_index < fixture.geometry.sectors_per_cluster; ++sector_index) {
		for (byte_index = 0; byte_index < sizeof(sector); ++byte_index) {
			uint64_t cluster_byte_offset = (uint64_t)sector_index * SECTOR_SIZE + byte_index;

			sector[byte_index] =
			    expected_cluster_byte(fixture.geometry.root_directory_cluster, cluster_byte_offset);
		}
		CHECK(fixture.memory.device.write(fixture.memory.device.context,
		          source_root_sector + sector_index, 1, sector) == 0);
	}
	memory_block_device_clear_operations(&fixture.memory);

	allocator.device = &fixture.memory;
	options = resize_options();
	options.allocator.context = &allocator;
	options.allocator.allocate = tracked_allocate;
	options.allocator.deallocate = tracked_deallocate;
	error = exfat_fixture_resize(&fixture.memory.device, target_sector_count, &options, NULL);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	CHECK(allocator.largest_allocation_size == WORK_BUFFER_SIZE);
	CHECK(allocator.live_size == 0);

	error = exfat_resize_map_growth_cluster(
	    &fixture.geometry, &target, fixture.geometry.root_directory_cluster, &mapped_root);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	target_root_sector = exfat_fixture_cluster_sector(&target, mapped_root);
	for (sector_index = 1; sector_index < target.sectors_per_cluster; ++sector_index) {
		CHECK(exfat_fixture_read_sector(
		          &fixture, target_root_sector + sector_index, sector, sizeof(sector)) == 0);
		for (byte_index = 0; byte_index < sizeof(sector); ++byte_index) {
			uint64_t cluster_byte_offset = (uint64_t)sector_index * SECTOR_SIZE + byte_index;

			CHECK(sector[byte_index] ==
			    expected_cluster_byte(
			        fixture.geometry.root_directory_cluster, cluster_byte_offset));
		}
	}

	source_data = fixture.crossing_first_cluster + fixture.crossing_cluster_count / 2;
	error = exfat_resize_map_growth_cluster(&fixture.geometry, &target, source_data, &mapped_data);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	check_cluster_pattern(&fixture, &target, source_data, mapped_data);

	for (operation_index = 0; operation_index < fixture.memory.operation_count; ++operation_index) {
		const struct memory_operation *operation = &fixture.memory.operations[operation_index];

		if (operation->kind == MEMORY_OPERATION_READ &&
		    operation->first_sector == source_root_sector &&
		    operation->sector_count >= fixture.geometry.sectors_per_cluster)
			saw_multi_sector_read = 1;
		if (operation->kind == MEMORY_OPERATION_WRITE &&
		    operation->first_sector == target_root_sector &&
		    operation->sector_count >= fixture.geometry.sectors_per_cluster)
			saw_multi_sector_write = 1;
	}
	CHECK(saw_multi_sector_read);
	CHECK(saw_multi_sector_write);
	exfat_fixture_destroy(&fixture);
}

int main(void)
{
	test_resize();
	test_entry_checksum_unsigned_wrap();
	test_fat_boundary_geometry();
	test_fat_padding_is_not_written();
	test_preflight_is_read_only();
	test_invalid_entry_checksum_does_not_direct_fat_reads();
	test_stream_extension_structure_is_validated();
	test_unsupported_directory_entries();
	test_insufficient_growth_is_rejected();
	test_bitmap_entry_rejections();
	test_malformed_bitmap_fat_chain_is_rejected();
	test_unallocated_benign_entries_are_preserved();
	test_malformed_fat_streams_are_rejected();
	test_misplaced_system_entry_is_rejected();
	test_truncated_entry_set_is_rejected();
	test_secondary_entry_io_errors_are_preserved();
	test_oversized_child_directory_is_rejected();
	test_oversized_root_directory_is_rejected();
	test_reserved_fat_entries();
	test_allocation_model_validates_bitmap();
	test_allocation_model_rejects_shared_directory();
	test_displaced_bad_cluster_is_rejected();
	test_non_displaced_bad_cluster_is_preserved();
	test_allocator_failure_is_read_only();
	test_directory_worklist_growth();
	test_directory_worklist_allocation_failure();
	test_deep_directory_tree();
	test_identity_mapping_rewrites_only_bitmap();
	test_no_fat_chain_ignores_stale_fat();
	test_failure_leaves_volume_dirty();
	test_resize_stages();
	test_options_validation();
	test_mapping_extremes();
	test_contiguous_relocation_is_batched();
	test_multi_sector_cluster_copy();

	if (failure_count != 0) {
		fprintf(stderr, "%d resize integration test(s) failed\n", failure_count);
		return 1;
	}
	printf("library resize: passed\n");
	return 0;
}
