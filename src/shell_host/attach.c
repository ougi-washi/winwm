#include "shell_host/entry.h"
#include <stdio.h>

static bool w_recovery_path(wchar_t path[os_path_cap]) {
    if (!w_os_self(path,os_path_cap)) return false;
    wchar_t *file = wcsrchr(path,L'\\');
    if (!file || (size_t)(file+1-path)+count_of(L"winwm_recovery.exe") > os_path_cap) return false;
    wcscpy(file+1,L"winwm_recovery.exe");
    return true;
}

bool w_recovery_validate(void) {
    wchar_t path[os_path_cap];
    if (!w_recovery_path(path)) return false;
    HANDLE check = w_os_launch(path,WINWM_RECOVERY_CHECK);
    if (!check) return false;
    DWORD code = ERROR_TIMEOUT;
    if (WaitForSingleObject(check,10000) == WAIT_OBJECT_0) GetExitCodeProcess(check,&code);
    CloseHandle(check);
    return code == ERROR_SUCCESS;
}

DWORD w_recovery_take_desktop(HANDLE host, HWND window) {
    // Keep the original shell identifiable if the monitor dies during handoff.
    DWORD pid = 0;
    HWND shell = GetShellWindow();
    if (shell) GetWindowThreadProcessId(shell, &pid);
    HANDLE original = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : NULL;
    DWORD_PTR reply = 0;
    SetLastError(ERROR_SUCCESS);
    LRESULT delivered = SendMessageTimeoutW(window, recovery_take_desktop, GetCurrentProcessId(), 0,
                                            SMTO_ERRORONEXIT, 30000, &reply);
    DWORD error = delivered ? ERROR_SUCCESS : GetLastError();
    if (delivered && reply && WaitForSingleObject(host, 0) == WAIT_TIMEOUT) {
        error = (DWORD)(reply - 1);
    } else {
        // Window destruction can precede the monitor process becoming signaled.
        // Recover an unacknowledged exit before releasing our session lock.
        if (original) WaitForSingleObject(original, 10000);
        w_os_ensure_shell();
        if (!error) error = ERROR_PROCESS_ABORTED;
    }
    if (original) CloseHandle(original);
    return error;
}

HANDLE w_recovery_attach(bool preview, HWND *window) {
    wchar_t path[os_path_cap], args[96];
    FILETIME created, exited, kernel, user;
    if (!w_recovery_path(path) || !GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user))
        return NULL;
    DWORD pid = GetCurrentProcessId();
    swprintf(args,count_of(args),L"/start2:%u:%lu:%llu",preview,pid,
             ((uint64_t)created.dwHighDateTime << 32) | created.dwLowDateTime);
    // The short-lived launcher exits before we change any windows. Ending the
    // manager's process tree then leaves the recovery monitor running.
    HANDLE launcher = w_os_launch(path,args);
    if (!launcher) return NULL;
    DWORD code = ERROR_TIMEOUT;
    if (WaitForSingleObject(launcher,15000) == WAIT_OBJECT_0) GetExitCodeProcess(launcher,&code);
    CloseHandle(launcher);
    if (code) { SetLastError(code); return NULL; }
    ULONGLONG end = GetTickCount64()+5000;
    do {
        HWND w = FindWindowW(L"WinwmHost",NULL);
        if ((DWORD)(uintptr_t)GetPropW(w,L"winwm.session_pid") == pid) {
            DWORD host_pid = 0;
            GetWindowThreadProcessId(w,&host_pid);
            HANDLE host = OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,host_pid);
            if (host) {
                wchar_t actual[os_path_cap];
                DWORD size = count_of(actual);
                if (QueryFullProcessImageNameW(host,0,actual,&size) && !_wcsicmp(actual,path)) {
                    *window = w;
                    return host;
                }
                CloseHandle(host);
            }
        }
        Sleep(20);
    } while (GetTickCount64() < end);
    SetLastError(ERROR_TIMEOUT);
    return NULL;
}
