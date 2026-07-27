/* SPDX-License-Identifier: MIT */

#include <exfat_resize.h>

int main()
{
	exfat_resize_stage stage = EXFAT_RESIZE_STAGE_COMPLETED;
	exfat_resize_error error;

	error = exfat_resize(nullptr, 1, nullptr, &stage);
	return error == EXFAT_RESIZE_INVALID_DEVICE && stage == EXFAT_RESIZE_STAGE_PREFLIGHT ? 0 : 1;
}
