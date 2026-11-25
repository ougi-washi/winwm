#include "layout/layout.h"
#include <limits.h>
#include <stdlib.h>

static w_window_id w_id(const w_desktop_model *m, int slot) {
    return (w_window_id){slot, m->windows[slot].generation};
}

static bool w_contains(w_rect r, int x, int y) {
    return x >= r.x && y >= r.y && x < r.x+r.w && y < r.y+r.h;
}

// Prefer the largest intersection, then the nearest monitor in a gap. Keep
// the current monitor on exact ties so a window straddling a seam stays put.
int w_layout_output_for_bounds(const w_rect screens[output_limit], int current, w_rect r) {
    int best = -1;
    int64_t area = -1, distance = INT64_MAX;
    for (int i = 0; i < output_limit; i++) {
        w_rect s = screens[i];
        if (s.w <= 0 || s.h <= 0)
            continue;
        int x = (s.x < r.x ? r.x : s.x), y = (s.y < r.y ? r.y : s.y);
        int width = (s.x+s.w < r.x+r.w ? s.x+s.w : r.x+r.w) - x;
        int height = (s.y+s.h < r.y+r.h ? s.y+s.h : r.y+r.h) - y;
        int64_t overlap = width > 0 && height > 0 ? (int64_t)width*height : 0;
        int64_t d = (width < 0 ? (int64_t)width*width : 0) + (height < 0 ? (int64_t)height*height : 0);
        if (overlap > area || (overlap == area && (d < distance || (d == distance && i == current)))) {
            best = i;
            area = overlap;
            distance = d;
        }
    }
    return best;
}

bool w_layout_grab_splits(const w_desktop_model *m, int output, int slot, int x, int y,
                          int tolerance, bool inside, w_split_grab grips[2]) {
    memset(grips, 0, 2*sizeof(*grips));
    if ((unsigned)output >= output_limit || (inside && slot < 0))
        return false;
    const w_workspace *ws = &m->spaces[output][m->selected[output]];
    if (slot >= 0 && ((unsigned)slot >= window_limit || !w_model_visible(m, slot) ||
        m->windows[slot].output != output || m->windows[slot].floating || m->windows[slot].fullscreen))
        return false;
    w_rect tile = slot >= 0 ? m->windows[slot].desired : (w_rect){0};
    int best[] = {INT_MAX, INT_MAX};
    if (ws->layout == layout_master) {
        int master = ws->master_slot;
        if (master < 0 || !w_contains(ws->master_area, x, y))
            return false;
        w_rect main = m->windows[master].desired;
        int available = ws->master_area.w-ws->master_gap;
        if (main.w < ws->master_area.w && available > 1) {
            best[split_x] = abs(x - (main.x + main.w + ws->master_gap/2));
            grips[split_x] = (w_split_grab){.kind=grab_column, .cut=main.w, .available=available,
                                             .first=w_id(m, master)};
        }
        if (x >= main.x + main.w) {
            for (int i = 0; i < window_limit; i++) {
                const w_model_window *a = &m->windows[i];
                int next = a->next_row;
                if (i == master || !w_model_visible(m, i) || a->output != output || a->floating || a->fullscreen ||
                    next < 0 || (slot >= 0 && slot != i && slot != next))
                    continue;
                const w_model_window *b = &m->windows[next];
                int bottom = a->desired.y+a->desired.h;
                int divider = bottom+(b->desired.y-bottom)/2;
                int distance = abs(y-divider);
                if (distance >= best[split_y])
                    continue;
                best[split_y] = distance;
                grips[split_y] = (w_split_grab){.kind=grab_row, .cut=a->desired.h,
                    .available=a->desired.h+b->desired.h, .weight=a->stack_weight+b->stack_weight,
                    .first=w_id(m,i), .second=w_id(m,next)};
            }
        }
    } else if (ws->layout == layout_tree || ws->layout == layout_alternate) {
        int child = slot >= 0 ? m->windows[slot].leaf : 0;
        int node = child ? m->nodes[child].parent : 1;
        while (node && (size_t)node < count_of(m->nodes)) {
            const w_tile_node *n = &m->nodes[node];
            if (n->live && n->available > 1 && (inside || w_contains(n->bounds, x, y))) {
                int axis = n->axis;
                int coordinate = axis == split_x ? x : y;
                int score = abs(coordinate - n->divider);
                if (inside) {
                    bool low = coordinate < (axis == split_x ? tile.x + tile.w/2 : tile.y + tile.h/2);
                    // Prefer the divider on the grabbed side, falling back to the other edge at an output boundary.
                    score = low == (n->second == child) ? 0 : 1;
                }
                int root = node;
                if (slot < 0)
                    while (m->nodes[root].parent)
                        root = m->nodes[root].parent;
                if (score < best[axis] && (slot >= 0 || root == ws->root)) {
                    best[axis] = score;
                    grips[axis] = (w_split_grab){.kind=grab_node, .node=node, .generation=n->generation,
                                                 .cut=n->cut, .available=n->available};
                }
            }
            child = node;
            node = slot >= 0 ? n->parent : node+1;
        }
    }
    if (!inside) {
        int axis = best[split_x] <= best[split_y] ? split_x : split_y;
        grips[1-axis].kind = grab_none;
        if (best[axis] > tolerance)
            grips[axis].kind = grab_none;
    }
    return grips[0].kind || grips[1].kind;
}

bool w_layout_drag_split(w_desktop_model *m, int output, int workspace, const w_split_grab *g, int delta) {
    if (!g->kind || g->available <= 1 || m->selected[output] != workspace)
        return false;
    w_workspace *ws = &m->spaces[output][workspace];
    int pixels = w_clamp_int(g->cut + delta, 1, g->available-1);
    int ratio = w_clamp_int((int)((int64_t)pixels*1000/g->available), 10, 990);
    if (g->kind == grab_node) {
        w_tile_node *n = &m->nodes[g->node];
        if ((ws->layout != layout_tree && ws->layout != layout_alternate) || !n->live ||
            n->generation != g->generation || n->available != g->available || n->ratio == ratio)
            return false;
        n->manual = true;
        n->ratio = ratio;
    } else {
        if (ws->layout != layout_master || !w_model_valid(m, g->first) || !w_model_visible(m, g->first.slot))
            return false;
        if (g->kind == grab_column) {
            if (ws->master_slot != g->first.slot || ws->master_ratio == ratio)
                return false;
            ws->master_ratio = ratio;
        } else {
            w_model_window *a = &m->windows[g->first.slot];
            if (a->output != output || a->workspace != workspace || !w_model_valid(m,g->second) ||
                a->next_row != g->second.slot || !w_model_visible(m,g->second.slot))
                return false;
            int weight = w_clamp_int((int)((int64_t)pixels*g->weight/g->available), 1, g->weight-1);
            if (a->stack_weight == weight)
                return false;
            a->stack_weight = weight;
            m->windows[g->second.slot].stack_weight = g->weight-weight;
        }
    }
    return true;
}

int w_layout_drop_edge(w_rect tile, int x, int y, w_rect *indicator) {
    int distance[] = {x-tile.x, tile.y+tile.h-1-y, y-tile.y, tile.x+tile.w-1-x};
    int thickness = w_clamp_int((tile.w < tile.h ? tile.w : tile.h)/4, 1, INT_MAX);
    int edge = 0;
    for (int i=1; i<4; i++)
        if (distance[i] < distance[edge])
            edge = i;
    *indicator = tile;
    if (distance[edge] >= thickness) {
        indicator->x += thickness;
        indicator->y += thickness;
        indicator->w -= 2*thickness;
        indicator->h -= 2*thickness;
        return -1;
    }
    if (edge == dir_left || edge == dir_right) {
        if (edge == dir_right)
            indicator->x += tile.w-thickness;
        indicator->w = thickness;
    } else {
        if (edge == dir_down)
            indicator->y += tile.h-thickness;
        indicator->h = thickness;
    }
    return edge;
}

// A per-monitor-DPI-aware app can resize itself in the middle of a move.
// Preserve the same relative point in its new frame, including at negative origins.
static int w_scale_offset(int offset, int from, int to) {
    int64_t numerator = (int64_t)offset * to;
    return (int)((numerator + (offset < 0 ? -from/2 : from/2))/from);
}

w_rect w_layout_drag_position(w_rect initial, w_rect actual, int offset_x, int offset_y, int x, int y) {
    if (initial.w <= 0 || initial.h <= 0)
        return initial;
    if (actual.w <= 0 || actual.h <= 0)
        actual = initial;
    actual.x = x - w_scale_offset(offset_x, initial.w, actual.w);
    actual.y = y - w_scale_offset(offset_y, initial.h, actual.h);
    return actual;
}

w_rect w_layout_drag_corner(w_rect r, int dx, int dy, bool left, bool top,
                            int min_w, int min_h, int max_w, int max_h) {
    int width = w_clamp_int(r.w + (left ? -dx : dx), min_w, max_w);
    int height = w_clamp_int(r.h + (top ? -dy : dy), min_h, max_h);
    return (w_rect){r.x + (left ? r.w-width : 0), r.y + (top ? r.h-height : 0), width, height};
}
