/* SPDX-License-Identifier: MIT */

#ifndef EXFAT_RESIZE_VOLUME_H
#define EXFAT_RESIZE_VOLUME_H

#include "exfat_resize.h"
#include "geometry.h"
#include "sector_adapter.h"

struct resize_operation;

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

/* Buffers are owned here; the operation services are borrowed for this call. */
struct resize_volume {
	struct resize_operation *operation;
	const struct exfat_resize_block_device *device;
	struct exfat_resize_sector_adapter sector_adapter;
	struct exfat_resize_geometry geometry;
	struct sector_cache caches[SECTOR_CACHE_COUNT];
	unsigned char *cache_buffer;
	unsigned char *io_buffer;
	uint32_t io_sector_capacity;
	size_t sector_size;
	uint64_t cluster_size;
};

/* Opens the device view and I/O buffer; geometry is read separately by resize.c. */
enum exfat_resize_error exfat_resize_open_volume(
    struct resize_volume *volume, const struct exfat_resize_block_device *device);
/* Kept separate so the operation controls allocation timing during preflight. */
enum exfat_resize_error exfat_resize_allocate_volume_caches(struct resize_volume *volume);
/* Releases buffers without flushing or issuing any device I/O. */
void exfat_resize_close_volume(struct resize_volume *volume);

/* Writes dirty cached sectors; the caller owns device synchronization. */
enum exfat_resize_error exfat_resize_flush_cache(
    struct resize_volume *volume, enum sector_cache_index cache_index);

int exfat_resize_cache_contains_sector(const struct sector_cache *cache, uint64_t sector);

/* A cache miss may flush the previous window before reading the new range. */
enum exfat_resize_error exfat_resize_load_cache(struct resize_volume *volume,
    enum sector_cache_index cache_index,
    uint64_t first_sector,
    uint32_t sector_count);

enum exfat_resize_error exfat_resize_cluster_sector(
    const struct exfat_resize_geometry *geometry, uint32_t cluster, uint64_t *sector);

/* Copies from the current volume geometry to target without synchronizing. */
enum exfat_resize_error exfat_resize_copy_cluster_run(struct resize_volume *volume,
    const struct exfat_resize_geometry *target,
    uint32_t source_cluster,
    uint32_t target_cluster,
    uint32_t cluster_count);

/* Coherent direct I/O: reads/copies reject overlapping dirty caches. Writes
 * invalidate clean aliases BEFORE attempting I/O, including partial failures.
 * These helpers never flush dirty data or synchronize the device implicitly. */
enum exfat_resize_error exfat_resize_read_volume(struct resize_volume *volume,
    uint64_t first_sector,
    uint32_t sector_count,
    void *buffer,
    size_t buffer_size);
enum exfat_resize_error exfat_resize_write_volume(struct resize_volume *volume,
    uint64_t first_sector,
    uint32_t sector_count,
    const void *buffer,
    size_t buffer_size);
enum exfat_resize_error exfat_resize_invalidate_volume_range(
    struct resize_volume *volume, uint64_t first_sector, uint64_t sector_count);
/* Explicit publication; flush matching windows, without device synchronization. */
enum exfat_resize_error exfat_resize_flush_volume_range(
    struct resize_volume *volume, uint64_t first_sector, uint64_t sector_count);
/* Call before modifying cached bytes. Reject dirty aliases and invalidate clean
 * ones; the selected window must already contain the whole modified range. */
enum exfat_resize_error exfat_resize_prepare_cached_write(struct resize_volume *volume,
    enum sector_cache_index cache_index,
    uint64_t first_sector,
    uint32_t sector_count);

#endif
