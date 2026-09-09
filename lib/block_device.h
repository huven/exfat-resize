/* SPDX-License-Identifier: MIT */

#ifndef EXFAT_RESIZE_BLOCK_DEVICE_H
#define EXFAT_RESIZE_BLOCK_DEVICE_H

#include "exfat_resize.h"

#include <stddef.h>
#include <stdint.h>

#define EXFAT_RESIZE_MIN_SECTOR_SIZE UINT32_C(512)
#define EXFAT_RESIZE_MAX_SECTOR_SIZE UINT32_C(4096)

struct exfat_resize_device_geometry {
	uint32_t logical_sector_size;
	uint64_t sector_count;
};

int exfat_resize_sector_size_is_supported(uint32_t size);

enum exfat_resize_error exfat_resize_validate_block_device(
    const struct exfat_resize_block_device *device);

/*
 * buffer_size is the number of bytes available from buffer, not merely the
 * requested transfer size (sector_count * device->sector_size).
 * buffer_size is used to validate the request.
 */
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
