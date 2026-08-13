/* SPDX-License-Identifier: MIT */

#ifndef EXFAT_RESIZE_WINDOWS_DEVICE_INTERNAL_H
#define EXFAT_RESIZE_WINDOWS_DEVICE_INTERNAL_H

#include <stddef.h>
#include <windows.h>

void windows_device_set_error(char *error, size_t size, const char *path, const char *message);
void windows_device_set_operation_error(
    char *error, size_t size, const char *path, const char *operation, DWORD error_number);

#endif
