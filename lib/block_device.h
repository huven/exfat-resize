/* SPDX-License-Identifier: MIT */

#ifndef EXFAT_RESIZE_BLOCK_DEVICE_H
#define EXFAT_RESIZE_BLOCK_DEVICE_H

#include "exfat_resize.h"

#include <stddef.h>
#include <stdint.h>

struct exfat_resize_device_geometry {
	uint32_t logical_sector_size;
	uint64_t sector_count;
};

enum exfat_resize_error exfat_resize_validate_block_device(
    const struct exfat_resize_block_device *device);

enum exfat_resize_error exfat_resize_block_device_read(
    const struct exfat_resize_block_device *device,
    uint64_t first_sector,
    uint32_t sector_count,
    void *buffer,
    size_t buffer_size);

enum exfat_resize_error exfat_resize_block_device_write(
    const struct exfat_resize_block_device *device,
    uint64_t first_sector,
    uint32_t sector_count,
    const void *buffer,
    size_t buffer_size);

enum exfat_resize_error exfat_resize_block_device_sync(
    const struct exfat_resize_block_device *device);

#endif
