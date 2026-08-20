/* SPDX-License-Identifier: MIT */

#include "common.h"

#include "boot_region.h"

#include "block_device.h"
#include "endian.h"

#include <string.h>

enum {
	EXFAT_MAIN_BOOT_REGION = 0,
	EXFAT_BACKUP_BOOT_REGION = 12,
	EXFAT_BOOT_REGION_SECTORS = 12,
	EXFAT_BOOT_CHECKSUM_SECTOR = 11,
	EXFAT_FIRST_EXTENDED_BOOT_SECTOR = 1,
	EXFAT_LAST_EXTENDED_BOOT_SECTOR = 8,

	EXFAT_FILESYSTEM_NAME_OFFSET = 3,
	EXFAT_MUST_BE_ZERO_OFFSET = 11,
	EXFAT_MUST_BE_ZERO_SIZE = 53,
	EXFAT_VOLUME_LENGTH_OFFSET = 72,
	EXFAT_FAT_OFFSET_OFFSET = 80,
	EXFAT_FAT_LENGTH_OFFSET = 84,
	EXFAT_CLUSTER_HEAP_OFFSET_OFFSET = 88,
	EXFAT_CLUSTER_COUNT_OFFSET = 92,
	EXFAT_ROOT_DIRECTORY_CLUSTER_OFFSET = 96,
	EXFAT_FILESYSTEM_REVISION_OFFSET = 104,
	EXFAT_VOLUME_FLAGS_OFFSET = 106,
	EXFAT_BYTES_PER_SECTOR_SHIFT_OFFSET = 108,
	EXFAT_SECTORS_PER_CLUSTER_SHIFT_OFFSET = 109,
	EXFAT_NUMBER_OF_FATS_OFFSET = 110,
	EXFAT_PERCENT_IN_USE_OFFSET = 112,
	EXFAT_BOOT_SIGNATURE_OFFSET = 510
};

enum { EXFAT_VOLUME_DIRTY = 0x0002, EXFAT_CLEAR_TO_ZERO = 0x0008 };

static const unsigned char exfat_jump_boot[3] = { 0xeb, 0x76, 0x90 };
static const unsigned char exfat_filesystem_name[8] = { 'E', 'X', 'F', 'A', 'T', ' ', ' ', ' ' };

struct exfat_boot_values {
	struct exfat_resize_geometry geometry;
	uint16_t filesystem_revision;
	uint16_t volume_flags;
	uint8_t bytes_per_sector_shift;
	uint8_t sectors_per_cluster_shift;
	uint8_t number_of_fats;
	uint8_t percent_in_use;
};

static enum exfat_resize_error load_boot_values(
    const unsigned char *sector, size_t sector_size, struct exfat_boot_values *values)
{
	enum exfat_resize_error error;
	uint16_t boot_signature;
	size_t index;

	if (memcmp(sector, exfat_jump_boot, sizeof(exfat_jump_boot)) != 0 ||
	    memcmp(sector + EXFAT_FILESYSTEM_NAME_OFFSET, exfat_filesystem_name,
	        sizeof(exfat_filesystem_name)) != 0)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;

	for (index = 0; index < EXFAT_MUST_BE_ZERO_SIZE; ++index) {
		if (sector[EXFAT_MUST_BE_ZERO_OFFSET + index] != 0)
			return EXFAT_RESIZE_INVALID_FILESYSTEM;
	}

	error = exfat_resize_load_le64(
	    sector, sector_size, EXFAT_VOLUME_LENGTH_OFFSET, &values->geometry.volume_sector_count);
	if (error != EXFAT_RESIZE_SUCCESS)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	error = exfat_resize_load_le32(
	    sector, sector_size, EXFAT_FAT_OFFSET_OFFSET, &values->geometry.fat_offset);
	if (error != EXFAT_RESIZE_SUCCESS)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	error = exfat_resize_load_le32(
	    sector, sector_size, EXFAT_FAT_LENGTH_OFFSET, &values->geometry.fat_length);
	if (error != EXFAT_RESIZE_SUCCESS)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	error = exfat_resize_load_le32(sector, sector_size, EXFAT_CLUSTER_HEAP_OFFSET_OFFSET,
	    &values->geometry.cluster_heap_offset);
	if (error != EXFAT_RESIZE_SUCCESS)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	error = exfat_resize_load_le32(
	    sector, sector_size, EXFAT_CLUSTER_COUNT_OFFSET, &values->geometry.cluster_count);
	if (error != EXFAT_RESIZE_SUCCESS)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	error = exfat_resize_load_le32(sector, sector_size, EXFAT_ROOT_DIRECTORY_CLUSTER_OFFSET,
	    &values->geometry.root_directory_cluster);
	if (error != EXFAT_RESIZE_SUCCESS)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	error = exfat_resize_load_le16(
	    sector, sector_size, EXFAT_FILESYSTEM_REVISION_OFFSET, &values->filesystem_revision);
	if (error != EXFAT_RESIZE_SUCCESS)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	error = exfat_resize_load_le16(
	    sector, sector_size, EXFAT_VOLUME_FLAGS_OFFSET, &values->volume_flags);
	if (error != EXFAT_RESIZE_SUCCESS)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;

	values->bytes_per_sector_shift = sector[EXFAT_BYTES_PER_SECTOR_SHIFT_OFFSET];
	values->sectors_per_cluster_shift = sector[EXFAT_SECTORS_PER_CLUSTER_SHIFT_OFFSET];
	values->number_of_fats = sector[EXFAT_NUMBER_OF_FATS_OFFSET];
	values->percent_in_use = sector[EXFAT_PERCENT_IN_USE_OFFSET];

	error =
	    exfat_resize_load_le16(sector, sector_size, EXFAT_BOOT_SIGNATURE_OFFSET, &boot_signature);
	if (error != EXFAT_RESIZE_SUCCESS || boot_signature != UINT16_C(0xaa55))
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	return EXFAT_RESIZE_SUCCESS;
}

static enum exfat_resize_error validate_boot_values(
    const struct exfat_resize_block_device *device, struct exfat_boot_values *values, int is_main)
{
	const struct exfat_resize_geometry *geometry = &values->geometry;
	const uint32_t sector_size = device->sector_size;
	uint64_t fat_end;
	uint64_t heap_end;
	uint64_t available_clusters;
	uint64_t expected_cluster_count;
	uint32_t minimum_fat_sectors;
	uint64_t minimum_volume_sectors;

	if (values->bytes_per_sector_shift < 9 || values->bytes_per_sector_shift > 12 ||
	    (UINT32_C(1) << values->bytes_per_sector_shift) != sector_size)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	if (values->sectors_per_cluster_shift > 25 - values->bytes_per_sector_shift)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;

	if (values->filesystem_revision != UINT16_C(0x0100))
		return EXFAT_RESIZE_UNSUPPORTED_REVISION;
	if (values->number_of_fats == 2)
		return EXFAT_RESIZE_UNSUPPORTED_MULTIPLE_FATS;
	if (values->number_of_fats != 1)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;

	if (is_main) {
		if ((values->volume_flags & UINT16_C(0xfff0)) != 0 ||
		    (values->volume_flags & UINT16_C(0x0001)) != 0)
			return EXFAT_RESIZE_INVALID_FILESYSTEM;
		if ((values->volume_flags & UINT16_C(0x0002)) != 0)
			return EXFAT_RESIZE_VOLUME_DIRTY;
		if ((values->volume_flags & UINT16_C(0x0004)) != 0)
			return EXFAT_RESIZE_MEDIA_FAILURE;
		if (values->percent_in_use > 100 && values->percent_in_use != UINT8_C(0xff))
			return EXFAT_RESIZE_INVALID_FILESYSTEM;
	}

	minimum_volume_sectors = (UINT64_C(1) << 20) / sector_size;
	if (geometry->volume_sector_count < minimum_volume_sectors ||
	    geometry->volume_sector_count > device->sector_count)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;

	if (geometry->cluster_count == 0 || geometry->cluster_count > UINT32_MAX - UINT32_C(10))
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	values->geometry.sectors_per_cluster = UINT32_C(1) << values->sectors_per_cluster_shift;

	if (geometry->fat_offset < EXFAT_BOOT_REGION_SECTORS * 2 || geometry->fat_length == 0)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	fat_end = (uint64_t)geometry->fat_offset + geometry->fat_length;
	if (fat_end > geometry->cluster_heap_offset)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;

	minimum_fat_sectors = exfat_resize_used_fat_sector_count(geometry->cluster_count, sector_size);
	if (geometry->fat_length < minimum_fat_sectors)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;

	heap_end = (uint64_t)geometry->cluster_count * values->geometry.sectors_per_cluster;
	heap_end += geometry->cluster_heap_offset;
	if (heap_end > geometry->volume_sector_count)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	available_clusters = (geometry->volume_sector_count - geometry->cluster_heap_offset) /
	    values->geometry.sectors_per_cluster;
	expected_cluster_count = available_clusters;
	if (expected_cluster_count > UINT32_MAX - UINT32_C(10))
		expected_cluster_count = UINT32_MAX - UINT32_C(10);
	if (geometry->cluster_count != expected_cluster_count)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;

	if (geometry->root_directory_cluster < 2 ||
	    geometry->root_directory_cluster > geometry->cluster_count + UINT32_C(1))
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	return EXFAT_RESIZE_SUCCESS;
}

static uint32_t update_checksum(uint32_t checksum, unsigned char value)
{
	return ((checksum << 31) | (checksum >> 1)) + value;
}

static int checksum_excludes_byte(uint32_t sector, size_t offset)
{
	if (sector != 0)
		return 0;
	return offset == EXFAT_VOLUME_FLAGS_OFFSET || offset == EXFAT_VOLUME_FLAGS_OFFSET + 1 ||
	    offset == EXFAT_PERCENT_IN_USE_OFFSET;
}

static enum exfat_resize_error validate_extended_boot_sector(
    const unsigned char *sector, size_t sector_size)
{
	enum exfat_resize_error error;
	uint32_t signature;

	error = exfat_resize_load_le32(sector, sector_size, sector_size - 4, &signature);
	if (error != EXFAT_RESIZE_SUCCESS || signature != UINT32_C(0xaa550000))
		return EXFAT_RESIZE_INVALID_FILESYSTEM;
	return EXFAT_RESIZE_SUCCESS;
}

static enum exfat_resize_error validate_checksum_sector(
    const unsigned char *sector, size_t sector_size, uint32_t expected)
{
	enum exfat_resize_error error;
	uint32_t stored;
	size_t offset;

	for (offset = 0; offset < sector_size; offset += sizeof(stored)) {
		error = exfat_resize_load_le32(sector, sector_size, offset, &stored);
		if (error != EXFAT_RESIZE_SUCCESS || stored != expected)
			return EXFAT_RESIZE_INVALID_FILESYSTEM;
	}
	return EXFAT_RESIZE_SUCCESS;
}

static enum exfat_resize_error read_boot_region(const struct exfat_resize_block_device *device,
    uint64_t first_sector,
    int is_main,
    unsigned char *work_buffer,
    size_t work_buffer_size,
    struct exfat_boot_values *values)
{
	const size_t sector_size = device->sector_size;
	enum exfat_resize_error error;
	struct exfat_boot_values result;
	uint32_t checksum = 0;
	uint32_t sector_index;
	size_t offset;

	memset(&result, 0, sizeof(result));
	for (sector_index = 0; sector_index < EXFAT_BOOT_CHECKSUM_SECTOR; ++sector_index) {
		error = exfat_resize_block_device_read(
		    device, first_sector + sector_index, 1, work_buffer, work_buffer_size);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;

		if (sector_index == 0) {
			error = load_boot_values(work_buffer, sector_size, &result);
			if (error != EXFAT_RESIZE_SUCCESS)
				return error;
		} else if (sector_index >= EXFAT_FIRST_EXTENDED_BOOT_SECTOR &&
		    sector_index <= EXFAT_LAST_EXTENDED_BOOT_SECTOR) {
			error = validate_extended_boot_sector(work_buffer, sector_size);
			if (error != EXFAT_RESIZE_SUCCESS)
				return error;
		}

		for (offset = 0; offset < sector_size; ++offset) {
			if (!checksum_excludes_byte(sector_index, offset))
				checksum = update_checksum(checksum, work_buffer[offset]);
		}
	}

	error = exfat_resize_block_device_read(
	    device, first_sector + EXFAT_BOOT_CHECKSUM_SECTOR, 1, work_buffer, work_buffer_size);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = validate_checksum_sector(work_buffer, sector_size, checksum);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;

	error = validate_boot_values(device, &result, is_main);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	*values = result;
	return EXFAT_RESIZE_SUCCESS;
}

static int boot_regions_are_consistent(
    const struct exfat_boot_values *main, const struct exfat_boot_values *backup)
{
	const struct exfat_resize_geometry *left = &main->geometry;
	const struct exfat_resize_geometry *right = &backup->geometry;

	return left->volume_sector_count == right->volume_sector_count &&
	    left->sectors_per_cluster == right->sectors_per_cluster &&
	    left->fat_offset == right->fat_offset && left->fat_length == right->fat_length &&
	    left->cluster_heap_offset == right->cluster_heap_offset &&
	    left->cluster_count == right->cluster_count &&
	    left->root_directory_cluster == right->root_directory_cluster;
}

enum exfat_resize_error exfat_resize_probe_sector_size(
    const struct exfat_resize_block_device *device,
    void *work_buffer,
    size_t work_buffer_size,
    uint32_t *filesystem_sector_size)
{
	struct exfat_boot_values values;
	enum exfat_resize_error error;

	if (device == NULL || work_buffer == NULL || filesystem_sector_size == NULL)
		return EXFAT_RESIZE_INVALID_ARGUMENT;
	if (work_buffer_size < device->sector_size)
		return EXFAT_RESIZE_INSUFFICIENT_WORKSPACE;

	error = exfat_resize_block_device_read(device, 0, 1, work_buffer, work_buffer_size);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	memset(&values, 0, sizeof(values));
	error = load_boot_values(work_buffer, device->sector_size, &values);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	if (values.bytes_per_sector_shift < 9 || values.bytes_per_sector_shift > 12)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;

	*filesystem_sector_size = UINT32_C(1) << values.bytes_per_sector_shift;
	return EXFAT_RESIZE_SUCCESS;
}

enum exfat_resize_error exfat_resize_read_boot_regions(
    const struct exfat_resize_block_device *device,
    void *work_buffer,
    size_t work_buffer_size,
    struct exfat_resize_geometry *geometry)
{
	struct exfat_boot_values main_values;
	struct exfat_boot_values backup_values;
	enum exfat_resize_error error;

	if (device == NULL || work_buffer == NULL || geometry == NULL)
		return EXFAT_RESIZE_INVALID_ARGUMENT;
	if (work_buffer_size < device->sector_size)
		return EXFAT_RESIZE_INSUFFICIENT_WORKSPACE;

	error = read_boot_region(
	    device, EXFAT_MAIN_BOOT_REGION, 1, work_buffer, work_buffer_size, &main_values);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = read_boot_region(
	    device, EXFAT_BACKUP_BOOT_REGION, 0, work_buffer, work_buffer_size, &backup_values);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	if (!boot_regions_are_consistent(&main_values, &backup_values))
		return EXFAT_RESIZE_INVALID_FILESYSTEM;

	*geometry = main_values.geometry;
	return EXFAT_RESIZE_SUCCESS;
}

enum exfat_resize_error exfat_resize_set_volume_dirty(
    const struct exfat_resize_block_device *device,
    void *work_buffer,
    size_t work_buffer_size,
    int dirty)
{
	size_t sector_size;
	enum exfat_resize_error error;
	unsigned char *sector = work_buffer;
	uint16_t volume_flags;

	if (device == NULL || work_buffer == NULL)
		return EXFAT_RESIZE_INVALID_ARGUMENT;
	sector_size = device->sector_size;
	if (work_buffer_size < sector_size)
		return EXFAT_RESIZE_INSUFFICIENT_WORKSPACE;

	error = exfat_resize_block_device_read(device, 0, 1, sector, work_buffer_size);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = exfat_resize_load_le16(sector, sector_size, EXFAT_VOLUME_FLAGS_OFFSET, &volume_flags);
	if (error != EXFAT_RESIZE_SUCCESS)
		return EXFAT_RESIZE_INVALID_FILESYSTEM;

	if (dirty) {
		volume_flags |= EXFAT_VOLUME_DIRTY;
		volume_flags &= (uint16_t)~EXFAT_CLEAR_TO_ZERO;
	} else {
		volume_flags &= (uint16_t)~EXFAT_VOLUME_DIRTY;
	}
	error = exfat_resize_store_le16(sector, sector_size, EXFAT_VOLUME_FLAGS_OFFSET, volume_flags);
	if (error != EXFAT_RESIZE_SUCCESS)
		return EXFAT_RESIZE_INTERNAL_ERROR;
	error = exfat_resize_block_device_write(device, 0, 1, sector, work_buffer_size);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	return exfat_resize_block_device_sync(device);
}

static enum exfat_resize_error write_boot_region(const struct exfat_resize_block_device *device,
    uint64_t first_sector,
    const struct exfat_resize_geometry *geometry,
    uint8_t percent_in_use,
    unsigned char *work_buffer,
    size_t work_buffer_size)
{
	const size_t sector_size = device->sector_size;
	enum exfat_resize_error error;
	uint32_t checksum = 0;
	uint32_t sector_index;
	size_t offset;

	for (sector_index = 0; sector_index < EXFAT_BOOT_CHECKSUM_SECTOR; ++sector_index) {
		error = exfat_resize_block_device_read(
		    device, first_sector + sector_index, 1, work_buffer, work_buffer_size);
		if (error != EXFAT_RESIZE_SUCCESS)
			return error;

		if (sector_index == 0) {
			error = exfat_resize_store_le64(work_buffer, sector_size, EXFAT_VOLUME_LENGTH_OFFSET,
			    geometry->volume_sector_count);
			if (error == EXFAT_RESIZE_SUCCESS)
				error = exfat_resize_store_le32(
				    work_buffer, sector_size, EXFAT_FAT_LENGTH_OFFSET, geometry->fat_length);
			if (error == EXFAT_RESIZE_SUCCESS)
				error = exfat_resize_store_le32(work_buffer, sector_size,
				    EXFAT_CLUSTER_HEAP_OFFSET_OFFSET, geometry->cluster_heap_offset);
			if (error == EXFAT_RESIZE_SUCCESS)
				error = exfat_resize_store_le32(
				    work_buffer, sector_size, EXFAT_CLUSTER_COUNT_OFFSET, geometry->cluster_count);
			if (error == EXFAT_RESIZE_SUCCESS)
				error = exfat_resize_store_le32(work_buffer, sector_size,
				    EXFAT_ROOT_DIRECTORY_CLUSTER_OFFSET, geometry->root_directory_cluster);
			if (error != EXFAT_RESIZE_SUCCESS)
				return EXFAT_RESIZE_INTERNAL_ERROR;
			work_buffer[EXFAT_PERCENT_IN_USE_OFFSET] = percent_in_use;
		}

		for (offset = 0; offset < sector_size; ++offset) {
			if (!checksum_excludes_byte(sector_index, offset))
				checksum = update_checksum(checksum, work_buffer[offset]);
		}
		if (sector_index == 0) {
			error = exfat_resize_block_device_write(
			    device, first_sector, 1, work_buffer, work_buffer_size);
			if (error != EXFAT_RESIZE_SUCCESS)
				return error;
		}
	}

	for (offset = 0; offset < sector_size; offset += sizeof(checksum)) {
		error = exfat_resize_store_le32(work_buffer, sector_size, offset, checksum);
		if (error != EXFAT_RESIZE_SUCCESS)
			return EXFAT_RESIZE_INTERNAL_ERROR;
	}
	return exfat_resize_block_device_write(
	    device, first_sector + EXFAT_BOOT_CHECKSUM_SECTOR, 1, work_buffer, work_buffer_size);
}

enum exfat_resize_error exfat_resize_write_boot_regions(
    const struct exfat_resize_block_device *device,
    const struct exfat_resize_geometry *geometry,
    uint32_t used_cluster_count,
    void *work_buffer,
    size_t work_buffer_size)
{
	enum exfat_resize_error error;
	uint8_t percent_in_use;

	if (device == NULL || geometry == NULL || work_buffer == NULL)
		return EXFAT_RESIZE_INVALID_ARGUMENT;
	if (work_buffer_size < device->sector_size)
		return EXFAT_RESIZE_INSUFFICIENT_WORKSPACE;
	if (geometry->cluster_count == 0 || used_cluster_count > geometry->cluster_count)
		return EXFAT_RESIZE_INVALID_ARGUMENT;

	percent_in_use = (uint8_t)((uint64_t)used_cluster_count * 100 / geometry->cluster_count);

	error = write_boot_region(
	    device, EXFAT_BACKUP_BOOT_REGION, geometry, percent_in_use, work_buffer, work_buffer_size);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = exfat_resize_block_device_sync(device);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	error = write_boot_region(
	    device, EXFAT_MAIN_BOOT_REGION, geometry, percent_in_use, work_buffer, work_buffer_size);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	return exfat_resize_block_device_sync(device);
}
