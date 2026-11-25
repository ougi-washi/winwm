#pragma once
#include "win_platform/win_platform.h"
#include <commctrl.h>

bool w_percent_slider_init(HWND slider);
LRESULT w_percent_slider_draw(HWND slider, NMCUSTOMDRAW *draw, COLORREF paper, COLORREF track,
                              COLORREF accent, COLORREF thumb);
