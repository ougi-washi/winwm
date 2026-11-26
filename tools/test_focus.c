#include "desktop/desktop.h"
#include <stdio.h>

// Use a private desktop and its thread-local active window so USER32 delivers
// real activation and focus messages without taking over the user's desktop.
static unsigned transfers;
static unsigned denied_transfers;
static HWND hung_window;
static BOOL hung(HWND window) { return (window && window == hung_window) || IsHungAppWindow(window); }
static BOOL activate(HWND window) {
    ++transfers;
    if (denied_transfers) { --denied_transfers; return FALSE; }
    SetActiveWindow(window);
    return GetActiveWindow() == window;
}

static BOOL thread_info(DWORD thread, GUITHREADINFO *info) {
    return GetGUIThreadInfo(thread ? thread : GetCurrentThreadId(), info);
}

#define GetForegroundWindow GetActiveWindow
#define SetForegroundWindow activate
#define GetGUIThreadInfo thread_info
#define IsHungAppWindow hung
static HWND broker_dialog;
static bool shell_dialog(HWND window) { return (window && window == broker_dialog) || w_os_shell_dialog(window); }
#define w_os_shell_dialog shell_dialog
#include "../src/desktop/focus.c"

w_desktop_state desk;
bool w_palette_shown(void) { return false; }
bool w_notice_visible(void) { return false; }
bool w_bar_menu_active(void) { return false; }
HANDLE w_desktop_popup_process(void) { return NULL; }
static uint64_t pointer_presses;
uint64_t w_input_pointer_press_count(void) { return pointer_presses; }
uint64_t w_desktop_activation_intent(void) { return pointer_presses; }
static unsigned scheduled;
void w_desktop_schedule(void) { ++scheduled; }
static int chooser_lists;
bool w_desktop_show_chooser_list(HWND window) { ++chooser_lists; return window == broker_dialog; }

int w_desktop_find(HWND window) {
    for (int i = 0; i < window_limit; ++i)
        if (desk.model.windows[i].live && desk.windows[i].hwnd == window)
            return i;
    return -1;
}

static w_window_id manage(HWND window) {
    w_window_id id = w_model_add(&desk.model, 0, 0, w_os_bounds(window));
    desk.windows[id.slot].hwnd = window;
    GetWindowThreadProcessId(window, &desk.windows[id.slot].pid);
    desk.windows[id.slot].wants_visible = true;
    return id;
}

static HWND dismiss_on_deactivation;
static LRESULT CALLBACK panel_proc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE && window == dismiss_on_deactivation)
        ShowWindow(window, SW_HIDE);
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(window, msg, wp, lp);
}

static int failures;
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "Focus test line %d failed: %s\n", __LINE__, #condition); ++failures; } } while (0)

static void hover(HWND window) {
    POINT point = {8, 8};
    CHECK(ClientToScreen(window, &point));
    CHECK(GetAncestor(WindowFromPoint(point), GA_ROOT) == GetAncestor(window, GA_ROOT));
    w_desktop_hover(point);
}

static void pump_focus(void) {
    ULONGLONG until = GetTickCount64() + 500;
    while (w_desktop_focus_pending() && GetTickCount64() < until) {
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        Sleep(5);
    }
}

int main(void) {
    wchar_t name[64];
    swprintf(name, count_of(name), L"WinwmFocusTest-%lu", GetCurrentProcessId());
    HDESK desktop = CreateDesktopW(name, NULL, NULL, 0, GENERIC_ALL, NULL);
    if (!desktop || !SetThreadDesktop(desktop)) {
        fprintf(stderr, "Could not open private test desktop (%lu)\n", GetLastError());
        return 1;
    }
    WNDCLASSW wc = {.lpfnWndProc = panel_proc, .hInstance = GetModuleHandleW(NULL), .lpszClassName = L"FocusTest"};
    if (!RegisterClassW(&wc)) {
        fprintf(stderr, "Could not register fixture class (%lu)\n", GetLastError());
        return 1;
    }
    HWND owner = CreateWindowW(wc.lpszClassName, L"Document", WS_OVERLAPPEDWINDOW,
                               20, 20, 390, 300, NULL, NULL, wc.hInstance, NULL);
    HWND edit = CreateWindowW(L"EDIT", L"Rename", WS_CHILD | WS_VISIBLE,
                              10, 35, 100, 24, owner, NULL, wc.hInstance, NULL);
    HWND panel = CreateWindowW(wc.lpszClassName, L"Panel", WS_POPUP,
                               180, 110, 100, 90, owner, NULL, wc.hInstance, NULL);
    HWND submenu = CreateWindowW(wc.lpszClassName, L"Submenu", WS_POPUP,
                                 290, 115, 90, 80, panel, NULL, wc.hInstance, NULL);
    // Same process and an explicit native owner, but a separate taskbar window.
    HWND document = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"Second document", WS_OVERLAPPEDWINDOW,
                                    420, 20, 260, 300, owner, NULL, wc.hInstance, NULL);
    HWND dialog = CreateWindowW(wc.lpszClassName, L"Dialog", WS_POPUP,
                                425, 110, 150, 100, document, NULL, wc.hInstance, NULL);
    HWND tool = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_APPWINDOW, wc.lpszClassName, L"Tool", WS_POPUP,
                                188, 210, 100, 70, owner, NULL, wc.hInstance, NULL);
    HWND detached = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"Detached tool", WS_POPUP,
                                    535, 180, 90, 80, NULL, NULL, wc.hInstance, NULL);
    if (!owner || !edit || !panel || !submenu || !document || !dialog || !tool || !detached) {
        fprintf(stderr, "Could not create fixture windows (%lu)\n", GetLastError());
        return 1;
    }
    ShowWindow(owner, SW_SHOW);
    ShowWindow(document, SW_SHOWNOACTIVATE);
    ShowWindow(panel, SW_SHOWNOACTIVATE);
    ShowWindow(submenu, SW_SHOWNOACTIVATE);
    w_model_init(&desk.model);
    desk.config.follow_mouse = true;
    w_window_id first = manage(owner), second = manage(document);

    SetActiveWindow(owner);
    SetFocus(edit);
    hover(edit);
    hover(panel);
    hover(submenu);
    CHECK(GetActiveWindow() == owner && GetFocus() == edit && transfers == 0);
    CHECK(desk.model.focus == first.slot);

    SetActiveWindow(panel);
    dismiss_on_deactivation = panel;
    transfers = 0;
    hover(owner);
    CHECK(GetActiveWindow() == panel && IsWindowVisible(panel) && transfers == 0);
    dismiss_on_deactivation = NULL;

    ShowWindow(tool, SW_SHOW);
    CHECK(GetActiveWindow() == tool);
    hover(owner);
    CHECK(GetActiveWindow() == tool && w_os_app_window(tool) == owner);

    ShowWindow(detached, SW_SHOW);
    dismiss_on_deactivation = detached;
    hover(detached);
    hover(owner);
    CHECK(GetActiveWindow() == detached && IsWindowVisible(detached));
    // A regular independent window in the same process still allows hover
    // to switch documents, even when it is excluded from this manager.
    SetWindowLongPtrW(detached, GWL_EXSTYLE, 0);
    hover(document);
    CHECK(GetActiveWindow() == document && desk.model.focus == second.slot);
    dismiss_on_deactivation = NULL;
    DestroyWindow(detached);

    SetActiveWindow(owner);
    hover(document);
    CHECK(GetActiveWindow() == document && desk.model.focus == second.slot);
    // Native activation can finish before a queued first-map request runs.
    CHECK(w_desktop_focus_new(second));
    ShowWindow(dialog, SW_SHOW);
    CHECK(GetActiveWindow() == dialog);
    transfers = 0;
    w_desktop_focus_apply();
    CHECK(GetActiveWindow() == dialog && transfers == 0 && !w_desktop_focus_pending());

    // A native keyboard activation can select a different app or dialog
    // between discovery and the deferred foreground request.
    SetActiveWindow(owner);
    CHECK(w_desktop_focus_new(first));
    SetActiveWindow(dialog);
    w_desktop_focus_apply();
    CHECK(GetActiveWindow() == dialog && !w_desktop_focus_pending());

    SetCapture(dialog);
    hover(owner);
    CHECK(GetActiveWindow() == dialog);
    ReleaseCapture();
    hover(owner);
    CHECK(w_os_app_window(GetActiveWindow()) == owner && desk.model.focus == first.slot);
    EnableWindow(document, FALSE);
    hover(dialog);
    CHECK(GetActiveWindow() == dialog && desk.model.focus == second.slot);

    EnableWindow(document, TRUE);
    SetActiveWindow(owner);
    CHECK(w_desktop_focus_new(first));
    // The in-process Windows picker uses this class independently of the
    // caller's executable, language and main window.
    wc.lpszClassName = L"Shell_Dialog";
    CHECK(RegisterClassW(&wc));
    HWND embedded = CreateWindowW(wc.lpszClassName, L"", WS_CHILD | WS_VISIBLE,
                                  0, 0, 40, 40, owner, NULL, wc.hInstance, NULL);
    CHECK(embedded && !w_os_shell_dialog(embedded));
    DestroyWindow(embedded);
    HWND chooser = CreateWindowW(wc.lpszClassName, L"Chooser", WS_POPUP,
                            0, 0, 1, 1, NULL, NULL, wc.hInstance, NULL);
    CHECK(chooser != NULL);
    desk.outputs[0].live = true;
    desk.outputs[0].work = (w_rect){32, 24, 640, 360};
    desk.outputs[0].dpi = 96;
    // Old show events can be dispatched after the user already switched apps.
    SetWindowPos(chooser, NULL, -110, -70, 160, 100, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    w_rect background = w_os_bounds(chooser);
    w_desktop_place_chooser(chooser, EVENT_OBJECT_SHOW);
    CHECK(w_rect_equal(w_os_bounds(chooser), background) && GetActiveWindow() == owner);
    SetWindowPos(chooser, NULL, 0, 0, 1, 1, SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(chooser, SW_SHOW);
    SetActiveWindow(chooser);
    dismiss_on_deactivation = chooser;
    w_desktop_place_chooser(chooser, EVENT_OBJECT_SHOW);
    SetWindowPos(chooser, NULL, -110, -70, 160, 100, SWP_NOZORDER | SWP_NOACTIVATE);
    w_desktop_place_chooser(chooser, EVENT_OBJECT_LOCATIONCHANGE);
    CHECK(w_rect_equal(w_os_bounds(chooser), ((w_rect){272, 154, 160, 100})));
    transfers = 0;
    w_desktop_focus_apply();
    CHECK(!w_desktop_focus_pending() && !w_desktop_focus_new(second));
    hover(document);
    CHECK(IsWindowVisible(chooser) && GetActiveWindow() == chooser && transfers == 0);
    SetWindowPos(chooser, NULL, 104, 96, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    w_desktop_place_chooser(chooser, EVENT_OBJECT_LOCATIONCHANGE);
    w_desktop_place_chooser(chooser, EVENT_SYSTEM_FOREGROUND);
    CHECK(w_rect_equal(w_os_bounds(chooser), ((w_rect){104, 96, 160, 100})));
    CHECK(chooser_lists == 0);
    // Closing the chooser immediately restores normal focus-follows-mouse.
    DestroyWindow(chooser);
    dismiss_on_deactivation = NULL;
    hover(document);
    CHECK(GetActiveWindow() == document);
    // A shell-less broker can leave its app list at zero height, with only
    // its confirmation strip visible. Keep the strip at its native size.
    wc.lpszClassName = L"Open With";
    CHECK(RegisterClassW(&wc));
    broker_dialog = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"", WS_POPUP,
                                    -224, -30, 448, 61, NULL, NULL, wc.hInstance, NULL);
    CHECK(broker_dialog != NULL);
    ShowWindow(broker_dialog, SW_SHOW);
    w_desktop_place_chooser(broker_dialog, EVENT_SYSTEM_FOREGROUND);
    w_desktop_place_chooser(broker_dialog, EVENT_OBJECT_LOCATIONCHANGE);
    CHECK(chooser_lists == 1 && w_rect_equal(w_os_bounds(broker_dialog), ((w_rect){128,173,448,61})));
    DestroyWindow(broker_dialog);

    wc.lpszClassName = L"WinwmEvents";
    CHECK(RegisterClassW(&wc));
    desk.root = CreateWindowW(wc.lpszClassName, L"Private desktop", WS_POPUP, 700, 400, 2, 2,
                              NULL, NULL, wc.hInstance, NULL);
    CHECK(desk.root);
    ShowWindow(desk.root, SW_SHOWNOACTIVATE);
    SetActiveWindow(owner);
    w_model_workspace(&desk.model, 1);
    transfers = 0;
    denied_transfers = 1;
    w_desktop_focus_empty();
    CHECK(w_desktop_focus_pending() && GetActiveWindow() == owner);
    pump_focus();
    CHECK(transfers == 2 && !w_desktop_focus_pending() && GetActiveWindow() == desk.root && desk.model.focus == -1);

    w_model_workspace(&desk.model, 0);
    denied_transfers = 1;
    w_request_focus(first.slot, false);
    w_desktop_focus_apply();
    pump_focus();
    CHECK(!w_desktop_focus_pending() && GetActiveWindow() == owner && desk.model.focus == first.slot);

    denied_transfers = 1;
    w_request_focus(second.slot, false);
    w_desktop_focus_apply();
    CHECK(w_desktop_focus_pending());
    ++pointer_presses;
    w_desktop_focus_apply();
    CHECK(!w_desktop_focus_pending() && GetActiveWindow() == owner);

    transfers = 0;
    denied_transfers = 4;
    w_request_focus(second.slot, false);
    w_desktop_focus_apply();
    pump_focus();
    CHECK(transfers == 2 && !w_desktop_focus_pending());
    denied_transfers = 0;
    SetActiveWindow(desk.root);
    w_model_workspace(&desk.model, 1);
    desk.windows[first.slot].wants_visible = false;
    scheduled = transfers = 0;
    w_desktop_activate(first.slot);
    CHECK(scheduled == 1 && transfers == 0 && w_desktop_focus_pending());
    CHECK(!w_desktop_focus_new(second));
    desk.windows[first.slot].wants_visible = true;
    w_desktop_focus_apply();
    CHECK(GetActiveWindow() == owner && transfers == 1 && !w_desktop_focus_pending());
    w_desktop_activate(second.slot);
    CHECK(w_os_app_window(GetActiveWindow()) == document && !w_desktop_focus_pending());
    ShowWindow(owner, SW_HIDE);
    hung_window = owner;
    w_request_focus(first.slot, false);
    w_desktop_focus_apply();
    CHECK(!w_desktop_focus_pending());
    ShowWindow(owner, SW_SHOWNOACTIVATE);
    hung_window = NULL;
    w_desktop_focus_apply();
    CHECK(w_os_app_window(GetActiveWindow()) == document);

    // A native app can immediately re-show its window before the manager
    // observes the hide. Only the notification for the outstanding request
    // acknowledges it; stale notifications leave that request pending.
    DWORD before_hide = GetTickCount() - 1000;
    CHECK(w_os_set_visible(owner, false) && !IsWindowVisible(owner));
    ShowWindow(owner, SW_SHOWNOACTIVATE);
    w_os_visibility_event(owner, EVENT_OBJECT_HIDE, before_hide);
    CHECK(!w_os_hidden_confirmed(owner));
    w_os_visibility_event(owner, EVENT_OBJECT_HIDE, GetTickCount());
    CHECK(w_os_hidden_confirmed(owner));
    CHECK(w_os_set_visible(owner, false) && !IsWindowVisible(owner));
    w_os_visibility_event(owner, EVENT_OBJECT_HIDE, GetTickCount());
    CHECK(w_os_set_visible(owner, true) && IsWindowVisible(owner));
    // The reverse ordering happens when an application dismisses itself
    // immediately after its workspace is shown.
    ShowWindow(owner, SW_HIDE);
    w_os_visibility_event(owner, EVENT_OBJECT_SHOW, before_hide);
    CHECK(w_os_shell_hidden(owner));
    w_os_visibility_event(owner, EVENT_OBJECT_SHOW, GetTickCount());
    CHECK(!w_os_shell_hidden(owner));
    DestroyWindow(desk.root);
    DestroyWindow(owner);
    return failures ? 1 : 0;
}
