/* SPDX-License-Identifier: MIT */

#ifndef EXFAT_RESIZE_INTERNAL_H
#define EXFAT_RESIZE_INTERNAL_H

#include "exfat_resize.h"

#define EXFAT_CLUSTER_CHECKPOINT_INTERVAL UINT32_C(1048576)

/* Shared services only; filesystem state belongs to the individual modules. */
struct resize_operation {
	struct exfat_resize_allocator allocator;
	struct exfat_resize_monitor monitor;
	enum exfat_resize_stage stage;
	/* Shared by cluster loops; reset at every cancellation checkpoint. */
	uint32_t cluster_steps_since_checkpoint;
};

void exfat_resize_enter_stage(
    struct resize_operation *operation, enum exfat_resize_stage stage, uint64_t value);
enum exfat_resize_error exfat_resize_cancellation_checkpoint(struct resize_operation *operation);
enum exfat_resize_error exfat_resize_cluster_step_checkpoint(struct resize_operation *operation);

#endif
