/* SPDX-License-Identifier: MIT */

#include "block_device.h"

#include <stddef.h>
#include <stdint.h>

static int supported_sector_size(uint32_t size)
{
	return size >= 512 && size <= 4096 && (size & (size - 1)) == 0;
}

enum exfat_resize_error exfat_resize_validate_block_device(
    const struct exfat_resize_block_device *device)
{
	if (device == NULL || device->read == NULL || device->write == NULL || device->sync == NULL)
		return EXFAT_RESIZE_INVALID_DEVICE;
	if (!supported_sector_size(device->sector_size) || device->sector_count == 0)
		return EXFAT_RESIZE_INVALID_DEVICE;
	return EXFAT_RESIZE_SUCCESS;
}

static enum exfat_resize_error validate_transfer(const struct exfat_resize_block_device *device,
    uint64_t first_sector,
    uint32_t sector_count,
    const void *buffer,
    size_t buffer_size)
{
	size_t required_size;

	if (sector_count == 0) {
		if (first_sector > device->sector_count)
			return EXFAT_RESIZE_OUT_OF_BOUNDS;
		return EXFAT_RESIZE_SUCCESS;
	}
	if (buffer == NULL)
		return EXFAT_RESIZE_INVALID_ARGUMENT;
	if ((size_t)sector_count > SIZE_MAX / device->sector_size)
		return EXFAT_RESIZE_ARITHMETIC_OVERFLOW;
	required_size = (size_t)sector_count * device->sector_size;
	if (buffer_size < required_size)
		return EXFAT_RESIZE_INVALID_ARGUMENT;
	if ((uint64_t)sector_count > device->sector_count ||
	    first_sector > device->sector_count - (uint64_t)sector_count)
		return EXFAT_RESIZE_OUT_OF_BOUNDS;
	return EXFAT_RESIZE_SUCCESS;
}

enum exfat_resize_error exfat_resize_block_device_read(
    const struct exfat_resize_block_device *device,
    uint64_t first_sector,
    uint32_t sector_count,
    void *buffer,
    size_t buffer_size)
{
	enum exfat_resize_error error =
	    validate_transfer(device, first_sector, sector_count, buffer, buffer_size);

	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	if (sector_count == 0)
		return EXFAT_RESIZE_SUCCESS;
	if (device->read(device->context, first_sector, sector_count, buffer) != 0)
		return EXFAT_RESIZE_IO_ERROR;
	return EXFAT_RESIZE_SUCCESS;
}

enum exfat_resize_error exfat_resize_block_device_write(
    const struct exfat_resize_block_device *device,
    uint64_t first_sector,
    uint32_t sector_count,
    const void *buffer,
    size_t buffer_size)
{
	enum exfat_resize_error error =
	    validate_transfer(device, first_sector, sector_count, buffer, buffer_size);

	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	if (sector_count == 0)
		return EXFAT_RESIZE_SUCCESS;
	if (device->write(device->context, first_sector, sector_count, buffer) != 0)
		return EXFAT_RESIZE_IO_ERROR;
	return EXFAT_RESIZE_SUCCESS;
}

enum exfat_resize_error exfat_resize_block_device_sync(
    const struct exfat_resize_block_device *device)
{
	if (device->sync(device->context) != 0)
		return EXFAT_RESIZE_IO_ERROR;
	return EXFAT_RESIZE_SUCCESS;
}
