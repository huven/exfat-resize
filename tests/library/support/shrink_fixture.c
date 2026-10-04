/* SPDX-License-Identifier: MIT */
#include "support/shrink_fixture.h"
#include "boot_region.h"
#include "endian.h"
#include "stream.h"
#include <stdlib.h>
#include <string.h>

#define TRY(expression) \
	do { \
		if ((expression) != 0) \
			goto fail; \
	} while (0)

static int read_cluster(struct exfat_fixture *f, uint32_t cluster, unsigned char *buffer)
{
	return f->memory.device.read(&f->memory, exfat_fixture_cluster_sector(&f->geometry, cluster),
	    f->geometry.sectors_per_cluster, buffer);
}

static int write_cluster(struct exfat_fixture *f, uint32_t cluster, const unsigned char *buffer)
{
	return f->memory.device.write(&f->memory, exfat_fixture_cluster_sector(&f->geometry, cluster),
	    f->geometry.sectors_per_cluster, buffer);
}

uint32_t shrink_fixture_fat(struct exfat_fixture *f, uint32_t cluster)
{
	unsigned char sector[512];
	uint32_t next = 0;
	if (exfat_fixture_read_sector(
	        f, f->geometry.fat_offset + cluster / 128, sector, sizeof(sector)) != 0)
		return 0;
	if (exfat_resize_load_le32(sector, sizeof(sector), cluster % 128 * 4, &next) !=
	    EXFAT_RESIZE_SUCCESS)
		return 0;
	return next;
}

int shrink_fixture_set_fat(struct exfat_fixture *f, uint32_t cluster, uint32_t value)
{
	unsigned char sector[512];
	uint64_t number = f->geometry.fat_offset + cluster / 128;
	if (exfat_fixture_read_sector(f, number, sector, sizeof(sector)) != 0 ||
	    exfat_resize_store_le32(sector, sizeof(sector), cluster % 128 * 4, value) !=
	        EXFAT_RESIZE_SUCCESS)
		return -1;
	return f->memory.device.write(&f->memory, number, 1, sector);
}

int shrink_fixture_set_bit(struct exfat_fixture *f, uint32_t cluster, int allocated)
{
	unsigned char sector[512];
	uint32_t byte = (cluster - 2) / 8;
	uint32_t cluster_size = f->geometry.sectors_per_cluster * 512;
	uint64_t number =
	    exfat_fixture_cluster_sector(&f->geometry, f->bitmap_clusters[byte / cluster_size]) +
	    byte % cluster_size / 512;
	unsigned char mask = (unsigned char)(1u << ((cluster - 2) % 8));
	if (exfat_fixture_read_sector(f, number, sector, sizeof(sector)) != 0)
		return -1;
	if (allocated)
		sector[byte % 512] |= mask;
	else
		sector[byte % 512] &= (unsigned char)~mask;
	return f->memory.device.write(&f->memory, number, 1, sector);
}

static uint16_t checksum(const unsigned char *entries)
{
	uint16_t sum = 0;
	size_t i;
	for (i = 0; i < 96; ++i) {
		if (i != 2 && i != 3)
			sum = (uint16_t)(((sum << 15) | (sum >> 1)) + entries[i]);
	}
	return sum;
}

static void file(unsigned char *entry,
    uint32_t cluster,
    uint64_t length,
    int contiguous,
    int directory,
    unsigned char name)
{
	memset(entry, 0, 96);
	entry[0] = 0x85;
	entry[1] = 2;
	entry[4] = directory ? 0x10 : 0x21;
	entry[8] = 0x51; /* Metadata which relocation must preserve. */
	entry[32] = 0xc0;
	entry[33] = contiguous ? 3 : 1;
	entry[35] = 1;
	(void)exfat_resize_store_le64(entry, 96, 40, directory ? length : length / 2);
	(void)exfat_resize_store_le32(entry, 96, 52, cluster);
	(void)exfat_resize_store_le64(entry, 96, 56, length);
	entry[64] = 0xc1;
	entry[66] = name;
	(void)exfat_resize_store_le16(entry, 96, 2, checksum(entry));
}

uint64_t shrink_fixture_target(const struct exfat_fixture *f)
{
	return f->geometry.cluster_heap_offset + (uint64_t)2000 * f->geometry.sectors_per_cluster;
}

int shrink_fixture_initialize(struct exfat_fixture *f, uint32_t spc)
{
	unsigned char *buffer;
	uint32_t cluster_size = spc * 512;
	const uint32_t c_first = spc == 1 ? 1999 : 2046;
	uint32_t i;
	uint32_t j;
	const uint32_t a[] = { 8000, 50, 8001, 8002 };
	memset(f, 0, sizeof(*f));
	f->geometry =
	    (struct exfat_resize_geometry){ .volume_sector_count = 104 + (uint64_t)10000 * spc,
		    .sectors_per_cluster = spc,
		    .fat_offset = 24,
		    .fat_length = 80,
		    .cluster_heap_offset = 104,
		    .cluster_count = 10000,
		    .root_directory_cluster = 9000 };
	f->bitmap_clusters[0] = 8500;
	f->bitmap_clusters[1] = 8502;
	f->bitmap_clusters[2] = 8501;
	f->bitmap_cluster_count = (1250 + cluster_size - 1) / cluster_size;
	memory_block_device_init(&f->memory, 512, 104 + (uint64_t)20000 * spc);
	buffer = calloc(2, cluster_size);
	if (buffer == NULL)
		goto fail;
	TRY(exfat_fixture_write_boot_regions(f));
	TRY(shrink_fixture_set_fat(f, 0, UINT32_C(0xfffffff8)));
	TRY(shrink_fixture_set_fat(f, 1, EXFAT_FAT_END_OF_CHAIN));
	TRY(shrink_fixture_set_fat(f, 9000, 9001));
	TRY(shrink_fixture_set_fat(f, 9001, EXFAT_FAT_END_OF_CHAIN));
	TRY(shrink_fixture_set_fat(f, 8600, EXFAT_FAT_END_OF_CHAIN));
	for (i = 0; i < f->bitmap_cluster_count; ++i)
		TRY(shrink_fixture_set_fat(f, f->bitmap_clusters[i],
		    i + 1 == f->bitmap_cluster_count ? EXFAT_FAT_END_OF_CHAIN : f->bitmap_clusters[i + 1]));
	for (i = 0; i < 4; ++i) {
		TRY(shrink_fixture_set_fat(f, a[i], i == 3 ? EXFAT_FAT_END_OF_CHAIN : a[i + 1]));
		/* Deliberately stale FAT entries for the contiguous stream. */
		TRY(shrink_fixture_set_fat(f, c_first + i, EXFAT_FAT_BAD_CLUSTER));
	}
	TRY(shrink_fixture_set_fat(f, 60, EXFAT_FAT_BAD_CLUSTER));
	TRY(shrink_fixture_set_fat(f, 9500, EXFAT_FAT_BAD_CLUSTER));
	for (i = 0; i < f->bitmap_cluster_count; ++i)
		TRY(shrink_fixture_set_bit(f, f->bitmap_clusters[i], 1));
	for (i = 0; i < 4; ++i) {
		TRY(shrink_fixture_set_bit(f, a[i], 1));
		TRY(shrink_fixture_set_bit(f, c_first + i, 1));
	}
	for (i = 0; i < 2; ++i) {
		TRY(shrink_fixture_set_bit(f, 9000 + i, 1));
		TRY(shrink_fixture_set_bit(f, 8800 + i, 1));
	}
	TRY(shrink_fixture_set_bit(f, 8600, 1));
	TRY(shrink_fixture_set_bit(f, 8900, 1));
	TRY(shrink_fixture_set_bit(f, 60, 1));
	TRY(shrink_fixture_set_bit(f, 9500, 1));
	buffer[0] = 0x81;
	TRY(exfat_resize_store_le32(buffer, cluster_size * 2, 20, 8500));
	TRY(exfat_resize_store_le64(buffer, cluster_size * 2, 24, 1250));
	buffer[32] = 0x82;
	TRY(exfat_resize_store_le32(buffer, cluster_size * 2, 52, 8600));
	TRY(exfat_resize_store_le64(buffer, cluster_size * 2, 56, cluster_size));
	file(buffer + 64, c_first, (uint64_t)4 * cluster_size, 1, 0, 'C');
	file(buffer + 160, 8800, (uint64_t)2 * cluster_size, 1, 1, 'D');
	for (i = 256; i < cluster_size - 32; i += 32)
		buffer[i] = 1; /* Inactive entries, not end-of-directory markers. */
	file(buffer + cluster_size - 32, 8000, (uint64_t)4 * cluster_size - 13, 0, 0, 'A');
	TRY(write_cluster(f, 9000, buffer));
	TRY(write_cluster(f, 9001, buffer + cluster_size));
	memset(buffer, 0, cluster_size * 2);
	for (i = 0; i < cluster_size - 32; i += 32)
		buffer[i] = 1;
	file(buffer + cluster_size - 32, 8900, cluster_size, 1, 0, 'N');
	TRY(write_cluster(f, 8800, buffer));
	TRY(write_cluster(f, 8801, buffer + cluster_size));
	for (i = 0; i < 4; ++i) {
		memset(buffer, 0xa0 + i, cluster_size);
		TRY(write_cluster(f, a[i], buffer));
		memset(buffer, 0xc0 + i, cluster_size);
		TRY(write_cluster(f, c_first + i, buffer));
	}
	for (j = 0; j < 2; ++j) {
		memset(buffer, j == 0 ? 0x44 : 0x88, cluster_size);
		TRY(write_cluster(f, j == 0 ? 8600 : 8900, buffer));
	}
	free(buffer);
	buffer = NULL;
	TRY(memory_block_device_make_durable(&f->memory));
	memory_block_device_clear_operations(&f->memory);
	return 0;
fail:
	free(buffer);
	(void)exfat_fixture_destroy(f);
	return -1;
}

static int verify_file(struct exfat_fixture *f,
    const unsigned char *entry,
    uint64_t length,
    unsigned char marker,
    uint32_t cluster_count)
{
	unsigned char *data = malloc(f->geometry.sectors_per_cluster * 512);
	uint32_t first = 0;
	uint64_t actual_length = 0;
	uint64_t valid = 0;
	uint64_t remaining = length;
	uint16_t sum = 0;
	uint32_t index = 0;
	int result = -1;
	if (data == NULL)
		return -1;
	TRY(exfat_resize_load_le32(entry, 96, 52, &first));
	TRY(exfat_resize_load_le64(entry, 96, 56, &actual_length));
	TRY(exfat_resize_load_le64(entry, 96, 40, &valid));
	TRY(exfat_resize_load_le16(entry, 96, 2, &sum));
	if (actual_length != length || valid != length / 2 || sum != checksum(entry) ||
	    entry[4] != 0x21 || entry[8] != 0x51)
		goto fail;
	while (remaining != 0) {
		size_t i;
		size_t size = f->geometry.sectors_per_cluster * 512;
		if (first < 2 || first > cluster_count + 1)
			goto fail;
		TRY(read_cluster(f, first, data));
		if (size > remaining)
			size = (size_t)remaining;
		for (i = 0; i < size; ++i) {
			if (data[i] != marker + index)
				goto fail;
		}
		remaining -= size;
		++index;
		first = entry[33] & 2 ? first + 1 : shrink_fixture_fat(f, first);
	}
	if (!(entry[33] & 2) && first != EXFAT_FAT_END_OF_CHAIN)
		goto fail;
	result = 0;
fail:
	free(data);
	return result;
}

int shrink_fixture_verify(struct exfat_fixture *f, uint32_t cluster_count)
{
	uint32_t size = f->geometry.sectors_per_cluster * 512;
	unsigned char *buffer = malloc(size * 2);
	unsigned char boot[512];
	struct exfat_resize_geometry geometry;
	uint32_t directory = 0;
	uint32_t upcase = 0;
	uint32_t next;
	uint64_t length = 0;
	uint16_t sum = 0;
	uint32_t i;
	int result = -1;
	if (buffer == NULL)
		return -1;
	TRY(exfat_resize_read_boot_regions(&f->memory.device, boot, sizeof(boot), &geometry));
	if (geometry.cluster_count != cluster_count ||
	    geometry.cluster_heap_offset != f->geometry.cluster_heap_offset)
		goto fail;
	TRY(read_cluster(f, geometry.root_directory_cluster, buffer));
	next = shrink_fixture_fat(f, geometry.root_directory_cluster);
	if (next < 2 || next > cluster_count + 1)
		goto fail;
	TRY(read_cluster(f, next, buffer + size));
	TRY(verify_file(f, buffer + 64, (uint64_t)4 * size, 0xc0, cluster_count));
	TRY(verify_file(f, buffer + size - 32, (uint64_t)4 * size - 13, 0xa0, cluster_count));
	TRY(exfat_resize_load_le32(buffer, size * 2, 160 + 52, &directory));
	TRY(exfat_resize_load_le64(buffer, size * 2, 160 + 40, &length));
	TRY(exfat_resize_load_le16(buffer, size * 2, 162, &sum));
	if (sum != checksum(buffer + 160) || length != (uint64_t)2 * size)
		goto fail;
	next = buffer[160 + 33] & 2 ? directory + 1 : shrink_fixture_fat(f, directory);
	TRY(exfat_resize_load_le32(buffer, size * 2, 52, &upcase));
	if (directory < 2 || directory > cluster_count + 1 || next < 2 || next > cluster_count + 1 ||
	    upcase < 2 || upcase > cluster_count + 1)
		goto fail;
	TRY(read_cluster(f, directory, buffer));
	TRY(read_cluster(f, next, buffer + size));
	TRY(verify_file(f, buffer + size - 32, size, 0x88, cluster_count));
	TRY(read_cluster(f, upcase, buffer));
	for (i = 0; i < size; ++i) {
		if (buffer[i] != 0x44)
			goto fail;
	}
	if (shrink_fixture_fat(f, 60) != EXFAT_FAT_BAD_CLUSTER)
		goto fail;
	result = 0;
fail:
	free(buffer);
	return result;
}
