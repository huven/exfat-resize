/* SPDX-License-Identifier: MIT */

#include "device.h"

#include "block_device.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static int ascii_letter(char character)
{
	return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z');
}

static int ascii_equal(char left, char right)
{
	if (left >= 'a' && left <= 'z')
		left -= 'a' - 'A';
	if (right >= 'a' && right <= 'z')
		right -= 'a' - 'A';
	return left == right;
}

static int starts_with(const char *path, const char *prefix)
{
	while (*prefix != '\0') {
		if (!ascii_equal(*path++, *prefix++))
			return 0;
	}
	return 1;
}

static int path_separator(char character)
{
	return character == '\\' || character == '/';
}

static int path_uses_unsupported_namespace(const char *path)
{
	if (ascii_letter(path[0]) && path[1] == ':' && path[2] == '\0')
		return 1;
	if (!path_separator(path[0]) || !path_separator(path[1]) ||
	    (path[2] != '.' && path[2] != '?') || !path_separator(path[3]))
		return 0;
	if (path[2] == '.')
		return 1;

	/* Permit only long absolute file paths, not volume or NT device namespaces. */
	if (ascii_letter(path[4]) && path[5] == ':' && path_separator(path[6]))
		return 0;
	if (starts_with(path + 4, "UNC\\") || starts_with(path + 4, "UNC/"))
		return 0;
	return 1;
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

int device_open(struct device *device, const char *path, char *error, size_t error_size)
{
	BY_HANDLE_FILE_INFORMATION information;
	LARGE_INTEGER file_size;
	wchar_t *wide_path;
	HANDLE handle;
	DWORD error_number;
	DWORD file_type;
	const uint32_t sector_size = 512;

	if (path_uses_unsupported_namespace(path)) {
		set_error(error, error_size, path,
		    "Windows volume and device paths are not supported yet; use an image file");
		return -1;
	}
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

	device->handle = handle;
	device->io_error_operation = NULL;
	device->io_error_number = ERROR_SUCCESS;
	device->block_device.context = device;
	device->block_device.sector_size = sector_size;
	device->block_device.sector_count = (uint64_t)file_size.QuadPart / sector_size;
	device->block_device.read = block_device_read;
	device->block_device.write = block_device_write;
	device->block_device.sync = block_device_sync;
	return 0;

fail_after_open:
	(void)CloseHandle(handle);
	return -1;
}

void device_close(struct device *device)
{
	if (device->handle != INVALID_HANDLE_VALUE)
		(void)CloseHandle(device->handle);
	device->handle = INVALID_HANDLE_VALUE;
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
	const size_t maximum_chunk = (size_t)MAXDWORD - (MAXDWORD % sector_size);

	if ((uint64_t)count > sector_count || sector > sector_count - (uint64_t)count ||
	    count > SIZE_MAX / sector_size)
		return -1;
	byte_offset = sector * sector_size;
	total = (size_t)count * sector_size;
	while (done < total) {
		LARGE_INTEGER offset;
		DWORD transferred;
		size_t remaining = total - done;
		DWORD requested = (DWORD)(remaining > maximum_chunk ? maximum_chunk : remaining);
		BOOL result;

		offset.QuadPart = (LONGLONG)(byte_offset + done);
		if (!SetFilePointerEx(device->handle, offset, NULL, FILE_BEGIN)) {
			record_io_error(device, write_data ? "write" : "read", GetLastError());
			return -1;
		}
		if (write_data) {
			result = WriteFile(device->handle, (const unsigned char *)buffer + done, requested,
			    &transferred, NULL);
		} else {
			result = ReadFile(
			    device->handle, (unsigned char *)buffer + done, requested, &transferred, NULL);
		}
		if (!result || transferred == 0) {
			DWORD transfer_error = result ? ERROR_HANDLE_EOF : GetLastError();
			record_io_error(device, write_data ? "write" : "read", transfer_error);
			return -1;
		}
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
