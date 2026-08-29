/* SPDX-License-Identifier: MIT */

#ifndef EXFAT_RESIZE_TEST_EXFAT_FIXTURE_H
#define EXFAT_RESIZE_TEST_EXFAT_FIXTURE_H

#include "geometry.h"
#include "support/memory_block_device.h"

#include <stddef.h>
#include <stdint.h>

struct exfat_fixture {
	struct memory_block_device memory;
	struct exfat_resize_geometry geometry;
	uint32_t bitmap_clusters[3];
	uint32_t bitmap_cluster_count;
	uint32_t fragmented_clusters[3];
	uint32_t crossing_first_cluster;
	uint32_t crossing_cluster_count;
};

static inline uint64_t exfat_fixture_target_size(uint64_t target_sector_count)
{
	if (target_sector_count > UINT64_MAX / UINT64_C(512))
		return UINT64_MAX;
	return target_sector_count * UINT64_C(512);
}

static inline enum exfat_resize_error exfat_fixture_resize(
    const struct exfat_resize_block_device *device,
    uint64_t target_sector_count,
    const struct exfat_resize_allocator *allocator,
    enum exfat_resize_stage *stage)
{
	return exfat_resize(
	    device, exfat_fixture_target_size(target_sector_count), allocator, NULL, stage);
}

static inline enum exfat_resize_error exfat_fixture_resize_with_monitor(
    const struct exfat_resize_block_device *device,
    uint64_t target_sector_count,
    const struct exfat_resize_allocator *allocator,
    const struct exfat_resize_monitor *monitor,
    enum exfat_resize_stage *stage)
{
	return exfat_resize(
	    device, exfat_fixture_target_size(target_sector_count), allocator, monitor, stage);
}

int exfat_fixture_initialize(struct exfat_fixture *fixture, uint64_t device_sector_count);
int exfat_fixture_initialize_with_sectors_per_cluster(
    struct exfat_fixture *fixture, uint64_t device_sector_count, uint32_t sectors_per_cluster);
void exfat_fixture_destroy(struct exfat_fixture *fixture);
int exfat_fixture_write_boot_regions(struct exfat_fixture *fixture);

int exfat_fixture_add_child_directories(
    struct exfat_fixture *fixture, uint32_t first_cluster, uint32_t count);
int exfat_fixture_add_directory_chain(
    struct exfat_fixture *fixture, uint32_t first_cluster, uint32_t count);
int exfat_fixture_add_contiguous_child_directory(struct exfat_fixture *fixture,
    uint32_t directory_first_cluster,
    uint32_t directory_cluster_count,
    uint32_t data_first_cluster);

int exfat_fixture_read_sector(
    struct exfat_fixture *fixture, uint64_t sector, void *buffer, size_t buffer_size);

uint64_t exfat_fixture_cluster_sector(
    const struct exfat_resize_geometry *geometry, uint32_t cluster);

#endif
