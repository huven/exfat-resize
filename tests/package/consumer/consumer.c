/* SPDX-License-Identifier: MIT */

#include <exfat_resize.h>

#include <stdlib.h>

struct monitor_state {
	int callback_count;
	int unknown_event_count;
};

static void *allocate(void *context, size_t size)
{
	(void)context;
	return malloc(size);
}

static void deallocate(void *context, void *memory, size_t size)
{
	(void)context;
	(void)size;
	free(memory);
}

static int cancellation_requested(void *context)
{
	struct monitor_state *state = context;

	++state->callback_count;
	return 0;
}

static void report_event(void *context, const struct exfat_resize_event *event)
{
	struct monitor_state *state = context;

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

int main(void)
{
	struct exfat_resize_allocator allocator = {
		.context = NULL,
		.allocate = allocate,
		.deallocate = deallocate,
	};
	struct monitor_state state = { 0 };
	struct exfat_resize_monitor monitor = {
		.context = &state,
		.cancellation_requested = cancellation_requested,
		.report_event = report_event,
	};
	const struct exfat_resize_event unknown_event = {
		.level = EXFAT_RESIZE_EVENT_LEVEL_INFO,
		.code = UINT32_MAX,
		.values = { 1, 2, 3 },
	};
	enum exfat_resize_stage stage = EXFAT_RESIZE_STAGE_COMPLETED;
	enum exfat_resize_error error;

	report_event(&state, &unknown_event);
	error = exfat_resize(NULL, 1, &allocator, &monitor, &stage);
	return error == EXFAT_RESIZE_INVALID_DEVICE && stage == EXFAT_RESIZE_STAGE_PREFLIGHT &&
	        state.callback_count == 0 && state.unknown_event_count == 1
	    ? 0
	    : 1;
}
