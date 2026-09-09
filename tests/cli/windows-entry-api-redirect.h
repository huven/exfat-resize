/* SPDX-License-Identifier: MIT */

#ifndef EXFAT_RESIZE_TEST_WINDOWS_ENTRY_API_REDIRECT_H
#define EXFAT_RESIZE_TEST_WINDOWS_ENTRY_API_REDIRECT_H

#include <windows.h>

BOOL WINAPI exfat_resize_test_set_console_ctrl_handler(PHANDLER_ROUTINE handler, BOOL add);

#define SetConsoleCtrlHandler exfat_resize_test_set_console_ctrl_handler

#endif
