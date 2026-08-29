/* SPDX-License-Identifier: MIT */

#include <exfat_resize.h>

#include <stdlib.h>

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
	int *callback_count = context;

	++*callback_count;
	return 0;
}

static void report_event(void *context, const struct exfat_resize_event *event)
{
	int *callback_count = context;

	if (event->code == EXFAT_RESIZE_EVENT_CODE_STAGE_ENTERED)
		++*callback_count;
}

int main(void)
{
	struct exfat_resize_allocator allocator = {
		.context = NULL,
		.allocate = allocate,
		.deallocate = deallocate,
	};
	int callback_count = 0;
	struct exfat_resize_monitor monitor = {
		.context = &callback_count,
		.cancellation_requested = cancellation_requested,
		.report_event = report_event,
	};
	enum exfat_resize_stage stage = EXFAT_RESIZE_STAGE_COMPLETED;
	enum exfat_resize_error error;

	error = exfat_resize(NULL, 1, &allocator, &monitor, &stage);
	return error == EXFAT_RESIZE_INVALID_DEVICE && stage == EXFAT_RESIZE_STAGE_PREFLIGHT &&
	        callback_count == 0
	    ? 0
	    : 1;
}
