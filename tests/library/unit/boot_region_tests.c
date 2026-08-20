/* SPDX-License-Identifier: MIT */

#include "block_device.h"
#include "boot_region.h"
#include "endian.h"
#include "support/memory_block_device.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
	BOOT_REGION_SECTORS = 12,
	BOOT_REGIONS_SECTORS = 24,
	BOOT_CHECKSUM_SECTOR = 11,
	BACKUP_BOOT_REGION = 12,
	VOLUME_LENGTH_OFFSET = 72,
	FAT_OFFSET_OFFSET = 80,
	FAT_LENGTH_OFFSET = 84,
	CLUSTER_HEAP_OFFSET_OFFSET = 88,
	CLUSTER_COUNT_OFFSET = 92,
	ROOT_DIRECTORY_CLUSTER_OFFSET = 96,
	FILESYSTEM_REVISION_OFFSET = 104,
	VOLUME_FLAGS_OFFSET = 106,
	BYTES_PER_SECTOR_SHIFT_OFFSET = 108,
	SECTORS_PER_CLUSTER_SHIFT_OFFSET = 109,
	NUMBER_OF_FATS_OFFSET = 110,
	PERCENT_IN_USE_OFFSET = 112
};

struct boot_fixture {
	struct memory_block_device memory;
	unsigned char *image;
	uint32_t sector_size;
	uint64_t volume_sector_count;
	uint32_t sectors_per_cluster;
	uint32_t fat_offset;
	uint32_t fat_length;
	uint32_t cluster_heap_offset;
	uint32_t cluster_count;
	uint32_t root_directory_cluster;
};

static int failure_count;

#define CHECK(expression) \
	do { \
		if (!(expression)) { \
			fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expression); \
			++failure_count; \
		} \
	} while (0)

static uint32_t checksum_byte(uint32_t checksum, unsigned char value)
{
	return ((checksum << 31) | (checksum >> 1)) + value;
}

static unsigned char *fixture_sector(struct boot_fixture *fixture, uint32_t sector)
{
	return fixture->image + (size_t)sector * fixture->sector_size;
}

static void set_region_checksum(struct boot_fixture *fixture, uint32_t first_sector)
{
	uint32_t checksum = 0;
	uint32_t sector_index;
	size_t offset;
	unsigned char *sector;

	for (sector_index = 0; sector_index < BOOT_CHECKSUM_SECTOR; ++sector_index) {
		sector = fixture_sector(fixture, first_sector + sector_index);
		for (offset = 0; offset < fixture->sector_size; ++offset) {
			if (sector_index == 0 &&
			    (offset == VOLUME_FLAGS_OFFSET || offset == VOLUME_FLAGS_OFFSET + 1 ||
			        offset == PERCENT_IN_USE_OFFSET))
				continue;
			checksum = checksum_byte(checksum, sector[offset]);
		}
	}

	sector = fixture_sector(fixture, first_sector + BOOT_CHECKSUM_SECTOR);
	for (offset = 0; offset < fixture->sector_size; offset += 4)
		CHECK(exfat_resize_store_le32(sector, fixture->sector_size, offset, checksum) ==
		    EXFAT_RESIZE_SUCCESS);
}

static void initialize_boot_sector(struct boot_fixture *fixture, unsigned char *sector)
{
	uint8_t bytes_per_sector_shift;
	uint8_t sectors_per_cluster_shift;
	uint32_t value;

	bytes_per_sector_shift = 0;
	for (value = fixture->sector_size; value > 1; value >>= 1)
		++bytes_per_sector_shift;
	sectors_per_cluster_shift = 0;
	for (value = fixture->sectors_per_cluster; value > 1; value >>= 1)
		++sectors_per_cluster_shift;

	sector[0] = 0xeb;
	sector[1] = 0x76;
	sector[2] = 0x90;
	memcpy(sector + 3, "EXFAT   ", 8);
	CHECK(exfat_resize_store_le64(sector, fixture->sector_size, VOLUME_LENGTH_OFFSET,
	          fixture->volume_sector_count) == EXFAT_RESIZE_SUCCESS);
	CHECK(exfat_resize_store_le32(sector, fixture->sector_size, FAT_OFFSET_OFFSET,
	          fixture->fat_offset) == EXFAT_RESIZE_SUCCESS);
	CHECK(exfat_resize_store_le32(sector, fixture->sector_size, FAT_LENGTH_OFFSET,
	          fixture->fat_length) == EXFAT_RESIZE_SUCCESS);
	CHECK(exfat_resize_store_le32(sector, fixture->sector_size, CLUSTER_HEAP_OFFSET_OFFSET,
	          fixture->cluster_heap_offset) == EXFAT_RESIZE_SUCCESS);
	CHECK(exfat_resize_store_le32(sector, fixture->sector_size, CLUSTER_COUNT_OFFSET,
	          fixture->cluster_count) == EXFAT_RESIZE_SUCCESS);
	CHECK(exfat_resize_store_le32(sector, fixture->sector_size, ROOT_DIRECTORY_CLUSTER_OFFSET,
	          fixture->root_directory_cluster) == EXFAT_RESIZE_SUCCESS);
	CHECK(exfat_resize_store_le16(sector, fixture->sector_size, FILESYSTEM_REVISION_OFFSET,
	          UINT16_C(0x0100)) == EXFAT_RESIZE_SUCCESS);
	sector[BYTES_PER_SECTOR_SHIFT_OFFSET] = bytes_per_sector_shift;
	sector[SECTORS_PER_CLUSTER_SHIFT_OFFSET] = sectors_per_cluster_shift;
	sector[NUMBER_OF_FATS_OFFSET] = 1;
	sector[PERCENT_IN_USE_OFFSET] = 0;
	CHECK(exfat_resize_store_le16(sector, fixture->sector_size, 510, UINT16_C(0xaa55)) ==
	    EXFAT_RESIZE_SUCCESS);
}

static void write_fixture(struct boot_fixture *fixture)
{
	enum exfat_resize_error error;
	size_t image_size = (size_t)BOOT_REGIONS_SECTORS * fixture->sector_size;

	memory_block_device_clear_operations(&fixture->memory);
	error = exfat_resize_block_device_write(
	    &fixture->memory.device, 0, BOOT_REGIONS_SECTORS, fixture->image, image_size);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	memory_block_device_clear_operations(&fixture->memory);
}

static void initialize_fixture(struct boot_fixture *fixture, uint32_t sector_size)
{
	unsigned char *main_region;
	unsigned char *backup_region;
	uint32_t sector_index;

	memset(fixture, 0, sizeof(*fixture));
	fixture->sector_size = sector_size;
	fixture->volume_sector_count = 16384;
	fixture->fat_offset = 24;
	fixture->root_directory_cluster = 2;
	if (sector_size == 512) {
		fixture->sectors_per_cluster = 8;
		fixture->fat_length = 16;
		fixture->cluster_heap_offset = 128;
		fixture->cluster_count = 2032;
	} else {
		fixture->sectors_per_cluster = 2;
		fixture->cluster_heap_offset = 128;
		fixture->cluster_count = 8128;
		fixture->fat_length = (fixture->cluster_count + 2) * 4 / sector_size;
		if ((fixture->cluster_count + 2) * 4 % sector_size != 0)
			++fixture->fat_length;
	}

	memory_block_device_init(&fixture->memory, sector_size, fixture->volume_sector_count);
	fixture->image = calloc(BOOT_REGIONS_SECTORS, (size_t)sector_size);
	CHECK(fixture->image != NULL);
	if (fixture->image == NULL)
		return;

	main_region = fixture_sector(fixture, 0);
	initialize_boot_sector(fixture, main_region);
	for (sector_index = 1; sector_index <= 8; ++sector_index) {
		CHECK(exfat_resize_store_le32(fixture_sector(fixture, sector_index), fixture->sector_size,
		          fixture->sector_size - 4, UINT32_C(0xaa550000)) == EXFAT_RESIZE_SUCCESS);
	}
	set_region_checksum(fixture, 0);

	backup_region = fixture_sector(fixture, BACKUP_BOOT_REGION);
	memcpy(backup_region, main_region, (size_t)BOOT_REGION_SECTORS * fixture->sector_size);
	set_region_checksum(fixture, BACKUP_BOOT_REGION);
	write_fixture(fixture);
}

static void destroy_fixture(struct boot_fixture *fixture)
{
	free(fixture->image);
	memory_block_device_destroy(&fixture->memory);
}

static enum exfat_resize_error read_fixture(
    struct boot_fixture *fixture, struct exfat_resize_geometry *geometry)
{
	unsigned char workspace[4096];

	return exfat_resize_read_boot_regions(
	    &fixture->memory.device, workspace, fixture->sector_size, geometry);
}

static void check_geometry(
    const struct boot_fixture *fixture, const struct exfat_resize_geometry *geometry)
{
	CHECK(geometry->volume_sector_count == fixture->volume_sector_count);
	CHECK(geometry->sectors_per_cluster == fixture->sectors_per_cluster);
	CHECK(geometry->fat_offset == fixture->fat_offset);
	CHECK(geometry->fat_length == fixture->fat_length);
	CHECK(geometry->cluster_heap_offset == fixture->cluster_heap_offset);
	CHECK(geometry->cluster_count == fixture->cluster_count);
	CHECK(geometry->root_directory_cluster == fixture->root_directory_cluster);
}

static void test_valid_boot_regions(uint32_t sector_size)
{
	struct exfat_resize_geometry geometry;
	struct boot_fixture fixture;
	enum exfat_resize_error error;
	size_t index;

	initialize_fixture(&fixture, sector_size);
	if (fixture.image == NULL) {
		destroy_fixture(&fixture);
		return;
	}

	error = read_fixture(&fixture, &geometry);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	if (error == EXFAT_RESIZE_SUCCESS) {
		check_geometry(&fixture, &geometry);
		CHECK(fixture.memory.operation_count == BOOT_REGIONS_SECTORS);
		for (index = 0; index < fixture.memory.operation_count; ++index) {
			CHECK(fixture.memory.operations[index].kind == MEMORY_OPERATION_READ);
			CHECK(fixture.memory.operations[index].first_sector == index);
			CHECK(fixture.memory.operations[index].sector_count == 1);
		}
	}

	destroy_fixture(&fixture);
}

static void test_stale_backup_fields(void)
{
	struct exfat_resize_geometry geometry;
	struct boot_fixture fixture;
	enum exfat_resize_error error;
	unsigned char *backup;

	initialize_fixture(&fixture, 512);
	if (fixture.image == NULL) {
		destroy_fixture(&fixture);
		return;
	}

	backup = fixture_sector(&fixture, BACKUP_BOOT_REGION);
	CHECK(exfat_resize_store_le16(backup, fixture.sector_size, VOLUME_FLAGS_OFFSET,
	          UINT16_C(0x0006)) == EXFAT_RESIZE_SUCCESS);
	backup[PERCENT_IN_USE_OFFSET] = UINT8_C(0xff);
	write_fixture(&fixture);

	error = read_fixture(&fixture, &geometry);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	if (error == EXFAT_RESIZE_SUCCESS)
		check_geometry(&fixture, &geometry);

	destroy_fixture(&fixture);
}

static void test_independent_region_checksums(void)
{
	struct exfat_resize_geometry geometry;
	struct boot_fixture fixture;
	enum exfat_resize_error error;

	initialize_fixture(&fixture, 512);
	if (fixture.image == NULL) {
		destroy_fixture(&fixture);
		return;
	}

	fixture_sector(&fixture, BACKUP_BOOT_REGION + 9)[0] = 1;
	set_region_checksum(&fixture, BACKUP_BOOT_REGION);
	write_fixture(&fixture);

	error = read_fixture(&fixture, &geometry);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	if (error == EXFAT_RESIZE_SUCCESS)
		check_geometry(&fixture, &geometry);

	destroy_fixture(&fixture);
}

static void check_parse_failure(struct boot_fixture *fixture, enum exfat_resize_error expected)
{
	struct exfat_resize_geometry geometry;
	struct exfat_resize_geometry unchanged;
	enum exfat_resize_error error;

	memset(&geometry, 0xa5, sizeof(geometry));
	unchanged = geometry;
	write_fixture(fixture);
	error = read_fixture(fixture, &geometry);
	CHECK(error == expected);
	CHECK(memcmp(&geometry, &unchanged, sizeof(geometry)) == 0);
}

static void test_checksums_and_region_consistency(void)
{
	struct boot_fixture fixture;
	unsigned char *backup;

	initialize_fixture(&fixture, 512);
	if (fixture.image == NULL) {
		destroy_fixture(&fixture);
		return;
	}

	fixture_sector(&fixture, 9)[0] = 1;
	check_parse_failure(&fixture, EXFAT_RESIZE_INVALID_FILESYSTEM);
	destroy_fixture(&fixture);

	initialize_fixture(&fixture, 512);
	if (fixture.image == NULL) {
		destroy_fixture(&fixture);
		return;
	}
	fixture_sector(&fixture, BACKUP_BOOT_REGION + 10)[0] = 1;
	check_parse_failure(&fixture, EXFAT_RESIZE_INVALID_FILESYSTEM);
	destroy_fixture(&fixture);

	initialize_fixture(&fixture, 512);
	if (fixture.image == NULL) {
		destroy_fixture(&fixture);
		return;
	}
	backup = fixture_sector(&fixture, BACKUP_BOOT_REGION);
	CHECK(exfat_resize_store_le32(backup, fixture.sector_size, FAT_OFFSET_OFFSET, 25) ==
	    EXFAT_RESIZE_SUCCESS);
	set_region_checksum(&fixture, BACKUP_BOOT_REGION);
	check_parse_failure(&fixture, EXFAT_RESIZE_INVALID_FILESYSTEM);
	destroy_fixture(&fixture);
}

static void test_required_boot_structures(void)
{
	struct boot_fixture fixture;

	initialize_fixture(&fixture, 512);
	if (fixture.image == NULL) {
		destroy_fixture(&fixture);
		return;
	}
	fixture_sector(&fixture, 0)[3] = 'N';
	set_region_checksum(&fixture, 0);
	check_parse_failure(&fixture, EXFAT_RESIZE_INVALID_FILESYSTEM);
	destroy_fixture(&fixture);

	initialize_fixture(&fixture, 512);
	if (fixture.image == NULL) {
		destroy_fixture(&fixture);
		return;
	}
	fixture_sector(&fixture, 0)[11] = 1;
	set_region_checksum(&fixture, 0);
	check_parse_failure(&fixture, EXFAT_RESIZE_INVALID_FILESYSTEM);
	destroy_fixture(&fixture);

	initialize_fixture(&fixture, 512);
	if (fixture.image == NULL) {
		destroy_fixture(&fixture);
		return;
	}
	fixture_sector(&fixture, 1)[fixture.sector_size - 1] = 0;
	set_region_checksum(&fixture, 0);
	check_parse_failure(&fixture, EXFAT_RESIZE_INVALID_FILESYSTEM);
	destroy_fixture(&fixture);

	initialize_fixture(&fixture, 512);
	if (fixture.image == NULL) {
		destroy_fixture(&fixture);
		return;
	}
	fixture_sector(&fixture, 0)[510] = 0;
	set_region_checksum(&fixture, 0);
	check_parse_failure(&fixture, EXFAT_RESIZE_INVALID_FILESYSTEM);
	destroy_fixture(&fixture);
}

static void test_unsupported_boot_values(void)
{
	struct boot_fixture fixture;
	unsigned char *main;

	initialize_fixture(&fixture, 512);
	if (fixture.image == NULL) {
		destroy_fixture(&fixture);
		return;
	}
	main = fixture_sector(&fixture, 0);
	CHECK(exfat_resize_store_le16(main, fixture.sector_size, FILESYSTEM_REVISION_OFFSET,
	          UINT16_C(0x0101)) == EXFAT_RESIZE_SUCCESS);
	set_region_checksum(&fixture, 0);
	check_parse_failure(&fixture, EXFAT_RESIZE_UNSUPPORTED_REVISION);
	destroy_fixture(&fixture);

	initialize_fixture(&fixture, 512);
	if (fixture.image == NULL) {
		destroy_fixture(&fixture);
		return;
	}
	main = fixture_sector(&fixture, 0);
	main[NUMBER_OF_FATS_OFFSET] = 2;
	set_region_checksum(&fixture, 0);
	check_parse_failure(&fixture, EXFAT_RESIZE_UNSUPPORTED_MULTIPLE_FATS);
	destroy_fixture(&fixture);

	initialize_fixture(&fixture, 512);
	if (fixture.image == NULL) {
		destroy_fixture(&fixture);
		return;
	}
	main = fixture_sector(&fixture, 0);
	CHECK(exfat_resize_store_le16(main, fixture.sector_size, VOLUME_FLAGS_OFFSET,
	          UINT16_C(0x0002)) == EXFAT_RESIZE_SUCCESS);
	check_parse_failure(&fixture, EXFAT_RESIZE_VOLUME_DIRTY);
	destroy_fixture(&fixture);

	initialize_fixture(&fixture, 512);
	if (fixture.image == NULL) {
		destroy_fixture(&fixture);
		return;
	}
	main = fixture_sector(&fixture, 0);
	CHECK(exfat_resize_store_le16(main, fixture.sector_size, VOLUME_FLAGS_OFFSET,
	          UINT16_C(0x0004)) == EXFAT_RESIZE_SUCCESS);
	check_parse_failure(&fixture, EXFAT_RESIZE_MEDIA_FAILURE);
	destroy_fixture(&fixture);
}

static void test_invalid_geometry(void)
{
	struct boot_fixture fixture;
	unsigned char *main;

	initialize_fixture(&fixture, 512);
	if (fixture.image == NULL) {
		destroy_fixture(&fixture);
		return;
	}
	main = fixture_sector(&fixture, 0);
	main[BYTES_PER_SECTOR_SHIFT_OFFSET] = 12;
	set_region_checksum(&fixture, 0);
	check_parse_failure(&fixture, EXFAT_RESIZE_INVALID_FILESYSTEM);
	destroy_fixture(&fixture);

	initialize_fixture(&fixture, 512);
	if (fixture.image == NULL) {
		destroy_fixture(&fixture);
		return;
	}
	main = fixture_sector(&fixture, 0);
	CHECK(exfat_resize_store_le32(main, fixture.sector_size, FAT_OFFSET_OFFSET, 23) ==
	    EXFAT_RESIZE_SUCCESS);
	set_region_checksum(&fixture, 0);
	check_parse_failure(&fixture, EXFAT_RESIZE_INVALID_FILESYSTEM);
	destroy_fixture(&fixture);

	initialize_fixture(&fixture, 512);
	if (fixture.image == NULL) {
		destroy_fixture(&fixture);
		return;
	}
	main = fixture_sector(&fixture, 0);
	CHECK(exfat_resize_store_le32(main, fixture.sector_size, FAT_LENGTH_OFFSET, 15) ==
	    EXFAT_RESIZE_SUCCESS);
	set_region_checksum(&fixture, 0);
	check_parse_failure(&fixture, EXFAT_RESIZE_INVALID_FILESYSTEM);
	destroy_fixture(&fixture);

	initialize_fixture(&fixture, 512);
	if (fixture.image == NULL) {
		destroy_fixture(&fixture);
		return;
	}
	main = fixture_sector(&fixture, 0);
	CHECK(exfat_resize_store_le32(main, fixture.sector_size, CLUSTER_HEAP_OFFSET_OFFSET, 32) ==
	    EXFAT_RESIZE_SUCCESS);
	set_region_checksum(&fixture, 0);
	check_parse_failure(&fixture, EXFAT_RESIZE_INVALID_FILESYSTEM);
	destroy_fixture(&fixture);

	initialize_fixture(&fixture, 512);
	if (fixture.image == NULL) {
		destroy_fixture(&fixture);
		return;
	}
	main = fixture_sector(&fixture, 0);
	CHECK(exfat_resize_store_le32(main, fixture.sector_size, CLUSTER_COUNT_OFFSET, 4096) ==
	    EXFAT_RESIZE_SUCCESS);
	set_region_checksum(&fixture, 0);
	check_parse_failure(&fixture, EXFAT_RESIZE_INVALID_FILESYSTEM);
	destroy_fixture(&fixture);

	initialize_fixture(&fixture, 512);
	if (fixture.image == NULL) {
		destroy_fixture(&fixture);
		return;
	}
	main = fixture_sector(&fixture, 0);
	CHECK(exfat_resize_store_le32(main, fixture.sector_size, ROOT_DIRECTORY_CLUSTER_OFFSET,
	          fixture.cluster_count + 2) == EXFAT_RESIZE_SUCCESS);
	set_region_checksum(&fixture, 0);
	check_parse_failure(&fixture, EXFAT_RESIZE_INVALID_FILESYSTEM);
	destroy_fixture(&fixture);

	initialize_fixture(&fixture, 512);
	if (fixture.image == NULL) {
		destroy_fixture(&fixture);
		return;
	}
	main = fixture_sector(&fixture, 0);
	main[PERCENT_IN_USE_OFFSET] = 101;
	check_parse_failure(&fixture, EXFAT_RESIZE_INVALID_FILESYSTEM);
	destroy_fixture(&fixture);

	initialize_fixture(&fixture, 512);
	if (fixture.image == NULL) {
		destroy_fixture(&fixture);
		return;
	}
	fixture.memory.device.sector_count = fixture.volume_sector_count - 1;
	check_parse_failure(&fixture, EXFAT_RESIZE_INVALID_FILESYSTEM);
	destroy_fixture(&fixture);
}

static void test_arguments_workspace_and_io_errors(void)
{
	struct exfat_resize_geometry geometry;
	struct exfat_resize_geometry unchanged;
	struct boot_fixture fixture;
	enum exfat_resize_error error;
	unsigned char workspace[512];

	initialize_fixture(&fixture, 512);
	if (fixture.image == NULL) {
		destroy_fixture(&fixture);
		return;
	}

	memset(&geometry, 0xa5, sizeof(geometry));
	unchanged = geometry;
	error = exfat_resize_read_boot_regions(
	    &fixture.memory.device, workspace, sizeof(workspace) - 1, &geometry);
	CHECK(error == EXFAT_RESIZE_INSUFFICIENT_WORKSPACE);
	CHECK(memcmp(&geometry, &unchanged, sizeof(geometry)) == 0);
	CHECK(fixture.memory.operation_count == 0);

	error =
	    exfat_resize_read_boot_regions(&fixture.memory.device, NULL, sizeof(workspace), &geometry);
	CHECK(error == EXFAT_RESIZE_INVALID_ARGUMENT);
	error =
	    exfat_resize_read_boot_regions(&fixture.memory.device, workspace, sizeof(workspace), NULL);
	CHECK(error == EXFAT_RESIZE_INVALID_ARGUMENT);
	error = exfat_resize_read_boot_regions(NULL, workspace, sizeof(workspace), &geometry);
	CHECK(error == EXFAT_RESIZE_INVALID_ARGUMENT);
	CHECK(fixture.memory.operation_count == 0);

	memory_block_device_fail_operation(&fixture.memory, 0, 1234);
	error = exfat_resize_read_boot_regions(
	    &fixture.memory.device, workspace, sizeof(workspace), &geometry);
	CHECK(error == EXFAT_RESIZE_IO_ERROR);
	CHECK(memcmp(&geometry, &unchanged, sizeof(geometry)) == 0);
	CHECK(fixture.memory.operation_count == 1);

	memory_block_device_clear_failure(&fixture.memory);
	memory_block_device_clear_operations(&fixture.memory);
	memory_block_device_fail_operation(&fixture.memory, 23, 1234);
	error = exfat_resize_read_boot_regions(
	    &fixture.memory.device, workspace, sizeof(workspace), &geometry);
	CHECK(error == EXFAT_RESIZE_IO_ERROR);
	CHECK(memcmp(&geometry, &unchanged, sizeof(geometry)) == 0);
	CHECK(fixture.memory.operation_count == 24);

	destroy_fixture(&fixture);
}

static void test_sector_size_probe(void)
{
	struct memory_block_device device;
	struct boot_fixture fixture;
	enum exfat_resize_error error;
	unsigned char workspace[512];
	uint32_t filesystem_sector_size = 0;

	initialize_fixture(&fixture, 4096);
	if (fixture.image == NULL) {
		destroy_fixture(&fixture);
		return;
	}
	memory_block_device_init(&device, 512, fixture.volume_sector_count * 8);
	error = exfat_resize_block_device_write(&device.device, 0, 1, fixture.image, sizeof(workspace));
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	memory_block_device_clear_operations(&device);

	error = exfat_resize_probe_sector_size(
	    &device.device, workspace, sizeof(workspace), &filesystem_sector_size);
	CHECK(error == EXFAT_RESIZE_SUCCESS);
	CHECK(filesystem_sector_size == 4096);
	CHECK(device.operation_count == 1);
	CHECK(device.operations[0].first_sector == 0);
	CHECK(device.operations[0].sector_count == 1);

	memory_block_device_destroy(&device);
	destroy_fixture(&fixture);
}

int main(void)
{
	test_valid_boot_regions(512);
	test_valid_boot_regions(1024);
	test_valid_boot_regions(2048);
	test_valid_boot_regions(4096);
	test_stale_backup_fields();
	test_independent_region_checksums();
	test_checksums_and_region_consistency();
	test_required_boot_structures();
	test_unsupported_boot_values();
	test_invalid_geometry();
	test_arguments_workspace_and_io_errors();
	test_sector_size_probe();

	if (failure_count != 0) {
		fprintf(stderr, "%d boot-region test(s) failed\n", failure_count);
		return 1;
	}

	printf("boot region: passed\n");
	return 0;
}
