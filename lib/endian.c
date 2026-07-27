/* SPDX-License-Identifier: MIT */

#include "endian.h"

#include <stddef.h>
#include <stdint.h>

static enum exfat_resize_error check_range(
    const void *buffer, size_t buffer_size, size_t offset, size_t width)
{
	if (buffer == NULL)
		return EXFAT_RESIZE_INVALID_ARGUMENT;
	if (offset > buffer_size || buffer_size - offset < width)
		return EXFAT_RESIZE_OUT_OF_BOUNDS;
	return EXFAT_RESIZE_SUCCESS;
}

enum exfat_resize_error exfat_resize_load_le16(
    const unsigned char *buffer, size_t buffer_size, size_t offset, uint16_t *value)
{
	enum exfat_resize_error error = check_range(buffer, buffer_size, offset, 2);
	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	if (value == NULL)
		return EXFAT_RESIZE_INVALID_ARGUMENT;
	*value = (uint16_t)buffer[offset] | (uint16_t)((uint16_t)buffer[offset + 1] << 8);
	return EXFAT_RESIZE_SUCCESS;
}

enum exfat_resize_error exfat_resize_load_le32(
    const unsigned char *buffer, size_t buffer_size, size_t offset, uint32_t *value)
{
	enum exfat_resize_error error = check_range(buffer, buffer_size, offset, 4);
	uint32_t result = 0;
	size_t index;

	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	if (value == NULL)
		return EXFAT_RESIZE_INVALID_ARGUMENT;
	for (index = 0; index < 4; ++index)
		result |= (uint32_t)buffer[offset + index] << (index * 8);
	*value = result;
	return EXFAT_RESIZE_SUCCESS;
}

enum exfat_resize_error exfat_resize_load_le64(
    const unsigned char *buffer, size_t buffer_size, size_t offset, uint64_t *value)
{
	enum exfat_resize_error error = check_range(buffer, buffer_size, offset, 8);
	uint64_t result = 0;
	size_t index;

	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	if (value == NULL)
		return EXFAT_RESIZE_INVALID_ARGUMENT;
	for (index = 0; index < 8; ++index)
		result |= (uint64_t)buffer[offset + index] << (index * 8);
	*value = result;
	return EXFAT_RESIZE_SUCCESS;
}

enum exfat_resize_error exfat_resize_store_le16(
    unsigned char *buffer, size_t buffer_size, size_t offset, uint16_t value)
{
	enum exfat_resize_error error = check_range(buffer, buffer_size, offset, 2);
	size_t index;

	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	for (index = 0; index < 2; ++index)
		buffer[offset + index] = (unsigned char)((value >> (index * 8)) & 0xffU);
	return EXFAT_RESIZE_SUCCESS;
}

enum exfat_resize_error exfat_resize_store_le32(
    unsigned char *buffer, size_t buffer_size, size_t offset, uint32_t value)
{
	enum exfat_resize_error error = check_range(buffer, buffer_size, offset, 4);
	size_t index;

	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	for (index = 0; index < 4; ++index)
		buffer[offset + index] = (unsigned char)((value >> (index * 8)) & 0xffU);
	return EXFAT_RESIZE_SUCCESS;
}

enum exfat_resize_error exfat_resize_store_le64(
    unsigned char *buffer, size_t buffer_size, size_t offset, uint64_t value)
{
	enum exfat_resize_error error = check_range(buffer, buffer_size, offset, 8);
	size_t index;

	if (error != EXFAT_RESIZE_SUCCESS)
		return error;
	for (index = 0; index < 8; ++index)
		buffer[offset + index] = (unsigned char)((value >> (index * 8)) & UINT64_C(0xff));
	return EXFAT_RESIZE_SUCCESS;
}
