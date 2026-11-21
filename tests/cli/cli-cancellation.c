/* SPDX-License-Identifier: MIT */

#include "cli.h"
#include "device.h"
#include "endian.h"
#include "support/shrink_fixture.h"
#include <errno.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum cancellation_mode {
	CANCEL_BEFORE_OPEN,
	CANCEL_AFTER_OPEN,
	CANCEL_AFTER_OPEN_DISMOUNT_FAILURE,
	CANCEL_SHRINK,
	CANCEL_SHRINK_CLEANUP_FAILURE,
	DIAGNOSTIC_ORPHAN,
	DIAGNOSTIC_FAT_CHAIN,
	DIAGNOSTIC_DIRECTORY_CHECKSUM
};

static enum cancellation_mode mode;
static struct exfat_fixture shrink_fixture;
static struct exfat_fixture diagnostic_fixture;
static int shrink_cancelled;
static int cancellation_calls;
static int claim_active;
static int open_calls;
static int dismount_calls;
static int close_calls;
static int device_callback_calls;
static int cleanup_order_valid = 1;

static int initialize_diagnostic_fixture(void)
{
	unsigned char sector[512];
	uint64_t sector_number;
	uint32_t cluster;

	if (exfat_fixture_initialize(&diagnostic_fixture, 65536) != 0)
		return -1;
	if (mode == DIAGNOSTIC_DIRECTORY_CHECKSUM) {
		sector_number = exfat_fixture_cluster_sector(
		    &diagnostic_fixture.geometry, diagnostic_fixture.geometry.root_directory_cluster);
		if (exfat_fixture_read_sector(&diagnostic_fixture, sector_number, sector, sizeof(sector)) !=
		    0)
			return -1;
		sector[64 + 5] ^= 1;
	} else {
		cluster = mode == DIAGNOSTIC_ORPHAN ? 9 : diagnostic_fixture.bitmap_clusters[0];
		sector_number = diagnostic_fixture.geometry.fat_offset + cluster * 4 / sizeof(sector);
		if (exfat_fixture_read_sector(&diagnostic_fixture, sector_number, sector, sizeof(sector)) !=
		        0 ||
		    exfat_resize_store_le32(sector, sizeof(sector), cluster * 4 % sizeof(sector),
		        UINT32_C(0xffffffff)) != EXFAT_RESIZE_SUCCESS)
			return -1;
	}
	if (diagnostic_fixture.memory.device.write(
	        diagnostic_fixture.memory.device.context, sector_number, 1, sector) != 0)
		return -1;
	if (mode == DIAGNOSTIC_ORPHAN) {
		const unsigned int bit = 9 - 2;
		sector_number = exfat_fixture_cluster_sector(
		    &diagnostic_fixture.geometry, diagnostic_fixture.bitmap_clusters[0]);
		if (exfat_fixture_read_sector(&diagnostic_fixture, sector_number, sector, sizeof(sector)) !=
		    0)
			return -1;
		sector[bit / 8] |= (unsigned char)(1u << (bit % 8));
		if (diagnostic_fixture.memory.device.write(
		        diagnostic_fixture.memory.device.context, sector_number, 1, sector) != 0)
			return -1;
	}
	memory_block_device_clear_operations(&diagnostic_fixture.memory);
	return 0;
}

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
}

int device_open(struct device *device, const char *path, char *error, size_t error_size)
{
	(void)path;
	(void)error;
	(void)error_size;
	++open_calls;
	claim_active = 1;
	if (mode >= DIAGNOSTIC_ORPHAN) {
		if (initialize_diagnostic_fixture() != 0)
			return -1;
		device->block_device = diagnostic_fixture.memory.device;
		return 0;
	}
	if (mode == CANCEL_SHRINK || mode == CANCEL_SHRINK_CLEANUP_FAILURE) {
		if (shrink_fixture_initialize(&shrink_fixture, 1) != 0)
			return -1;
		device->block_device = shrink_fixture.memory.device;
		return 0;
	}
	device->block_device.context = device;
	device->block_device.sector_size = 512;
	device->block_device.sector_count = 32;
	device->block_device.read = block_read;
	device->block_device.write = block_write;
	device->block_device.sync = block_sync;
	return 0;
}

#if defined(_WIN32)
enum device_partition_growth_result device_grow_partition(struct device *device,
    const char *path,
    uint64_t target_size,
    const struct cli_cancellation *cancellation,
    enum device_partition_state *partition_state,
    char *error,
    size_t error_size)
{
	(void)device;
	(void)path;
	(void)target_size;
	(void)cancellation;
	(void)partition_state;
	(void)error;
	(void)error_size;
	++device_callback_calls;
	return DEVICE_PARTITION_GROWTH_ERROR;
}

int device_prepare_partition_shrink(struct device *device,
    const char *path,
    uint64_t target_size,
    struct device_partition_shrink **plan,
    char *error,
    size_t error_size)
{
	(void)device;
	(void)path;
	(void)target_size;
	(void)plan;
	(void)error;
	(void)error_size;
	return -1;
}
int device_shrink_partition(struct device *device,
    const char *path,
    struct device_partition_shrink *plan,
    enum device_partition_state *state,
    char *error,
    size_t error_size)
{
	(void)device;
	(void)path;
	(void)plan;
	(void)state;
	(void)error;
	(void)error_size;
	return -1;
}
void device_free_partition_shrink(struct device_partition_shrink *plan)
{
	(void)plan;
}
#endif

int device_dismount(struct device *device, const char *path, char *error, size_t error_size)
{
	(void)device;
	(void)path;
	if (!claim_active || close_calls != 0)
		cleanup_order_valid = 0;
	++dismount_calls;
	if (mode == CANCEL_AFTER_OPEN_DISMOUNT_FAILURE) {
		(void)snprintf(error, error_size, "test-device: cannot dismount test volume");
		return -1;
	}
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
	(void)device;
	if (claim_active && dismount_calls != 1)
		cleanup_order_valid = 0;
	++close_calls;
	claim_active = 0;
}

static int cancellation_requested(void *context)
{
	(void)context;
	++cancellation_calls;
	if ((mode == CANCEL_SHRINK || mode == CANCEL_SHRINK_CLEANUP_FAILURE) && claim_active) {
		size_t i;
		size_t syncs = 0;
		for (i = 0; i < shrink_fixture.memory.operation_count; ++i) {
			if (shrink_fixture.memory.operations[i].kind == MEMORY_OPERATION_SYNC)
				++syncs;
		}
		if (syncs >= 2 && !shrink_cancelled) {
			shrink_cancelled = 1;
			if (mode == CANCEL_SHRINK_CLEANUP_FAILURE)
				memory_block_device_fail_operation(
				    &shrink_fixture.memory, shrink_fixture.memory.operation_index, EIO);
		}
		return shrink_cancelled;
	}
	return mode == CANCEL_BEFORE_OPEN ||
	    ((mode == CANCEL_AFTER_OPEN || mode == CANCEL_AFTER_OPEN_DISMOUNT_FAILURE) && claim_active);
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
		fprintf(stderr, "usage: cli-cancellation MODE (cancellation or diagnostic case)\n");
		return EXIT_FAILURE;
	}
	if (strcmp(argv[1], "before-open") == 0)
		mode = CANCEL_BEFORE_OPEN;
	else if (strcmp(argv[1], "after-open") == 0)
		mode = CANCEL_AFTER_OPEN;
	else if (strcmp(argv[1], "after-open-dismount-failure") == 0)
		mode = CANCEL_AFTER_OPEN_DISMOUNT_FAILURE;
	else if (strcmp(argv[1], "shrink") == 0)
		mode = CANCEL_SHRINK;
	else if (strcmp(argv[1], "shrink-cleanup-failure") == 0)
		mode = CANCEL_SHRINK_CLEANUP_FAILURE;
	else if (strcmp(argv[1], "diagnostic-orphan") == 0)
		mode = DIAGNOSTIC_ORPHAN;
	else if (strcmp(argv[1], "diagnostic-fat-chain") == 0)
		mode = DIAGNOSTIC_FAT_CHAIN;
	else if (strcmp(argv[1], "diagnostic-directory-checksum") == 0)
		mode = DIAGNOSTIC_DIRECTORY_CHECKSUM;
	else
		return EXIT_FAILURE;
	if (mode == CANCEL_SHRINK || mode == CANCEL_SHRINK_CLEANUP_FAILURE)
		cli_argv[2] = "1077248";
	if (mode >= DIAGNOSTIC_ORPHAN)
		cli_argv[2] = "33554432";
	status = cli_main(3, cli_argv, &cancellation);
	if (status !=
	        (mode == CANCEL_SHRINK_CLEANUP_FAILURE || mode >= DIAGNOSTIC_ORPHAN
	                ? EXIT_FAILURE
	                : CLI_CANCELLED_EXIT_STATUS) ||
	    cancellation_calls == 0 || claim_active || device_callback_calls != 0 ||
	    !cleanup_order_valid) {
		fprintf(stderr, "cancellation did not follow the expected error path\n");
		return mode >= DIAGNOSTIC_ORPHAN ? 2 : EXIT_FAILURE;
	}
	if (mode == CANCEL_BEFORE_OPEN &&
	    (open_calls != 0 || dismount_calls != 0 || close_calls != 0)) {
		fprintf(stderr, "pre-open cancellation acquired or released a device claim\n");
		return EXIT_FAILURE;
	}
	if (mode != CANCEL_BEFORE_OPEN &&
	    (open_calls != 1 || dismount_calls != 1 || close_calls != 1)) {
		fprintf(stderr, "post-open cancellation did not perform normal cleanup\n");
		return mode >= DIAGNOSTIC_ORPHAN ? 2 : EXIT_FAILURE;
	}
	if (mode == CANCEL_SHRINK || mode == CANCEL_SHRINK_CLEANUP_FAILURE) {
		if (mode == CANCEL_SHRINK &&
		    (memory_block_device_crash(&shrink_fixture.memory) != 0 ||
		        shrink_fixture_verify(&shrink_fixture, 10000) != 0))
			return EXIT_FAILURE;
		if (exfat_fixture_destroy(&shrink_fixture) != 0)
			return EXIT_FAILURE;
	}
	if (mode >= DIAGNOSTIC_ORPHAN) {
		size_t i;
		if (diagnostic_fixture.memory.operation_count == 0)
			return 2;
		for (i = 0; i < diagnostic_fixture.memory.operation_count; ++i) {
			if (diagnostic_fixture.memory.operations[i].kind != MEMORY_OPERATION_READ) {
				fprintf(stderr, "diagnostic preflight modified the filesystem\n");
				return 2;
			}
		}
		if (exfat_fixture_destroy(&diagnostic_fixture) != 0)
			return 2;
	}
	return status;
}
