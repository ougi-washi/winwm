#pragma once
#include "bar/bar.h"

typedef struct {
    const char *module;
    const wchar_t *heading, *click, *right, *wheel;
} w_bar_description;
const w_bar_description *w_bar_describe(const char *module);

void w_bar_tip_track(w_bar_window *bar, int index);
void w_bar_tip_update(w_bar_window *bar);
void w_bar_tip_move(w_bar_window *bar);
void w_bar_tip_timer(w_bar_window *bar, UINT_PTR timer);
void w_bar_tip_hide(w_bar_window *bar);
