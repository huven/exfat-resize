/* SPDX-License-Identifier: MIT */

#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/*
 * Independent manifest for the ordinary File entry sets in platform fixtures.
 * Read unmounted images so filesystem consumers cannot change access times.
 * This is deliberately not a filesystem checker: the platform fsck and mounted
 * content comparison remain separate tests. Hex entry sets retain Unicode names
 * without locale conversion; begin/end records bind them to their directories.
 */
struct volume {
	FILE *image;
	uint64_t image_size;
	uint64_t fat;
	uint64_t heap;
	uint32_t cluster_size;
	uint32_t cluster_count;
	uint32_t directory_clusters_left;
};

struct cursor {
	uint32_t cluster;
	uint32_t offset;
	uint32_t clusters_left;
	int contiguous;
	int root;
};

static void fail(const char *message)
{
	fprintf(stderr, "directory-metadata: %s\n", message);
	exit(EXIT_FAILURE);
}

static uint64_t little(const unsigned char *bytes, size_t count)
{
	uint64_t value = 0;
	while (count != 0)
		value = (value << 8) | bytes[--count];
	return value;
}

static void read_at(struct volume *volume, uint64_t offset, unsigned char *bytes, size_t count)
{
	if (offset > volume->image_size || count > volume->image_size - offset ||
	    fseeko(volume->image, (off_t)offset, SEEK_SET) != 0 ||
	    fread(bytes, 1, count, volume->image) != count)
		fail("cannot read image metadata");
}

static void check_cluster(const struct volume *volume, uint32_t cluster)
{
	if (cluster < 2 || (uint64_t)cluster >= (uint64_t)volume->cluster_count + 2)
		fail("directory cluster is out of range");
}

static uint32_t next_cluster(struct volume *volume, uint32_t cluster)
{
	unsigned char bytes[4];
	read_at(volume, volume->fat + (uint64_t)cluster * 4, bytes, sizeof(bytes));
	return (uint32_t)little(bytes, sizeof(bytes));
}

/* Return zero only at a chain's end, never in the middle of an entry set. */
static int read_entry(struct volume *volume, struct cursor *cursor, unsigned char entry[32])
{
	if (cursor->offset == volume->cluster_size) {
		uint32_t next =
		    cursor->contiguous ? cursor->cluster + 1 : next_cluster(volume, cursor->cluster);
		if (next == UINT32_MAX && !cursor->contiguous) {
			if (!cursor->root && cursor->clusters_left != 0)
				fail("directory chain is shorter than its data length");
			return 0;
		}
		if (cursor->clusters_left == 0) {
			if (cursor->contiguous)
				return 0;
			fail("directory chain exceeds its bound");
		}
		check_cluster(volume, next);
		cursor->cluster = next;
		cursor->offset = 0;
	}
	if (cursor->offset == 0) {
		if (cursor->clusters_left == 0 || volume->directory_clusters_left == 0)
			fail("directory traversal exceeds the volume cluster count");
		--cursor->clusters_left;
		--volume->directory_clusters_left;
	}
	read_at(volume,
	    volume->heap + (uint64_t)(cursor->cluster - 2) * volume->cluster_size + cursor->offset,
	    entry, 32);
	cursor->offset += 32;
	return 1;
}

static void walk_directory(
    struct volume *volume, uint32_t cluster, uint64_t length, int contiguous, unsigned int depth)
{
	struct cursor cursor = { 0 };
	unsigned char entries[256 * 32];

	/* Platform fixtures are shallow; reject malformed recursive output promptly. */
	if (depth > 64)
		fail("directory nesting exceeds the fixture limit");
	check_cluster(volume, cluster);
	cursor.cluster = cluster;
	cursor.contiguous = contiguous;
	cursor.root = depth == 0;
	if (cursor.root) {
		cursor.clusters_left = volume->cluster_count;
	} else {
		if (length == 0 || length % volume->cluster_size != 0 ||
		    length / volume->cluster_size > volume->cluster_count)
			fail("invalid directory data length");
		cursor.clusters_left = (uint32_t)(length / volume->cluster_size);
	}
	puts("begin");
	while (read_entry(volume, &cursor, entries) && entries[0] != 0) {
		unsigned char *stream = entries + 32;
		size_t secondary_count, index, bytes;
		uint32_t child_cluster;
		uint64_t child_length;
		int child_contiguous, directory;

		if (!(entries[0] & 0x80))
			continue;
		if (entries[0] != 0x85) {
			/* Allocation bitmap, up-case table, volume label and volume GUID. */
			if (cursor.root &&
			    (entries[0] == 0x81 || entries[0] == 0x82 || entries[0] == 0x83 ||
			        entries[0] == 0xa0))
				continue;
			fail("unsupported live directory entry in platform fixture");
		}
		secondary_count = entries[1];
		if (secondary_count < 2)
			fail("File entry set lacks stream or filename entries");
		for (index = 1; index <= secondary_count; ++index) {
			if (!read_entry(volume, &cursor, entries + index * 32))
				fail("truncated File entry set");
			if (entries[index * 32] != (index == 1 ? 0xc0 : 0xc1))
				fail("unsupported File secondary entry in platform fixture");
		}
		if (stream[3] == 0 || ((size_t)stream[3] + 14) / 15 != secondary_count - 1)
			fail("filename length does not match its entries");
		directory = (entries[4] & 0x10) != 0;
		child_cluster = (uint32_t)little(stream + 20, 4);
		child_length = little(stream + 24, 8);
		child_contiguous = (stream[1] & 2) != 0;

		/* Resize may update only these entry-set fields for ordinary files. */
		entries[2] = entries[3] = 0;    /* SetChecksum */
		stream[1] &= (unsigned char)~2; /* NoFatChain */
		memset(stream + 20, 0, 4);      /* FirstCluster */
		bytes = (secondary_count + 1) * 32;
		fputs("entry\t", stdout);
		for (index = 0; index < bytes; ++index)
			printf("%02x", (unsigned int)entries[index]);
		putchar('\n');
		if (directory)
			walk_directory(volume, child_cluster, child_length, child_contiguous, depth + 1);
	}
	puts("end");
}

int main(int argc, char **argv)
{
	struct volume volume = { 0 };
	struct stat status;
	unsigned char boot[512];
	uint64_t sector_size, fat_length, volume_sectors;
	uint32_t root;

	if (argc != 2)
		fail("usage: directory-metadata IMAGE");
	volume.image = fopen(argv[1], "rb");
	if (volume.image == NULL || fstat(fileno(volume.image), &status) != 0 ||
	    !S_ISREG(status.st_mode) || status.st_size < (off_t)sizeof(boot))
		fail("cannot open a regular exFAT image");
	volume.image_size = (uint64_t)status.st_size;
	read_at(&volume, 0, boot, sizeof(boot));
	if (memcmp(boot + 3, "EXFAT   ", 8) != 0 || little(boot + 510, 2) != 0xaa55 || boot[108] < 9 ||
	    boot[108] > 12 || boot[109] > 25 - boot[108] || boot[110] != 1 || (boot[106] & 1) != 0)
		fail("unsupported exFAT boot geometry");
	sector_size = UINT64_C(1) << boot[108];
	volume.cluster_size = UINT32_C(1) << (boot[108] + boot[109]);
	volume.cluster_count = (uint32_t)little(boot + 92, 4);
	volume.directory_clusters_left = volume.cluster_count;
	volume.fat = little(boot + 80, 4) * sector_size;
	fat_length = little(boot + 84, 4) * sector_size;
	volume.heap = little(boot + 88, 4) * sector_size;
	volume_sectors = little(boot + 72, 8);
	if (volume.cluster_count == 0 || volume.cluster_count > UINT32_C(0xfffffff5) ||
	    volume_sectors > volume.image_size / sector_size || volume.fat < 24 * sector_size ||
	    volume.fat + fat_length > volume.heap ||
	    fat_length < ((uint64_t)volume.cluster_count + 2) * 4 ||
	    volume.heap + (uint64_t)volume.cluster_count * volume.cluster_size >
	        volume_sectors * sector_size)
		fail("image geometry cannot contain its directories");
	root = (uint32_t)little(boot + 96, 4);
	walk_directory(&volume, root, 0, 0, 0);
	if (fclose(volume.image) != 0 || fflush(stdout) != 0 || ferror(stdout))
		fail("cannot complete metadata manifest");
	return EXIT_SUCCESS;
}
