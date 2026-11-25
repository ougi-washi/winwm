#pragma once
#include "win_platform/win_platform.h"

void w_display_create(HWND window, HWND list, HWND status, HWND apply);
bool w_display_stretches(HWND control);
void w_display_preview(void);
bool w_display_message(UINT message, WPARAM wp, LPARAM lp);
