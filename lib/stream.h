/* SPDX-License-Identifier: MIT */

#ifndef EXFAT_RESIZE_STREAM_H
#define EXFAT_RESIZE_STREAM_H

#include "exfat_resize.h"
#include "geometry.h"
#include "volume.h"

struct resize_context;

enum stream_chain_source { STREAM_CHAIN_SOURCE_FAT, STREAM_CHAIN_TARGET_MODEL };

struct allocation_stream {
	uint32_t first_cluster;
	uint64_t data_length;
	int no_fat_chain;
	int root_directory;
};

struct stream_cursor {
	const struct exfat_resize_geometry *geometry;
	enum sector_cache_index data_cache;
	enum stream_chain_source chain_source;
	uint32_t current_cluster;
	uint32_t traversed_clusters;
	uint64_t cluster_offset;
	uint64_t remaining_bytes;
	int no_fat_chain;
	int root_directory;
	int exhausted;
};

int exfat_resize_cluster_is_valid(const struct exfat_resize_geometry *geometry, uint32_t cluster);

enum exfat_resize_error exfat_resize_stream_cluster_count(const struct resize_context *context,
    const struct allocation_stream *stream,
    uint32_t *cluster_count);

enum exfat_resize_error exfat_resize_initialize_stream_cursor(
    const struct exfat_resize_geometry *geometry,
    const struct allocation_stream *stream,
    enum sector_cache_index data_cache,
    enum stream_chain_source chain_source,
    struct stream_cursor *cursor);

enum exfat_resize_error exfat_resize_read_stream(
    struct resize_context *context, struct stream_cursor *cursor, void *buffer, size_t count);

#endif
