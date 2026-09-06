/* SPDX-License-Identifier: MIT */

#include "cli.h"
#include "device.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum cancellation_mode { CANCEL_BEFORE_OPEN, CANCEL_AFTER_OPEN, CANCEL_DURING_PREFLIGHT };

static enum cancellation_mode mode;
static int cancellation_calls;
static int claim_active;
static int open_calls;
static int dismount_calls;
static int close_calls;
static int device_callback_calls;

static int block_read(void *context, uint64_t first_sector, uint32_t sector_count, void *buffer)
{
	(void)context;
	(void)first_sector;
	(void)sector_count;
	(void)buffer;
	++device_callback_calls;
	return -1;
}

static int block_write(
    void *context, uint64_t first_sector, uint32_t sector_count, const void *buffer)
{
	(void)context;
	(void)first_sector;
	(void)sector_count;
	(void)buffer;
	++device_callback_calls;
	return -1;
}

static int block_sync(void *context)
{
	(void)context;
	++device_callback_calls;
	return -1;
}

void device_init(struct device *device)
{
	(void)memset(device, 0, sizeof(*device));
	device->fd = -1;
}

int device_open(struct device *device, const char *path, char *error, size_t error_size)
{
	(void)path;
	(void)error;
	(void)error_size;
	++open_calls;
	claim_active = 1;
	device->fd = 1;
	device->block_device.context = device;
	device->block_device.sector_size = 512;
	device->block_device.sector_count = 32;
	device->block_device.read = block_read;
	device->block_device.write = block_write;
	device->block_device.sync = block_sync;
	return 0;
}

int device_grow_partition(struct device *device,
    const char *path,
    uint64_t target_size,
    enum device_partition_state *partition_state,
    char *error,
    size_t error_size)
{
	(void)device;
	(void)path;
	(void)target_size;
	(void)partition_state;
	(void)error;
	(void)error_size;
	++device_callback_calls;
	return -1;
}

int device_dismount(struct device *device, const char *path, char *error, size_t error_size)
{
	(void)device;
	(void)path;
	(void)error;
	(void)error_size;
	++dismount_calls;
	return 0;
}

void device_format_io_error(const struct device *device, char *error, size_t error_size)
{
	(void)device;
	if (error_size != 0)
		error[0] = '\0';
}

void device_close(struct device *device)
{
	++close_calls;
	claim_active = 0;
	device->fd = -1;
}

static int cancellation_requested(void *context)
{
	(void)context;
	++cancellation_calls;
	return mode == CANCEL_BEFORE_OPEN || (mode == CANCEL_AFTER_OPEN && claim_active) ||
	    (mode == CANCEL_DURING_PREFLIGHT && cancellation_calls >= 3);
}

int main(int argc, char **argv)
{
	struct cli_cancellation cancellation = {
		.context = NULL,
		.requested = cancellation_requested,
	};
	char *cli_argv[] = { "exfat-resize", "test-device", "8192", NULL };
	int status;

	if (argc != 2) {
		fprintf(stderr, "usage: cli-cancellation before-open|after-open|during-preflight\n");
		return EXIT_FAILURE;
	}
	if (strcmp(argv[1], "before-open") == 0)
		mode = CANCEL_BEFORE_OPEN;
	else if (strcmp(argv[1], "after-open") == 0)
		mode = CANCEL_AFTER_OPEN;
	else if (strcmp(argv[1], "during-preflight") == 0)
		mode = CANCEL_DURING_PREFLIGHT;
	else
		return EXIT_FAILURE;
	status = cli_main(3, cli_argv, &cancellation);
	if (status != CLI_CANCELLED_EXIT_STATUS || cancellation_calls == 0 || claim_active ||
	    device_callback_calls != 0) {
		fprintf(stderr, "cancellation did not follow the expected error path\n");
		return EXIT_FAILURE;
	}
	if (mode == CANCEL_BEFORE_OPEN &&
	    (open_calls != 0 || dismount_calls != 0 || close_calls != 0)) {
		fprintf(stderr, "pre-open cancellation acquired or released a device claim\n");
		return EXIT_FAILURE;
	}
	if (mode != CANCEL_BEFORE_OPEN &&
	    (open_calls != 1 || dismount_calls != 1 || close_calls != 1)) {
		fprintf(stderr, "post-open cancellation did not perform normal cleanup\n");
		return EXIT_FAILURE;
	}
	return status;
}
