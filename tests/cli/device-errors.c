/* SPDX-License-Identifier: MIT */

#define _POSIX_C_SOURCE 200809L

#include "device.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int open_with_closed_descriptor(struct device *device, const char *path)
{
	char error[256];

	if (device_open(device, path, error, sizeof(error)) != 0) {
		fprintf(stderr, "device_open: %s\n", error);
		return -1;
	}
	if (close(device->fd) != 0) {
		perror("close");
		device_close(device);
		return -1;
	}
	device->fd = -1;
	return 0;
}

static int check_error(
    const struct device *device, const char *expected_operation, int expected_number)
{
	if (device->io_error_operation == NULL ||
	    strcmp(device->io_error_operation, expected_operation) != 0 ||
	    device->io_error_number != expected_number) {
		fprintf(stderr, "wrong I/O diagnostic for %s failure\n", expected_operation);
		return -1;
	}
	return 0;
}

int main(void)
{
	char path[] = "/tmp/exfat-resize-device-errors.XXXXXX";
	unsigned char sector[512] = { 0 };
	struct device device = { .fd = -1 };
	int fd, status = EXIT_FAILURE;

	fd = mkstemp(path);
	if (fd < 0 || ftruncate(fd, sizeof(sector)) != 0 || close(fd) != 0) {
		perror("create test image");
		if (fd >= 0)
			(void)close(fd);
		goto cleanup;
	}

	if (open_with_closed_descriptor(&device, path) != 0)
		goto cleanup;
	if (device.block_device.read(device.block_device.context, 0, 1, sector) == 0 ||
	    check_error(&device, "read", EBADF) != 0)
		goto close_device;
	if (device.block_device.write(device.block_device.context, 0, 1, sector) == 0 ||
	    check_error(&device, "read", EBADF) != 0) {
		fprintf(stderr, "a later failure replaced the first I/O diagnostic\n");
		goto close_device;
	}
	device_close(&device);

	if (open_with_closed_descriptor(&device, path) != 0)
		goto cleanup;
	if (device.block_device.write(device.block_device.context, 0, 1, sector) == 0 ||
	    check_error(&device, "write", EBADF) != 0)
		goto close_device;
	device_close(&device);

	if (open_with_closed_descriptor(&device, path) != 0)
		goto cleanup;
	if (device.block_device.sync(device.block_device.context) == 0 ||
	    check_error(&device, "synchronize", EBADF) != 0)
		goto close_device;
	device_close(&device);

	status = EXIT_SUCCESS;
	printf("device-errors: passed\n");
	goto cleanup;

close_device:
	device_close(&device);
cleanup:
	(void)unlink(path);
	return status;
}
