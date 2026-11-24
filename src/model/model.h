#pragma once
#include "base/base.h"
enum split_axis { split_x, split_y };
enum layout_kind { layout_tree, layout_alternate, layout_master, layout_tabs, layout_stack };
enum direction { dir_left, dir_down, dir_up, dir_right };

////////////////
//~ Model

typedef struct w_window_id {
    int slot;
    uint32_t generation;
} w_window_id;

typedef struct w_model_window {
    uint32_t generation;
    bool live, floating, fullscreen, minimized, withdrawn, scratch, scratch_visible;
    int output, workspace, leaf, stack_weight, next_row;
    uint64_t focus_order;
    w_rect floating_bounds, desired;
} w_model_window;

typedef struct w_tile_node {
    int parent, first, second, slot, ratio;
    enum split_axis axis;
    bool live, manual;
    int span[2], cut, available, divider;
    uint64_t generation;
    w_rect bounds;
} w_tile_node;

typedef struct w_workspace {
    int root, master_slot, master_ratio, master_gap;
    w_rect master_area;
    enum split_axis next_axis;
    enum layout_kind layout;
} w_workspace;

typedef struct w_desktop_model {
    w_model_window windows[window_limit];
    w_tile_node nodes[window_limit * 2 + 1];
    w_workspace spaces[output_limit][workspace_limit];
    int selected[output_limit], previous[output_limit], output, focus;
    uint64_t serial;
} w_desktop_model;

////////////////
//~ Model Interface

void w_model_init(w_desktop_model *m);
w_window_id w_model_add(w_desktop_model *m, int output, int ws, w_rect original);
bool w_model_valid(const w_desktop_model *m, w_window_id id);
void w_model_remove(w_desktop_model *m, w_window_id id);
void w_model_move_beside(w_desktop_model *m, int slot, int output, int ws, int anchor, enum split_axis axis,
                       bool before);
void w_model_move(w_desktop_model *m, int slot, int output, int ws);
void w_model_focus(w_desktop_model *m, int slot);
int w_model_recent(const w_desktop_model *m, int output, int ws);
bool w_model_visible(const w_desktop_model *m, int slot);
void w_model_workspace(w_desktop_model *m, int ws);
void w_model_resize(w_desktop_model *m, int delta);
void w_model_resize_pixels(w_desktop_model *m, enum split_axis axis, int delta);
void w_model_swap(w_desktop_model *m, int a, int b);
