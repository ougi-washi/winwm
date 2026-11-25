#pragma once
#include "bar/bar.h"

////////////////
//~ Menu

bool w_bar_menu_open(w_bar_window *bar, int hit);
bool w_bar_menu_at(const w_bar_window *bar, const struct w_bar_hit *hit);
void w_bar_menu_reanchor(w_bar_window *bar);
HWND w_bar_remember_foreground(void);
HWND w_bar_input_target(void);
void w_bar_layout_label(HKL layout, wchar_t *out, size_t cap);
void w_bar_cycle_layout(int steps);
void w_bar_menu_audio_changed(w_bar_window *bar);
