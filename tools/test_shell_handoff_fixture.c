// The controller gets a private namespace so developers can run the fixture
// alongside a real desktop manager. No fixture command reaches its control pipe.
#include <windows.h>
#include "input/input.h"
#include "desktop/desktop.h"
#include <stdio.h>
#include <stdlib.h>

static bool object_name(wchar_t *out, int cap, const wchar_t *prefix, unsigned kind) {
    wchar_t id[80];
    DWORD n = GetEnvironmentVariableW(L"WINWM_TEST_NAMESPACE", id, count_of(id));
    return n && n < count_of(id) && swprintf(out, cap, L"%ls%ls.%u", prefix, id, kind) > 0;
}

bool __wrap_w_ipc_name(wchar_t *out, int cap) {
    return object_name(out, cap, L"\\\\.\\pipe\\", 0);
}

DWORD __wrap_w_os_instance_claim(os_instance_kind kind, w_os_instance_lock *lock) {
    wchar_t name[128];
    if (!object_name(name, count_of(name), L"Local\\", kind)) return ERROR_INVALID_NAME;
    *lock = CreateMutexW(NULL, FALSE, name);
    if (!*lock) return GetLastError();
    if (GetLastError() != ERROR_ALREADY_EXISTS) return ERROR_SUCCESS;
    w_os_instance_release(lock);
    return ERROR_ALREADY_EXISTS;
}

DWORD __wrap_w_os_instance_status(os_instance_kind kind) {
    wchar_t name[128];
    if (!object_name(name, count_of(name), L"Local\\", kind)) return ERROR_INVALID_NAME;
    HANDLE instance = OpenMutexW(SYNCHRONIZE, FALSE, name);
    if (!instance) return GetLastError() == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : GetLastError();
    CloseHandle(instance);
    return ERROR_ALREADY_EXISTS;
}

static HANDLE named_event(const wchar_t *variable, DWORD access) {
    wchar_t name[128];
    DWORD n = GetEnvironmentVariableW(variable, name, 128);
    return n && n < 128 ? OpenEventW(access, FALSE, name) : NULL;
}

DWORD __wrap_w_os_exit_shell(HANDLE cancel) {
    (void)cancel;
    HANDLE entered = named_event(L"WINWM_TEST_HANDOFF_ENTERED", EVENT_MODIFY_STATE);
    HANDLE release = named_event(L"WINWM_TEST_HANDOFF_RELEASE", SYNCHRONIZE);
    HANDLE refuse = named_event(L"WINWM_TEST_REFUSE_ONCE", EVENT_MODIFY_STATE | SYNCHRONIZE);
    bool declined = refuse && WaitForSingleObject(refuse, 0) == WAIT_OBJECT_0;
    if (refuse) { ResetEvent(refuse); CloseHandle(refuse); }
    if (entered) { SetEvent(entered); CloseHandle(entered); }
    if (!release) return ERROR_INVALID_HANDLE;
    if (declined) { CloseHandle(release); return ERROR_BUSY; }
    DWORD wait = WaitForSingleObject(release, 30000);
    CloseHandle(release);
    HANDLE present = named_event(L"WINWM_TEST_EXPLORER_PRESENT", EVENT_MODIFY_STATE);
    if (present) { ResetEvent(present); CloseHandle(present); }
    return wait == WAIT_OBJECT_0 ? ERROR_SUCCESS : ERROR_TIMEOUT;
}

bool __wrap_w_os_explorer_shell_present(void) {
    HANDLE present = named_event(L"WINWM_TEST_EXPLORER_PRESENT", SYNCHRONIZE);
    if (!present) return false;
    bool found = WaitForSingleObject(present, 0) == WAIT_OBJECT_0;
    CloseHandle(present);
    return found;
}

DWORD __wrap_w_os_ensure_shell(void) {
    HANDLE restored = named_event(L"WINWM_TEST_HANDOFF_RESTORED", EVENT_MODIFY_STATE);
    if (!restored) return ERROR_INVALID_HANDLE;
    SetEvent(restored);
    CloseHandle(restored);
    return ERROR_SUCCESS;
}

typedef HHOOK (WINAPI *install_hook_fn)(int, HOOKPROC, HINSTANCE, DWORD);
extern install_hook_fn __real___imp_SetWindowsHookExW;

typedef HWND (WINAPI *get_foreground_fn)(void);
extern get_foreground_fn __real___imp_GetForegroundWindow;
static HWND WINAPI fixture_foreground(void) {
    HWND app = FindWindowW(L"WinwmStartupFixture", NULL);
    HWND override = app ? (HWND)GetPropW(app, L"winwm.test_foreground") : NULL;
    return override ? override : __real___imp_GetForegroundWindow();
}
get_foreground_fn __wrap___imp_GetForegroundWindow = fixture_foreground;

void __real_w_desktop_event(HWND window, DWORD event, DWORD tick);
void __wrap_w_desktop_event(HWND window, DWORD event, DWORD tick) {
    HANDLE stamp = GetPropW(window, L"winwm.test_event_tick");
    if (stamp && (event == EVENT_SYSTEM_FOREGROUND || event == EVENT_OBJECT_SHOW)) {
        tick = stamp == (HANDLE)(INT_PTR)-1 ? (DWORD)desk.outputs[desk.model.output].selection_tick
                                            : (DWORD)(uintptr_t)stamp;
    }
    __real_w_desktop_event(window, event, tick);
    if (stamp && event == (DWORD)(uintptr_t)GetPropW(window, L"winwm.test_wait_event"))
        SetPropW(window, L"winwm.test_delivered", (HANDLE)1);
}

// Drive the same matcher on its real hook thread without injecting any input
// onto the user's active desktop. Only the private fixture creates this window.
static HOOKPROC keyboard_callback;
static LRESULT registered_input(void) {
    UINT count = 0;
    if (GetRegisteredRawInputDevices(NULL, &count, sizeof(RAWINPUTDEVICE)) != 0) return 0;
    if (!count) return 1;
    RAWINPUTDEVICE *devices = calloc(count, sizeof(*devices));
    if (!devices) return 0;
    UINT copied = GetRegisteredRawInputDevices(devices, &count, sizeof(*devices));
    int mask = 0;
    if (copied != (UINT)-1) {
        for (UINT i = 0; i < copied; ++i) {
            if (devices[i].usUsagePage != 1) continue;
            if (devices[i].usUsage == 2) mask |= 1;
            if (devices[i].usUsage == 6) mask |= 2;
            if (!devices[i].usUsage && (devices[i].dwFlags & RIDEV_PAGEONLY)) mask |= 3;
        }
    }
    free(devices);
    return copied == (UINT)-1 ? 0 : 1 + mask;
}

static LRESULT CALLBACK fixture_chord(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_APP + 2) return registered_input();
    if (msg == WM_APP + 3) {
        // New unbound input after a workspace switch, without injecting any
        // hardware events into another desktop or dispatching a winwm command.
        KBDLLHOOKSTRUCT key = {.vkCode = VK_F23, .scanCode = MapVirtualKeyW(VK_F23, MAPVK_VK_TO_VSC),
                               .time = GetTickCount()};
        keyboard_callback(HC_ACTION, WM_KEYDOWN, (LPARAM)&key);
        key.flags = LLKHF_UP;
        keyboard_callback(HC_ACTION, WM_KEYUP, (LPARAM)&key);
        return TRUE;
    }
    if (msg != WM_APP + 1) return DefWindowProcW(hwnd, msg, wp, lp);
    DWORD keys[] = {VK_LWIN, (DWORD)wp, (DWORD)wp, VK_LWIN};
    for (int i = 0; i < 4; ++i) {
        KBDLLHOOKSTRUCT key = {.vkCode = keys[i], .scanCode = MapVirtualKeyW(keys[i], MAPVK_VK_TO_VSC),
                               .time = GetTickCount(), .flags = i >= 2 ? LLKHF_UP : 0};
        if (!keyboard_callback(HC_ACTION, i >= 2 ? WM_KEYUP : WM_KEYDOWN, (LPARAM)&key)) return FALSE;
    }
    return TRUE;
}

static HHOOK WINAPI fixture_install_hook(int type, HOOKPROC callback, HINSTANCE module, DWORD thread) {
    if (type == WH_KEYBOARD_LL) {
        HANDLE unavailable = named_event(L"WINWM_TEST_INPUT_UNAVAILABLE", SYNCHRONIZE);
        bool failed = unavailable && WaitForSingleObject(unavailable, 0) == WAIT_OBJECT_0;
        if (unavailable) CloseHandle(unavailable);
        if (failed) { SetLastError(ERROR_ACCESS_DENIED); return NULL; }
    }
    HHOOK hook = __real___imp_SetWindowsHookExW(type, callback, module, thread);
    if (hook && type == WH_KEYBOARD_LL) {
        WNDCLASSW wc = {.lpfnWndProc = fixture_chord, .hInstance = module, .lpszClassName = L"WinwmFixtureKeyboard"};
        RegisterClassW(&wc);
        keyboard_callback = callback;
        CreateWindowExW(0, wc.lpszClassName, NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, module, NULL);
    }
    return hook;
}

install_hook_fn __wrap___imp_SetWindowsHookExW = fixture_install_hook;
