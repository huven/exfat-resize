/* SPDX-License-Identifier: MIT */

#ifndef EXFAT_RESIZE_ENDIAN_H
#define EXFAT_RESIZE_ENDIAN_H

#include "exfat_resize.h"

#include <stddef.h>
#include <stdint.h>

enum exfat_resize_error exfat_resize_load_le16(
    const unsigned char *buffer, size_t buffer_size, size_t offset, uint16_t *value);

enum exfat_resize_error exfat_resize_load_le32(
    const unsigned char *buffer, size_t buffer_size, size_t offset, uint32_t *value);

enum exfat_resize_error exfat_resize_load_le64(
    const unsigned char *buffer, size_t buffer_size, size_t offset, uint64_t *value);

enum exfat_resize_error exfat_resize_store_le16(
    unsigned char *buffer, size_t buffer_size, size_t offset, uint16_t value);

enum exfat_resize_error exfat_resize_store_le32(
    unsigned char *buffer, size_t buffer_size, size_t offset, uint32_t value);

enum exfat_resize_error exfat_resize_store_le64(
    unsigned char *buffer, size_t buffer_size, size_t offset, uint64_t value);

#endif
