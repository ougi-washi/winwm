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

static HWND w_root_owner(HWND window) {
    return window ? GetAncestor(window, GA_ROOTOWNER) : NULL;
}

void w_desktop_focus_empty(void) {
    w_desktop_focus_cancel();
    HWND bar = desk.outputs[desk.model.output].bar.hwnd;
    if (!w_palette_shown() && !w_bar_menu_active() && w_desktop_find(w_root_owner(GetForegroundWindow())) >= 0)
        SetForegroundWindow(IsWindowVisible(bar) ? bar : GetDesktopWindow());
    desk.model.focus = -1;
    w_desktop_schedule();
}

static bool w_can_activate(HWND window) {
    return window && IsWindowVisible(window) && IsWindowEnabled(window) &&
           !(GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_NOACTIVATE);
}

static HWND w_activation_target(HWND owner, HWND hit) {
    if (hit && w_root_owner(hit) == owner && w_can_activate(hit))
        return hit;
    HWND popup = GetLastActivePopup(owner);
    if (w_can_activate(popup))
        return popup;
    if (w_can_activate(owner))
        return owner;
    popup = GetWindow(owner, GW_ENABLEDPOPUP);
    return w_can_activate(popup) ? popup : NULL;
}

static void w_select_owner(HWND owner) {
    int current = desk.model.focus;
    if (current >= 0 && desk.windows[current].hwnd == owner && w_model_visible(&desk.model, current) &&
        desk.model.output == desk.model.windows[current].output)
        return;
    int slot = w_desktop_find(owner);
    if (slot < 0 || !w_model_visible(&desk.model, slot)) {
        if (desk.model.focus >= 0) {
            desk.model.focus = -1;
            w_desktop_schedule();
        }
    } else if (desk.model.focus != slot || desk.model.output != desk.model.windows[slot].output) {
        w_model_focus(&desk.model, slot);
        w_desktop_schedule();
    }
}

////////////////
//~ Foreground tracking

void w_desktop_focus_observe(HWND window) {
    HWND actual = w_root_owner(GetForegroundWindow());
    if (actual != w_root_owner(window))
        return;
    if (w_model_valid(&desk.model, pending.window)) {
        // Hiding a workspace can briefly activate another application while
        // its replacement is still becoming visible. A new click supersedes it.
        if (pending.pointer_presses == w_input_pointer_press_count())
            return;
        w_desktop_focus_cancel();
    }
    w_select_owner(actual);
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
    w_model_focus(&desk.model, slot);
    pending.window = (w_window_id){slot, w->generation};
    pending.pointer_presses = w_input_pointer_press_count();
    w_desktop_apply();
}

bool w_desktop_close_foreground(void) {
    HWND window = GetForegroundWindow();
    int slot = w_desktop_find(w_root_owner(window));
    if (slot < 0 || !w_model_visible(&desk.model, slot) || !w_can_activate(window))
        return false;
    w_desktop_focus_cancel();
    w_select_owner(desk.windows[slot].hwnd);
    return PostMessageW(window, WM_CLOSE, 0, 0) != 0;
}

static BOOL CALLBACK w_await_owned_popup(HWND window, LPARAM owner) {
    return !(w_root_owner(window) == (HWND)owner && window != (HWND)owner && IsWindowEnabled(window) &&
             w_os_shell_hidden(window));
}

void w_desktop_focus_apply(void) {
    if (!w_model_valid(&desk.model, pending.window))
        return;
    int slot = pending.window.slot;
    if (!w_model_visible(&desk.model, slot) || w_palette_shown() || w_bar_menu_active() ||
        pending.pointer_presses != w_input_pointer_press_count()) {
        w_desktop_focus_cancel();
        w_select_owner(w_root_owner(GetForegroundWindow()));
        return;
    }
    HWND owner = desk.windows[slot].hwnd;
    if (!IsWindowVisible(owner) || IsIconic(owner))
        return;
    HWND target = w_activation_target(owner, NULL);
    if (!target) {
        // Only a shell-hidden owned dialog has a show operation to await. An
        // independently disabled owner cannot become an activation target.
        if (!EnumWindows(w_await_owned_popup, (LPARAM)owner))
            return;
        w_desktop_focus_cancel();
        w_select_owner(w_root_owner(GetForegroundWindow()));
        return;
    }
    w_desktop_focus_cancel();
    if (GetForegroundWindow() != target && !SetForegroundWindow(target))
        w_select_owner(w_root_owner(GetForegroundWindow()));
}

////////////////
//~ Pointer focus

static bool w_thread_owns_pointer(DWORD thread_id) {
    GUITHREADINFO info = {.cbSize = sizeof(info)};
    return GetGUIThreadInfo(thread_id, &info) &&
           (info.hwndCapture ||
            (info.flags & (GUI_INMENUMODE | GUI_POPUPMENUMODE | GUI_SYSTEMMENUMODE | GUI_INMOVESIZE)));
}

void w_desktop_hover(POINT point) {
    if (!desk.config.follow_mouse || w_palette_shown() || w_bar_menu_active() ||
        GetAsyncKeyState(VK_LBUTTON) < 0 || GetAsyncKeyState(VK_RBUTTON) < 0 ||
        GetAsyncKeyState(VK_MBUTTON) < 0)
        return;
    HWND hit = GetAncestor(WindowFromPoint(point), GA_ROOT);
    HWND owner = w_root_owner(hit);
    if (!owner)
        return;
    HWND foreground = GetForegroundWindow();
    if (w_root_owner(foreground) == owner) {
        if (!w_model_valid(&desk.model, pending.window))
            w_select_owner(owner);
        return;
    }
    // Foreign menus, modal move/size loops and drags own the pointer until
    // their thread releases it. GetCapture() only sees our own input queue.
    if (w_thread_owns_pointer(0) || w_thread_owns_pointer(GetCurrentThreadId()))
        return;
    int slot = w_desktop_find(owner);
    if (slot < 0 || !w_model_visible(&desk.model, slot))
        return;
    HWND target = w_activation_target(owner, hit);
    if (target && SetForegroundWindow(target)) {
        w_desktop_focus_cancel();
        w_select_owner(owner);
    }
}
