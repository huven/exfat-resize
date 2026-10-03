/* SPDX-License-Identifier: MIT */

#ifndef EXFAT_RESIZE_SHRINK_H
#define EXFAT_RESIZE_SHRINK_H

#include "exfat_resize.h"

struct resize_volume;

enum exfat_resize_error exfat_resize_shrink(
    struct resize_volume *volume, uint64_t target_sector_count);

#endif
