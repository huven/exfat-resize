/* SPDX-License-Identifier: MIT */

#include "common.h"

#include "block_device.h"
#include "boot_region.h"
#include "event.h"
#include "exfat_resize.h"
#include "grow.h"
#include "resize_internal.h"
#include "shrink.h"
#include "volume.h"

void exfat_resize_enter_stage(
    struct resize_operation *operation, enum exfat_resize_stage stage, uint64_t value)
{
	operation->stage = stage;
	exfat_resize_report_event(&operation->monitor, EXFAT_RESIZE_EVENT_LEVEL_INFO,
	    EXFAT_RESIZE_EVENT_CODE_STAGE_ENTERED, (uint64_t)stage, value, 0);
}

enum exfat_resize_error exfat_resize_cancellation_checkpoint(struct resize_operation *operation)
{
	if (operation->cancellation_deferred)
		return EXFAT_RESIZE_SUCCESS;
	operation->cluster_steps_since_checkpoint = 0;
	if (operation->monitor.cancellation_requested != NULL &&
	    operation->monitor.cancellation_requested(operation->monitor.context) != 0)
		return EXFAT_RESIZE_CANCELLED;
	return EXFAT_RESIZE_SUCCESS;
}

enum exfat_resize_error exfat_resize_cluster_step_checkpoint(struct resize_operation *operation)
{
	if (operation->cancellation_deferred)
		return EXFAT_RESIZE_SUCCESS;
	if (++operation->cluster_steps_since_checkpoint < EXFAT_CLUSTER_CHECKPOINT_INTERVAL)
		return EXFAT_RESIZE_SUCCESS;
	return exfat_resize_cancellation_checkpoint(operation);
}

enum exfat_resize_error exfat_resize(const struct exfat_resize_block_device *device,
    uint64_t target_size,
    const struct exfat_resize_allocator *allocator,
    const struct exfat_resize_monitor *monitor,
    enum exfat_resize_stage *stage)
{
	struct resize_operation operation = { .stage = EXFAT_RESIZE_STAGE_PREFLIGHT };
	struct resize_volume volume = { .operation = &operation };
	enum exfat_resize_error error;
	enum exfat_resize_stage final_stage;
	uint64_t target_sector_count;

	error = exfat_resize_validate_block_device(device);
	if (error != EXFAT_RESIZE_SUCCESS)
		goto out;
	if (target_size == 0) {
		error = EXFAT_RESIZE_INVALID_ARGUMENT;
		goto out;
	}
	if (allocator == NULL || allocator->allocate == NULL || allocator->deallocate == NULL) {
		error = EXFAT_RESIZE_INVALID_ARGUMENT;
		goto out;
	}
	operation.allocator = *allocator;
	if (monitor != NULL)
		operation.monitor = *monitor;
	/* Report PREFLIGHT only after structural validation so rejected calls invoke no monitor. */
	exfat_resize_enter_stage(&operation, EXFAT_RESIZE_STAGE_PREFLIGHT, 0);
	error = exfat_resize_cancellation_checkpoint(&operation);
	if (error != EXFAT_RESIZE_SUCCESS)
		goto out;

	error = exfat_resize_open_volume(&volume, device);
	if (error != EXFAT_RESIZE_SUCCESS)
		goto out;
	target_sector_count = target_size / volume.sector_size;
	if (target_sector_count > volume.device->sector_count) {
		error = EXFAT_RESIZE_OUT_OF_BOUNDS;
		goto out;
	}
	error = exfat_resize_read_boot_regions(
	    volume.device, volume.io_buffer, EXFAT_IO_BUFFER_SIZE, &volume.geometry);
	if (error != EXFAT_RESIZE_SUCCESS)
		goto out;
	volume.io_sector_capacity = EXFAT_IO_BUFFER_SIZE / volume.sector_size;
	volume.cluster_size = (uint64_t)volume.geometry.sectors_per_cluster * volume.sector_size;

	if (target_sector_count < volume.geometry.volume_sector_count)
		error = exfat_resize_shrink(&volume, target_sector_count);
	else
		error = exfat_resize_grow(&volume, target_sector_count);

out:
	final_stage = operation.stage;
	exfat_resize_close_volume(&volume);
	if (stage != NULL)
		*stage = final_stage;
	return error;
}
