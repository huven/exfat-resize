/* SPDX-License-Identifier: MIT */

#define _POSIX_C_SOURCE 200809L

#include "cli.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

enum { EXPECTED_CLI_STATUS = 73 };

static volatile sig_atomic_t previous_handler_calls;
static int failure_count;

int exfat_resize_test_posix_entry(int argc, char **argv);

#define CHECK(expression) \
	do { \
		if (!(expression)) { \
			fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expression); \
			++failure_count; \
		} \
	} while (0)

static void previous_handler(int signal_number)
{
	(void)signal_number;
	++previous_handler_calls;
}

int cli_main(int argc, char **argv, const struct cli_cancellation *cancellation)
{
	CHECK(argc == 1);
	CHECK(argv != NULL);
	CHECK(cancellation != NULL);
	CHECK(cancellation != NULL && cancellation->requested != NULL);
	if (cancellation == NULL || cancellation->requested == NULL)
		return EXIT_FAILURE;
	CHECK(cancellation->requested(cancellation->context) == 0);
	CHECK(raise(SIGINT) == 0);
	CHECK(cancellation->requested(cancellation->context) != 0);
	/* A repeated Ctrl-C remains handled while cli_main() performs cleanup. */
	CHECK(raise(SIGINT) == 0);
	CHECK(cancellation->requested(cancellation->context) != 0);
	/* A closed progress pipe cannot terminate the process before cleanup. */
	CHECK(raise(SIGPIPE) == 0);
	return EXPECTED_CLI_STATUS;
}

int cli_report_startup_error(const char *message)
{
	(void)message;
	++failure_count;
	return EXIT_FAILURE;
}

int main(void)
{
	struct sigaction interrupt_action = { 0 };
	struct sigaction pipe_action = { 0 };
	struct sigaction installed_interrupt_action;
	struct sigaction installed_pipe_action;
	struct sigaction original_interrupt_action;
	struct sigaction original_pipe_action;
	char *argv[] = { "exfat-resize", NULL };
	int status;

	interrupt_action.sa_handler = previous_handler;
	(void)sigemptyset(&interrupt_action.sa_mask);
	CHECK(sigaction(SIGINT, &interrupt_action, &original_interrupt_action) == 0);
	pipe_action.sa_handler = previous_handler;
	(void)sigemptyset(&pipe_action.sa_mask);
	CHECK(sigaction(SIGPIPE, &pipe_action, &original_pipe_action) == 0);
	status = exfat_resize_test_posix_entry(1, argv);
	CHECK(status == EXPECTED_CLI_STATUS);
	CHECK(sigaction(SIGINT, NULL, &installed_interrupt_action) == 0);
	CHECK(installed_interrupt_action.sa_handler != previous_handler);
	CHECK(sigaction(SIGPIPE, NULL, &installed_pipe_action) == 0);
	CHECK(installed_pipe_action.sa_handler == SIG_IGN);
	CHECK(previous_handler_calls == 0);
	CHECK(sigaction(SIGPIPE, &original_pipe_action, NULL) == 0);
	CHECK(sigaction(SIGINT, &original_interrupt_action, NULL) == 0);
	if (failure_count != 0)
		return EXIT_FAILURE;
	printf("posix-entry: passed\n");
	return EXIT_SUCCESS;
}
