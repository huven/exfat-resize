/* SPDX-License-Identifier: MIT */

#ifndef EXFAT_RESIZE_WINDOWS_DEVICE_PATH_H
#define EXFAT_RESIZE_WINDOWS_DEVICE_PATH_H

#include <stddef.h>

enum windows_device_path_type {
	WINDOWS_DEVICE_PATH_IMAGE,
	WINDOWS_DEVICE_PATH_VOLUME,
	WINDOWS_DEVICE_PATH_UNSUPPORTED
};

enum windows_device_path_type windows_classify_device_path(const char *path);
int windows_normalize_volume_path(const char *path, char *normalized, size_t normalized_size);

#endif
