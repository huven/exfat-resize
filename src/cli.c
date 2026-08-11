/* SPDX-License-Identifier: MIT */

#include "cli.h"

#include "device.h"
#include "exfat_resize.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef EXFAT_RESIZE_BUILD_VERSION
#error "EXFAT_RESIZE_BUILD_VERSION must be provided by the build system"
#endif

#if defined(_WIN32)
static const char target_name[] = "IMAGE";
static const char introduction[] = "Grow an existing exFAT filesystem in a Windows image file.";
static const char target_description[] = "Regular image file with the exFAT main boot sector\n"
                                         "                     at sector zero";
static const char documentation_lead[] = "";
static const char platform_note[] =
    "\nWindows volumes such as E: and volume-GUID paths are not supported yet.\n";
#else
static const char target_name[] = "DEVICE";
static const char introduction[] = "Grow an existing exFAT filesystem.";
static const char target_description[] = "Regular file or raw block device with the exFAT\n"
                                         "                     main boot sector at sector zero";
static const char documentation_lead[] = "  See exfat-resize(8).\n";
static const char platform_note[] = "";
#endif

static void print_usage(FILE *stream)
{
	fprintf(stream, "Usage: exfat-resize %s [SIZE]\n", target_name);
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
	printf("Usage: exfat-resize %s [SIZE]\n"
	       "\n"
	       "%s\n"
	       "\n"
	       "Arguments:\n"
	       "  %-18s %s\n"
	       "  SIZE               Desired filesystem size in bytes\n"
	       "                     (default: all available space)\n"
	       "\n"
	       "Options:\n"
	       "  -h, --help         Show this help message\n"
	       "  -V, --version      Show version information\n"
	       "\n"
	       "Safety:\n"
	       "  Make and verify a backup before using this tool.\n"
	       "\n"
	       "Documentation:\n"
	       "%s"
	       "  Read the safety requirements, supported-filesystem limitations, and\n"
	       "  recovery instructions in README.md distributed with exfat-resize or at:\n"
	       "    https://github.com/huven/exfat-resize#safety\n"
	       "%s",
	    target_name, introduction, target_name, target_description, documentation_lead,
	    platform_note);
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
	uint64_t parsed = 0;

	if (*text == '\0')
		return -1;
	while (*text != '\0') {
		uint64_t digit;

		if (*text < '0' || *text > '9')
			return -1;
		digit = (uint64_t)(*text - '0');
		if (parsed > (UINT64_MAX - digit) / 10)
			return -1;
		parsed = parsed * 10 + digit;
		++text;
	}
	if (parsed == 0)
		return -1;
	*value = parsed;
	return 0;
}

int cli_main(int argc, char **argv)
{
	struct exfat_resize_options options;
	struct device device;
	enum exfat_resize_error result;
	enum exfat_resize_stage stage;
	const char *positional[2];
	uint64_t target = 0;
	char error[512];
	char io_error[256];
	int positional_count = 0;
	int parse_options = 1;
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

	device_init(&device);
	if (device_open(&device, positional[0], error, sizeof(error)) != 0) {
		fprintf(stderr, "exfat-resize: %s\n", error);
		print_no_write_guidance();
		return EXIT_FAILURE;
	}
	if (positional_count != 2)
		target = device.block_device.sector_count * (uint64_t)device.block_device.sector_size;
	options.allocator.context = NULL;
	options.allocator.allocate = allocate_memory;
	options.allocator.deallocate = deallocate_memory;

	result = exfat_resize(&device.block_device, target, &options, &stage);
	if (result != EXFAT_RESIZE_SUCCESS) {
		fprintf(stderr, "exfat-resize: resize failed: %s\n", resize_error(result));
		if (result == EXFAT_RESIZE_IO_ERROR && device.io_error_operation != NULL) {
			device_format_io_error(&device, io_error, sizeof(io_error));
			fprintf(stderr, "exfat-resize: %s %s: %s\n", device.io_error_operation, positional[0],
			    io_error);
		}
		switch (stage) {
		case EXFAT_RESIZE_STAGE_PREFLIGHT:
			print_no_write_guidance();
			break;
		case EXFAT_RESIZE_STAGE_PREPARING:
			fprintf(stderr,
			    "exfat-resize: the source filesystem remains authoritative; run a "
			    "filesystem checker before retrying\n");
			break;
		case EXFAT_RESIZE_STAGE_RESIZING:
			fprintf(stderr,
			    "exfat-resize: the filesystem may be incomplete; restore the verified "
			    "backup\n");
			break;
		case EXFAT_RESIZE_STAGE_FINALIZING:
			fprintf(stderr,
			    "exfat-resize: the resize completed, but its dirty state is uncertain; "
			    "run a filesystem checker and do not retry the resize\n");
			break;
		case EXFAT_RESIZE_STAGE_COMPLETED:
			break;
		}
		goto out;
	}
	printf("exfat-resize: resized %s\n", positional[0]);
	status = EXIT_SUCCESS;

out:
	device_close(&device);
	return status;
}
