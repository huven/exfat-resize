/* SPDX-License-Identifier: MIT */

#ifndef EXFAT_RESIZE_GROW_H
#define EXFAT_RESIZE_GROW_H

#include "exfat_resize.h"

struct resize_volume;

/*
 * The volume geometry and operation callbacks have been initialized. Performs
 * grow preflight and its transaction, releasing all grow-owned allocations on
 * every exit. The caller retains the volume buffers and reported recovery stage.
 */
enum exfat_resize_error exfat_resize_grow(
    struct resize_volume *volume, uint64_t target_sector_count);

#endif
