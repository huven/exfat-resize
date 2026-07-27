/* SPDX-License-Identifier: MIT */

#include <exfat_resize.h>

int main(void)
{
	enum exfat_resize_stage stage = EXFAT_RESIZE_STAGE_COMPLETED;
	enum exfat_resize_error error;

	error = exfat_resize(NULL, 1, NULL, &stage);
	return error == EXFAT_RESIZE_INVALID_DEVICE && stage == EXFAT_RESIZE_STAGE_PREFLIGHT ? 0 : 1;
}
