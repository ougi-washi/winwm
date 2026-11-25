#pragma once
#include "os_win32/os_win32.h"

// Keys are local to the window. Bounds are client coordinates and include the
// entire highlight. A negative key releases the current pointer target.
void w_hover_target(HWND host, INT_PTR key, const RECT *bounds);
// A native edit owns its client pixels, caret and scrollbars. Animate only its frame.
enum { w_hover_frame_changed=WM_APP+0x43d };
void w_hover_frame_target(HWND field,bool hot);
int w_hover_amount(HWND host, INT_PTR key);
void w_hover_clear(HWND host);
void w_hover_clear_children(HWND parent);
// Rebind active fades after scrolling or reflowing a host. Return false for a
// target that disappeared. Repeating the same layout leaves the paint region clean.
void w_hover_relayout(HWND host,bool (*locate)(HWND,INT_PTR,RECT *,void *),void *data);
void w_hover_preferences_changed(void);
// Rows and adjacent tab/header cells share a continuous surface. The returned
// ink matches the fill, including the system's high-contrast selection pair.
COLORREF w_hover_row(HDC dc,const RECT *bounds,bool selected,int amount,bool pressed);

// Paint a translucent face and its shadow within the original hit rectangle.
// The caller draws the resting background first and the opaque content last.
void w_hover_plate(HDC dc, const RECT *bounds, UINT dpi, COLORREF base, COLORREF target, int amount, bool pressed);
