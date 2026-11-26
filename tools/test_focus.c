#include "desktop/desktop.h"
#include <stdio.h>

// Use a private desktop and its thread-local active window so USER32 delivers
// real activation and focus messages without taking over the user's desktop.
static unsigned transfers;
static BOOL activate(HWND window) {
    ++transfers;
    SetActiveWindow(window);
    return GetActiveWindow() == window;
}

static BOOL thread_info(DWORD thread, GUITHREADINFO *info) {
    return GetGUIThreadInfo(thread ? thread : GetCurrentThreadId(), info);
}

#define GetForegroundWindow GetActiveWindow
#define SetForegroundWindow activate
#define GetGUIThreadInfo thread_info
#include "../src/desktop/focus.c"

w_desktop_state desk;
bool w_palette_shown(void) { return false; }
bool w_notice_visible(void) { return false; }
bool w_bar_menu_active(void) { return false; }
HANDLE w_desktop_popup_process(void) { return NULL; }
uint64_t w_input_pointer_press_count(void) { return 0; }
void w_desktop_schedule(void) {}

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

    SetCapture(dialog);
    hover(owner);
    CHECK(GetActiveWindow() == dialog);
    ReleaseCapture();
    hover(owner);
    CHECK(w_os_app_window(GetActiveWindow()) == owner && desk.model.focus == first.slot);
    EnableWindow(document, FALSE);
    hover(dialog);
    CHECK(GetActiveWindow() == dialog && desk.model.focus == second.slot);
    DestroyWindow(owner);
    return failures ? 1 : 0;
}
