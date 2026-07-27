/* SPDX-License-Identifier: MIT */

#define _POSIX_C_SOURCE 200809L

#include "device.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void)
{
	char path[] = "/tmp/exfat-resize-device-bounds.XXXXXX";
	char error[256];
	unsigned char buffer[1024];
	struct device device = { .fd = -1 };
	struct stat st;
	size_t index;
	int fd, status = EXIT_FAILURE;

	fd = mkstemp(path);
	if (fd < 0 || ftruncate(fd, 4096) != 0 || close(fd) != 0) {
		perror("create test image");
		if (fd >= 0)
			(void)close(fd);
		goto cleanup;
	}
	if (device_open(&device, path, error, sizeof(error)) != 0) {
		fprintf(stderr, "device_open: %s\n", error);
		goto cleanup;
	}
	memset(buffer, 0xA5, sizeof(buffer));
	if (device.block_device.write(device.block_device.context, 7, 1, buffer) != 0 ||
	    device.block_device.sync(device.block_device.context) != 0) {
		fprintf(stderr, "in-range write or synchronization failed\n");
		goto close_device;
	}
	memset(buffer, 0, sizeof(buffer));
	buffer[512] = 0x5A;
	if (device.block_device.read(device.block_device.context, 7, 1, buffer) != 0) {
		fprintf(stderr, "last in-range sector was rejected\n");
		goto close_device;
	}
	for (index = 0; index < 512; ++index) {
		if (buffer[index] != 0xA5) {
			fprintf(stderr, "read did not complete the requested transfer\n");
			goto close_device;
		}
	}
	if (buffer[512] != 0x5A) {
		fprintf(stderr, "read modified bytes beyond the requested transfer\n");
		goto close_device;
	}
	if (device.block_device.write(device.block_device.context, 7, 2, buffer) == 0 ||
	    device.block_device.read(device.block_device.context, 7, 2, buffer) == 0) {
		fprintf(stderr, "out-of-range transfer was accepted\n");
		goto close_device;
	}
	device_close(&device);
	if (stat(path, &st) != 0 || st.st_size != 4096) {
		fprintf(stderr, "out-of-range write changed the image size\n");
		goto cleanup;
	}
	status = EXIT_SUCCESS;
	printf("device-bounds: passed\n");
	goto cleanup;

close_device:
	device_close(&device);
cleanup:
	(void)unlink(path);
	return status;
}
