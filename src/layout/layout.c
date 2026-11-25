#include "layout/layout.h"
#include <limits.h>

////////////////
//~ Layout

static bool w_tiled(const w_desktop_model *m, int slot) {
    return w_model_visible(m, slot) && !m->windows[slot].floating && !m->windows[slot].fullscreen;
}

static int w_measure(w_desktop_model *m, int node, int *counts, int *slots, int *length) {
    if (!node)
        return 0;
    w_tile_node *n = &m->nodes[node];
    n->available = 0;
    if (n->slot >= 0) {
        slots[(*length)++] = n->slot;
        return counts[node] = n->span[0] = n->span[1] = w_tiled(m, n->slot);
    }
    int left = w_measure(m, n->first, counts, slots, length);
    int right = w_measure(m, n->second, counts, slots, length);
    for (int axis = 0; axis < 2; ++axis) {
        int sum = m->nodes[n->first].span[axis] + m->nodes[n->second].span[axis];
        n->span[axis] = left && right && axis != (int)n->axis ? 1 : sum;
    }
    return counts[node] = left + right;
}

static void w_place(w_desktop_model *m, int node, w_rect r, int gap, const int *counts) {
    if (!node || !counts[node])
        return;
    w_tile_node *n = &m->nodes[node];
    if (n->slot >= 0) {
        m->windows[n->slot].desired = r;
        return;
    }
    if (!counts[n->first]) {
        w_place(m, n->second, r, gap, counts);
        return;
    }
    if (!counts[n->second]) {
        w_place(m, n->first, r, gap, counts);
        return;
    }
    w_rect a = r, b = r;
    int size = n->axis == split_x ? r.w : r.h;
    int padding = w_clamp_int(gap, 0, size > 2 ? size - 2 : 0);
    int space = size - padding;
    int first = m->nodes[n->first].span[n->axis], second = m->nodes[n->second].span[n->axis];
    int pixels = size - (first + second - 1) * padding;
    int cut = n->manual    ? (int)((int64_t)space * n->ratio / 1000)
              : pixels > 0 ? (int)((int64_t)pixels * first / (first + second)) + (first - 1) * padding
                           : (int)((int64_t)space * first / (first + second));
    cut = space > 1 ? w_clamp_int(cut, 1, space - 1) : 1;
    n->bounds = r;
    n->cut = cut;
    n->available = space;
    n->divider = (n->axis == split_x ? r.x : r.y) + cut + padding / 2;
    if (!n->manual && space > 1)
        n->ratio = (int)(((int64_t)cut * 1000 + space / 2) / space);
    int rest = space > 1 ? space - cut : 1;
    if (n->axis == split_x) {
        a.w = cut;
        b.x += size > 1 ? cut + padding : 0;
        b.w = rest;
    } else {
        a.h = cut;
        b.y += size > 1 ? cut + padding : 0;
        b.h = rest;
    }
    w_place(m, n->first, a, gap, counts);
    w_place(m, n->second, b, gap, counts);
}

void w_layout_compute_margins(w_desktop_model *m, int output, w_rect usable, w_rect screen, int gap, int outer) {
    int counts[window_limit * 2 + 1] = {0};
    w_workspace *ws = &m->spaces[output][m->selected[output]];
    int slots[window_limit], length = 0;
    int total = w_measure(m, ws->root, counts, slots, &length);
    int margin = w_clamp_int(outer, 0, (usable.w < usable.h ? usable.w : usable.h) / 4);
    usable.x += margin;
    usable.y += margin;
    usable.w -= 2 * margin;
    usable.h -= 2 * margin;
    if (ws->layout == layout_tree || ws->layout == layout_alternate)
        w_place(m, ws->root, usable, gap, counts);
    int index = 0, previous_row = -1;
    int rows = total > 1 ? total-1 : 0;
    int row_gap = rows > 1 && usable.h > rows ? w_clamp_int(gap,0,(usable.h-rows)/(rows-1)) : 0;
    int row_pixels = usable.h - (rows > 1 ? (rows-1)*row_gap : 0);
    int row_top = 0;
    int64_t weights = 0, used_weight = 0;
    if (ws->layout == layout_master) {
        ws->master_slot = -1;
        ws->master_area = usable;
        ws->master_gap = w_clamp_int(gap,0,usable.w > 2 ? usable.w-2 : 0);
        for (int j = 0; j < length; ++j) {
            int slot = slots[j];
            if (!w_tiled(m, slot))
                continue;
            if (ws->master_slot < 0)
                ws->master_slot = slot;
            else
                weights += m->windows[slot].stack_weight;
        }
    }
    for (int j = 0; j < length; j++) {
        int i = slots[j];
        w_model_window *w = &m->windows[i];
        if (!w_model_visible(m, i) || w->output != output)
            continue;
        if (w->fullscreen) {
            w->desired = screen;
            continue;
        }
        if (w->floating) {
            w->desired = w->floating_bounds;
            continue;
        }
        if (ws->layout == layout_tabs || ws->layout == layout_stack) {
            w_rect r = usable;
            if (ws->layout == layout_stack) {
                int step = w_clamp_int(usable.h / (total + 1), 0, 24);
                r.y += index * step;
                r.h -= (total - 1) * step;
            }
            w->desired = r;
        } else if (ws->layout == layout_master) {
            w_rect r = usable;
            if (total > 1) {
                int available = usable.w-ws->master_gap;
                int cut =
                    w_clamp_int((int)((int64_t)available * (ws->master_ratio ? ws->master_ratio : 500) / 1000),
                              1, available > 1 ? available - 1 : 1);
                if (!index)
                    r.w = cut;
                else {
                    int row = index-1;
                    used_weight += w->stack_weight;
                    r.x += available > 1 ? cut + ws->master_gap : 0;
                    r.w = w_clamp_int(available - cut, 1, available);
                    if (rows <= row_pixels) {
                        // Reserve at least one pixel for each remaining row, including at extreme weights.
                        int bottom = w_clamp_int((int)(row_pixels * used_weight / weights),
                                                 row_top+1,row_pixels-(rows-row-1));
                        r.y += row_top + row*row_gap;
                        r.h = bottom-row_top;
                        row_top = bottom;
                    } else {
                        // More windows than scan lines: share pixels while keeping every tile on this output.
                        r.y += (int)((int64_t)row*usable.h/rows);
                        r.h = 1;
                    }
                    if (previous_row >= 0)
                        m->windows[previous_row].next_row = i;
                    previous_row = i;
                    w->next_row = -1;
                }
            }
            w->desired = r;
        }
        ++index;
    }
}

void w_layout_compute(w_desktop_model *m, int output, w_rect usable, w_rect screen, int gap) {
    w_layout_compute_margins(m, output, usable, screen, gap, gap);
}

////////////////
//~ Directional navigation

static bool w_horizontal(enum direction dir) {
    return dir == dir_left || dir == dir_right;
}

static bool w_backward(enum direction dir) {
    return dir == dir_left || dir == dir_up;
}

static int w_edge_gap(int a, int length_a, int b, int length_b) {
    if (a + length_a <= b)
        return b - (a + length_a);
    if (b + length_b <= a)
        return a - (b + length_b);
    return 0;
}

int w_layout_neighbor(const w_desktop_model *m, int output, w_rect origin, enum direction dir, bool tiles_only) {
    int best = -1, best_gap = INT_MAX;
    int64_t distance = INT64_MAX;
    for (int i = 0; i < window_limit; ++i) {
        const w_model_window *w = &m->windows[i];
        if (i == m->focus || w->output != output || !w_model_visible(m, i) ||
            (tiles_only && (w->floating || w->fullscreen)))
            continue;
        w_rect b = w->desired;
        int64_t dx = ((int64_t)b.x - origin.x) * 2 + b.w - origin.w;
        int64_t dy = ((int64_t)b.y - origin.y) * 2 + b.h - origin.h;
        int64_t forward = w_horizontal(dir) ? dx : dy, side = w_horizontal(dir) ? dy : dx;
        if (w_backward(dir))
            forward = -forward;
        if (forward <= 0)
            continue;
        bool aligned = w_horizontal(dir) ? (origin.y < b.y + b.h && b.y < origin.y + origin.h)
                                       : (origin.x < b.x + b.w && b.x < origin.x + origin.w);
        if (tiles_only && m->focus >= 0 && output == m->windows[m->focus].output && !aligned)
            continue;
        int gap =
            w_horizontal(dir) ? w_edge_gap(origin.y, origin.h, b.y, b.h) : w_edge_gap(origin.x, origin.w, b.x, b.w);
        int64_t score = forward * forward + side * side;
        if (gap < best_gap || (gap == best_gap && score < distance)) {
            best = i;
            best_gap = gap;
            distance = score;
        }
    }
    return best;
}

int w_layout_adjacent_output(const w_rect screens[output_limit], int output, w_rect origin, enum direction dir) {
    w_rect from = screens[output];
    int best = -1, best_alignment = -1;
    int64_t best_distance = INT64_MAX, best_projection = INT64_MAX;
    for (int i = 0; i < output_limit; i++) {
        w_rect b = screens[i];
        if (i == output || b.w <= 0 || b.h <= 0)
            continue;
        int forward;
        switch (dir) {
        case dir_left:
            forward = from.x - (b.x + b.w);
            break;
        case dir_right:
            forward = b.x - (from.x + from.w);
            break;
        case dir_up:
            forward = from.y - (b.y + b.h);
            break;
        default:
            forward = b.y - (from.y + from.h);
            break;
        }
        if (forward < 0)
            continue;
        int side =
            w_horizontal(dir) ? w_edge_gap(origin.y, origin.h, b.y, b.h) : w_edge_gap(origin.x, origin.w, b.x, b.w);
        int aligned = w_horizontal(dir) ? (origin.y < b.y + b.h && b.y < origin.y + origin.h)
                                      : (origin.x < b.x + b.w && b.x < origin.x + origin.w);
        int64_t score = (int64_t)forward * forward + (int64_t)side * side;
        // Two stacked displays may share the same edge. Prefer the one opposite the window's center.
        int64_t projection = w_horizontal(dir) ? ((int64_t)origin.y - b.y) * 2 + origin.h - b.h
                                             : ((int64_t)origin.x - b.x) * 2 + origin.w - b.w;
        if (projection < 0)
            projection = -projection;
        if (aligned > best_alignment ||
            (aligned == best_alignment &&
             (score < best_distance || (score == best_distance && projection < best_projection)))) {
            best = i;
            best_alignment = aligned;
            best_distance = score;
            best_projection = projection;
        }
    }
    return best;
}

////////////////
//~ Window relocation

// Preserve relative position when a work area changes size, including negative desktop origins.
static int w_relocate_axis(int position, int extent, int old_start, int old_size, int new_start, int new_size) {
    int travel = old_size - extent;
    if (travel <= 0 || new_size <= extent)
        return new_start;
    return new_start +
           (int)((int64_t)w_clamp_int(position - old_start, 0, travel) * (new_size - extent) / travel);
}

w_rect w_layout_relocate(w_rect r, w_rect from, w_rect to) {
    r.x = w_relocate_axis(r.x, r.w, from.x, from.w, to.x, to.w);
    r.y = w_relocate_axis(r.y, r.h, from.y, from.h, to.y, to.h);
    r.w = w_clamp_int(r.w, 1, to.w);
    r.h = w_clamp_int(r.h, 1, to.h);
    return r;
}

// Tabs and stacked windows can occupy the same rectangle. Navigate their actual tree order.
int w_layout_sibling(const w_desktop_model *m, int slot, enum direction dir) {
    int node = m->windows[slot].leaf;
    bool previous = w_backward(dir);
    while (node) {
        int parent = m->nodes[node].parent;
        if (!parent)
            break;
        const w_tile_node *p = &m->nodes[parent];
        if (node == (previous ? p->second : p->first)) {
            node = previous ? p->first : p->second;
            while (m->nodes[node].slot < 0)
                node = previous ? m->nodes[node].second : m->nodes[node].first;
            int candidate = m->nodes[node].slot;
            if (w_tiled(m, candidate))
                return candidate;
        } else {
            node = parent;
        }
    }
    return -1;
}
