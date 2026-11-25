#pragma once
#include "os_win32/os_win32.h"

////////////////
//~ Shell Host

enum { recovery_session_closing = WM_APP + 1, recovery_take_desktop };
int w_recovery_run(HINSTANCE instance, const wchar_t *operation);
HANDLE w_recovery_attach(bool preview, HWND *window);
DWORD w_recovery_take_desktop(HANDLE host, HWND window);
