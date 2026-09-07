/* SPDX-License-Identifier: MIT */

#ifndef EXFAT_RESIZE_DEVICE_H
#define EXFAT_RESIZE_DEVICE_H

#include "exfat_resize.h"

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>
#endif

struct cli_cancellation;

struct device {
#if defined(_WIN32)
	HANDLE handle;
	DWORD io_error_number;
	void *volume_io_buffer;
	size_t volume_io_buffer_size;
#else
	int fd;
	int is_regular_file;
	int io_error_number;
#endif
	const char *io_error_operation;
	struct exfat_resize_block_device block_device;
};

enum device_partition_state {
	DEVICE_PARTITION_UNCHANGED,
	DEVICE_PARTITION_UPDATE_ATTEMPTED,
	DEVICE_PARTITION_GROWN
};

enum device_partition_growth_result {
	DEVICE_PARTITION_GROWTH_SUCCESS,
	DEVICE_PARTITION_GROWTH_ERROR,
	DEVICE_PARTITION_GROWTH_CANCELLED
};

void device_init(struct device *device);
int device_open(struct device *device, const char *path, char *error, size_t error_size);
enum device_partition_growth_result device_grow_partition(struct device *device,
    const char *path,
    uint64_t target_size,
    const struct cli_cancellation *cancellation,
    enum device_partition_state *partition_state,
    char *error,
    size_t error_size);
int device_dismount(struct device *device, const char *path, char *error, size_t error_size);
void device_format_io_error(const struct device *device, char *error, size_t error_size);
void device_close(struct device *device);

#endif
