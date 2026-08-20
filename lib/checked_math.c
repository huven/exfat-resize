/* SPDX-License-Identifier: MIT */

#include "common.h"

#include "checked_math.h"

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
