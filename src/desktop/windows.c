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
    w_rect previous_work[output_limit];
    for (int i = 0; i < output_limit; i++) {
        previous_work[i] = desk.outputs[i].work;
        desk.outputs[i].live = false;
    }
    int primary = -1;
    if (!EnumDisplayMonitors(NULL, NULL, w_visit_monitor, (LPARAM)&primary) || primary < 0)
        return;
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
        if (w->floating && !w_rect_equal(previous_work[from], desk.outputs[to].work))
            w_desktop_place_floating(i, w_layout_relocate(w->floating_bounds, previous_work[from], desk.outputs[to].work));
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
    w_model_remove(&desk.model, (w_window_id){i, desk.model.windows[i].generation});
    desk.windows[i] = (w_window_state){0};
}

static int w_discover(HWND w) {
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
    if (!w_os_remember(w))
        return -1;
    if (ws < 0)
        ws = desk.model.selected[output];
    w_window_id id = w_model_add(&desk.model, output, ws, bounds);
    if (id.slot < 0) {
        w_os_restore(w);
        return -1;
    }
    desk.windows[id.slot] = (w_window_state){
        .hwnd = w, .pid = pid, .custom_border = colored, .border = RGB(border >> 16, border >> 8, border)};
    desk.model.windows[id.slot].floating = floating;
    if (floating && initial_output != output)
        w_desktop_place_floating(id.slot, w_layout_relocate(bounds, desk.outputs[initial_output].work,
                                                        desk.outputs[output].work));
    if (layout >= 0)
        desk.model.spaces[output][ws].layout = (enum layout_kind)layout;
    return id.slot;
}

static void w_observe_visibility(int slot) {
    w_model_window *w = &desk.model.windows[slot];
    HWND hwnd = desk.windows[slot].hwnd;
    bool visible = IsWindowVisible(hwnd) != 0;
    bool shell_hidden = w_os_shell_hidden(hwnd);
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
    if (state->floating_update)
        return;
    w_rect actual = w_os_bounds(state->hwnd);
    // The hook delivers notifications asynchronously. A queued event may still
    // see the previous geometry while the owner's thread is applying our request.
    if (state->float_request_sent && w_rect_equal(actual, state->float_before_request))
        return;
    w_model_window *w = &desk.model.windows[slot];
    if (state->native_move == move_dragging)
        return;
    int output = w_output_for(state->hwnd);
    if (output != w->output && state->native_move != move_released && !w_in_native_move(state->hwnd)) {
        // Workspace navigation and display topology own output assignment.
        // A late native rescue move must not move a window off its assigned display.
        state->floating_update = float_move | float_size;
        w_desktop_schedule();
        return;
    }
    if (output != w->output) {
        w_model_move(&desk.model, slot, output, desk.model.selected[output]);
        state->native_move = move_idle;
        state->displaced_from = 0;
        if (desk.model.focus == slot)
            w_model_focus(&desk.model, slot);
    }
    state->float_request_sent = false;
    w->floating_bounds = actual;
    if (state->displaced_from) {
        w_rect area = desk.outputs[w->output].work, visible = actual;
        visible.x = w_clamp_int(actual.x, area.x, max(area.x, area.x + area.w - actual.w));
        visible.y = w_clamp_int(actual.y, area.y, max(area.y, area.y + area.h - actual.h));
        w_desktop_place_floating(slot, visible);
    }
}

static BOOL CALLBACK w_enumerate(HWND w, LPARAM unused) {
    (void)unused;
    int slot = w_discover(w);
    if (slot >= 0) {
        w_window_state *state = &desk.windows[slot];
        w_observe_visibility(slot);
        state->has_issued = false;
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

void w_desktop_scan(void) {
    EnumWindows(w_enumerate, 0);
    for (int i = 0; i < window_limit; i++)
        if (desk.model.windows[i].live && !IsWindow(desk.windows[i].hwnd))
            w_forget(i);
    owned_visibility_dirty = true;
    w_desktop_focus_observe(GetForegroundWindow());
    w_desktop_schedule();
}

void w_desktop_event(HWND w, DWORD event) {
    int i = w_desktop_find(w);
    if (!IsWindow(w)) {
        if (i >= 0) {
            w_forget(i);
            w_desktop_schedule();
        }
        return;
    }
    if (event == EVENT_SYSTEM_FOREGROUND)
        w = GetAncestor(w, GA_ROOTOWNER);
    i = w_discover(w);
    if (i < 0) {
        if (event == EVENT_SYSTEM_FOREGROUND) {
            w_desktop_focus_observe(w);
            w_desktop_schedule();
        }
        if ((event == EVENT_OBJECT_SHOW || event == EVENT_OBJECT_HIDE) && GetWindow(w, GW_OWNER) &&
            w_desktop_find(GetAncestor(w, GA_ROOTOWNER)) >= 0) {
            owned_visibility_dirty = true;
            w_desktop_schedule();
        }
        return;
    }
    w_model_window *mw = &desk.model.windows[i];
    w_window_state *state = &desk.windows[i];
    if (event == EVENT_OBJECT_SHOW || event == EVENT_OBJECT_HIDE) {
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
        state->displaced_from = 0;
        state->native_move = move_dragging;
        return;
    }
    if (event == EVENT_OBJECT_LOCATIONCHANGE) {
        if (mw->floating && !mw->fullscreen) {
            if (!IsIconic(w) && IsWindowVisible(w))
                w_observe_floating(i);
        }
        // Reconcile a user's finished move via MOVESIZEEND. An app may enforce a
        // minimum size, so repeating rejected placement on every echo would spin.
        else if (IsZoomed(w) && !state->restoring)
            w_desktop_schedule();
        else
            return;
    }
    if (event == EVENT_SYSTEM_MOVESIZEEND) {
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

static w_rect w_frame_bounds(HWND hwnd, w_rect desired) {
    RECT frame;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame)))) {
        w_rect actual = w_os_bounds(hwnd);
        int l = frame.left - actual.x, t = frame.top - actual.y;
        int r = actual.x + actual.w - frame.right, b = actual.y + actual.h - frame.bottom;
        if (abs(l) < 40 && abs(r) < 40 && abs(t) < 40 && abs(b) < 40) {
            desired.x -= l;
            desired.y -= t;
            desired.w += l + r;
            desired.h += t + b;
        }
    }
    return desired;
}

static BOOL CALLBACK w_sync_owned(HWND w, LPARAM unused) {
    (void)unused;
    if (!GetWindow(w, GW_OWNER))
        return TRUE;
    HWND owner = GetAncestor(w, GA_ROOTOWNER);
    if (!GetPropW(owner, L"winwm.managed"))
        return TRUE;
    int i = w_desktop_find(owner);
    if (i >= 0)
        w_os_set_visible(w, desk.windows[i].wants_visible && IsWindowVisible(owner));
    return TRUE;
}

void w_desktop_apply(void) {
    bool visibility_changed = false;
    HWND foreground = GetForegroundWindow();
    HWND active = foreground ? GetAncestor(foreground, GA_ROOTOWNER) : NULL;
    for (int o = 0; o < output_limit; o++)
        if (desk.outputs[o].live) {
            w_output_state *output = &desk.outputs[o];
            int ws = desk.model.selected[o];
            int recent = w_model_recent(&desk.model, o, ws);
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
                if (visible && desk.model.spaces[o][ws].layout == layout_tabs && recent >= 0 &&
                    !w->floating && i != recent && !desk.model.windows[recent].floating)
                    visible = false;
                s->wants_visible = visible;
                if (w->minimized || w->withdrawn)
                    continue;
                visibility_changed |= w_os_set_visible(hwnd, visible);
                if (!visible)
                    continue;
                bool focused = hwnd == active;
                DWORD color =
                    focused ? RGB(desk.config.accent >> 16, desk.config.accent >> 8, desk.config.accent)
                    : s->custom_border ? s->border
                                       : RGB(desk.config.inactive_border >> 16,
                                             desk.config.inactive_border >> 8, desk.config.inactive_border);
                if (!s->decorated || focused != s->active || color != s->applied_border ||
                    s->alpha != desk.config.inactive_alpha) {
                    w_os_window_appearance(hwnd, focused, color, desk.config.inactive_alpha);
                    s->alpha = desk.config.inactive_alpha;
                    s->active = focused;
                    s->applied_border = color;
                    s->decorated = true;
                }
                if (w->floating && !w->fullscreen) {
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
                    if (!s->restoring) {
                        s->restoring = ShowWindowAsync(hwnd, SW_SHOWNOACTIVATE) != 0;
                    }
                    s->has_issued = false;
                    continue;
                }
                s->restoring = false;
                if (s->has_issued && w_rect_equal(s->issued, w->desired))
                    continue;
                w_rect target = w_frame_bounds(hwnd, w->desired);
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
                    w_rect r = w_frame_bounds(s->hwnd, w->desired);
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
}
