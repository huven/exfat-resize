/* SPDX-License-Identifier: MIT */

#undef DeviceIoControl
#undef FlushFileBuffers

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <winioctl.h>

static int partition_grown;
static int properties_updated;

static int configured_fault(const char *point)
{
	const char *configured = getenv("EXFAT_RESIZE_TEST_PARTITION_FAULT");
	size_t point_length = strlen(point);

	while (configured != NULL && *configured != '\0') {
		const char *separator = strchr(configured, ',');
		size_t length = separator == NULL ? strlen(configured) : (size_t)(separator - configured);

		if (length == point_length && memcmp(configured, point, length) == 0) {
			SetLastError(ERROR_GEN_FAILURE);
			return 1;
		}
		configured = separator == NULL ? NULL : separator + 1;
	}
	return 0;
}

BOOL WINAPI windows_partition_test_device_io_control(HANDLE handle,
    DWORD control,
    LPVOID input,
    DWORD input_size,
    LPVOID output,
    DWORD output_size,
    LPDWORD returned,
    LPOVERLAPPED overlapped)
{
	BOOL succeeded;

	if (control == FSCTL_DISMOUNT_VOLUME && configured_fault("dismount")) {
		SetLastError(ERROR_GEN_FAILURE);
		return FALSE;
	}
	succeeded = DeviceIoControl(
	    handle, control, input, input_size, output, output_size, returned, overlapped);
	if (!succeeded)
		return FALSE;

	if (control == IOCTL_DISK_GROW_PARTITION) {
		partition_grown = 1;
		if (configured_fault("grow-result"))
			goto fail;
	} else if (control == IOCTL_DISK_UPDATE_PROPERTIES && partition_grown) {
		properties_updated = 1;
		if (configured_fault("refresh"))
			goto fail;
	} else if (control == IOCTL_DISK_GET_PARTITION_INFO_EX && properties_updated &&
	    configured_fault("readback")) {
		goto fail;
	} else if (control == FSCTL_DISMOUNT_VOLUME) {
		fprintf(stderr, "exfat-resize-test: dismounted the volume\n");
	}
	return TRUE;

fail:
	SetLastError(ERROR_GEN_FAILURE);
	return FALSE;
}

BOOL WINAPI windows_partition_test_flush_file_buffers(HANDLE handle)
{
	BOOL succeeded = FlushFileBuffers(handle);

	if (succeeded && partition_grown && configured_fault("flush")) {
		SetLastError(ERROR_GEN_FAILURE);
		return FALSE;
	}
	return succeeded;
}
