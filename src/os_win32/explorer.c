#include "os_win32/os_win32.h"
#include "os_win32/environment.h"
#include <restartmanager.h>
#include <wchar.h>

static bool w_named_user_object(HANDLE object, const wchar_t *expected) {
    wchar_t name[64];
    DWORD bytes;
    return GetUserObjectInformationW(object,UOI_NAME,name,sizeof(name),&bytes) && !_wcsicmp(name,expected);
}

static DWORD w_shell_desktop(void) {
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
    while ((tray = FindWindowExW(NULL,tray,L"Shell_TrayWnd",NULL)))
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

// During Explorer startup and teardown the primary tray and desktop can briefly
// exist independently. Third-party bars can also use the Shell_TrayWnd class.
static bool w_explorer_tray_pending(const wchar_t *exe) {
    HWND tray = NULL;
    while ((tray = FindWindowExW(NULL,tray,L"Shell_TrayWnd",NULL))) {
        DWORD pid = w_window_pid(tray);
        if (pid == GetCurrentProcessId()) continue;
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
        if (!process) continue;
        DWORD error = w_check_explorer(process,pid,exe);
        CloseHandle(process);
        if (!error) return true;
    }
    return false;
}

bool w_os_explorer_tray_present(void) {
    wchar_t directory[MAX_PATH], exe[MAX_PATH];
    return !w_explorer_path(directory,exe) && w_explorer_tray_pending(exe);
}

static DWORD w_shutdown_explorer(HWND shell, HANDLE process, DWORD pid) {
    RM_UNIQUE_PROCESS app = {.dwProcessId=pid};
    FILETIME exited, kernel, user;
    if (!GetProcessTimes(process,&app.ProcessStartTime,&exited,&kernel,&user)) return GetLastError();
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
        if (!error && (count != 1 || pid != affected.Process.dwProcessId ||
            CompareFileTime(&app.ProcessStartTime,&affected.Process.ProcessStartTime) != 0))
            error = ERROR_INVALID_OWNER;
    }
    if (!error) {
        if (GetSystemMetrics(SM_SHUTTINGDOWN)) error = ERROR_SHUTDOWN_IN_PROGRESS;
        else if (GetShellWindow() != shell || w_window_pid(shell) != pid) error = ERROR_RETRY;
        else error = RmShutdown(session,0,NULL);
    }
    RmEndSession(session);
    // RM can race an independent shell exit. Only a signaled process confirms
    // termination, including when Windows reports an empty affected list.
    DWORD wait = WaitForSingleObject(process,error ? 0 : 10000);
    if (wait == WAIT_OBJECT_0) return ERROR_SUCCESS;
    if (error) return error;
    return wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
}

DWORD w_os_exit_shell(void) {
    DWORD error = w_shell_desktop();
    wchar_t directory[MAX_PATH], exe[MAX_PATH];
    if (!error) error = w_explorer_path(directory,exe);
    if (error) return error;
    ULONGLONG deadline = GetTickCount64()+10000;
    bool exited = false;
    for (;;) {
        if (GetSystemMetrics(SM_SHUTTINGDOWN)) return ERROR_SHUTDOWN_IN_PROGRESS;
        HWND shell = GetShellWindow();
        if (!shell && !w_explorer_tray_pending(exe)) return exited ? ERROR_SUCCESS : ERROR_NOT_FOUND;
        if (GetTickCount64() >= deadline) return ERROR_TIMEOUT;
        DWORD pid = w_window_pid(shell);
        if (pid) {
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,pid);
            if (!process) {
                error = GetLastError();
                if (GetShellWindow() == shell && w_window_pid(shell) == pid) return error;
                continue;
            }
            error = w_check_explorer(process,pid,exe);
            if (GetShellWindow() != shell || w_window_pid(shell) != pid) error = ERROR_RETRY;
            if (!error) error = w_shutdown_explorer(shell,process,pid);
            CloseHandle(process);
            if (error == ERROR_RETRY) continue;
            if (error) return error;
            exited = true;
            continue;
        }
        Sleep(50);
    }
}

DWORD w_os_ensure_shell(void) {
    DWORD error = w_shell_desktop();
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
        if (!launched && !shell && !w_explorer_tray_pending(exe)) {
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
