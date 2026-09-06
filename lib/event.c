/* SPDX-License-Identifier: MIT */

#include "event.h"

void exfat_resize_report_event(const struct exfat_resize_monitor *monitor,
    enum exfat_resize_event_level level,
    uint32_t code,
    uint64_t value0,
    uint64_t value1,
    uint64_t value2)
{
	struct exfat_resize_event event = { 0 };

	if (monitor == NULL || monitor->report_event == NULL)
		return;
	event.level = level;
	event.code = code;
	event.values[0] = value0;
	event.values[1] = value1;
	event.values[2] = value2;
	monitor->report_event(monitor->context, &event);
}
