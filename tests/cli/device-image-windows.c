/* SPDX-License-Identifier: MIT */

#include "device.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static int create_image(wchar_t *path, size_t path_size)
{
	wchar_t directory[MAX_PATH];
	wchar_t original_path[MAX_PATH];
	LARGE_INTEGER size;
	HANDLE file;
	DWORD directory_length;

	directory_length = GetTempPathW(MAX_PATH, directory);
	if (path_size < MAX_PATH || directory_length == 0 || directory_length >= MAX_PATH ||
	    GetTempFileNameW(directory, L"exr", 0, path) == 0)
		return -1;
	(void)wcscpy(original_path, path);
	path[directory_length] = L'\u03a9';
	if (!MoveFileW(original_path, path)) {
		(void)DeleteFileW(original_path);
		return -1;
	}
	file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, TRUNCATE_EXISTING,
	    FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return -1;
	size.QuadPart = 4096;
	if (!SetFilePointerEx(file, size, NULL, FILE_BEGIN) || !SetEndOfFile(file)) {
		(void)CloseHandle(file);
		return -1;
	}
	if (!CloseHandle(file))
		return -1;
	return 0;
}

static int wide_to_utf8(const wchar_t *wide, char *utf8, size_t utf8_size)
{
	int required;

	required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, NULL, 0, NULL, NULL);
	if (required == 0 || (size_t)required > utf8_size)
		return -1;
	if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, utf8, required, NULL, NULL) ==
	    0)
		return -1;
	return 0;
}

static int open_with_closed_handle(struct device *device, const char *path)
{
	char error[512];

	device_init(device);
	if (device_open(device, path, error, sizeof(error)) != 0) {
		fprintf(stderr, "device_open: %s\n", error);
		return -1;
	}
	if (!CloseHandle(device->handle)) {
		fprintf(stderr, "cannot invalidate the test image handle\n");
		device_close(device);
		return -1;
	}
	device->handle = INVALID_HANDLE_VALUE;
	return 0;
}

static int check_error(
    const struct device *device, const char *expected_operation, DWORD expected_number)
{
	char formatted_error[256];

	if (device->io_error_operation == NULL ||
	    strcmp(device->io_error_operation, expected_operation) != 0 ||
	    device->io_error_number != expected_number) {
		fprintf(stderr, "wrong I/O diagnostic for %s failure\n", expected_operation);
		return -1;
	}
	device_format_io_error(device, formatted_error, sizeof(formatted_error));
	if (formatted_error[0] == '\0') {
		fprintf(stderr, "empty formatted I/O diagnostic\n");
		return -1;
	}
	return 0;
}

static int check_io_diagnostics(const char *path)
{
	unsigned char sector[512] = { 0 };
	struct device device;

	if (open_with_closed_handle(&device, path) != 0)
		return -1;
	if (device.block_device.read(device.block_device.context, 0, 1, sector) == 0 ||
	    check_error(&device, "read", ERROR_INVALID_HANDLE) != 0)
		return -1;
	if (device.block_device.write(device.block_device.context, 0, 1, sector) == 0 ||
	    check_error(&device, "read", ERROR_INVALID_HANDLE) != 0) {
		fprintf(stderr, "a later failure replaced the first I/O diagnostic\n");
		return -1;
	}

	if (open_with_closed_handle(&device, path) != 0)
		return -1;
	if (device.block_device.write(device.block_device.context, 0, 1, sector) == 0 ||
	    check_error(&device, "write", ERROR_INVALID_HANDLE) != 0)
		return -1;

	if (open_with_closed_handle(&device, path) != 0)
		return -1;
	if (device.block_device.sync(device.block_device.context) == 0 ||
	    check_error(&device, "synchronize", ERROR_INVALID_HANDLE) != 0)
		return -1;

	return 0;
}

static int expect_unsupported_path(const char *path)
{
	struct device device;
	char error[512];

	device_init(&device);
	if (device_open(&device, path, error, sizeof(error)) == 0) {
		fprintf(stderr, "unsupported path was opened: %s\n", path);
		device_close(&device);
		return -1;
	}
	if (strstr(error, "not supported yet") == NULL) {
		fprintf(stderr, "unexpected path-rejection diagnostic: %s\n", error);
		return -1;
	}
	return 0;
}

static int check_image_size(const wchar_t *path)
{
	LARGE_INTEGER size;
	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
	    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

	if (file == INVALID_HANDLE_VALUE)
		return -1;
	if (!GetFileSizeEx(file, &size)) {
		(void)CloseHandle(file);
		return -1;
	}
	(void)CloseHandle(file);
	return size.QuadPart == 4096 ? 0 : -1;
}

int main(void)
{
	wchar_t wide_path[MAX_PATH];
	char path[MAX_PATH * 4];
	char error[512];
	unsigned char buffer[1024];
	struct device device;
	HANDLE competing;
	size_t index;
	int status = EXIT_FAILURE;

	device_init(&device);
	if (create_image(wide_path, sizeof(wide_path) / sizeof(wide_path[0])) != 0) {
		fwprintf(stderr, L"cannot create test image: Windows error %lu\n",
		    (unsigned long)GetLastError());
		return EXIT_FAILURE;
	}
	if (wide_to_utf8(wide_path, path, sizeof(path)) != 0) {
		fprintf(stderr, "cannot convert the test image path to UTF-8\n");
		goto cleanup;
	}
	if (device_open(&device, path, error, sizeof(error)) != 0) {
		fprintf(stderr, "device_open: %s\n", error);
		goto cleanup;
	}
	if (device.block_device.sector_size != 512 || device.block_device.sector_count != 8) {
		fprintf(stderr, "wrong image geometry\n");
		goto close_device;
	}

	competing =
	    CreateFileW(wide_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
	        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (competing != INVALID_HANDLE_VALUE || GetLastError() != ERROR_SHARING_VIOLATION) {
		fprintf(stderr, "image was not held with exclusive sharing\n");
		if (competing != INVALID_HANDLE_VALUE)
			(void)CloseHandle(competing);
		goto close_device;
	}

	memset(buffer, 0xa5, sizeof(buffer));
	if (device.block_device.write(device.block_device.context, 7, 1, buffer) != 0 ||
	    device.block_device.sync(device.block_device.context) != 0) {
		fprintf(stderr, "in-range write or synchronization failed\n");
		goto close_device;
	}
	memset(buffer, 0, sizeof(buffer));
	buffer[512] = 0x5a;
	if (device.block_device.read(device.block_device.context, 7, 1, buffer) != 0) {
		fprintf(stderr, "last in-range sector was rejected\n");
		goto close_device;
	}
	for (index = 0; index < 512; ++index) {
		if (buffer[index] != 0xa5) {
			fprintf(stderr, "read did not complete the requested transfer\n");
			goto close_device;
		}
	}
	if (buffer[512] != 0x5a) {
		fprintf(stderr, "read modified bytes beyond the requested transfer\n");
		goto close_device;
	}
	if (device.block_device.write(device.block_device.context, 7, 2, buffer) == 0 ||
	    device.block_device.read(device.block_device.context, 7, 2, buffer) == 0) {
		fprintf(stderr, "out-of-range transfer was accepted\n");
		goto close_device;
	}

	device_close(&device);
	if (check_image_size(wide_path) != 0) {
		fprintf(stderr, "out-of-range write changed the image size\n");
		goto cleanup;
	}
	if (check_io_diagnostics(path) != 0)
		goto cleanup;
	if (expect_unsupported_path("E:") != 0 ||
	    expect_unsupported_path("\\\\.\\PhysicalDrive0") != 0 ||
	    expect_unsupported_path("//./PhysicalDrive0") != 0 ||
	    expect_unsupported_path("\\\\?\\Volume{00000000-0000-0000-0000-000000000000}\\") != 0)
		goto cleanup;

	status = EXIT_SUCCESS;
	printf("device-image: passed\n");
	goto cleanup;

close_device:
	device_close(&device);
cleanup:
	(void)DeleteFileW(wide_path);
	return status;
}
