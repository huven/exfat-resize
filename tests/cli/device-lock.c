/* SPDX-License-Identifier: MIT */

#define _POSIX_C_SOURCE 200809L

#include "device.h"

#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static int expect_locked_device(const char *path)
{
	struct device device = { .fd = -1 };
	char error[512];

	if (device_open(&device, path, error, sizeof(error)) == 0) {
		fprintf(stderr, "device_open succeeded despite an existing exclusive lock\n");
		device_close(&device);
		return EXIT_FAILURE;
	}
	if (strstr(error, "mounted or already in use") == NULL) {
		fprintf(stderr, "unexpected exclusive-open error: %s\n", error);
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}

static int wait_for_child(pid_t child)
{
	pid_t waited;
	int status;

	do {
		waited = waitpid(child, &status, 0);
	} while (waited < 0 && errno == EINTR);
	if (waited < 0) {
		perror("waitpid");
		return -1;
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS)
		return -1;
	return 0;
}

static int check_open_flags(void)
{
#if defined(__linux__)
	if ((device_open_flags(0) & O_EXCL) != 0 || (device_open_flags(1) & O_EXCL) == 0) {
		fprintf(stderr, "O_EXCL selection does not distinguish regular and block devices\n");
		return -1;
	}
#endif
	return 0;
}

static int test_device_lock(const char *program, const char *path)
{
	struct device held_device = { .fd = -1 };
	struct device reopened_device = { .fd = -1 };
	posix_spawn_file_actions_t actions;
	char error[512];
	char *child_arguments[4];
	pid_t child;
	int result;

	if (device_open(&held_device, path, error, sizeof(error)) != 0) {
		fprintf(stderr, "initial device_open failed: %s\n", error);
		return EXIT_FAILURE;
	}

	child_arguments[0] = (char *)program;
	child_arguments[1] = "--expect-locked";
	child_arguments[2] = (char *)path;
	child_arguments[3] = NULL;
	result = posix_spawn_file_actions_init(&actions);
	if (result != 0) {
		fprintf(stderr, "cannot initialize competing process: %s\n", strerror(result));
		device_close(&held_device);
		return EXIT_FAILURE;
	}
	result = posix_spawn_file_actions_addclose(&actions, held_device.fd);
	if (result == 0)
		result = posix_spawn(&child, program, &actions, NULL, child_arguments, environ);
	(void)posix_spawn_file_actions_destroy(&actions);
	if (result != 0) {
		fprintf(stderr, "cannot start competing process: %s\n", strerror(result));
		device_close(&held_device);
		return EXIT_FAILURE;
	}
	if (wait_for_child(child) != 0) {
		fprintf(stderr, "competing process did not observe the exclusive lock\n");
		device_close(&held_device);
		return EXIT_FAILURE;
	}
	device_close(&held_device);

	if (device_open(&reopened_device, path, error, sizeof(error)) != 0) {
		fprintf(stderr, "device_open failed after releasing lock: %s\n", error);
		return EXIT_FAILURE;
	}
	device_close(&reopened_device);
	printf("device-lock: passed\n");
	return EXIT_SUCCESS;
}

static int test_regular_file(const char *program)
{
	char path[] = "/tmp/exfat-resize-device-lock.XXXXXX";
	int fd, result;

	fd = mkstemp(path);
	if (fd < 0 || ftruncate(fd, 4096) != 0 || close(fd) != 0) {
		perror("create lock-test image");
		if (fd >= 0)
			(void)close(fd);
		(void)unlink(path);
		return EXIT_FAILURE;
	}
	result = test_device_lock(program, path);
	(void)unlink(path);
	return result;
}

int main(int argc, char **argv)
{
	if (check_open_flags() != 0)
		return EXIT_FAILURE;
	if (argc == 3 && strcmp(argv[1], "--expect-locked") == 0)
		return expect_locked_device(argv[2]);
	if (argc == 1)
		return test_regular_file(argv[0]);
	if (argc == 2)
		return test_device_lock(argv[0], argv[1]);
	fprintf(stderr, "usage: device-lock-test [device]\n");
	return EXIT_FAILURE;
}
