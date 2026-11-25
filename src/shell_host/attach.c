#include "shell_host/entry.h"
#include <stdio.h>

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
    if (!w_os_self(path,count_of(path)) || !GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user))
        return NULL;
    DWORD pid = GetCurrentProcessId();
    swprintf(args,count_of(args),L"--recovery-start:%u:%lu:%llu",preview,pid,
             ((uint64_t)created.dwHighDateTime << 32) | created.dwLowDateTime);
    // The short-lived launcher exits before we change any windows. Ending the
    // manager's process tree then leaves the recovery monitor running.
    HANDLE launcher = w_os_launch(path,args);
    if (!launcher) return NULL;
    DWORD code = ERROR_TIMEOUT;
    if (WaitForSingleObject(launcher,15000) == WAIT_OBJECT_0) GetExitCodeProcess(launcher,&code);
    CloseHandle(launcher);
    if (code) { SetLastError(code); return NULL; }
    HWND w = NULL;
    while ((w = FindWindowExW(NULL,w,L"WinwmHost",NULL))) {
        if ((DWORD)(uintptr_t)GetPropW(w,L"winwm.session_pid") != pid) continue;
        DWORD host_pid = 0;
        GetWindowThreadProcessId(w,&host_pid);
        HANDLE host = OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,host_pid);
        if (!host) continue;
        wchar_t actual[os_path_cap];
        DWORD size = count_of(actual);
        if (QueryFullProcessImageNameW(host,0,actual,&size) && !_wcsicmp(actual,path)) {
            *window = w;
            return host;
        }
        CloseHandle(host);
    }
    SetLastError(ERROR_INVALID_OWNER);
    return NULL;
}
