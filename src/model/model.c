#include "model/model.h"

////////////////
//~ Model

void w_model_init(w_desktop_model *m) {
    memset(m, 0, sizeof(*m));
    m->focus = -1;
}

static int w_alloc_node(w_desktop_model *m) {
    for (int i = 1; i < (int)count_of(m->nodes); ++i) {
        if (!m->nodes[i].live) {
            m->nodes[i] = (w_tile_node){.live = true, .slot = -1, .ratio = 500, .generation = ++m->serial};
            return i;
        }
    }
    return 0;
}

static void w_replace_child(w_desktop_model *m, w_workspace *ws, int parent, int old, int child) {
    if (!parent)
        ws->root = child;
    else if (m->nodes[parent].first == old)
        m->nodes[parent].first = child;
    else
        m->nodes[parent].second = child;
    if (child)
        m->nodes[child].parent = parent;
}

static void w_attach(w_desktop_model *m, int slot, int anchor, enum split_axis axis, bool before) {
    w_model_window *w = &m->windows[slot];
    w_workspace *ws = &m->spaces[w->output][w->workspace];
    int leaf = w->leaf = w_alloc_node(m);
    m->nodes[leaf].slot = slot;
    if (!ws->root) {
        ws->root = leaf;
        return;
    }
    int target = ws->root;
    if (anchor >= 0 && anchor != slot && m->windows[anchor].live && m->windows[anchor].output == w->output &&
        m->windows[anchor].workspace == w->workspace)
        target = m->windows[anchor].leaf;
    while (m->nodes[target].slot < 0)
        target = m->nodes[target].second;
    int parent = m->nodes[target].parent, branch = w_alloc_node(m);
    w_replace_child(m, ws, parent, target, branch);
    w_tile_node *b = &m->nodes[branch];
    b->axis = axis;
    if (ws->layout == layout_alternate)
        ws->next_axis = b->axis == split_x ? split_y : split_x;
    b->first = before ? leaf : target;
    b->second = before ? target : leaf;
    m->nodes[target].parent = m->nodes[leaf].parent = branch;
}

static void w_detach(w_desktop_model *m, int slot) {
    w_model_window *w = &m->windows[slot];
    w_workspace *ws = &m->spaces[w->output][w->workspace];
    int leaf = w->leaf, parent = m->nodes[leaf].parent;
    if (!parent)
        ws->root = 0;
    else {
        w_tile_node *p = &m->nodes[parent];
        w_replace_child(m, ws, p->parent, parent, p->first == leaf ? p->second : p->first);
        p->live = false;
    }
    m->nodes[leaf].live = false;
    w->leaf = 0;
}

////////////////
//~ Window membership

w_window_id w_model_add(w_desktop_model *m, int output, int ws, w_rect original) {
    if ((unsigned)output >= output_limit || (unsigned)ws >= workspace_limit)
        return (w_window_id){-1, 0};
    for (int i = 0; i < window_limit; ++i) {
        w_model_window *w = &m->windows[i];
        if (w->live)
            continue;
        uint32_t gen = w->generation + 1;
        *w = (w_model_window){.live = true,
                            .stack_weight = 1024,
                            .generation = gen ? gen : 1,
                            .output = output,
                            .workspace = ws,
                            .floating_bounds = original,
                            .desired = original,
                            .focus_order = ++m->serial};
        w_attach(m, i, m->focus, m->spaces[output][ws].next_axis, false);
        return (w_window_id){i, w->generation};
    }
    return (w_window_id){-1, 0};
}

bool w_model_valid(const w_desktop_model *m, w_window_id id) {
    return (unsigned)id.slot < window_limit && m->windows[id.slot].live &&
           m->windows[id.slot].generation == id.generation;
}

void w_model_remove(w_desktop_model *m, w_window_id id) {
    if (!w_model_valid(m, id))
        return;
    w_detach(m, id.slot);
    m->windows[id.slot].live = false;
    if (m->focus == id.slot)
        m->focus = w_model_recent(m, m->output, m->selected[m->output]);
}

void w_model_move_beside(w_desktop_model *m, int slot, int output, int ws, int anchor, enum split_axis axis,
                       bool before) {
    if ((unsigned)slot >= window_limit || !m->windows[slot].live || (unsigned)output >= output_limit ||
        (unsigned)ws >= workspace_limit)
        return;
    w_model_window *w = &m->windows[slot];
    if (w->output == output && w->workspace == ws)
        return;
    w_detach(m, slot);
    w->output = output;
    w->workspace = ws;
    w_attach(m, slot, anchor, axis, before);
}

void w_model_move(w_desktop_model *m, int slot, int output, int ws) {
    if ((unsigned)output < output_limit && (unsigned)ws < workspace_limit)
        w_model_move_beside(m, slot, output, ws, m->focus, m->spaces[output][ws].next_axis, false);
}

bool w_model_visible(const w_desktop_model *m, int slot) {
    const w_model_window *w = &m->windows[slot];
    return w->live && !w->withdrawn && !w->minimized && w->workspace == m->selected[w->output] &&
           (!w->scratch || w->scratch_visible);
}

////////////////
//~ Workspace focus

int w_model_recent(const w_desktop_model *m, int output, int ws) {
    int best = -1;
    for (int i = 0; i < window_limit; ++i) {
        const w_model_window *w = &m->windows[i];
        if (w_model_visible(m, i) && w->output == output && w->workspace == ws &&
            (best < 0 || w->focus_order > m->windows[best].focus_order))
            best = i;
    }
    return best;
}

void w_model_focus(w_desktop_model *m, int slot) {
    if ((unsigned)slot >= window_limit || !w_model_visible(m, slot))
        return;
    m->focus = slot;
    m->output = m->windows[slot].output;
    m->windows[slot].focus_order = ++m->serial;
}

void w_model_workspace(w_desktop_model *m, int ws) {
    if ((unsigned)ws >= workspace_limit || ws == m->selected[m->output])
        return;
    m->previous[m->output] = m->selected[m->output];
    m->selected[m->output] = ws;
    m->focus = w_model_recent(m, m->output, ws);
}

////////////////
//~ Split resizing

void w_model_resize(w_desktop_model *m, int delta) {
    if (m->focus < 0)
        return;
    w_model_window *focused = &m->windows[m->focus];
    w_workspace *ws = &m->spaces[focused->output][focused->workspace];
    if (ws->layout == layout_master) {
        int ratio = ws->master_ratio ? ws->master_ratio : 500;
        ws->master_ratio = w_clamp_int(ratio + (m->focus == ws->master_slot ? delta : -delta), 10, 990);
        return;
    }
    int leaf = focused->leaf, parent = m->nodes[leaf].parent;
    while (parent && !m->nodes[parent].available) {
        leaf = parent;
        parent = m->nodes[leaf].parent;
    }
    if (parent) {
        w_tile_node *p = &m->nodes[parent];
        p->manual = true;
        p->ratio = w_clamp_int(p->ratio + (p->first == leaf ? delta : -delta), 50, 950);
    }
}

void w_model_resize_pixels(w_desktop_model *m, enum split_axis axis, int delta) {
    if (m->focus < 0)
        return;
    w_model_window *w = &m->windows[m->focus];
    if (!w_model_visible(m, m->focus) || w->floating || w->fullscreen)
        return;
    w_workspace *ws = &m->spaces[w->output][w->workspace];
    if (ws->layout == layout_master) {
        if (axis == split_x && ws->master_area.w > ws->master_gap)
            w_model_resize(m, delta * 1000 / (ws->master_area.w - ws->master_gap));
        else if (axis == split_y && m->focus != ws->master_slot) {
            int peer = w->next_row;
            if (peer < 0) {
                for (int k = 0; k < window_limit; k++) {
                    w_model_window *candidate = &m->windows[k];
                    if (w_model_visible(m, k) && candidate->output == w->output && !candidate->floating &&
                        !candidate->fullscreen && k != ws->master_slot && candidate->next_row == m->focus) {
                        peer = k;
                        break;
                    }
                }
            }
            if (peer >= 0) {
                w_model_window *other = &m->windows[peer];
                int pixels = w->desired.h + other->desired.h;
                int weights = w->stack_weight + other->stack_weight;
                if (pixels > 1) {
                    int size = w_clamp_int(w->desired.h + delta, 1, pixels - 1);
                    w->stack_weight = w_clamp_int((int)((int64_t)size * weights / pixels), 1, weights - 1);
                    other->stack_weight = weights - w->stack_weight;
                }
            }
        }
        return;
    }
    for (int child = w->leaf; m->nodes[child].parent; child = m->nodes[child].parent) {
        w_tile_node *p = &m->nodes[m->nodes[child].parent];
        if (p->axis != axis || p->available < 2)
            continue;
        int pixels = w_clamp_int(p->cut + (p->first == child ? delta : -delta), 1, p->available - 1);
        p->ratio = w_clamp_int((int)(((int64_t)pixels * 1000 + p->available / 2) / p->available), 1, 999);
        p->manual = true;
        break;
    }
}

void w_model_swap(w_desktop_model *m, int a, int b) {
    if (a < 0 || b < 0 || a == b)
        return;
    w_model_window *x = &m->windows[a], *y = &m->windows[b];
    if (x->workspace != y->workspace || x->output != y->output)
        return;
    int leaf = x->leaf;
    x->leaf = y->leaf;
    y->leaf = leaf;
    m->nodes[x->leaf].slot = a;
    m->nodes[y->leaf].slot = b;
}
