#include "desktop/desktop.h"

////////////////
//~ Focus

enum { focus_retry_timer = 33, focus_retry_ms = 120 };

// Workspace visibility and native foreground ownership settle independently.
// Carry the requested destination across native visibility and foreground changes.
static struct w_pending_focus {
    w_window_id window;
    uint64_t pointer_presses;
    bool automatic, empty, retried;
    HWND foreground;
    UINT_PTR timer;
} pending;

////////////////
//~ Focus Functions

void w_desktop_focus_cancel(void) {
    if (pending.timer) KillTimer(desk.root, pending.timer);
    pending.timer = 0;
    pending.retried = false;
    pending.empty = false;
    pending.window.generation = 0;
}

bool w_desktop_focus_pending(void) {
    if (pending.pointer_presses != w_input_pointer_press_count()) return false;
    if (pending.empty) return true;
    if (!w_model_valid(&desk.model, pending.window)) return false;
    HWND owner = desk.windows[pending.window.slot].hwnd;
    // A stalled application's queued show may never complete. It must yield
    // to the next foreground event or pointer movement on another application.
    if (!IsWindow(owner) || (!w_os_ready_for_focus(owner) && IsHungAppWindow(owner))) {
        w_desktop_focus_cancel();
        return false;
    }
    return true;
}

static void w_request_focus(int slot, bool automatic) {
    w_desktop_focus_cancel();
    w_model_focus(&desk.model, slot);
    pending.window = (w_window_id){slot, desk.model.windows[slot].generation};
    pending.pointer_presses = w_input_pointer_press_count();
    pending.automatic = automatic;
    pending.foreground = GetForegroundWindow();
}

void w_desktop_focus_successor(void) {
    w_desktop_model *m = &desk.model;
    int slot = w_model_recent(m, m->output, m->selected[m->output]);
    w_desktop_focus_cancel();
    if (slot >= 0) w_request_focus(slot, true);
    else m->focus = -1;
    w_desktop_schedule();
}

static HWND w_desktop_surface(void) {
    if (desk.root && IsWindowVisible(desk.root))
        return desk.root;
    HWND shell = GetShellWindow();
    return shell ? shell : GetDesktopWindow();
}

static bool w_can_activate(HWND window) {
    return window && IsWindowVisible(window) && IsWindowEnabled(window) &&
           !(GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_NOACTIVATE);
}

// Native focus can change between hit testing, target selection and the
// foreground app handling SC_HOTKEY. Respect a handoff at either boundary.
static bool w_foreground(HWND previous, HWND target) {
    HWND current = GetForegroundWindow();
    if (previous && current == previous && current != target && !w_os_same_process(previous) &&
        !IsHungAppWindow(previous)) {
        // The foreground app can transfer activation directly. A rejected
        // SetForegroundWindow call makes Windows flash the target's taskbar button.
        DWORD_PTR result;
        SendMessageTimeoutW(previous, WM_SYSCOMMAND, SC_HOTKEY, (LPARAM)target,
                            SMTO_ABORTIFHUNG | SMTO_BLOCK, 75, &result);
        current = GetForegroundWindow();
    }
    if (current == target)
        return true;
    if (current != previous) {
        HWND owner = w_os_app_window(target);
        return owner && w_os_app_window(current) == owner;
    }
    return SetForegroundWindow(target) != 0;
}

static HWND w_activation_target(HWND owner, HWND foreground) {
    // WinWM selects a managed window. The app owns activation among its
    // top-level surfaces and keyboard focus among their child controls.
    if ((foreground == owner || w_os_app_window(foreground) == owner) && w_can_activate(foreground))
        return foreground;
    HWND popup = GetLastActivePopup(owner);
    if (w_os_app_window(popup) == owner && w_can_activate(popup))
        return popup;
    if (w_can_activate(owner))
        return owner;
    popup = GetWindow(owner, GW_ENABLEDPOPUP);
    return w_os_app_window(popup) == owner && w_can_activate(popup) ? popup : NULL;
}

static void w_guard_withdrawal(int output, int slot, uint64_t intent) {
    if (slot < 0 || desk.model.windows[slot].output != output || !desk.windows[slot].wants_visible) return;
    desk.windows[slot].withdrawing_foreground = true;
    desk.windows[slot].withdrawal_intent = intent;
}

void w_desktop_record_selection(int output) {
    w_output_state *o = &desk.outputs[output];
    o->selection_tick = GetTickCount64();
    o->selection_foreground = GetForegroundWindow();
    o->selection_foreground_visible = IsWindowVisible(o->selection_foreground) != FALSE;
    uint64_t intent = w_desktop_activation_intent();
    w_guard_withdrawal(output,w_desktop_find(w_os_app_window(o->selection_foreground)),intent);
    // Foreground changes cross UI threads. The destination of our last
    // successful activation can still be in flight when another key arrives.
    if (w_model_valid(&desk.model,o->last_activation))
        w_guard_withdrawal(output,o->last_activation.slot,intent);
    o->last_activation.generation = 0;
}

static void w_request_empty(void) {
    w_desktop_focus_cancel();
    desk.model.focus = -1;
    pending.empty = true;
    pending.automatic = false;
    pending.pointer_presses = w_input_pointer_press_count();
    pending.foreground = GetForegroundWindow();
    w_desktop_focus_apply();
    w_desktop_schedule();
}

void w_desktop_focus_empty(void) {
    w_desktop_record_selection(desk.model.output);
    w_request_empty();
}

void w_desktop_reclaim_focus(void) {
    w_desktop_model *m = &desk.model;
    int slot = w_model_recent(m,m->output,m->selected[m->output]);
    if (slot < 0) {
        w_request_empty();
        return;
    }
    w_request_focus(slot,false);
    w_desktop_focus_apply();
    w_desktop_schedule();
}

static void w_select_owner(HWND owner) {
    int current = desk.model.focus;
    // Windows can give the desktop temporary foreground while hiding or
    // destroying a window. Keep the workspace's keyboard selection.
    if (w_os_desktop_surface(owner))
        return;
    if (current >= 0 && desk.windows[current].hwnd == owner && w_model_visible(&desk.model, current) &&
        desk.model.output == desk.model.windows[current].output)
        return;
    int slot = w_desktop_find(owner);
    if (slot < 0) {
        if (desk.model.focus >= 0) {
            desk.model.focus = -1;
            w_desktop_schedule();
        }
    } else if (w_model_visible(&desk.model, slot) && w_os_ready_for_focus(owner)) {
        w_model_focus(&desk.model, slot);
        w_desktop_schedule();
    }
}

static bool w_thread_owns_pointer(DWORD thread_id) {
    GUITHREADINFO info = {.cbSize = sizeof(info)};
    return GetGUIThreadInfo(thread_id, &info) &&
           (info.hwndCapture ||
            (info.flags & (GUI_INMENUMODE | GUI_POPUPMENUMODE | GUI_SYSTEMMENUMODE | GUI_INMOVESIZE)));
}

bool w_desktop_focus_new(w_window_id id) {
    if ((w_desktop_focus_pending() && !pending.automatic) || !w_model_visible(&desk.model, id.slot) ||
        w_palette_shown() || w_notice_visible() || w_bar_menu_active() || w_desktop_popup_process() ||
        w_thread_owns_pointer(0) || w_thread_owns_pointer(GetCurrentThreadId()))
        return false;
    HWND owner = desk.windows[id.slot].hwnd;
    HWND foreground = GetForegroundWindow();
    if (w_os_shell_dialog(foreground) || !w_activation_target(owner, foreground))
        return false;
    // Make the new tab visible in the next layout pass. The same pass applies
    // foreground activation after asynchronous workspace and popup visibility.
    w_request_focus(id.slot, true);
    return true;
}

////////////////
//~ Foreground tracking

// The broker can show a placeholder before XAML supplies the chooser's
// native size. Place the real surface on the invoking output when it settles.
void w_desktop_place_chooser(HWND window, DWORD event) {
    static HWND placed, assisted;
    static w_rect previous, requested;
    if (event == EVENT_OBJECT_HIDE || event == EVENT_OBJECT_DESTROY) {
        if (window == placed) placed = NULL;
        if (window == assisted) assisted = NULL;
        return;
    }
    if (event != EVENT_OBJECT_SHOW && event != EVENT_SYSTEM_FOREGROUND && event != EVENT_OBJECT_UNCLOAKED &&
        event != EVENT_OBJECT_LOCATIONCHANGE) return;
    // WinEvents are queued. By dispatch time this HWND may only be the
    // previous foreground window or a background window being created.
    if (window != GetForegroundWindow() || !w_os_shell_dialog(window) || !w_can_activate(window) ||
        IsIconic(window) || w_os_cloaked(window)) return;
    w_rect bounds = w_os_bounds(window);
    if (bounds.w <= 1 || bounds.h <= 1) return;
    int owner = w_desktop_find(w_os_app_window(GetWindow(window, GW_OWNER)));
    int output = owner >= 0 ? desk.model.windows[owner].output : desk.model.output;
    if ((unsigned)output >= output_limit || !desk.outputs[output].live) return;
    w_rect work = desk.outputs[output].work;
    if (work.w <= 0 || work.h <= 0) return;
    if (window != assisted && bounds.h < MulDiv(160, desk.outputs[output].dpi, 96)) {
        wchar_t cls[32];
        if (GetClassNameW(window, cls, count_of(cls)) && !wcscmp(cls, L"Open With") && !GetShellWindow() &&
            w_desktop_show_chooser_list(window)) assisted = window;
    }
    if (placed == window && requested.w == bounds.w && requested.h == bounds.h &&
        bounds.x >= work.x && bounds.x <= work.x + max(0, work.w - bounds.w) &&
        bounds.y >= work.y && bounds.y <= work.y + max(0, work.h - bounds.h)) {
        previous = bounds;
        return;
    }
    w_rect target = bounds;
    target.x = work.x + max(0, (work.w - target.w) / 2);
    target.y = work.y + max(0, (work.h - target.h) / 2);
    if (!w_rect_equal(target, bounds)) {
        if (event == EVENT_OBJECT_LOCATIONCHANGE && placed == window && w_rect_equal(previous, bounds) &&
            w_rect_equal(requested, target)) return;
        if (!SetWindowPos(window, NULL, target.x, target.y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE |
                          SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS)) return;
    }
    placed = window;
    previous = bounds;
    requested = target;
}

typedef struct w_float_order {
    HWND tile;
    RECT bounds;
    HWND windows[window_limit];
    int count;
    bool below_tile, fullscreen;
} w_float_order;

static BOOL CALLBACK w_collect_covered_floats(HWND hwnd, LPARAM data) {
    w_float_order *order = (w_float_order *)data;
    if (hwnd == order->tile)
        order->below_tile = true;
    else if (order->below_tile != order->fullscreen && GetPropW(hwnd, L"winwm.managed")) {
        int slot = w_desktop_find(hwnd);
        if (slot >= 0 && desk.model.windows[slot].floating && !desk.model.windows[slot].fullscreen &&
            desk.windows[slot].wants_visible && w_model_visible(&desk.model, slot) && IsWindowVisible(hwnd) &&
            !(GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST)) {
            RECT area, overlap;
            if (GetWindowRect(hwnd, &area) && IntersectRect(&overlap, &order->bounds, &area))
                order->windows[order->count++] = hwnd;
        }
    }
    return TRUE;
}

void w_desktop_stack_floats(int slot, bool fullscreen_changed) {
    if ((unsigned)slot >= window_limit || slot != desk.model.focus || !w_model_visible(&desk.model,slot))
        return;
    const w_model_window *target = &desk.model.windows[slot];
    HWND tile = desk.windows[slot].hwnd, foreground = GetForegroundWindow();
    if ((target->floating && !target->fullscreen) || !IsWindowEnabled(tile) ||
        (foreground && foreground != tile) || (GetWindowLongPtrW(tile, GWL_EXSTYLE) & WS_EX_TOPMOST))
        return;
    for (int i = 0; i < window_limit; i++) {
        if (!desk.model.windows[i].floating || !w_model_visible(&desk.model, i))
            continue;
        w_float_order order = {.tile=tile, .fullscreen=target->fullscreen};
        if (order.fullscreen)
            order.bounds = w_os_to_rect(desk.outputs[target->output].screen);
        else if (!GetWindowRect(tile, &order.bounds))
            return;
        if (!EnumWindows(w_collect_covered_floats, (LPARAM)&order) ||
            !order.count || foreground != GetForegroundWindow())
            return;
        UINT flags = SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS;
        if (order.fullscreen || fullscreen_changed) {
            // One request to the tile's input queue keeps the peers in native order
            // even when a floating app's input queue is stalled.
            HWND anchor = order.fullscreen ? GetWindow(order.windows[0], GW_HWNDPREV)
                                           : order.windows[order.count-1];
            if (anchor && (GetWindowLongPtrW(anchor,GWL_EXSTYLE) & WS_EX_TOPMOST))
                anchor = HWND_TOP;
            SetWindowPos(tile,anchor,0,0,0,0,flags);
            return;
        }
        HWND above = GetWindow(tile, GW_HWNDPREV);
        if (above && (GetWindowLongPtrW(above, GWL_EXSTYLE) & WS_EX_TOPMOST))
            above = HWND_TOP;
        for (int j = 0; j < order.count; j++) {
            HWND hwnd = order.windows[j];
            if (SetWindowPos(hwnd, above, 0, 0, 0, 0, flags))
                above = hwnd;
        }
        break;
    }
}

void w_desktop_focus_observe(HWND window) {
    HWND actual = w_os_app_window(GetForegroundWindow());
    if (actual != w_os_app_window(window))
        return;
    if (pending.empty || w_model_valid(&desk.model, pending.window)) {
        // Hiding a workspace can briefly activate another application while
        // its replacement is still becoming visible. A new click supersedes it.
        if (w_desktop_focus_pending()) return;
        w_desktop_focus_cancel();
    }
    w_select_owner(actual);
    w_desktop_stack_floats(w_desktop_find(actual),false);
}

void w_desktop_activate(int slot) {
    if ((unsigned)slot >= window_limit || !desk.model.windows[slot].live)
        return;
    w_model_window *w = &desk.model.windows[slot];
    w_desktop_record_selection(w->output);
    desk.model.output = w->output;
    w_model_workspace(&desk.model, w->workspace);
    if (w->scratch)
        w->scratch_visible = true;
    if (w->minimized) {
        if (!ShowWindowAsync(desk.windows[slot].hwnd, SW_RESTORE))
            return;
        w->minimized = false;
    }
    w_request_focus(slot, false);
    // A visible selection can take input immediately. A workspace or tab
    // transition completes after the single queued visibility/layout pass.
    if (desk.windows[slot].wants_visible && w_os_ready_for_focus(desk.windows[slot].hwnd))
        w_desktop_focus_apply();
    w_desktop_schedule();
}

bool w_desktop_close_target(HWND window, DWORD pid, w_window_id owner) {
    DWORD current = 0;
    GetWindowThreadProcessId(window,&current);
    if (!pid || current != pid || !w_model_valid(&desk.model,owner) ||
        !w_model_visible(&desk.model,owner.slot) || !w_can_activate(window) || IsIconic(window) ||
        w_os_cloaked(window) || w_os_app_window(window) != desk.windows[owner.slot].hwnd)
        return false;
    w_desktop_focus_cancel();
    w_select_owner(desk.windows[owner.slot].hwnd);
    return PostMessageW(window, WM_CLOSE, 0, 0) != 0;
}

static bool w_tool_panel(HWND window, DWORD pid) {
    if (!pid || !w_can_activate(window) || IsIconic(window) || w_os_cloaked(window))
        return false;
    bool dialog = GetClassLongPtrW(window, GCW_ATOM) == (ULONG_PTR)WC_DIALOG;
    if (pid == GetCurrentProcessId())
        return dialog;
    HWND owner = GetAncestor(window, GA_ROOTOWNER);
    if (!dialog && !((GetWindowLongPtrW(window, GWL_EXSTYLE) | GetWindowLongPtrW(owner, GWL_EXSTYLE)) & WS_EX_TOOLWINDOW))
        return false;
    wchar_t self[os_path_cap], image[os_path_cap];
    DWORD length = count_of(image);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return false;
    bool matches = QueryFullProcessImageNameW(process, 0, image, &length) &&
                   w_os_self(self, count_of(self)) && !_wcsicmp(image, self);
    CloseHandle(process);
    return matches;
}

bool w_desktop_close_foreground(void) {
    if (w_notice_close(true)) return true;
    HWND window = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(window,&pid);
    if (w_tool_panel(window, pid))
        return GetForegroundWindow() == window && PostMessageW(window, WM_CLOSE, 0, 0);
    if (w_bar_menu_close(NULL, true) || w_palette_close(true))
        return true;
    if (w_desktop_popup_process()) {
        w_desktop_popup_cancel();
        return true;
    }
    int slot = w_desktop_find(w_os_app_window(window));
    if (slot < 0)
        return false;
    return w_desktop_close_target(window,pid,(w_window_id){slot,desk.model.windows[slot].generation});
}

static BOOL CALLBACK w_await_owned_popup(HWND window, LPARAM owner) {
    return !(w_os_app_window(window) == (HWND)owner && window != (HWND)owner && IsWindowEnabled(window) &&
             w_os_shell_hidden(window));
}

static void CALLBACK w_focus_retry(HWND hwnd, UINT msg, UINT_PTR id, DWORD time) {
    (void)msg;
    (void)time;
    if (pending.timer != id) return;
    KillTimer(hwnd, id);
    pending.timer = 0;
    w_desktop_focus_apply();
}

static void w_apply_foreground(HWND foreground, HWND target) {
    if (pending.timer) return;
    bool activated = w_foreground(foreground, target);
    if (activated && !pending.empty && w_model_valid(&desk.model,pending.window))
        desk.outputs[desk.model.windows[pending.window.slot].output].last_activation = pending.window;
    if (!activated && foreground == GetForegroundWindow() && !pending.automatic && !pending.retried &&
        desk.root && (pending.timer = SetTimer(desk.root, focus_retry_timer, focus_retry_ms, w_focus_retry))) {
        // Windows can temporarily deny activation as it hides the outgoing app.
        // Give the explicit command one more attempt, without polling the layout.
        pending.retried = true;
        pending.foreground = foreground;
        return;
    }
    bool reconcile = !activated && !pending.empty;
    w_desktop_focus_cancel();
    if (reconcile) w_select_owner(w_os_app_window(GetForegroundWindow()));
}

void w_desktop_focus_apply(void) {
    if (!pending.empty && !w_model_valid(&desk.model, pending.window))
        return;
    HWND foreground = GetForegroundWindow();
    if (pending.empty) {
        if (desk.model.focus >= 0 || w_palette_shown() || w_notice_visible() || w_bar_menu_active() ||
            w_desktop_popup_process() || pending.pointer_presses != w_input_pointer_press_count() ||
            (IsWindowVisible(foreground) && w_desktop_find(w_os_app_window(foreground)) < 0) ||
            (pending.retried && foreground != pending.foreground && IsWindowVisible(foreground) &&
             !w_os_desktop_surface(foreground)))
            w_desktop_focus_cancel();
        else
            w_apply_foreground(foreground, w_desktop_surface());
        return;
    }
    int slot = pending.window.slot;
    HWND owner = desk.windows[slot].hwnd;
    // Pending automatic placement must yield when the user or application
    // has already selected a different foreground surface, including via
    // the keyboard. A temporary desktop activation can occur during layout.
    bool handoff = (pending.automatic || pending.retried) && foreground != pending.foreground && !w_os_desktop_surface(foreground) &&
                   w_os_app_window(foreground) != owner;
    if (!w_model_visible(&desk.model, slot) || w_palette_shown() || w_notice_visible() || w_bar_menu_active() ||
        pending.pointer_presses != w_input_pointer_press_count() ||
        handoff || (pending.automatic && w_os_shell_dialog(foreground))) {
        w_desktop_focus_cancel();
        w_select_owner(w_os_app_window(GetForegroundWindow()));
        return;
    }
    if (w_desktop_popup_process() || !w_os_ready_for_focus(owner)) {
        if (!IsWindow(owner) || IsHungAppWindow(owner)) w_desktop_focus_cancel();
        return;
    }
    HWND target = w_activation_target(owner, foreground);
    if (!target) {
        // Only a shell-hidden owned dialog has a show operation to await. An
        // independently disabled owner cannot become an activation target.
        if (!EnumWindows(w_await_owned_popup, (LPARAM)owner))
            return;
        w_desktop_focus_cancel();
        w_select_owner(w_os_app_window(GetForegroundWindow()));
        return;
    }
    w_apply_foreground(foreground, target);
}

////////////////
//~ Pointer focus

// Some toolkits use an unowned tool window or put their popups under a
// private helper HWND. There is no document to select from that ownership
// chain. Leave the application's active tool alone on passive mouse motion.
static bool w_active_app_tool(HWND foreground, HWND app, DWORD target_pid) {
    if (!foreground || w_desktop_find(app) >= 0)
        return false;
    if (foreground == app && !(GetWindowLongPtrW(foreground, GWL_EXSTYLE) & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)))
        return false;
    DWORD pid = 0;
    return GetWindowThreadProcessId(foreground, &pid) && pid == target_pid;
}

void w_desktop_hover(POINT point) {
    static w_window_id denied;
    static HWND denied_by;
    if (!desk.config.follow_mouse || w_desktop_focus_pending() || w_palette_shown() || w_notice_visible() ||
        w_bar_menu_active() || w_desktop_popup_process() ||
        GetAsyncKeyState(VK_LBUTTON) < 0 || GetAsyncKeyState(VK_RBUTTON) < 0 ||
        GetAsyncKeyState(VK_MBUTTON) < 0 || GetAsyncKeyState(VK_XBUTTON1) < 0 || GetAsyncKeyState(VK_XBUTTON2) < 0)
        return;
    HWND hit = GetAncestor(WindowFromPoint(point), GA_ROOT);
    HWND foreground = GetForegroundWindow();
    HWND owner = w_os_app_window(hit);
    int slot = w_desktop_find(owner);
    if (slot < 0 || !desk.windows[slot].wants_visible || !w_model_visible(&desk.model, slot) ||
        (GetWindowLongPtrW(hit, GWL_EXSTYLE) & WS_EX_NOACTIVATE)) {
        denied.generation = 0;
        return;
    }
    HWND active = foreground == hit || foreground == owner ? owner : w_os_app_window(foreground);
    if (active == owner) {
        denied.generation = 0;
        w_select_owner(owner);
        return;
    }
    if (denied.slot == slot && w_model_valid(&desk.model, denied) && denied_by == foreground)
        return;
    denied.generation = 0;
    if (w_os_shell_dialog(foreground) || w_active_app_tool(foreground, active, desk.windows[slot].pid))
        return;
    // Foreign menus, modal move/size loops and drags own the pointer until
    // their thread releases it. GetCapture() only sees our own input queue.
    if (w_thread_owns_pointer(0) || w_thread_owns_pointer(GetCurrentThreadId()))
        return;
    HWND target = w_activation_target(owner, foreground);
    if (!target)
        return;
    if (!w_foreground(foreground, target)) {
        denied = (w_window_id){slot, desk.model.windows[slot].generation};
        denied_by = foreground;
        return;
    }
    w_desktop_focus_cancel();
    w_select_owner(owner);
}
