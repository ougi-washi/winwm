#pragma once
#include "os_win32/os_win32.h"

HWND w_calendar_create(HWND parent);
enum { w_calendar_names_changed=WM_APP+0x392 };
int w_calendar_width(UINT dpi);
int w_calendar_height(UINT dpi);
void w_calendar_place(int x,int y,int width,int height);
void w_calendar_today(const SYSTEMTIME *now,bool select);
void w_calendar_focus(void);
bool w_calendar_dropdown(void);
bool w_calendar_finish_edit(void);
bool w_calendar_enter(HWND control);
