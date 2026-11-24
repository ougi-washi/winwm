#pragma once
#include "os_win32/os_win32.h"

////////////////
//~ Theme

HBRUSH w_theme_brush(void);
void w_theme_rich_edit(HWND edit);
void w_theme_colors(uint32_t background, uint32_t foreground, uint32_t accent);
bool w_theme_message(HWND window, UINT message, WPARAM wp, LPARAM lp, LRESULT *result);
