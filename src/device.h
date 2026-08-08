/* SPDX-License-Identifier: MIT */

#ifndef EXFAT_RESIZE_DEVICE_H
#define EXFAT_RESIZE_DEVICE_H

#include "exfat_resize.h"

#include <stddef.h>
#include <stdint.h>

struct device {
	int fd;
	int is_regular_file;
	const char *io_error_operation;
	int io_error_number;
	struct exfat_resize_block_device block_device;
};

int device_open(struct device *device, const char *path, char *error, size_t error_size);
void device_close(struct device *device);

#endif
