/* SPDX-License-Identifier: MIT */

#ifndef EXFAT_RESIZE_CHECKED_MATH_H
#define EXFAT_RESIZE_CHECKED_MATH_H

#include "exfat_resize.h"

#include <stdint.h>

enum exfat_resize_error exfat_resize_checked_ceil_divide_u64(
    uint64_t dividend, uint64_t divisor, uint64_t *result);

#endif
