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
int w_layout_output_for_bounds(const w_rect screens[output_limit], int current, w_rect bounds);

typedef struct w_split_grab {
    enum { grab_none, grab_node, grab_column, grab_row } kind;
    int node, cut, available, weight;
    uint64_t generation;
    w_window_id first, second;
} w_split_grab;

bool w_layout_grab_splits(const w_desktop_model *m, int output, int slot, int x, int y,
                          int tolerance, bool inside, w_split_grab grips[2]);
bool w_layout_drag_split(w_desktop_model *m, int output, int ws, const w_split_grab *grip, int delta);
int w_layout_drop_edge(w_rect tile, int x, int y, w_rect *indicator);
w_rect w_layout_drag_position(w_rect initial, w_rect actual, int offset_x, int offset_y, int x, int y);
w_rect w_layout_drag_corner(w_rect initial, int dx, int dy, bool left, bool top,
                            int min_w, int min_h, int max_w, int max_h);
