#include "desktop/desktop.h"

////////////////
//~ Focus

// Workspace visibility and native foreground ownership settle independently.
// Hold an explicit request only while waiting for its HWND or modal popup to show.
static struct w_pending_focus {
    w_window_id window;
    uint64_t pointer_presses;
} pending;

////////////////
//~ Focus Functions

void w_desktop_focus_cancel(void) {
    pending.window.generation = 0;
}

bool w_desktop_focus_pending(void) {
    return w_model_valid(&desk.model, pending.window) &&
           pending.pointer_presses == w_input_pointer_press_count();
}

static void w_request_focus(int slot) {
    w_model_focus(&desk.model, slot);
    pending.window = (w_window_id){slot, desk.model.windows[slot].generation};
    pending.pointer_presses = w_input_pointer_press_count();
}

void w_desktop_focus_successor(void) {
    w_desktop_model *m = &desk.model;
    int slot = w_model_recent(m, m->output, m->selected[m->output]);
    w_desktop_focus_cancel();
    if (slot >= 0) w_request_focus(slot);
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

static bool w_foreground(HWND target) {
    HWND previous = GetForegroundWindow();
    if (previous == target)
        return true;
    if (previous && !w_os_same_process(previous) && !IsHungAppWindow(previous)) {
        // The foreground app can transfer activation directly. A rejected
        // SetForegroundWindow call makes Windows flash the target's taskbar button.
        DWORD_PTR result;
        SendMessageTimeoutW(previous, WM_SYSCOMMAND, SC_HOTKEY, (LPARAM)target,
                            SMTO_ABORTIFHUNG | SMTO_BLOCK, 75, &result);
        HWND current = GetForegroundWindow();
        if (current == target)
            return true;
        if (current != previous)
            return w_os_app_window(current) == w_os_app_window(target);
    }
    return SetForegroundWindow(target) != 0;
}

static HWND w_activation_target(HWND owner, HWND hit) {
    if (hit && w_os_app_window(hit) == owner && w_can_activate(hit))
        return hit;
    HWND popup = GetLastActivePopup(owner);
    if (w_os_app_window(popup) == owner && w_can_activate(popup))
        return popup;
    if (w_can_activate(owner))
        return owner;
    popup = GetWindow(owner, GW_ENABLEDPOPUP);
    return w_os_app_window(popup) == owner && w_can_activate(popup) ? popup : NULL;
}

static bool w_release_app(HWND foreground, HWND surface) {
    return w_desktop_find(w_os_app_window(foreground)) < 0 || w_foreground(surface);
}

void w_desktop_focus_empty(void) {
    w_desktop_focus_cancel();
    if (!w_palette_shown() && !w_bar_menu_active() && !w_desktop_popup_process())
        w_release_app(GetForegroundWindow(), w_desktop_surface());
    desk.model.focus = -1;
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
    if (!w_model_visible(&desk.model, id.slot) || w_palette_shown() || w_bar_menu_active() || w_desktop_popup_process() ||
        w_thread_owns_pointer(0) || w_thread_owns_pointer(GetCurrentThreadId()))
        return false;
    HWND owner = desk.windows[id.slot].hwnd;
    if (!w_activation_target(owner, NULL))
        return false;
    // Make the new tab visible in the next layout pass. The same pass applies
    // foreground activation after asynchronous workspace and popup visibility.
    w_request_focus(id.slot);
    return true;
}

////////////////
//~ Foreground tracking

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
    if (w_model_valid(&desk.model, pending.window)) {
        // Hiding a workspace can briefly activate another application while
        // its replacement is still becoming visible. A new click supersedes it.
        if (pending.pointer_presses == w_input_pointer_press_count())
            return;
        w_desktop_focus_cancel();
    }
    w_select_owner(actual);
    w_desktop_stack_floats(w_desktop_find(actual),false);
}

void w_desktop_activate(int slot) {
    if ((unsigned)slot >= window_limit || !desk.model.windows[slot].live)
        return;
    w_model_window *w = &desk.model.windows[slot];
    desk.model.output = w->output;
    w_model_workspace(&desk.model, w->workspace);
    if (w->scratch)
        w->scratch_visible = true;
    if (w->minimized) {
        if (!ShowWindowAsync(desk.windows[slot].hwnd, SW_RESTORE))
            return;
        w->minimized = false;
    }
    w_request_focus(slot);
    w_desktop_apply();
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
    HWND window = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(window,&pid);
    if (w_tool_panel(window, pid))
        return GetForegroundWindow() == window && PostMessageW(window, WM_CLOSE, 0, 0);
    if (w_bar_menu_close(NULL, true) || w_palette_close())
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

void w_desktop_focus_apply(void) {
    if (!w_model_valid(&desk.model, pending.window))
        return;
    int slot = pending.window.slot;
    if (!w_model_visible(&desk.model, slot) || w_palette_shown() || w_bar_menu_active() ||
        pending.pointer_presses != w_input_pointer_press_count()) {
        w_desktop_focus_cancel();
        w_select_owner(w_os_app_window(GetForegroundWindow()));
        return;
    }
    HWND owner = desk.windows[slot].hwnd;
    if (w_desktop_popup_process() || !w_os_ready_for_focus(owner))
        return;
    HWND target = w_activation_target(owner, NULL);
    if (!target) {
        // Only a shell-hidden owned dialog has a show operation to await. An
        // independently disabled owner cannot become an activation target.
        if (!EnumWindows(w_await_owned_popup, (LPARAM)owner))
            return;
        w_desktop_focus_cancel();
        w_select_owner(w_os_app_window(GetForegroundWindow()));
        return;
    }
    w_desktop_focus_cancel();
    w_foreground(target);
}

////////////////
//~ Pointer focus

void w_desktop_hover(POINT point) {
    static HWND last_hit, denied_by;
    static uint32_t last_generation;
    static bool denied;
    if (!desk.config.follow_mouse || w_desktop_focus_pending() || w_palette_shown() ||
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
        last_hit = NULL;
        return;
    }
    uint32_t generation = desk.model.windows[slot].generation;
    bool same_hit = hit == last_hit && generation == last_generation;
    if (same_hit && denied && denied_by == foreground)
        return;
    if (same_hit && !denied && foreground == hit) {
        w_select_owner(owner);
        return;
    }
    // Foreign menus, modal move/size loops and drags own the pointer until
    // their thread releases it. GetCapture() only sees our own input queue.
    if (w_thread_owns_pointer(0) || w_thread_owns_pointer(GetCurrentThreadId()))
        return;
    last_hit = hit;
    last_generation = generation;
    denied = false;
    HWND target = w_activation_target(owner, hit);
    if (!target)
        return;
    if (foreground != target && !w_foreground(target)) {
        denied = true;
        denied_by = foreground;
        return;
    }
    w_desktop_focus_cancel();
    w_select_owner(owner);
}
