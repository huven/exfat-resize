/* SPDX-License-Identifier: MIT */

#include "cli.h"

#include "block_device.h"
#include "boot_region.h"
#include "device.h"
#include "exfat_resize.h"
#include "geometry.h"
#include "sector_adapter.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef EXFAT_RESIZE_BUILD_VERSION
#error "EXFAT_RESIZE_BUILD_VERSION must be provided by the build system"
#endif

#if defined(_WIN32)
static const char target_name[] = "DEVICE";
static const char usage[] = "Usage: exfat-resize DEVICE [SIZE]\n"
                            "       exfat-resize --grow-partition DEVICE SIZE\n";
static const char introduction[] =
    "Grow an existing exFAT filesystem in a Windows image file or logical volume.";
static const char target_description[] =
    "Regular image file, drive letter such as E:, or\n"
    "                     volume-GUID path with the exFAT main boot sector\n"
    "                     at sector zero";
static const char platform_safety[] = "";
static const char documentation_lead[] = "";
static const char platform_options[] =
    "  --grow-partition   Grow a basic partition to explicit SIZE when needed\n";
static const char platform_note[] =
    "\nPhysical-disk paths such as \\\\.\\PhysicalDrive0 are not supported.\n";
#else
static const char target_name[] = "DEVICE";
static const char usage[] = "Usage: exfat-resize DEVICE [SIZE]\n";
static const char introduction[] = "Grow an existing exFAT filesystem.";
static const char target_description[] = "Regular file or raw block device with the exFAT\n"
                                         "                     main boot sector at sector zero";
static const char platform_safety[] =
    "  Ctrl-C requests cooperative cancellation at the next safe boundary.\n";
static const char documentation_lead[] = "  See exfat-resize(8).\n";
static const char platform_options[] = "";
static const char platform_note[] = "";
#endif

static void print_usage(FILE *stream)
{
	fputs(usage, stream);
}

static void print_no_write_guidance(void)
{
	fprintf(stderr,
	    "exfat-resize: no filesystem write was attempted; correct the error and retry when "
	    "appropriate\n");
}

int cli_report_startup_error(const char *message)
{
	fprintf(stderr, "exfat-resize: %s\n", message);
	print_no_write_guidance();
	return EXIT_FAILURE;
}

static void print_help(void)
{
	printf("%s"
	       "\n"
	       "%s\n"
	       "\n"
	       "Arguments:\n"
	       "  %-18s %s\n"
	       "  SIZE               Desired filesystem size in bytes or with an optional\n"
	       "                     K, M, or G suffix (powers of 1024)\n"
	       "                     (default: all available space)\n"
	       "\n"
	       "Options:\n"
	       "%s"
	       "  -h, --help         Show this help message\n"
	       "  -V, --version      Show version information\n"
	       "\n"
	       "Safety:\n"
	       "  Make and verify a backup before using this tool.\n"
	       "%s"
	       "\n"
	       "Documentation:\n"
	       "%s"
	       "  Read the safety requirements, supported-filesystem limitations, and\n"
	       "  recovery instructions in README.md distributed with exfat-resize or at:\n"
	       "    https://github.com/huven/exfat-resize#safety\n"
	       "%s",
	    usage, introduction, target_name, target_description, platform_options, platform_safety,
	    documentation_lead, platform_note);
}

static const char *resize_error(enum exfat_resize_error error)
{
	switch (error) {
	case EXFAT_RESIZE_SUCCESS:
		return "success";
	case EXFAT_RESIZE_INVALID_ARGUMENT:
		return "invalid argument";
	case EXFAT_RESIZE_INVALID_DEVICE:
		return "invalid block device";
	case EXFAT_RESIZE_INVALID_FILESYSTEM:
		return "invalid or corrupt exFAT filesystem";
	case EXFAT_RESIZE_OUT_OF_BOUNDS:
		return "request is outside the backing device";
	case EXFAT_RESIZE_ARITHMETIC_OVERFLOW:
		return "filesystem geometry is too large";
	case EXFAT_RESIZE_IO_ERROR:
		return "device I/O error";
	case EXFAT_RESIZE_INSUFFICIENT_WORKSPACE:
		return "working memory is too small";
	case EXFAT_RESIZE_INTERNAL_ERROR:
		return "internal error";
	case EXFAT_RESIZE_OUT_OF_MEMORY:
		return "cannot allocate working memory";
	case EXFAT_RESIZE_UNSUPPORTED_REVISION:
		return "unsupported exFAT revision";
	case EXFAT_RESIZE_UNSUPPORTED_MULTIPLE_FATS:
		return "multiple FATs are not supported";
	case EXFAT_RESIZE_VOLUME_DIRTY:
		return "filesystem is marked dirty";
	case EXFAT_RESIZE_MEDIA_FAILURE:
		return "filesystem has the media-failure flag set";
	case EXFAT_RESIZE_INSUFFICIENT_GROWTH:
		return "target does not add enough usable clusters";
	case EXFAT_RESIZE_CLUSTER_LIMIT_REACHED:
		return "filesystem has reached the exFAT cluster limit";
	case EXFAT_RESIZE_UNSUPPORTED_VENDOR_ALLOCATION:
		return "filesystem contains a Vendor Allocation entry";
	case EXFAT_RESIZE_UNSUPPORTED_CRITICAL_ENTRY:
		return "filesystem contains an unsupported critical directory entry";
	case EXFAT_RESIZE_UNSUPPORTED_ALLOCATED_ENTRY:
		return "filesystem contains unsupported allocated directory metadata";
	case EXFAT_RESIZE_BAD_CLUSTER_CONFLICT:
		return "expanded FAT conflicts with a source bad cluster";
	case EXFAT_RESIZE_UNSUPPORTED_SECTOR_MAPPING:
		return "filesystem sector size is incompatible with device sector size";
	case EXFAT_RESIZE_CANCELLED:
		return "operation cancelled";
	}
	return "unknown error";
}

static void *allocate_memory(void *context, size_t size)
{
	(void)context;
	return malloc(size);
}

static void deallocate_memory(void *context, void *memory, size_t size)
{
	(void)context;
	(void)size;
	free(memory);
}

static int parse_size(const char *text, uint64_t *value)
{
	const char *cursor = text;
	uint64_t multiplier = 1;
	uint64_t parsed = 0;

	if (*cursor == '\0')
		return -1;
	while (*cursor >= '0' && *cursor <= '9') {
		uint64_t digit;

		digit = (uint64_t)(*cursor - '0');
		if (parsed > (UINT64_MAX - digit) / 10)
			return -1;
		parsed = parsed * 10 + digit;
		++cursor;
	}
	switch (*cursor) {
	case '\0':
		break;
	case 'K':
		multiplier = UINT64_C(1024);
		break;
	case 'M':
		multiplier = UINT64_C(1024) * 1024;
		break;
	case 'G':
		multiplier = UINT64_C(1024) * 1024 * 1024;
		break;
	default:
		return -1;
	}
	if (*cursor != '\0' && cursor[1] != '\0')
		return -1;
	if (parsed == 0 || parsed > UINT64_MAX / multiplier)
		return -1;
	*value = parsed * multiplier;
	return 0;
}

static enum exfat_resize_error validate_partition_growth_preflight(
    const struct exfat_resize_block_device *device,
    uint64_t target_size,
    uint64_t *effective_target_size)
{
	struct exfat_resize_device_geometry target_device_geometry;
	struct exfat_resize_sector_adapter adapter;
	struct exfat_resize_geometry target_geometry;
	struct exfat_resize_geometry geometry;
	enum exfat_resize_error result;
	unsigned char *buffer;
	uint64_t target_sector_count;
	uint32_t filesystem_sector_size;

	buffer = malloc(EXFAT_RESIZE_MAX_SECTOR_SIZE);
	if (buffer == NULL)
		return EXFAT_RESIZE_OUT_OF_MEMORY;
	result = exfat_resize_validate_block_device(device);
	if (result == EXFAT_RESIZE_SUCCESS)
		result = exfat_resize_probe_sector_size(
		    device, buffer, EXFAT_RESIZE_MAX_SECTOR_SIZE, &filesystem_sector_size);
	if (result == EXFAT_RESIZE_SUCCESS)
		result = exfat_resize_adapt_block_device(device, filesystem_sector_size, &adapter);
	if (result == EXFAT_RESIZE_SUCCESS)
		result = exfat_resize_read_boot_regions(
		    &adapter.device, buffer, EXFAT_RESIZE_MAX_SECTOR_SIZE, &geometry);
	if (result == EXFAT_RESIZE_SUCCESS) {
		target_sector_count = target_size / filesystem_sector_size;
		*effective_target_size = target_sector_count * (uint64_t)filesystem_sector_size;
		if (target_sector_count <= geometry.volume_sector_count) {
			result = EXFAT_RESIZE_INSUFFICIENT_GROWTH;
		} else {
			target_device_geometry.logical_sector_size = filesystem_sector_size;
			target_device_geometry.sector_count = target_sector_count;
			result = exfat_resize_plan_growth(
			    &target_device_geometry, &geometry, target_sector_count, &target_geometry);
		}
	}
	free(buffer);
	return result;
}

static const char *device_error_separator(const char *path)
{
#if defined(_WIN32)
	size_t length = strlen(path);

	if (length != 0 && path[length - 1] == ':')
		return " ";
#else
	(void)path;
#endif
	return ": ";
}

static void print_device_io_error(const struct device *device, const char *path)
{
	char error[256];

	device_format_io_error(device, error, sizeof(error));
	fprintf(stderr, "exfat-resize: %s %s%s%s\n", device->io_error_operation, path,
	    device_error_separator(path), error);
}

static void print_partition_grown_guidance(void)
{
	fprintf(stderr,
	    "exfat-resize: the partition was enlarged, but the filesystem remains unchanged; "
	    "verify the partition size before retrying and do not shrink it\n");
}

static void print_partition_update_uncertain_guidance(void)
{
	fprintf(stderr,
	    "exfat-resize: a partition update was attempted, but its result is uncertain; "
	    "verify the partition layout and do not retry or shrink it\n");
}

static int cancellation_requested(const struct cli_cancellation *cancellation)
{
	return cancellation != NULL && cancellation->requested != NULL &&
	    cancellation->requested(cancellation->context) != 0;
}

struct cli_monitor_context {
	const struct cli_cancellation *cancellation;
	const char *path;
};

static int monitor_cancellation_requested(void *opaque)
{
	const struct cli_monitor_context *context = opaque;

	return cancellation_requested(context->cancellation);
}

static void report_resize_event(void *opaque, const struct exfat_resize_event *event)
{
	const struct cli_monitor_context *context = opaque;

	if (event->code == EXFAT_RESIZE_EVENT_CODE_STAGE_ENTERED) {
		switch (event->values[0]) {
		case EXFAT_RESIZE_STAGE_PREFLIGHT:
			printf("exfat-resize: checking filesystem\n");
			goto flush;
		case EXFAT_RESIZE_STAGE_PREPARING:
			printf("exfat-resize: preparing resize\n");
			goto flush;
		case EXFAT_RESIZE_STAGE_RESIZING:
			printf("exfat-resize: resizing filesystem\n");
			goto flush;
		case EXFAT_RESIZE_STAGE_FINALIZING:
			printf("exfat-resize: finalizing resize\n");
			goto flush;
		case EXFAT_RESIZE_STAGE_COMPLETED:
			printf(
			    "exfat-resize: resized %s to %" PRIu64 " bytes\n", context->path, event->values[1]);
			goto flush;
		}
	}
	printf("exfat-resize: event level=%d code=%" PRIu32 " values=[%" PRIu64 ", %" PRIu64
	       ", %" PRIu64 "]\n",
	    (int)event->level, event->code, event->values[0], event->values[1], event->values[2]);

flush:
	(void)fflush(stdout);
}

static void print_recovery_guidance(
    enum exfat_resize_stage stage, enum device_partition_state partition_state)
{
	switch (stage) {
	case EXFAT_RESIZE_STAGE_PREFLIGHT:
		print_no_write_guidance();
		if (partition_state == DEVICE_PARTITION_GROWN)
			print_partition_grown_guidance();
		break;
	case EXFAT_RESIZE_STAGE_PREPARING:
		fprintf(stderr,
		    "exfat-resize: the source filesystem remains authoritative; run a "
		    "filesystem checker before retrying\n");
		break;
	case EXFAT_RESIZE_STAGE_RESIZING:
		fprintf(stderr,
		    "exfat-resize: the filesystem may be incomplete; restore the verified backup\n");
		break;
	case EXFAT_RESIZE_STAGE_FINALIZING:
		fprintf(stderr,
		    "exfat-resize: the resize completed, but its dirty state is uncertain; run a "
		    "filesystem checker and do not retry the resize\n");
		break;
	case EXFAT_RESIZE_STAGE_COMPLETED:
		break;
	}
}

static void print_resize_failure(const struct device *device,
    const char *path,
    enum exfat_resize_error result,
    enum exfat_resize_stage stage,
    enum device_partition_state partition_state)
{
	if (result == EXFAT_RESIZE_CANCELLED)
		fprintf(stderr, "exfat-resize: interrupted by user\n");
	else
		fprintf(stderr, "exfat-resize: resize failed: %s\n", resize_error(result));
	if (result == EXFAT_RESIZE_IO_ERROR && device->io_error_operation != NULL)
		print_device_io_error(device, path);
	print_recovery_guidance(stage, partition_state);
}

int cli_main(int argc, char **argv, const struct cli_cancellation *cancellation)
{
	struct exfat_resize_allocator allocator;
	struct exfat_resize_monitor monitor;
	struct cli_monitor_context monitor_context;
	struct device device;
	enum device_partition_state partition_state = DEVICE_PARTITION_UNCHANGED;
	enum exfat_resize_error result;
	enum exfat_resize_stage stage = EXFAT_RESIZE_STAGE_PREFLIGHT;
	const char *positional[2];
	uint64_t target = 0;
	char error[512];
	int positional_count = 0;
	int parse_options = 1;
	int grow_partition = 0;
	int status = EXIT_FAILURE;
	int index;

	for (index = 1; index < argc; ++index) {
		if (parse_options && strcmp(argv[index], "--") == 0) {
			parse_options = 0;
			continue;
		}
		if (parse_options && strcmp(argv[index], "--help") == 0) {
			print_help();
			return EXIT_SUCCESS;
		}
		if (parse_options && strcmp(argv[index], "--version") == 0) {
			printf("exfat-resize %s\n", EXFAT_RESIZE_BUILD_VERSION);
			return EXIT_SUCCESS;
		}
		if (parse_options && strcmp(argv[index], "--grow-partition") == 0) {
			grow_partition = 1;
			continue;
		}
		if (parse_options && argv[index][0] == '-' && argv[index][1] != '\0' &&
		    argv[index][1] != '-') {
			if (argv[index][1] == 'h') {
				print_help();
				return EXIT_SUCCESS;
			}
			if (argv[index][1] == 'V') {
				printf("exfat-resize %s\n", EXFAT_RESIZE_BUILD_VERSION);
				return EXIT_SUCCESS;
			}
			print_usage(stderr);
			print_no_write_guidance();
			return EXIT_FAILURE;
		}
		if (parse_options && argv[index][0] == '-' && argv[index][1] != '\0') {
			print_usage(stderr);
			print_no_write_guidance();
			return EXIT_FAILURE;
		}
		if (positional_count == 2) {
			print_usage(stderr);
			print_no_write_guidance();
			return EXIT_FAILURE;
		}
		positional[positional_count++] = argv[index];
	}
	if (positional_count < 1) {
		print_usage(stderr);
		print_no_write_guidance();
		return EXIT_FAILURE;
	}
	if (positional_count == 2 && parse_size(positional[1], &target) != 0) {
		fprintf(stderr, "exfat-resize: invalid size: %s\n", positional[1]);
		print_no_write_guidance();
		return EXIT_FAILURE;
	}
	if (grow_partition && positional_count != 2) {
		fprintf(stderr, "exfat-resize: --grow-partition requires an explicit SIZE\n");
		print_no_write_guidance();
		return EXIT_FAILURE;
	}
#if !defined(_WIN32)
	if (grow_partition) {
		fprintf(stderr,
		    "exfat-resize: --grow-partition is supported only for logical Windows volumes\n");
		print_no_write_guidance();
		return EXIT_FAILURE;
	}
#endif

	if (cancellation_requested(cancellation)) {
		fprintf(stderr, "exfat-resize: interrupted by user\n");
		print_no_write_guidance();
		return CLI_CANCELLED_EXIT_STATUS;
	}

	device_init(&device);
	if (device_open(&device, positional[0], error, sizeof(error)) != 0) {
		fprintf(stderr, "exfat-resize: %s\n", error);
		print_no_write_guidance();
		return EXIT_FAILURE;
	}
	if (positional_count != 2)
		target = device.block_device.sector_count * (uint64_t)device.block_device.sector_size;
	if (grow_partition) {
		uint64_t current_size;

		if (device.block_device.sector_count >
		    UINT64_MAX / (uint64_t)device.block_device.sector_size) {
			fprintf(stderr, "exfat-resize: logical volume size is too large\n");
			print_no_write_guidance();
			goto out;
		}
		current_size = device.block_device.sector_count * (uint64_t)device.block_device.sector_size;
		/*
		 * The library's complete preflight cannot run until a requested target
		 * beyond the current device fits. Validate the source and planned geometry
		 * first, and use its filesystem-sector-rounded target for every subsequent
		 * decision.
		 */
		result = validate_partition_growth_preflight(&device.block_device, target, &target);
		if (result != EXFAT_RESIZE_SUCCESS) {
			fprintf(stderr, "exfat-resize: cannot grow partition: preflight failed: %s\n",
			    resize_error(result));
			if (result == EXFAT_RESIZE_IO_ERROR && device.io_error_operation != NULL)
				print_device_io_error(&device, positional[0]);
			print_no_write_guidance();
			goto out;
		}
		if (target > current_size &&
		    device_grow_partition(
		        &device, positional[0], target, &partition_state, error, sizeof(error)) != 0) {
			fprintf(stderr, "exfat-resize: %s\n", error);
			if (partition_state == DEVICE_PARTITION_GROWN)
				print_partition_grown_guidance();
			else if (partition_state == DEVICE_PARTITION_UPDATE_ATTEMPTED)
				print_partition_update_uncertain_guidance();
			else
				print_no_write_guidance();
			if (partition_state != DEVICE_PARTITION_UNCHANGED &&
			    device_dismount(&device, positional[0], error, sizeof(error)) != 0) {
				fprintf(stderr, "exfat-resize: %s\n", error);
				fprintf(stderr,
				    "exfat-resize: the volume may remain mounted; prevent access until the "
				    "partition layout is verified\n");
			}
			goto out;
		}
		if (partition_state == DEVICE_PARTITION_GROWN) {
			printf("exfat-resize: grew the partition containing %s to %" PRIu64 " bytes\n",
			    positional[0],
			    device.block_device.sector_count * (uint64_t)device.block_device.sector_size);
		}
	}
	allocator.context = NULL;
	allocator.allocate = allocate_memory;
	allocator.deallocate = deallocate_memory;

	monitor_context.cancellation = cancellation;
	monitor_context.path = positional[0];
	monitor.context = &monitor_context;
	monitor.cancellation_requested = cancellation != NULL && cancellation->requested != NULL
	    ? monitor_cancellation_requested
	    : NULL;
	monitor.report_event = report_resize_event;
	if (cancellation_requested(cancellation)) {
		result = EXFAT_RESIZE_CANCELLED;
		status = CLI_CANCELLED_EXIT_STATUS;
		print_resize_failure(&device, positional[0], result, stage, partition_state);
		goto dismount;
	}

	result = exfat_resize(&device.block_device, target, &allocator, &monitor, &stage);
	if (result != EXFAT_RESIZE_SUCCESS) {
		print_resize_failure(&device, positional[0], result, stage, partition_state);
		if (result == EXFAT_RESIZE_CANCELLED)
			status = CLI_CANCELLED_EXIT_STATUS;
	}

dismount:
	if (device_dismount(&device, positional[0], error, sizeof(error)) != 0) {
		fprintf(stderr, "exfat-resize: %s\n", error);
		if (result == EXFAT_RESIZE_SUCCESS)
			fprintf(stderr,
			    "exfat-resize: the resize completed and was synchronized; remount the "
			    "volume and run a filesystem checker; do not retry the resize\n");
		goto out;
	}
	if (result != EXFAT_RESIZE_SUCCESS)
		goto out;
	status = EXIT_SUCCESS;

out:
	device_close(&device);
	return status;
}
