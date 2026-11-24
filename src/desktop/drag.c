#include "desktop/desktop.h"
#include <limits.h>
#include <stdlib.h>
enum drag_kind { drag_idle, drag_tree, drag_master, drag_float };

////////////////
//~ Drag

static struct w_drag_state {
    enum drag_kind kind;
    int node, cut, start, available, workspace, output;
    uint64_t generation;
    bool rows, moved;
    POINT origin;
    w_rect initial;
    bool left, top;
    int weight;
    w_window_id first, second;
} drag_state;

////////////////
//~ Drag Functions

static bool w_master_begin(w_desktop_model *m, w_workspace *ws, int slot, POINT point, int tolerance) {
    if (ws->master_slot < 0)
        return false;
    w_model_window *master = &m->windows[ws->master_slot];
    w_rect area = ws->master_area;
    if (point.x < area.x || point.x >= area.x + area.w || point.y < area.y || point.y >= area.y + area.h ||
        master->desired.w == area.w)
        return false;
    int seam = master->desired.x + master->desired.w + ws->master_gap / 2;
    int best = abs((int)point.x - seam), row = -1;
    if (point.x > seam) {
        for (int i = 0; i < window_limit; ++i) {
            w_model_window *w = &m->windows[i];
            if (!w->live || w->floating || w->fullscreen || !w_model_visible(m, i) ||
                w->output != master->output || i == ws->master_slot || w->next_row < 0 ||
                (slot >= 0 && slot != i && slot != w->next_row))
                continue;
            int d = abs((int)point.y - (w->desired.y + w->desired.h + ws->master_gap / 2));
            if (d < best) {
                row = i;
                best = d;
            }
        }
    }
    if (best > tolerance)
        return false;
    drag_state.kind = drag_master;
    drag_state.rows = row >= 0;
    drag_state.moved = false;
    drag_state.start = row >= 0 ? point.y : point.x;
    drag_state.output = master->output;
    drag_state.workspace = master->workspace;
    if (row >= 0) {
        w_model_window *a = &m->windows[row], *b = &m->windows[a->next_row];
        drag_state.first = (w_window_id){row, a->generation};
        drag_state.second = (w_window_id){a->next_row, b->generation};
        drag_state.weight = a->stack_weight + b->stack_weight;
        drag_state.cut = a->desired.h;
        drag_state.available = a->desired.h + b->desired.h;
    } else {
        drag_state.first = (w_window_id){ws->master_slot, master->generation};
        drag_state.cut = master->desired.w;
        drag_state.available = area.w - ws->master_gap;
    }
    return drag_state.available > 1;
}

static void w_master_update(w_desktop_model *m, POINT point) {
    w_workspace *ws = &m->spaces[drag_state.output][drag_state.workspace];
    w_window_id first = drag_state.first, second = drag_state.second;
    if (ws->layout != layout_master || m->selected[drag_state.output] != drag_state.workspace ||
        !w_model_valid(m, first) || !w_model_visible(m, first.slot))
        return;
    int shift = (drag_state.rows ? point.y : point.x) - drag_state.start;
    if (!shift && !drag_state.moved)
        return;
    drag_state.moved = true;
    int pixels = w_clamp_int(drag_state.cut + shift, 1, drag_state.available - 1);
    if (drag_state.rows) {
        w_model_window *a = &m->windows[first.slot];
        if (!w_model_valid(m, second) || !w_model_visible(m, second.slot) || a->next_row != second.slot)
            return;
        int weight = w_clamp_int((int)((int64_t)pixels * drag_state.weight / drag_state.available), 1,
                               drag_state.weight - 1);
        if (weight == a->stack_weight)
            return;
        a->stack_weight = weight;
        m->windows[second.slot].stack_weight = drag_state.weight - weight;
    } else {
        if (ws->master_slot != first.slot)
            return;
        int ratio = w_clamp_int((int)((int64_t)pixels * 1000 / drag_state.available), 10, 990);
        if (ws->master_ratio == ratio)
            return;
        ws->master_ratio = ratio;
    }
    w_desktop_schedule();
}

////////////////
//~ Floating window drag

static void w_float_begin(int slot, POINT point) {
    const w_model_window *w = &desk.model.windows[slot];
    drag_state.kind = drag_float;
    drag_state.first = (w_window_id){slot, w->generation};
    drag_state.output = w->output;
    drag_state.workspace = w->workspace;
    drag_state.origin = point;
    drag_state.initial = w_os_bounds(desk.windows[slot].hwnd);
    const w_rect r = drag_state.initial;
    drag_state.left = point.x < r.x + r.w / 2;
    drag_state.top = point.y < r.y + r.h / 2;
}

// Anchor the opposite corner for the entire gesture, including when the pointer crosses it.
static void w_float_update(POINT point) {
    w_window_id id = drag_state.first;
    if (!w_model_valid(&desk.model, id))
        return;
    w_model_window *w = &desk.model.windows[id.slot];
    if (!w_model_visible(&desk.model, id.slot) || !w->floating || w->fullscreen ||
        w->output != drag_state.output || w->workspace != drag_state.workspace)
        return;
    w_rect area = desk.outputs[w->output].work;
    w_rect start = drag_state.initial;
    int left = start.x, top = start.y, right = left + start.w, bottom = top + start.h;
    int dx = point.x - drag_state.origin.x, dy = point.y - drag_state.origin.y;
    if (drag_state.left)
        left = w_clamp_int(left + dx, min(area.x, left), right - 1);
    else
        right = w_clamp_int(right + dx, left + 1, max(area.x + area.w, right));
    if (drag_state.top)
        top = w_clamp_int(top + dy, min(area.y, top), bottom - 1);
    else
        bottom = w_clamp_int(bottom + dy, top + 1, max(area.y + area.h, bottom));
    w_rect next = {left, top, right - left, bottom - top};
    w_desktop_place_floating(id.slot, next);
}

bool w_desktop_drag(enum input_drag_event event, POINT point) {
    w_desktop_model *m = &desk.model;
    if (event == input_drag_move || event == input_drag_end) {
        if (drag_state.kind == drag_idle)
            return false;
        if (drag_state.kind != drag_tree) {
            if (drag_state.kind == drag_float)
                w_float_update(point);
            else
                w_master_update(m, point);
            if (event == input_drag_end)
                drag_state.kind = drag_idle;
            return true;
        }
        w_tile_node *n = &m->nodes[drag_state.node];
        w_workspace *ws = &m->spaces[drag_state.output][drag_state.workspace];
        if (drag_state.node && n->live && n->generation == drag_state.generation &&
            n->available == drag_state.available && m->selected[drag_state.output] == drag_state.workspace &&
            (ws->layout == layout_tree || ws->layout == layout_alternate)) {
            int delta = (n->axis == split_x ? point.x : point.y) - drag_state.start;
            if (delta || n->manual) {
                int ratio =
                    w_clamp_int((int)((int64_t)(drag_state.cut + delta) * 1000 / n->available), 10, 990);
                if (ratio != n->ratio) {
                    n->manual = true;
                    n->ratio = ratio;
                    w_desktop_schedule();
                }
            }
        } else
            drag_state.kind = drag_idle;
        if (event == input_drag_end)
            drag_state.kind = drag_idle;
        return true;
    }
    drag_state.kind = drag_idle;
    HWND hit = GetAncestor(WindowFromPoint(point), GA_ROOT);
    int slot = w_desktop_find(hit), output = -1;
    if (slot >= 0) {
        w_model_window *w = &m->windows[slot];
        if (!w_model_visible(m, slot) || w->fullscreen)
            return false;
        if (w->floating) {
            if (event != input_drag_inside || IsZoomed(hit) ||
                !(GetWindowLongPtrW(hit, GWL_STYLE) & WS_THICKFRAME))
                return false;
            w_float_begin(slot, point);
            return true;
        }
        output = w->output;
    } else if (event == input_drag_edge && w_os_desktop_surface(hit)) {
        HMONITOR monitor = MonitorFromPoint(point, MONITOR_DEFAULTTONULL);
        for (int i = 0; i < output_limit; ++i) {
            w_output_state *o = &desk.outputs[i];
            if (o->live && o->monitor == monitor) {
                output = i;
                break;
            }
        }
    }
    if (output < 0)
        return false;
    w_workspace *ws = &m->spaces[output][m->selected[output]];
    int tolerance = MulDiv(4 + desk.config.gap / 2, desk.outputs[output].dpi, 96);
    if (ws->layout == layout_master)
        return w_master_begin(m, ws, slot, point, event == input_drag_inside ? INT_MAX : tolerance);
    if (ws->layout != layout_tree && ws->layout != layout_alternate)
        return false;
    int best = 0, distance = INT_MAX;
    int node = slot >= 0 ? m->nodes[m->windows[slot].leaf].parent : 1;
    while (node > 0 && node < (int)count_of(m->nodes)) {
        w_tile_node *n = &m->nodes[node];
        w_rect r = n->bounds;
        if (n->live && n->available > 1 && point.x >= r.x && point.x < r.x + r.w && point.y >= r.y &&
            point.y < r.y + r.h) {
            int d = abs((int)(n->axis == split_x ? point.x : point.y) - n->divider);
            if (d < distance && (event == input_drag_inside || d <= tolerance)) {
                int root = node;
                while (m->nodes[root].parent)
                    root = m->nodes[root].parent;
                if (root == ws->root) {
                    distance = d;
                    best = node;
                }
            }
        }
        node = slot >= 0 ? n->parent : node + 1;
    }
    if (!best)
        return false;
    w_tile_node *n = &m->nodes[best];
    drag_state.kind = drag_tree;
    drag_state.node = best;
    drag_state.generation = n->generation;
    drag_state.start = n->axis == split_x ? point.x : point.y;
    drag_state.cut = n->cut;
    drag_state.available = n->available;
    drag_state.output = output;
    drag_state.workspace = m->selected[output];
    return true;
}
