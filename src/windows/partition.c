/* SPDX-License-Identifier: MIT */

#include "device.h"

#include "windows/device_internal.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <winioctl.h>

#define DRIVE_LAYOUT_BUFFER_SIZE ((DWORD)(1024 * 1024))

static const GUID basic_data_partition_guid = { 0xebd0a0a2, 0xb9e5, 0x4433,
	{ 0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7 } };

static int query_partition_information(HANDLE handle,
    const char *path,
    PARTITION_INFORMATION_EX *partition,
    char *error,
    size_t error_size)
{
	DWORD returned;
	BOOL succeeded;

	(void)memset(partition, 0, sizeof(*partition));
	succeeded = DeviceIoControl(handle, IOCTL_DISK_GET_PARTITION_INFO_EX, NULL, 0, partition,
	    sizeof(*partition), &returned, NULL);
	if (succeeded && returned >= sizeof(*partition))
		return 0;
	if (!succeeded)
		windows_device_set_operation_error(
		    error, error_size, path, "cannot identify the volume partition", GetLastError());
	else
		windows_device_set_error(
		    error, error_size, path, "the volume returned invalid partition information");
	return -1;
}

static int query_volume_extent(
    HANDLE handle, const char *path, DISK_EXTENT *extent, char *error, size_t error_size)
{
	VOLUME_DISK_EXTENTS extents;
	DWORD returned;
	DWORD error_number;

	(void)memset(&extents, 0, sizeof(extents));
	if (!DeviceIoControl(handle, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0, &extents,
	        sizeof(extents), &returned, NULL)) {
		error_number = GetLastError();
		if (error_number == ERROR_MORE_DATA) {
			windows_device_set_error(error, error_size, path,
			    "partition growth requires a volume with one physical disk extent");
		} else {
			windows_device_set_operation_error(error, error_size, path,
			    "cannot determine the volume's physical disk extent", error_number);
		}
		return -1;
	}
	if (returned < offsetof(VOLUME_DISK_EXTENTS, Extents) + sizeof(extents.Extents[0]) ||
	    extents.NumberOfDiskExtents != 1) {
		windows_device_set_error(error, error_size, path,
		    "partition growth requires a volume with one physical disk extent");
		return -1;
	}
	*extent = extents.Extents[0];
	return 0;
}

static DRIVE_LAYOUT_INFORMATION_EX *query_drive_layout(
    HANDLE handle, const char *path, char *error, size_t error_size)
{
	DRIVE_LAYOUT_INFORMATION_EX *layout;
	size_t maximum_partition_count;
	DWORD returned;

	layout = malloc(DRIVE_LAYOUT_BUFFER_SIZE);
	if (layout == NULL) {
		windows_device_set_error(
		    error, error_size, path, "cannot allocate memory for the disk layout");
		return NULL;
	}
	(void)memset(layout, 0, DRIVE_LAYOUT_BUFFER_SIZE);
	if (!DeviceIoControl(handle, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, NULL, 0, layout,
	        DRIVE_LAYOUT_BUFFER_SIZE, &returned, NULL)) {
		windows_device_set_operation_error(
		    error, error_size, path, "cannot read the physical disk layout", GetLastError());
		free(layout);
		return NULL;
	}
	if (returned < offsetof(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry)) {
		windows_device_set_error(
		    error, error_size, path, "the physical disk returned an invalid layout");
		free(layout);
		return NULL;
	}
	maximum_partition_count = (returned - offsetof(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry)) /
	    sizeof(layout->PartitionEntry[0]);
	if (layout->PartitionCount > maximum_partition_count) {
		windows_device_set_error(
		    error, error_size, path, "the physical disk returned an invalid layout");
		free(layout);
		return NULL;
	}
	return layout;
}

static int is_supported_basic_partition(const PARTITION_INFORMATION_EX *partition)
{
	if (partition->PartitionStyle == PARTITION_STYLE_GPT)
		return memcmp(&partition->Gpt.PartitionType, &basic_data_partition_guid,
		           sizeof(basic_data_partition_guid)) == 0;
	if (partition->PartitionStyle == PARTITION_STYLE_MBR)
		return partition->Mbr.PartitionType == PARTITION_IFS;
	return 0;
}

static int checked_end(uint64_t start, uint64_t length, uint64_t *end)
{
	if (start > UINT64_MAX - length)
		return -1;
	*end = start + length;
	return 0;
}

int device_grow_partition(struct device *device,
    const char *path,
    uint64_t target_size,
    int *partition_grown,
    char *error,
    size_t error_size)
{
	DRIVE_LAYOUT_INFORMATION_EX *layout = NULL;
	PARTITION_INFORMATION_EX partition;
	PARTITION_INFORMATION_EX *layout_partition = NULL;
	DISK_GROW_PARTITION request;
	GET_LENGTH_INFORMATION disk_length;
	GET_LENGTH_INFORMATION volume_length;
	DISK_EXTENT extent;
	wchar_t disk_path[64];
	HANDLE disk = INVALID_HANDLE_VALUE;
	uint64_t current_length;
	uint64_t current_end;
	uint64_t maximum_end;
	uint64_t requested_length;
	uint64_t growth;
	uint64_t starting_offset;
	uint32_t sector_size = device->block_device.sector_size;
	DWORD disk_number;
	DWORD partition_number;
	DWORD returned;
	DWORD error_number;
	size_t index;
	int path_length;
	int result = -1;

	*partition_grown = 0;
	if (device->volume_io_buffer == NULL) {
		windows_device_set_error(
		    error, error_size, path, "--grow-partition requires a logical Windows volume target");
		return -1;
	}
	if (device->block_device.sector_count > UINT64_MAX / sector_size) {
		windows_device_set_error(error, error_size, path, "the logical volume size is too large");
		return -1;
	}
	current_length = device->block_device.sector_count * (uint64_t)sector_size;
	if (target_size <= current_length)
		return 0;
	if (target_size > (uint64_t)LLONG_MAX ||
	    target_size > UINT64_MAX - ((uint64_t)sector_size - 1)) {
		windows_device_set_error(
		    error, error_size, path, "the requested partition size is too large");
		return -1;
	}
	requested_length = (target_size + sector_size - 1) / sector_size * (uint64_t)sector_size;
	if (requested_length > (uint64_t)LLONG_MAX) {
		windows_device_set_error(
		    error, error_size, path, "the requested partition size is too large");
		return -1;
	}

	if (query_partition_information(device->handle, path, &partition, error, error_size) != 0 ||
	    query_volume_extent(device->handle, path, &extent, error, error_size) != 0)
		return -1;
	if (partition.IsServicePartition || partition.PartitionNumber == 0 ||
	    partition.StartingOffset.QuadPart < 0 || partition.PartitionLength.QuadPart <= 0 ||
	    extent.StartingOffset.QuadPart < 0 || extent.ExtentLength.QuadPart <= 0 ||
	    (uint64_t)partition.PartitionLength.QuadPart != current_length ||
	    extent.StartingOffset.QuadPart != partition.StartingOffset.QuadPart ||
	    extent.ExtentLength.QuadPart != partition.PartitionLength.QuadPart) {
		windows_device_set_error(error, error_size, path,
		    "the volume does not map to one complete basic-disk partition");
		return -1;
	}
	disk_number = extent.DiskNumber;
	partition_number = partition.PartitionNumber;
	starting_offset = (uint64_t)partition.StartingOffset.QuadPart;

	path_length = swprintf(disk_path, sizeof(disk_path) / sizeof(disk_path[0]),
	    L"\\\\.\\PhysicalDrive%lu", (unsigned long)disk_number);
	if (path_length < 0 || (size_t)path_length >= sizeof(disk_path) / sizeof(disk_path[0])) {
		windows_device_set_error(
		    error, error_size, path, "cannot construct the physical disk path");
		return -1;
	}
	disk = CreateFileW(disk_path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
	    NULL, OPEN_EXISTING, FILE_FLAG_WRITE_THROUGH, NULL);
	if (disk == INVALID_HANDLE_VALUE) {
		error_number = GetLastError();
		if (error_number == ERROR_ACCESS_DENIED) {
			windows_device_set_operation_error(error, error_size, path,
			    "cannot open the physical disk; run as Administrator", error_number);
		} else {
			windows_device_set_operation_error(
			    error, error_size, path, "cannot open the physical disk", error_number);
		}
		return -1;
	}

	layout = query_drive_layout(disk, path, error, error_size);
	if (layout == NULL)
		goto out;
	if (!DeviceIoControl(disk, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &disk_length,
	        sizeof(disk_length), &returned, NULL)) {
		windows_device_set_operation_error(
		    error, error_size, path, "cannot determine the physical disk size", GetLastError());
		goto out;
	}
	if (returned < sizeof(disk_length) || disk_length.Length.QuadPart <= 0) {
		windows_device_set_error(
		    error, error_size, path, "the physical disk returned an invalid size");
		goto out;
	}
	if (layout->PartitionStyle != partition.PartitionStyle) {
		windows_device_set_error(
		    error, error_size, path, "the volume and physical disk partition styles differ");
		goto out;
	}
	for (index = 0; index < layout->PartitionCount; ++index) {
		PARTITION_INFORMATION_EX *candidate = &layout->PartitionEntry[index];

		if (candidate->PartitionNumber != partition.PartitionNumber)
			continue;
		if (layout_partition != NULL || candidate->StartingOffset.QuadPart < 0 ||
		    candidate->PartitionLength.QuadPart <= 0 ||
		    candidate->StartingOffset.QuadPart != partition.StartingOffset.QuadPart ||
		    candidate->PartitionLength.QuadPart != partition.PartitionLength.QuadPart) {
			windows_device_set_error(error, error_size, path,
			    "the volume partition does not match the physical disk layout");
			goto out;
		}
		layout_partition = candidate;
	}
	if (layout_partition == NULL || !is_supported_basic_partition(layout_partition)) {
		windows_device_set_error(error, error_size, path,
		    "partition growth supports only basic GPT or MBR data partitions");
		goto out;
	}
	if (checked_end((uint64_t)partition.StartingOffset.QuadPart, current_length, &current_end) !=
	    0) {
		windows_device_set_error(error, error_size, path, "the partition geometry is too large");
		goto out;
	}
	maximum_end = (uint64_t)disk_length.Length.QuadPart;
	if (layout->PartitionStyle == PARTITION_STYLE_GPT) {
		uint64_t usable_start;

		if (layout->Gpt.StartingUsableOffset.QuadPart < 0 ||
		    layout->Gpt.UsableLength.QuadPart <= 0 ||
		    checked_end((uint64_t)layout->Gpt.StartingUsableOffset.QuadPart,
		        (uint64_t)layout->Gpt.UsableLength.QuadPart, &maximum_end) != 0) {
			windows_device_set_error(error, error_size, path, "the GPT usable range is invalid");
			goto out;
		}
		usable_start = (uint64_t)layout->Gpt.StartingUsableOffset.QuadPart;
		if (starting_offset < usable_start || current_end > maximum_end ||
		    maximum_end > (uint64_t)disk_length.Length.QuadPart) {
			windows_device_set_error(
			    error, error_size, path, "the partition is outside the GPT usable range");
			goto out;
		}
	}
	for (index = 0; index < layout->PartitionCount; ++index) {
		PARTITION_INFORMATION_EX *candidate = &layout->PartitionEntry[index];
		uint64_t candidate_end;
		uint64_t candidate_start;

		if (candidate == layout_partition || candidate->StartingOffset.QuadPart < 0 ||
		    candidate->PartitionLength.QuadPart <= 0)
			continue;
		candidate_start = (uint64_t)candidate->StartingOffset.QuadPart;
		if (checked_end(candidate_start, (uint64_t)candidate->PartitionLength.QuadPart,
		        &candidate_end) != 0) {
			windows_device_set_error(
			    error, error_size, path, "the physical disk layout is too large");
			goto out;
		}
		if (candidate_end > (uint64_t)disk_length.Length.QuadPart) {
			windows_device_set_error(
			    error, error_size, path, "a partition extends beyond the physical disk");
			goto out;
		}
		if (candidate_start < current_end && candidate_end > starting_offset) {
			windows_device_set_error(error, error_size, path,
			    "partition growth does not support overlapping physical-disk layout entries");
			goto out;
		}
		if (candidate_start >= current_end && candidate_start < maximum_end)
			maximum_end = candidate_start;
	}
	if (maximum_end < current_end ||
	    requested_length - current_length > maximum_end - current_end) {
		windows_device_set_error(error, error_size, path,
		    "not enough immediately trailing unallocated space for the requested size");
		goto out;
	}

	growth = requested_length - current_length;
	(void)memset(&request, 0, sizeof(request));
	request.PartitionNumber = partition_number;
	request.BytesToGrow.QuadPart = (LONGLONG)growth;
	if (!DeviceIoControl(
	        disk, IOCTL_DISK_GROW_PARTITION, &request, sizeof(request), NULL, 0, &returned, NULL)) {
		windows_device_set_operation_error(
		    error, error_size, path, "cannot grow the partition", GetLastError());
		goto out;
	}
	*partition_grown = 1;
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
	if (query_partition_information(device->handle, path, &partition, error, error_size) != 0 ||
	    query_volume_extent(device->handle, path, &extent, error, error_size) != 0)
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
	result = 0;

out:
	free(layout);
	(void)CloseHandle(disk);
	return result;
}
