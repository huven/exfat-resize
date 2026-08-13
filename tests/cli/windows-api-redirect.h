/* SPDX-License-Identifier: MIT */

#ifndef EXFAT_RESIZE_TEST_WINDOWS_API_REDIRECT_H
#define EXFAT_RESIZE_TEST_WINDOWS_API_REDIRECT_H

#include <windows.h>

BOOL WINAPI windows_partition_test_device_io_control(HANDLE handle,
    DWORD control,
    LPVOID input,
    DWORD input_size,
    LPVOID output,
    DWORD output_size,
    LPDWORD returned,
    LPOVERLAPPED overlapped);
BOOL WINAPI windows_partition_test_flush_file_buffers(HANDLE handle);

#define DeviceIoControl windows_partition_test_device_io_control
#define FlushFileBuffers windows_partition_test_flush_file_buffers

#endif
