/* SPDX-License-Identifier: MIT */

#ifndef EXFAT_RESIZE_DIRECTORY_H
#define EXFAT_RESIZE_DIRECTORY_H

#include "exfat_resize.h"
#include "stream.h"

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

struct resize_directory {
	struct resize_volume *volume;
	struct directory_worklist worklist;
	struct directory_location bitmap_location;
};

enum directory_owner_kind {
	DIRECTORY_OWNER_ROOT, /* Boot-region reference; not an editable directory entry. */
	DIRECTORY_OWNER_FILE,
	DIRECTORY_OWNER_BITMAP,
	DIRECTORY_OWNER_UPCASE
};

/* Identity is the containing directory's first cluster at observation time, not
 * its current physical address. Keep it stable for the operation, even if that
 * cluster is freed/reused. entry_offset identifies the primary entry in bytes. */
struct directory_reference {
	uint32_t directory_id;
	uint64_t entry_offset;
	enum directory_owner_kind kind;
};

/* Entries/checksums are validated before visiting. The visitor owns allocation
 * validation; the transform supplies replacement values without doing I/O.
 * Callbacks may not reuse volume->io_buffer or edit directory data recursively.
 * A transform scan may write/evict dirty metadata and cannot grow the worklist. */
struct directory_scan {
	const struct exfat_resize_geometry *geometry;
	struct stream_chain_reader chain;
	void *context;
	enum exfat_resize_error (*visit)(void *context,
	    const struct directory_reference *owner,
	    const struct allocation_stream *stream,
	    int is_directory);
	enum exfat_resize_error (*transform)(void *context,
	    const struct directory_reference *owner,
	    const struct allocation_stream *stream,
	    int is_directory,
	    struct allocation_stream *replacement);
};

struct directory_access {
	const struct exfat_resize_geometry *geometry;
	struct stream_chain_reader chain;
	void *context;
	enum exfat_resize_error (*resolve)(
	    void *context, uint32_t directory_id, struct allocation_stream *directory);
};

/* The caller resolves this location in the current on-disk directory layout. */
enum exfat_resize_error exfat_resize_rewrite_bitmap_entry(struct resize_directory *directory,
    const struct directory_location *location,
    const struct allocation_stream *replacement);
void exfat_resize_release_directory(struct resize_directory *directory);

enum exfat_resize_error exfat_resize_scan_directory_tree(struct resize_directory *directory,
    const struct allocation_stream *root,
    const struct directory_scan *scan);

/* Resolve a stable owner, seek to its logical entry, verify its expected allocation,
 * and update first cluster/NoFatChain/checksum. Only the bitmap may change length;
 * file, directory and upcase lengths must match. ValidDataLength is preserved.
 * No allocation or sync. Cache eviction can publish earlier edits. The operation
 * must defer cancellation across a consistent batch, including flushes and sync,
 * then check at its safe boundary. This helper does not establish that boundary. */
enum exfat_resize_error exfat_resize_edit_directory_allocation(struct resize_directory *directory,
    const struct directory_access *access,
    const struct directory_reference *owner,
    const struct allocation_stream *expected,
    const struct allocation_stream *replacement);

#endif
