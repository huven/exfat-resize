/* SPDX-License-Identifier: MIT */
#ifndef EXFAT_RESIZE_TEST_SHRINK_COMPACTION_MONITOR_H
#define EXFAT_RESIZE_TEST_SHRINK_COMPACTION_MONITOR_H

#include "exfat_resize.h"

enum exfat_resize_error windows_partition_test_resize(
    const struct exfat_resize_block_device *device,
    uint64_t target_size,
    const struct exfat_resize_allocator *allocator,
    const struct exfat_resize_monitor *monitor,
    enum exfat_resize_stage *stage);

#endif
