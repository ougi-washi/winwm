#pragma once
#include "win_platform/win_platform.h"

void w_bluetooth_open(HWND window, HWND list, HWND status, HWND action);
void w_bluetooth_poll(bool device_change);
HANDLE w_bluetooth_pending(void);
bool w_bluetooth_finish(void);
void w_bluetooth_selected(void);
void w_bluetooth_apply(void);
void w_bluetooth_close(void);
