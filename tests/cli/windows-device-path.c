/* SPDX-License-Identifier: MIT */

#include "windows/device_path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct path_case {
	const char *path;
	enum windows_device_path_type type;
	const char *normalized;
};

static int check_case(const struct path_case *test)
{
	char normalized[64];
	enum windows_device_path_type type = windows_classify_device_path(test->path);

	if (type != test->type) {
		fprintf(stderr, "wrong classification for %s: got %d, expected %d\n", test->path, (int)type,
		    (int)test->type);
		return -1;
	}
	if (test->normalized == NULL)
		return 0;
	if (windows_normalize_volume_path(test->path, normalized, sizeof(normalized)) != 0 ||
	    strcmp(normalized, test->normalized) != 0) {
		fprintf(stderr, "wrong normalized volume path for %s\n", test->path);
		return -1;
	}
	return 0;
}

int main(void)
{
	static const struct path_case cases[] = {
		{ "E:", WINDOWS_DEVICE_PATH_VOLUME, "\\\\.\\E:" },
		{ "e:", WINDOWS_DEVICE_PATH_VOLUME, "\\\\.\\e:" },
		{ "\\\\.\\E:", WINDOWS_DEVICE_PATH_VOLUME, "\\\\.\\E:" },
		{ "//./e:", WINDOWS_DEVICE_PATH_VOLUME, "\\\\.\\e:" },
		{ "\\\\?\\Volume{01234567-89ab-cdef-0123-456789abcdef}", WINDOWS_DEVICE_PATH_VOLUME,
		    "\\\\?\\Volume{01234567-89ab-cdef-0123-456789abcdef}" },
		{ "\\\\?\\Volume{01234567-89AB-CDEF-0123-456789ABCDEF}\\", WINDOWS_DEVICE_PATH_VOLUME,
		    "\\\\?\\Volume{01234567-89AB-CDEF-0123-456789ABCDEF}" },
		{ "image.exfat", WINDOWS_DEVICE_PATH_IMAGE, NULL },
		{ "E:\\images\\image.exfat", WINDOWS_DEVICE_PATH_IMAGE, NULL },
		{ "\\\\server\\share\\image.exfat", WINDOWS_DEVICE_PATH_IMAGE, NULL },
		{ "\\\\?\\C:\\long\\image.exfat", WINDOWS_DEVICE_PATH_IMAGE, NULL },
		{ "\\\\?\\UNC\\server\\share\\image.exfat", WINDOWS_DEVICE_PATH_IMAGE, NULL },
		{ "\\\\.\\PhysicalDrive0", WINDOWS_DEVICE_PATH_UNSUPPORTED, NULL },
		{ "//./PhysicalDrive0", WINDOWS_DEVICE_PATH_UNSUPPORTED, NULL },
		{ "\\\\?\\GLOBALROOT\\Device\\Harddisk0", WINDOWS_DEVICE_PATH_UNSUPPORTED, NULL },
		{ "\\\\?\\Volume{01234567-89ab-cdef-0123-456789abcdef}\\image.exfat",
		    WINDOWS_DEVICE_PATH_UNSUPPORTED, NULL },
		{ "\\\\?\\Volume{not-a-guid}\\", WINDOWS_DEVICE_PATH_UNSUPPORTED, NULL },
		{ "\\\\.\\E:\\", WINDOWS_DEVICE_PATH_UNSUPPORTED, NULL },
	};
	char normalized[6];
	size_t index;

	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
		if (check_case(&cases[index]) != 0)
			return EXIT_FAILURE;
	}
	if (windows_normalize_volume_path("image.exfat", normalized, sizeof(normalized)) == 0 ||
	    windows_normalize_volume_path("E:", normalized, sizeof(normalized)) == 0) {
		fprintf(stderr, "invalid volume normalization succeeded\n");
		return EXIT_FAILURE;
	}
	printf("windows-device-path: passed\n");
	return EXIT_SUCCESS;
}
