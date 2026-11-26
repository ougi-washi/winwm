#include "input/input.h"
#include <limits.h>
#include <stdio.h>

////////////////
//~ Input

static HWND target;
static UINT action_msg, hover_msg, press_msg, gesture_msg;
static POINT hover_point;
static bool hover_pending, hover_known;
static LONG64 pointer_presses, key_presses;

////////////////
//~ Input Functions

uint64_t w_input_pointer_press_count(void) {
    return (uint64_t)InterlockedCompareExchange64(&pointer_presses, 0, 0);
}

uint64_t w_input_press_count(void) {
    return w_input_pointer_press_count() + (uint64_t)InterlockedCompareExchange64(&key_presses, 0, 0);
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
static HANDLE hook_thread;
static DWORD hook_thread_id;
static LONG hook_state, stopping;
static int reported_hooks = -1;
static ULONGLONG retry_after;
static bool retry_timer;
enum { keyboard_ready = 1, mouse_ready = 2, both_hooks = 3, hook_retry_ms = 1500 };
static SRWLOCK matcher_lock = SRWLOCK_INIT;
static bool modal;
enum { key_idle, key_forwarded, key_claimed };
enum { key_slot_count = 256 + 512 }; // Virtual-only keys, followed by hardware scan codes.
static unsigned char key_owner[key_slot_count];
static int on_release[key_slot_count], on_repeat[key_slot_count];
static unsigned prefix_key, prefix_mods;
static DWORD prefix_time, generation;
static unsigned super_keys;
static unsigned native_modifiers;
// Aggregate delivery metrics. No virtual keys or text are recorded.
static LONG keyboard_events, matched_chords, unmatched_super, repeated_keys;
static LONG unmatched_mods, commands_posted, commands_failed;

void w_input_diagnostics(char *output, size_t cap) {
    snprintf(output, cap, "Keyboard events %ld, matches %ld, unmatched Super %ld (last modifiers %ld), "
             "held keydowns %ld, commands posted %ld, post failures %ld",
             InterlockedCompareExchange(&keyboard_events, 0, 0), InterlockedCompareExchange(&matched_chords, 0, 0),
             InterlockedCompareExchange(&unmatched_super, 0, 0), InterlockedCompareExchange(&unmatched_mods, 0, 0),
             InterlockedCompareExchange(&repeated_keys, 0, 0), InterlockedCompareExchange(&commands_posted, 0, 0),
             InterlockedCompareExchange(&commands_failed, 0, 0));
}

static bool w_post_action(WPARAM id, LPARAM version) {
    bool posted = PostMessageW(target, action_msg, id, version) != 0;
    InterlockedIncrement(posted ? &commands_posted : &commands_failed);
    return posted;
}
static bool mapped_super[2];
// The sender and the low-level hook share this process-private marker.
static char media_input_tag;
// Use the WM_HOTKEY foreground grant when opening Menu from another process.
// Binding dispatch always follows the matched physical press, including very
// short taps where a queued notification may outlive its registration.
enum { key_dispatch = WM_APP + 1, reset_buttons, retry_hooks };

static int menu_binding = -1, registered_menu = -1;
static DWORD menu_generation;
static void w_menu_hotkey(bool enable) {
    if (!enable && registered_menu >= 0) {
        UnregisterHotKey(NULL, 1);
        registered_menu = -1;
    }
    if (enable && registered_menu < 0 && menu_binding >= 0) {
        w_chord_match *b = &bindings[menu_binding];
        if (RegisterHotKey(NULL, 1, (b->mods & ~MOD_WIN) | MOD_NOREPEAT, b->key)) {
            registered_menu = menu_binding;
            menu_generation = generation;
        }
    }
}

static input_drag_callback drag;
// Button pairing belongs to the hook thread. The desktop thread alone edits windows.
static UINT drag_button, secondary_button;
static struct {
    bool held;
    int release;
    DWORD generation;
} side_buttons[2];
static LONG next_drag, hook_drag, active_drag, canceled_drag, offered_drag;
static HANDLE gesture_answer;
static LONG follow_pointer, motion_pending;
static bool border_drag;
static POINT drag_point;
static unsigned char mouse_actions[2][16];
enum { pointer_motion = input_drag_commit + 1 };
enum { gesture_budget_ms = 20 };

static void w_queue_hover(POINT point) {
    bool moved = !hover_known || hover_point.x != point.x || hover_point.y != point.y;
    hover_point = point;
    hover_known = true;
    if (hover_msg && moved && !active_drag && !hover_pending)
        hover_pending = PostMessageW(target, hover_msg, 0, 0) != 0;
}

static void w_input_mouse_motion(void) {
    POINT point;
    w_input_cancel_pointer();
    if ((hover_msg || active_drag) && GetCursorPos(&point)) {
        w_queue_hover(point);
        if (active_drag && (point.x != drag_point.x || point.y != drag_point.y)) {
            drag_point = point;
            drag(input_drag_move, point);
        }
    }
}

static bool w_pointer_post(UINT event, LONG id, POINT point) {
    WPARAM request = ((uint64_t)(DWORD)id << 32) | event;
    LPARAM position = (LPARAM)(((uint64_t)(DWORD)point.y << 32) | (DWORD)point.x);
    return PostMessageW(target, gesture_msg, request, position) != 0;
}

// Only button-down needs a desktop decision. A busy UI forfeits the click.
// Messages carry values so even a late delivery is safe after a timeout.
static LONG w_offer_pointer(UINT event, POINT point) {
    if (!gesture_answer) return 0;
    next_drag = next_drag == LONG_MAX ? 1 : next_drag + 1;
    LONG id = next_drag;
    ResetEvent(gesture_answer);
    InterlockedExchange(&offered_drag, id);
    if (w_pointer_post(event, id, point)) {
        ULONGLONG deadline = GetTickCount64() + gesture_budget_ms;
        while (InterlockedCompareExchange(&offered_drag, 0, 0) == id) {
            ULONGLONG now = GetTickCount64();
            if (now >= deadline || WaitForSingleObject(gesture_answer, (DWORD)(deadline - now)) != WAIT_OBJECT_0)
                break;
        }
    }
    return InterlockedExchange(&offered_drag, 0) == -id ? id : 0;
}

void w_input_pointer_message(WPARAM request, LPARAM position) {
    UINT event = (UINT)request;
    LONG id = (LONG)(request >> 32);
    POINT point = {(LONG)position, (LONG)((uint64_t)position >> 32)};
    w_input_cancel_pointer();
    if (event == pointer_motion) {
        InterlockedExchange(&motion_pending, 0);
        w_input_mouse_motion();
    } else if (event == input_drag_end) {
        if (id && InterlockedCompareExchange(&active_drag, 0, id) == id)
            drag(input_drag_end, point);
    } else if (id > 0 && InterlockedCompareExchange(&offered_drag, 0, 0) == id) {
        bool accepted = event <= input_drag_reposition && drag && drag((enum input_drag_event)event, point);
        // Commit side effects only when the hook still owns this physical down.
        bool claimed = InterlockedCompareExchange(&offered_drag, accepted ? -id : 0, id) == id;
        if (claimed) SetEvent(gesture_answer);
        if (accepted) {
            if (claimed) {
                drag_point = point;
                InterlockedExchange(&active_drag, id);
                drag(input_drag_commit, point);
            } else
                drag(input_drag_cancel, point);
        }
    }
}

void w_input_cancel_pointer(void) {
    LONG canceled = InterlockedExchange(&canceled_drag, 0);
    if (canceled && canceled == InterlockedCompareExchange(&active_drag,0,canceled))
        drag(input_drag_cancel, drag_point);
}

static int w_modifier_side(unsigned vk, unsigned scan, bool extended) {
    if (vk == VK_SHIFT) vk = scan == 0x36 ? VK_RSHIFT : VK_LSHIFT;
    if (vk == VK_CONTROL) vk = extended ? VK_RCONTROL : VK_LCONTROL;
    if (vk == VK_MENU) vk = extended ? VK_RMENU : VK_LMENU;
    return vk >= VK_LSHIFT && vk <= VK_RMENU ? (int)(vk - VK_LSHIFT) : -1;
}

static unsigned w_sample_modifiers(void) {
    unsigned sides = 0;
    for (int i = 0; i < 6; ++i)
        if (GetAsyncKeyState(VK_LSHIFT + i) & 0x8000) sides |= 1u << i;
    return sides;
}

static void w_seed_modifiers(void) {
    static const int super_vk[] = {VK_LWIN, VK_RWIN, VK_F13, VK_F14};
    native_modifiers = w_sample_modifiers();
    super_keys = 0;
    for (unsigned i = 0; i < count_of(super_vk); ++i) {
        if ((i < 2 || mapped_super[i - 2]) && (GetAsyncKeyState(super_vk[i]) & 0x8000))
            super_keys |= 1u << i;
    }
}

static unsigned w_modifiers(unsigned sides) {
    return (sides & 3 ? MOD_SHIFT : 0) | (sides & 12 ? MOD_CONTROL : 0) |
           (sides & 48 ? MOD_ALT : 0) | (super_keys ? MOD_WIN : 0);
}

static unsigned w_mouse_modifiers(void) {
    unsigned mods = w_modifiers(keyboard ? native_modifiers : w_sample_modifiers());
    if (!keyboard && ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000))
        mods |= MOD_WIN;
    return mods;
}

static int w_super_key(unsigned vk) {
    if (vk == VK_LWIN) return 0;
    if (vk == VK_RWIN) return 1;
    if (vk == VK_F13 && mapped_super[0]) return 2;
    if (vk == VK_F14 && mapped_super[1]) return 3;
    return -1;
}

static bool w_match_key(unsigned key, unsigned vk, unsigned scan) {
    return key == vk || (scan && key == 0x400 + scan);
}

static int w_match(unsigned vk, unsigned scan, unsigned mods, DWORD time, int *release_id) {
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
        else
            found = i;
    }
    if (candidate) {
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
    if (vk >= 256 || key->dwExtraInfo == (ULONG_PTR)&media_input_tag)
        return false;
    // Focus can change the active layout between down and up. Physical keys
    // keep their scan code, including the extended bit for keypad Enter.
    // VK_PACKET carries Unicode text in scanCode, not a physical key.
    unsigned scan = vk != VK_PACKET && key->scanCode && key->scanCode <= 0xff ?
                    key->scanCode + ((key->flags & LLKHF_EXTENDED) ? 256 : 0) : 0;
    unsigned slot = scan ? 256 + scan : vk;
    bool up = event == WM_KEYUP || event == WM_SYSKEYUP;
    if (key_owner[slot] == key_forwarded) {
        // The foreground client received the first down. Changing modifiers
        // mid-press cannot take its repeats or leave it waiting for the up.
        if (up) key_owner[slot] = key_idle;
        return false;
    }
    if (!up && vk == VK_ESCAPE) {
        LONG id = hook_drag;
        if (id) {
            hook_drag = 0;
            InterlockedExchange(&canceled_drag,id);
            PostMessageW(target, WM_CANCELMODE, 0, 0);
            key_owner[slot] = key_claimed;
            return true;
        }
    }
    int super = w_super_key(vk);
    if (super >= 0) {
        if (up) super_keys &= ~(1u << super);
        else super_keys |= 1u << super;
        if (!super_keys) w_menu_hotkey(false);
        return true;
    }
    if (registered_menu >= 0 && (up || vk != bindings[registered_menu].key))
        w_menu_hotkey(false);
    int side = w_modifier_side(vk, key->scanCode, key->flags & LLKHF_EXTENDED);
    if (side >= 0) {
        // A low-level callback precedes the asynchronous key-state update.
        // Match chords against this ordered stream, including both sides.
        if (up) native_modifiers &= ~(1u << side);
        else native_modifiers |= 1u << side;
        return false;
    }
    if (up) {
        if (key_owner[slot] == key_claimed) {
            if (on_release[slot] > 0) w_post_action(on_release[slot] - 1, generation);
            key_owner[slot] = key_idle;
            on_release[slot] = on_repeat[slot] = 0;
            return true;
        }
    } else {
        bool held = key_owner[slot] == key_claimed;
        if (!held) {
            InterlockedIncrement64(&key_presses);
            key_owner[slot] = key_forwarded;
        }
        if (!held && !super_keys && !prefix_key && !bound[vk] &&
            !bound[0x400 + scan])
            return false;
        unsigned mods = w_modifiers(native_modifiers);
        if ((vk == VK_DELETE || vk == VK_F12) && (mods & (MOD_CONTROL | MOD_ALT)) == (MOD_CONTROL | MOD_ALT)) {
            key_owner[slot] = key_forwarded;
            on_release[slot] = on_repeat[slot] = 0;
            return false;
        }
        if (held) {
            InterlockedIncrement(&repeated_keys);
            // Typematic keydowns belong to the original press. Changing modifiers
            // must not fire a different action before the physical key is released.
            int id = on_repeat[slot] - 1;
            if (id >= 0 && bindings[id].mods == mods && (!bindings[id].modal || modal))
                w_post_action(id, generation);
            else
                on_repeat[slot] = 0;
            return true;
        }
        int release_id = -1, id = w_match(vk, scan, mods, key->time, &release_id);
        if (id != -1 || release_id >= 0) InterlockedIncrement(&matched_chords);
        else if (mods & MOD_WIN) {
            InterlockedIncrement(&unmatched_super);
            InterlockedExchange(&unmatched_mods, (LONG)mods);
        }
        // Request a foreground grant only for the matched Menu press. Holding
        // or losing a Super key alone must never reserve a plain letter system-wide.
        if (id >= 0 && id == menu_binding)
            w_menu_hotkey(true);
        if (id >= 0 && id == registered_menu && generation == menu_generation) {
            if (PostThreadMessageW(hook_thread_id, key_dispatch, id, generation))
                key_owner[slot] = key_claimed;
            else
                w_menu_hotkey(false);
            return false;
        }
        if (id != -1 || release_id >= 0 || (mods & MOD_WIN)) {
            if (id >= 0) {
                if (!w_post_action(id, generation))
                    return false;
                if (bindings[id].repeat)
                    on_repeat[slot] = id + 1;
            }
            if (release_id >= 0)
                on_release[slot] = release_id + 1;
            key_owner[slot] = key_claimed;
            return true;
        }
    }
    return false;
}

bool w_input_send_media_key(WORD key) {
    INPUT keys[2] = {
        {.type = INPUT_KEYBOARD, .ki = {.wVk = key, .dwExtraInfo = (ULONG_PTR)&media_input_tag}},
        {.type = INPUT_KEYBOARD, .ki = {.wVk = key, .dwFlags = KEYEVENTF_KEYUP, .dwExtraInfo = (ULONG_PTR)&media_input_tag}},
    };
    return SendInput(2, keys, sizeof(keys[0])) == 2;
}

////////////////
//~ Keyboard hook

static LRESULT CALLBACK w_keyboard_proc(int code, WPARAM event, LPARAM data) {
    if (code == HC_ACTION && !InterlockedCompareExchange(&stopping, 0, 0)) {
        InterlockedIncrement(&keyboard_events);
        AcquireSRWLockExclusive(&matcher_lock);
        bool consumed = w_consume_key(event, (const KBDLLHOOKSTRUCT *)data);
        ReleaseSRWLockExclusive(&matcher_lock);
        if (consumed)
            return 1;
    }
    return CallNextHookEx(NULL, code, event, data);
}

static LRESULT CALLBACK w_mouse_proc(int code, WPARAM event, LPARAM data);

static void w_reset_buttons(void) {
    drag_button = secondary_button = 0;
    hook_drag = 0;
    memset(side_buttons, 0, sizeof(side_buttons));
}

static void w_install_hooks(void) {
    if (!keyboard) {
        keyboard = SetWindowsHookExW(WH_KEYBOARD_LL, w_keyboard_proc, GetModuleHandleW(NULL), 0);
        if (keyboard) {
            AcquireSRWLockExclusive(&matcher_lock);
            w_seed_modifiers();
            ReleaseSRWLockExclusive(&matcher_lock);
        }
    }
    if (!mouse) mouse = SetWindowsHookExW(WH_MOUSE_LL, w_mouse_proc, GetModuleHandleW(NULL), 0);
    LONG state = (keyboard ? keyboard_ready : 0) | (mouse ? mouse_ready : 0);
    if (InterlockedExchange(&hook_state, state) != state)
        PostMessageW(target, WM_TIMER, input_health_timer, 0);
}

static DWORD WINAPI w_pump_hooks(void *unused) {
    (void)unused;
    MSG msg;
    // PostThreadMessage needs a queue. If shutdown won the race to create
    // it, the flag is already set and this thread exits before blocking.
    PeekMessageW(&msg, NULL, 0, 0, PM_NOREMOVE);
    if (!InterlockedCompareExchange(&stopping, 0, 0)) w_install_hooks();
    while (!InterlockedCompareExchange(&stopping, 0, 0) && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.hwnd) {
            DispatchMessageW(&msg);
            continue;
        }
        switch (msg.message) {
        case key_dispatch: w_post_action(msg.wParam, msg.lParam); break;
        case reset_buttons: w_reset_buttons(); break;
        case retry_hooks: w_install_hooks(); break;
        case WM_APP:
            AcquireSRWLockExclusive(&matcher_lock);
            w_menu_hotkey(false);
            ReleaseSRWLockExclusive(&matcher_lock);
            break;
        // WM_HOTKEY grants foreground activation. The matched physical
        // press already queued key_dispatch, even on very short taps.
        }
    }
    w_menu_hotkey(false);
    if (keyboard) UnhookWindowsHookEx(keyboard);
    if (mouse) UnhookWindowsHookEx(mouse);
    keyboard = mouse = NULL;
    InterlockedExchange(&hook_state, 0);
    if (!InterlockedCompareExchange(&stopping, 0, 0)) PostMessageW(target, WM_TIMER, input_health_timer, 0);
    return 0;
}

HANDLE w_input_thread(void) { return hook_thread; }

void w_input_check(bool *status) {
    if (!target || InterlockedCompareExchange(&stopping, 0, 0)) return;
    if (hook_thread && WaitForSingleObject(hook_thread, 0) == WAIT_OBJECT_0) {
        CloseHandle(hook_thread);
        hook_thread = NULL;
        hook_thread_id = 0;
        InterlockedExchange(&hook_state, 0);
        w_reset_buttons();
        w_input_reset();
    }
    LONG state = InterlockedCompareExchange(&hook_state, 0, 0);
    if (reported_hooks != state) {
        for (int i = 0; i < binding_count; ++i) {
            unsigned key = bindings[i].key;
            status[i] = (state & (key >= 0x201 && key <= 0x204 ? mouse_ready : keyboard_ready)) != 0;
        }
        reported_hooks = state;
    }
    if (state == both_hooks) {
        if (retry_timer) KillTimer(target, input_health_timer);
        retry_timer = false;
        return;
    }
    if (!retry_timer) retry_timer = SetTimer(target, input_health_timer, hook_retry_ms, NULL) != 0;
    ULONGLONG now = GetTickCount64();
    if (now < retry_after) return;
    retry_after = now + hook_retry_ms;
    if (hook_thread) {
        // If the queue is still being created, its initial install is pending.
        PostThreadMessageW(hook_thread_id, retry_hooks, 0, 0);
    } else {
        hook_thread = CreateThread(NULL, 0, w_pump_hooks, NULL, 0, &hook_thread_id);
    }
}

////////////////
//~ Mouse hook

static LRESULT CALLBACK w_mouse_proc(int code, WPARAM event, LPARAM data) {
    if (code != HC_ACTION || !target || InterlockedCompareExchange(&stopping, 0, 0))
        return CallNextHookEx(NULL, code, event, data);
    const MSLLHOOKSTRUCT *m = (const MSLLHOOKSTRUCT *)data;
    if (event == WM_LBUTTONDOWN || event == WM_RBUTTONDOWN || event == WM_MBUTTONDOWN ||
        event == WM_XBUTTONDOWN) {
        InterlockedIncrement64(&pointer_presses);
        PostMessageW(target, press_msg, (WPARAM)(LONG_PTR)m->pt.x, (LPARAM)m->pt.y);
    }
    if (event == WM_MOUSEMOVE && (hook_drag || InterlockedCompareExchange(&follow_pointer, 0, 0)) &&
        !InterlockedExchange(&motion_pending, 1) && !w_pointer_post(pointer_motion, 0, m->pt))
        InterlockedExchange(&motion_pending, 0);
    if ((secondary_button == WM_LBUTTONDOWN && event == WM_LBUTTONUP) ||
        (secondary_button == WM_RBUTTONDOWN && event == WM_RBUTTONUP)) {
        secondary_button = 0;
        return 1;
    }
    if (drag_button) {
        if (event == WM_LBUTTONDOWN || event == WM_RBUTTONDOWN) {
            if (event != drag_button)
                secondary_button = (UINT)event;
            return 1;
        }
        if ((drag_button == WM_RBUTTONDOWN && event == WM_RBUTTONUP) ||
            (drag_button == WM_LBUTTONDOWN && event == WM_LBUTTONUP)) {
            if (hook_drag && !w_pointer_post(input_drag_end, hook_drag, m->pt))
                InterlockedExchange(&canceled_drag, hook_drag);
            drag_button = 0;
            hook_drag = 0;
            return 1;
        }
    } else if (event == WM_LBUTTONDOWN || event == WM_RBUTTONDOWN) {
        bool right = event == WM_RBUTTONDOWN;
        AcquireSRWLockShared(&matcher_lock);
        unsigned mods = w_mouse_modifiers();
        enum w_mouse_action action = mouse_actions[right][mods];
        bool edge = !right && border_drag;
        ReleaseSRWLockShared(&matcher_lock);
        if (action != mouse_unbound || edge) {
            UINT gesture = action == mouse_move ? input_drag_reposition : action == mouse_resize ? input_drag_inside :
                           input_drag_edge;
            LONG id = w_offer_pointer(gesture, m->pt);
            if (id) {
                hook_drag = id;
                drag_button = (UINT)event;
                return 1;
            }
        }
    }
    unsigned key = 0;
    if (event == WM_MOUSEWHEEL)
        key = (short)HIWORD(m->mouseData) > 0 ? 0x201 : 0x202;
    else if (event == WM_XBUTTONDOWN || event == WM_XBUTTONUP) {
        unsigned side = HIWORD(m->mouseData) == XBUTTON1 ? 0 : 1;
        if (event == WM_XBUTTONUP) {
            if (side_buttons[side].held) {
                if (side_buttons[side].release >= 0)
                    w_post_action(side_buttons[side].release, side_buttons[side].generation);
                side_buttons[side].held = false;
                return 1;
            }
            return CallNextHookEx(NULL, code, event, data);
        }
        key = 0x203 + side;
    }
    if (key) {
        AcquireSRWLockExclusive(&matcher_lock);
        int release_id = -1, id = w_match(key, 0, w_mouse_modifiers(), m->time, &release_id);
        DWORD version = generation;
        ReleaseSRWLockExclusive(&matcher_lock);
        if (id != -1 || release_id >= 0) {
            if (id >= 0 && !w_post_action(id, version))
                return CallNextHookEx(NULL, code, event, data);
            if (key >= 0x203) {
                unsigned side = key - 0x203;
                side_buttons[side].held = true;
                side_buttons[side].release = release_id;
                side_buttons[side].generation = version;
            } else if (release_id >= 0)
                w_post_action(release_id, version);
            return 1;
        }
    }
    return CallNextHookEx(NULL, code, event, data);
}

////////////////
//~ Binding lifecycle

void w_input_stop(void) {
    if (retry_timer) KillTimer(target, input_health_timer);
    retry_timer = false;
    InterlockedExchange(&stopping, TRUE);
    InterlockedExchange(&offered_drag, 0);
    if (gesture_answer) SetEvent(gesture_answer);
    if (hook_thread) PostThreadMessageW(hook_thread_id, WM_QUIT, 0, 0);
    if (hook_thread) {
        // The manager is exiting. If a Windows call is still stalled, keep
        // its handles alive for process teardown instead of hanging the UI.
        if (WaitForSingleObject(hook_thread, 1000) != WAIT_OBJECT_0) return;
        CloseHandle(hook_thread);
        hook_thread = NULL;
    }
    InterlockedExchange(&follow_pointer, 0);
    if (gesture_answer) CloseHandle(gesture_answer);
    gesture_answer = NULL;
    w_reset_buttons();
    keyboard = mouse = NULL;
    target = NULL;
    retry_after = 0;
    w_input_reset();
}

void w_input_reset(void) {
    InterlockedExchange(&offered_drag, 0);
    if (InterlockedExchange(&active_drag, 0) && drag)
        drag(input_drag_cancel, (POINT){0});
    InterlockedExchange(&canceled_drag, 0);
    hover_known = hover_pending = false;
    AcquireSRWLockExclusive(&matcher_lock);
    if (hook_thread && !InterlockedCompareExchange(&stopping, 0, 0)) w_seed_modifiers();
    else super_keys = native_modifiers = 0;
    prefix_key = 0;
    memset(key_owner, 0, sizeof(key_owner));
    memset(on_release, 0, sizeof(on_release));
    memset(on_repeat, 0, sizeof(on_repeat));
    ReleaseSRWLockExclusive(&matcher_lock);
    if (hook_thread) {
        PostThreadMessageW(hook_thread_id, reset_buttons, 0, 0);
        PostThreadMessageW(hook_thread_id, WM_APP, 0, 0);
    }
}

DWORD w_input_bind(HWND w, UINT message, UINT hover_message, UINT press_message,
                UINT gesture_message, const w_desktop_config *c, bool *status, input_drag_callback resize_drag) {
    if (!target) {
        InterlockedExchange(&stopping, FALSE);
        target = w;
        action_msg = message;
        press_msg = press_message;
        gesture_msg = gesture_message;
        gesture_answer = CreateEventW(NULL, FALSE, FALSE, NULL);
        w_os_modifier_keys(mapped_super);
    }
    drag = resize_drag;
    hover_msg = c->follow_mouse ? hover_message : 0;
    InterlockedExchange(&follow_pointer, c->follow_mouse);
    AcquireSRWLockExclusive(&matcher_lock);
    border_drag = c->resize_on_border;
    binding_count = c->binding_count;
    menu_binding = -1;
    memset(bound, 0, sizeof(bound));
    for (int i = 0; i < binding_count; ++i) {
        const w_key_binding *b = &c->bindings[i];
        bindings[i] =
            (w_chord_match){b->key, b->next_key, b->mods, b->next_mods, b->modal, b->release, b->repeat};
        bound[b->key] = true;
        if (b->action.kind == cmd_menu && b->mods == MOD_WIN && b->key < 256 && !b->modal &&
            !b->next_key && !b->release && !b->repeat)
            menu_binding = i;
    }
    memcpy(mouse_actions, c->mouse_actions, sizeof(mouse_actions));
    generation++;
    prefix_key = 0;
    memset(on_release, 0, sizeof(on_release));
    memset(on_repeat, 0, sizeof(on_repeat));
    ReleaseSRWLockExclusive(&matcher_lock);
    if (hook_thread)
        PostThreadMessageW(hook_thread_id, WM_APP, 0, 0);
    reported_hooks = -1;
    w_input_check(status);
    return generation;
}

void w_input_mode(bool resize) {
    AcquireSRWLockExclusive(&matcher_lock);
    modal = resize;
    prefix_key = 0;
    ReleaseSRWLockExclusive(&matcher_lock);
}
