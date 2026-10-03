/* SPDX-License-Identifier: MIT */

#ifndef EXFAT_RESIZE_STREAM_H
#define EXFAT_RESIZE_STREAM_H

#include "exfat_resize.h"
#include "geometry.h"
#include "volume.h"

#define EXFAT_FAT_BAD_CLUSTER UINT32_C(0xfffffff7)
#define EXFAT_FAT_END_OF_CHAIN UINT32_C(0xffffffff)

/* Borrowed backend. It returns a FAT entry in this cursor's coordinates, including
 * free/bad markers; traversal validates links. The context must outlive the cursor.
 * Reads must not modify chains during traversal. */
struct stream_chain_reader {
	const void *context;
	enum exfat_resize_error (*next)(const void *context, uint32_t cluster, uint32_t *next);
};

struct allocation_stream {
	uint32_t first_cluster;
	uint64_t data_length;
	int no_fat_chain;
	int root_directory;
};

struct stream_cursor {
	const struct exfat_resize_geometry *geometry;
	enum sector_cache_index data_cache;
	struct stream_chain_reader chain;
	uint32_t current_cluster;
	uint32_t traversed_clusters;
	uint64_t cluster_offset;
	uint64_t remaining_bytes;
	int no_fat_chain;
	int root_directory;
	int exhausted;
};

int exfat_resize_cluster_is_valid(const struct exfat_resize_geometry *geometry, uint32_t cluster);

enum exfat_resize_error exfat_resize_stream_cluster_count(
    uint64_t cluster_size, const struct allocation_stream *stream, uint32_t *cluster_count);

enum exfat_resize_error exfat_resize_initialize_stream_cursor(
    const struct exfat_resize_geometry *geometry,
    const struct allocation_stream *stream,
    enum sector_cache_index data_cache,
    const struct stream_chain_reader *chain,
    struct stream_cursor *cursor);

enum exfat_resize_error exfat_resize_read_stream(
    struct resize_volume *volume, struct stream_cursor *cursor, void *buffer, size_t count);

/* Advances an initialized cursor without reading payload. Offset is relative to its
 * current position. The caller supplies the current geometry and chain backend. */
enum exfat_resize_error exfat_resize_skip_stream(
    struct resize_volume *volume, struct stream_cursor *cursor, uint64_t count);

#endif
