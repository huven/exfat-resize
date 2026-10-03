/* SPDX-License-Identifier: MIT */

#include "device.h"

#include "cli.h"
#include "windows/device_internal.h"
#include "windows/partition.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <winioctl.h>

static int cancellation_requested(const struct cli_cancellation *cancellation)
{
	return cancellation != NULL && cancellation->requested != NULL &&
	    cancellation->requested(cancellation->context) != 0;
}

enum device_partition_growth_result device_grow_partition(struct device *device,
    const char *path,
    uint64_t target_size,
    const struct cli_cancellation *cancellation,
    enum device_partition_state *partition_state,
    char *error,
    size_t error_size)
{
	struct windows_partition plan;
	PARTITION_INFORMATION_EX partition;
	DISK_GROW_PARTITION request;
	GET_LENGTH_INFORMATION volume_length;
	DISK_EXTENT extent;
	HANDLE disk;
	uint64_t current_length;
	uint64_t requested_length;
	uint64_t growth;
	uint64_t starting_offset;
	uint32_t sector_size = device->block_device.sector_size;
	DWORD disk_number, partition_number, returned;
	enum device_partition_growth_result result = DEVICE_PARTITION_GROWTH_ERROR;

	*partition_state = DEVICE_PARTITION_UNCHANGED;
	if (device->volume_io_buffer == NULL) {
		windows_device_set_error(
		    error, error_size, path, "--grow-partition requires a logical Windows volume target");
		return DEVICE_PARTITION_GROWTH_ERROR;
	}
	if (device->block_device.sector_count > UINT64_MAX / sector_size) {
		windows_device_set_error(error, error_size, path, "the logical volume size is too large");
		return DEVICE_PARTITION_GROWTH_ERROR;
	}
	current_length = device->block_device.sector_count * (uint64_t)sector_size;
	if (target_size <= current_length)
		return DEVICE_PARTITION_GROWTH_SUCCESS;
	if (target_size > (uint64_t)LLONG_MAX ||
	    target_size > UINT64_MAX - ((uint64_t)sector_size - 1)) {
		windows_device_set_error(
		    error, error_size, path, "the requested partition size is too large");
		return DEVICE_PARTITION_GROWTH_ERROR;
	}
	requested_length = (target_size + sector_size - 1) / sector_size * (uint64_t)sector_size;
	if (requested_length > (uint64_t)LLONG_MAX) {
		windows_device_set_error(
		    error, error_size, path, "the requested partition size is too large");
		return DEVICE_PARTITION_GROWTH_ERROR;
	}

	if (windows_partition_open(device, path, &plan, error, error_size) != 0)
		return DEVICE_PARTITION_GROWTH_ERROR;
	disk = plan.disk;
	partition = plan.layout->PartitionEntry[plan.partition_index];
	disk_number = plan.disk_number;
	partition_number = partition.PartitionNumber;
	starting_offset = (uint64_t)partition.StartingOffset.QuadPart;
	if (requested_length > plan.maximum_length) {
		windows_device_set_error(error, error_size, path,
		    "not enough immediately trailing unallocated space for the requested size");
		goto out;
	}
	/*
	 * This is the final cancellation point until partition mutation, required
	 * synchronization, property refresh, and geometry readback have completed.
	 */
	if (cancellation_requested(cancellation)) {
		result = DEVICE_PARTITION_GROWTH_CANCELLED;
		goto out;
	}

	growth = requested_length - current_length;
	(void)memset(&request, 0, sizeof(request));
	request.PartitionNumber = partition_number;
	request.BytesToGrow.QuadPart = (LONGLONG)growth;
	*partition_state = DEVICE_PARTITION_UPDATE_ATTEMPTED;
	if (!DeviceIoControl(
	        disk, IOCTL_DISK_GROW_PARTITION, &request, sizeof(request), NULL, 0, &returned, NULL)) {
		windows_device_set_operation_error(
		    error, error_size, path, "cannot grow the partition", GetLastError());
		goto out;
	}
	*partition_state = DEVICE_PARTITION_GROWN;
	if (!FlushFileBuffers(disk)) {
		windows_device_set_operation_error(error, error_size, path,
		    "cannot synchronize the enlarged partition table", GetLastError());
		goto out;
	}
	if (!DeviceIoControl(disk, IOCTL_DISK_UPDATE_PROPERTIES, NULL, 0, NULL, 0, &returned, NULL)) {
		windows_device_set_operation_error(
		    error, error_size, path, "cannot refresh the enlarged physical disk", GetLastError());
		goto out;
	}
	(void)DeviceIoControl(
	    device->handle, IOCTL_DISK_UPDATE_PROPERTIES, NULL, 0, NULL, 0, &returned, NULL);
	if (windows_partition_query_information(device->handle, path, &partition, error, error_size) !=
	        0 ||
	    windows_partition_query_extent(device->handle, path, &extent, error, error_size) != 0)
		goto out;
	if (!DeviceIoControl(device->handle, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &volume_length,
	        sizeof(volume_length), &returned, NULL)) {
		windows_device_set_operation_error(
		    error, error_size, path, "cannot refresh the enlarged volume size", GetLastError());
		goto out;
	}
	if (returned < sizeof(volume_length) || volume_length.Length.QuadPart <= 0) {
		windows_device_set_error(
		    error, error_size, path, "the enlarged volume returned an invalid size");
		goto out;
	}
	if (partition.StartingOffset.QuadPart < 0 || partition.PartitionLength.QuadPart <= 0 ||
	    partition.PartitionNumber != partition_number ||
	    (uint64_t)partition.StartingOffset.QuadPart != starting_offset ||
	    (uint64_t)partition.PartitionLength.QuadPart != requested_length ||
	    extent.DiskNumber != disk_number ||
	    extent.StartingOffset.QuadPart != partition.StartingOffset.QuadPart ||
	    extent.ExtentLength.QuadPart != partition.PartitionLength.QuadPart ||
	    volume_length.Length.QuadPart != partition.PartitionLength.QuadPart) {
		windows_device_set_error(error, error_size, path,
		    "Windows did not expose the requested enlarged partition geometry");
		goto out;
	}
	device->block_device.sector_count = requested_length / sector_size;
	result = DEVICE_PARTITION_GROWTH_SUCCESS;

out:
	windows_partition_close(&plan);
	return result;
}
