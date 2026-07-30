/* SPDX-License-Identifier: MIT */

#define _FILE_OFFSET_BITS 64
#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "device.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *peer_path;
static off_t peer_size;
static int peer_result;

int __real_flock(int fd, int operation);

static int wait_for_child(pid_t child)
{
	pid_t waited;
	int status;

	do {
		waited = waitpid(child, &status, 0);
	} while (waited < 0 && errno == EINTR);
	if (waited < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS)
		return -1;
	return 0;
}

static int resize_with_peer(const char *path, off_t size)
{
	pid_t child = fork();

	if (child < 0)
		return -1;
	if (child == 0) {
		int fd = open(path, O_RDWR);

		if (fd < 0 || __real_flock(fd, LOCK_EX) != 0 || ftruncate(fd, size) != 0)
			_exit(EXIT_FAILURE);
		if (__real_flock(fd, LOCK_UN) != 0 || close(fd) != 0)
			_exit(EXIT_FAILURE);
		_exit(EXIT_SUCCESS);
	}
	return wait_for_child(child);
}

int __wrap_flock(int fd, int operation)
{
	if (peer_path != NULL && operation == (LOCK_EX | LOCK_NB)) {
		const char *path = peer_path;

		peer_path = NULL;
		peer_result = resize_with_peer(path, peer_size);
	}
	return __real_flock(fd, operation);
}

static int create_image(char *path, off_t size)
{
	int fd = mkstemp(path);

	if (fd < 0 || ftruncate(fd, size) != 0 || close(fd) != 0) {
		perror("create lock-size test image");
		if (fd >= 0)
			(void)close(fd);
		(void)unlink(path);
		return -1;
	}
	return 0;
}

static int test_size_change(off_t initial_size, off_t locked_size)
{
	char path[] = "/tmp/exfat-resize-device-lock-size.XXXXXX";
	struct device device = { .fd = -1 };
	unsigned char sector[512] = { 0 };
	struct stat st;
	char error[512];
	int result = EXIT_FAILURE;

	if (create_image(path, initial_size) != 0)
		return EXIT_FAILURE;

	peer_path = path;
	peer_size = locked_size;
	peer_result = 0;
	if (device_open(&device, path, error, sizeof(error)) != 0) {
		fprintf(stderr, "device_open: %s\n", error);
		goto out;
	}
	if (peer_path != NULL || peer_result != 0) {
		fprintf(stderr, "competing process did not resize the image before locking\n");
		goto out;
	}
	if (device.block_device.sector_count != (uint64_t)locked_size / 512) {
		fprintf(stderr, "stale size after locking: got %llu sectors, expected %llu\n",
		    (unsigned long long)device.block_device.sector_count,
		    (unsigned long long)((uint64_t)locked_size / 512));
		goto out;
	}
	if (locked_size < initial_size &&
	    device.block_device.write(
	        device.block_device.context, (uint64_t)locked_size / 512, 1, sector) == 0) {
		fprintf(stderr, "write beyond the locked file size succeeded\n");
		goto out;
	}
	if (stat(path, &st) != 0 || st.st_size != locked_size) {
		fprintf(stderr, "out-of-range write changed the locked file size\n");
		goto out;
	}

	result = EXIT_SUCCESS;

out:
	peer_path = NULL;
	device_close(&device);
	(void)unlink(path);
	return result;
}

int main(void)
{
	if (test_size_change(4096, 8192) != EXIT_SUCCESS)
		return EXIT_FAILURE;
	if (test_size_change(8192, 4096) != EXIT_SUCCESS)
		return EXIT_FAILURE;
	printf("device-lock-size: passed\n");
	return EXIT_SUCCESS;
}
