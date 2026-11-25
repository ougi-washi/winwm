// gcc -Isrc tools/test_pointer_layout.c src/layout/{layout,gesture}.c src/model/model.c
#include "layout/layout.h"
#include <assert.h>
#include <stdio.h>

static w_desktop_model m;
static w_rect screens[output_limit] = {
    {0,0,1920,1080}, {-1600,-300,1600,1200}, {320,-1740,2560,1440}
};

static void measure(int o) {
    w_layout_compute_margins(&m, o, screens[o], screens[o], 10, 8);
}

static void visit_tile(int node, int parent, int output, int workspace, bool *seen) {
    assert(node > 0 && (size_t)node < count_of(m.nodes) && !seen[node]);
    seen[node] = true;
    const w_tile_node *tile = &m.nodes[node];
    assert(tile->live && tile->parent == parent);
    if (tile->slot < 0) {
        visit_tile(tile->first,node,output,workspace,seen);
        visit_tile(tile->second,node,output,workspace,seen);
    } else {
        assert(tile->slot < window_limit);
        const w_model_window *w = &m.windows[tile->slot];
        assert(w->live && w->leaf == node && w->output == output && w->workspace == workspace);
    }
}

static void check_workspace_trees(void) {
    bool seen[count_of(m.nodes)] = {0};
    for (int o = 0; o < output_limit; o++)
        for (int ws = 0; ws < workspace_limit; ws++)
            if (m.spaces[o][ws].root)
                visit_tile(m.spaces[o][ws].root,0,o,ws,seen);
    for (size_t i = 1; i < count_of(m.nodes); i++)
        assert(seen[i] == m.nodes[i].live);
    for (int i = 0; i < window_limit; i++) {
        if (!m.windows[i].live)
            continue;
        int leaf = m.windows[i].leaf;
        assert(leaf > 0 && (size_t)leaf < count_of(m.nodes) && seen[leaf] && m.nodes[leaf].slot == i);
    }
}

static unsigned pick(unsigned limit) {
    static uint32_t state = 42017;
    state = state * 1664525u + 1013904223u;
    return (state >> 12) % limit;
}

static void stress_reparenting(void) {
    w_model_init(&m);
    w_rect bounds = {0,0,400,250};
    for (int i = 0; i < window_limit; i++)
        assert(w_model_add(&m,i%3,i%workspace_limit,bounds).slot == i);
    assert(w_model_add(&m,0,0,bounds).slot < 0);
    for (int i = 0; i < 4096; i++) {
        int slot = pick(window_limit), peer = pick(window_limit), o = pick(3), ws = pick(workspace_limit);
        switch (pick(4)) {
        case 0: w_model_swap(&m,slot,peer); break;
        case 1: w_model_insert_tile(&m,slot,peer,pick(2) ? split_x : split_y, i%2); break;
        case 2: w_model_move(&m,slot,o,ws); break;
        case 3: {
            w_window_id old = {slot,m.windows[slot].generation};
            w_model_remove(&m,old);
            assert(w_model_add(&m,o,ws,bounds).slot == slot && !w_model_valid(&m,old));
            w_model_remove(&m,old);
            break;
        }
        }
        check_workspace_trees();
    }
}

static void crowded_master(w_rect area, int count, int gap) {
    w_model_init(&m);
    m.spaces[0][0].layout = layout_master;
    for (int i = 0; i < count; i++) {
        assert(w_model_add(&m,0,0,area).slot == i);
        m.windows[i].stack_weight = i == 1 ? 65536 : 1;
    }
    w_layout_compute_margins(&m,0,area,area,gap,0);
    for (int i = 0; i < count; i++) {
        w_rect r = m.windows[i].desired;
        assert(r.w > 0 && r.h > 0 && r.x >= area.x && r.y >= area.y);
        assert(r.x+r.w <= area.x+area.w && r.y+r.h <= area.y+area.h);
        int peer = m.windows[i].next_row;
        if (i != m.spaces[0][0].master_slot && peer >= 0 && area.h >= count-1) {
            w_rect next = m.windows[peer].desired;
            int bottom = r.y+r.h;
            assert(bottom <= next.y);
            w_split_grab grips[2];
            int seam = bottom+(next.y-bottom)/2;
            assert(w_layout_grab_splits(&m,0,i,r.x+r.w/2,seam,0,false,grips));
            assert(grips[1].kind == grab_row && grips[1].second.slot == peer);
        }
    }
}

int main(void) {
    assert(w_layout_output_for_bounds(screens, 0, (w_rect){-400,10,800,300}) == 0);
    assert(w_layout_output_for_bounds(screens, 1, (w_rect){-400,10,800,300}) == 1);
    assert(w_layout_output_for_bounds(screens, 0, (w_rect){-401,10,800,300}) == 1);
    assert(w_layout_output_for_bounds(screens, 1, (w_rect){800,-820,600,450}) == 2);
    assert(w_layout_output_for_bounds(screens, 0, (w_rect){700,-250,100,40}) == 2);
    w_rect unplugged[output_limit];
    memcpy(unplugged,screens,sizeof(screens));
    unplugged[1]=(w_rect){0};
    assert(w_layout_output_for_bounds(unplugged,1,(w_rect){-500,300,300,220}) == 0);
    memset(unplugged,0,sizeof(unplugged));
    assert(w_layout_output_for_bounds(unplugged,1,(w_rect){-500,300,300,220}) == -1);
    w_rect original = {-120,35,800,420};
    w_rect work = {-1600,-200,1600,900};
    w_rect natural = {-1400,-100,800,450};
    assert(w_rect_equal(w_layout_initial_float(natural,work), natural));
    assert(w_rect_equal(w_layout_initial_float(work,work), (w_rect){-1333,-50,1066,600}));
    w_rect larger = {100,40,2400,1350};
    w_rect destination = w_layout_relocate(natural,work,larger,96,144);
    assert(w_rect_equal(destination,(w_rect){400,190,1200,675}));
    assert(w_rect_equal(w_layout_relocate(destination,larger,work,144,96),natural));
    w_rect tiny = {-7,20,1,1};
    assert(w_rect_equal(w_layout_relocate(natural,work,tiny,96,96),tiny));
    w_rect scaled = w_layout_drag_position(original,(w_rect){0,0,1200,630},200,315,-900,-130);
    assert(scaled.x == -1200 && scaled.y == -603 && scaled.w == 1200 && scaled.h == 630);
    w_rect back = w_layout_drag_position(original,original,200,315,80,350);
    assert(w_rect_equal(back,original));
    for (int left=0; left<=1; left++) {
        for (int top=0; top<=1; top++) {
            w_rect r = w_layout_drag_corner(original, left ? 900 : -900, top ? 500 : -500,
                                             left, top, 200, 100, 1800, 1024);
            assert(r.w == 200 && r.h == 100);
            assert(left ? r.x+r.w == original.x+original.w : r.x == original.x);
            assert(top ? r.y+r.h == original.y+original.h : r.y == original.y);
        }
    }
    assert(w_layout_drag_corner(original, -2000, 2000, true, false, 1, 1, 1800, 1024).w == 1800);
    w_model_init(&m);
    int a = w_model_add(&m,0,0,original).slot;
    int b = w_model_add(&m,0,0,original).slot;
    w_model_focus(&m,b);
    m.spaces[0][0].next_axis = split_y;
    int c = w_model_add(&m,0,0,original).slot;
    m.selected[1] = 4;
    int other = w_model_add(&m,1,4,original).slot;
    measure(0);
    measure(1);
    w_rect tile = m.windows[c].desired, hint;
    int x = tile.x+tile.w/2, y = tile.y+tile.h/2;
    assert(w_layout_drop_edge(tile,x,y,&hint) == -1);
    assert(w_layout_drop_edge(tile,x,tile.y,&hint) == dir_up);
    assert(w_layout_drop_edge(tile,x,tile.y+tile.h-1,&hint) == dir_down);
    assert(w_layout_drop_edge(tile,tile.x,y,&hint) == dir_left);
    assert(w_layout_drop_edge(tile,tile.x+tile.w-1,y,&hint) == dir_right);
    w_split_grab grips[2];
    assert(w_layout_grab_splits(&m,0,c,x,y,5,true,grips));
    assert(w_layout_drag_split(&m,0,0,&grips[0],-75));
    assert(w_layout_drag_split(&m,0,0,&grips[1],40));
    measure(0);
    assert(m.windows[c].desired.x < tile.x && m.windows[c].desired.y > tile.y);
    assert(!w_layout_grab_splits(&m,0,c,x,y,1,false,grips));
    m.windows[b].minimized=true;
    measure(0);
    tile=m.windows[c].desired;
    assert(w_layout_grab_splits(&m,0,c,tile.x+10,tile.y+10,5,true,grips));
    assert(grips[0].kind == grab_node && grips[1].kind == grab_none);
    m.windows[a].floating=true;
    measure(0);
    assert(!w_layout_grab_splits(&m,0,c,tile.x+10,tile.y+10,5,true,grips));
    m.windows[b].minimized=false;
    m.windows[a].floating=false;
    m.spaces[0][0].layout = layout_master;
    measure(0);
    tile = m.windows[c].desired;
    assert(w_layout_grab_splits(&m,0,c,tile.x+10,tile.y+10,5,true,grips));
    assert(grips[0].kind == grab_column && grips[1].kind == grab_row);
    assert(w_layout_drag_split(&m,0,0,&grips[0],100));
    assert(w_layout_drag_split(&m,0,0,&grips[1],-90));
    m.spaces[0][0].layout = layout_tabs;
    assert(!w_layout_grab_splits(&m,0,c,x,y,5,true,grips));
    w_model_swap(&m,c,other);
    assert(m.windows[c].output == 1 && m.windows[c].workspace == 4 && m.windows[other].output == 0);
    w_model_insert_tile(&m,a,c,split_y,true);
    measure(1);
    assert(m.windows[a].desired.y < m.windows[c].desired.y);
    w_model_insert_tile(&m,c,a,split_x,true);
    measure(1);
    assert(m.windows[c].desired.x < m.windows[a].desired.x);
    assert(w_layout_grab_splits(&m,1,c,m.windows[c].desired.x+40,100,4,true,grips));
    w_model_remove(&m,(w_window_id){a,m.windows[a].generation});
    assert(!w_layout_drag_split(&m,1,4,&grips[0],50));
    w_model_move(&m,c,2,0);
    measure(2);
    assert(m.windows[c].desired.y < 0 && m.windows[c].desired.w == screens[2].w-16);
    crowded_master((w_rect){-320,-180,320,240},40,10);
    crowded_master((w_rect){-40,80,1,3},16,128);
    stress_reparenting();
    puts("Workspace trees stayed acyclic and retained every window through 4,096 mutations at full capacity.");
    puts("Three-output ownership, seams, monitor gaps, workspace membership, tile drops, split resizing, DPI grab offsets and floating size limits passed.");
}
