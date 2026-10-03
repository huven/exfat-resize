/* SPDX-License-Identifier: MIT */

#ifndef EXFAT_RESIZE_ALLOCATION_H
#define EXFAT_RESIZE_ALLOCATION_H

#include "exfat_resize.h"
#include "stream.h"

struct resize_context;

#define EXFAT_FAT_BAD_CLUSTER UINT32_C(0xfffffff7)
#define EXFAT_FAT_END_OF_CHAIN UINT32_C(0xffffffff)

/*
 * A nonzero model value means that the cluster is allocated. Values 2 and
 * above are target FAT entries. The otherwise-invalid value 1 represents an
 * allocation whose NoFatChain stream deliberately has no FAT entry.
 */
#define EXFAT_MODEL_NO_FAT_CHAIN UINT32_C(1)

enum exfat_resize_error exfat_resize_zero_allocation_model(struct resize_context *context);

int exfat_resize_fat_value_is_end_of_chain(uint32_t value);

enum exfat_resize_error exfat_resize_load_source_fat(struct resize_context *context);

enum exfat_resize_error exfat_resize_source_fat_get(
    const struct resize_context *context, uint32_t cluster, uint32_t *value);

void exfat_resize_release_source_fat(struct resize_context *context);

enum exfat_resize_error exfat_resize_validate_reserved_fat_entries(struct resize_context *context);

enum exfat_resize_error exfat_resize_model_entry_for_source_cluster(
    struct resize_context *context, uint32_t source_cluster, uint32_t **entry);

enum exfat_resize_error exfat_resize_claim_allocation_stream(
    struct resize_context *context, const struct allocation_stream *stream);

enum exfat_resize_error exfat_resize_claim_root_directory(
    struct resize_context *context, uint32_t first_cluster);

enum exfat_resize_error exfat_resize_validate_allocation_model(struct resize_context *context);

enum exfat_resize_error exfat_resize_remove_old_bitmap_from_model(struct resize_context *context);

enum exfat_resize_error exfat_resize_add_new_bitmap_to_model(struct resize_context *context);

/* The caller selects the recovery stage and checks cancellation before the first write. */
enum exfat_resize_error exfat_resize_write_target_fat(struct resize_context *context);

enum exfat_resize_error exfat_resize_write_target_bitmap(struct resize_context *context);

#endif
