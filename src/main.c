/* SPDX-License-Identifier: MIT */

#define _POSIX_C_SOURCE 200809L

#include "device.h"
#include "exfat_resize.h"
#include "version.h"

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void print_usage(FILE *stream)
{
	fprintf(stream, "Usage: exfat-resize DEVICE [SIZE]\n");
}

static void print_help(void)
{
	printf("Usage: exfat-resize DEVICE [SIZE]\n"
	       "\n"
	       "Grow an existing exFAT filesystem.\n"
	       "\n"
	       "Arguments:\n"
	       "  DEVICE             Regular file or raw block device with the exFAT\n"
	       "                     main boot sector at sector zero\n"
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
	       "  Read the safety requirements, supported-filesystem limitations, and\n"
	       "  recovery instructions in README.md distributed with exfat-resize or at:\n"
	       "    https://github.com/huven/exfat-resize#safety\n");
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
	char *end;
	uintmax_t parsed;

	if (*text == '\0' || *text == '-' || *text == '+')
		return -1;
	errno = 0;
	parsed = strtoumax(text, &end, 10);
	if (errno == ERANGE || *end != '\0' || parsed == 0 || parsed > UINT64_MAX)
		return -1;
	*value = (uint64_t)parsed;
	return 0;
}

int main(int argc, char **argv)
{
	static const struct option long_options[] = {
		{ "help", no_argument, NULL, 'h' },
		{ "version", no_argument, NULL, 'V' },
		{ NULL, 0, NULL, 0 },
	};
	struct exfat_resize_options options;
	struct device device = { .fd = -1 };
	enum exfat_resize_error result;
	enum exfat_resize_stage stage;
	uint64_t target = 0;
	char error[512];
	int option;
	int status = EXIT_FAILURE;

	opterr = 0;
	while ((option = getopt_long(argc, argv, "hV", long_options, NULL)) != -1) {
		switch (option) {
		case 'h':
			print_help();
			return EXIT_SUCCESS;
		case 'V':
			printf("exfat-resize %s\n", EXFAT_RESIZE_VERSION);
			return EXIT_SUCCESS;
		default:
			print_usage(stderr);
			return EXIT_FAILURE;
		}
	}
	argc -= optind;
	argv += optind;
	if (argc < 1 || argc > 2) {
		print_usage(stderr);
		return EXIT_FAILURE;
	}
	if (argc == 2 && parse_size(argv[1], &target) != 0) {
		fprintf(stderr, "exfat-resize: invalid size: %s\n", argv[1]);
		return EXIT_FAILURE;
	}
	if (device_open(&device, argv[0], error, sizeof(error)) != 0) {
		fprintf(stderr, "exfat-resize: %s\n", error);
		return EXIT_FAILURE;
	}
	if (argc != 2) {
		target = device.block_device.sector_count * (uint64_t)device.block_device.sector_size;
	}
	options.allocator.context = NULL;
	options.allocator.allocate = allocate_memory;
	options.allocator.deallocate = deallocate_memory;

	result = exfat_resize(&device.block_device, target, &options, &stage);
	if (result != EXFAT_RESIZE_SUCCESS) {
		fprintf(stderr, "exfat-resize: resize failed: %s\n", resize_error(result));
		if (result == EXFAT_RESIZE_IO_ERROR && device.io_error_operation != NULL) {
			fprintf(stderr, "exfat-resize: %s %s: %s\n", device.io_error_operation, argv[0],
			    strerror(device.io_error_number));
		}
		switch (stage) {
		case EXFAT_RESIZE_STAGE_PREFLIGHT:
			fprintf(stderr,
			    "exfat-resize: the filesystem was not modified; correct the error and "
			    "retry\n");
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
	printf("exfat-resize: resized %s\n", argv[0]);
	status = EXIT_SUCCESS;

out:
	device_close(&device);
	return status;
}
