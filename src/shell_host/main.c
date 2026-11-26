#include "theme/dialog.h"
#include "theme/caption.h"
#include "theme/icon_button.h"
#include "theme/theme.h"
#include "shell_host/entry.h"
#include "install/install.h"
#include "native_notifications/toast.h"
#include "os_win32/environment.h"
#include "win_platform/win_platform.h"
#include <shellapi.h>
#include <shellscalingapi.h>
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
enum { first_recovery_id = 101, exit_windows_id = 109 };

static const wchar_t *const labels[] = {
    L"&Restart winwm",        L"Open &terminal", L"&Edit configuration",   L"Use built-in &defaults",
    L"Use &previous release", L"&Sign out",      L"Re&move winwm from startup", L"E&xit to Windows"};
static const wchar_t *const glyphs[] = {L"\uE72C", L"\uE756", L"\uE70F", L"\uE777",
                                       L"\uE72B", L"\uE8AC", L"\uE74D", L"\uE8A9"};
static HFONT recovery_font;
static UINT recovery_dpi;
static bool w_restart(const wchar_t *exe) {
    if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) return false;
    wcscpy(restart_exe,exe);
    PostQuitMessage(0);
    return true;
}

static DWORD w_check_previous(const wchar_t *exe,const wchar_t *args) {
    wchar_t command[os_path_cap+32];
    swprintf(command,count_of(command),L"\"%ls\" %ls",exe,args);
    STARTUPINFOW si = {.cb=sizeof(si)};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(exe,command,NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,NULL,&si,&pi)) return ERROR_GEN_FAILURE;
    CloseHandle(pi.hThread);
    DWORD code = ERROR_TIMEOUT;
    if (WaitForSingleObject(pi.hProcess,15000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess,&code);
    CloseHandle(pi.hProcess);
    return code;
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
    // Ask the previous release to validate its own startup requirements.
    DWORD code = w_check_previous(exe,L"validate-startup");
    // Releases predating direct commands return 2 for unknown CLI syntax.
    if (code == 2) code = w_check_previous(exe,L"ctl validate-startup");
    return !code && w_restart(exe);
}

////////////////
//~ Session recovery

static int w_recovery_id(int index) {
    return index == count_of(labels)-1 ? exit_windows_id : first_recovery_id + index;
}

static void w_fit_recovery(HWND w, const RECT *anchor, HMONITOR monitor) {
    MONITORINFO mi = {.cbSize = sizeof(mi)};
    if (!GetMonitorInfoW(monitor, &mi)) return;
    RECT work = mi.rcWork, frame = {0};
    UINT dpi = 96, dpi_y;
    if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi, &dpi_y))) dpi = GetDpiForWindow(w);
    int side = w_theme_button_size(dpi), gap = w_theme_spacing(dpi), pad = gap * 2;
    w_theme_adjust_rect(&frame, (DWORD)GetWindowLongPtrW(w, GWL_STYLE),
                        (DWORD)GetWindowLongPtrW(w, GWL_EXSTYLE), dpi);
    int available = work.bottom - work.top - (frame.bottom - frame.top) - 2 * pad;
    int rows = w_clamp_int((available + gap) / (side + gap), 1, (int)count_of(labels));
    int columns = ((int)count_of(labels) + rows - 1) / rows;
    rows = ((int)count_of(labels) + columns - 1) / columns;
    HFONT old = NULL;
    if (!recovery_font || dpi != recovery_dpi) {
        HFONT next = w_theme_message_font(dpi);
        if (next) {
            old = recovery_font;
            recovery_font = next;
            recovery_dpi = dpi;
        }
    }
    HFONT font = recovery_font ? recovery_font : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    int column_width[count_of(labels)]={0}, actions_width=0;
    for (int i=0;i<(int)count_of(labels);++i) {
        int preferred=w_labeled_button_width(font,labels[i],dpi);
        column_width[i/rows]=max(column_width[i/rows],preferred);
    }
    for (int i=0;i<columns;++i) actions_width+=column_width[i];
    int title_width=w_theme_caption_width(w);
    int width=max(title_width,actions_width+(columns-1)*gap+2*pad)+frame.right-frame.left;
    int height = rows * side + (rows-1)*gap + 2*pad + frame.bottom-frame.top;
    width = min(width, work.right-work.left);
    height = min(height, work.bottom-work.top);
    if (!anchor) anchor = &work;
    int x = w_clamp_int((anchor->left+anchor->right-width)/2, work.left, work.right-width);
    int y = w_clamp_int((anchor->top+anchor->bottom-height)/2, work.top, work.bottom-height);
    SetWindowPos(w,NULL,x,y,width,height,SWP_NOACTIVATE | SWP_NOZORDER);
    RECT client;
    w_theme_content_rect(w,&client);
    int content_top = w_theme_caption_height(w);
    int room=max(1,client.right-2*pad-(columns-1)*gap);
    int sum=0,left=0;
    for (int col=0;col<columns;++col) {
        sum+=column_width[col];
        int right=MulDiv(room,sum,actions_width);
        for (int row=0;row<rows && col*rows+row<(int)count_of(labels);++row) {
            HWND button=GetDlgItem(w,w_recovery_id(col*rows+row));
            if (!button) continue;
            SendMessageW(button,WM_SETFONT,(WPARAM)font,FALSE);
            SetWindowPos(button,NULL,pad+left+col*gap,content_top+pad+row*(side+gap),max(1,right-left),side,
                         SWP_NOACTIVATE|SWP_NOZORDER);
        }
        left=right;
    }
    if (old) DeleteObject(old);
}

static void w_rescue(void) {
    w_theme_load_profile();
    w_theme_material(panel);
    for (int i = 0; i < (int)count_of(labels); ++i) {
        int id = w_recovery_id(i);
        if (GetDlgItem(panel,id)) continue;
        HWND button = CreateWindowW(L"BUTTON", labels[i], WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                               0,0,0,0,panel,(HMENU)(INT_PTR)id,GetModuleHandleW(NULL),NULL);
        w_labeled_button(button,glyphs[i],labels[i]);
    }
    w_fit_recovery(panel,NULL,MonitorFromWindow(GetForegroundWindow(),MONITOR_DEFAULTTOPRIMARY));
    ShowWindow(panel, SW_SHOW);
    SetForegroundWindow(panel);
    SetFocus(GetDlgItem(panel, 101));
    w_os_mark(L"rescue_visible");
}

static bool w_reset_profile(void) {
    wchar_t paths[2][os_path_cap], backups[2][os_path_cap+64];
    bool moved[2] = {false};
    if (!w_os_config_path(paths[0],count_of(paths[0])) || !w_os_toolbar_path(paths[1],count_of(paths[1])))
        return false;
    ULONGLONG stamp = GetTickCount64();
    for (int i=0; i<2; ++i) {
        swprintf(backups[i],count_of(backups[i]),L"%ls.recovery.%llu",paths[i],stamp);
        moved[i] = MoveFileW(paths[i],backups[i]) != FALSE;
        if (!moved[i]) {
            DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) goto restore;
        }
    }
    if (w_os_config_seed()) return true;
restore:
    for (int i=0; i<2; ++i) if (moved[i]) MoveFileW(backups[i],paths[i]);
    return false;
}

static void w_handle_action(int id) {
    if (id == 101) {
        if (!w_restart(session_exe))
            w_dialog(panel, L"The selected winwm executable could not start.", L"Recovery", MB_OK);
    } else if (id == 102)
        w_os_launch_terminal("%SystemRoot%\\System32\\cmd.exe", false);
    else if (id == 103) {
        wchar_t path[os_path_cap];
        char arg[8192];
        if (w_os_config_path(path, count_of(path)) && w_os_utf8(path, arg, sizeof(arg)))
            w_os_launch_provider("%SystemRoot%\\System32\\notepad.exe", arg, false);
    } else if (id == 104) {
        if (!w_reset_profile()) {
            w_dialog(panel, L"Could not back up and reset the profile.", L"Recovery", MB_OK);
            return;
        }
        w_os_cache_discard();
        w_restart(session_exe);
    } else if (id == 105 || id == 107) {
        wchar_t current[os_path_cap];
        if (id == 105) {
            DWORD error = w_install_current(current);
            if ((error == ERROR_ALREADY_ASSIGNED || (!error && !*current)) && w_start_managed_previous()) return;
        }
        w_install_run(id == 105 ? op_rollback : op_uninstall,true);
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
    case WM_FONTCHANGE:
        recovery_dpi=0;
        /* fall through */
    case WM_SETTINGCHANGE:
        if (msg==WM_SETTINGCHANGE && wp!=SPI_SETNONCLIENTMETRICS && wp!=SPI_SETWORKAREA) break;
        if (wp==SPI_SETNONCLIENTMETRICS) recovery_dpi=0;
        /* fall through */
    case winwm_theme_updated:
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED: {
        if (!GetDlgItem(w, first_recovery_id)) break;
        RECT r;
        if (msg == WM_DPICHANGED) r = *(RECT *)lp;
        else GetWindowRect(w, &r);
        w_fit_recovery(w, &r, MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST));
        return 0;
    }
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
        if (!session && LOWORD(wp) == IDCANCEL)
            PostMessageW(w, WM_CLOSE, 0, 0);
        else if (!session)
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
    }
    return DefWindowProcW(w, msg, wp, lp);
}

static int w_host_run(HINSTANCE inst, DWORD pid, uint64_t birth, HANDLE ready) {
    w_os_instance_lock lock;
    DWORD error = w_os_instance_claim(os_host_instance, &lock);
    if (error)
        return (int)error;
    if (!w_os_self(session_exe,count_of(session_exe))) { error = GetLastError(); goto done; }
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
                            WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, 0, 0, 1, 1, NULL, NULL, inst, NULL);
    if (!panel || !SetPropW(panel,L"winwm.session_pid",(HANDLE)(uintptr_t)pid)) { error = GetLastError(); goto done; }
    if (!RegisterHotKey(panel, 1, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F12))
        SetPropW(panel, L"winwm.recovery_hotkey_error", (HANDLE)(uintptr_t)GetLastError());
    if (!SetEvent(ready)) { error = GetLastError(); goto done; }
    w_theme_follow(panel);
    w_environment_start(NULL,0);
    while (!ending) {
        DWORD n = session ? 1 : 0;
        DWORD wait =
            MsgWaitForMultipleObjectsEx(n, session ? &session : NULL, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (wait == WAIT_FAILED)
            break;
        if (n && wait == WAIT_OBJECT_0) {
            DWORD code = 0;
            FILETIME born, exited, kernel, user;
            if (GetProcessTimes(session,&born,&exited,&kernel,&user))
                w_notification_submit_recover(GetProcessId(session),((uint64_t)born.dwHighDateTime << 32) | born.dwLowDateTime);
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
    if (recovery_font) DeleteObject(recovery_font);
    w_os_instance_release(&lock);
    if (*restart_exe && !logging_off) {
        HANDLE next = w_os_launch(restart_exe,preview ? L"--preview" : NULL);
        if (!next) error = GetLastError();
        else CloseHandle(next);
    }
    w_os_wait_layer_cleanup();
    w_environment_stop();
    return (int)error;
}

static DWORD w_start_watch(const wchar_t *name, unsigned mode, DWORD pid, uint64_t birth) {
    wchar_t self[os_path_cap], watch[96];
    if (!w_os_self(self,count_of(self))) return GetLastError();
    swprintf(watch,count_of(watch),L"--recovery-watch:%u:%lu:%llu",mode,pid,(unsigned long long)birth);
    HANDLE ready = CreateEventW(NULL, TRUE, FALSE, name);
    if (!ready) return GetLastError();
    DWORD error = GetLastError();
    if (error != ERROR_ALREADY_EXISTS) {
        HANDLE monitor = w_os_launch(self,watch);
        if (monitor) {
            HANDLE waits[] = {monitor,ready};
            DWORD wait = WaitForMultipleObjects(2,waits,FALSE,10000), code;
            if (wait == WAIT_OBJECT_0 + 1) error = ERROR_SUCCESS;
            else if (wait == WAIT_OBJECT_0 && GetExitCodeProcess(monitor,&code))
                error = code ? code : ERROR_PROCESS_ABORTED;
            else error = wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
            CloseHandle(monitor);
        } else error = GetLastError();
    }
    CloseHandle(ready);
    return error;
}

int w_recovery_run(HINSTANCE inst, const wchar_t *operation) {
    bool launch = !wcsncmp(operation, L"start:", 6);
    unsigned mode;
    unsigned long pid;
    unsigned long long birth;
    wchar_t extra;
    if ((!launch && wcsncmp(operation, L"watch:", 6)) ||
        swscanf(operation + 6, L"%u:%lu:%llu%lc", &mode, &pid, &birth, &extra) != 3 ||
        mode > 1 || !pid || !birth)
        return ERROR_INVALID_PARAMETER;
    wchar_t name[96];
    swprintf(name, count_of(name), L"Local\\winwm.recovery.ready.%lu.%llu", pid, birth);
    if (launch)
        return (int)w_start_watch(name, mode, pid, birth);
    HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
    if (!ready) return (int)GetLastError();
    preview = mode != 0;
    int result = w_host_run(inst, pid, birth, ready);
    CloseHandle(ready);
    return result;
}
