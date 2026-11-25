#pragma once
#include "os_win32/os_win32.h"

// Standard Win32 response IDs and MB_OK, MB_OKCANCEL or MB_YESNO semantics.
int w_dialog(HWND owner, const wchar_t *message, const wchar_t *caption, UINT flags);
void w_theme_load_profile(void);
