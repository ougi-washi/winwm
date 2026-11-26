#define COBJMACROS
#include "desktop_tools/tools.h"
#include "theme/theme.h"
#include <uiautomationclient.h>
#include <stdio.h>

// A private desktop cannot become foreground without switching the user's
// session. Drive that one input explicitly. The two windows and their input
// queues, focus, timers and messages are real.
static HDESK test_desktop;
static HWND native_window;
static PVOID volatile foreground;
static HANDLE provider_entered, release_provider, provider_returned;
static HWND test_foreground(void) { return InterlockedCompareExchangePointer(&foreground, NULL, NULL); }
static HRESULT WINAPI slow_provider(REFCLSID cls, IUnknown *outer, DWORD context, REFIID iid, void **out) {
    (void)cls; (void)outer; (void)context; (void)iid; (void)out;
    SetEvent(provider_entered);
    WaitForSingleObject(release_provider, 10000);
    SetEvent(provider_returned);
    return E_ABORT;
}
#define CoCreateInstance slow_provider
#define GetForegroundWindow test_foreground
#include "../src/desktop_tools/open_with.c"

bool w_os_open_with_broker(HWND w) { return w == native_window; }
bool w_theme_message(HWND w, UINT m, WPARAM wp, LPARAM lp, LRESULT *result) {
    (void)w; (void)m; (void)wp; (void)lp; (void)result; return false;
}
HFONT w_theme_message_font(UINT dpi) { (void)dpi; return NULL; }
int w_theme_button_size(UINT dpi) { return MulDiv(32, dpi, 96); }
int w_theme_spacing(UINT dpi) { return MulDiv(4, dpi, 96); }

static DWORD WINAPI run_helper(void *unused) {
    (void)unused;
    wchar_t target[64];
    swprintf(target, count_of(target), L"%llx-%lu", (unsigned long long)(uintptr_t)native_window, GetCurrentProcessId());
    return SetThreadDesktop(test_desktop) ? w_open_with_list(target) : 1;
}

static int failures;
#define CHECK(test) do { if (!(test)) { fprintf(stderr, "Open with test line %d failed: %s\n", __LINE__, #test); ++failures; } } while (0)

static void click(HWND control, int y) {
    LPARAM point = MAKELPARAM(4, y);
    SendMessageW(control, WM_LBUTTONDOWN, MK_LBUTTON, point);
    SendMessageW(control, WM_LBUTTONUP, 0, point);
}

int main(void) {
    wchar_t name[80];
    swprintf(name, count_of(name), L"WinwmPickerTest-%lu", GetCurrentProcessId());
    test_desktop = CreateDesktopW(name, NULL, NULL, 0, GENERIC_ALL, NULL);
    if (!test_desktop || !SetThreadDesktop(test_desktop)) return 1;
    WNDCLASSW wc = {.lpszClassName=L"Open With", .lpfnWndProc=DefWindowProcW, .hInstance=GetModuleHandleW(NULL)};
    if (!RegisterClassW(&wc)) return 1;
    native_window = CreateWindowW(wc.lpszClassName, L"Test picker", WS_VISIBLE | WS_POPUP,
                                  200,250,420,60, NULL,NULL,wc.hInstance,NULL);
    HWND edit = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE, 0,0,100,30,native_window,NULL,wc.hInstance,NULL);
    SetActiveWindow(native_window);
    SetFocus(edit);
    InterlockedExchangePointer(&foreground, native_window);
    provider_entered = CreateEventW(NULL, TRUE, FALSE, NULL);
    release_provider = CreateEventW(NULL, TRUE, FALSE, NULL);
    provider_returned = CreateEventW(NULL, TRUE, FALSE, NULL);
    DWORD ui_thread = 0;
    HANDLE helper = CreateThread(NULL, 0, run_helper, NULL, 0, &ui_thread);
    if (!edit || !provider_entered || !release_provider || !provider_returned || !helper) return 1;
    CHECK(WaitForSingleObject(provider_entered, 3000) == WAIT_OBJECT_0);
    HWND overlay = FindWindowW(L"WinwmOpenWithList", NULL);
    DWORD_PTR response = 0;
    CHECK(overlay && !GetWindow(overlay, GW_OWNER) && (GetWindowLongPtrW(overlay, GWL_EXSTYLE) & WS_EX_NOACTIVATE));
    CHECK(SendMessageTimeoutW(overlay, WM_TIMER, 1, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 300, &response));
    CHECK(active_view && SendMessageTimeoutW(overlay, WM_MOUSEACTIVATE, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 300, &response)
          && response == MA_NOACTIVATE);
    GUITHREADINFO info = {.cbSize=sizeof(info)};
    CHECK(GetGUIThreadInfo(ui_thread, &info) && info.hwndFocus != edit && GetFocus() == edit);
    // Deliver completed rows while the next automation call is still blocked.
    // Click the second item, whose provider index differs from its list index.
    if (overlay && active_view) {
        w_choice_labels labels = {.count=3, .names=calloc(3, sizeof(BSTR)), .title=SysAllocString(L"Test apps")};
        if (!labels.names) return 1;
        labels.names[0] = SysAllocString(L"First app");
        labels.names[2] = SysAllocString(L"Second app");
        AcquireSRWLockExclusive(&view_lock);
        changes.labels = labels;
        changes.labels_view = active_view;
        ReleaseSRWLockExclusive(&view_lock);
        SendMessageW(overlay, provider_changed, 0, 0);
        HWND options = FindWindowExW(overlay, NULL, L"ListBox", NULL);
        CHECK(IsWindowVisible(overlay) && options && SendMessageW(options, LB_GETCOUNT, 0, 0) == 2);
        int row_height = (int)SendMessageW(options, LB_GETITEMHEIGHT, 0, 0);
        CHECK(row_height > 0);
        click(options, row_height + 2);
        CHECK((DWORD)InterlockedExchange64(&requested_choice, 0) == 2 && GetFocus() == edit);
        SetWindowPos(options, NULL, 0, 0, 250, 4 * row_height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        click(options, 3 * row_height);
        CHECK(!InterlockedCompareExchange64(&requested_choice, 0, 0));
        InterlockedExchangePointer(&foreground, NULL);
        SendMessageW(overlay, WM_TIMER, 1, 0);
        click(options, 2);
        CHECK(!IsWindowVisible(overlay) && !InterlockedCompareExchange64(&requested_choice, 0, 0));
    }
    PostMessageW(overlay, WM_CLOSE, 0, 0);
    CHECK(WaitForSingleObject(helper, 1000) == WAIT_OBJECT_0 && GetFocus() == edit);
    SetEvent(release_provider);
    CHECK(WaitForSingleObject(provider_returned, 1000) == WAIT_OBJECT_0);
    CloseHandle(helper);
    DestroyWindow(native_window);
    return failures != 0;
}
