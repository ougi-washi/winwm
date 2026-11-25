#include "desktop/desktop.h"
#include "providers/audio.h"
#include <dwmapi.h>
#include <shellscalingapi.h>
#include <stdio.h>
#include <wchar.h>

////////////////
//~ Windows

static bool owned_visibility_dirty;

////////////////
//~ Windows Functions

int w_desktop_find(HWND w) {
    for (int i = 0; i < window_limit; i++)
        if (desk.windows[i].hwnd == w && desk.model.windows[i].live)
            return i;
    return -1;
}

static int w_output_for(HWND w) {
    HMONITOR mon = MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST);
    for (int i = 0; i < output_limit; i++)
        if (desk.outputs[i].live && desk.outputs[i].monitor == mon)
            return i;
    return desk.model.output;
}

int w_desktop_output_for_bounds(int current, w_rect r) {
    w_rect screens[output_limit] = {0};
    for (int i = 0; i < output_limit; i++)
        if (desk.outputs[i].live)
            screens[i] = desk.outputs[i].screen;
    int o = w_layout_output_for_bounds(screens, current, r);
    return o >= 0 ? o : current;
}

static bool w_custom_text(const char *name, wchar_t *s, int cap, char *action, size_t acap) {
    if (strncmp(name, "custom/", 7))
        return w_desktop_status_text(name, s, cap, action, acap);
    name += 7;
    w_provider_value v;
    if (!w_providers_value(name, &v, action, acap))
        return false;
    wcsncpy(s, v.text, cap - 1);
    s[cap - 1] = 0;
    return true;
}

static bool w_module_detail(const char *name, wchar_t *text, int cap) {
    if (strncmp(name, "custom/", 7))
        return w_desktop_status_detail(name, text, cap);
    w_provider_value v;
    char action[128];
    if (!w_providers_value(name + 7, &v, action, sizeof(action)) || !v.tooltip[0]) return false;
    lstrcpynW(text, v.tooltip, cap);
    return true;
}

static void w_dock_bar(int output) {
    w_output_state *o = &desk.outputs[output];
    o->work = w_bar_position(&o->bar, o->screen, o->dpi, o->bar_hidden);
    w_os_area_apply(o->monitor, o->work);
    InvalidateRect(o->bar.hwnd, NULL, FALSE);
    w_desktop_schedule();
}

static BOOL CALLBACK w_visit_monitor(HMONITOR mon, HDC dc, LPRECT bounds, LPARAM arg) {
    (void)dc;
    (void)bounds;
    MONITORINFOEXW mi = {.cbSize = sizeof(mi)};
    if (!GetMonitorInfoW(mon, (MONITORINFO *)&mi))
        return TRUE;
    int slot = -1;
    for (int i = 0; i < output_limit; i++) {
        if (!wcscmp(desk.outputs[i].device, mi.szDevice)) {
            slot = i;
            break;
        }
        if (slot < 0 && !desk.outputs[i].device[0])
            slot = i;
    }
    if (slot < 0)
        return TRUE;
    w_output_state *o = &desk.outputs[slot];
    o->live = true;
    if (mi.dwFlags & MONITORINFOF_PRIMARY)
        *(int *)arg = slot;
    o->monitor = mon;
    wcscpy(o->device, mi.szDevice);
    RECT r = mi.rcMonitor;
    o->screen = (w_rect){r.left, r.top, r.right - r.left, r.bottom - r.top};
    UINT x = 96, y = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &x, &y);
    o->dpi = (int)x;
    if (!o->bar.hwnd)
        w_bar_create(&o->bar, slot, &desk.config, w_desktop_action);
    w_audio_state audio = w_audio_read();
    w_bar_set_audio(&o->bar, audio.available ? audio.percent : -1, audio.muted);
    o->bar.custom = w_custom_text;
    o->bar.detail = w_module_detail;
    o->bar.dock = w_dock_bar;
    o->bar_hidden = desk.hide_bars;
    o->work = w_bar_position(&o->bar, o->screen, o->dpi, desk.hide_bars);
    w_os_area_apply(mon, o->work);
    return TRUE;
}

void w_desktop_place_floating(int slot, w_rect bounds) {
    w_model_window *w = &desk.model.windows[slot];
    w_rect old = w->floating_bounds;
    if (w_rect_equal(old, bounds))
        return;
    w_window_state *state = &desk.windows[slot];
    state->native_move = move_idle;
    if (bounds.x != old.x || bounds.y != old.y)
        state->floating_update |= float_move;
    if (bounds.w != old.w || bounds.h != old.h)
        state->floating_update |= float_size;
    w->floating_bounds = bounds;
    w_desktop_schedule();
}

////////////////
//~ Display topology

void w_desktop_outputs(void) {
    w_desktop_cancel_drag();
    w_rect previous_work[output_limit];
    int previous_dpi[output_limit];
    bool previously_live[output_limit];
    for (int i = 0; i < output_limit; i++) {
        previous_work[i] = desk.outputs[i].work;
        previous_dpi[i] = desk.outputs[i].dpi;
        previously_live[i] = desk.outputs[i].live;
        desk.outputs[i].live = false;
    }
    int primary = -1;
    if (!EnumDisplayMonitors(NULL, NULL, w_visit_monitor, (LPARAM)&primary) || primary < 0) {
        for (int i = 0; i < output_limit; i++)
            desk.outputs[i].live = previously_live[i];
        return;
    }
    for (int i = 0; i < output_limit; i++) {
        w_output_state *output = &desk.outputs[i];
        if (!output->live && output->bar.hwnd)
            ShowWindow(output->bar.hwnd, SW_HIDE);
    }
    for (int i = 0; i < window_limit; i++) {
        w_model_window *w = &desk.model.windows[i];
        if (!w->live)
            continue;
        w_window_state *state = &desk.windows[i];
        state->native_move = move_idle;
        state->motion.start = 0;
        state->has_issued = false;
        int from = w->output, to = from;
        int home = state->displaced_from - 1;
        if (home >= 0 && desk.outputs[home].live) {
            to = home;
            state->displaced_from = 0;
        } else if (!desk.outputs[from].live) {
            to = primary;
            if (home < 0)
                state->displaced_from = from + 1;
        }
        if (w->floating && (!w_rect_equal(previous_work[from], desk.outputs[to].work) ||
                            previous_dpi[from] != desk.outputs[to].dpi))
            w_desktop_place_floating(i, w_layout_relocate(w->floating_bounds, previous_work[from],
                                      desk.outputs[to].work, previous_dpi[from], desk.outputs[to].dpi));
        if (from != to)
            w_model_move(&desk.model, i, to, w->workspace);
    }
    w_desktop_model *m = &desk.model;
    if (!desk.outputs[m->output].live)
        m->output = primary;
    if (m->focus >= 0) {
        if (w_model_visible(m, m->focus))
            m->output = m->windows[m->focus].output;
        else
            m->focus = w_model_recent(m, m->output, m->selected[m->output]);
    }
    w_desktop_schedule();
}

////////////////
//~ Window matching

static bool w_matches(const char *needle, const wchar_t *s, bool exact) {
    if (!*needle)
        return true;
    wchar_t wide[256];
    return w_os_wide(needle, wide, count_of(wide)) && (exact ? !wcscmp(s, wide) : wcsstr(s, wide) != NULL);
}

static void w_forget(int i) {
    w_effects_hide(i);
    w_model_remove(&desk.model, (w_window_id){i, desk.model.windows[i].generation});
    desk.windows[i] = (w_window_state){0};
    // Discovery and placement can both observe teardown before its destroy
    // notification arrives. Recompute even when the current layout pass has
    // already measured this slot.
    w_desktop_schedule();
}

static int w_discover(HWND w, bool *focus_new) {
    int i = w_desktop_find(w);
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (i >= 0) {
        if (pid == desk.windows[i].pid && GetPropW(w, L"winwm.managed"))
            return i;
        w_forget(i);
    }
    if (!w_os_candidate(w) || IsIconic(w))
        return -1;
    int initial_output = w_output_for(w), output = initial_output, ws = -1;
    bool floating = false, colored = false;
    uint32_t border = 0;
    int layout = -1;
    if (desk.config.rule_count) {
        wchar_t cls[128] = {0}, title[256] = {0}, path[1024] = {0};
        GetClassNameW(w, cls, count_of(cls));
        GetWindowTextW(w, title, count_of(title));
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (p) {
            DWORD n = count_of(path);
            QueryFullProcessImageNameW(p, 0, path, &n);
            CloseHandle(p);
        }
        for (int j = 0; j < desk.config.rule_count; j++) {
            const w_window_rule *r = &desk.config.rules[j];
            if (!w_matches(r->class_name, cls, r->exact_match) || !w_matches(r->title, title, r->exact_match) ||
                !w_matches(r->executable, path, false))
                continue;
            if (r->exclude)
                return -1;
            if (r->workspace >= 0)
                ws = r->workspace;
            if (r->output >= 0 && desk.outputs[r->output].live)
                output = r->output;
            if (r->layout >= 0)
                layout = r->layout;
            if (r->colored) {
                colored = true;
                border = r->border;
            }
            if (r->sets_floating)
                floating = r->floating;
        }
    }
    // Leave borderless fullscreen clients on their native presentation path.
    LONG_PTR style = GetWindowLongPtrW(w, GWL_STYLE);
    w_rect bounds = w_os_bounds(w);
    if (!(style & (WS_CAPTION | WS_THICKFRAME)) && w_rect_equal(bounds, desk.outputs[initial_output].screen))
        return -1;
    w_rect original;
    if (!w_os_remember(w, &original))
        return -1;
    if (ws < 0)
        ws = desk.model.selected[output];
    w_window_id id = w_model_add(&desk.model, output, ws, bounds);
    if (id.slot < 0) {
        w_os_restore(w);
        return -1;
    }
    int saved_output = w_desktop_output_for_bounds(initial_output, original);
    w_output_state *saved = &desk.outputs[saved_output];
    desk.windows[id.slot] = (w_window_state){.hwnd = w, .pid = pid, .custom_border = colored,
        .border = RGB(border >> 16, border >> 8, border), .saved_float_work = saved->work, .saved_float_dpi = saved->dpi,
        .direct_surface = w_os_direct_surface(w)};
    w_model_window *window = &desk.model.windows[id.slot];
    window->floating = floating;
    if (!floating)
        window->floating_bounds = w_layout_initial_float(original, saved->work);
    else if (IsZoomed(w))
        window->floating_bounds = original;
    if (floating && initial_output != output)
        w_desktop_place_floating(id.slot, w_layout_relocate(window->floating_bounds, desk.outputs[initial_output].work,
                                 desk.outputs[output].work, desk.outputs[initial_output].dpi, desk.outputs[output].dpi));
    if (layout >= 0)
        desk.model.spaces[output][ws].layout = (enum layout_kind)layout;
    if (*focus_new && w_desktop_focus_new(id))
        *focus_new = false;
    // A late size or position change can be the first eligible event.
    w_desktop_schedule();
    return id.slot;
}

static void w_observe_visibility(int slot) {
    w_model_window *w = &desk.model.windows[slot];
    HWND hwnd = desk.windows[slot].hwnd;
    bool visible = IsWindowVisible(hwnd) != 0;
    bool shell_hidden = w_os_shell_hidden(hwnd);
    w->cloaked = w_os_cloaked(hwnd);
    if (visible || !shell_hidden)
        w->minimized = IsIconic(hwnd) != 0;
    if (visible)
        w->withdrawn = false;
    else if (!shell_hidden)
        w->withdrawn = true;
}

static bool w_in_native_move(HWND window) {
    GUITHREADINFO info = {.cbSize = sizeof(info)};
    DWORD thread = GetWindowThreadProcessId(window, NULL);
    return thread && GetGUIThreadInfo(thread, &info) && (info.flags & GUI_INMOVESIZE) &&
           info.hwndMoveSize == window;
}

static void w_observe_floating(int slot) {
    w_window_state *state = &desk.windows[slot];
    if (w_desktop_dragging_float(slot)) {
        w_desktop_drag_client_resized(slot);
        return;
    }
    if (state->floating_update || IsZoomed(state->hwnd))
        return;
    w_rect actual = w_os_bounds(state->hwnd);
    // The hook delivers notifications asynchronously. A queued event may still
    // see the previous geometry while the owner's thread is applying our request.
    if (state->float_request_sent && w_rect_equal(actual, state->float_before_request))
        return;
    w_model_window *w = &desk.model.windows[slot];
    if (state->native_move == move_dragging)
        return;
    int output = w_desktop_output_for_bounds(w->output, actual);
    if (output != w->output && state->native_move != move_released && !w_in_native_move(state->hwnd)) {
        // Workspace navigation and display topology own output assignment.
        // A late native rescue move must not move a window off its assigned display.
        state->floating_update = float_move | float_size;
        w_desktop_schedule();
        return;
    }
    if (output != w->output) {
        w_model_move(&desk.model, slot, output, desk.model.selected[output]);
        state->displaced_from = 0;
        if (desk.model.focus == slot)
            w_model_focus(&desk.model, slot);
    }
    state->float_request_sent = false;
    state->native_move = move_idle;
    w->floating_bounds = actual;
    if (state->displaced_from) {
        w_rect area = desk.outputs[w->output].work, visible = actual;
        visible.x = w_clamp_int(actual.x, area.x, max(area.x, area.x + area.w - actual.w));
        visible.y = w_clamp_int(actual.y, area.y, max(area.y, area.y + area.h - actual.h));
        w_desktop_place_floating(slot, visible);
    }
}

void w_desktop_set_floating(int slot, bool floating) {
    w_model_window *w = &desk.model.windows[slot];
    if (w->floating == floating)
        return;
    w_window_state *state = &desk.windows[slot];
    w_output_state *out = &desk.outputs[w->output];
    if (floating) {
        if (!w_rect_equal(state->saved_float_work, out->work) || state->saved_float_dpi != out->dpi)
            w->floating_bounds = w_layout_relocate(w->floating_bounds, state->saved_float_work, out->work,
                                                   state->saved_float_dpi, out->dpi);
    } else {
        // Native resize notifications can still be queued when the shortcut arrives.
        if (!w->fullscreen && !IsIconic(state->hwnd) && IsWindowVisible(state->hwnd))
            w_observe_floating(slot);
        out = &desk.outputs[w->output];
        state->saved_float_work = out->work;
        state->saved_float_dpi = out->dpi;
    }
    state->motion.start = 0;
    state->native_move = move_idle;
    state->has_issued = false;
    state->float_request_sent = false;
    state->floating_update = floating ? float_move | float_size : 0;
    w->floating = floating;
    w_desktop_schedule();
}

static BOOL CALLBACK w_enumerate(HWND w, LPARAM focus_new) {
    int slot = w_discover(w, (bool *)focus_new);
    if (slot >= 0) {
        w_window_state *state = &desk.windows[slot];
        w_observe_visibility(slot);
        if (state->has_issued && !state->motion.start && state->native_move != move_dragging) {
            w_rect actual = w_os_bounds(w);
            if (!w_rect_equal(actual, state->issued_native) && !w_rect_equal(actual, state->rejected_bounds))
                state->has_issued = false;
        }
        if (!IsZoomed(w))
            state->restoring = false;
        if (desk.model.windows[slot].floating && !desk.model.windows[slot].fullscreen &&
            IsWindowVisible(w) && !IsIconic(w))
            w_observe_floating(slot);
    }
    return TRUE;
}

////////////////
//~ Window discovery

void w_desktop_scan(bool focus_new) {
    // Enumeration is in z-order. When recovering missed events, give at most
    // one newly eligible window the same activation as a normal first map.
    EnumWindows(w_enumerate, (LPARAM)&focus_new);
    for (int i = 0; i < window_limit; i++)
        if (desk.model.windows[i].live && !IsWindow(desk.windows[i].hwnd))
            w_forget(i);
    owned_visibility_dirty = true;
    w_desktop_focus_observe(GetForegroundWindow());
    w_desktop_schedule();
}

void w_desktop_event(HWND w, DWORD event) {
    int i = w_desktop_find(w);
    if (event == EVENT_OBJECT_DESTROY || !IsWindow(w)) {
        if (i >= 0)
            w_forget(i);
        return;
    }
    if (event == EVENT_SYSTEM_FOREGROUND)
        w = w_os_app_window(w);
    bool focus_new = event != EVENT_OBJECT_HIDE && event != EVENT_OBJECT_CLOAKED &&
                     event != EVENT_SYSTEM_MINIMIZESTART;
    i = w_discover(w, &focus_new);
    if (i < 0) {
        if (event == EVENT_SYSTEM_FOREGROUND) {
            w_desktop_focus_observe(w);
            w_desktop_schedule();
        }
        if ((event == EVENT_OBJECT_SHOW || event == EVENT_OBJECT_HIDE) && GetWindow(w, GW_OWNER) &&
            w_desktop_find(w_os_app_window(w)) >= 0) {
            owned_visibility_dirty = true;
            w_desktop_schedule();
        }
        return;
    }
    w_model_window *mw = &desk.model.windows[i];
    w_window_state *state = &desk.windows[i];
    if (event == EVENT_OBJECT_SHOW || event == EVENT_OBJECT_HIDE ||
        event == EVENT_OBJECT_CLOAKED || event == EVENT_OBJECT_UNCLOAKED) {
        owned_visibility_dirty = true;
        w_observe_visibility(i);
    } else if (event == EVENT_SYSTEM_MINIMIZESTART || event == EVENT_SYSTEM_MINIMIZEEND ||
               event == EVENT_SYSTEM_FOREGROUND)
        w_observe_visibility(i);
    if (state->restoring && !IsZoomed(w)) {
        state->restoring = false;
        state->has_issued = false;
        w_desktop_schedule();
    }
    if (event == EVENT_SYSTEM_FOREGROUND)
        w_desktop_focus_observe(w);
    if (event == EVENT_SYSTEM_MOVESIZESTART) {
        state->motion.start = 0;
        state->displaced_from = 0;
        state->native_move = move_dragging;
        state->drag_start = w_os_bounds(w);
        return;
    }
    if (event == EVENT_OBJECT_LOCATIONCHANGE) {
        w_outline_position(i);
        if (state->motion.start && !IsZoomed(w))
            return;
        if (mw->floating && !mw->fullscreen) {
            if (!IsIconic(w) && IsWindowVisible(w))
                w_observe_floating(i);
        }
        else if (IsZoomed(w) && !state->restoring)
            w_desktop_schedule();
        else if (state->has_issued && state->native_move != move_dragging && w_model_visible(&desk.model, i)) {
            w_rect actual = w_os_bounds(w);
            if (w_rect_equal(actual, state->issued_native))
                state->rejected_bounds = (w_rect){0};
            else if (!w_rect_equal(actual, state->rejected_bounds)) {
                // Reconcile an application's own startup placement, but don't
                // fight a minimum size that rejects the same request twice.
                state->rejected_bounds = actual;
                state->has_issued = false;
                w_desktop_schedule();
            }
            return;
        } else
            return;
    }
    if (event == EVENT_SYSTEM_MOVESIZEEND) {
        if (state->native_move == move_dragging && !mw->floating && !mw->fullscreen && w_model_visible(&desk.model, i)) {
            w_rect start = state->drag_start, end = w_os_bounds(w);
            POINT point;
            if (start.w == end.w && start.h == end.h && (start.x != end.x || start.y != end.y) &&
                GetCursorPos(&point))
                w_desktop_drop_tile(i, point);
        }
        state->native_move = move_released;
        state->has_issued = false;
        if (mw->floating && !mw->fullscreen) {
            state->float_request_sent = false;
            state->floating_update = 0;
            w_observe_floating(i);
        }
    }
    w_desktop_schedule();
}

////////////////
//~ Window placement

static w_rect w_frame_bounds(w_window_state *s, w_rect desired) {
    HWND hwnd = s->hwnd;
    UINT dpi = GetDpiForWindow(hwnd);
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE) & (WS_CAPTION | WS_THICKFRAME | WS_BORDER | WS_DLGFRAME);
    LONG_PTR exstyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & (WS_EX_CLIENTEDGE | WS_EX_WINDOWEDGE | WS_EX_TOOLWINDOW);
    // DWM and USER may publish different frames mid-restore or mid-animation.
    // The invisible resize margins depend on the frame style and monitor DPI.
    if (!s->frame_dpi || dpi != s->frame_dpi || style != s->frame_style || exstyle != s->frame_exstyle) {
        RECT frame;
        if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame)))) {
            w_rect actual = w_os_bounds(hwnd);
            RECT m = {frame.left - actual.x, frame.top - actual.y,
                      actual.x + actual.w - frame.right, actual.y + actual.h - frame.bottom};
            if (m.left >= 0 && m.top >= 0 && m.right >= 0 && m.bottom >= 0 &&
                m.left < 64 && m.right < 64 && m.top < 64 && m.bottom < 64) {
                s->frame_margins = m;
                s->frame_dpi = dpi;
                s->frame_style = style;
                s->frame_exstyle = exstyle;
            }
        }
    }
    RECT m = s->frame_margins;
    desired.x -= m.left;
    desired.y -= m.top;
    desired.w += m.left + m.right;
    desired.h += m.top + m.bottom;
    return desired;
}

static BOOL CALLBACK w_sync_owned(HWND w, LPARAM unused) {
    (void)unused;
    if (!GetWindow(w, GW_OWNER))
        return TRUE;
    HWND owner = w_os_app_window(w);
    if (owner == w || !GetPropW(owner, L"winwm.managed"))
        return TRUE;
    int i = w_desktop_find(owner);
    if (i >= 0)
        w_os_set_visible(w, desk.windows[i].wants_visible && IsWindowVisible(owner));
    return TRUE;
}

void w_desktop_apply(void) {
    static int previous_workspace[output_limit];
    bool visibility_changed = false;
    HWND foreground = GetForegroundWindow();
    HWND active = w_os_app_window(foreground);
    for (int o = 0; o < output_limit; o++)
        if (desk.outputs[o].live) {
            w_output_state *output = &desk.outputs[o];
            int ws = desk.model.selected[o];
            int direction = ws < previous_workspace[o] ? -1 : 1;
            previous_workspace[o] = ws;
            int recent = w_model_recent(&desk.model, o, ws);
            int tab = recent;
            if (desk.model.spaces[o][ws].layout == layout_tabs && recent >= 0 && desk.model.windows[recent].floating) {
                tab = -1;
                for (int i = 0; i < window_limit; i++) {
                    const w_model_window *w = &desk.model.windows[i];
                    if (w->output == o && !w->floating && w_model_visible(&desk.model, i) &&
                        (tab < 0 || w->focus_order > desk.model.windows[tab].focus_order))
                        tab = i;
                }
            }
            bool hide = desk.hide_bars || (recent >= 0 && desk.model.windows[recent].fullscreen);
            if (hide != output->bar_hidden) {
                output->work = w_bar_position(&output->bar, output->screen, output->dpi, hide);
                w_os_area_apply(output->monitor, output->work);
                output->bar_hidden = hide;
            }
            w_layout_compute_margins(&desk.model, o, output->work, output->screen,
                                   MulDiv(desk.config.gap, output->dpi, 96),
                                   MulDiv(desk.config.outer_gap, output->dpi, 96));
            w_bar_view view = {.selected = ws, .resize_mode = desk.resize, .inhibit = desk.inhibit};
            if (recent >= 0)
                GetWindowTextW(desk.windows[recent].hwnd, view.title, count_of(view.title));
            HDWP batch = NULL;
            bool batch_failed = false;
            int changed[window_limit], n = 0;
            for (int i = 0; i < window_limit; i++) {
                w_model_window *w = &desk.model.windows[i];
                w_window_state *s = &desk.windows[i];
                if (!w->live || w->output != o)
                    continue;
                HWND hwnd = s->hwnd;
                if (!GetPropW(hwnd, L"winwm.managed")) {
                    w_forget(i);
                    continue;
                }
                view.occupied |= 1 << w->workspace;
                bool visible = w_model_visible(&desk.model, i);
                if (visible && desk.model.spaces[o][ws].layout == layout_tabs && !w->floating && i != tab)
                    visible = false;
                bool entering = !s->wants_visible;
                s->wants_visible = visible;
                if (w->minimized || w->withdrawn || w->cloaked) {
                    w_effects_hide(i);
                    s->has_issued = false;
                    continue;
                }
                visibility_changed |= w_os_set_visible(hwnd, visible);
                if (!visible) {
                    w_effects_hide(i);
                    s->has_issued = false;
                    continue;
                }
                bool focused = hwnd == active;
                DWORD color =
                    focused ? RGB(desk.config.accent >> 16, desk.config.accent >> 8, desk.config.accent)
                    : s->custom_border ? s->border
                                       : RGB(desk.config.inactive_border >> 16,
                                             desk.config.inactive_border >> 8, desk.config.inactive_border);
                bool framed = desk.config.border_size > 0 && !desk.borders_hidden && !w->fullscreen &&
                              !(w->floating && IsZoomed(hwnd));
                if (!framed)
                    w_outline_hide(i);
                if (!framed || w_outline_sync(i, focused))
                    color = 0xfffffffeu; // DWMWA_COLOR_NONE
                BYTE corner = w->fullscreen || !desk.config.rounding ? 1 : desk.config.rounding <= 4 ? 3 : 2;
                w_effects_opacity(i,focused,entering || !s->presented);
                if (!s->decorated || corner != s->corner || focused != s->active || color != s->applied_border ||
                    s->alpha != desk.config.inactive_alpha) {
                    w_os_window_appearance(hwnd, color, corner);
                    s->corner = corner;
                    s->alpha = desk.config.inactive_alpha;
                    s->active = focused;
                    s->applied_border = color;
                    s->decorated = true;
                }
                if (w->floating && !w->fullscreen) {
                    s->motion.start = 0;
                    s->presented = true;
                    if (s->floating_update) {
                        if (IsZoomed(hwnd)) {
                            if (!s->restoring)
                                s->restoring = ShowWindowAsync(hwnd, SW_SHOWNOACTIVATE) != 0;
                            continue;
                        }
                        UINT flags = SWP_ASYNCWINDOWPOS | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
                        if (!(s->floating_update & float_move))
                            flags |= SWP_NOMOVE;
                        if (!(s->floating_update & float_size))
                            flags |= SWP_NOSIZE;
                        s->floating_update = 0;
                        s->float_before_request = w_os_bounds(hwnd);
                        s->float_request_sent =
                            SetWindowPos(hwnd, NULL, w->desired.x, w->desired.y, w->desired.w, w->desired.h, flags) != 0;
                        if (!s->float_request_sent)
                            w->floating_bounds = w_os_bounds(hwnd);
                    }
                    s->has_issued = false;
                    continue;
                }
                if (IsZoomed(hwnd)) {
                    s->motion.start = 0;
                    if (!s->restoring) {
                        s->restoring = ShowWindowAsync(hwnd, SW_SHOWNOACTIVATE) != 0;
                    }
                    s->has_issued = false;
                    continue;
                }
                s->restoring = false;
                if (s->native_move == move_dragging || (s->has_issued && w_rect_equal(s->issued, w->desired)))
                    continue;
                w_rect inner = w->desired;
                if (framed) {
                    int edge = MulDiv(desk.config.border_size, output->dpi, 96);
                    int x = min(edge, (inner.w - 1)/2), y = min(edge, (inner.h - 1)/2);
                    inner.x += x;
                    inner.y += y;
                    inner.w -= 2*x;
                    inner.h -= 2*y;
                }
                w_rect target = w_frame_bounds(s, inner);
                bool moved = !w_rect_equal(s->issued, w->desired);
                if (moved)
                    s->rejected_bounds = (w_rect){0};
                s->issued_native = target;
                enum motion_kind kind = !s->presented ? motion_open : entering ? motion_workspace : motion_move;
                s->presented = true;
                if (!w->fullscreen && !w_desktop_resizing() && (moved || entering) &&
                    w_effects_move(i, target, kind, direction)) {
                    s->issued = w->desired;
                    s->has_issued = true;
                    continue;
                }
                s->motion.start = 0;
                // A recovered event or refreshed configuration can invalidate
                // the layout cache while the actual placement is already right.
                if (w_rect_equal(w_os_bounds(hwnd), target)) {
                    s->issued = w->desired;
                    s->has_issued = true;
                    continue;
                }
                if (!batch && !batch_failed) {
                    batch = BeginDeferWindowPos(8);
                    batch_failed = !batch;
                }
                if (batch) {
                    batch = DeferWindowPos(batch, hwnd, NULL, target.x, target.y, target.w, target.h,
                                           SWP_ASYNCWINDOWPOS | SWP_NOZORDER | SWP_NOACTIVATE |
                                               SWP_NOOWNERZORDER);
                    if (!batch)
                        batch_failed = true;
                }
                changed[n++] = i;
            }
            if (batch && !EndDeferWindowPos(batch))
                batch_failed = true;
            for (int j = 0; j < n; j++) {
                int i = changed[j];
                w_window_state *s = &desk.windows[i];
                w_model_window *w = &desk.model.windows[i];
                bool ok = !batch_failed;
                if (!ok) {
                    w_rect r = s->issued_native;
                    ok = SetWindowPos(s->hwnd, NULL, r.x, r.y, r.w, r.h,
                                      SWP_ASYNCWINDOWPOS | SWP_NOZORDER | SWP_NOACTIVATE) != 0;
                }
                s->issued = w->desired;
                s->has_issued = ok;
            }
            w_bar_update(&output->bar, &view);
        }
    if (visibility_changed || owned_visibility_dirty) {
        owned_visibility_dirty = false;
        EnumWindows(w_sync_owned, 0);
    }
    w_desktop_focus_apply();
    w_desktop_drag_preview();
}
