/* SPDX-License-Identifier: MIT */

#include <exfat_resize.h>

#include <cstdlib>

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
	int *callback_count = static_cast<int *>(context);

	++*callback_count;
	return 0;
}

static void report_event(void *context, const exfat_resize_event *event)
{
	int *callback_count = static_cast<int *>(context);

	if (event->code == EXFAT_RESIZE_EVENT_CODE_STAGE_ENTERED)
		++*callback_count;
}

int main()
{
	exfat_resize_allocator allocator;
	exfat_resize_monitor monitor;
	int callback_count = 0;
	exfat_resize_stage stage = EXFAT_RESIZE_STAGE_COMPLETED;
	exfat_resize_error error;

	allocator.context = nullptr;
	allocator.allocate = allocate;
	allocator.deallocate = deallocate;
	monitor.context = &callback_count;
	monitor.cancellation_requested = cancellation_requested;
	monitor.report_event = report_event;
	error = exfat_resize(nullptr, 1, &allocator, &monitor, &stage);
	return error == EXFAT_RESIZE_INVALID_DEVICE && stage == EXFAT_RESIZE_STAGE_PREFLIGHT &&
	        callback_count == 0
	    ? 0
	    : 1;
}
