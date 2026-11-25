#pragma once
#include "theme/surface.h"

bool w_theme_message(HWND window, UINT message, WPARAM wp, LPARAM lp, LRESULT *result);
// Paint a button face and return the foreground for opaque text or glyphs.
COLORREF w_theme_button_face(const DRAWITEMSTRUCT *item, bool selected, bool flat);
