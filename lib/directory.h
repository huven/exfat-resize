/* SPDX-License-Identifier: MIT */

#ifndef EXFAT_RESIZE_DIRECTORY_H
#define EXFAT_RESIZE_DIRECTORY_H

#include "exfat_resize.h"
#include "stream.h"

struct resize_context;

#define EXFAT_MAX_DIRECTORY_SIZE (UINT64_C(256) * 1024 * 1024)

struct directory_location {
	uint64_t sector;
	size_t offset;
};

struct directory_worklist {
	struct allocation_stream *items;
	size_t count;
	size_t capacity;
};

enum directory_scan_mode { DIRECTORY_SCAN_VALIDATE, DIRECTORY_SCAN_REWRITE };

enum exfat_resize_error exfat_resize_rewrite_identity_bitmap_entry(struct resize_context *context);

enum exfat_resize_error exfat_resize_scan_directory_tree(struct resize_context *context,
    const struct allocation_stream *root,
    enum directory_scan_mode mode);

#endif
