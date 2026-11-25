#include "theme/theme.h"
#include "shell_host/entry.h"
#include "win_platform/win_platform.h"
#include <wchar.h>
#include <stdio.h>

////////////////
//~ Shell Host

static HANDLE child;
static HWND panel;
static bool snap_suspended, backdrop_changed;
static COLORREF previous_backdrop;
static const COLORREF backdrop = RGB(9, 10, 13);
static const int backdrop_index = COLOR_DESKTOP;

////////////////
//~ Shell Host Functions

static void w_restore_session_preferences(void) {
    if (backdrop_changed && GetSysColor(backdrop_index) == backdrop)
        SetSysColors(1, &backdrop_index, &previous_backdrop);
    backdrop_changed = false;
    if (snap_suspended) {
        SystemParametersInfoW(SPI_SETWINARRANGING, 0, (void *)(UINT_PTR)TRUE, 0);
        snap_suspended = false;
    }
}

static bool ending, rescue_requested, preview;
enum { first_recovery_id = 101, exit_preview_id = 109, recovery_row = 36 };

static const wchar_t *const labels[] = {
    L"&Restart winwm",        L"Open &terminal", L"&Edit configuration",   L"Use built-in &defaults",
    L"Use &previous release", L"&Sign out",      L"Restore sign-in &shell"};
static bool w_launch_child(const wchar_t *exe) {
    child = w_os_launch(exe, preview ? L"--preview-session" : L"--session");
    if (child) {
        // Supply the shell desktop color when no wallpaper is configured.
        if (!preview) {
            previous_backdrop = GetSysColor(backdrop_index);
            if (previous_backdrop != backdrop)
                backdrop_changed = SetSysColors(1, &backdrop_index, &backdrop) != 0;
        }
        BOOL enabled = FALSE;
        if (!preview && SystemParametersInfoW(SPI_GETWINARRANGING, 0, &enabled, 0) && enabled)
            snap_suspended = SystemParametersInfoW(SPI_SETWINARRANGING, 0, NULL, 0) != 0;
        ShowWindow(panel, SW_HIDE);
        return true;
    }
    return false;
}

static bool w_start_child(void) {
    wchar_t exe[os_path_cap];
    return w_os_self(exe, count_of(exe)) && w_launch_child(exe);
}

static bool w_start_managed_previous(void) {
    wchar_t record[os_path_cap], releases[os_path_cap], exe[os_path_cap];
    if (!w_os_data_path(record, count_of(record), L"previous.txt", false) ||
        !w_os_data_path(releases, count_of(releases), L"releases\\", false))
        return false;
    FILE *f = _wfopen(record, L"rb");
    if (!f)
        return false;
    char path[8192];
    size_t size = fread(path, 1, sizeof(path) - 1, f);
    bool complete = !ferror(f) && feof(f);
    fclose(f);
    path[size] = 0;
    if (!complete || !w_os_provider(path, exe, count_of(exe)) || _wcsnicmp(exe, releases, wcslen(releases)))
        return false;
    const wchar_t *file_name = wcsrchr(exe, L'\\');
    return file_name && !_wcsicmp(file_name + 1, L"winwm.exe") && w_launch_child(exe);
}

////////////////
//~ Session recovery

static void w_create_recovery_controls(void) {
    HINSTANCE instance = GetModuleHandleW(NULL);
    for (int i = 0; i < (int)count_of(labels); ++i) {
        int id = first_recovery_id + i;
        if (!GetDlgItem(panel, id))
            CreateWindowW(L"BUTTON", labels[i], WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 14,
                          12 + recovery_row * i, 290, 30, panel, (HMENU)(INT_PTR)id, instance, NULL);
    }
    if (preview && !GetDlgItem(panel, exit_preview_id))
        CreateWindowW(L"BUTTON", L"E&xit preview", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 14,
                      12 + recovery_row * count_of(labels), 290, 30, panel, (HMENU)exit_preview_id, instance,
                      NULL);
}

static void w_rescue(void) {
    w_restore_session_preferences();
    w_os_area_restore();
    w_os_restore_all();
    w_create_recovery_controls();
    ShowWindow(panel, SW_SHOW);
    SetForegroundWindow(panel);
    SetFocus(GetDlgItem(panel, 101));
    w_os_mark(L"rescue_visible");
}

static void w_handle_action(int id) {
    if (id == 101) {
        if (!w_start_child())
            MessageBoxW(panel, L"The selected winwm executable could not start.", L"Recovery", MB_OK);
    } else if (id == 102)
        w_os_launch_terminal("%SystemRoot%\\System32\\cmd.exe", false);
    else if (id == 103) {
        wchar_t path[os_path_cap];
        char arg[8192];
        if (w_os_config_path(path, count_of(path)) && w_os_utf8(path, arg, sizeof(arg)))
            w_os_launch_provider("%SystemRoot%\\System32\\notepad.exe", arg, false);
    } else if (id == 104) {
        wchar_t path[os_path_cap], backup[os_path_cap + 64];
        if (!w_os_config_path(path, count_of(path)))
            return;
        swprintf(backup, count_of(backup), L"%ls.recovery.%llu", path, (unsigned long long)GetTickCount64());
        if (!MoveFileW(path, backup) && GetLastError() != ERROR_FILE_NOT_FOUND &&
            GetLastError() != ERROR_PATH_NOT_FOUND) {
            MessageBoxW(panel, L"Could not preserve the old configuration.", L"Recovery", MB_OK);
            return;
        }
        if (!w_os_config_seed(false)) {
            MessageBoxW(panel, L"Could not create a fresh configuration.", L"Recovery", MB_OK);
            return;
        }
        w_os_cache_discard();
        w_start_child();
    } else if (id == 105 || id == 107) {
        wchar_t exe[os_path_cap];
        const wchar_t *name = L"winwm_install.exe";
        wchar_t *file = w_os_self(exe, count_of(exe)) ? wcsrchr(exe, L'\\') : NULL;
        HANDLE setup = NULL;
        if (file && (size_t)(file - exe) + 2 + wcslen(name) <= count_of(exe)) {
            wcscpy(file + 1, name);
            setup = w_os_launch(exe, id == 105 ? L"/previous" : L"/restore");
        }
        if (setup)
            CloseHandle(setup);
        else if (id != 105 || !w_start_managed_previous())
            MessageBoxW(panel,
                        id == 105 ? L"No previous winwm release is available."
                                  : L"Run winwm_install.exe /uninstall from the release folder.",
                        L"winwm recovery", MB_OK);
    } else if (id == exit_preview_id && preview) {
        PostQuitMessage(0);
    } else if (id == 106)
        ExitWindowsEx(EWX_LOGOFF, 0);
}

////////////////
//~ Host messages

static LRESULT CALLBACK w_host_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    LRESULT color_result;
    if (w_theme_message(w, msg, wp, lp, &color_result))
        return color_result;
    switch (msg) {
    case WM_COMMAND:
        if (!child)
            w_handle_action(LOWORD(wp));
        return 0;
    case WM_HOTKEY:
        if (child) {
            // Closing the owned desktop process runs its normal cleanup. A hung
            // process is handled by the independent recovery action below.
            DWORD pid = GetProcessId(child);
            HWND h = FindWindowW(L"WinwmEvents", NULL);
            DWORD owner = 0;
            if (h)
                GetWindowThreadProcessId(h, &owner);
            rescue_requested = true;
            if (owner == pid)
                PostMessageW(h, WM_CLOSE, 0, 0);
            SetTimer(w, 1, 2000, NULL);
        } else
            w_rescue();
        return 0;
    case WM_TIMER:
        KillTimer(w, 1);
        if (child && rescue_requested && WaitForSingleObject(child, 0) == WAIT_TIMEOUT)
            TerminateProcess(child, 42);
        return 0;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (wp) {
            ending = true;
            PostQuitMessage(0);
        }
        return 0;
    case WM_CLOSE:
        if (!child)
            SetFocus(GetDlgItem(w, 101));
        return 0;
    default:
        return DefWindowProcW(w, msg, wp, lp);
    }
}

int w_host_run(HINSTANCE inst, bool preview_mode) {
    preview = preview_mode;
    w_os_instance_lock lock;
    DWORD error = w_os_instance_claim(os_host_instance, &lock);
    if (error)
        return (int)error;
    error = w_os_instance_status(os_interactive_instance);
    if (error) {
        w_os_instance_release(&lock);
        return (int)error;
    }
    w_os_mark(L"host_entry");
    WNDCLASSW wc = {.lpfnWndProc = w_host_proc,
                    .hInstance = inst,
                    .lpszClassName = L"WinwmHost",
                    .hCursor = LoadCursorW(NULL, IDC_ARROW),
                    .hbrBackground = NULL};
    RegisterClassW(&wc);
    panel = CreateWindowExW(WS_EX_TOPMOST | WS_EX_CONTROLPARENT, wc.lpszClassName, L"winwm recovery",
                            WS_CAPTION | WS_SYSMENU, 100, 100, 338,
                            74 + recovery_row * ((int)count_of(labels) + preview), NULL, NULL, inst, NULL);
    if (!panel) {
        w_os_instance_release(&lock);
        return 3;
    }
    if (!RegisterHotKey(panel, 1, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F12))
        SetPropW(panel, L"winwm.recovery_hotkey_error", (HANDLE)(uintptr_t)GetLastError());
    if (!preview)
        w_os_area_open(true);
    if (!w_start_child())
        w_rescue();
    while (!ending) {
        DWORD n = child ? 1 : 0;
        DWORD wait =
            MsgWaitForMultipleObjectsEx(n, child ? &child : NULL, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (wait == WAIT_FAILED)
            break;
        if (n && wait == WAIT_OBJECT_0) {
            DWORD code = 0;
            GetExitCodeProcess(child, &code);
            SetPropW(panel, L"winwm.child_exit", (HANDLE)(uintptr_t)((uint64_t)code + 1));
            CloseHandle(child);
            child = NULL;
            rescue_requested = false;
            KillTimer(panel, 1);
            w_rescue();
        }
        MSG m;
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) {
                ending = true;
                break;
            }
            if (!IsDialogMessageW(panel, &m)) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
        }
    }
    if (child)
        CloseHandle(child);
    w_restore_session_preferences();
    w_os_area_close();
    w_os_instance_release(&lock);
    return 0;
}
