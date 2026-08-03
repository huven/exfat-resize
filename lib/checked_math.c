/* SPDX-License-Identifier: MIT */

#include "common.h"

#include "checked_math.h"

enum exfat_resize_error exfat_resize_checked_add_u64(
    uint64_t left, uint64_t right, uint64_t *result)
{
	if (result == NULL)
		return EXFAT_RESIZE_INVALID_ARGUMENT;
	if (right > UINT64_MAX - left)
		return EXFAT_RESIZE_ARITHMETIC_OVERFLOW;
	*result = left + right;
	return EXFAT_RESIZE_SUCCESS;
}

enum exfat_resize_error exfat_resize_checked_multiply_u64(
    uint64_t left, uint64_t right, uint64_t *result)
{
	if (result == NULL)
		return EXFAT_RESIZE_INVALID_ARGUMENT;
	if (left != 0 && right > UINT64_MAX / left)
		return EXFAT_RESIZE_ARITHMETIC_OVERFLOW;
	*result = left * right;
	return EXFAT_RESIZE_SUCCESS;
}

enum exfat_resize_error exfat_resize_checked_ceil_divide_u64(
    uint64_t dividend, uint64_t divisor, uint64_t *result)
{
	if (result == NULL || divisor == 0)
		return EXFAT_RESIZE_INVALID_ARGUMENT;
	*result = dividend / divisor;
	if (dividend % divisor != 0)
		++*result;
	return EXFAT_RESIZE_SUCCESS;
}
