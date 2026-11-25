#pragma once
#include "os_win32/os_win32.h"

typedef struct {
    UINT id; // Zero denotes a separator.
    const wchar_t *label, *glyph;
    bool checked;
} w_theme_menu_item;

UINT w_theme_track_menu(HWND owner, POINT at, UINT flags, TPMPARAMS *anchor,
                       const w_theme_menu_item *items, size_t count);
