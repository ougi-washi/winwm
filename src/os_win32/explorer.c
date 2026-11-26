#include "os_win32/os_win32.h"
#include "os_win32/environment.h"
#include <restartmanager.h>
#include <tlhelp32.h>
#include <wchar.h>

static bool w_named_user_object(HANDLE object, const wchar_t *expected) {
    wchar_t name[64];
    DWORD bytes;
    return GetUserObjectInformationW(object,UOI_NAME,name,sizeof(name),&bytes) && !_wcsicmp(name,expected);
}

DWORD w_os_shell_desktop(void) {
    if (GetSystemMetrics(SM_SHUTTINGDOWN)) return ERROR_SHUTDOWN_IN_PROGRESS;
    if (!w_named_user_object(GetProcessWindowStation(),L"WinSta0") ||
        !w_named_user_object(GetThreadDesktop(GetCurrentThreadId()),L"Default"))
        return ERROR_REQUIRES_INTERACTIVE_WINDOWSTATION;
    return ERROR_SUCCESS;
}

static DWORD w_explorer_path(wchar_t directory[MAX_PATH], wchar_t exe[MAX_PATH]) {
    UINT n = GetSystemWindowsDirectoryW(directory,MAX_PATH);
    if (!n) return GetLastError();
    if (n+count_of(L"\\explorer.exe") > MAX_PATH) return ERROR_FILENAME_EXCED_RANGE;
    wcscpy(exe,directory);
    wcscat(exe,L"\\explorer.exe");
    return ERROR_SUCCESS;
}

static DWORD w_window_pid(HWND window) {
    DWORD pid = 0;
    if (window) GetWindowThreadProcessId(window,&pid);
    return pid;
}

static HWND w_shell_tray(DWORD pid) {
    HWND tray = NULL;
    while ((tray = FindWindowExW(GetDesktopWindow(),tray,L"Shell_TrayWnd",NULL)))
        if (w_window_pid(tray) == pid) return tray;
    return NULL;
}

typedef struct {
    TOKEN_USER user;
    BYTE sid[SECURITY_MAX_SID_SIZE];
} w_token_user;

static DWORD w_process_user(HANDLE process, w_token_user *user) {
    HANDLE token;
    DWORD size;
    if (!OpenProcessToken(process,TOKEN_QUERY,&token)) return GetLastError();
    DWORD error = GetTokenInformation(token,TokenUser,user,sizeof(*user),&size) ? ERROR_SUCCESS : GetLastError();
    CloseHandle(token);
    return error;
}

static DWORD w_check_explorer(HANDLE process, DWORD pid, const wchar_t *expected) {
    DWORD session, own_session;
    if (!ProcessIdToSessionId(pid,&session) || !ProcessIdToSessionId(GetCurrentProcessId(),&own_session))
        return GetLastError();
    if (session != own_session) return ERROR_INVALID_OWNER;
    wchar_t image[os_path_cap];
    DWORD size = count_of(image);
    if (!QueryFullProcessImageNameW(process,0,image,&size)) return GetLastError();
    if (_wcsicmp(image,expected)) return ERROR_INVALID_OWNER;
    w_token_user own, other;
    DWORD error = w_process_user(GetCurrentProcess(),&own);
    if (!error) error = w_process_user(process,&other);
    if (error) return error;
    return EqualSid(own.user.User.Sid,other.user.User.Sid) ? ERROR_SUCCESS : ERROR_INVALID_OWNER;
}

// The desktop and primary taskbar can appear or disappear independently.
// Only their current owner on this desktop can be a shutdown target.
typedef struct {
    HWND window;
    DWORD pid;
    HANDLE process;
    bool remnant;
} w_explorer;

static bool w_only_thread(DWORD pid, DWORD tid) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    THREADENTRY32 entry = {.dwSize = sizeof(entry)};
    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID != pid) continue;
            if (entry.th32ThreadID != tid) { CloseHandle(snapshot); return false; }
            found = true;
        } while (Thread32Next(snapshot, &entry));
        if (GetLastError() != ERROR_NO_MORE_FILES) found = false;
    }
    CloseHandle(snapshot);
    return found;
}

static bool w_shell_remnant(const w_explorer *shell, DWORD tid) {
    // A shell can drop its desktop and tray and leave one adapter thread
    // behind. Give a newly launched Explorer time to open its folder first.
    FILETIME born, exit_time, kernel, user, now;
    if (!GetProcessTimes(shell->process, &born, &exit_time, &kernel, &user)) return false;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER start = {.LowPart=born.dwLowDateTime, .HighPart=born.dwHighDateTime};
    ULARGE_INTEGER current = {.LowPart=now.dwLowDateTime, .HighPart=now.dwHighDateTime};
    if (current.QuadPart < start.QuadPart || current.QuadPart - start.QuadPart < 100000000ULL) return false;
    HWND root = NULL;
    while ((root = FindWindowExW(GetDesktopWindow(), root, NULL, NULL)))
        if (w_window_pid(root) == shell->pid) return false;
    HWND worker = NULL;
    while ((worker = FindWindowExW(HWND_MESSAGE, worker, L"WorkerW", NULL)))
        if (GetWindowThreadProcessId(worker, NULL) == tid) return w_only_thread(shell->pid, tid);
    return false;
}

static bool w_still_shell(const w_explorer *shell) {
    DWORD pid = 0;
    DWORD tid = GetWindowThreadProcessId(shell->window, &pid);
    wchar_t desktop[128];
    DWORD bytes;
    if (!tid || pid != shell->pid ||
        !GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, desktop, sizeof(desktop), &bytes) ||
        !w_named_user_object(GetThreadDesktop(tid), desktop)) return false;
    if (!shell->remnant) return GetShellWindow() == shell->window || w_shell_tray(pid) == shell->window;
    HWND adapter = NULL;
    while ((adapter = FindWindowExW(HWND_MESSAGE, adapter, L"SystemUserAdapterWindowClass", NULL)))
        if (adapter == shell->window) return w_shell_remnant(shell, tid);
    return false;
}

static DWORD w_open_explorer(HWND window, const wchar_t *exe, bool stopping, bool remnant, w_explorer *shell) {
    DWORD pid = w_window_pid(window);
    if (!pid || pid == GetCurrentProcessId()) return ERROR_NOT_FOUND;
    DWORD rights = PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE;
    HANDLE process = stopping ? OpenProcess(rights | PROCESS_TERMINATE, FALSE, pid) : NULL;
    if (!process) process = OpenProcess(rights, FALSE, pid);
    if (!process) {
        DWORD error = GetLastError();
        return w_window_pid(window) == pid ? error : ERROR_RETRY;
    }
    DWORD error = w_check_explorer(process, pid, exe);
    if (!error) {
        *shell = (w_explorer){window, pid, process, remnant};
        if (w_still_shell(shell)) return ERROR_SUCCESS;
        error = remnant ? ERROR_NOT_FOUND : ERROR_RETRY;
    }
    CloseHandle(process);
    return error;
}

static DWORD w_find_explorer(const wchar_t *exe, bool tray_only, bool stopping, w_explorer *shell) {
    DWORD unresolved = ERROR_NOT_FOUND;
    HWND window = tray_only ? NULL : GetShellWindow();
    if (window) {
        DWORD error = w_open_explorer(window, exe, stopping, false, shell);
        if (!error) return ERROR_SUCCESS;
        if (error != ERROR_INVALID_OWNER && error != ERROR_NOT_FOUND) unresolved = error;
    }
    HWND tray = NULL;
    while ((tray = FindWindowExW(GetDesktopWindow(), tray, L"Shell_TrayWnd", NULL))) {
        if (tray == window) continue;
        DWORD error = w_open_explorer(tray, exe, stopping, false, shell);
        if (!error) return ERROR_SUCCESS;
        if (error != ERROR_INVALID_OWNER && error != ERROR_NOT_FOUND) unresolved = error;
    }
    if (!tray_only) {
        HWND adapter = NULL;
        while ((adapter = FindWindowExW(HWND_MESSAGE, adapter, L"SystemUserAdapterWindowClass", NULL))) {
            if (!w_open_explorer(adapter, exe, stopping, true, shell)) return ERROR_SUCCESS;
        }
    }
    return unresolved;
}

static bool w_explorer_present(bool tray_only) {
    wchar_t directory[MAX_PATH], exe[MAX_PATH];
    w_explorer shell;
    if (w_explorer_path(directory, exe) || w_find_explorer(exe, tray_only, false, &shell)) return false;
    CloseHandle(shell.process);
    return true;
}

bool w_os_explorer_shell_present(void) { return w_explorer_present(false); }
bool w_os_explorer_tray_present(void) { return w_explorer_present(true); }

static DWORD w_check_shutdown(HANDLE cancel) {
    if (GetSystemMetrics(SM_SHUTTINGDOWN)) return ERROR_SHUTDOWN_IN_PROGRESS;
    return cancel && WaitForSingleObject(cancel, 0) != WAIT_TIMEOUT ? ERROR_CANCELLED : ERROR_SUCCESS;
}

static DWORD w_graceful_exit(const w_explorer *shell, HANDLE cancel) {
    RM_UNIQUE_PROCESS app = {.dwProcessId = shell->pid};
    FILETIME exited, kernel, user;
    if (!GetProcessTimes(shell->process, &app.ProcessStartTime, &exited, &kernel, &user)) return GetLastError();
    DWORD session;
    wchar_t key[CCH_RM_SESSION_KEY+1] = {0};
    DWORD error = RmStartSession(&session,0,key);
    if (error) return error;
    error = RmRegisterResources(session,0,NULL,1,&app,0,NULL);
    if (!error) {
        RM_PROCESS_INFO affected;
        UINT needed = 0, count = 1;
        DWORD reboot = RmRebootReasonNone;
        error = RmGetList(session,&needed,&count,&affected,&reboot);
        if (error == ERROR_MORE_DATA) error = ERROR_INVALID_OWNER;
        if (!error && reboot != RmRebootReasonNone) error = ERROR_FAIL_NOACTION_REBOOT;
        if (!error && (count != 1 || shell->pid != affected.Process.dwProcessId ||
            CompareFileTime(&app.ProcessStartTime,&affected.Process.ProcessStartTime) != 0))
            error = ERROR_INVALID_OWNER;
    }
    if (!error) error = w_check_shutdown(cancel);
    if (!error && !w_still_shell(shell)) error = ERROR_RETRY;
    if (!error) error = RmShutdown(session, 0, NULL);
    RmEndSession(session);
    return error;
}

static DWORD w_shutdown_explorer(const w_explorer *shell, HANDLE cancel) {
    DWORD error = w_check_shutdown(cancel);
    if (error || !w_still_shell(shell)) return error ? error : ERROR_RETRY;
    // Restart Manager can wait tens of seconds on a ghosted Explorer. Give
    // its shell window an ordinary close request without blocking either UI.
    HWND hung = NULL;
    if (!shell->remnant) {
        HWND tray = w_shell_tray(shell->pid);
        if (tray && IsHungAppWindow(tray)) hung = tray;
        else if (IsHungAppWindow(shell->window)) hung = shell->window;
    }
    if (hung && w_window_pid(hung) != shell->pid) return ERROR_RETRY;
    DWORD graceful = hung ? (PostMessageW(hung, WM_CLOSE, 0, 0) ? ERROR_SUCCESS : GetLastError()) :
                            w_graceful_exit(shell, cancel);
    HANDLE handles[] = {shell->process, cancel};
    DWORD wait = WaitForMultipleObjects(cancel ? 2 : 1, handles, FALSE, graceful ? 0 : hung ? 500 : 3000);
    if (wait == WAIT_OBJECT_0) return ERROR_SUCCESS;
    if (wait == WAIT_FAILED) return GetLastError();
    error = w_check_shutdown(cancel);
    if (error) return error;
    // A former shell can keep servicing folder windows after it drops the
    // desktop. Force only the original, still-verified owner using the same
    // open process handle obtained before asking Restart Manager.
    if (!w_still_shell(shell)) return ERROR_RETRY;
    if (!TerminateProcess(shell->process, ERROR_PROCESS_ABORTED)) {
        error = GetLastError();
        if (WaitForSingleObject(shell->process, 0) != WAIT_OBJECT_0) return error;
    }
    wait = WaitForSingleObject(shell->process, 5000);
    return wait == WAIT_OBJECT_0 ? ERROR_SUCCESS : wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
}

static DWORD w_exit_explorer(const wchar_t *exe, HANDLE cancel) {
    ULONGLONG deadline = GetTickCount64() + 10000;
    bool exited = false;
    for (;;) {
        DWORD error = w_check_shutdown(cancel);
        if (error) return error;
        w_explorer shell;
        error = w_find_explorer(exe, false, true, &shell);
        if (error == ERROR_NOT_FOUND) return exited ? ERROR_SUCCESS : ERROR_NOT_FOUND;
        if (!error) {
            error = w_shutdown_explorer(&shell, cancel);
            CloseHandle(shell.process);
            if (!error) exited = true;
        }
        if (error && error != ERROR_RETRY) return error;
        if (GetTickCount64() >= deadline) {
            // The graceful request itself can take longer than the discovery
            // deadline. Confirm the final state before reporting a timeout.
            error = w_find_explorer(exe, false, false, &shell);
            if (error == ERROR_NOT_FOUND) return exited ? ERROR_SUCCESS : ERROR_NOT_FOUND;
            if (!error) CloseHandle(shell.process);
            return ERROR_TIMEOUT;
        }
        if (error == ERROR_RETRY) Sleep(10);
    }
}

DWORD w_os_exit_shell(HANDLE cancel) {
    DWORD error = w_os_shell_desktop();
    wchar_t directory[MAX_PATH], exe[MAX_PATH];
    if (!error) error = w_explorer_path(directory, exe);
    return error ? error : w_exit_explorer(exe, cancel);
}

DWORD w_os_ensure_shell(void) {
    DWORD error = w_os_shell_desktop();
    wchar_t directory[MAX_PATH], exe[MAX_PATH];
    if (!error) error = w_explorer_path(directory,exe);
    if (error) return error;
    ULONGLONG deadline = GetTickCount64()+10000;
    bool launched = false;
    for (;;) {
        if (GetSystemMetrics(SM_SHUTTINGDOWN)) return ERROR_SHUTDOWN_IN_PROGRESS;
        HWND shell = GetShellWindow();
        DWORD pid = w_window_pid(shell);
        if (pid && w_shell_tray(pid)) return ERROR_SUCCESS;
        if (GetTickCount64() >= deadline) return ERROR_TIMEOUT;
        if (!launched && !shell && !w_os_explorer_tray_present()) {
            STARTUPINFOW startup = {.cb=sizeof(startup)};
            PROCESS_INFORMATION child = {0};
            w_environment_for_launch();
            if (!CreateProcessW(exe,NULL,NULL,NULL,FALSE,0,NULL,directory,&startup,&child))
                return GetLastError();
            CloseHandle(child.hThread);
            CloseHandle(child.hProcess);
            launched = true;
        }
        Sleep(50);
    }
}
