/* SPDX-License-Identifier: MIT */

#ifndef EXFAT_RESIZE_SECTOR_ADAPTER_H
#define EXFAT_RESIZE_SECTOR_ADAPTER_H

#include "exfat_resize.h"

#include <stdint.h>

struct exfat_resize_sector_adapter {
	const struct exfat_resize_block_device *source;
	uint32_t device_sectors_per_filesystem_sector;
	struct exfat_resize_block_device device;
};

/* source must already have passed exfat_resize_validate_block_device(). */
enum exfat_resize_error exfat_resize_adapt_block_device(
    const struct exfat_resize_block_device *source,
    uint32_t filesystem_sector_size,
    struct exfat_resize_sector_adapter *adapter);

#endif
