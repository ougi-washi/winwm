#pragma once
#include "os_win32/os_win32.h"

////////////////
//~ Desktop Tools

int w_tools_run(const wchar_t *mode);
bool w_tools_set_popup(HWND owner, RECT button, const wchar_t *edge, HANDLE cancel);
