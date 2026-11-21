/* SPDX-License-Identifier: MIT */

#include "exfat_resize.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failure_count;

#define CHECK(expression) \
	do { \
		if (!(expression)) { \
			fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expression); \
			++failure_count; \
		} \
	} while (0)

static void check_capacity(
    const struct exfat_resize_event *event, const char *complete, size_t capacity)
{
	struct {
		unsigned char before[8];
		char message[256];
		unsigned char after[8];
	} guarded;
	size_t length = strlen(complete);
	size_t copied = capacity - 1;
	size_t index;

	if (copied > length)
		copied = length;
	memset(&guarded, 0xa5, sizeof(guarded));
	exfat_resize_format_event(event, guarded.message, capacity);
	for (index = 0; index < sizeof(guarded.before); ++index)
		CHECK(guarded.before[index] == 0xa5);
	for (index = 0; index < sizeof(guarded.after); ++index)
		CHECK(guarded.after[index] == 0xa5);
	CHECK(memcmp(guarded.message, complete, copied) == 0);
	CHECK(guarded.message[copied] == '\0');
	for (index = capacity; index < sizeof(guarded.message); ++index)
		CHECK((unsigned char)guarded.message[index] == 0xa5);
}

static void check_message(const struct exfat_resize_event *event)
{
	char complete[1024];
	const char *terminator;
	size_t length;

	memset(complete, 0xa5, sizeof(complete));
	exfat_resize_format_event(event, complete, sizeof(complete));
	terminator = memchr(complete, '\0', sizeof(complete));
	CHECK(terminator != NULL);
	if (terminator == NULL)
		return;
	length = (size_t)(terminator - complete);
	CHECK(length != 0);
	CHECK(length < 256);
	CHECK(strchr(complete, '\n') == NULL);
	check_capacity(event, complete, 1);
	check_capacity(event, complete, 7);
	check_capacity(event, complete, 256);
	if (length < 256) {
		check_capacity(event, complete, length);
		check_capacity(event, complete, length + 1);
	}
}

static void test_all_event_messages(void)
{
	static const uint32_t codes[] = {
		EXFAT_RESIZE_EVENT_CODE_UNREFERENCED_CLUSTER,
		EXFAT_RESIZE_EVENT_CODE_REFERENCED_CLUSTER_MARKED_FREE,
		EXFAT_RESIZE_EVENT_CODE_BAD_CLUSTER_MARKED_FREE,
		EXFAT_RESIZE_EVENT_CODE_CLUSTER_ALREADY_REFERENCED,
		EXFAT_RESIZE_EVENT_CODE_BITMAP_MISSING,
		EXFAT_RESIZE_EVENT_CODE_BITMAP_TOO_SHORT,
		EXFAT_RESIZE_EVENT_CODE_RESERVED_FAT_ENTRY_INVALID,
	};
	static const uint32_t chain_reasons[] = {
		EXFAT_RESIZE_CHAIN_EARLY_END,
		EXFAT_RESIZE_CHAIN_INVALID_LINK,
		EXFAT_RESIZE_CHAIN_MISSING_END,
		EXFAT_RESIZE_CHAIN_LENGTH_LIMIT,
	};
	static const uint32_t stream_reasons[] = {
		EXFAT_RESIZE_STREAM_EMPTY_WITH_CLUSTER,
		EXFAT_RESIZE_STREAM_INVALID_FIRST_CLUSTER,
		EXFAT_RESIZE_STREAM_CONTIGUOUS_RANGE,
		EXFAT_RESIZE_STREAM_LENGTH_EXCEEDS_VOLUME,
	};
	static const uint32_t directory_reasons[] = {
		EXFAT_RESIZE_DIRECTORY_CHECKSUM,
		EXFAT_RESIZE_DIRECTORY_SECONDARY_COUNT,
		EXFAT_RESIZE_DIRECTORY_TRUNCATED_SET,
		EXFAT_RESIZE_DIRECTORY_ENTRY_ORDER,
		EXFAT_RESIZE_DIRECTORY_ENTRY_FLAGS,
		EXFAT_RESIZE_DIRECTORY_VALID_LENGTH,
		EXFAT_RESIZE_DIRECTORY_LENGTH_MISMATCH,
		EXFAT_RESIZE_DIRECTORY_TOO_LARGE,
		EXFAT_RESIZE_DIRECTORY_EMPTY_NO_FAT_CHAIN,
		EXFAT_RESIZE_DIRECTORY_MISPLACED_SYSTEM_ENTRY,
		EXFAT_RESIZE_DIRECTORY_DUPLICATE_BITMAP,
		EXFAT_RESIZE_DIRECTORY_SYSTEM_ALLOCATION,
		EXFAT_RESIZE_DIRECTORY_VOLUME_GUID,
		EXFAT_RESIZE_DIRECTORY_UNEXPECTED_ENTRY,
		EXFAT_RESIZE_DIRECTORY_ALLOCATION_CHANGED,
	};
	struct exfat_resize_event event = { .level = EXFAT_RESIZE_EVENT_LEVEL_ERROR,
		.values = { UINT64_MAX, UINT64_MAX, UINT64_MAX } };
	size_t index;

	for (index = 0; index < sizeof(codes) / sizeof(codes[0]); ++index) {
		event.code = codes[index];
		check_message(&event);
	}
	event.code = EXFAT_RESIZE_EVENT_CODE_FAT_CHAIN_INVALID;
	for (index = 0; index < sizeof(chain_reasons) / sizeof(chain_reasons[0]); ++index) {
		event.values[2] = chain_reasons[index];
		check_message(&event);
	}
	event.values[2] = UINT64_MAX;
	check_message(&event);
	event.code = EXFAT_RESIZE_EVENT_CODE_ALLOCATION_STREAM_INVALID;
	for (index = 0; index < sizeof(stream_reasons) / sizeof(stream_reasons[0]); ++index) {
		event.values[2] = stream_reasons[index];
		check_message(&event);
	}
	event.values[2] = UINT64_MAX;
	check_message(&event);
	event.code = EXFAT_RESIZE_EVENT_CODE_DIRECTORY_ENTRY_INVALID;
	for (index = 0; index < sizeof(directory_reasons) / sizeof(directory_reasons[0]); ++index) {
		event.values[2] = directory_reasons[index];
		check_message(&event);
	}
	event.values[2] = UINT64_MAX;
	check_message(&event);
	event.code = EXFAT_RESIZE_EVENT_CODE_DIRECTORY_ENTRY_UNSUPPORTED;
	check_message(&event);
	event.code = EXFAT_RESIZE_EVENT_CODE_STAGE_ENTERED;
	for (index = EXFAT_RESIZE_STAGE_PREFLIGHT; index <= EXFAT_RESIZE_STAGE_SOURCE_READY; ++index) {
		event.values[0] = index;
		check_message(&event);
	}
	event.values[0] = UINT64_MAX;
	check_message(&event);
	event.code = UINT32_MAX;
	check_message(&event);
}

static void test_message_details(void)
{
	struct exfat_resize_event event = { .level = EXFAT_RESIZE_EVENT_LEVEL_ERROR,
		.code = EXFAT_RESIZE_EVENT_CODE_UNREFERENCED_CLUSTER,
		.values = { 100, UINT32_C(0xffffffff), 0 } };
	char message[256];
	char saved[256];

	exfat_resize_format_event(&event, message, sizeof(message));
	CHECK(strstr(message, "100") != NULL);
	CHECK(strstr(message, "0xffffffff") != NULL);
	CHECK(strstr(message, "owner") != NULL);
	memcpy(saved, message, sizeof(saved));
	event.code = EXFAT_RESIZE_EVENT_CODE_DIRECTORY_ENTRY_INVALID;
	event.values[0] = 5528;
	event.values[1] = 96;
	event.values[2] = EXFAT_RESIZE_DIRECTORY_CHECKSUM;
	exfat_resize_format_event(&event, message, sizeof(message));
	CHECK(strstr(message, "5528") != NULL);
	CHECK(strstr(message, "96") != NULL);
	CHECK(strstr(message, "checksum") != NULL);
	CHECK(strstr(message, "byte") != NULL);
	CHECK(strstr(saved, "100") != NULL);
	CHECK(strstr(saved, "owner") != NULL);
	event.code = UINT32_MAX;
	event.values[0] = UINT64_MAX;
	event.values[1] = UINT64_MAX - 1;
	event.values[2] = UINT64_MAX - 2;
	exfat_resize_format_event(&event, message, sizeof(message));
	CHECK(strstr(message, "4294967295") != NULL);
	CHECK(strstr(message, "18446744073709551615") != NULL);
	CHECK(strstr(message, "18446744073709551614") != NULL);
	CHECK(strstr(message, "18446744073709551613") != NULL);
	event.code = EXFAT_RESIZE_EVENT_CODE_DIRECTORY_ENTRY_INVALID;
	exfat_resize_format_event(&event, message, sizeof(message));
	CHECK(strstr(message, "18446744073709551613") != NULL);
}

static void test_error_strings(void)
{
	static const struct {
		enum exfat_resize_error error;
		const char *message;
	} cases[] = {
		{ EXFAT_RESIZE_SUCCESS, "success" },
		{ EXFAT_RESIZE_INVALID_ARGUMENT, "invalid argument" },
		{ EXFAT_RESIZE_INVALID_DEVICE, "invalid block device" },
		{ EXFAT_RESIZE_INVALID_FILESYSTEM, "invalid or corrupt exFAT filesystem" },
		{ EXFAT_RESIZE_OUT_OF_BOUNDS, "request is outside the backing device" },
		{ EXFAT_RESIZE_ARITHMETIC_OVERFLOW, "filesystem geometry is too large" },
		{ EXFAT_RESIZE_IO_ERROR, "device I/O error" },
		{ EXFAT_RESIZE_INSUFFICIENT_WORKSPACE, "working memory is too small" },
		{ EXFAT_RESIZE_INTERNAL_ERROR, "internal error" },
		{ EXFAT_RESIZE_OUT_OF_MEMORY, "cannot allocate working memory" },
		{ EXFAT_RESIZE_UNSUPPORTED_REVISION, "unsupported exFAT revision" },
		{ EXFAT_RESIZE_UNSUPPORTED_MULTIPLE_FATS, "multiple FATs are not supported" },
		{ EXFAT_RESIZE_VOLUME_DIRTY, "filesystem is marked dirty" },
		{ EXFAT_RESIZE_MEDIA_FAILURE, "filesystem has the media-failure flag set" },
		{ EXFAT_RESIZE_INSUFFICIENT_GROWTH, "target does not add enough usable clusters" },
		{ EXFAT_RESIZE_CLUSTER_LIMIT_REACHED, "filesystem has reached the exFAT cluster limit" },
		{ EXFAT_RESIZE_UNSUPPORTED_VENDOR_ALLOCATION,
		    "filesystem contains a Vendor Allocation entry" },
		{ EXFAT_RESIZE_UNSUPPORTED_CRITICAL_ENTRY,
		    "filesystem contains an unsupported critical directory entry" },
		{ EXFAT_RESIZE_UNSUPPORTED_ALLOCATED_ENTRY,
		    "filesystem contains unsupported allocated directory metadata" },
		{ EXFAT_RESIZE_BAD_CLUSTER_CONFLICT, "expanded FAT conflicts with a source bad cluster" },
		{ EXFAT_RESIZE_UNSUPPORTED_SECTOR_MAPPING,
		    "filesystem sector size is incompatible with device sector size" },
		{ EXFAT_RESIZE_CANCELLED, "operation cancelled" },
		{ EXFAT_RESIZE_INSUFFICIENT_SHRINK_SPACE,
		    "target cannot hold current allocations, including the full allocation bitmap" },
	};
	size_t index;
	const char *saved = exfat_resize_error_string(EXFAT_RESIZE_INVALID_FILESYSTEM);

	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index)
		CHECK(strcmp(exfat_resize_error_string(cases[index].error), cases[index].message) == 0);
	CHECK(strcmp(exfat_resize_error_string((enum exfat_resize_error)999), "unknown error") == 0);
	CHECK(strcmp(exfat_resize_error_string((enum exfat_resize_error) - 1), "unknown error") == 0);
	CHECK(strcmp(saved, "invalid or corrupt exFAT filesystem") == 0);
}

int main(void)
{
	test_all_event_messages();
	test_message_details();
	test_error_strings();
	if (failure_count != 0) {
		fprintf(stderr, "%d message test(s) failed\n", failure_count);
		return 1;
	}
	printf("library messages: passed\n");
	return 0;
}
