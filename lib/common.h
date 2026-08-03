/* SPDX-License-Identifier: MIT */

#ifndef EXFAT_RESIZE_COMMON_H
#define EXFAT_RESIZE_COMMON_H

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#if CHAR_BIT != 8
#error "exfat-resize requires 8-bit bytes"
#endif

#if !defined(UINT8_MAX) || !defined(UINT16_MAX) || \
    !defined(UINT32_MAX) || !defined(UINT64_MAX)
#error "exfat-resize requires exact-width unsigned 8-, 16-, 32-, and 64-bit integer types"
#endif

#if SIZE_MAX < UINT32_C(1048576)
#error "exfat-resize requires size_t to represent a 1 MiB work buffer"
#endif

#endif
