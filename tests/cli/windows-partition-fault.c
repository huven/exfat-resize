/* SPDX-License-Identifier: MIT */

#undef DeviceIoControl
#undef FlushFileBuffers
#undef CloseHandle

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <winioctl.h>

static int partition_grown;
static int partition_shrunk;
static unsigned int layout_reads;
static HANDLE old_volume;
static int properties_updated;
static PHANDLER_ROUTINE console_handler;

static int configured_point(const char *point)
{
	const char *configured = getenv("EXFAT_RESIZE_TEST_PARTITION_FAULT");
	size_t point_length = strlen(point);

	while (configured != NULL && *configured != '\0') {
		const char *separator = strchr(configured, ',');
		size_t length = separator == NULL ? strlen(configured) : (size_t)(separator - configured);

		if (length == point_length && memcmp(configured, point, length) == 0) {
			return 1;
		}
		configured = separator == NULL ? NULL : separator + 1;
	}
	return 0;
}

static int configured_fault(const char *point)
{
	if (!configured_point(point))
		return 0;
	SetLastError(ERROR_GEN_FAILURE);
	return 1;
}

static int request_cancellation(void)
{
	if (console_handler != NULL && console_handler(CTRL_C_EVENT))
		return 0;
	SetLastError(ERROR_GEN_FAILURE);
	return -1;
}

BOOL WINAPI windows_partition_test_set_console_ctrl_handler(PHANDLER_ROUTINE handler, BOOL add)
{
	if (handler == NULL || (add && console_handler != NULL) ||
	    (!add && handler != console_handler)) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	console_handler = add ? handler : NULL;
	return TRUE;
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
	if (control == IOCTL_DISK_GROW_PARTITION && configured_point("cancel-grow") &&
	    request_cancellation() != 0)
		return FALSE;
	if (partition_shrunk && handle == old_volume) {
		SetLastError(ERROR_NO_SUCH_DEVICE);
		return FALSE;
	}
	if (control == IOCTL_DISK_GET_DRIVE_LAYOUT_EX) {
		++layout_reads;
		if (layout_reads == 2 && configured_fault("shrink-revalidate"))
			return FALSE;
	}
	if (control == IOCTL_DISK_SET_DRIVE_LAYOUT_EX && configured_fault("shrink-before-set"))
		return FALSE;
	if (control == IOCTL_DISK_SET_DRIVE_LAYOUT_EX && configured_point("cancel-shrink") &&
	    request_cancellation() != 0)
		return FALSE;
	if (partition_shrunk && control == IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS &&
	    configured_fault("shrink-volume"))
		return FALSE;
	succeeded = DeviceIoControl(
	    handle, control, input, input_size, output, output_size, returned, overlapped);
	if (!succeeded)
		return FALSE;
	if (control == IOCTL_DISK_GET_DRIVE_LAYOUT_EX && layout_reads == 2 &&
	    configured_point("shrink-layout-changed")) {
		DRIVE_LAYOUT_INFORMATION_EX *layout = output;
		if (layout->PartitionCount != 0)
			layout->PartitionEntry[layout->PartitionCount - 1].PartitionLength.QuadPart -= 512;
	}
	if (control == IOCTL_DISK_GET_DRIVE_LAYOUT_EX && configured_point("cancel-discovery") &&
	    request_cancellation() != 0)
		return FALSE;

	if (control == FSCTL_LOCK_VOLUME)
		old_volume = handle;
	if (control == IOCTL_DISK_SET_DRIVE_LAYOUT_EX) {
		partition_shrunk = 1;
		if (configured_fault("shrink-result"))
			goto fail;
	}
	if (partition_shrunk && control == IOCTL_DISK_UPDATE_PROPERTIES &&
	    configured_fault("shrink-refresh"))
		goto fail;
	if (partition_shrunk && control == IOCTL_DISK_GET_DRIVE_LAYOUT_EX &&
	    configured_fault("shrink-readback"))
		goto fail;
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

	if (succeeded &&
	    ((partition_grown && configured_fault("flush")) ||
	        (partition_shrunk && configured_fault("shrink-flush")))) {
		SetLastError(ERROR_GEN_FAILURE);
		return FALSE;
	}
	return succeeded;
}

BOOL WINAPI windows_partition_test_close_handle(HANDLE handle)
{
	if (handle == old_volume)
		old_volume = NULL;
	return CloseHandle(handle);
}
