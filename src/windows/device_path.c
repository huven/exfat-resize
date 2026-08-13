/* SPDX-License-Identifier: MIT */

#include "windows/device_path.h"

#include <string.h>

#define VOLUME_GUID_PREFIX_LENGTH ((size_t)11)
#define VOLUME_GUID_LENGTH ((size_t)36)
#define VOLUME_GUID_PATH_LENGTH (VOLUME_GUID_PREFIX_LENGTH + VOLUME_GUID_LENGTH + 1)

static int ascii_letter(char character)
{
	return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z');
}

static int ascii_hex_digit(char character)
{
	return (character >= '0' && character <= '9') || (character >= 'A' && character <= 'F') ||
	    (character >= 'a' && character <= 'f');
}

static int ascii_equal(char left, char right)
{
	if (left >= 'a' && left <= 'z')
		left -= 'a' - 'A';
	if (right >= 'a' && right <= 'z')
		right -= 'a' - 'A';
	return left == right;
}

static int path_separator(char character)
{
	return character == '\\' || character == '/';
}

static int starts_with(const char *path, const char *prefix)
{
	while (*prefix != '\0') {
		if (!ascii_equal(*path++, *prefix++))
			return 0;
	}
	return 1;
}

static int drive_designator(const char *path, size_t length)
{
	return length == 2 && ascii_letter(path[0]) && path[1] == ':';
}

static int device_namespace(const char *path, size_t length)
{
	return length >= 4 && path_separator(path[0]) && path_separator(path[1]) &&
	    (path[2] == '.' || path[2] == '?') && path_separator(path[3]);
}

static int volume_guid(const char *path, size_t length)
{
	static const size_t hyphens[] = { 8, 13, 18, 23 };
	const char *guid;
	size_t index, hyphen_index = 0;

	if ((length != VOLUME_GUID_PATH_LENGTH && length != VOLUME_GUID_PATH_LENGTH + 1) ||
	    path[2] != '?' || !starts_with(path + 4, "Volume{"))
		return 0;
	guid = path + VOLUME_GUID_PREFIX_LENGTH;
	for (index = 0; index < VOLUME_GUID_LENGTH; ++index) {
		if (hyphen_index < sizeof(hyphens) / sizeof(hyphens[0]) && index == hyphens[hyphen_index]) {
			if (guid[index] != '-')
				return 0;
			++hyphen_index;
		} else if (!ascii_hex_digit(guid[index])) {
			return 0;
		}
	}
	if (guid[VOLUME_GUID_LENGTH] != '}')
		return 0;
	return length == VOLUME_GUID_PATH_LENGTH || path_separator(path[length - 1]);
}

enum windows_device_path_type windows_classify_device_path(const char *path)
{
	size_t length = strlen(path);

	if (drive_designator(path, length))
		return WINDOWS_DEVICE_PATH_VOLUME;
	if (!device_namespace(path, length))
		return WINDOWS_DEVICE_PATH_IMAGE;
	if (path[2] == '.' && length == 6 && drive_designator(path + 4, 2))
		return WINDOWS_DEVICE_PATH_VOLUME;
	if (volume_guid(path, length))
		return WINDOWS_DEVICE_PATH_VOLUME;

	/* Permit long absolute file paths, but no other device namespaces. */
	if (path[2] == '?' && length >= 7 && ascii_letter(path[4]) && path[5] == ':' &&
	    path_separator(path[6]))
		return WINDOWS_DEVICE_PATH_IMAGE;
	if (path[2] == '?' && length >= 8 &&
	    (starts_with(path + 4, "UNC\\") || starts_with(path + 4, "UNC/")))
		return WINDOWS_DEVICE_PATH_IMAGE;
	return WINDOWS_DEVICE_PATH_UNSUPPORTED;
}

int windows_normalize_volume_path(const char *path, char *normalized, size_t normalized_size)
{
	char drive_letter;
	size_t length = strlen(path);

	if (windows_classify_device_path(path) != WINDOWS_DEVICE_PATH_VOLUME)
		return -1;
	if (drive_designator(path, length))
		drive_letter = path[0];
	else if (path[2] == '.')
		drive_letter = path[4];
	else {
		if (normalized_size < VOLUME_GUID_PATH_LENGTH + 1)
			return -1;
		(void)memcpy(normalized, "\\\\?\\Volume{", VOLUME_GUID_PREFIX_LENGTH);
		(void)memcpy(normalized + VOLUME_GUID_PREFIX_LENGTH, path + VOLUME_GUID_PREFIX_LENGTH,
		    VOLUME_GUID_LENGTH + 1);
		normalized[VOLUME_GUID_PATH_LENGTH] = '\0';
		return 0;
	}

	if (normalized_size < 7)
		return -1;
	normalized[0] = '\\';
	normalized[1] = '\\';
	normalized[2] = '.';
	normalized[3] = '\\';
	normalized[4] = drive_letter;
	normalized[5] = ':';
	normalized[6] = '\0';
	return 0;
}
