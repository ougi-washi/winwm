#pragma once
#include "bar/bar.h"

////////////////
//~ Menu

bool w_bar_menu_open(w_bar_window *bar, int hit);
HWND w_bar_input_target(void);
void w_bar_cycle_layout(int steps);
void w_bar_menu_audio_changed(w_bar_window *bar);
