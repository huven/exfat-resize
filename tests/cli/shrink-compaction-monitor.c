/* SPDX-License-Identifier: MIT */
#undef exfat_resize

#include "shrink-compaction-monitor.h"
#include "endian.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int windows_partition_test_request_compaction_cancellation(void);

/* This observer is deliberately restricted to the prepared native fixture:
 * 512-byte sectors, a single-sector bitmap, and retained root/bitmap heads.
 * Count synchronized tail releases, rather than sync calls or polling calls. */
struct compaction_monitor {
	const struct exfat_resize_block_device *device;
	const struct exfat_resize_monitor *monitor;
	uint64_t bitmap_sector;
	uint32_t first_tail_bit;
	uint32_t cluster_count;
	uint32_t initial_tail;
	uint32_t remaining_tail;
	uint32_t cancel_after;
	int resizing;
	int requested;
};

static int tail_count(struct compaction_monitor *context, uint32_t *count)
{
	unsigned char bitmap[512];
	uint32_t bit;
	*count = 0;
	if (context->device->read(context->device->context, context->bitmap_sector, 1, bitmap) != 0)
		return -1;
	for (bit = context->first_tail_bit; bit < context->cluster_count; ++bit)
		*count += (bitmap[bit / 8] & (1u << (bit % 8))) != 0;
	return 0;
}

static int observed_read(void *opaque, uint64_t first, uint32_t count, void *buffer)
{
	struct compaction_monitor *context = opaque;
	return context->device->read(context->device->context, first, count, buffer);
}

static int observed_write(void *opaque, uint64_t first, uint32_t count, const void *buffer)
{
	struct compaction_monitor *context = opaque;
	return context->device->write(context->device->context, first, count, buffer);
}

static int observed_sync(void *opaque)
{
	struct compaction_monitor *context = opaque;
	uint32_t remaining;
	if (context->device->sync(context->device->context) != 0)
		return -1;
	if (tail_count(context, &remaining) != 0 || remaining > context->remaining_tail)
		return -1;
	context->remaining_tail = remaining;
	return 0;
}

static int observed_cancellation(void *opaque)
{
	struct compaction_monitor *context = opaque;
	if (context->resizing && !context->requested &&
	    context->initial_tail - context->remaining_tail >= context->cancel_after) {
		context->requested = windows_partition_test_request_compaction_cancellation() == 0 ? 1 : -1;
		if (context->requested != 1)
			return 1;
	}
	return context->monitor->cancellation_requested(context->monitor->context);
}

static void observed_event(void *opaque, const struct exfat_resize_event *event)
{
	struct compaction_monitor *context = opaque;
	if (event->code == EXFAT_RESIZE_EVENT_CODE_STAGE_ENTERED)
		context->resizing = event->values[0] == EXFAT_RESIZE_STAGE_RESIZING;
	if (context->monitor->report_event != NULL)
		context->monitor->report_event(context->monitor->context, event);
}

enum exfat_resize_error windows_partition_test_resize(
    const struct exfat_resize_block_device *device,
    uint64_t target_size,
    const struct exfat_resize_allocator *allocator,
    const struct exfat_resize_monitor *monitor,
    enum exfat_resize_stage *stage)
{
	const char *fault = getenv("EXFAT_RESIZE_TEST_PARTITION_FAULT");
	struct compaction_monitor context = { .device = device, .monitor = monitor };
	struct exfat_resize_block_device observed_device = { .context = &context,
		.sector_size = device->sector_size,
		.sector_count = device->sector_count,
		.read = observed_read,
		.write = observed_write,
		.sync = observed_sync };
	struct exfat_resize_monitor observed_monitor = { .context = &context,
		.cancellation_requested = observed_cancellation,
		.report_event = observed_event };
	unsigned char sector[512];
	uint32_t heap, root, bitmap = 0, spc;
	uint64_t bitmap_length = 0;
	size_t offset;
	enum exfat_resize_error result;
	int late = fault != NULL && strcmp(fault, "cancel-compaction-late") == 0;

	if (!late && (fault == NULL || strcmp(fault, "cancel-compaction-early") != 0))
		return exfat_resize(device, target_size, allocator, monitor, stage);
	if (device->sector_size != 512 || monitor == NULL || monitor->cancellation_requested == NULL ||
	    stage == NULL || device->read(device->context, 0, 1, sector) != 0 || sector[108] != 9 ||
	    sector[109] > 16)
		goto invalid_fixture;
	spc = UINT32_C(1) << sector[109];
	if (exfat_resize_load_le32(sector, sizeof(sector), 88, &heap) != EXFAT_RESIZE_SUCCESS ||
	    exfat_resize_load_le32(sector, sizeof(sector), 92, &context.cluster_count) !=
	        EXFAT_RESIZE_SUCCESS ||
	    exfat_resize_load_le32(sector, sizeof(sector), 96, &root) != EXFAT_RESIZE_SUCCESS ||
	    target_size / 512 <= heap || context.cluster_count > 4096)
		goto invalid_fixture;
	context.first_tail_bit = (uint32_t)((target_size / 512 - heap) / spc);
	if (root < 2 || root - 2 >= context.first_tail_bit ||
	    context.first_tail_bit >= context.cluster_count ||
	    device->read(device->context, heap + (uint64_t)(root - 2) * spc, 1, sector) != 0)
		goto invalid_fixture;
	for (offset = 0; offset < sizeof(sector); offset += 32) {
		if (sector[offset] == 0x81) {
			(void)exfat_resize_load_le32(sector, sizeof(sector), offset + 20, &bitmap);
			(void)exfat_resize_load_le64(sector, sizeof(sector), offset + 24, &bitmap_length);
			break;
		}
	}
	if (bitmap < 2 || bitmap - 2 >= context.first_tail_bit ||
	    bitmap_length != (context.cluster_count + 7u) / 8)
		goto invalid_fixture;
	context.bitmap_sector = heap + (uint64_t)(bitmap - 2) * spc;
	if (tail_count(&context, &context.initial_tail) != 0 || context.initial_tail < 3)
		goto invalid_fixture;
	context.remaining_tail = context.initial_tail;
	context.cancel_after = late ? context.initial_tail - 1 : 1;
	result = exfat_resize(&observed_device, target_size, allocator, &observed_monitor, stage);
	if (context.requested != 1 ||
	    context.initial_tail - context.remaining_tail != context.cancel_after ||
	    result != EXFAT_RESIZE_CANCELLED || *stage != EXFAT_RESIZE_STAGE_SOURCE_READY) {
		fprintf(stderr, "exfat-resize-test: compaction cancellation checkpoint was not reached\n");
		return EXFAT_RESIZE_INTERNAL_ERROR;
	}
	fprintf(stderr, "exfat-resize-test: SOURCE_READY after %u of %u synchronized tail moves\n",
	    context.cancel_after, context.initial_tail);
	return result;

invalid_fixture:
	fprintf(stderr, "exfat-resize-test: unsupported compaction cancellation fixture\n");
	return EXFAT_RESIZE_INVALID_FILESYSTEM;
}
