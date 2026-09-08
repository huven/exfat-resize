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
	struct sigaction interrupt_action = { 0 };
	struct sigaction pipe_action = { 0 };
	char error[256];

	interrupt_requested = 0;
	interrupt_action.sa_handler = handle_interrupt;
	interrupt_action.sa_flags = SA_RESTART;
	(void)sigemptyset(&interrupt_action.sa_mask);
	if (sigaction(SIGINT, &interrupt_action, NULL) != 0) {
		(void)snprintf(error, sizeof(error), "cannot install Ctrl-C handler: %s", strerror(errno));
		return cli_report_startup_error(error);
	}
	/* Retain both dispositions through process teardown and its stream flushes. */
	pipe_action.sa_handler = SIG_IGN;
	(void)sigemptyset(&pipe_action.sa_mask);
	if (sigaction(SIGPIPE, &pipe_action, NULL) != 0) {
		(void)snprintf(error, sizeof(error), "cannot ignore SIGPIPE: %s", strerror(errno));
		return cli_report_startup_error(error);
	}
	return cli_main(argc, argv, &cancellation);
}
