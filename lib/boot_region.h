/* SPDX-License-Identifier: MIT */

#ifndef EXFAT_RESIZE_BOOT_REGION_H
#define EXFAT_RESIZE_BOOT_REGION_H

#include "block_device.h"
#include "geometry.h"

#include <stddef.h>
#include <stdint.h>

/*
 * Reads sector zero through the caller's device view and returns the exFAT
 * filesystem sector size encoded in the boot sector.
 */
enum exfat_resize_error exfat_resize_probe_sector_size(
    const struct exfat_resize_block_device *device,
    void *work_buffer,
    size_t work_buffer_size,
    uint32_t *filesystem_sector_size);

/*
 * Reads and validates the main and backup boot regions.
 *
 * The device must already have passed exfat_resize_validate_block_device().
 * work_buffer must hold at least one logical sector. geometry is modified
 * only when both boot regions are valid and consistent.
 */
enum exfat_resize_error exfat_resize_read_boot_regions(
    const struct exfat_resize_block_device *device,
    void *work_buffer,
    size_t work_buffer_size,
    struct exfat_resize_geometry *geometry);

/*
 * Updates the VolumeFlags field in main boot sector zero, then synchronizes
 * the device. Setting the volume dirty also clears the ClearToZero flag.
 *
 * The device and both boot regions must already have been validated.
 * work_buffer must hold at least one logical sector and is overwritten. The
 * caller must select any externally reported transaction stage before calling.
 * Success means the updated sector is durable. On failure, the sector may
 * already have been written without a successful synchronization.
 */
enum exfat_resize_error exfat_resize_set_volume_dirty(
    const struct exfat_resize_block_device *device,
    void *work_buffer,
    size_t work_buffer_size,
    int dirty);

/*
 * Updates the target geometry and PercentInUse in the backup boot sector,
 * rebuilds its checksum sector, and synchronizes the device. It then performs
 * the same updates to the main boot and checksum sectors and synchronizes
 * again. The remaining sectors in both boot regions are preserved.
 *
 * The device and existing boot regions must already have been validated.
 * geometry must describe a valid planned target; used_cluster_count must not
 * exceed its nonzero cluster count. work_buffer must hold at least one logical
 * sector and is overwritten. The caller must select any externally reported
 * transaction stage before calling. Success means both updated regions are
 * durable. On failure, earlier writes may remain, and the most recent writes
 * may not have been synchronized.
 */
enum exfat_resize_error exfat_resize_write_boot_regions(
    const struct exfat_resize_block_device *device,
    const struct exfat_resize_geometry *geometry,
    uint32_t used_cluster_count,
    void *work_buffer,
    size_t work_buffer_size);

#endif
