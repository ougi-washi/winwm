#include "theme/theme.h"
#include "shell_host/entry.h"
#include "win_platform/win_platform.h"
#include <shellapi.h>
#include <wchar.h>
#include <stdio.h>

////////////////
//~ Shell Host

static HANDLE session;
static HWND panel;
static wchar_t session_exe[os_path_cap], restart_exe[os_path_cap];

////////////////
//~ Shell Host Functions

static bool ending, rescue_requested, quit_requested, preview, logging_off;
static bool owns_desktop, waiting_for_shell;
enum { first_recovery_id = 101, exit_windows_id = 109, recovery_row = 36 };

static const wchar_t *const labels[] = {
    L"&Restart winwm",        L"Open &terminal", L"&Edit configuration",   L"Use built-in &defaults",
    L"Use &previous release", L"&Sign out",      L"Remove winwm from &startup"};
static bool w_restart(const wchar_t *exe) {
    if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) return false;
    wcscpy(restart_exe,exe);
    PostQuitMessage(0);
    return true;
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
    if (!file_name || _wcsicmp(file_name + 1, L"winwm.exe")) return false;
    // Validate the release's recovery companion and configuration before switching.
    wchar_t command[os_path_cap+32];
    swprintf(command,count_of(command),L"\"%ls\" ctl validate-startup",exe);
    STARTUPINFOW si = {.cb=sizeof(si)};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(exe,command,NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,NULL,&si,&pi)) return false;
    CloseHandle(pi.hThread);
    DWORD code = ERROR_TIMEOUT;
    if (WaitForSingleObject(pi.hProcess,15000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess,&code);
    CloseHandle(pi.hProcess);
    return !code && w_restart(exe);
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
    if (!GetDlgItem(panel, exit_windows_id))
        CreateWindowW(L"BUTTON", L"E&xit to Windows", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 14,
                      12 + recovery_row * count_of(labels), 290, 30, panel, (HMENU)exit_windows_id, instance,
                      NULL);
}

static void w_rescue(void) {
    w_create_recovery_controls();
    ShowWindow(panel, SW_SHOW);
    SetForegroundWindow(panel);
    SetFocus(GetDlgItem(panel, 101));
    w_os_mark(L"rescue_visible");
}

static void w_handle_action(int id) {
    if (id == 101) {
        if (!w_restart(session_exe))
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
        w_restart(session_exe);
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
    } else if (id == exit_windows_id) {
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
    case recovery_take_desktop: {
        if (preview || !session || quit_requested || rescue_requested || logging_off ||
            (owns_desktop && !waiting_for_shell) || (DWORD)wp != GetProcessId(session) ||
            WaitForSingleObject(session, 0) != WAIT_TIMEOUT)
            return ERROR_CANCELLED + 1;
        owns_desktop = true;
        waiting_for_shell = false;
        DWORD error = w_os_exit_shell();
        waiting_for_shell = error == ERROR_NOT_FOUND;
        return (LRESULT)error + 1;
    }
    case recovery_session_closing:
        if (session && (DWORD)wp == GetProcessId(session)) {
            if (lp == 42) rescue_requested = true;
            if (!rescue_requested) quit_requested = true;
            SetTimer(w,1,5000,NULL);
        }
        return 0;
    case WM_COMMAND:
        if (!session)
            w_handle_action(LOWORD(wp));
        return 0;
    case WM_HOTKEY:
    case WM_CLOSE:
        if (msg == WM_CLOSE) quit_requested = true;
        if (session) {
            // Closing the session runs its normal cleanup. A hung
            // process is handled by the independent recovery action below.
            DWORD pid = GetProcessId(session);
            HWND h = FindWindowW(L"WinwmEvents", NULL);
            DWORD owner = 0;
            if (h)
                GetWindowThreadProcessId(h, &owner);
            rescue_requested = !quit_requested;
            if (owner == pid)
                PostMessageW(h, WM_CLOSE, 0, 0);
            SetTimer(w, 1, 2000, NULL);
        } else if (quit_requested)
            PostQuitMessage(0);
        else
            w_rescue();
        return 0;
    case WM_TIMER:
        KillTimer(w, 1);
        if (session && (quit_requested || rescue_requested) && WaitForSingleObject(session, 0) == WAIT_TIMEOUT)
            TerminateProcess(session, rescue_requested ? 42 : ERROR_CANCELLED);
        return 0;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (wp) {
            logging_off = true;
            ending = true;
            PostQuitMessage(0);
        }
        return 0;
    default:
        return DefWindowProcW(w, msg, wp, lp);
    }
}

static int w_host_run(HINSTANCE inst, DWORD pid, uint64_t birth) {
    w_os_instance_lock lock;
    DWORD error = w_os_instance_claim(os_host_instance, &lock);
    if (error)
        return (int)error;
    if (!w_os_self(session_exe,count_of(session_exe))) { error = GetLastError(); goto done; }
    wchar_t *name = wcsrchr(session_exe,L'\\');
    if (!name || (size_t)(name+1-session_exe)+count_of(L"winwm.exe") > count_of(session_exe)) {
        error = ERROR_BAD_PATHNAME; goto done;
    }
    wcscpy(name+1,L"winwm.exe");
    session = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE|PROCESS_TERMINATE,FALSE,pid);
    if (!session) { error = GetLastError(); goto done; }
    FILETIME created, exited, kernel, user;
    wchar_t actual[os_path_cap];
    DWORD size = count_of(actual);
    if (!GetProcessTimes(session,&created,&exited,&kernel,&user) ||
        birth != (((uint64_t)created.dwHighDateTime << 32) | created.dwLowDateTime) ||
        !QueryFullProcessImageNameW(session,0,actual,&size) || _wcsicmp(actual,session_exe)) {
        error = ERROR_INVALID_OWNER; goto done;
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
                            74 + recovery_row * ((int)count_of(labels) + 1), NULL, NULL, inst, NULL);
    if (!panel || !SetPropW(panel,L"winwm.session_pid",(HANDLE)(uintptr_t)pid)) { error = GetLastError(); goto done; }
    if (!RegisterHotKey(panel, 1, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F12))
        SetPropW(panel, L"winwm.recovery_hotkey_error", (HANDLE)(uintptr_t)GetLastError());
    while (!ending) {
        DWORD n = session ? 1 : 0;
        DWORD wait =
            MsgWaitForMultipleObjectsEx(n, session ? &session : NULL, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (wait == WAIT_FAILED)
            break;
        if (n && wait == WAIT_OBJECT_0) {
            DWORD code = 0;
            GetExitCodeProcess(session, &code);
            SetPropW(panel, L"winwm.child_exit", (HANDLE)(uintptr_t)((uint64_t)code + 1));
            CloseHandle(session);
            session = NULL;
            KillTimer(panel, 1);
            w_os_restore_all();
            if (owns_desktop && !logging_off) {
                error = w_os_ensure_shell();
                owns_desktop = false;
            }
            if (!quit_requested && (rescue_requested || code == 42))
                w_rescue();
            else
                break;
            rescue_requested = false;
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
    if (owns_desktop && !logging_off) error = w_os_ensure_shell();
done:
    if (session) CloseHandle(session);
    if (panel) DestroyWindow(panel);
    w_os_instance_release(&lock);
    if (*restart_exe && !logging_off) {
        HANDLE next = w_os_launch(restart_exe,preview ? L"--preview" : NULL);
        if (!next) error = GetLastError();
        else CloseHandle(next);
    }
    w_os_wait_layer_cleanup();
    return (int)error;
}

int WINAPI wWinMain(HINSTANCE inst,HINSTANCE previous,PWSTR args,int show) {
    (void)previous; (void)args; (void)show;
    int argc;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(),&argc);
    if (!argv) return (int)GetLastError();
    int result = ERROR_INVALID_PARAMETER;
    if (argc == 2 && !wcscmp(argv[1],WINWM_RECOVERY_CHECK))
        result = ERROR_SUCCESS;
    else if (argc == 2) {
        const wchar_t *arg = argv[1];
        bool launch = !wcsncmp(arg,L"/start2:",8);
        unsigned mode;
        unsigned long pid;
        unsigned long long birth;
        wchar_t extra;
        if ((launch || !wcsncmp(arg,L"/watch2:",8)) &&
            swscanf(arg+8,L"%u:%lu:%llu%lc",&mode,&pid,&birth,&extra) == 3 && mode <= 1 && pid && birth) {
            if (launch) {
                wchar_t self[os_path_cap], watch[96];
                swprintf(watch,count_of(watch),L"/watch2:%u:%lu:%llu",mode,pid,birth);
                HANDLE monitor = w_os_self(self,count_of(self)) ? w_os_launch(self,watch) : NULL;
                result = monitor ? 0 : (int)GetLastError();
                if (monitor) CloseHandle(monitor);
            } else {
                preview = mode != 0;
                SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
                result = w_host_run(inst,pid,birth);
            }
        }
    }
    LocalFree(argv);
    return result;
}
