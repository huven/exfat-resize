/* SPDX-License-Identifier: MIT */

#include "cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

enum { EXPECTED_CLI_STATUS = 73 };

static PHANDLER_ROUTINE installed_handler;
static DWORD requested_event;
static int install_calls;
static int removal_calls;
static int cli_calls;
static int cli_returned;
static int startup_error_calls;
static int failure_count;

int exfat_resize_test_windows_entry(int argc, wchar_t **argv);

#define CHECK(expression) \
	do { \
		if (!(expression)) { \
			fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expression); \
			++failure_count; \
		} \
	} while (0)

BOOL WINAPI exfat_resize_test_set_console_ctrl_handler(PHANDLER_ROUTINE handler, BOOL add)
{
	if (add) {
		CHECK(handler != NULL);
		CHECK(installed_handler == NULL);
		installed_handler = handler;
		cli_returned = 0;
		++install_calls;
	} else {
		CHECK(handler == installed_handler);
		CHECK(cli_returned != 0);
		installed_handler = NULL;
		++removal_calls;
	}
	return TRUE;
}

int cli_main(int argc, char **argv, const struct cli_cancellation *cancellation)
{
	++cli_calls;
	CHECK(argc == 1);
	CHECK(argv != NULL);
	CHECK(argv != NULL && strcmp(argv[0], "exfat-resize") == 0);
	CHECK(cancellation != NULL);
	CHECK(cancellation != NULL && cancellation->requested != NULL);
	CHECK(installed_handler != NULL);
	if (cancellation == NULL || cancellation->requested == NULL || installed_handler == NULL)
		return EXIT_FAILURE;
	CHECK(cancellation->requested(cancellation->context) == 0);
	CHECK(installed_handler(CTRL_CLOSE_EVENT) == FALSE);
	CHECK(cancellation->requested(cancellation->context) == 0);
	CHECK(installed_handler(requested_event) == TRUE);
	CHECK(cancellation->requested(cancellation->context) != 0);
	/* Repeated requests remain handled while cli_main() performs cleanup. */
	CHECK(installed_handler(requested_event) == TRUE);
	CHECK(cancellation->requested(cancellation->context) != 0);
	cli_returned = 1;
	return EXPECTED_CLI_STATUS;
}

int cli_report_startup_error(const char *message)
{
	(void)message;
	++startup_error_calls;
	return EXIT_FAILURE;
}

static void test_control_event(DWORD control_event)
{
	wchar_t program[] = L"exfat-resize";
	wchar_t *argv[] = { program, NULL };
	int previous_install_calls = install_calls;
	int previous_removal_calls = removal_calls;
	int previous_cli_calls = cli_calls;
	int status;

	requested_event = control_event;
	status = exfat_resize_test_windows_entry(1, argv);
	CHECK(status == EXPECTED_CLI_STATUS);
	CHECK(install_calls == previous_install_calls + 1);
	CHECK(removal_calls == previous_removal_calls + 1);
	CHECK(cli_calls == previous_cli_calls + 1);
	CHECK(installed_handler == NULL);
}

int main(void)
{
	test_control_event(CTRL_C_EVENT);
	test_control_event(CTRL_BREAK_EVENT);
	CHECK(startup_error_calls == 0);
	if (failure_count != 0)
		return EXIT_FAILURE;
	printf("windows-entry: passed\n");
	return EXIT_SUCCESS;
}
