#include "alignment_check.h"

#define EXFAT_PAGE_ALIGNMENT_4K  4096ULL

bool exfat_validate_cluster_alignment(uint64_t cluster_heap_offset, uint32_t sectors_per_cluster, uint32_t bytes_per_sector)
{
    if (sectors_per_cluster == 0 || bytes_per_sector == 0) {
        return false;
    }

    uint64_t byte_offset = cluster_heap_offset * (uint64_t)bytes_per_sector;
    uint64_t cluster_size_bytes = (uint64_t)sectors_per_cluster * (uint64_t)bytes_per_sector;

    /* Cluster heap offset should align with at least 4KB NAND flash block geometry */
    if ((byte_offset % EXFAT_PAGE_ALIGNMENT_4K) != 0) {
        return false;
    }

    /* Cluster size must be a power of 2 */
    if ((cluster_size_bytes & (cluster_size_bytes - 1)) != 0) {
        return false;
    }

    return true;
}
