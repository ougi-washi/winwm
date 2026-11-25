#pragma once
#include "os_win32/os_win32.h"

// Keep the native button caption as its accessible name and tooltip.
bool w_icon_button(HWND button, const wchar_t *glyph, const wchar_t *label);
bool w_labeled_button(HWND button, const wchar_t *glyph, const wchar_t *label);
int w_labeled_button_width(HFONT font,const wchar_t *label,UINT dpi);
bool w_icon_button_compact(HWND button);
bool w_icon_button_draw(const DRAWITEMSTRUCT *item);
