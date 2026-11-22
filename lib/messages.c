/* SPDX-License-Identifier: MIT */

#include "exfat_resize.h"

#include <inttypes.h>
#include <stdio.h>

const char *exfat_resize_error_string(enum exfat_resize_error error)
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
	case EXFAT_RESIZE_INSUFFICIENT_SHRINK_SPACE:
		return "target cannot hold current allocations, including the full allocation bitmap";
	}
	return "unknown error";
}

static const char *chain_reason(uint64_t reason)
{
	switch (reason) {
	case EXFAT_RESIZE_CHAIN_EARLY_END:
		return "ends before its declared length";
	case EXFAT_RESIZE_CHAIN_INVALID_LINK:
		return "contains an invalid link";
	case EXFAT_RESIZE_CHAIN_MISSING_END:
		return "has no end marker at its declared end";
	case EXFAT_RESIZE_CHAIN_LENGTH_LIMIT:
		return "exceeds its traversal limit";
	}
	return NULL;
}

static const char *stream_reason(uint64_t reason)
{
	switch (reason) {
	case EXFAT_RESIZE_STREAM_EMPTY_WITH_CLUSTER:
		return "empty allocation has a nonzero first cluster";
	case EXFAT_RESIZE_STREAM_INVALID_FIRST_CLUSTER:
		return "first cluster is outside the cluster heap";
	case EXFAT_RESIZE_STREAM_CONTIGUOUS_RANGE:
		return "contiguous allocation extends outside the cluster heap";
	case EXFAT_RESIZE_STREAM_LENGTH_EXCEEDS_VOLUME:
		return "allocation length exceeds the cluster heap";
	}
	return NULL;
}

static const char *directory_reason(uint64_t reason)
{
	switch (reason) {
	case EXFAT_RESIZE_DIRECTORY_CHECKSUM:
		return "entry-set checksum does not match";
	case EXFAT_RESIZE_DIRECTORY_SECONDARY_COUNT:
		return "too few secondary entries";
	case EXFAT_RESIZE_DIRECTORY_TRUNCATED_SET:
		return "entry or entry set is truncated";
	case EXFAT_RESIZE_DIRECTORY_ENTRY_ORDER:
		return "invalid secondary-entry order or type";
	case EXFAT_RESIZE_DIRECTORY_ENTRY_FLAGS:
		return "invalid allocation flags";
	case EXFAT_RESIZE_DIRECTORY_VALID_LENGTH:
		return "valid data length exceeds data length";
	case EXFAT_RESIZE_DIRECTORY_LENGTH_MISMATCH:
		return "directory valid data length differs from data length";
	case EXFAT_RESIZE_DIRECTORY_TOO_LARGE:
		return "directory exceeds the maximum size";
	case EXFAT_RESIZE_DIRECTORY_EMPTY_NO_FAT_CHAIN:
		return "empty stream has NoFatChain set";
	case EXFAT_RESIZE_DIRECTORY_MISPLACED_SYSTEM_ENTRY:
		return "system entry occurs outside the root directory";
	case EXFAT_RESIZE_DIRECTORY_DUPLICATE_BITMAP:
		return "more than one allocation bitmap entry";
	case EXFAT_RESIZE_DIRECTORY_SYSTEM_ALLOCATION:
		return "invalid system-metadata allocation";
	case EXFAT_RESIZE_DIRECTORY_VOLUME_GUID:
		return "invalid Volume GUID entry";
	case EXFAT_RESIZE_DIRECTORY_UNEXPECTED_ENTRY:
		return "unexpected directory-entry type";
	case EXFAT_RESIZE_DIRECTORY_ALLOCATION_CHANGED:
		return "allocation differs from the validated reference";
	}
	return NULL;
}

void exfat_resize_format_event(
    const struct exfat_resize_event *event, char *buffer, size_t capacity)
{
	const char *reason;

	buffer[0] = '\0';
	switch (event->code) {
	case EXFAT_RESIZE_EVENT_CODE_STAGE_ENTERED:
		switch (event->values[0]) {
		case EXFAT_RESIZE_STAGE_PREFLIGHT:
			(void)snprintf(buffer, capacity, "checking filesystem");
			return;
		case EXFAT_RESIZE_STAGE_PREPARING:
			(void)snprintf(buffer, capacity, "preparing resize");
			return;
		case EXFAT_RESIZE_STAGE_RESIZING:
			(void)snprintf(buffer, capacity, "resizing filesystem");
			return;
		case EXFAT_RESIZE_STAGE_FINALIZING:
			(void)snprintf(buffer, capacity, "finalizing resize");
			return;
		case EXFAT_RESIZE_STAGE_COMPLETED:
			(void)snprintf(
			    buffer, capacity, "resized filesystem to %" PRIu64 " bytes", event->values[1]);
			return;
		case EXFAT_RESIZE_STAGE_SOURCE_READY:
			(void)snprintf(buffer, capacity,
			    "cancelled; clean filesystem remains at %" PRIu64 " bytes", event->values[1]);
			return;
		}
		break;
	case EXFAT_RESIZE_EVENT_CODE_UNREFERENCED_CLUSTER:
		(void)snprintf(buffer, capacity,
		    "cluster %" PRIu64 " is allocated in the bitmap but has no recognized owner "
		    "(FAT=0x%08" PRIx64 ")",
		    event->values[0], event->values[1]);
		return;
	case EXFAT_RESIZE_EVENT_CODE_REFERENCED_CLUSTER_MARKED_FREE:
		(void)snprintf(buffer, capacity,
		    "cluster %" PRIu64 " is referenced but marked free in the bitmap", event->values[0]);
		return;
	case EXFAT_RESIZE_EVENT_CODE_BAD_CLUSTER_MARKED_FREE:
		(void)snprintf(buffer, capacity,
		    "FAT marks cluster %" PRIu64 " bad but the bitmap marks it free", event->values[0]);
		return;
	case EXFAT_RESIZE_EVENT_CODE_CLUSTER_ALREADY_REFERENCED:
		(void)snprintf(buffer, capacity,
		    "cluster %" PRIu64 " is referenced more than once (stream starts at cluster %" PRIu64
		    ", index %" PRIu64 ")",
		    event->values[0], event->values[1], event->values[2]);
		return;
	case EXFAT_RESIZE_EVENT_CODE_FAT_CHAIN_INVALID:
		reason = chain_reason(event->values[2]);
		if (reason == NULL)
			break;
		(void)snprintf(buffer, capacity,
		    "FAT chain %s at cluster %" PRIu64 " (next=0x%08" PRIx64 ")", reason, event->values[0],
		    event->values[1]);
		return;
	case EXFAT_RESIZE_EVENT_CODE_ALLOCATION_STREAM_INVALID:
		reason = stream_reason(event->values[2]);
		if (reason == NULL)
			break;
		(void)snprintf(buffer, capacity,
		    "invalid allocation starting at cluster %" PRIu64 " (%" PRIu64 " bytes): %s",
		    event->values[0], event->values[1], reason);
		return;
	case EXFAT_RESIZE_EVENT_CODE_DIRECTORY_ENTRY_INVALID:
		reason = directory_reason(event->values[2]);
		if (reason == NULL)
			break;
		(void)snprintf(buffer, capacity,
		    "invalid directory entry at byte offset %" PRIu64
		    " in directory starting at cluster %" PRIu64 ": %s",
		    event->values[1], event->values[0], reason);
		return;
	case EXFAT_RESIZE_EVENT_CODE_DIRECTORY_ENTRY_UNSUPPORTED:
		(void)snprintf(buffer, capacity,
		    "unsupported directory entry type 0x%02" PRIx64 " at byte offset %" PRIu64
		    " in directory starting at cluster %" PRIu64,
		    event->values[2], event->values[1], event->values[0]);
		return;
	case EXFAT_RESIZE_EVENT_CODE_BITMAP_MISSING:
		(void)snprintf(buffer, capacity,
		    "allocation bitmap entry is missing from root directory "
		    "starting at cluster %" PRIu64,
		    event->values[0]);
		return;
	case EXFAT_RESIZE_EVENT_CODE_BITMAP_TOO_SHORT:
		(void)snprintf(buffer, capacity,
		    "allocation bitmap at cluster %" PRIu64 " is too short: %" PRIu64
		    " bytes declared, %" PRIu64 " bytes required",
		    event->values[0], event->values[1], event->values[2]);
		return;
	case EXFAT_RESIZE_EVENT_CODE_RESERVED_FAT_ENTRY_INVALID:
		(void)snprintf(buffer, capacity,
		    "reserved FAT entry %" PRIu64 " has an invalid value (0x%08" PRIx64 ")",
		    event->values[0], event->values[1]);
		return;
	}
	(void)snprintf(buffer, capacity,
	    "unknown event code=%" PRIu32 " values=[%" PRIu64 ", %" PRIu64 ", %" PRIu64 "]",
	    event->code, event->values[0], event->values[1], event->values[2]);
}
