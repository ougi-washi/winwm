#pragma once
#include "win_platform/percent_slider.h"

// Enlist WinWM's native list, dropdown, header or trackbar in the shared hover renderer.
void w_native_hover_control(HWND control);
// Supply a parent-relative rectangle for a composite search field, or NULL
// for an EDIT created with WS_BORDER. Classic EDIT consumes that style bit.
void w_native_edit_frame(HWND edit,const RECT *parent_frame);
// Single-line EDIT with a full-size hit target and a vertically centered native caret.
HWND w_native_field(HWND parent,int id,DWORD style);
HFONT w_native_requested_font(HWND control);
bool w_native_draw_combo(const DRAWITEMSTRUCT *item);
w_slider_feedback w_native_slider_feedback(HWND slider);
