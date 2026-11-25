#pragma once
#include "win_platform/win_platform.h"

void w_display_create(HWND window, HWND list, HWND status, HWND apply);
void w_display_start(void);
int w_display_pending(HANDLE handles[2]);
void w_display_finish(HANDLE completed);
void w_display_layout(int pad, int width, int footer, int side, int gap,bool update_font);
bool w_display_slider(HWND control);
void w_display_preview(void);
bool w_display_key(HWND focus,WPARAM key);
bool w_display_message(UINT message, WPARAM wp, LPARAM lp);
