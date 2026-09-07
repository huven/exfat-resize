/* SPDX-License-Identifier: MIT */

#include <exfat_resize.h>

#include <cstdlib>

struct monitor_state {
	int callback_count;
	int unknown_event_count;
};

static void *allocate(void *, size_t size)
{
	return std::malloc(size);
}

static void deallocate(void *, void *memory, size_t)
{
	std::free(memory);
}

static int cancellation_requested(void *context)
{
	monitor_state *state = static_cast<monitor_state *>(context);

	++state->callback_count;
	return 0;
}

static void report_event(void *context, const exfat_resize_event *event)
{
	monitor_state *state = static_cast<monitor_state *>(context);

	switch (event->code) {
	case EXFAT_RESIZE_EVENT_CODE_STAGE_ENTERED:
		if (event->values[0] <= EXFAT_RESIZE_STAGE_COMPLETED)
			++state->callback_count;
		break;
	default:
		++state->unknown_event_count;
		break;
	}
}

int main()
{
	exfat_resize_allocator allocator;
	exfat_resize_monitor monitor;
	monitor_state state = {};
	exfat_resize_event unknown_event = {};
	exfat_resize_stage stage = EXFAT_RESIZE_STAGE_COMPLETED;
	exfat_resize_error error;

	allocator.context = nullptr;
	allocator.allocate = allocate;
	allocator.deallocate = deallocate;
	monitor.context = &state;
	monitor.cancellation_requested = cancellation_requested;
	monitor.report_event = report_event;
	unknown_event.level = EXFAT_RESIZE_EVENT_LEVEL_INFO;
	unknown_event.code = UINT32_MAX;
	unknown_event.values[0] = 1;
	unknown_event.values[1] = 2;
	unknown_event.values[2] = 3;
	report_event(&state, &unknown_event);
	error = exfat_resize(nullptr, 1, &allocator, &monitor, &stage);
	return error == EXFAT_RESIZE_INVALID_DEVICE && stage == EXFAT_RESIZE_STAGE_PREFLIGHT &&
	        state.callback_count == 0 && state.unknown_event_count == 1
	    ? 0
	    : 1;
}
