/* SPDX-License-Identifier: MIT */

#define _FILE_OFFSET_BITS 64
#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/disk.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int test_fsync(int fd);
static int test_fcntl(int fd, int command, ...);
static int test_ioctl(int fd, unsigned long command, ...);

/* Exercise the actual adapter without opening a device or flushing real storage. */
#define fsync test_fsync
#define fcntl test_fcntl
#define ioctl test_ioctl
#include "../../src/posix/device.c"
#undef fsync
#undef fcntl
#undef ioctl

enum { TEST_FD = 123, WRITTEN_VALUE = 73 };

static int failure_count;
static char calls[16];
static size_t call_count;
static unsigned host_cache;
static unsigned media_cache;
static unsigned durable_value;
static unsigned drain_interrupts;
static unsigned flush_interrupts;
static int drain_error;
static int flush_error;

#define CHECK(expression) \
	do { \
		if (!(expression)) { \
			fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expression); \
			++failure_count; \
		} \
	} while (0)

static void record_call(int fd, char operation)
{
	CHECK(fd == TEST_FD);
	CHECK(call_count + 1 < sizeof(calls));
	if (call_count + 1 < sizeof(calls)) {
		calls[call_count++] = operation;
		calls[call_count] = '\0';
	}
}

static int injected_result(unsigned *interrupts, int error)
{
	if (*interrupts != 0) {
		--*interrupts;
		errno = EINTR;
		return -1;
	}
	if (error != 0) {
		errno = error;
		return -1;
	}
	return 0;
}

static void drain_host_cache(void)
{
	if (host_cache != 0) {
		media_cache = host_cache;
		host_cache = 0;
	}
}

static int test_fsync(int fd)
{
	record_call(fd, 'D');
	if (injected_result(&drain_interrupts, drain_error) != 0)
		return -1;
	drain_host_cache();
	return 0;
}

static int test_fcntl(int fd, int command, ...)
{
	record_call(fd, 'F');
	CHECK(command == F_FULLFSYNC);
	if (injected_result(&flush_interrupts, flush_error) != 0)
		return -1;
	drain_host_cache();
	durable_value = media_cache;
	return 0;
}

static int test_ioctl(int fd, unsigned long command, ...)
{
	dk_synchronize_t *request;
	va_list args;

	record_call(fd, 'M');
	CHECK(command == DKIOCSYNCHRONIZE);
	va_start(args, command);
	request = va_arg(args, dk_synchronize_t *);
	va_end(args);
	CHECK(request->offset == 0 && request->length == 0 && request->options == 0);
	CHECK(host_cache == 0);
	if (injected_result(&flush_interrupts, flush_error) != 0)
		return -1;
	/* An ordering barrier may leave every pending write volatile. */
	if (request->options == 0)
		durable_value = media_cache;
	return 0;
}

static void check_sync(int regular_file,
    int buffered,
    unsigned drain_retries,
    int drain_failure,
    unsigned flush_retries,
    int flush_failure,
    const char *expected_calls)
{
	struct device device;
	int expected_error = drain_failure != 0 ? drain_failure : flush_failure;
	int result;

	device_init(&device);
	device.fd = TEST_FD;
	device.is_regular_file = regular_file;
	calls[0] = '\0';
	call_count = 0;
	host_cache = buffered ? WRITTEN_VALUE : 0;
	media_cache = buffered ? 0 : WRITTEN_VALUE;
	durable_value = 0;
	drain_interrupts = drain_retries;
	flush_interrupts = flush_retries;
	drain_error = drain_failure;
	flush_error = flush_failure;

	result = block_device_sync(&device);
	CHECK(strcmp(calls, expected_calls) == 0);
	if (expected_error != 0) {
		CHECK(result == -1);
		CHECK(device.io_error_number == expected_error);
		CHECK(device.io_error_operation != NULL &&
		    strcmp(device.io_error_operation, "synchronize") == 0);
		CHECK(durable_value == 0);
	} else {
		CHECK(result == 0);
		CHECK(device.io_error_operation == NULL);
		CHECK(durable_value == WRITTEN_VALUE);
	}
}

int main(void)
{
	/* Buffered and raw nodes must both reach durable storage before returning. */
	for (int buffered = 0; buffered <= 1; ++buffered) {
		check_sync(0, buffered, 0, 0, 0, 0, "DM");
		check_sync(0, buffered, 2, 0, 2, 0, "DDDMMM");
		check_sync(0, buffered, 2, EIO, 0, 0, "DDD");
		check_sync(0, buffered, 0, 0, 2, ENOTTY, "DMMM");
	}
	/* Regular images keep the filesystem's combined drain-and-flush operation. */
	check_sync(1, 1, 0, 0, 0, 0, "F");
	check_sync(1, 1, 0, 0, 2, 0, "FFF");
	check_sync(1, 1, 0, 0, 2, EIO, "FFF");
	if (failure_count != 0)
		return EXIT_FAILURE;
	printf("macos-device-sync: passed\n");
	return EXIT_SUCCESS;
}
