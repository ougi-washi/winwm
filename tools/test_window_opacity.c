#include "os_win32/os_win32.h"
#include <stdio.h>
#include <string.h>

static const wchar_t fixture_class[] = L"WinwmOpacityTestApp";
static const wchar_t hold[] = L"winwm.test.hold_style", entered[] = L"winwm.test.style_entered";
static unsigned failures;

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "Opacity test line %d failed: %s\n", __LINE__, #condition); ++failures; } } while (0)

static LRESULT CALLBACK fixture_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_STYLECHANGING && wp == (WPARAM)GWL_EXSTYLE && GetPropW(w, hold)) {
        SetPropW(w, entered, (HANDLE)1);
        ULONGLONG start = GetTickCount64();
        while (GetPropW(w, hold) && GetTickCount64() - start < 3000) Sleep(5);
        RemovePropW(w, entered);
    }
    if (msg == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProcW(w, msg, wp, lp);
}

static int fixture(void) {
    WNDCLASSW cls = {.hInstance=GetModuleHandleW(NULL), .lpfnWndProc=fixture_proc, .lpszClassName=fixture_class};
    if (!RegisterClassW(&cls) || !CreateWindowW(fixture_class, L"", WS_OVERLAPPEDWINDOW,
        0, 0, 200, 120, NULL, NULL, cls.hInstance, NULL)) return 2;
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) DispatchMessageW(&msg);
    return 0;
}

static bool wait_style_callback(HWND w) {
    for (ULONGLONG start = GetTickCount64(); GetTickCount64() - start < 2000; Sleep(5))
        if (GetPropW(w, entered)) return true;
    return false;
}

static bool wait_alpha(HWND w, BYTE expected) {
    BYTE alpha = 0;
    DWORD flags = 0;
    for (ULONGLONG start = GetTickCount64(); GetTickCount64() - start < 3000; Sleep(5))
        if (GetLayeredWindowAttributes(w, NULL, &alpha, &flags) && (flags & LWA_ALPHA) && alpha == expected)
            return true;
    return false;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--fixture")) return fixture();
    wchar_t name[80], exe[os_path_cap], command[] = L"winwm-opacity-test --fixture";
    swprintf(name, count_of(name), L"WinwmOpacityTest-%lu", GetCurrentProcessId());
    HDESK desktop = CreateDesktopW(name, NULL, NULL, 0, GENERIC_ALL, NULL);
    if (!desktop || !SetThreadDesktop(desktop) || !GetModuleFileNameW(NULL, exe, count_of(exe))) return 2;
    PROCESS_INFORMATION child;
    STARTUPINFOW si = {.cb=sizeof(si), .lpDesktop=name};
    if (!CreateProcessW(exe, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &child)) return 2;
    HWND w = NULL;
    for (ULONGLONG start = GetTickCount64(); !w && GetTickCount64() - start < 3000; Sleep(5))
        w = FindWindowW(fixture_class, NULL);
    CHECK(w);
    if (w) {
        SetPropW(w, hold, (HANDLE)1);
        ULONGLONG start = GetTickCount64();
        CHECK(w_os_window_opacity(w, 128, true));
        CHECK(GetTickCount64() - start < 1000);
        CHECK(wait_style_callback(w));
        // User input can refocus the window several times while it is busy.
        CHECK(w_os_window_opacity(w, 255, false));
        CHECK(w_os_window_opacity(w, 194, true));
        RemovePropW(w, hold);
        CHECK(wait_alpha(w, 194));

        SetPropW(w, hold, (HANDLE)1);
        start = GetTickCount64();
        CHECK(w_os_window_opacity(w, 255, false));
        CHECK(GetTickCount64() - start < 1000);
        CHECK(wait_style_callback(w));
        CHECK(w_os_window_opacity(w, 112, true));
        RemovePropW(w, hold);
        CHECK(wait_alpha(w, 112));

        SetPropW(w, hold, (HANDLE)1);
        CHECK(w_os_window_opacity(w, 255, false));
        CHECK(wait_style_callback(w));
        w_os_restore(w);
        CHECK(!w_os_window_opacity(w, 108, true));
        RemovePropW(w, hold);
        w_os_wait_layer_cleanup();
        CHECK(!(GetWindowLongPtrW(w, GWL_EXSTYLE) & WS_EX_LAYERED));
        CHECK(!GetPropW(w, L"winwm.dim_added") && !GetPropW(w, L"winwm.dim_job"));
        // A client that owns its own layered style also owns its opacity.
        SetWindowLongPtrW(w, GWL_EXSTYLE, GetWindowLongPtrW(w, GWL_EXSTYLE) | WS_EX_LAYERED);
        CHECK(SetLayeredWindowAttributes(w, 0, 219, LWA_ALPHA));
        CHECK(!w_os_window_opacity(w, 121, true));
        w_os_restore(w);
        CHECK(wait_alpha(w, 219));
        PostMessageW(w, WM_CLOSE, 0, 0);
    }
    if (WaitForSingleObject(child.hProcess, 3000) != WAIT_OBJECT_0) {
        TerminateProcess(child.hProcess, 2);
        ++failures;
    }
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    return failures ? 1 : 0;
}
