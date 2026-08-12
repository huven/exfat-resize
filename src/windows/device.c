/* SPDX-License-Identifier: MIT */

#include "device.h"

#include "block_device.h"
#include "windows/device_path.h"

#include <limits.h>
#include <malloc.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <winioctl.h>

#define DRIVE_LAYOUT_BUFFER_SIZE ((DWORD)(1024 * 1024))
#define VOLUME_IO_BUFFER_SIZE ((size_t)1024 * 1024)

static const GUID basic_data_partition_guid = { 0xebd0a0a2, 0xb9e5, 0x4433,
	{ 0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7 } };

static int block_device_read(
    void *context, uint64_t first_sector, uint32_t sector_count, void *buffer);
static int block_device_write(
    void *context, uint64_t first_sector, uint32_t sector_count, const void *buffer);
static int block_device_sync(void *context);

static void windows_error_message(DWORD error_number, char *message, size_t size)
{
	wchar_t wide_message[256];
	char utf8_message[1024];
	DWORD length;
	int converted;

	if (size == 0)
		return;
	length = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
	    error_number, 0, wide_message, (DWORD)(sizeof(wide_message) / sizeof(wide_message[0])),
	    NULL);
	if (length == 0) {
		(void)snprintf(message, size, "Windows error %lu", (unsigned long)error_number);
		return;
	}
	while (length > 0 &&
	    (wide_message[length - 1] == L'\r' || wide_message[length - 1] == L'\n' ||
	        wide_message[length - 1] == L' ')) {
		wide_message[--length] = L'\0';
	}
	converted = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_message, -1, utf8_message,
	    (int)sizeof(utf8_message), NULL, NULL);
	if (converted == 0) {
		(void)snprintf(message, size, "Windows error %lu", (unsigned long)error_number);
		return;
	}
	(void)snprintf(message, size, "%s", utf8_message);
}

static void set_error(char *error, size_t size, const char *path, const char *message)
{
	(void)snprintf(error, size, "%s: %s", path, message);
}

static void set_windows_error(char *error, size_t size, const char *path, DWORD error_number)
{
	char message[1024];

	windows_error_message(error_number, message, sizeof(message));
	set_error(error, size, path, message);
}

static void set_windows_operation_error(
    char *error, size_t size, const char *path, const char *operation, DWORD error_number)
{
	char message[1024];
	char detail[768];

	windows_error_message(error_number, detail, sizeof(detail));
	(void)snprintf(message, sizeof(message), "%s: %s", operation, detail);
	set_error(error, size, path, message);
}

static wchar_t *utf8_path(const char *path, char *error, size_t error_size)
{
	wchar_t *converted;
	int size;

	size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
	if (size == 0) {
		set_error(error, error_size, path, "path is not valid UTF-8");
		return NULL;
	}
	if ((size_t)size > SIZE_MAX / sizeof(*converted)) {
		set_error(error, error_size, path, "path is too large");
		return NULL;
	}
	converted = malloc((size_t)size * sizeof(*converted));
	if (converted == NULL) {
		set_error(error, error_size, path, "cannot allocate memory for the path");
		return NULL;
	}
	if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, converted, size) == 0) {
		free(converted);
		set_error(error, error_size, path, "cannot convert the path to UTF-16");
		return NULL;
	}
	return converted;
}

static void record_io_error(struct device *device, const char *operation, DWORD error_number)
{
	if (device->io_error_operation != NULL)
		return;
	device->io_error_operation = operation;
	device->io_error_number = error_number;
}

void device_init(struct device *device)
{
	(void)memset(device, 0, sizeof(*device));
	device->handle = INVALID_HANDLE_VALUE;
}

static void configure_device(struct device *device,
    HANDLE handle,
    uint32_t sector_size,
    uint64_t sector_count,
    void *volume_io_buffer,
    size_t volume_io_buffer_size)
{
	device->handle = handle;
	device->io_error_operation = NULL;
	device->io_error_number = ERROR_SUCCESS;
	device->volume_io_buffer = volume_io_buffer;
	device->volume_io_buffer_size = volume_io_buffer_size;
	device->block_device.context = device;
	device->block_device.sector_size = sector_size;
	device->block_device.sector_count = sector_count;
	device->block_device.read = block_device_read;
	device->block_device.write = block_device_write;
	device->block_device.sync = block_device_sync;
}

static int open_image(struct device *device, const char *path, char *error, size_t error_size)
{
	BY_HANDLE_FILE_INFORMATION information;
	LARGE_INTEGER file_size;
	wchar_t *wide_path;
	HANDLE handle;
	DWORD error_number;
	DWORD file_type;
	const uint32_t sector_size = 512;

	wide_path = utf8_path(path, error, error_size);
	if (wide_path == NULL)
		return -1;
	handle = CreateFileW(wide_path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
	    FILE_ATTRIBUTE_NORMAL, NULL);
	free(wide_path);
	if (handle == INVALID_HANDLE_VALUE) {
		error_number = GetLastError();
		if (error_number == ERROR_SHARING_VIOLATION || error_number == ERROR_LOCK_VIOLATION)
			set_error(error, error_size, path, "already in use");
		else
			set_windows_error(error, error_size, path, error_number);
		return -1;
	}
	file_type = GetFileType(handle);
	if (file_type != FILE_TYPE_DISK) {
		set_error(error, error_size, path, "not a regular image file");
		goto fail_after_open;
	}
	if (!GetFileInformationByHandle(handle, &information)) {
		error_number = GetLastError();
		set_windows_error(error, error_size, path, error_number);
		goto fail_after_open;
	}
	if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
		set_error(error, error_size, path, "not a regular image file");
		goto fail_after_open;
	}
	if (!GetFileSizeEx(handle, &file_size)) {
		set_windows_error(error, error_size, path, GetLastError());
		goto fail_after_open;
	}
	if (file_size.QuadPart < sector_size) {
		set_error(error, error_size, path, "image file is too small");
		goto fail_after_open;
	}
	if (!exfat_resize_sector_size_is_supported(sector_size)) {
		set_error(error, error_size, path, "unsupported virtual sector size");
		goto fail_after_open;
	}

	configure_device(
	    device, handle, sector_size, (uint64_t)file_size.QuadPart / sector_size, NULL, 0);
	return 0;

fail_after_open:
	(void)CloseHandle(handle);
	return -1;
}

static int power_of_two(DWORD value)
{
	return value != 0 && (value & (value - 1)) == 0;
}

static int volume_sector_sizes(HANDLE handle,
    const char *path,
    uint32_t *logical_sector_size,
    size_t *buffer_alignment,
    char *error,
    size_t error_size)
{
	STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR alignment;
	STORAGE_PROPERTY_QUERY query;
	DISK_GEOMETRY_EX geometry;
	DWORD returned;
	DWORD logical, physical;

	(void)memset(&query, 0, sizeof(query));
	query.PropertyId = StorageAccessAlignmentProperty;
	query.QueryType = PropertyStandardQuery;
	(void)memset(&alignment, 0, sizeof(alignment));
	if (DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), &alignment,
	        sizeof(alignment), &returned, NULL) &&
	    returned >= sizeof(alignment) && alignment.Size >= sizeof(alignment) &&
	    alignment.BytesPerLogicalSector != 0) {
		logical = alignment.BytesPerLogicalSector;
		physical = alignment.BytesPerPhysicalSector;
	} else {
		(void)memset(&geometry, 0, sizeof(geometry));
		if (!DeviceIoControl(handle, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0, &geometry,
		        sizeof(geometry), &returned, NULL)) {
			set_windows_operation_error(
			    error, error_size, path, "cannot determine the volume sector size", GetLastError());
			return -1;
		}
		logical = geometry.Geometry.BytesPerSector;
		physical = logical;
	}
	if (!exfat_resize_sector_size_is_supported(logical)) {
		char message[128];

		(void)snprintf(message, sizeof(message), "unsupported logical sector size %lu",
		    (unsigned long)logical);
		set_error(error, error_size, path, message);
		return -1;
	}
	if (physical == 0)
		physical = logical;
	if (physical < logical || !power_of_two(physical)) {
		set_error(error, error_size, path, "unsupported physical-sector alignment");
		return -1;
	}
	*logical_sector_size = logical;
	*buffer_alignment = physical;
	return 0;
}

static int volume_control(HANDLE handle,
    DWORD control,
    const char *path,
    const char *operation,
    char *error,
    size_t error_size)
{
	DWORD returned;

	if (DeviceIoControl(handle, control, NULL, 0, NULL, 0, &returned, NULL))
		return 0;
	set_windows_operation_error(error, error_size, path, operation, GetLastError());
	return -1;
}

static int open_volume(struct device *device, const char *path, char *error, size_t error_size)
{
	GET_LENGTH_INFORMATION length;
	char normalized[64];
	wchar_t *wide_path;
	void *io_buffer = NULL;
	HANDLE handle;
	size_t buffer_alignment;
	uint32_t sector_size;
	DWORD error_number;
	DWORD file_type;
	DWORD returned;

	if (windows_normalize_volume_path(path, normalized, sizeof(normalized)) != 0) {
		set_error(error, error_size, path, "invalid logical-volume path");
		return -1;
	}
	wide_path = utf8_path(normalized, error, error_size);
	if (wide_path == NULL)
		return -1;
	handle =
	    CreateFileW(wide_path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
	        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING, NULL);
	free(wide_path);
	if (handle == INVALID_HANDLE_VALUE) {
		error_number = GetLastError();
		if (error_number == ERROR_SHARING_VIOLATION || error_number == ERROR_LOCK_VIOLATION)
			set_error(error, error_size, path, "already in use");
		else if (error_number == ERROR_ACCESS_DENIED)
			set_windows_operation_error(error, error_size, path,
			    "cannot open the volume; run as Administrator", error_number);
		else
			set_windows_error(error, error_size, path, error_number);
		return -1;
	}
	file_type = GetFileType(handle);
	if (file_type != FILE_TYPE_DISK) {
		set_error(error, error_size, path, "not a logical Windows volume");
		goto fail_after_open;
	}
	if (volume_control(handle, FSCTL_LOCK_VOLUME, path,
	        "cannot lock the volume; close files and applications using it and run as "
	        "Administrator",
	        error, error_size) != 0)
		goto fail_after_open;
	if (volume_sector_sizes(handle, path, &sector_size, &buffer_alignment, error, error_size) != 0)
		goto fail_after_open;
	(void)memset(&length, 0, sizeof(length));
	if (!DeviceIoControl(handle, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &length, sizeof(length),
	        &returned, NULL)) {
		set_windows_operation_error(
		    error, error_size, path, "cannot determine the volume size", GetLastError());
		goto fail_after_open;
	}
	if (length.Length.QuadPart < sector_size) {
		set_error(error, error_size, path, "logical volume is too small");
		goto fail_after_open;
	}
	io_buffer = _aligned_malloc(VOLUME_IO_BUFFER_SIZE, buffer_alignment);
	if (io_buffer == NULL) {
		set_error(error, error_size, path, "cannot allocate aligned volume I/O memory");
		goto fail_after_open;
	}
	if (volume_control(handle, FSCTL_ALLOW_EXTENDED_DASD_IO, path,
	        "cannot enable access to the complete volume", error, error_size) != 0)
		goto fail_after_allocation;

	configure_device(device, handle, sector_size, (uint64_t)length.Length.QuadPart / sector_size,
	    io_buffer, VOLUME_IO_BUFFER_SIZE);
	return 0;

fail_after_allocation:
	_aligned_free(io_buffer);
fail_after_open:
	(void)CloseHandle(handle);
	return -1;
}

int device_open(struct device *device, const char *path, char *error, size_t error_size)
{
	switch (windows_classify_device_path(path)) {
	case WINDOWS_DEVICE_PATH_IMAGE:
		return open_image(device, path, error, error_size);
	case WINDOWS_DEVICE_PATH_VOLUME:
		return open_volume(device, path, error, error_size);
	case WINDOWS_DEVICE_PATH_UNSUPPORTED:
		set_error(error, error_size, path,
		    "unsupported Windows device path; use an image file, drive letter, or volume-GUID "
		    "path");
		return -1;
	}
	set_error(error, error_size, path, "unsupported Windows path");
	return -1;
}

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
		set_windows_operation_error(
		    error, error_size, path, "cannot identify the volume partition", GetLastError());
	else
		set_error(error, error_size, path, "the volume returned invalid partition information");
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
			set_error(error, error_size, path,
			    "partition growth requires a volume with one physical disk extent");
		} else {
			set_windows_operation_error(error, error_size, path,
			    "cannot determine the volume's physical disk extent", error_number);
		}
		return -1;
	}
	if (returned < offsetof(VOLUME_DISK_EXTENTS, Extents) + sizeof(extents.Extents[0]) ||
	    extents.NumberOfDiskExtents != 1) {
		set_error(error, error_size, path,
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
		set_error(error, error_size, path, "cannot allocate memory for the disk layout");
		return NULL;
	}
	(void)memset(layout, 0, DRIVE_LAYOUT_BUFFER_SIZE);
	if (!DeviceIoControl(handle, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, NULL, 0, layout,
	        DRIVE_LAYOUT_BUFFER_SIZE, &returned, NULL)) {
		set_windows_operation_error(
		    error, error_size, path, "cannot read the physical disk layout", GetLastError());
		free(layout);
		return NULL;
	}
	if (returned < offsetof(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry)) {
		set_error(error, error_size, path, "the physical disk returned an invalid layout");
		free(layout);
		return NULL;
	}
	maximum_partition_count = (returned - offsetof(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry)) /
	    sizeof(layout->PartitionEntry[0]);
	if (layout->PartitionCount > maximum_partition_count) {
		set_error(error, error_size, path, "the physical disk returned an invalid layout");
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
		set_error(
		    error, error_size, path, "--grow-partition requires a logical Windows volume target");
		return -1;
	}
	if (device->block_device.sector_count > UINT64_MAX / sector_size) {
		set_error(error, error_size, path, "the logical volume size is too large");
		return -1;
	}
	current_length = device->block_device.sector_count * (uint64_t)sector_size;
	if (target_size <= current_length)
		return 0;
	if (target_size > (uint64_t)LLONG_MAX ||
	    target_size > UINT64_MAX - ((uint64_t)sector_size - 1)) {
		set_error(error, error_size, path, "the requested partition size is too large");
		return -1;
	}
	requested_length = (target_size + sector_size - 1) / sector_size * (uint64_t)sector_size;
	if (requested_length > (uint64_t)LLONG_MAX) {
		set_error(error, error_size, path, "the requested partition size is too large");
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
		set_error(error, error_size, path,
		    "the volume does not map to one complete basic-disk partition");
		return -1;
	}
	disk_number = extent.DiskNumber;
	partition_number = partition.PartitionNumber;
	starting_offset = (uint64_t)partition.StartingOffset.QuadPart;

	path_length = swprintf(disk_path, sizeof(disk_path) / sizeof(disk_path[0]),
	    L"\\\\.\\PhysicalDrive%lu", (unsigned long)disk_number);
	if (path_length < 0 || (size_t)path_length >= sizeof(disk_path) / sizeof(disk_path[0])) {
		set_error(error, error_size, path, "cannot construct the physical disk path");
		return -1;
	}
	disk = CreateFileW(disk_path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
	    NULL, OPEN_EXISTING, FILE_FLAG_WRITE_THROUGH, NULL);
	if (disk == INVALID_HANDLE_VALUE) {
		error_number = GetLastError();
		if (error_number == ERROR_ACCESS_DENIED) {
			set_windows_operation_error(error, error_size, path,
			    "cannot open the physical disk; run as Administrator", error_number);
		} else {
			set_windows_operation_error(
			    error, error_size, path, "cannot open the physical disk", error_number);
		}
		return -1;
	}

	layout = query_drive_layout(disk, path, error, error_size);
	if (layout == NULL)
		goto out;
	if (!DeviceIoControl(disk, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &disk_length,
	        sizeof(disk_length), &returned, NULL)) {
		set_windows_operation_error(
		    error, error_size, path, "cannot determine the physical disk size", GetLastError());
		goto out;
	}
	if (returned < sizeof(disk_length) || disk_length.Length.QuadPart <= 0) {
		set_error(error, error_size, path, "the physical disk returned an invalid size");
		goto out;
	}
	if (layout->PartitionStyle != partition.PartitionStyle) {
		set_error(error, error_size, path, "the volume and physical disk partition styles differ");
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
			set_error(error, error_size, path,
			    "the volume partition does not match the physical disk layout");
			goto out;
		}
		layout_partition = candidate;
	}
	if (layout_partition == NULL || !is_supported_basic_partition(layout_partition)) {
		set_error(error, error_size, path,
		    "partition growth supports only basic GPT or MBR data partitions");
		goto out;
	}
	if (checked_end((uint64_t)partition.StartingOffset.QuadPart, current_length, &current_end) !=
	    0) {
		set_error(error, error_size, path, "the partition geometry is too large");
		goto out;
	}
	maximum_end = (uint64_t)disk_length.Length.QuadPart;
	if (layout->PartitionStyle == PARTITION_STYLE_GPT) {
		uint64_t usable_start;

		if (layout->Gpt.StartingUsableOffset.QuadPart < 0 ||
		    layout->Gpt.UsableLength.QuadPart <= 0 ||
		    checked_end((uint64_t)layout->Gpt.StartingUsableOffset.QuadPart,
		        (uint64_t)layout->Gpt.UsableLength.QuadPart, &maximum_end) != 0) {
			set_error(error, error_size, path, "the GPT usable range is invalid");
			goto out;
		}
		usable_start = (uint64_t)layout->Gpt.StartingUsableOffset.QuadPart;
		if (starting_offset < usable_start || current_end > maximum_end ||
		    maximum_end > (uint64_t)disk_length.Length.QuadPart) {
			set_error(error, error_size, path, "the partition is outside the GPT usable range");
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
			set_error(error, error_size, path, "the physical disk layout is too large");
			goto out;
		}
		if (candidate_end > (uint64_t)disk_length.Length.QuadPart) {
			set_error(error, error_size, path, "a partition extends beyond the physical disk");
			goto out;
		}
		if (candidate_start < current_end && candidate_end > starting_offset) {
			set_error(error, error_size, path,
			    "partition growth does not support overlapping physical-disk layout entries");
			goto out;
		}
		if (candidate_start >= current_end && candidate_start < maximum_end)
			maximum_end = candidate_start;
	}
	if (maximum_end < current_end ||
	    requested_length - current_length > maximum_end - current_end) {
		set_error(error, error_size, path,
		    "not enough immediately trailing unallocated space for the requested size");
		goto out;
	}

	growth = requested_length - current_length;
	(void)memset(&request, 0, sizeof(request));
	request.PartitionNumber = partition_number;
	request.BytesToGrow.QuadPart = (LONGLONG)growth;
	if (!DeviceIoControl(
	        disk, IOCTL_DISK_GROW_PARTITION, &request, sizeof(request), NULL, 0, &returned, NULL)) {
		set_windows_operation_error(
		    error, error_size, path, "cannot grow the partition", GetLastError());
		goto out;
	}
	*partition_grown = 1;
	if (!FlushFileBuffers(disk)) {
		set_windows_operation_error(error, error_size, path,
		    "cannot synchronize the enlarged partition table", GetLastError());
		goto out;
	}
	if (!DeviceIoControl(disk, IOCTL_DISK_UPDATE_PROPERTIES, NULL, 0, NULL, 0, &returned, NULL)) {
		set_windows_operation_error(
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
		set_windows_operation_error(
		    error, error_size, path, "cannot refresh the enlarged volume size", GetLastError());
		goto out;
	}
	if (returned < sizeof(volume_length) || volume_length.Length.QuadPart <= 0) {
		set_error(error, error_size, path, "the enlarged volume returned an invalid size");
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
		set_error(error, error_size, path,
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

int device_dismount(struct device *device, const char *path, char *error, size_t error_size)
{
	if (device->volume_io_buffer == NULL)
		return 0;
	return volume_control(device->handle, FSCTL_DISMOUNT_VOLUME, path,
	    "cannot dismount the volume after resizing", error, error_size);
}

void device_close(struct device *device)
{
	if (device->handle != INVALID_HANDLE_VALUE)
		(void)CloseHandle(device->handle);
	device->handle = INVALID_HANDLE_VALUE;
	if (device->volume_io_buffer != NULL)
		_aligned_free(device->volume_io_buffer);
	device->volume_io_buffer = NULL;
	device->volume_io_buffer_size = 0;
}

void device_format_io_error(const struct device *device, char *error, size_t error_size)
{
	windows_error_message(device->io_error_number, error, error_size);
}

static int transfer(
    struct device *device, void *buffer, uint64_t sector, uint32_t count, int write_data)
{
	uint64_t byte_offset;
	size_t total, done = 0;
	uint32_t sector_size = device->block_device.sector_size;
	uint64_t sector_count = device->block_device.sector_count;
	size_t maximum_chunk = (size_t)MAXDWORD - (MAXDWORD % sector_size);

	if ((uint64_t)count > sector_count || sector > sector_count - (uint64_t)count ||
	    count > SIZE_MAX / sector_size)
		return -1;
	if (device->volume_io_buffer != NULL && device->volume_io_buffer_size < maximum_chunk)
		maximum_chunk = device->volume_io_buffer_size;
	byte_offset = sector * sector_size;
	total = (size_t)count * sector_size;
	while (done < total) {
		LARGE_INTEGER offset;
		DWORD transferred;
		size_t remaining = total - done;
		DWORD requested = (DWORD)(remaining > maximum_chunk ? maximum_chunk : remaining);
		void *io_buffer = device->volume_io_buffer != NULL ? device->volume_io_buffer
		                                                   : (unsigned char *)buffer + done;
		BOOL result;

		offset.QuadPart = (LONGLONG)(byte_offset + done);
		if (!SetFilePointerEx(device->handle, offset, NULL, FILE_BEGIN)) {
			record_io_error(device, write_data ? "write" : "read", GetLastError());
			return -1;
		}
		if (write_data) {
			if (device->volume_io_buffer != NULL)
				(void)memcpy(io_buffer, (const unsigned char *)buffer + done, requested);
			result = WriteFile(device->handle, io_buffer, requested, &transferred, NULL);
		} else {
			result = ReadFile(device->handle, io_buffer, requested, &transferred, NULL);
		}
		if (!result || transferred != requested) {
			DWORD transfer_error =
			    result ? (write_data ? ERROR_WRITE_FAULT : ERROR_HANDLE_EOF) : GetLastError();
			record_io_error(device, write_data ? "write" : "read", transfer_error);
			return -1;
		}
		if (!write_data && device->volume_io_buffer != NULL)
			(void)memcpy((unsigned char *)buffer + done, io_buffer, requested);
		done += transferred;
	}
	return 0;
}

static int block_device_read(
    void *context, uint64_t first_sector, uint32_t sector_count, void *buffer)
{
	return transfer(context, buffer, first_sector, sector_count, 0);
}

static int block_device_write(
    void *context, uint64_t first_sector, uint32_t sector_count, const void *buffer)
{
	return transfer(context, (void *)buffer, first_sector, sector_count, 1);
}

static int block_device_sync(void *context)
{
	struct device *device = context;

	if (FlushFileBuffers(device->handle))
		return 0;
	record_io_error(device, "synchronize", GetLastError());
	return -1;
}
