/* SPDX-License-Identifier: MIT */

#ifndef EXFAT_RESIZE_ALLOCATION_H
#define EXFAT_RESIZE_ALLOCATION_H

#include "exfat_resize.h"
#include "stream.h"

struct resize_volume;

#define EXFAT_FAT_BAD_CLUSTER UINT32_C(0xfffffff7)
#define EXFAT_FAT_END_OF_CHAIN UINT32_C(0xffffffff)

/*
 * A nonzero model value means that the cluster is allocated. Values 2 and
 * above are target FAT entries. The otherwise-invalid value 1 represents an
 * allocation whose NoFatChain stream deliberately has no FAT entry.
 */
#define EXFAT_MODEL_NO_FAT_CHAIN UINT32_C(1)

/*
 * Placement policy supplied by the operation. All callbacks are required and
 * must be pure: they issue no I/O, allocate no memory, and do not poll cancellation.
 * The context and target geometry remain valid until allocation state is released.
 */
struct allocation_mapping {
	const void *context;
	enum exfat_resize_error (*map_cluster)(
	    const void *context, uint32_t source_cluster, uint32_t *target_cluster);
	int (*breaks_contiguity)(const void *context, uint32_t first_cluster, uint32_t cluster_count);
	enum exfat_resize_error (*validate_bad_cluster)(const void *context, uint32_t cluster);
};

struct resize_allocation {
	struct resize_volume *volume;
	const struct exfat_resize_geometry *target;
	struct allocation_mapping mapping;
	struct allocation_stream old_bitmap;
	struct allocation_stream new_bitmap;
	uint32_t used_cluster_count;
	uint32_t fat_entry_zero;
	unsigned char *source_fat;
	size_t source_fat_size;
	/* Indexed by target cluster minus 2; one entry for every target cluster. */
	uint32_t *allocation_model;
	size_t allocation_model_size;
};

enum exfat_resize_error exfat_resize_create_allocation_model(struct resize_allocation *allocation);
void exfat_resize_release_allocation_model(struct resize_allocation *allocation);
enum exfat_resize_error exfat_resize_map_cluster(
    const struct resize_allocation *allocation, uint32_t source_cluster, uint32_t *target_cluster);
int exfat_resize_mapping_breaks_contiguity(
    const struct resize_allocation *allocation, uint32_t first_cluster, uint32_t cluster_count);

int exfat_resize_fat_value_is_end_of_chain(uint32_t value);

enum exfat_resize_error exfat_resize_load_source_fat(struct resize_allocation *allocation);

enum exfat_resize_error exfat_resize_source_fat_get(
    const struct resize_allocation *allocation, uint32_t cluster, uint32_t *value);

void exfat_resize_release_source_fat(struct resize_allocation *allocation);

enum exfat_resize_error exfat_resize_validate_reserved_fat_entries(
    struct resize_allocation *allocation);

enum exfat_resize_error exfat_resize_model_entry_for_source_cluster(
    struct resize_allocation *allocation, uint32_t source_cluster, uint32_t **entry);

enum exfat_resize_error exfat_resize_claim_allocation_stream(
    struct resize_allocation *allocation, const struct allocation_stream *stream);

enum exfat_resize_error exfat_resize_claim_root_directory(
    struct resize_allocation *allocation, uint32_t first_cluster);

enum exfat_resize_error exfat_resize_validate_allocation_model(
    struct resize_allocation *allocation);

enum exfat_resize_error exfat_resize_remove_old_bitmap_from_model(
    struct resize_allocation *allocation);

enum exfat_resize_error exfat_resize_add_new_bitmap_to_model(struct resize_allocation *allocation);

/* The caller selects the recovery stage and checks cancellation before the first write. */
enum exfat_resize_error exfat_resize_write_target_fat(struct resize_allocation *allocation);

enum exfat_resize_error exfat_resize_write_target_bitmap(struct resize_allocation *allocation);

#endif
