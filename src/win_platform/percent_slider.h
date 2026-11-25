#pragma once
#include "win_platform/win_platform.h"
#include <commctrl.h>

typedef enum { slider_live, slider_preview } w_slider_mode;
bool w_percent_slider_init(HWND slider, w_slider_mode mode);
// Reconcile a provider reading and return the percentage currently shown to the user.
int w_percent_slider_sync(HWND slider, int value);
typedef struct { int amount; bool shadow,contrast; } w_slider_feedback;
LRESULT w_percent_slider_draw(HWND slider, NMCUSTOMDRAW *draw, COLORREF paper, COLORREF track,
                              COLORREF accent, COLORREF thumb, w_slider_feedback feedback);
