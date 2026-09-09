/* SPDX-License-Identifier: MIT */

#ifndef EXFAT_RESIZE_EVENT_H
#define EXFAT_RESIZE_EVENT_H

#include "exfat_resize.h"

#include <stdint.h>

void exfat_resize_report_event(const struct exfat_resize_monitor *monitor,
    enum exfat_resize_event_level level,
    uint32_t code,
    uint64_t value0,
    uint64_t value1,
    uint64_t value2);

#endif
