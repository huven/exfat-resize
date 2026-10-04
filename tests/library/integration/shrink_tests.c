/* SPDX-License-Identifier: MIT */
#include "boot_region.h"
#include "endian.h"
#include "exfat_resize.h"
#include "stream.h"
#include "support/shrink_fixture.h"
#include "support/test_allocator.h"
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

static int cancellation_requested(void *opaque)
{
	return *(int *)opaque;
}

static void cancel_at_preparing(void *opaque, const struct exfat_resize_event *event)
{
	if (event->code == EXFAT_RESIZE_EVENT_CODE_STAGE_ENTERED &&
	    event->values[0] == EXFAT_RESIZE_STAGE_PREPARING)
		*(int *)opaque = 1;
}

static void validate_allocation(struct exfat_fixture *f)
{
	struct test_allocator allocator = { 0 };
	struct exfat_resize_allocator callbacks = test_allocator_callbacks(&allocator);
	int cancel = 0;
	struct exfat_resize_monitor monitor = { .context = &cancel,
		.cancellation_requested = cancellation_requested,
		.report_event = cancel_at_preparing };
	enum exfat_resize_stage stage;
	unsigned char boot[512];
	struct exfat_resize_geometry geometry;
	CHECK(exfat_resize_read_boot_regions(&f->memory.device, boot, sizeof(boot), &geometry) ==
	    EXFAT_RESIZE_SUCCESS);
	CHECK(exfat_fixture_resize_with_monitor(&f->memory.device,
	          geometry.volume_sector_count - geometry.sectors_per_cluster, &callbacks, &monitor,
	          &stage) == EXFAT_RESIZE_CANCELLED);
	CHECK(stage == EXFAT_RESIZE_STAGE_PREPARING);
	CHECK(test_allocator_is_clean(&allocator));
}

static void check_no_writes(const struct exfat_fixture *f)
{
	size_t i;
	for (i = 0; i < f->memory.operation_count; ++i)
		CHECK(f->memory.operations[i].kind == MEMORY_OPERATION_READ);
}

static size_t read_bitmap(struct exfat_fixture *f, unsigned char *bitmap, size_t capacity)
{
	unsigned char sector[512];
	uint32_t cluster = 0;
	uint64_t length = 0;
	size_t copied = 0;
	CHECK(exfat_fixture_read_sector(f, 0, sector, sizeof(sector)) == 0);
	CHECK(exfat_resize_load_le32(sector, sizeof(sector), 96, &cluster) == EXFAT_RESIZE_SUCCESS);
	CHECK(exfat_fixture_read_sector(
	          f, exfat_fixture_cluster_sector(&f->geometry, cluster), sector, sizeof(sector)) == 0);
	CHECK(sector[0] == 0x81);
	CHECK(exfat_resize_load_le32(sector, sizeof(sector), 20, &cluster) == EXFAT_RESIZE_SUCCESS);
	CHECK(exfat_resize_load_le64(sector, sizeof(sector), 24, &length) == EXFAT_RESIZE_SUCCESS);
	CHECK(length != 0 && length <= capacity);
	if (length == 0 || length > capacity)
		return 0;
	while (copied < length) {
		uint32_t index;
		for (index = 0; index < f->geometry.sectors_per_cluster && copied < length; ++index) {
			size_t amount =
			    length - copied < sizeof(sector) ? (size_t)(length - copied) : sizeof(sector);
			CHECK(exfat_fixture_read_sector(f,
			          exfat_fixture_cluster_sector(&f->geometry, cluster) + index, sector,
			          sizeof(sector)) == 0);
			memcpy(bitmap + copied, sector, amount);
			copied += amount;
		}
		cluster = shrink_fixture_fat(f, cluster);
	}
	CHECK(cluster == EXFAT_FAT_END_OF_CHAIN);
	return copied;
}

static void test_newly_reserved_bitmap_bits(uint32_t spc)
{
	uint32_t remainder;
	for (remainder = 0; remainder < 8; ++remainder) {
		struct exfat_fixture f;
		struct test_allocator allocator = { 0 };
		struct exfat_resize_allocator callbacks = test_allocator_callbacks(&allocator);
		enum exfat_resize_stage stage;
		unsigned char source[1250];
		unsigned char bitmap[1250];
		unsigned char boot[512];
		uint32_t target = 2000 + remainder;
		uint32_t cluster;
		uint32_t bit;
		uint32_t used = 0;
		size_t length;
		CHECK(shrink_fixture_initialize(&f, spc) == 0);
		/* Include retained bad markers, including the last valid bit for spc=4.
		 * Twenty retained allocations make even one miscounted tail marker
		 * change PercentInUse for every non-byte-aligned target here. */
		for (cluster = 70; cluster < (spc == 1 ? 74u : 73u); ++cluster) {
			CHECK(shrink_fixture_set_fat(&f, cluster, EXFAT_FAT_BAD_CLUSTER) == 0);
			CHECK(shrink_fixture_set_bit(&f, cluster, 1) == 0);
		}
		if (spc == 4) {
			CHECK(shrink_fixture_set_fat(&f, target + 1, EXFAT_FAT_BAD_CLUSTER) == 0);
			CHECK(shrink_fixture_set_bit(&f, target + 1, 1) == 0);
		}
		for (cluster = remainder == 0 ? 2005 : target + 2; cluster < 2010; ++cluster) {
			CHECK(shrink_fixture_set_fat(&f, cluster, EXFAT_FAT_BAD_CLUSTER) == 0);
			CHECK(shrink_fixture_set_bit(&f, cluster, 1) == 0);
		}
		CHECK(read_bitmap(&f, source, sizeof(source)) == sizeof(source));
		CHECK(exfat_fixture_resize(&f.memory.device,
		          f.geometry.cluster_heap_offset + (uint64_t)target * spc, &callbacks,
		          &stage) == EXFAT_RESIZE_SUCCESS);
		CHECK(stage == EXFAT_RESIZE_STAGE_COMPLETED);
		CHECK(memory_block_device_crash(&f.memory) == 0);
		CHECK(shrink_fixture_verify(&f, target) == 0);
		length = read_bitmap(&f, bitmap, sizeof(bitmap));
		CHECK(length == (target + 7u) / 8);
		CHECK(bitmap[length - 1] ==
		    (spc == 1 ? (remainder == 0 ? 0xe0 : 1)
		              : (1u << (remainder == 0 ? 7 : remainder - 1))));
		for (bit = 0; bit < target; ++bit) {
			unsigned int mask = 1u << (bit % 8);
			used += (bitmap[bit / 8] & mask) != 0;
			/* All new destinations are below cluster 50 in this fixture. */
			if (bit >= 48)
				CHECK((bitmap[bit / 8] & mask) == (source[bit / 8] & mask));
		}
		CHECK(used == 20);
		CHECK(exfat_fixture_read_sector(&f, 0, boot, sizeof(boot)) == 0);
		CHECK(boot[112] == (uint64_t)used * 100 / target);
		CHECK(shrink_fixture_fat(&f, 60) == EXFAT_FAT_BAD_CLUSTER);
		for (cluster = 70; cluster < (spc == 1 ? 74u : 73u); ++cluster)
			CHECK(shrink_fixture_fat(&f, cluster) == EXFAT_FAT_BAD_CLUSTER);
		if (spc == 4)
			CHECK(shrink_fixture_fat(&f, target + 1) == EXFAT_FAT_BAD_CLUSTER);
		validate_allocation(&f);
		CHECK(test_allocator_is_clean(&allocator));
		CHECK(exfat_fixture_destroy(&f) == 0);
	}
}

static void test_existing_reserved_bitmap_bits(uint32_t spc, int slack_only)
{
	struct exfat_fixture f;
	struct test_allocator allocator = { 0 };
	struct exfat_resize_allocator callbacks = test_allocator_callbacks(&allocator);
	unsigned char source[1250];
	unsigned char bitmap[1250];
	enum exfat_resize_stage stage;
	uint32_t cluster;
	uint32_t target = slack_only ? 9997 : 9993;
	CHECK(shrink_fixture_initialize(&f, spc) == 0);
	f.geometry.cluster_count = 9997;
	f.geometry.volume_sector_count = f.geometry.cluster_heap_offset + (uint64_t)9997 * spc;
	if (slack_only)
		f.geometry.volume_sector_count += 3;
	CHECK(exfat_fixture_write_boot_regions(&f) == 0);
	for (cluster = 9994; cluster < 9999; ++cluster) {
		CHECK(shrink_fixture_set_fat(&f, cluster, EXFAT_FAT_BAD_CLUSTER) == 0);
		CHECK(shrink_fixture_set_bit(&f, cluster, 1) == 0);
	}
	/* These three padding bits were reserved before the resize. */
	for (cluster = 9999; cluster < 10002; ++cluster)
		CHECK(shrink_fixture_set_bit(&f, cluster, 1) == 0);
	CHECK(read_bitmap(&f, source, sizeof(source)) == sizeof(source));
	CHECK(source[1249] == 0xff);
	CHECK(exfat_fixture_resize(&f.memory.device,
	          slack_only ? f.geometry.volume_sector_count - 1
	                     : f.geometry.cluster_heap_offset + (uint64_t)target * spc,
	          &callbacks, &stage) == EXFAT_RESIZE_SUCCESS);
	CHECK(stage == EXFAT_RESIZE_STAGE_COMPLETED);
	CHECK(memory_block_device_crash(&f.memory) == 0);
	CHECK(shrink_fixture_verify(&f, target) == 0);
	CHECK(read_bitmap(&f, bitmap, sizeof(bitmap)) == sizeof(bitmap));
	CHECK(memcmp(source, bitmap, sizeof(bitmap) - 1) == 0);
	CHECK(bitmap[1249] == (slack_only ? 0xff : 0xe1));
	CHECK(shrink_fixture_fat(&f, 9994) == EXFAT_FAT_BAD_CLUSTER);
	validate_allocation(&f);
	CHECK(test_allocator_is_clean(&allocator));
	CHECK(exfat_fixture_destroy(&f) == 0);
}

static size_t test_success(uint32_t spc)
{
	struct exfat_fixture f;
	struct test_allocator allocator = { 0 };
	struct exfat_resize_allocator callbacks = test_allocator_callbacks(&allocator);
	enum exfat_resize_stage stage;
	unsigned char boot[512];
	struct exfat_resize_geometry geometry;
	size_t attempts;
	CHECK(shrink_fixture_initialize(&f, spc) == 0);
	CHECK(shrink_fixture_verify(&f, 10000) == 0);
	validate_allocation(&f);
	CHECK(exfat_fixture_resize(&f.memory.device, shrink_fixture_target(&f), &callbacks, &stage) ==
	    EXFAT_RESIZE_SUCCESS);
	CHECK(stage == EXFAT_RESIZE_STAGE_COMPLETED);
	CHECK(test_allocator_is_clean(&allocator));
	attempts = allocator.allocation_attempts;
	CHECK(memory_block_device_crash(&f.memory) == 0);
	CHECK(shrink_fixture_verify(&f, 2000) == 0);
	CHECK(exfat_resize_read_boot_regions(&f.memory.device, boot, sizeof(boot), &geometry) ==
	    EXFAT_RESIZE_SUCCESS);
	CHECK(geometry.fat_length == exfat_resize_used_fat_sector_count(2000, 512));
	CHECK(geometry.cluster_heap_offset == f.geometry.cluster_heap_offset);
	validate_allocation(&f);
	/* Truncation is external and is safe only after the successful return. */
	f.memory.device.sector_count = shrink_fixture_target(&f);
	CHECK(shrink_fixture_verify(&f, 2000) == 0);
	f.memory.device.sector_count = 104 + (uint64_t)20000 * spc;
	/* Modest regrowth keeps heap coordinates, enabling the independent payload oracle. */
	CHECK(exfat_fixture_resize(&f.memory.device, 104 + (uint64_t)8000 * spc, &callbacks, &stage) ==
	    EXFAT_RESIZE_SUCCESS);
	CHECK(shrink_fixture_verify(&f, 8000) == 0);
	validate_allocation(&f);
	CHECK(exfat_fixture_destroy(&f) == 0);
	return attempts;
}

static void test_preflight_failures(size_t allocations)
{
	size_t i;
	for (i = 1; i <= allocations; ++i) {
		struct exfat_fixture f;
		struct test_allocator allocator = { 0 };
		struct exfat_resize_allocator callbacks = test_allocator_callbacks(&allocator);
		enum exfat_resize_stage stage;
		CHECK(shrink_fixture_initialize(&f, 1) == 0);
		test_allocator_set_fail_on_attempt(&allocator, i);
		CHECK(exfat_fixture_resize(&f.memory.device, shrink_fixture_target(&f), &callbacks,
		          &stage) == EXFAT_RESIZE_OUT_OF_MEMORY);
		CHECK(stage == EXFAT_RESIZE_STAGE_PREFLIGHT);
		CHECK(test_allocator_is_clean(&allocator));
		check_no_writes(&f);
		CHECK(exfat_fixture_destroy(&f) == 0);
	}
	for (i = 0; i < 5; ++i) {
		struct exfat_fixture f;
		struct test_allocator allocator = { 0 };
		struct exfat_resize_allocator callbacks = test_allocator_callbacks(&allocator);
		enum exfat_resize_stage stage;
		enum exfat_resize_error expected;
		CHECK(shrink_fixture_initialize(&f, 1) == 0);
		if (i == 0 || i >= 3) {
			uint32_t cluster;
			uint32_t marked = 0;
			for (cluster = 2; cluster < 1999; ++cluster) {
				if (cluster == 50 || cluster == 60)
					continue;
				CHECK(shrink_fixture_set_fat(&f, cluster, EXFAT_FAT_BAD_CLUSTER) == 0);
				CHECK(shrink_fixture_set_bit(&f, cluster, 1) == 0);
				/* Leave twelve destinations for thirteen tail clusters. The final
				 * bitmap would free two clusters, but compaction needs the full bitmap.
				 * The next case leaves exactly thirteen and must succeed. */
				if (i >= 3 && ++marked == (i == 3 ? 1983u : 1982u))
					break;
			}
			expected = i == 4 ? EXFAT_RESIZE_SUCCESS : EXFAT_RESIZE_INSUFFICIENT_SHRINK_SPACE;
		} else if (i == 1) {
			CHECK(shrink_fixture_set_bit(&f, 8000, 0) == 0);
			expected = EXFAT_RESIZE_INVALID_FILESYSTEM;
		} else {
			CHECK(shrink_fixture_set_fat(&f, 8002, 8000) == 0);
			expected = EXFAT_RESIZE_INVALID_FILESYSTEM;
		}
		memory_block_device_clear_operations(&f.memory);
		CHECK(exfat_fixture_resize(
		          &f.memory.device, shrink_fixture_target(&f), &callbacks, &stage) == expected);
		if (expected == EXFAT_RESIZE_SUCCESS) {
			CHECK(stage == EXFAT_RESIZE_STAGE_COMPLETED);
			CHECK(shrink_fixture_verify(&f, 2000) == 0);
			validate_allocation(&f);
		} else {
			CHECK(stage == EXFAT_RESIZE_STAGE_PREFLIGHT);
			check_no_writes(&f);
		}
		CHECK(test_allocator_is_clean(&allocator));
		CHECK(exfat_fixture_destroy(&f) == 0);
	}
}

static void test_slack_and_rounding(void)
{
	struct exfat_fixture f;
	struct test_allocator allocator = { 0 };
	struct exfat_resize_allocator callbacks = test_allocator_callbacks(&allocator);
	enum exfat_resize_stage stage;
	CHECK(shrink_fixture_initialize(&f, 4) == 0);
	f.geometry.volume_sector_count += 3;
	CHECK(exfat_fixture_write_boot_regions(&f) == 0);
	CHECK(exfat_resize(&f.memory.device, (f.geometry.volume_sector_count - 1) * 512 + 511,
	          &callbacks, NULL, &stage) == EXFAT_RESIZE_SUCCESS);
	CHECK(shrink_fixture_verify(&f, 10000) == 0);
	validate_allocation(&f);
	CHECK(test_allocator_is_clean(&allocator));
	CHECK(exfat_fixture_destroy(&f) == 0);
}

static void test_growing_workspace(void)
{
	size_t attempt;
	size_t allocations = 0;
	for (attempt = 0; attempt <= allocations; ++attempt) {
		struct exfat_fixture f;
		struct test_allocator allocator = { 0 };
		struct exfat_resize_allocator callbacks = test_allocator_callbacks(&allocator);
		enum exfat_resize_stage stage;
		enum exfat_resize_error error;
		CHECK(exfat_fixture_initialize(&f, 20000) == 0);
		CHECK(exfat_fixture_add_directory_chain(&f, 9000, 40) == 0);
		memory_block_device_clear_operations(&f.memory);
		if (attempt != 0)
			test_allocator_set_fail_on_attempt(&allocator, attempt);
		error = exfat_fixture_resize(
		    &f.memory.device, f.geometry.cluster_heap_offset + 2000, &callbacks, &stage);
		if (attempt == 0) {
			CHECK(error == EXFAT_RESIZE_SUCCESS);
			allocations = allocator.allocation_attempts;
			validate_allocation(&f);
		} else {
			CHECK(error == EXFAT_RESIZE_OUT_OF_MEMORY);
			CHECK(stage == EXFAT_RESIZE_STAGE_PREFLIGHT);
			check_no_writes(&f);
		}
		CHECK(test_allocator_is_clean(&allocator));
		CHECK(exfat_fixture_destroy(&f) == 0);
	}
}

static void test_geometry(void)
{
	struct exfat_resize_device_geometry device = { 512, 50000 };
	struct exfat_resize_geometry source = { .volume_sector_count = 10104,
		.sectors_per_cluster = 1,
		.fat_offset = 24,
		.fat_length = 80,
		.cluster_heap_offset = 104,
		.cluster_count = 10000,
		.root_directory_cluster = 9000 };
	struct exfat_resize_geometry target;
	CHECK(exfat_resize_plan_shrink(&device, &source, 2104, &target) == EXFAT_RESIZE_SUCCESS);
	CHECK(target.cluster_count == 2000 && target.root_directory_cluster == 9000);
	CHECK(target.fat_offset == source.fat_offset && target.cluster_heap_offset == 104);
	CHECK(
	    exfat_resize_plan_shrink(&device, &source, 2047, &target) == EXFAT_RESIZE_INVALID_ARGUMENT);
	CHECK(exfat_resize_plan_shrink(&device, &source, 10104, &target) ==
	    EXFAT_RESIZE_INVALID_ARGUMENT);
	CHECK(exfat_resize_plan_shrink(&device, &source, 20000, &target) ==
	    EXFAT_RESIZE_INVALID_ARGUMENT);
	CHECK(exfat_resize_plan_shrink(NULL, &source, 2104, &target) == EXFAT_RESIZE_INVALID_ARGUMENT);
}

int main(void)
{
	size_t allocations = test_success(1);
	(void)test_success(4);
	test_preflight_failures(allocations);
	test_slack_and_rounding();
	test_newly_reserved_bitmap_bits(1);
	test_newly_reserved_bitmap_bits(4);
	test_existing_reserved_bitmap_bits(1, 0);
	test_existing_reserved_bitmap_bits(4, 0);
	test_existing_reserved_bitmap_bits(4, 1);
	test_geometry();
	test_growing_workspace();
	return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
