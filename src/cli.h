/* SPDX-License-Identifier: MIT */

#ifndef EXFAT_RESIZE_CLI_H
#define EXFAT_RESIZE_CLI_H

#define CLI_CANCELLED_EXIT_STATUS 130

struct cli_cancellation {
	void *context;
	int (*requested)(void *context);
};

int cli_main(int argc, char **argv, const struct cli_cancellation *cancellation);
int cli_report_startup_error(const char *message);

#endif
