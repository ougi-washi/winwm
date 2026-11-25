#pragma once
#include "bar/bar.h"

void w_bar_tray_layout(w_bar_window *bar, RECT client, bool vertical, int margin);
void w_bar_tray_hover(w_bar_window *bar, int hit);
bool w_bar_tray_action(w_bar_window *bar, int hit, UINT event);
bool w_bar_tray_scroll(w_bar_window *bar, int hit, int steps);
void w_bar_tray_reset(w_bar_window *bar);
