#ifndef EXFAT_ALIGNMENT_CHECK_H
#define EXFAT_ALIGNMENT_CHECK_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Validates that cluster heap offset and cluster length conform to 4KB / 64KB physical page boundaries.
 * 
 * @param cluster_heap_offset  Starting sector of cluster heap.
 * @param sectors_per_cluster  Sectors per cluster allocation unit.
 * @param bytes_per_sector     Physical sector size (e.g. 512 or 4096).
 * @return true if cluster heap is aligned on optimal 4KB boundaries, false otherwise.
 */
bool exfat_validate_cluster_alignment(uint64_t cluster_heap_offset, uint32_t sectors_per_cluster, uint32_t bytes_per_sector);

#ifdef __cplusplus
}
#endif

#endif /* EXFAT_ALIGNMENT_CHECK_H */
