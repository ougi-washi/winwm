#include "desktop/desktop.h"

////////////////
//~ Navigation

void w_desktop_select(int i) {
    if (i >= 0)
        w_desktop_activate(i);
    else
        w_desktop_focus_empty();
}

static void w_measure_output(int output) {
    w_output_state *o = &desk.outputs[output];
    w_layout_compute_margins(&desk.model, output, o->work, o->screen, MulDiv(desk.config.gap, o->dpi, 96),
                           MulDiv(desk.config.outer_gap, o->dpi, 96));
}

void w_desktop_resize_axis(enum split_axis axis, int delta) {
    w_desktop_model *m = &desk.model;
    if (m->focus < 0 || !w_model_visible(m, m->focus))
        return;
    w_model_window *w = &m->windows[m->focus];
    if (w->fullscreen || !delta)
        return;
    w_output_state *output = &desk.outputs[w->output];
    if (w->floating) {
        w_rect r = w->floating_bounds;

        if (axis == split_x) {
            int limit = output->work.x + output->work.w - r.x;
            r.w = w_clamp_int(r.w + delta, 1, max(1, max(limit, r.w)));
        } else {
            int limit = output->work.y + output->work.h - r.y;
            r.h = w_clamp_int(r.h + delta, 1, max(1, max(limit, r.h)));
        }
        w_desktop_place_floating(m->focus, r);
        return;
    }
    // Measure from the latest ratios even when multiple key repeats share one paint batch.
    w_measure_output(w->output);
    w_model_resize_pixels(m, axis, delta);
}

void w_desktop_resize_direction(int direction) {
    w_desktop_resize_axis(direction == dir_left || direction == dir_right ? split_x : split_y,
                        direction == dir_left || direction == dir_up ? -100 : 100);
}

////////////////
//~ Moving windows between outputs

static int w_next_output(int from) {
    for (int j = 1; j <= output_limit; j++)
        if (desk.outputs[(from + j) % output_limit].live)
            return (from + j) % output_limit;
    return from;
}

static void w_clear_fullscreen(int output, int ws) {
    for (int i = 0; i < window_limit; ++i) {
        w_model_window *w = &desk.model.windows[i];
        if (!w->live || !w->fullscreen || w->output != output || w->workspace != ws)
            continue;
        w->fullscreen = false;
        if (w->floating)
            desk.windows[i].floating_update = float_move | float_size;
    }
}

static void w_move_to(int slot, int output, int ws, int anchor, enum split_axis axis, bool before) {
    w_desktop_model *m = &desk.model;
    w_model_window *w = &m->windows[slot];
    if (w->output == output && w->workspace == ws)
        return;
    if (w->floating && w->output != output) {
        w_rect next =
            w_layout_relocate(w->floating_bounds, desk.outputs[w->output].work, desk.outputs[output].work);
        w_desktop_place_floating(slot, next);
    }
    if (w->fullscreen)
        w_clear_fullscreen(output, ws);
    desk.windows[slot].displaced_from = 0;
    w_model_move_beside(m, slot, output, ws, anchor, axis, before);
}

void w_desktop_move_window(int slot, int output, int ws) {
    w_desktop_model *m = &desk.model;
    w_move_to(slot, output, ws, m->focus, m->spaces[output][ws].next_axis, false);
}

bool w_desktop_tile_drop(int slot, POINT point, w_rect *preview, bool commit) {
    w_desktop_model *m = &desk.model;
    w_model_window *source = &m->windows[slot];
    HMONITOR monitor = MonitorFromPoint(point, MONITOR_DEFAULTTONULL);
    HWND hit = GetAncestor(WindowFromPoint(point), GA_ROOT);
    int under = w_desktop_find(hit);
    // During a native caption move Windows puts the source above its destination.
    // During a modifier drag it stays in place, so dropping onto itself is a no-op.
    if (under == slot && desk.windows[slot].native_move != move_dragging)
        return false;
    if (under >= 0 && (m->windows[under].floating || m->windows[under].fullscreen))
        return false;
    if (under < 0 && hit && !w_os_desktop_surface(hit))
        return false;
    for (int o = 0; o < output_limit; o++) {
        if (!desk.outputs[o].live || desk.outputs[o].monitor != monitor)
            continue;
        RECT work = w_os_to_rect(desk.outputs[o].work);
        if (!PtInRect(&work, point))
            return false;
        int ws = m->selected[o], target = -1;
        bool occupied = false;
        for (int i = 0; i < window_limit; i++) {
            w_model_window *w = &m->windows[i];
            if (i == slot || w->output != o || !w_model_visible(m, i) || w->floating || w->fullscreen ||
                !desk.windows[i].wants_visible)
                continue;
            occupied = true;
            RECT r = w_os_to_rect(w->desired);
            if (i == under) {
                target = i;
                break;
            }
            if (PtInRect(&r, point) && (target < 0 || w->focus_order > m->windows[target].focus_order))
                target = i;
        }
        if (target < 0 && (o == source->output || occupied))
            return false;
        int edge = -1;
        if (target < 0)
            *preview = desk.outputs[o].work;
        else {
            w_rect r = m->windows[target].desired;
            edge = w_layout_drop_edge(r, w_clamp_int(point.x,r.x,r.x+r.w-1),
                                      w_clamp_int(point.y,r.y,r.y+r.h-1), preview);
        }
        if (!commit)
            return true;
        desk.windows[slot].displaced_from = 0;
        if (target < 0)
            w_move_to(slot, o, ws, -1, m->spaces[o][ws].next_axis, false);
        else if (edge < 0) {
            desk.windows[target].displaced_from = 0;
            w_model_swap(m, slot, target);
        } else
            w_model_insert_tile(m, slot, target, edge == dir_left || edge == dir_right ? split_x : split_y,
                                edge == dir_left || edge == dir_up);
        if (m->focus >= 0 && (m->focus == slot || m->focus == target))
            w_model_focus(m, m->focus);
        w_desktop_schedule();
        return true;
    }
    return false;
}

void w_desktop_drop_tile(int slot, POINT point) {
    w_rect preview;
    w_desktop_tile_drop(slot, point, &preview, true);
}

void w_desktop_toggle_fullscreen(int slot) {
    w_model_window *w = &desk.model.windows[slot];
    bool enable = !w->fullscreen;
    w_clear_fullscreen(w->output, w->workspace);
    w->fullscreen = enable;
    w_desktop_stack_floats(slot,true);
}

static void w_transfer(int slot, int output, int anchor, enum direction dir) {
    enum split_axis axis = dir == dir_left || dir == dir_right ? split_x : split_y;
    w_move_to(slot, output, desk.model.selected[output], anchor, axis, dir == dir_right || dir == dir_down);
    w_desktop_select(slot);
}

void w_desktop_navigate(enum direction dir, bool move) {
    w_desktop_model *m = &desk.model;
    int slot = m->focus;
    w_model_window *w = slot >= 0 && w_model_visible(m, slot) ? &m->windows[slot] : NULL;
    int from = w ? w->output : m->output;
    w_rect screens[output_limit] = {0};
    for (int i = 0; i < output_limit; i++)
        if (desk.outputs[i].live)
            screens[i] = desk.outputs[i].screen;
    if (!screens[from].w || (move && !w))
        return;
    if (w && desk.refresh_pending)
        w_measure_output(from);
    w_rect origin = w ? w->desired : screens[from];
    if (w && w->floating && !w->fullscreen)
        origin = w->floating_bounds;
    enum layout_kind layout = m->spaces[from][m->selected[from]].layout;
    if (w && !(move && (w->floating || w->fullscreen))) {
        int local = !w->floating && (layout == layout_tabs || layout == layout_stack)
                        ? w_layout_sibling(m, slot, dir)
                        : w_layout_neighbor(m, from, origin, dir, move);
        if (local >= 0) {
            if (move)
                w_model_swap(m, slot, local);
            else
                w_desktop_select(local);
            return;
        }
    }
    int target = w_layout_adjacent_output(screens, from, origin, dir);
    if (target < 0) {
        if (move && w->floating && !w->fullscreen) {
            w_rect r = origin, area = desk.outputs[from].work;
            int step = MulDiv(32, desk.outputs[from].dpi, 96);
            if (dir == dir_left || dir == dir_right)
                r.x = w_clamp_int(r.x + (dir == dir_left ? -step : step), area.x,
                                max(area.x, area.x + area.w - r.w));
            else
                r.y = w_clamp_int(r.y + (dir == dir_up ? -step : step), area.y,
                                max(area.y, area.y + area.h - r.h));
            w_desktop_place_floating(slot, r);
        }
        return;
    }
    if (desk.refresh_pending)
        w_measure_output(target);
    int anchor = w_layout_neighbor(m, target, origin, dir, move);
    if (move)
        w_transfer(slot, target, anchor, dir);
    else {
        m->output = target;
        w_desktop_select(anchor >= 0 ? anchor : w_model_recent(m, target, m->selected[target]));
    }
}

void w_desktop_cycle_output(bool carry) {
    w_desktop_model *m = &desk.model;
    if (carry && m->focus < 0)
        return;
    int from = carry ? m->windows[m->focus].output : m->output;
    int next = w_next_output(from);
    if (next == from)
        return;
    if (carry)
        w_transfer(m->focus, next, -1, dir_right);
    else {
        m->output = next;
        w_desktop_select(w_model_recent(m, next, m->selected[next]));
    }
}
