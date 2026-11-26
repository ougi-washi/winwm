#include "input/input.h"
#include <stdio.h>

// Exercise the production hook thread and desktop message loop without
// intercepting the user's keyboard or sending input to the active desktop.
enum { test_key = WM_APP + 60, test_mouse, test_workspace_burst, action, hover, press, gesture };
enum { burst_rounds = 64, select_one = 8, select_two, send_one, select_keypad };
static HDESK desktop;
static HWND driver;
static HANDLE delivered;
static HANDLE installing_mouse, finish_install;
static HANDLE open_hook_queue;
static LONG delay_queue;
static HOOKPROC key_proc, mouse_proc;
static LONG consumed, modifiers;
static LONG keyboard_attempts, mouse_attempts;
static bool status[select_keypad + 1];
static DWORD ui_thread;
static DWORD key_time = 100;
static POINT cursor = {-38000, 42000};
static bool miss_mouse = true, accept_edge, delay_edge;
static int failures, commits, releases, cancellations, moves, attempts, actions[select_keypad + 1];
static int workspace_trace[burst_rounds * 4 + 1], workspace_count;
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "Input line %d: %s\n", __LINE__, #expr); ++failures; } } while (0)

static LRESULT hook_key(WPARAM event, DWORD input) {
    unsigned scan = HIWORD(input);
    KBDLLHOOKSTRUCT key = {.vkCode = LOWORD(input), .scanCode = scan & 0xff,
                           .flags = scan & 0x100 ? LLKHF_EXTENDED : 0, .time = ++key_time};
    return key_proc(HC_ACTION, event, (LPARAM)&key);
}

static LRESULT CALLBACK driver_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    LRESULT result;
    if (msg == test_key) {
        result = hook_key(wp, (DWORD)lp);
    } else if (msg == test_workspace_burst) {
        result = hook_key(WM_KEYDOWN, VK_LWIN);
        for (int i = 0; i < burst_rounds; ++i) {
            result &= hook_key(WM_KEYDOWN, '1');
            result &= hook_key(WM_KEYDOWN, '1'); // typematic, no extra select
            result &= hook_key(WM_KEYDOWN, '2'); // both numbers down
            result &= hook_key(WM_KEYUP, '1');
            result &= !hook_key(WM_KEYDOWN, VK_RSHIFT);
            result &= hook_key(WM_KEYDOWN, '1'); // move while 2 is held
            result &= hook_key(WM_KEYDOWN, '2'); // still belongs to original select
            result &= hook_key(WM_KEYUP, '1');
            result &= !hook_key(WM_KEYUP, VK_RSHIFT);
            result &= hook_key(WM_KEYUP, '2');
            result &= hook_key(WM_KEYDOWN, '2');
            result &= hook_key(WM_KEYUP, '2');
        }
        result &= hook_key(WM_KEYUP, VK_LWIN);
    } else if (msg == test_mouse) {
        MSLLHOOKSTRUCT mouse = {.pt = cursor, .mouseData = (DWORD)lp, .time = key_time};
        result = mouse_proc(HC_ACTION, wp, (LPARAM)&mouse);
    } else return DefWindowProcW(w, msg, wp, lp);
    InterlockedExchange(&consumed, (LONG)result);
    SetEvent(delivered);
    return 0;
}

static HHOOK install_hook(int type, HOOKPROC fn, HINSTANCE inst, DWORD tid) {
    (void)tid;
    if (type == WH_KEYBOARD_LL) {
        InterlockedIncrement(&keyboard_attempts);
        if (GetCurrentThreadId() == ui_thread || !SetThreadDesktop(desktop)) return NULL;
        WNDCLASSW wc = {.hInstance = inst, .lpszClassName = L"WinwmInputDriver", .lpfnWndProc = driver_proc};
        RegisterClassW(&wc);
        driver = CreateWindowW(wc.lpszClassName, L"", 0, 0,0,0,0, HWND_MESSAGE, NULL, inst, NULL);
        if (!driver) return NULL;
        key_proc = fn;
    } else if (type == WH_MOUSE_LL && GetWindowThreadProcessId(driver, NULL) == GetCurrentThreadId()) {
        LONG attempt = InterlockedIncrement(&mouse_attempts);
        if (miss_mouse) { miss_mouse = false; return NULL; }
        if (attempt == 2) {
            SetEvent(installing_mouse);
            if (WaitForSingleObject(finish_install, 8000) != WAIT_OBJECT_0) return NULL;
        }
        mouse_proc = fn;
    } else return NULL;
    return (HHOOK)(uintptr_t)type;
}
static LRESULT next_hook(HHOOK hook, int code, WPARAM wp, LPARAM lp) {
    (void)hook; (void)code; (void)wp; (void)lp;
    return 0;
}
static SHORT key_state(int key) {
    int bit = key == VK_MENU || key == VK_RMENU ? MOD_ALT :
              key == VK_CONTROL || key == VK_RCONTROL ? MOD_CONTROL :
              key == VK_SHIFT || key == VK_RSHIFT ? MOD_SHIFT :
              key == VK_LWIN ? MOD_WIN : 0;
    return modifiers & bit ? (SHORT)0x8000 : 0;
}
static BOOL get_cursor(POINT *point) { *point = cursor; return TRUE; }
static BOOL peek_message(MSG *msg, HWND window, UINT first, UINT last, UINT remove) {
    if (GetCurrentThreadId() != ui_thread && InterlockedCompareExchange(&delay_queue, 0, 0))
        WaitForSingleObject(open_hook_queue, 5000);
    return PeekMessageW(msg, window, first, last, remove);
}
static BOOL post_thread_message(DWORD thread, UINT message, WPARAM wp, LPARAM lp) {
    BOOL result = PostThreadMessageW(thread, message, wp, lp);
    if (message == WM_QUIT && InterlockedCompareExchange(&delay_queue, 0, 0))
        SetEvent(open_hook_queue);
    return result;
}
#define SetWindowsHookExW install_hook
#define UnhookWindowsHookEx(hook) ((void)(hook))
#define CallNextHookEx next_hook
#define GetAsyncKeyState key_state
#define GetCursorPos get_cursor
#define PeekMessageW peek_message
#define PostThreadMessageW post_thread_message
#include "../src/input/input.c"
#undef PeekMessageW
#undef PostThreadMessageW

void w_os_modifier_keys(bool mapped[2]) { mapped[0] = mapped[1] = true; }

static bool on_drag(enum input_drag_event ev, POINT point) {
    CHECK(GetCurrentThreadId() == ui_thread);
    if (ev == input_drag_edge || ev == input_drag_reposition) {
        CHECK(point.x == cursor.x && point.y == cursor.y);
        ++attempts;
        if (delay_edge) Sleep(gesture_budget_ms * 4);
        return accept_edge;
    }
    if (ev == input_drag_commit) ++commits;
    if (ev == input_drag_end) ++releases;
    if (ev == input_drag_cancel) ++cancellations;
    if (ev == input_drag_move) ++moves;
    return true;
}

static LRESULT CALLBACK ui_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == gesture) w_input_pointer_message(wp, lp);
    else if (msg == action && wp < count_of(actions)) {
        ++actions[wp];
        if (wp >= select_one && workspace_count < (int)count_of(workspace_trace))
            workspace_trace[workspace_count++] = (int)wp;
    }
    else if (msg == WM_CANCELMODE) w_input_cancel_pointer();
    else if (msg == WM_TIMER && wp == input_health_timer) w_input_check(status);
    else return DefWindowProcW(w, msg, wp, lp);
    return 0;
}
static void pump(void) {
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
}

static bool wait_for_status(bool both) {
    ULONGLONG deadline = GetTickCount64() + 5000;
    do {
        pump();
        if (status[0] && status[1] == both) return true;
        Sleep(5);
    } while (GetTickCount64() < deadline);
    return false;
}
static int deliver(UINT event, WPARAM wp, LPARAM lp, bool paused) {
    ResetEvent(delivered);
    if (!PostMessageW(driver, event, wp, lp)) return -1;
    if (paused) {
        if (WaitForSingleObject(delivered, 500) != WAIT_OBJECT_0) return -1;
    } else {
        for (;;) {
            DWORD wait = MsgWaitForMultipleObjects(1, &delivered, FALSE, 2000, QS_ALLINPUT);
            if (wait == WAIT_OBJECT_0) break;
            if (wait != WAIT_OBJECT_0 + 1) return -1;
            pump();
        }
    }
    pump();
    return consumed;
}
static int key(bool down, UINT vk) { return deliver(test_key, down ? WM_KEYDOWN : WM_KEYUP, vk, false); }
static int scanned_key(bool down, UINT vk, UINT scan) {
    return deliver(test_key, down ? WM_KEYDOWN : WM_KEYUP, MAKELPARAM(vk, scan), false);
}
static int mouse_event_result(UINT event, DWORD data) { return deliver(test_mouse, event, data, false); }

int main(void) {
    wchar_t name[64];
    swprintf(name, count_of(name), L"WinwmInputTest-%lu", GetCurrentProcessId());
    desktop = CreateDesktopW(name, NULL, NULL, 0, GENERIC_ALL, NULL);
    if (!desktop || !SetThreadDesktop(desktop)) return 1;
    ui_thread = GetCurrentThreadId();
    HINSTANCE instance = GetModuleHandleW(NULL);
    WNDCLASSW wc = {.lpszClassName=L"WinwmInputTest", .lpfnWndProc=ui_proc, .hInstance=instance};
    RegisterClassW(&wc);
    HWND ui = CreateWindowW(wc.lpszClassName, L"", 0, 0,0,0,0, HWND_MESSAGE, NULL, instance, NULL);
    delivered = CreateEventW(NULL, TRUE, FALSE, NULL);
    installing_mouse = CreateEventW(NULL, TRUE, FALSE, NULL);
    finish_install = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ui || !delivered || !installing_mouse || !finish_install) return 1;
    static w_desktop_config cfg;
    cfg.resize_on_border = true;
    cfg.mouse_actions[0][MOD_WIN] = mouse_move;
    cfg.bindings[0] = (w_key_binding){.mods=MOD_WIN, .key='A'};
    cfg.bindings[1] = (w_key_binding){.mods=MOD_WIN, .key=0x203, .release=true};
    cfg.bindings[2] = (w_key_binding){.mods=MOD_WIN, .key=0x201};
    cfg.bindings[3] = (w_key_binding){.mods=MOD_WIN, .key='K', .next_key='Q'};
    cfg.bindings[4] = (w_key_binding){.mods=MOD_WIN, .key='D', .action={.kind=cmd_menu}};
    cfg.bindings[5] = (w_key_binding){.mods=MOD_CONTROL | MOD_SHIFT, .key=VK_F2, .repeat=true};
    cfg.bindings[6] = (w_key_binding){.mods=MOD_ALT, .key='R', .release=true};
    cfg.bindings[7] = (w_key_binding){.mods=MOD_WIN | MOD_SHIFT, .key='A'};
    cfg.bindings[select_one] = (w_key_binding){.mods=MOD_WIN, .key='1', .action={cmd_workspace_select, "1"}};
    cfg.bindings[select_two] = (w_key_binding){.mods=MOD_WIN, .key='2', .action={cmd_workspace_select, "2"}};
    cfg.bindings[send_one] = (w_key_binding){.mods=MOD_WIN|MOD_SHIFT, .key='1', .action={cmd_workspace_send, "1"}};
    cfg.bindings[select_keypad] = (w_key_binding){.mods=MOD_WIN, .key=0x51c, .action={cmd_workspace_select, "3"}};
    cfg.binding_count = count_of(actions);
    w_input_bind(ui, action, hover, press, gesture, &cfg, status, on_drag);
    DWORD configured = generation;
    CHECK(wait_for_status(false));
    ULONGLONG deadline = GetTickCount64() + 5000;
    while (WaitForSingleObject(installing_mouse, 0) == WAIT_TIMEOUT && GetTickCount64() < deadline) {
        pump();
        Sleep(5);
    }
    CHECK(WaitForSingleObject(installing_mouse,0) == WAIT_OBJECT_0);
    ULONGLONG started = GetTickCount64();
    w_input_check(status);
    CHECK(GetTickCount64() - started < 250 && generation == configured && keyboard_attempts == 1);
    // Reload while the Windows installation call is blocked on the other
    // thread. Applying a new configuration must still finish immediately.
    w_input_bind(ui, action, hover, press, gesture, &cfg, status, on_drag);
    CHECK(GetTickCount64() - started < 250);
    configured = generation;
    SetEvent(finish_install);
    CHECK(wait_for_status(true));
    CHECK(keyboard_attempts == 1 && mouse_attempts == 2 && generation == configured);
    CHECK(status[0] && status[1] && driver && mouse_proc && key_proc);
    if (!driver || !mouse_proc || !key_proc) return 1;

    CHECK(!key(true, 'A') && !key(false, 'A'));
    CHECK(deliver(test_workspace_burst, 0, 0, true) == 1);
    CHECK(workspace_count == burst_rounds * 4);
    const int expected[] = {select_one, select_two, send_one, select_two};
    for (int i = 0; i < workspace_count; ++i) CHECK(workspace_trace[i] == expected[i % count_of(expected)]);
    // A client that received the initial down also owns its typematic and up
    // when a Super chord overlaps the end of ordinary typing.
    uint64_t presses = w_input_press_count();
    CHECK(!key(true,'1'));
    CHECK(key(true,VK_LWIN));
    CHECK(!key(true,'1'));
    CHECK(!key(false,'1'));
    CHECK(w_input_press_count() == presses + 1);
    CHECK(workspace_count == burst_rounds * 4);
    CHECK(key(true,'1') && key(false,'1') && key(false,VK_LWIN));
    CHECK(w_input_press_count() == presses + 2);
    CHECK(workspace_count == burst_rounds * 4 + 1 && workspace_trace[workspace_count - 1] == select_one);
    // Main and keypad Enter have the same VK and independent physical presses.
    CHECK(!scanned_key(true,VK_RETURN,0x1c));
    CHECK(key(true,VK_LWIN));
    CHECK(scanned_key(true,VK_RETURN,0x11c));
    CHECK(!scanned_key(false,VK_RETURN,0x1c));
    CHECK(scanned_key(false,VK_RETURN,0x11c));
    CHECK(actions[select_keypad] == 1 && key(false,VK_LWIN));
    // A release translated under a different layout still ends its original press.
    int before_select = actions[select_one];
    CHECK(key(true,VK_LWIN) && scanned_key(true,'1',2));
    CHECK(scanned_key(false,VK_OEM_PLUS,2));
    CHECK(scanned_key(true,'1',2) && scanned_key(false,'1',2) && key(false,VK_LWIN));
    CHECK(actions[select_one] == before_select + 2);
    // The low-level callback precedes USER's asynchronous state update.
    // Accept a rapid move chord even if GetAsyncKeyState still reports Shift up.
    CHECK(key(true,VK_LWIN));
    CHECK(!key(true,VK_LSHIFT) && !key(true,VK_RSHIFT) && !key(false,VK_LSHIFT));
    CHECK(key(true,'A') && key(false,'A') && actions[7]==1);
    CHECK(!key(false,VK_RSHIFT) && key(false,VK_LWIN));
    CHECK(key(true, VK_LWIN) && key(true, VK_F13) && key(false, VK_LWIN));
    CHECK(key(true, 'A') && key(false, 'A') && actions[0] == 1);
    CHECK(key(true, VK_DELETE) && key(false, VK_DELETE));
    CHECK(!key(true,VK_LCONTROL) && !key(true,VK_RMENU));
    CHECK(!key(true, VK_F12) && !key(false, VK_F12));
    CHECK(!key(false,VK_LCONTROL) && !key(false,VK_RMENU));
    CHECK(mouse_event_result(WM_XBUTTONDOWN, MAKELONG(0, XBUTTON1)) && !actions[1]);
    CHECK(mouse_event_result(WM_XBUTTONUP, MAKELONG(0, XBUTTON1)) && actions[1] == 1);
    CHECK(mouse_event_result(WM_MOUSEWHEEL, MAKELONG(0, WHEEL_DELTA)) && actions[2] == 1);
    CHECK(key(true, 'K') && key(false, 'K') && key(false, VK_F13));
    CHECK(key(true, 'Q') && key(false, 'Q') && actions[3] == 1);
    CHECK(key(true, VK_LWIN));
    CHECK(key(true, 'A') && actions[0] == 2);
    // Leaving the input desktop can hide both releases from the hook.
    w_input_reset();
    CHECK(!key(true, 'A') && !key(false, 'A') && actions[0] == 2);
    CHECK(key(true, VK_LWIN) && registered_menu == -1);
    key(true, 'D'); // The native hotkey registration determines which thread consumes this down.
    CHECK(key(false, 'D') && key(false, VK_LWIN) && registered_menu == -1 && actions[4] == 1);
    CHECK(!key(true, 'D') && !key(false, 'D') && actions[4] == 1);
    CHECK(!key(true,VK_LCONTROL) && !key(true,VK_RSHIFT));
    CHECK(key(true, VK_F2) && key(true, VK_F2) && actions[5] == 2);
    CHECK(!key(false,VK_LCONTROL) && !key(false,VK_RSHIFT));
    CHECK(key(true, VK_F2) && key(false, VK_F2) && actions[5] == 2);
    CHECK(!key(true,VK_LMENU));
    CHECK(key(true, 'R') && !actions[6] && key(false, 'R') && actions[6] == 1);
    CHECK(key(true, 'R'));
    // A reload cancels the old action but owns the remainder of the same press.
    configured = w_input_bind(ui, action, hover, press, gesture, &cfg, status, on_drag);
    CHECK(key(true, 'R') && key(false, 'R') && actions[6] == 1);
    CHECK(key(true, 'R') && key(false, 'R') && actions[6] == 2);
    CHECK(!key(false,VK_LMENU));
    CHECK(key(true,VK_LWIN) && !key(true,VK_RSHIFT));
    CHECK(key(true,'A') && key(false,'A') && actions[7] == 2);
    CHECK(!key(false,VK_RSHIFT));
    CHECK(key(true,'A') && key(false,'A') && key(false,VK_LWIN) && actions[0] == 3);

    CHECK(!mouse_event_result(WM_LBUTTONDOWN, 0) && !mouse_event_result(WM_LBUTTONUP, 0));
    LONG offers = next_drag;
    CHECK(!mouse_event_result(WM_RBUTTONDOWN, 0) && !mouse_event_result(WM_RBUTTONUP, 0));
    CHECK(next_drag == offers);
    accept_edge = true;
    int before = attempts;
    CHECK(deliver(test_mouse, WM_LBUTTONDOWN, 0, true) == 0 && attempts == before);
    CHECK(!mouse_event_result(WM_LBUTTONUP, 0) && !commits);
    delay_edge = true;
    CHECK(!mouse_event_result(WM_LBUTTONDOWN, 0) && !commits && cancellations == 1);
    delay_edge = false;
    CHECK(mouse_event_result(WM_LBUTTONDOWN, 0) && commits == 1);
    ++cursor.x;
    CHECK(!mouse_event_result(WM_MOUSEMOVE, 0) && moves == 1);
    CHECK(mouse_event_result(WM_LBUTTONUP, 0) && releases == 1);
    CHECK(mouse_event_result(WM_LBUTTONDOWN, 0) && commits == 2);
    CHECK(key(true, VK_ESCAPE) && key(false, VK_ESCAPE) && cancellations == 2);
    CHECK(mouse_event_result(WM_LBUTTONUP, 0) && releases == 1);
    CHECK(!mouse_event_result(WM_MBUTTONDOWN, 0) && !mouse_event_result(WM_MBUTTONUP, 0));
    CHECK(w_input_pointer_press_count() == 8);
    // Losing the hook pump must release held input and establish both hooks
    // on a fresh thread using the same configuration.
    CHECK(key(true, VK_LWIN));
    HANDLE original;
    CHECK(DuplicateHandle(GetCurrentProcess(), hook_thread, GetCurrentProcess(), &original, SYNCHRONIZE, FALSE, 0));
    CHECK(PostThreadMessageW(hook_thread_id, WM_QUIT, 0, 0));
    CHECK(WaitForSingleObject(original, 3000) == WAIT_OBJECT_0);
    CloseHandle(original);
    modifiers = MOD_SHIFT | MOD_WIN;
    w_input_check(status);
    CHECK(wait_for_status(true));
    CHECK(keyboard_attempts == 2 && mouse_attempts == 3 && generation == configured);
    // The new hook starts with modifiers already held during the unhooked interval.
    CHECK(key(true,'A') && key(false,'A') && actions[7] == 3);
    modifiers = 0;
    CHECK(!key(false,VK_RSHIFT) && key(false,VK_LWIN));
    CHECK(!key(true, 'A') && !key(false, 'A'));
    char report[256];
    w_input_diagnostics(report, sizeof(report));
    CHECK(strstr(report, "unmatched Super 1 (last modifiers 8)") && strstr(report, "post failures 0"));
    w_input_stop();
    // Stop before the hook thread has a queue to receive WM_QUIT.
    open_hook_queue = CreateEventW(NULL, TRUE, FALSE, NULL);
    InterlockedExchange(&delay_queue, 1);
    w_input_bind(ui, action, hover, press, gesture, &cfg, status, on_drag);
    CHECK(hook_thread != NULL);
    w_input_stop();
    CHECK(hook_thread == NULL && keyboard_attempts == 2);
    CloseHandle(open_hook_queue);
    DestroyWindow(ui);
    CloseHandle(installing_mouse);
    CloseHandle(finish_install);
    CloseHandle(delivered);
    return failures ? 1 : 0;
}
