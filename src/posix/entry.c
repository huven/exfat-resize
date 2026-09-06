/* SPDX-License-Identifier: MIT */

#define _POSIX_C_SOURCE 200809L

#include "cli.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

static volatile sig_atomic_t interrupt_requested;

static void handle_interrupt(int signal_number)
{
	(void)signal_number;
	interrupt_requested = 1;
}

static int cancellation_requested(void *context)
{
	(void)context;
	return interrupt_requested != 0;
}

int main(int argc, char **argv)
{
	struct cli_cancellation cancellation = {
		.context = NULL,
		.requested = cancellation_requested,
	};
	struct sigaction action = { 0 };
	struct sigaction previous_action;
	char error[256];
	int status;

	interrupt_requested = 0;
	action.sa_handler = handle_interrupt;
	action.sa_flags = SA_RESTART;
	(void)sigemptyset(&action.sa_mask);
	if (sigaction(SIGINT, &action, &previous_action) != 0) {
		(void)snprintf(error, sizeof(error), "cannot install Ctrl-C handler: %s", strerror(errno));
		return cli_report_startup_error(error);
	}
	status = cli_main(argc, argv, &cancellation);
	(void)sigaction(SIGINT, &previous_action, NULL);
	return status;
}
