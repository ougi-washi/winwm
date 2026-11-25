#pragma once
#include "bar/bar.h"

void w_desktop_tray_bind(w_bar_window *bar);
void w_desktop_tray_observe(HWND window, DWORD event);
void w_desktop_tray_scan(bool begin);
void w_desktop_tray_update(void);
bool w_desktop_tray_locate(uint64_t key, RECT *bounds);
void w_desktop_tray_stop(void);
