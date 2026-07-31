/* SPDX-License-Identifier: MIT */

#include "sector_adapter.h"

#include "block_device.h"

#include <stddef.h>
#include <stdint.h>

static int adapt_transfer(const struct exfat_resize_sector_adapter *adapter,
    uint64_t first_sector,
    uint32_t sector_count,
    uint64_t *source_first_sector,
    uint32_t *source_sector_count)
{
	uint32_t ratio = adapter->device_sectors_per_filesystem_sector;

	if (first_sector > UINT64_MAX / ratio || sector_count > UINT32_MAX / ratio)
		return -1;
	*source_first_sector = first_sector * ratio;
	*source_sector_count = sector_count * ratio;
	return 0;
}

static int adapted_read(void *context, uint64_t first_sector, uint32_t sector_count, void *buffer)
{
	struct exfat_resize_sector_adapter *adapter = context;
	uint64_t source_first_sector;
	uint32_t source_sector_count;

	if (adapt_transfer(
	        adapter, first_sector, sector_count, &source_first_sector, &source_sector_count) != 0)
		return -1;
	return adapter->source->read(
	    adapter->source->context, source_first_sector, source_sector_count, buffer);
}

static int adapted_write(
    void *context, uint64_t first_sector, uint32_t sector_count, const void *buffer)
{
	struct exfat_resize_sector_adapter *adapter = context;
	uint64_t source_first_sector;
	uint32_t source_sector_count;

	if (adapt_transfer(
	        adapter, first_sector, sector_count, &source_first_sector, &source_sector_count) != 0)
		return -1;
	return adapter->source->write(
	    adapter->source->context, source_first_sector, source_sector_count, buffer);
}

static int adapted_sync(void *context)
{
	struct exfat_resize_sector_adapter *adapter = context;

	return adapter->source->sync(adapter->source->context);
}

enum exfat_resize_error exfat_resize_adapt_block_device(
    const struct exfat_resize_block_device *source,
    uint32_t filesystem_sector_size,
    struct exfat_resize_sector_adapter *adapter)
{
	uint32_t ratio;

	if (adapter == NULL)
		return EXFAT_RESIZE_INVALID_ARGUMENT;
	if (!exfat_resize_sector_size_is_supported(filesystem_sector_size) ||
	    filesystem_sector_size < source->sector_size ||
	    filesystem_sector_size % source->sector_size != 0)
		return EXFAT_RESIZE_UNSUPPORTED_SECTOR_MAPPING;

	ratio = filesystem_sector_size / source->sector_size;
	adapter->source = source;
	adapter->device_sectors_per_filesystem_sector = ratio;
	adapter->device.context = adapter;
	adapter->device.sector_size = filesystem_sector_size;
	adapter->device.sector_count = source->sector_count / ratio;
	adapter->device.read = adapted_read;
	adapter->device.write = adapted_write;
	adapter->device.sync = adapted_sync;
	if (adapter->device.sector_count == 0)
		return EXFAT_RESIZE_OUT_OF_BOUNDS;
	return EXFAT_RESIZE_SUCCESS;
}
