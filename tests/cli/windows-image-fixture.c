/* SPDX-License-Identifier: MIT */

#include "support/exfat_fixture.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
	struct exfat_fixture fixture;
	unsigned char buffer[64 * 512];
	FILE *image;
	uint64_t sector;
	int status = EXIT_FAILURE;
	if (argc != 2 || exfat_fixture_initialize(&fixture, 20000) != 0)
		return EXIT_FAILURE;
	image = fopen(argv[1], "wb");
	if (image == NULL)
		goto out;
	/* A 12000-sector filesystem inside a 20000-sector backing image. */
	for (sector = 0; sector < fixture.memory.device.sector_count; sector += 64) {
		uint64_t remaining = fixture.memory.device.sector_count - sector;
		uint32_t count = remaining < 64 ? (uint32_t)remaining : 64;
		if (fixture.memory.device.read(&fixture.memory, sector, count, buffer) != 0 ||
		    fwrite(buffer, 512, count, image) != count)
			goto close;
	}
	status = EXIT_SUCCESS;
close:
	if (fclose(image) != 0)
		status = EXIT_FAILURE;
out:
	if (exfat_fixture_destroy(&fixture) != 0)
		status = EXIT_FAILURE;
	return status;
}
