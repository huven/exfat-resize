/* SPDX-License-Identifier: MIT */

#include "block_device.h"
#include "geometry.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define EXFAT_MAX_CLUSTER_COUNT (UINT32_MAX - UINT32_C(10))

static int failure_count;

#define CHECK(expression) \
	do { \
		if (!(expression)) { \
			fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expression); \
			++failure_count; \
		} \
	} while (0)

static struct exfat_resize_geometry base_source(void)
{
	struct exfat_resize_geometry source;

	source.volume_sector_count = 256;
	source.sectors_per_cluster = 1;
	source.fat_offset = 24;
	source.fat_length = 1;
	source.cluster_heap_offset = 26;
	source.cluster_count = 230;
	source.root_directory_cluster = 2;
	return source;
}

static uint32_t heap_shift(
    const struct exfat_resize_geometry *source, const struct exfat_resize_geometry *target)
{
	return (target->cluster_heap_offset - source->cluster_heap_offset) /
	    source->sectors_per_cluster;
}

static void check_common_target(const struct exfat_resize_geometry *source,
    const struct exfat_resize_geometry *target,
    uint64_t target_volume_sector_count)
{
	uint64_t expected_cluster_count;

	CHECK(target->volume_sector_count == target_volume_sector_count);
	CHECK(target->sectors_per_cluster == source->sectors_per_cluster);
	CHECK(target->fat_offset == source->fat_offset);
	CHECK(target->fat_length >= source->fat_length);
	CHECK(target->cluster_heap_offset >= source->cluster_heap_offset);
	CHECK(target->cluster_count > source->cluster_count);
	CHECK((uint64_t)target->cluster_heap_offset +
	        (uint64_t)target->cluster_count * target->sectors_per_cluster <=
	    target->volume_sector_count);
	expected_cluster_count =
	    (target->volume_sector_count - target->cluster_heap_offset) / target->sectors_per_cluster;
	if (expected_cluster_count > EXFAT_MAX_CLUSTER_COUNT)
		expected_cluster_count = EXFAT_MAX_CLUSTER_COUNT;
	CHECK(target->cluster_count == expected_cluster_count);
	CHECK(heap_shift(source, target) >= source->cluster_count ||
	    target->cluster_count - source->cluster_count >= heap_shift(source, target));
}

static void test_growth_without_heap_movement(void)
{
	struct exfat_resize_device_geometry device = { 4096, 4096 };
	struct exfat_resize_geometry source = base_source();
	struct exfat_resize_geometry target;
	enum exfat_resize_error error;

	error = exfat_resize_plan_growth(&device, &source, 1024, &target);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	if (error != EXFAT_RESIZE_SUCCESS)
		return;

	check_common_target(&source, &target, 1024);
	CHECK(target.fat_length == 1);
	CHECK(target.cluster_heap_offset == 26);
	CHECK(target.cluster_count == 998);
	CHECK(target.root_directory_cluster == 2);
	CHECK(heap_shift(&source, &target) == 0);
}

static void test_growth_with_partial_heap_movement(void)
{
	struct exfat_resize_device_geometry device = { 4096, 4096 };
	struct exfat_resize_geometry source = base_source();
	struct exfat_resize_geometry target;
	enum exfat_resize_error error;
	uint32_t mapped;
	unsigned char seen[230] = { 0 };
	uint32_t cluster;

	error = exfat_resize_plan_growth(&device, &source, 2500, &target);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	if (error != EXFAT_RESIZE_SUCCESS)
		return;

	check_common_target(&source, &target, 2500);
	CHECK(target.fat_length == 3);
	CHECK(target.cluster_heap_offset == 27);
	CHECK(target.cluster_count == 2473);
	CHECK(target.root_directory_cluster == 231);
	CHECK(heap_shift(&source, &target) == 1);

	for (cluster = 2; cluster < source.cluster_count + 2; ++cluster) {
		error = exfat_resize_map_growth_cluster(&source, &target, cluster, &mapped);
		CHECK(error == EXFAT_RESIZE_SUCCESS);
		if (error != EXFAT_RESIZE_SUCCESS)
			continue;
		CHECK(mapped >= 2);
		CHECK(mapped < source.cluster_count + 2);
		CHECK(seen[mapped - 2] == 0);
		seen[mapped - 2] = 1;
	}
	for (cluster = 0; cluster < source.cluster_count; ++cluster)
		CHECK(seen[cluster] == 1);

	error = exfat_resize_map_growth_cluster(&source, &target, 0, &mapped);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	CHECK(mapped == 0);
	error = exfat_resize_map_growth_cluster(&source, &target, 1, &mapped);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	CHECK(mapped == 1);
	error = exfat_resize_map_growth_cluster(&source, &target, 2, &mapped);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	CHECK(mapped == 231);
	error = exfat_resize_map_growth_cluster(&source, &target, 3, &mapped);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	CHECK(mapped == 2);
	error = exfat_resize_map_growth_cluster(&source, &target, 231, &mapped);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	CHECK(mapped == 230);
}

static void test_512_byte_sector_geometry(void)
{
	struct exfat_resize_device_geometry device = { 512, 524288 };
	struct exfat_resize_geometry source;
	struct exfat_resize_geometry target;
	enum exfat_resize_error error;

	source.volume_sector_count = 16384;
	source.sectors_per_cluster = 8;
	source.fat_offset = 24;
	source.fat_length = 16;
	source.cluster_heap_offset = 128;
	source.cluster_count = 2032;
	source.root_directory_cluster = 2;

	error = exfat_resize_plan_growth(&device, &source, device.sector_count, &target);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	if (error != EXFAT_RESIZE_SUCCESS)
		return;

	check_common_target(&source, &target, device.sector_count);
	CHECK(target.fat_length == 512);
	CHECK(target.cluster_heap_offset == 536);
	CHECK(target.cluster_count == 65469);
	CHECK(target.root_directory_cluster == 1983);
	CHECK(heap_shift(&source, &target) == 51);
}

static void test_fat_boundary_geometry(void)
{
	static const uint32_t sector_sizes[] = { 512, 1024, 2048, 4096 };
	static const uint32_t cluster_sizes[] = { 1, 8, 64 };
	struct exfat_resize_device_geometry device;
	struct exfat_resize_geometry source;
	struct exfat_resize_geometry target;
	enum exfat_resize_error error;
	uint32_t expected_cluster_count;
	size_t cluster_index;
	size_t sector_index;

	source.fat_offset = 24;
	source.fat_length = 96;
	source.cluster_heap_offset = 256;
	source.cluster_count = 11744;
	source.root_directory_cluster = 2;

	for (sector_index = 0; sector_index < sizeof(sector_sizes) / sizeof(sector_sizes[0]);
	    ++sector_index) {
		device.logical_sector_size = sector_sizes[sector_index];
		expected_cluster_count = 232 * (sector_sizes[sector_index] / 4) - 2;

		for (cluster_index = 0; cluster_index < sizeof(cluster_sizes) / sizeof(cluster_sizes[0]);
		    ++cluster_index) {
			source.sectors_per_cluster = cluster_sizes[cluster_index];
			source.volume_sector_count = source.cluster_heap_offset +
			    (uint64_t)source.cluster_count * source.sectors_per_cluster;
			device.sector_count = source.cluster_heap_offset +
			    (uint64_t)expected_cluster_count * source.sectors_per_cluster;

			error = exfat_resize_plan_growth(&device, &source, device.sector_count, &target);
			CHECK(error == EXFAT_RESIZE_SUCCESS);
			if (error != EXFAT_RESIZE_SUCCESS)
				continue;

			check_common_target(&source, &target, device.sector_count);
			CHECK(target.fat_length == 232);
			CHECK(target.cluster_heap_offset == 256);
			CHECK(target.cluster_count == expected_cluster_count);
			CHECK(target.root_directory_cluster == 2);
			CHECK(heap_shift(&source, &target) == 0);

			device.sector_count += source.sectors_per_cluster;
			error = exfat_resize_plan_growth(&device, &source, device.sector_count, &target);
			CHECK(error == EXFAT_RESIZE_SUCCESS);
			if (error != EXFAT_RESIZE_SUCCESS)
				continue;

			check_common_target(&source, &target, device.sector_count);
			CHECK(target.fat_length == 232);
			CHECK(target.cluster_heap_offset == 256 + source.sectors_per_cluster);
			CHECK(target.cluster_count == expected_cluster_count);
			CHECK(target.root_directory_cluster == 11745);
			CHECK(heap_shift(&source, &target) == 1);
		}
	}
}

static void check_full_heap_movement(uint64_t target_volume_sector_count,
    uint32_t expected_shift,
    uint32_t expected_fat_length,
    uint32_t expected_heap_offset,
    uint32_t expected_cluster_count)
{
	struct exfat_resize_device_geometry device = { 4096, target_volume_sector_count };
	struct exfat_resize_geometry source = base_source();
	struct exfat_resize_geometry target;
	enum exfat_resize_error error;
	uint32_t cluster;
	uint32_t mapped;

	error = exfat_resize_plan_growth(&device, &source, target_volume_sector_count, &target);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	if (error != EXFAT_RESIZE_SUCCESS)
		return;

	check_common_target(&source, &target, target_volume_sector_count);
	CHECK(target.fat_length == expected_fat_length);
	CHECK(target.cluster_heap_offset == expected_heap_offset);
	CHECK(target.cluster_count == expected_cluster_count);
	CHECK(target.root_directory_cluster == source.root_directory_cluster);
	CHECK(heap_shift(&source, &target) == expected_shift);

	for (cluster = 0; cluster < source.cluster_count + 2; ++cluster) {
		error = exfat_resize_map_growth_cluster(&source, &target, cluster, &mapped);
		CHECK(error == EXFAT_RESIZE_SUCCESS);
		CHECK(mapped == cluster);
	}
}

static void test_full_heap_movement(void)
{
	check_full_heap_movement(236799, 230, 232, 256, 236543);
	check_full_heap_movement(237824, 231, 233, 257, 237567);
}

static void test_maximum_cluster_count(void)
{
	struct exfat_resize_device_geometry device = { 4096, UINT64_MAX };
	struct exfat_resize_geometry source = base_source();
	struct exfat_resize_geometry limit_source;
	struct exfat_resize_geometry target;
	enum exfat_resize_error error;

	error = exfat_resize_plan_growth(&device, &source, UINT64_MAX, &target);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	if (error != EXFAT_RESIZE_SUCCESS)
		return;

	check_common_target(&source, &target, UINT64_MAX);
	CHECK(target.cluster_count == EXFAT_MAX_CLUSTER_COUNT);
	CHECK(target.fat_length == UINT32_C(4194304));
	CHECK(target.cluster_heap_offset == UINT32_C(4194328));
	CHECK(target.root_directory_cluster == source.root_directory_cluster);
	CHECK(heap_shift(&source, &target) > source.cluster_count);

	limit_source = target;
	limit_source.volume_sector_count =
	    (uint64_t)limit_source.cluster_heap_offset + limit_source.cluster_count;
	device.sector_count = limit_source.volume_sector_count + 1;
	error = exfat_resize_plan_growth(&device, &limit_source, device.sector_count, &target);
	CHECK(error == EXFAT_RESIZE_CLUSTER_LIMIT_REACHED);
}

static void test_invalid_targets(void)
{
	struct exfat_resize_device_geometry device = { 4096, 4096 };
	struct exfat_resize_geometry source = base_source();
	struct exfat_resize_geometry target;
	struct exfat_resize_geometry unchanged;
	enum exfat_resize_error error;

	memset(&target, 0xa5, sizeof(target));
	unchanged = target;

	error = exfat_resize_plan_growth(&device, &source, source.volume_sector_count, &target);
	CHECK(error == EXFAT_RESIZE_INVALID_ARGUMENT);
	CHECK(memcmp(&target, &unchanged, sizeof(target)) == 0);

	error = exfat_resize_plan_growth(&device, &source, source.volume_sector_count - 1, &target);
	CHECK(error == EXFAT_RESIZE_INVALID_ARGUMENT);
	CHECK(memcmp(&target, &unchanged, sizeof(target)) == 0);

	error = exfat_resize_plan_growth(&device, &source, device.sector_count + 1, &target);
	CHECK(error == EXFAT_RESIZE_OUT_OF_BOUNDS);
	CHECK(memcmp(&target, &unchanged, sizeof(target)) == 0);

	source.sectors_per_cluster = 8;
	source.cluster_heap_offset = 32;
	source.cluster_count = 28;
	source.volume_sector_count = 256;
	error = exfat_resize_plan_growth(&device, &source, 257, &target);
	CHECK(error == EXFAT_RESIZE_INSUFFICIENT_GROWTH);
	CHECK(memcmp(&target, &unchanged, sizeof(target)) == 0);

	error = exfat_resize_plan_growth(NULL, &source, 512, &target);
	CHECK(error == EXFAT_RESIZE_INVALID_ARGUMENT);
	error = exfat_resize_plan_growth(&device, NULL, 512, &target);
	CHECK(error == EXFAT_RESIZE_INVALID_ARGUMENT);
	error = exfat_resize_plan_growth(&device, &source, 512, NULL);
	CHECK(error == EXFAT_RESIZE_INVALID_ARGUMENT);
}

static void test_invalid_mapping(void)
{
	struct exfat_resize_geometry source = base_source();
	struct exfat_resize_geometry target;
	enum exfat_resize_error error;
	uint32_t mapped = UINT32_C(0xa5a5a5a5);

	target = source;
	target.volume_sector_count = 1024;
	target.cluster_count = 998;

	error = exfat_resize_map_growth_cluster(&source, &target, source.cluster_count + 2, &mapped);
	CHECK(error == EXFAT_RESIZE_OUT_OF_BOUNDS);
	CHECK(mapped == UINT32_C(0xa5a5a5a5));

	target.cluster_heap_offset = source.cluster_heap_offset + 1;
	target.sectors_per_cluster = 2;
	error = exfat_resize_map_growth_cluster(&source, &target, 2, &mapped);
	CHECK(error == EXFAT_RESIZE_INVALID_ARGUMENT);
	CHECK(mapped == UINT32_C(0xa5a5a5a5));

	error = exfat_resize_map_growth_cluster(NULL, &target, 2, &mapped);
	CHECK(error == EXFAT_RESIZE_INVALID_ARGUMENT);
	error = exfat_resize_map_growth_cluster(&source, NULL, 2, &mapped);
	CHECK(error == EXFAT_RESIZE_INVALID_ARGUMENT);
	error = exfat_resize_map_growth_cluster(&source, &target, 2, NULL);
	CHECK(error == EXFAT_RESIZE_INVALID_ARGUMENT);
}

int main(void)
{
	test_growth_without_heap_movement();
	test_growth_with_partial_heap_movement();
	test_512_byte_sector_geometry();
	test_fat_boundary_geometry();
	test_full_heap_movement();
	test_maximum_cluster_count();
	test_invalid_targets();
	test_invalid_mapping();

	if (failure_count != 0) {
		fprintf(stderr, "%d geometry test(s) failed\n", failure_count);
		return 1;
	}

	printf("geometry: passed\n");
	return 0;
}
