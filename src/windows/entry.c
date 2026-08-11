/* SPDX-License-Identifier: MIT */

#include "cli.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

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
	const char *error = NULL;
	char **argv;
	UINT original_output_code_page;
	int index;
	int status = EXIT_FAILURE;
	int output_code_page_changed;

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
		status = cli_main(argc, argv);
	else
		status = cli_report_startup_error(error);
	while (index > 0)
		free(argv[--index]);
	free(argv);

restore_console:
	(void)fflush(NULL);
	if (output_code_page_changed)
		(void)SetConsoleOutputCP(original_output_code_page);
	return status;
}
