#pragma once
#include "os_win32/os_win32.h"

////////////////
//~ Shell Host

#define WINWM_RECOVERY_CHECK L"/validate:2"
enum { recovery_session_closing = WM_APP + 1, recovery_take_desktop };
bool w_recovery_validate(void);
HANDLE w_recovery_attach(bool preview, HWND *window);
DWORD w_recovery_take_desktop(HANDLE host, HWND window);
