#include "input/input.h"

////////////////
//~ Input

static HWND target;
static UINT action_msg, hover_msg, context_msg;
static bool desktop_context_press;
static POINT hover_point;
static bool hover_pending, hover_known;
// Mouse hook and desktop dispatch both run on the session thread.
static uint64_t pointer_presses;

////////////////
//~ Input Functions

uint64_t w_input_pointer_press_count(void) {
    return pointer_presses;
}

void w_input_hover_point(POINT *out) {
    *out = hover_point;
    hover_pending = false;
}

typedef struct w_chord_match {
    unsigned short key, next_key;
    unsigned char mods, next_mods;
    bool modal, release, repeat;
} w_chord_match;

static w_chord_match bindings[binding_limit];
static bool bound[0x600];
static int binding_count;
static HHOOK keyboard, mouse;
static HANDLE key_thread;
static DWORD key_thread_id;
static SRWLOCK matcher_lock = SRWLOCK_INIT;
static bool modal, held[256];
static int on_release[256];
static unsigned prefix_key, prefix_mods;
static DWORD prefix_time, generation;
static bool super_down[2], mapped_super[2];
// Use the WM_HOTKEY foreground grant when opening Run from another process.
// Binding dispatch always follows the matched physical press, including very
// short taps where a queued notification may outlive its registration.
enum { key_dispatch = WM_APP + 1 };

static int launcher_binding = -1, registered_launcher = -1;
static DWORD launcher_generation;
static void w_launcher_hotkey(bool enable) {
    if (!enable && registered_launcher >= 0) {
        UnregisterHotKey(NULL, 1);
        registered_launcher = -1;
    }
    if (enable && registered_launcher < 0 && launcher_binding >= 0) {
        w_chord_match *b = &bindings[launcher_binding];
        if (RegisterHotKey(NULL, 1, (b->mods & ~MOD_WIN) | MOD_NOREPEAT, b->key)) {
            registered_launcher = launcher_binding;
            launcher_generation = generation;
        }
    }
}

static input_drag_callback drag;
static UINT drag_button;

static unsigned w_modifiers(void) {
    unsigned mods = 0;
    if (GetAsyncKeyState(VK_MENU) & 0x8000)
        mods |= MOD_ALT;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000)
        mods |= MOD_CONTROL;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000)
        mods |= MOD_SHIFT;
    if (super_down[0] || super_down[1])
        mods |= MOD_WIN;
    return mods;
}

static bool w_match_key(unsigned key, unsigned vk, unsigned scan) {
    return key == vk || (scan && key == 0x400 + scan);
}

static int w_match(unsigned vk, unsigned scan, unsigned mods, DWORD time, bool repeat, int *release_id) {
    if (prefix_key && time - prefix_time > 800)
        prefix_key = 0;
    unsigned candidate = 0;
    int found = -1;
    for (int i = 0; i < binding_count; i++) {
        const w_chord_match *b = &bindings[i];
        if (b->modal && !modal)
            continue;
        if (prefix_key && b->next_key && b->key == prefix_key && b->mods == prefix_mods &&
            b->next_mods == mods && w_match_key(b->next_key, vk, scan)) {
            prefix_key = 0;
            return i;
        }
        if (b->mods != mods || !w_match_key(b->key, vk, scan))
            continue;
        if (b->next_key)
            candidate = b->key;
        else if (b->release)
            *release_id = i;
        else if (!repeat || b->repeat)
            found = i;
    }
    if (candidate && !repeat) {
        prefix_key = candidate;
        prefix_mods = mods;
        prefix_time = time;
        return -2;
    }
    prefix_key = 0;
    return found;
}

static bool w_consume_key(WPARAM event, const KBDLLHOOKSTRUCT *key) {
    unsigned vk = key->vkCode;
    if (vk >= 256 || (key->flags & LLKHF_INJECTED))
        return false;
    bool up = event == WM_KEYUP || event == WM_SYSKEYUP;
    if (vk == VK_LWIN || vk == VK_RWIN || (mapped_super[0] && vk == VK_F13) ||
        (mapped_super[1] && vk == VK_F14)) {
        super_down[vk == VK_RWIN || vk == VK_F14] = !up;
        w_launcher_hotkey(super_down[0] || super_down[1]);
        return true;
    }
    if (vk >= VK_LSHIFT && vk <= VK_RMENU)
        return false;
    if (up) {
        if (held[vk]) {
            if (on_release[vk] > 0)
                PostMessageW(target, action_msg, on_release[vk] - 1, generation);
            held[vk] = false;
            on_release[vk] = 0;
            return true;
        }
    } else {
        unsigned scan = key->scanCode + ((key->flags & LLKHF_EXTENDED) ? 256 : 0);
        if (!held[vk] && !super_down[0] && !super_down[1] && !prefix_key && !bound[vk] &&
            (scan >= 512 || !bound[0x400 + scan]))
            return false;
        unsigned mods = w_modifiers();
        if ((vk == VK_DELETE && (mods & 3) == 3) || (vk == VK_F12 && mods == (MOD_CONTROL | MOD_ALT)))
            return false;
        int release_id = -1, id = w_match(vk, scan, mods, key->time, held[vk], &release_id);
        if (id >= 0 && id == registered_launcher && generation == launcher_generation) {
            held[vk] = true;
            PostThreadMessageW(key_thread_id, key_dispatch, id, generation);
            return false;
        }
        if (id != -1 || release_id >= 0 || held[vk] || (mods & MOD_WIN)) {
            if (id >= 0)
                PostMessageW(target, action_msg, id, generation);
            if (release_id >= 0)
                on_release[vk] = release_id + 1;
            held[vk] = true;
            return true;
        }
    }
    return false;
}

////////////////
//~ Keyboard hook

static LRESULT CALLBACK w_keyboard_proc(int code, WPARAM event, LPARAM data) {
    if (code == HC_ACTION) {
        AcquireSRWLockExclusive(&matcher_lock);
        bool consumed = w_consume_key(event, (const KBDLLHOOKSTRUCT *)data);
        ReleaseSRWLockExclusive(&matcher_lock);
        if (consumed)
            return 1;
    }
    return CallNextHookEx(NULL, code, event, data);
}

static DWORD WINAPI w_pump_keyboard(void *ready) {
    MSG msg;
    PeekMessageW(&msg, NULL, 0, 0, PM_NOREMOVE);
    keyboard = SetWindowsHookExW(WH_KEYBOARD_LL, w_keyboard_proc, GetModuleHandleW(NULL), 0);
    SetEvent((HANDLE)ready);
    if (keyboard) {
        while (GetMessageW(&msg, NULL, 0, 0) > 0) {
            if (msg.message == WM_HOTKEY && !msg.hwnd) {
                // The registered notification gives this process activation
                // rights. The physical press below is the single dispatch.
                continue;
            } else if (msg.message == key_dispatch && !msg.hwnd) {
                PostMessageW(target, action_msg, msg.wParam, msg.lParam);
            } else if (msg.message == WM_APP && !msg.hwnd) {
                AcquireSRWLockExclusive(&matcher_lock);
                w_launcher_hotkey(false);
                w_launcher_hotkey(super_down[0] || super_down[1]);
                ReleaseSRWLockExclusive(&matcher_lock);
            } else
                DispatchMessageW(&msg);
        }
        w_launcher_hotkey(false);
        UnhookWindowsHookEx(keyboard);
    }
    return 0;
}

static void w_start_keyboard(void) {
    HANDLE ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ready)
        return;
    key_thread = CreateThread(NULL, 0, w_pump_keyboard, ready, 0, &key_thread_id);
    if (key_thread) {
        WaitForSingleObject(ready, INFINITE);
        if (!keyboard) {
            CloseHandle(key_thread);
            key_thread = NULL;
        }
    }
    CloseHandle(ready);
}

static bool w_super_pressed(void) {
    AcquireSRWLockShared(&matcher_lock);
    bool down = super_down[0] || super_down[1];
    ReleaseSRWLockShared(&matcher_lock);
    return down;
}

static bool mouse_held[2];

////////////////
//~ Mouse hook

static LRESULT CALLBACK w_mouse_proc(int code, WPARAM event, LPARAM data) {
    if (code != HC_ACTION || !target)
        return CallNextHookEx(NULL, code, event, data);
    const MSLLHOOKSTRUCT *m = (const MSLLHOOKSTRUCT *)data;
    if (event == WM_LBUTTONDOWN || event == WM_RBUTTONDOWN || event == WM_MBUTTONDOWN ||
        event == WM_XBUTTONDOWN)
        pointer_presses++;
    if (event == WM_MOUSEMOVE) {
        bool moved = !hover_known || hover_point.x != m->pt.x || hover_point.y != m->pt.y;
        hover_point = m->pt;
        hover_known = true;
        // Repositioning a window can generate mouse motion at the same screen
        // point. Only a change of pointer coordinates is new hover intent.
        if (hover_msg && moved && !drag_button && !hover_pending)
            hover_pending = PostMessageW(target, hover_msg, 0, 0) != 0;
    }
    if (drag_button) {
        if (event == WM_MOUSEMOVE)
            drag(input_drag_move, m->pt);
        if ((drag_button == WM_RBUTTONDOWN && event == WM_RBUTTONUP) ||
            (drag_button == WM_LBUTTONDOWN && event == WM_LBUTTONUP)) {
            drag(input_drag_end, m->pt);
            drag_button = 0;
            return 1;
        }
    } else if (drag && (event == WM_LBUTTONDOWN || (event == WM_RBUTTONDOWN && w_super_pressed())) &&
               drag(event == WM_RBUTTONDOWN ? input_drag_inside : input_drag_edge, m->pt)) {
        drag_button = (UINT)event;
        return 1;
    }
    if (event == WM_RBUTTONDOWN && context_msg && !w_super_pressed() &&
        w_os_desktop_surface(GetAncestor(WindowFromPoint(m->pt), GA_ROOT))) {
        desktop_context_press = true;
        return 1;
    }
    if (event == WM_RBUTTONUP && desktop_context_press) {
        desktop_context_press = false;
        if (w_os_desktop_surface(GetAncestor(WindowFromPoint(m->pt), GA_ROOT)))
            PostMessageW(target, context_msg, (WPARAM)(LONG_PTR)m->pt.x, (LPARAM)m->pt.y);
        return 1;
    }
    unsigned key = 0;
    if (event == WM_MOUSEWHEEL)
        key = (short)HIWORD(m->mouseData) > 0 ? 0x201 : 0x202;
    else if (event == WM_XBUTTONDOWN || event == WM_XBUTTONUP) {
        unsigned side = HIWORD(m->mouseData) == XBUTTON1 ? 0 : 1;
        if (event == WM_XBUTTONUP) {
            if (mouse_held[side]) {
                mouse_held[side] = false;
                return 1;
            }
            return CallNextHookEx(NULL, code, event, data);
        }
        key = 0x203 + side;
    }
    if (key) {
        AcquireSRWLockExclusive(&matcher_lock);
        int release_id = -1, id = w_match(key, 0, w_modifiers(), m->time, false, &release_id);
        ReleaseSRWLockExclusive(&matcher_lock);
        if (id >= 0) {
            PostMessageW(target, action_msg, id, generation);
            if (key >= 0x203)
                mouse_held[key - 0x203] = true;
            return 1;
        }
    }
    return CallNextHookEx(NULL, code, event, data);
}

////////////////
//~ Binding lifecycle

void w_input_stop(void) {
    if (key_thread) {
        PostThreadMessageW(key_thread_id, WM_QUIT, 0, 0);
        WaitForSingleObject(key_thread, INFINITE);
        CloseHandle(key_thread);
        key_thread = NULL;
    }
    if (mouse)
        UnhookWindowsHookEx(mouse);
    keyboard = mouse = NULL;
    target = NULL;
    w_input_reset();
}

void w_input_reset(void) {
    AcquireSRWLockExclusive(&matcher_lock);
    drag_button = 0;
    desktop_context_press = false;
    if (key_thread)
        PostThreadMessageW(key_thread_id, WM_APP, 0, 0);
    memset(super_down, 0, sizeof(super_down));
    prefix_key = 0;
    memset(held, 0, sizeof(held));
    memset(on_release, 0, sizeof(on_release));
    memset(mouse_held, 0, sizeof(mouse_held));
    ReleaseSRWLockExclusive(&matcher_lock);
}

void w_input_bind(HWND w, UINT message, UINT hover_message, UINT context_message, const w_desktop_config *c,
                bool *status, input_drag_callback resize_drag) {
    if (!target)
        w_os_modifier_keys(mapped_super);
    AcquireSRWLockExclusive(&matcher_lock);
    target = w;
    action_msg = message;
    hover_msg = c->follow_mouse ? hover_message : 0;
    context_msg = context_message;
    binding_count = c->binding_count;
    launcher_binding = -1;
    memset(bound, 0, sizeof(bound));
    for (int i = 0; i < binding_count; ++i) {
        const w_key_binding *b = &c->bindings[i];
        bindings[i] =
            (w_chord_match){b->key, b->next_key, b->mods, b->next_mods, b->modal, b->release, b->repeat};
        bound[b->key] = true;
        if (b->action.kind == cmd_launcher && b->mods == MOD_WIN && b->key < 256 && !b->modal &&
            !b->next_key && !b->release && !b->repeat)
            launcher_binding = i;
    }
    drag = resize_drag;
    generation++;
    if (key_thread && registered_launcher >= 0)
        PostThreadMessageW(key_thread_id, WM_APP, 0, 0);
    prefix_key = 0;
    memset(on_release, 0, sizeof(on_release));
    SetPropW(target, L"winwm.binding_generation", (HANDLE)(uintptr_t)generation);
    ReleaseSRWLockExclusive(&matcher_lock);
    if (!key_thread)
        w_start_keyboard();
    if (!mouse)
        mouse = SetWindowsHookExW(WH_MOUSE_LL, w_mouse_proc, GetModuleHandleW(NULL), 0);
    for (int i = 0; i < c->binding_count; i++) {
        unsigned key = c->bindings[i].key;
        status[i] = key >= 0x201 && key <= 0x204 ? mouse != NULL : keyboard != NULL;
    }
}

void w_input_mode(bool resize) {
    AcquireSRWLockExclusive(&matcher_lock);
    modal = resize;
    prefix_key = 0;
    ReleaseSRWLockExclusive(&matcher_lock);
}
