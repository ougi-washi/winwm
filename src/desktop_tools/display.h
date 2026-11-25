#pragma once
#include "win_platform/win_platform.h"

void w_display_create(HWND window, HWND list, HWND status, HWND apply);
void w_display_start(void);
int w_display_pending(HANDLE handles[2]);
void w_display_finish(HANDLE completed);
bool w_display_stretches(HWND control);
void w_display_preview(void);
bool w_display_message(UINT message, WPARAM wp, LPARAM lp);
