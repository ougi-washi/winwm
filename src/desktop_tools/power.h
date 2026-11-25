#pragma once
#include "win_platform/win_platform.h"

void w_power_create(HWND panel, HWND list, HWND status, HWND apply);
void w_power_start(void);
void w_power_layout(int pad, int width, int footer, int side, int gap);
HANDLE w_power_pending(void);
void w_power_finish(void);
void w_power_selected(void);
void w_power_save(void);
bool w_power_enter(HWND focus);
bool w_power_message(UINT msg, WPARAM wp, LPARAM lp);
