/* SPDX-License-Identifier: MIT */

#ifndef EXFAT_RESIZE_H
#define EXFAT_RESIZE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Library-owned namespace
 *
 * By contract with this library, applications must not introduce file-scope C
 * identifiers or macro names beginning with exfat_resize_ or EXFAT_RESIZE_.
 * Only functions, types, and constants documented by this header are public
 * API; other identifiers in these namespaces may change or disappear without
 * notice.
 */

/*
 * Before calling exfat_resize(), read the operational safety requirements and
 * supported filesystem subset in README.md, distributed with the library and
 * available at:
 * https://github.com/huven/exfat-resize#safety
 */

enum exfat_resize_error {
	/* The requested operation completed successfully. */
	EXFAT_RESIZE_SUCCESS = 0,
	/* A public function argument or requested target is invalid. */
	EXFAT_RESIZE_INVALID_ARGUMENT = 1,
	/* The block-device geometry or callback table is invalid. */
	EXFAT_RESIZE_INVALID_DEVICE = 2,
	/* The on-disk filesystem structure is malformed or inconsistent. */
	EXFAT_RESIZE_INVALID_FILESYSTEM = 3,
	/* A requested sector or target lies outside the backing device. */
	EXFAT_RESIZE_OUT_OF_BOUNDS = 4,
	/* A required size or offset calculation overflowed. */
	EXFAT_RESIZE_ARITHMETIC_OVERFLOW = 5,
	/* A block-device callback reported failure. */
	EXFAT_RESIZE_IO_ERROR = 6,
	/* A work area is too small for the requested operation. */
	EXFAT_RESIZE_INSUFFICIENT_WORKSPACE = 7,
	/* An internal invariant was unexpectedly violated. */
	EXFAT_RESIZE_INTERNAL_ERROR = 8,
	/* The caller's allocator could not satisfy a request. */
	EXFAT_RESIZE_OUT_OF_MEMORY = 9,
	/* The filesystem does not use exFAT revision 1.00. */
	EXFAT_RESIZE_UNSUPPORTED_REVISION = 10,
	/* The filesystem uses more than one FAT. */
	EXFAT_RESIZE_UNSUPPORTED_MULTIPLE_FATS = 11,
	/* The filesystem has its VolumeDirty flag set. */
	EXFAT_RESIZE_VOLUME_DIRTY = 12,
	/* The filesystem has its MediaFailure flag set. */
	EXFAT_RESIZE_MEDIA_FAILURE = 13,
	/* The target does not add enough clusters for the resize. */
	EXFAT_RESIZE_INSUFFICIENT_GROWTH = 14,
	/* The filesystem already has the maximum exFAT cluster count. */
	EXFAT_RESIZE_CLUSTER_LIMIT_REACHED = 15,
	/* A Vendor Allocation directory entry is present. */
	EXFAT_RESIZE_UNSUPPORTED_VENDOR_ALLOCATION = 16,
	/* An unrecognized critical directory entry is present. */
	EXFAT_RESIZE_UNSUPPORTED_CRITICAL_ENTRY = 17,
	/* An unrecognized directory entry references a cluster allocation. */
	EXFAT_RESIZE_UNSUPPORTED_ALLOCATED_ENTRY = 18,
	/* Growth would move or consume storage marked as a bad cluster. */
	EXFAT_RESIZE_BAD_CLUSTER_CONFLICT = 19,
	/* Filesystem sectors cannot be mapped to whole device sectors. */
	EXFAT_RESIZE_UNSUPPORTED_SECTOR_MAPPING = 20
};

/* Recovery boundary reached by exfat_resize(). */
enum exfat_resize_stage {
	/* No write was attempted; correct the error and retry when appropriate. */
	EXFAT_RESIZE_STAGE_PREFLIGHT = 0,
	/* The source remains authoritative; check it before retrying. */
	EXFAT_RESIZE_STAGE_PREPARING = 1,
	/* Source metadata may be overwritten; restore the verified backup. */
	EXFAT_RESIZE_STAGE_RESIZING = 2,
	/* The target is synchronized; check it and do not retry the resize. */
	EXFAT_RESIZE_STAGE_FINALIZING = 3,
	/* The resized target and its clean state were synchronized. */
	EXFAT_RESIZE_STAGE_COMPLETED = 4
};

/*
 * Callback contract
 *
 * These rules apply to block-device and allocator callbacks. Callbacks are
 * synchronous, and the library does not invoke them concurrently within one
 * public call. It passes context unchanged and retains no callback or context
 * pointer after the call returns. Callback tables, context pointers, and
 * callback-owned state must remain valid for the complete call; callback
 * tables and context pointers must not change during it. Callbacks may modify
 * state referenced by context. C++ exceptions must not escape a callback
 * across the C interface.
 *
 * Block-device callbacks
 *
 * read and write are invoked only for a nonzero sector_count and for a range
 * wholly contained in the device. The buffer is valid for at least
 * sector_count * sector_size bytes and must be the only bytes accessed. The
 * buffer is borrowed only for that callback invocation; it must not be retained
 * or accessed after the callback returns and may be reused immediately. Zero
 * reports a complete transfer; later reads must observe completed writes. A
 * nonzero result reports failure, and a failed write may already have modified
 * part of the requested range. sync returns zero only after every preceding
 * completed write is durable on the backing storage.
 *
 * context is the block-device context below. first_sector is the first device
 * sector of the transfer, sector_count is its length, and buffer is its source
 * or destination.
 */
struct exfat_resize_block_device {
	/* Opaque caller value passed unchanged to each device callback. */
	void *context;
	/* Size of one callback-addressable sector; power of two from 512 through 4096 bytes. */
	uint32_t sector_size;
	/* Nonzero number of sectors addressable through the callbacks. */
	uint64_t sector_count;

	/* Reads sector_count sectors starting at first_sector into buffer. */
	int (*read)(void *context, uint64_t first_sector, uint32_t sector_count, void *buffer);
	/* Writes sector_count sectors from buffer starting at first_sector. */
	int (*write)(void *context, uint64_t first_sector, uint32_t sector_count, const void *buffer);
	/* Makes every preceding completed write durable. */
	int (*sync)(void *context);
};

/*
 * Allocator callbacks
 *
 * allocate is called only with a nonzero size. It returns either NULL or at
 * least size bytes of readable and writable, max_align_t-aligned storage whose
 * effective-type semantics match those of storage returned by malloc (ISO/IEC
 * 9899:2011 section 6.5, paragraphs 6 and 7). A successful allocation must be
 * disjoint from the public argument objects, callback-owned state, and every
 * other live allocation made for the call.
 *
 * Each successful allocation remains valid until deallocate is called exactly
 * once with its original pointer and size. deallocate is never called with
 * NULL or a zero size. Every allocation is released before the public call
 * returns. context is the allocator context below, size is the requested or
 * original allocation size, and memory is the pointer returned by allocate.
 */
struct exfat_resize_allocator {
	/* Opaque caller value passed unchanged to each allocator callback. */
	void *context;
	/* Allocates size writable bytes, or returns NULL on failure. */
	void *(*allocate)(void *context, size_t size);
	/* Releases memory using the original allocation size. */
	void (*deallocate)(void *context, void *memory, size_t size);
};

struct exfat_resize_options {
	/* Allocator used for all working memory owned by the call. */
	struct exfat_resize_allocator allocator;
};

/*
 * Grows the exFAT filesystem at sector zero to target_size bytes.
 *
 * The caller must provide exclusive access to the backing device. The device
 * and options objects must remain valid and unchanged until the call returns.
 * The library performs no synchronization between calls. Concurrent or
 * callback-reentrant calls must use different backing devices, and any shared
 * callback state must support that use.
 *
 * Working-memory sizes and lifetimes requested through options->allocator
 * are documented in the "Memory requirements" section of docs/TRANSACTION.md
 * distributed with exfat-resize.
 *
 * If stage is not NULL, it must remain writable until the function returns.
 * It receives the resize stage reached even when the function returns an
 * error. The exFAT filesystem sector size must be a multiple of device
 * sector_size. Growth must add enough clusters for a replacement allocation
 * bitmap.
 *
 * device supplies the sector-addressed backing-device view and callbacks;
 * target_size is the requested filesystem length in bytes and is rounded down
 * to a whole filesystem sector; options supplies working-memory allocation;
 * and stage optionally receives the recovery boundary reached.
 *
 * Returns an exfat_resize_error describing success or the reason for failure.
 */
enum exfat_resize_error exfat_resize(const struct exfat_resize_block_device *device,
    uint64_t target_size,
    const struct exfat_resize_options *options,
    enum exfat_resize_stage *stage);

#ifdef __cplusplus
}
#endif

#endif
