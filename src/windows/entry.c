/* SPDX-License-Identifier: MIT */

#include "cli.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

static volatile LONG interrupt_requested;

static BOOL WINAPI handle_control(DWORD control_type)
{
	switch (control_type) {
	case CTRL_C_EVENT:
	case CTRL_BREAK_EVENT:
		(void)InterlockedExchange(&interrupt_requested, 1);
		return TRUE;
	default:
		return FALSE;
	}
}

static int cancellation_requested(void *context)
{
	(void)context;
	return InterlockedCompareExchange(&interrupt_requested, 0, 0) != 0;
}

static char *utf8_argument(const wchar_t *argument, const char **error)
{
	char *converted;
	int size;

	size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argument, -1, NULL, 0, NULL, NULL);
	if (size == 0) {
		*error = "command line contains invalid Unicode";
		return NULL;
	}
	converted = malloc((size_t)size);
	if (converted == NULL) {
		*error = "cannot allocate memory for the command line";
		return NULL;
	}
	if (WideCharToMultiByte(
	        CP_UTF8, WC_ERR_INVALID_CHARS, argument, -1, converted, size, NULL, NULL) == 0) {
		free(converted);
		*error = "cannot convert the command line to UTF-8";
		return NULL;
	}
	return converted;
}

int wmain(int argc, wchar_t **wide_argv)
{
	struct cli_cancellation cancellation = {
		.context = NULL,
		.requested = cancellation_requested,
	};
	const char *error = NULL;
	char **argv;
	UINT original_output_code_page;
	char startup_error[128];
	DWORD error_number;
	int index;
	int status = EXIT_FAILURE;
	int output_code_page_changed;

	(void)InterlockedExchange(&interrupt_requested, 0);
	if (!SetConsoleCtrlHandler(handle_control, TRUE)) {
		error_number = GetLastError();
		(void)snprintf(startup_error, sizeof(startup_error),
		    "cannot install console control handler: Windows error %lu",
		    (unsigned long)error_number);
		return cli_report_startup_error(startup_error);
	}
	original_output_code_page = GetConsoleOutputCP();
	output_code_page_changed = SetConsoleOutputCP(CP_UTF8);
	if (argc < 0 || (size_t)argc > SIZE_MAX / sizeof(*argv) - 1) {
		status = cli_report_startup_error("command line is too large");
		goto restore_console;
	}
	argv = calloc((size_t)argc + 1, sizeof(*argv));
	if (argv == NULL) {
		status = cli_report_startup_error("cannot allocate memory for the command line");
		goto restore_console;
	}
	for (index = 0; index < argc; ++index) {
		argv[index] = utf8_argument(wide_argv[index], &error);
		if (argv[index] == NULL)
			break;
	}
	if (index == argc)
		status = cli_main(argc, argv, &cancellation);
	else
		status = cli_report_startup_error(error);
	while (index > 0)
		free(argv[--index]);
	free(argv);

restore_console:
	(void)fflush(NULL);
	if (output_code_page_changed)
		(void)SetConsoleOutputCP(original_output_code_page);
	(void)SetConsoleCtrlHandler(handle_control, FALSE);
	return status;
}
