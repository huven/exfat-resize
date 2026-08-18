/* SPDX-License-Identifier: MIT */

#include "support/exfat_fixture.h"

#include "block_device.h"
#include "endian.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
	SECTOR_SIZE = 512,
	BOOT_REGION_SECTORS = 12,
	BOOT_CHECKSUM_SECTOR = 11,
	BACKUP_BOOT_REGION = 12,

	VOLUME_LENGTH_OFFSET = 72,
	FAT_OFFSET_OFFSET = 80,
	FAT_LENGTH_OFFSET = 84,
	CLUSTER_HEAP_OFFSET_OFFSET = 88,
	CLUSTER_COUNT_OFFSET = 92,
	ROOT_DIRECTORY_CLUSTER_OFFSET = 96,
	FILESYSTEM_REVISION_OFFSET = 104,
	BYTES_PER_SECTOR_SHIFT_OFFSET = 108,
	SECTORS_PER_CLUSTER_SHIFT_OFFSET = 109,
	NUMBER_OF_FATS_OFFSET = 110,
	PERCENT_IN_USE_OFFSET = 112,

	ENTRY_SIZE = 32,
	ENTRY_BITMAP = 0x81,
	ENTRY_UPCASE = 0x82,
	ENTRY_FILE = 0x85,
	ENTRY_STREAM = 0xc0,
	ENTRY_FILE_NAME = 0xc1,
	NO_FAT_CHAIN = 0x02,
	ALLOCATION_POSSIBLE = 0x01,
	READ_ONLY_ATTRIBUTE = 0x01,
	HIDDEN_ATTRIBUTE = 0x02,
	SYSTEM_ATTRIBUTE = 0x04,
	DIRECTORY_ATTRIBUTE = 0x10,
	ARCHIVE_ATTRIBUTE = 0x20,

	FILE_ATTRIBUTES_OFFSET = 4,
	FILE_CREATE_TIMESTAMP_OFFSET = 8,
	FILE_MODIFIED_TIMESTAMP_OFFSET = 12,
	FILE_ACCESS_TIMESTAMP_OFFSET = 16,
	FILE_CREATE_INCREMENT_OFFSET = 20,
	FILE_MODIFIED_INCREMENT_OFFSET = 21,
	FILE_CREATE_UTC_OFFSET = 22,
	FILE_MODIFIED_UTC_OFFSET = 23,
	FILE_ACCESS_UTC_OFFSET = 24
};

static uint32_t boot_checksum_byte(uint32_t checksum, unsigned char value)
{
	return ((checksum << 31) | (checksum >> 1)) + value;
}

static uint16_t entry_checksum_byte(uint16_t checksum, unsigned char value)
{
	uint16_t rotated = (uint16_t)(((uint32_t)checksum << 15) | ((uint32_t)checksum >> 1));

	return (uint16_t)(rotated + value);
}

static uint16_t entry_set_checksum(const unsigned char entries[ENTRY_SIZE * 3])
{
	uint16_t checksum = 0;
	size_t index;

	for (index = 0; index < ENTRY_SIZE * 3; ++index) {
		if (index != 2 && index != 3)
			checksum = entry_checksum_byte(checksum, entries[index]);
	}
	return checksum;
}

static uint32_t file_timestamp(unsigned int seed)
{
	uint32_t year = 2026 - 1980;
	uint32_t month = seed % 12 + 1;
	uint32_t day = seed % 28 + 1;
	uint32_t hour = seed % 24;
	uint32_t minute = seed % 60;
	uint32_t double_second = seed % 30;

	return (year << 25) | (month << 21) | (day << 16) | (hour << 11) | (minute << 5) |
	    double_second;
}

static int write_sectors(
    struct exfat_fixture *fixture, uint64_t sector, uint32_t count, const void *buffer)
{
	return fixture->memory.device.write(fixture->memory.device.context, sector, count, buffer);
}

uint64_t exfat_fixture_cluster_sector(
    const struct exfat_resize_geometry *geometry, uint32_t cluster)
{
	return geometry->cluster_heap_offset + (uint64_t)(cluster - 2) * geometry->sectors_per_cluster;
}

int exfat_fixture_write_boot_regions(struct exfat_fixture *fixture)
{
	unsigned char *regions;
	unsigned char *sector;
	uint32_t sectors_per_cluster;
	uint32_t region;
	uint32_t sector_index;
	uint32_t checksum;
	unsigned char sectors_per_cluster_shift = 0;
	size_t offset;
	int result = 0;

	sectors_per_cluster = fixture->geometry.sectors_per_cluster;
	if (sectors_per_cluster == 0)
		return -1;
	while (sectors_per_cluster > 1) {
		if ((sectors_per_cluster & 1) != 0)
			return -1;
		sectors_per_cluster >>= 1;
		++sectors_per_cluster_shift;
	}

	regions = calloc(BOOT_REGION_SECTORS * 2, SECTOR_SIZE);
	if (regions == NULL)
		return -1;

	for (region = 0; region < 2; ++region) {
		unsigned char *first = regions + (size_t)region * BOOT_REGION_SECTORS * SECTOR_SIZE;

		first[0] = 0xeb;
		first[1] = 0x76;
		first[2] = 0x90;
		memcpy(first + 3, "EXFAT   ", 8);
		if (exfat_resize_store_le64(first, SECTOR_SIZE, VOLUME_LENGTH_OFFSET,
		        fixture->geometry.volume_sector_count) != EXFAT_RESIZE_SUCCESS ||
		    exfat_resize_store_le32(first, SECTOR_SIZE, FAT_OFFSET_OFFSET,
		        fixture->geometry.fat_offset) != EXFAT_RESIZE_SUCCESS ||
		    exfat_resize_store_le32(first, SECTOR_SIZE, FAT_LENGTH_OFFSET,
		        fixture->geometry.fat_length) != EXFAT_RESIZE_SUCCESS ||
		    exfat_resize_store_le32(first, SECTOR_SIZE, CLUSTER_HEAP_OFFSET_OFFSET,
		        fixture->geometry.cluster_heap_offset) != EXFAT_RESIZE_SUCCESS ||
		    exfat_resize_store_le32(first, SECTOR_SIZE, CLUSTER_COUNT_OFFSET,
		        fixture->geometry.cluster_count) != EXFAT_RESIZE_SUCCESS ||
		    exfat_resize_store_le32(first, SECTOR_SIZE, ROOT_DIRECTORY_CLUSTER_OFFSET,
		        fixture->geometry.root_directory_cluster) != EXFAT_RESIZE_SUCCESS ||
		    exfat_resize_store_le16(first, SECTOR_SIZE, FILESYSTEM_REVISION_OFFSET,
		        UINT16_C(0x0100)) != EXFAT_RESIZE_SUCCESS ||
		    exfat_resize_store_le16(first, SECTOR_SIZE, 510, UINT16_C(0xaa55)) !=
		        EXFAT_RESIZE_SUCCESS) {
			result = -1;
			goto done;
		}
		first[BYTES_PER_SECTOR_SHIFT_OFFSET] = 9;
		first[SECTORS_PER_CLUSTER_SHIFT_OFFSET] = sectors_per_cluster_shift;
		first[NUMBER_OF_FATS_OFFSET] = 1;
		first[PERCENT_IN_USE_OFFSET] = 3;

		for (sector_index = 1; sector_index <= 8; ++sector_index) {
			sector = first + (size_t)sector_index * SECTOR_SIZE;
			if (exfat_resize_store_le32(sector, SECTOR_SIZE, SECTOR_SIZE - 4,
			        UINT32_C(0xaa550000)) != EXFAT_RESIZE_SUCCESS) {
				result = -1;
				goto done;
			}
		}

		checksum = 0;
		for (sector_index = 0; sector_index < BOOT_CHECKSUM_SECTOR; ++sector_index) {
			sector = first + (size_t)sector_index * SECTOR_SIZE;
			for (offset = 0; offset < SECTOR_SIZE; ++offset) {
				if (sector_index == 0 && (offset == 106 || offset == 107 || offset == 112))
					continue;
				checksum = boot_checksum_byte(checksum, sector[offset]);
			}
		}
		sector = first + (size_t)BOOT_CHECKSUM_SECTOR * SECTOR_SIZE;
		for (offset = 0; offset < SECTOR_SIZE; offset += 4) {
			if (exfat_resize_store_le32(sector, SECTOR_SIZE, offset, checksum) !=
			    EXFAT_RESIZE_SUCCESS) {
				result = -1;
				goto done;
			}
		}
	}

	result = write_sectors(fixture, 0, BOOT_REGION_SECTORS * 2, regions);

done:
	free(regions);
	return result;
}

static int initialize_fat(struct exfat_fixture *fixture)
{
	unsigned char *fat;
	size_t fat_size = (size_t)fixture->geometry.fat_length * SECTOR_SIZE;
	uint32_t index;
	int result = -1;

	fat = calloc(1, fat_size);
	if (fat == NULL)
		return -1;
	if (exfat_resize_store_le32(fat, fat_size, 0, UINT32_C(0xfffffff8)) != EXFAT_RESIZE_SUCCESS ||
	    exfat_resize_store_le32(fat, fat_size, 4, UINT32_C(0xffffffff)) != EXFAT_RESIZE_SUCCESS ||
	    exfat_resize_store_le32(fat, fat_size, 2 * 4, UINT32_C(0xffffffff)) !=
	        EXFAT_RESIZE_SUCCESS ||
	    exfat_resize_store_le32(fat, fat_size, 4 * 4, UINT32_C(0xffffffff)) != EXFAT_RESIZE_SUCCESS)
		goto done;

	for (index = 0; index < fixture->bitmap_cluster_count; ++index) {
		uint32_t next = index + 1 == fixture->bitmap_cluster_count
		    ? UINT32_C(0xffffffff)
		    : fixture->bitmap_clusters[index + 1];

		if (exfat_resize_store_le32(fat, fat_size, fixture->bitmap_clusters[index] * 4, next) !=
		    EXFAT_RESIZE_SUCCESS)
			goto done;
	}
	for (index = 0; index < 3; ++index) {
		uint32_t next = index == 2 ? UINT32_C(0xffffffff) : fixture->fragmented_clusters[index + 1];

		if (exfat_resize_store_le32(fat, fat_size, fixture->fragmented_clusters[index] * 4, next) !=
		    EXFAT_RESIZE_SUCCESS)
			goto done;
	}

	result =
	    write_sectors(fixture, fixture->geometry.fat_offset, fixture->geometry.fat_length, fat);

done:
	free(fat);
	return result;
}

static void set_bitmap_cluster(unsigned char *bitmap, uint32_t cluster)
{
	uint32_t bit = cluster - 2;
	bitmap[bit / 8] |= (unsigned char)(1u << (bit % 8));
}

static int write_bitmap_cluster_state(
    struct exfat_fixture *fixture, uint32_t cluster, int allocated)
{
	unsigned char sector[SECTOR_SIZE];
	uint32_t bit;
	uint32_t bitmap_index;
	uint32_t sector_in_cluster;
	uint64_t sector_number;
	uint64_t cluster_size = (uint64_t)fixture->geometry.sectors_per_cluster * SECTOR_SIZE;

	if (cluster < 2 || cluster > fixture->geometry.cluster_count + 1)
		return -1;
	bit = cluster - 2;
	bitmap_index = (uint32_t)((bit / 8) / cluster_size);
	if (bitmap_index >= fixture->bitmap_cluster_count)
		return -1;
	sector_in_cluster = (uint32_t)(((bit / 8) % cluster_size) / SECTOR_SIZE);
	sector_number =
	    exfat_fixture_cluster_sector(&fixture->geometry, fixture->bitmap_clusters[bitmap_index]) +
	    sector_in_cluster;
	if (fixture->memory.device.read(fixture->memory.device.context, sector_number, 1, sector) != 0)
		return -1;
	if (allocated)
		sector[bit / 8 % SECTOR_SIZE] |= (unsigned char)(1u << (bit % 8));
	else
		sector[bit / 8 % SECTOR_SIZE] &= (unsigned char)~(1u << (bit % 8));
	return write_sectors(fixture, sector_number, 1, sector);
}

static int write_bitmap_cluster(struct exfat_fixture *fixture, uint32_t cluster)
{
	return write_bitmap_cluster_state(fixture, cluster, 1);
}

static int initialize_bitmap(struct exfat_fixture *fixture)
{
	unsigned char *bitmap;
	uint64_t bitmap_length = ((uint64_t)fixture->geometry.cluster_count + 7) / 8;
	uint64_t allocation_sector_count;
	uint32_t cluster;
	uint64_t sector_index;
	int result = -1;

	bitmap = calloc(1, (size_t)bitmap_length);
	if (bitmap == NULL)
		return -1;
	set_bitmap_cluster(bitmap, 2);
	set_bitmap_cluster(bitmap, 4);
	set_bitmap_cluster(bitmap, 5);
	set_bitmap_cluster(bitmap, 6);
	set_bitmap_cluster(bitmap, 8);
	for (cluster = 0; cluster < fixture->bitmap_cluster_count; ++cluster)
		set_bitmap_cluster(bitmap, fixture->bitmap_clusters[cluster]);
	for (cluster = 0; cluster < 3; ++cluster)
		set_bitmap_cluster(bitmap, fixture->fragmented_clusters[cluster]);
	for (cluster = fixture->crossing_first_cluster;
	    cluster < fixture->crossing_first_cluster + fixture->crossing_cluster_count; ++cluster)
		set_bitmap_cluster(bitmap, cluster);

	allocation_sector_count =
	    (bitmap_length + (uint64_t)fixture->geometry.sectors_per_cluster * SECTOR_SIZE - 1) /
	    ((uint64_t)fixture->geometry.sectors_per_cluster * SECTOR_SIZE) *
	    fixture->geometry.sectors_per_cluster;
	for (sector_index = 0; sector_index < allocation_sector_count; ++sector_index) {
		size_t offset = (size_t)sector_index * SECTOR_SIZE;
		size_t remaining = offset < bitmap_length ? (size_t)bitmap_length - offset : 0;
		size_t count = remaining < SECTOR_SIZE ? remaining : SECTOR_SIZE;
		uint32_t bitmap_index = (uint32_t)(sector_index / fixture->geometry.sectors_per_cluster);
		uint32_t sector_in_cluster =
		    (uint32_t)(sector_index % fixture->geometry.sectors_per_cluster);
		unsigned char sector[SECTOR_SIZE] = { 0 };

		if (count != 0)
			memcpy(sector, bitmap + offset, count);
		if (write_sectors(fixture,
		        exfat_fixture_cluster_sector(
		            &fixture->geometry, fixture->bitmap_clusters[bitmap_index]) +
		            sector_in_cluster,
		        1, sector) != 0)
			goto done;
	}
	result = 0;

done:
	free(bitmap);
	return result;
}

static int append_file_set(unsigned char directory[SECTOR_SIZE],
    size_t *offset,
    uint16_t attributes,
    uint32_t first_cluster,
    uint64_t data_length,
    int no_fat_chain,
    unsigned char name)
{
	unsigned char *entries;
	uint16_t checksum;

	if (*offset > SECTOR_SIZE - ENTRY_SIZE * 3)
		return -1;
	entries = directory + *offset;
	entries[0] = ENTRY_FILE;
	entries[1] = 2;
	if (exfat_resize_store_le16(entries, ENTRY_SIZE, FILE_ATTRIBUTES_OFFSET, attributes) !=
	        EXFAT_RESIZE_SUCCESS ||
	    exfat_resize_store_le32(entries, ENTRY_SIZE, FILE_CREATE_TIMESTAMP_OFFSET,
	        file_timestamp(name)) != EXFAT_RESIZE_SUCCESS ||
	    exfat_resize_store_le32(entries, ENTRY_SIZE, FILE_MODIFIED_TIMESTAMP_OFFSET,
	        file_timestamp((unsigned int)name + 31)) != EXFAT_RESIZE_SUCCESS ||
	    exfat_resize_store_le32(entries, ENTRY_SIZE, FILE_ACCESS_TIMESTAMP_OFFSET,
	        file_timestamp((unsigned int)name + 67)) != EXFAT_RESIZE_SUCCESS)
		return -1;
	entries[FILE_CREATE_INCREMENT_OFFSET] = (unsigned char)((unsigned int)name % 200);
	entries[FILE_MODIFIED_INCREMENT_OFFSET] = (unsigned char)(((unsigned int)name + 73) % 200);
	entries[FILE_CREATE_UTC_OFFSET] = (unsigned char)(0x80 | ((unsigned int)name % 8));
	entries[FILE_MODIFIED_UTC_OFFSET] = (unsigned char)(0x80 | (((unsigned int)name + 3) % 8));
	entries[FILE_ACCESS_UTC_OFFSET] = (unsigned char)(0x80 | (((unsigned int)name + 6) % 8));

	entries[ENTRY_SIZE] = ENTRY_STREAM;
	entries[ENTRY_SIZE + 1] = ALLOCATION_POSSIBLE | (no_fat_chain ? NO_FAT_CHAIN : 0);
	entries[ENTRY_SIZE + 3] = 1;
	if (exfat_resize_store_le64(entries + ENTRY_SIZE, ENTRY_SIZE, 8, data_length) !=
	        EXFAT_RESIZE_SUCCESS ||
	    exfat_resize_store_le32(entries + ENTRY_SIZE, ENTRY_SIZE, 20, first_cluster) !=
	        EXFAT_RESIZE_SUCCESS ||
	    exfat_resize_store_le64(entries + ENTRY_SIZE, ENTRY_SIZE, 24, data_length) !=
	        EXFAT_RESIZE_SUCCESS)
		return -1;

	entries[ENTRY_SIZE * 2] = ENTRY_FILE_NAME;
	if (exfat_resize_store_le16(entries + ENTRY_SIZE * 2, ENTRY_SIZE, 2, name) !=
	    EXFAT_RESIZE_SUCCESS)
		return -1;
	checksum = entry_set_checksum(entries);
	if (exfat_resize_store_le16(entries, ENTRY_SIZE, 2, checksum) != EXFAT_RESIZE_SUCCESS)
		return -1;
	*offset += ENTRY_SIZE * 3;
	return 0;
}

static size_t directory_end(const unsigned char directory[SECTOR_SIZE])
{
	size_t offset = 0;

	while (offset < SECTOR_SIZE && directory[offset] != 0)
		offset += ENTRY_SIZE;
	return offset;
}

int exfat_fixture_add_child_directories(
    struct exfat_fixture *fixture, uint32_t first_cluster, uint32_t count)
{
	unsigned char child[SECTOR_SIZE];
	unsigned char empty[SECTOR_SIZE] = { 0 };
	size_t offset;
	uint32_t index;

	if (count == 0 ||
	    (uint64_t)first_cluster + count > (uint64_t)fixture->geometry.cluster_count + 2)
		return -1;
	if (exfat_fixture_read_sector(fixture, exfat_fixture_cluster_sector(&fixture->geometry, 6),
	        child, sizeof(child)) != 0)
		return -1;
	offset = directory_end(child);
	for (index = 0; index < count; ++index) {
		if (append_file_set(child, &offset, DIRECTORY_ATTRIBUTE, first_cluster + index, SECTOR_SIZE,
		        1, (unsigned char)('a' + index)) != 0)
			return -1;
	}
	if (write_sectors(fixture, exfat_fixture_cluster_sector(&fixture->geometry, 6), 1, child) != 0)
		return -1;
	for (index = 0; index < count; ++index) {
		if (write_sectors(fixture,
		        exfat_fixture_cluster_sector(&fixture->geometry, first_cluster + index), 1,
		        empty) != 0 ||
		    write_bitmap_cluster(fixture, first_cluster + index) != 0)
			return -1;
	}
	return 0;
}

int exfat_fixture_add_directory_chain(
    struct exfat_fixture *fixture, uint32_t first_cluster, uint32_t count)
{
	unsigned char child[SECTOR_SIZE];
	size_t offset;
	uint32_t index;

	if (count == 0 ||
	    (uint64_t)first_cluster + count > (uint64_t)fixture->geometry.cluster_count + 2)
		return -1;
	if (exfat_fixture_read_sector(fixture, exfat_fixture_cluster_sector(&fixture->geometry, 6),
	        child, sizeof(child)) != 0)
		return -1;
	offset = directory_end(child);
	if (append_file_set(child, &offset, DIRECTORY_ATTRIBUTE, first_cluster, SECTOR_SIZE, 1, 'd') !=
	        0 ||
	    write_sectors(fixture, exfat_fixture_cluster_sector(&fixture->geometry, 6), 1, child) != 0)
		return -1;

	for (index = 0; index < count; ++index) {
		unsigned char directory[SECTOR_SIZE] = { 0 };

		offset = 0;
		if (index + 1 < count &&
		    append_file_set(directory, &offset, DIRECTORY_ATTRIBUTE, first_cluster + index + 1,
		        SECTOR_SIZE, 1, 'd') != 0)
			return -1;
		if (write_sectors(fixture,
		        exfat_fixture_cluster_sector(&fixture->geometry, first_cluster + index), 1,
		        directory) != 0 ||
		    write_bitmap_cluster(fixture, first_cluster + index) != 0)
			return -1;
	}
	return 0;
}

static int initialize_directories(struct exfat_fixture *fixture)
{
	unsigned char root[SECTOR_SIZE] = { 0 };
	unsigned char child[SECTOR_SIZE] = { 0 };
	uint64_t bitmap_length = ((uint64_t)fixture->geometry.cluster_count + 7) / 8;
	size_t offset = 0;

	root[offset] = ENTRY_BITMAP;
	if (exfat_resize_store_le32(root + offset, ENTRY_SIZE, 20, fixture->bitmap_clusters[0]) !=
	        EXFAT_RESIZE_SUCCESS ||
	    exfat_resize_store_le64(root + offset, ENTRY_SIZE, 24, bitmap_length) !=
	        EXFAT_RESIZE_SUCCESS)
		return -1;
	offset += ENTRY_SIZE;

	root[offset] = ENTRY_UPCASE;
	if (exfat_resize_store_le32(root + offset, ENTRY_SIZE, 20, 4) != EXFAT_RESIZE_SUCCESS ||
	    exfat_resize_store_le64(root + offset, ENTRY_SIZE, 24, SECTOR_SIZE) != EXFAT_RESIZE_SUCCESS)
		return -1;
	offset += ENTRY_SIZE;

	if (append_file_set(root, &offset, ARCHIVE_ATTRIBUTE, 5, SECTOR_SIZE, 1, 'A') != 0 ||
	    append_file_set(root, &offset, DIRECTORY_ATTRIBUTE, 6, SECTOR_SIZE, 1, 'D') != 0 ||
	    append_file_set(root, &offset, ARCHIVE_ATTRIBUTE | READ_ONLY_ATTRIBUTE,
	        fixture->crossing_first_cluster,
	        (uint64_t)fixture->crossing_cluster_count * fixture->geometry.sectors_per_cluster *
	            SECTOR_SIZE,
	        1, 'C') != 0 ||
	    append_file_set(root, &offset, ARCHIVE_ATTRIBUTE | HIDDEN_ATTRIBUTE,
	        fixture->fragmented_clusters[0],
	        (uint64_t)3 * fixture->geometry.sectors_per_cluster * SECTOR_SIZE, 0, 'F') != 0)
		return -1;

	offset = 0;
	if (append_file_set(
	        child, &offset, ARCHIVE_ATTRIBUTE | SYSTEM_ATTRIBUTE, 8, SECTOR_SIZE, 1, 'N') != 0)
		return -1;

	if (write_sectors(fixture, exfat_fixture_cluster_sector(&fixture->geometry, 2), 1, root) != 0 ||
	    write_sectors(fixture, exfat_fixture_cluster_sector(&fixture->geometry, 6), 1, child) != 0)
		return -1;
	return 0;
}

static int write_cluster_marker(
    struct exfat_fixture *fixture, uint32_t cluster, unsigned char marker)
{
	unsigned char sector[SECTOR_SIZE];

	memset(sector, marker, sizeof(sector));
	return write_sectors(
	    fixture, exfat_fixture_cluster_sector(&fixture->geometry, cluster), 1, sector);
}

static unsigned char cluster_pattern_byte(uint32_t cluster, uint64_t cluster_byte_offset)
{
	return (unsigned char)(((uint64_t)cluster * 131 + cluster_byte_offset * 29) % 251 + 1);
}

static int write_cluster_pattern(struct exfat_fixture *fixture, uint32_t cluster)
{
	unsigned char sector[SECTOR_SIZE];
	uint32_t sector_index;
	size_t byte_index;

	for (sector_index = 0; sector_index < fixture->geometry.sectors_per_cluster; ++sector_index) {
		for (byte_index = 0; byte_index < sizeof(sector); ++byte_index) {
			uint64_t cluster_byte_offset = (uint64_t)sector_index * SECTOR_SIZE + byte_index;

			sector[byte_index] = cluster_pattern_byte(cluster, cluster_byte_offset);
		}
		if (write_sectors(fixture,
		        exfat_fixture_cluster_sector(&fixture->geometry, cluster) + sector_index, 1,
		        sector) != 0)
			return -1;
	}
	return 0;
}

int exfat_fixture_add_contiguous_child_directory(struct exfat_fixture *fixture,
    uint32_t directory_first_cluster,
    uint32_t directory_cluster_count,
    uint32_t data_first_cluster)
{
	unsigned char entry_set[SECTOR_SIZE] = { 0 };
	unsigned char root[SECTOR_SIZE];
	uint64_t directory_data_length;
	uint32_t index;
	size_t offset = 0;

	if (fixture->geometry.sectors_per_cluster != 1 || directory_cluster_count == 0 ||
	    directory_cluster_count > 26 || directory_first_cluster < 2 || data_first_cluster < 2 ||
	    (uint64_t)directory_first_cluster + directory_cluster_count >
	        (uint64_t)fixture->geometry.cluster_count + 2 ||
	    (uint64_t)data_first_cluster + directory_cluster_count >
	        (uint64_t)fixture->geometry.cluster_count + 2 ||
	    ((uint64_t)directory_first_cluster <
	            (uint64_t)data_first_cluster + directory_cluster_count &&
	        (uint64_t)data_first_cluster <
	            (uint64_t)directory_first_cluster + directory_cluster_count))
		return -1;
	directory_data_length = (uint64_t)directory_cluster_count * SECTOR_SIZE;
	if (exfat_fixture_read_sector(fixture,
	        exfat_fixture_cluster_sector(
	            &fixture->geometry, fixture->geometry.root_directory_cluster),
	        root, sizeof(root)) != 0 ||
	    append_file_set(entry_set, &offset, DIRECTORY_ATTRIBUTE, directory_first_cluster,
	        directory_data_length, 1, 'D') != 0)
		return -1;
	memcpy(root + ENTRY_SIZE * 5, entry_set, ENTRY_SIZE * 3);
	if (write_sectors(fixture,
	        exfat_fixture_cluster_sector(
	            &fixture->geometry, fixture->geometry.root_directory_cluster),
	        1, root) != 0 ||
	    write_bitmap_cluster_state(fixture, 6, 0) != 0 ||
	    write_bitmap_cluster_state(fixture, 8, 0) != 0)
		return -1;

	for (index = 0; index < directory_cluster_count; ++index) {
		unsigned char directory[SECTOR_SIZE] = { 0 };

		offset = 0;
		if (append_file_set(directory, &offset, ARCHIVE_ATTRIBUTE, data_first_cluster + index,
		        SECTOR_SIZE, 1, (unsigned char)('a' + index)) != 0)
			return -1;
		if (index + 1 < directory_cluster_count) {
			while (offset < sizeof(directory)) {
				directory[offset] = 1;
				offset += ENTRY_SIZE;
			}
		}
		if (write_sectors(fixture,
		        exfat_fixture_cluster_sector(&fixture->geometry, directory_first_cluster + index),
		        1, directory) != 0 ||
		    write_bitmap_cluster(fixture, directory_first_cluster + index) != 0 ||
		    write_bitmap_cluster(fixture, data_first_cluster + index) != 0 ||
		    write_cluster_pattern(fixture, data_first_cluster + index) != 0)
			return -1;
	}
	return 0;
}

int exfat_fixture_initialize_with_sectors_per_cluster(
    struct exfat_fixture *fixture, uint64_t device_sector_count, uint32_t sectors_per_cluster)
{
	uint32_t cluster;
	uint64_t bitmap_length;
	uint64_t cluster_size;

	memset(fixture, 0, sizeof(*fixture));
	if (sectors_per_cluster == 0 || (sectors_per_cluster & (sectors_per_cluster - 1)) != 0)
		return -1;
	fixture->geometry.sectors_per_cluster = sectors_per_cluster;
	fixture->geometry.fat_offset = 24;
	fixture->geometry.fat_length = 96;
	fixture->geometry.cluster_heap_offset = 256;
	fixture->geometry.cluster_count =
	    (12000 - fixture->geometry.cluster_heap_offset) / sectors_per_cluster;
	fixture->geometry.volume_sector_count = fixture->geometry.cluster_heap_offset +
	    (uint64_t)fixture->geometry.cluster_count * sectors_per_cluster;
	fixture->geometry.root_directory_cluster = 2;
	fixture->bitmap_clusters[0] = 3;
	fixture->bitmap_clusters[1] = 20;
	fixture->bitmap_clusters[2] = 7;
	cluster_size = (uint64_t)sectors_per_cluster * SECTOR_SIZE;
	bitmap_length = ((uint64_t)fixture->geometry.cluster_count + 7) / 8;
	fixture->bitmap_cluster_count = (uint32_t)((bitmap_length + cluster_size - 1) / cluster_size);
	fixture->fragmented_clusters[0] = 10;
	fixture->fragmented_clusters[1] = 12;
	fixture->fragmented_clusters[2] = 11;
	fixture->crossing_first_cluster = 100;
	fixture->crossing_cluster_count = 300;
	if (fixture->bitmap_cluster_count == 0 || fixture->bitmap_cluster_count > 3 ||
	    fixture->crossing_first_cluster + fixture->crossing_cluster_count >
	        fixture->geometry.cluster_count + 2)
		return -1;
	memory_block_device_init(&fixture->memory, SECTOR_SIZE, device_sector_count);

	if (exfat_fixture_write_boot_regions(fixture) != 0 || initialize_fat(fixture) != 0 ||
	    initialize_bitmap(fixture) != 0 || initialize_directories(fixture) != 0 ||
	    write_cluster_marker(fixture, 4, 0x44) != 0 ||
	    write_cluster_marker(fixture, 5, 0x55) != 0 ||
	    write_cluster_marker(fixture, 8, 0x88) != 0) {
		exfat_fixture_destroy(fixture);
		return -1;
	}
	for (cluster = 0; cluster < 3; ++cluster) {
		if (write_cluster_pattern(fixture, fixture->fragmented_clusters[cluster]) != 0) {
			exfat_fixture_destroy(fixture);
			return -1;
		}
	}
	for (cluster = fixture->crossing_first_cluster;
	    cluster < fixture->crossing_first_cluster + fixture->crossing_cluster_count; ++cluster) {
		if (write_cluster_pattern(fixture, cluster) != 0) {
			exfat_fixture_destroy(fixture);
			return -1;
		}
	}
	memory_block_device_clear_operations(&fixture->memory);
	return 0;
}

int exfat_fixture_initialize(struct exfat_fixture *fixture, uint64_t device_sector_count)
{
	return exfat_fixture_initialize_with_sectors_per_cluster(fixture, device_sector_count, 1);
}

void exfat_fixture_destroy(struct exfat_fixture *fixture)
{
	memory_block_device_destroy(&fixture->memory);
	memset(fixture, 0, sizeof(*fixture));
}

int exfat_fixture_read_sector(
    struct exfat_fixture *fixture, uint64_t sector, void *buffer, size_t buffer_size)
{
	if (buffer_size < SECTOR_SIZE)
		return -1;
	return fixture->memory.device.read(fixture->memory.device.context, sector, 1, buffer);
}
