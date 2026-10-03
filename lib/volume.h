/* SPDX-License-Identifier: MIT */

#ifndef EXFAT_RESIZE_VOLUME_H
#define EXFAT_RESIZE_VOLUME_H

#include "exfat_resize.h"
#include "geometry.h"

struct resize_context;

#define EXFAT_IO_BUFFER_SIZE ((size_t)UINT32_C(1048576))
#define EXFAT_IO_MAX_CHUNK_SIZE ((size_t)UINT32_C(1048576))
#define EXFAT_SECTOR_CACHE_SIZE ((size_t)UINT32_C(262144))

struct sector_cache {
	unsigned char *data;
	uint64_t first_sector;
	uint32_t sector_count;
	uint32_t sector_capacity;
	uint32_t dirty_first;
	uint32_t dirty_end;
};

enum sector_cache_index {
	/* Source directory payload sectors read during preflight validation. */
	SECTOR_CACHE_SOURCE_DIRECTORY_DATA,
	/* Source allocation-bitmap payload sectors read during reconciliation. */
	SECTOR_CACHE_SOURCE_BITMAP_DATA,
	/* Target directory payload sectors read and written during rewrite. */
	SECTOR_CACHE_TARGET_DIRECTORY_DATA,
	/* Number of independently backed sector caches. Must be last! */
	SECTOR_CACHE_COUNT
};

#define EXFAT_SECTOR_CACHE_BUFFER_SIZE ((size_t)SECTOR_CACHE_COUNT * EXFAT_SECTOR_CACHE_SIZE)

/* Writes dirty cached sectors; the caller owns device synchronization. */
enum exfat_resize_error exfat_resize_flush_cache(
    struct resize_context *context, enum sector_cache_index cache_index);

int exfat_resize_cache_contains_sector(const struct sector_cache *cache, uint64_t sector);

/* A cache miss may flush the previous window before reading the new range. */
enum exfat_resize_error exfat_resize_load_cache(struct resize_context *context,
    enum sector_cache_index cache_index,
    uint64_t first_sector,
    uint32_t sector_count);

enum exfat_resize_error exfat_resize_cluster_sector(
    const struct exfat_resize_geometry *geometry, uint32_t cluster, uint64_t *sector);

/* Copies between the context's source and target geometries without synchronizing. */
enum exfat_resize_error exfat_resize_copy_cluster_run(struct resize_context *context,
    uint32_t source_cluster,
    uint32_t target_cluster,
    uint32_t cluster_count);

#endif
