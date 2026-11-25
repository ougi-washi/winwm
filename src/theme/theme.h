#pragma once
#include "os_win32/os_win32.h"

////////////////
//~ Theme

HBRUSH w_theme_brush(void);
COLORREF w_theme_background(void);
COLORREF w_theme_foreground(void);
COLORREF w_theme_separator(void);
COLORREF w_theme_accent(void);
COLORREF w_theme_hint(void);
COLORREF w_theme_selection(bool foreground);
void w_theme_colors(uint32_t background, uint32_t foreground, uint32_t accent);
bool w_theme_message(HWND window, UINT message, WPARAM wp, LPARAM lp, LRESULT *result);
