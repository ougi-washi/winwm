#pragma once
#include "model/model.h"

////////////////
//~ Layout

void w_layout_compute_margins(w_desktop_model *m, int output, w_rect usable, w_rect screen, int gap, int outer);
void w_layout_compute(w_desktop_model *m, int output, w_rect usable, w_rect screen, int gap);
int w_layout_neighbor(const w_desktop_model *m, int output, w_rect origin, enum direction dir, bool tiles_only);
int w_layout_adjacent_output(const w_rect screens[output_limit], int output, w_rect origin, enum direction dir);
int w_layout_sibling(const w_desktop_model *m, int slot, enum direction dir);
w_rect w_layout_relocate(w_rect r, w_rect from, w_rect to);
