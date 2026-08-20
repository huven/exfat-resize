#ifndef EXFAT_BITMAP_BOUNDS_H
#define EXFAT_BITMAP_BOUNDS_H

#include <stdint.h>
#include <stdbool.h>

/**
 * Bitmap allocation validation macros for exFAT cluster expansion.
 */

#define EXFAT_CLUSTER_TO_BITMAP_BYTE(cluster) (((cluster) - 2) / 8)
#define EXFAT_CLUSTER_TO_BITMAP_BIT(cluster)  (((cluster) - 2) % 8)

static inline bool exfat_is_cluster_in_bounds(uint32_t cluster, uint32_t total_clusters) {
    return (cluster >= 2 && cluster < (total_clusters + 2));
}

#endif /* EXFAT_BITMAP_BOUNDS_H */
