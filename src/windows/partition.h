/* SPDX-License-Identifier: MIT */

#ifndef EXFAT_RESIZE_WINDOWS_PARTITION_H
#define EXFAT_RESIZE_WINDOWS_PARTITION_H

#include "device.h"
#include <winioctl.h>

#define DRIVE_LAYOUT_BUFFER_SIZE ((DWORD)(1024 * 1024))

struct windows_partition {
	HANDLE disk;
	DRIVE_LAYOUT_INFORMATION_EX *layout;
	DWORD partition_index;
	DWORD disk_number;
	uint64_t maximum_length;
};

int windows_partition_query_information(HANDLE handle,
    const char *path,
    PARTITION_INFORMATION_EX *partition,
    char *error,
    size_t error_size);
int windows_partition_query_extent(
    HANDLE handle, const char *path, DISK_EXTENT *extent, char *error, size_t error_size);
int windows_partition_read_layout(HANDLE handle,
    const char *path,
    DRIVE_LAYOUT_INFORMATION_EX *layout,
    char *error,
    size_t error_size);
int windows_partition_same_entry(
    const PARTITION_INFORMATION_EX *a, const PARTITION_INFORMATION_EX *b);
int windows_partition_same_layout(
    const DRIVE_LAYOUT_INFORMATION_EX *a, const DRIVE_LAYOUT_INFORMATION_EX *b);
DRIVE_LAYOUT_INFORMATION_EX *windows_partition_query_layout(
    HANDLE handle, const char *path, char *error, size_t error_size);
int windows_partition_open(struct device *device,
    const char *path,
    struct windows_partition *plan,
    char *error,
    size_t error_size);
void windows_partition_close(struct windows_partition *plan);

#endif
