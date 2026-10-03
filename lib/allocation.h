/* SPDX-License-Identifier: MIT */

#ifndef EXFAT_RESIZE_ALLOCATION_H
#define EXFAT_RESIZE_ALLOCATION_H

#include "exfat_resize.h"
#include "stream.h"

struct resize_volume;

/*
 * A nonzero model value means that the cluster is allocated. Values 2 and
 * above are FAT entries in model_geometry coordinates. The otherwise-invalid value 1 represents an
 * allocation whose NoFatChain stream deliberately has no FAT entry.
 */
#define EXFAT_MODEL_NO_FAT_CHAIN UINT32_C(1)

/* Current/source coordinates, independent of destination placement. Zero is the
 * predecessor of a stream head; next is END_OF_CHAIN at the tail, including
 * NoFatChain streams. Consumers may retain only links that need evacuation. */
struct allocation_link {
	uint32_t index;
	uint32_t previous;
	uint32_t cluster;
	uint32_t next;
};

/* claim reserves ownership and rejects duplicate claims. record, when supplied,
 * observes a validated link after that claim; it need not construct a target FAT.
 * A failed scan leaves partial in-memory claims which the caller must discard.
 * is_claimed and bad_cluster are required for bitmap reconciliation only. */
struct allocation_observer {
	void *context;
	enum exfat_resize_error (*claim)(void *context, uint32_t cluster);
	enum exfat_resize_error (*record)(
	    void *context, const struct allocation_stream *stream, const struct allocation_link *link);
	enum exfat_resize_error (*is_claimed)(void *context, uint32_t cluster, int *claimed);
	enum exfat_resize_error (*bad_cluster)(void *context, uint32_t cluster);
};

enum exfat_resize_error exfat_resize_visit_allocation(struct resize_volume *volume,
    const struct exfat_resize_geometry *geometry,
    const struct stream_chain_reader *chain,
    const struct allocation_stream *stream,
    const struct allocation_observer *observer);
enum exfat_resize_error exfat_resize_reconcile_allocation(struct resize_volume *volume,
    const struct exfat_resize_geometry *geometry,
    const struct stream_chain_reader *chain,
    const struct allocation_stream *bitmap,
    const struct allocation_observer *observer);

struct resize_allocation {
	struct resize_volume *volume;
	/* Model coordinates: grow target, or the full current volume during shrink. */
	const struct exfat_resize_geometry *model_geometry;
	struct allocation_stream old_bitmap;
	struct allocation_stream new_bitmap;
	uint32_t used_cluster_count;
	uint32_t fat_entry_zero;
	unsigned char *source_fat;
	size_t source_fat_size;
	/* Indexed by model cluster minus 2; covers every cluster in model_geometry. */
	uint32_t *allocation_model;
	size_t allocation_model_size;
};

enum exfat_resize_error exfat_resize_create_allocation_model(struct resize_allocation *allocation);
void exfat_resize_release_allocation_model(struct resize_allocation *allocation);
struct stream_chain_reader exfat_resize_source_chain(const struct resize_allocation *allocation);
struct stream_chain_reader exfat_resize_model_chain(const struct resize_allocation *allocation);

enum exfat_resize_error exfat_resize_load_source_fat(struct resize_allocation *allocation);

enum exfat_resize_error exfat_resize_source_fat_get(
    const struct resize_allocation *allocation, uint32_t cluster, uint32_t *value);

void exfat_resize_release_source_fat(struct resize_allocation *allocation);

enum exfat_resize_error exfat_resize_validate_reserved_fat_entries(
    struct resize_allocation *allocation);

enum exfat_resize_error exfat_resize_model_entry(
    struct resize_allocation *allocation, uint32_t cluster, uint32_t **entry);
enum exfat_resize_error exfat_resize_claim_model_cluster(
    struct resize_allocation *allocation, uint32_t cluster);
enum exfat_resize_error exfat_resize_remove_allocation_from_model(
    struct resize_allocation *allocation, const struct allocation_stream *stream);

enum exfat_resize_error exfat_resize_add_new_bitmap_to_model(struct resize_allocation *allocation);

/* The caller selects the recovery stage and checks cancellation before the first write. */
enum exfat_resize_error exfat_resize_write_target_fat(struct resize_allocation *allocation);

enum exfat_resize_error exfat_resize_write_target_bitmap(struct resize_allocation *allocation);

/* Incremental publication primitives in current-geometry coordinates. Preserve
 * neighboring FAT entries/bitmap bits. They use io_buffer, perform coherent
 * sector read/modify/write, and never sync or change the in-memory model.
 * The operation owns model updates and durability barriers. Defer cancellation
 * across a consistent publication batch (bitmap seeks can reach checkpoints),
 * then check after its flushes and sync. Not valid inside a directory callback. */
enum exfat_resize_error exfat_resize_write_fat_entry(struct resize_volume *volume,
    const struct exfat_resize_geometry *geometry,
    uint32_t cluster,
    uint32_t value);
enum exfat_resize_error exfat_resize_set_bitmap_bit(struct resize_volume *volume,
    const struct exfat_resize_geometry *geometry,
    const struct stream_chain_reader *chain,
    const struct allocation_stream *bitmap,
    uint32_t cluster,
    int allocated);

#endif
