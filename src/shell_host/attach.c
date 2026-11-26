#include "shell_host/entry.h"
#include <stdio.h>

HANDLE w_recovery_take_desktop(HWND window) {
    // The monitor duplicates its worker handle into this process. Both message
    // loops remain available while Restart Manager negotiates with Explorer.
    DWORD_PTR reply = 0;
    LRESULT delivered = SendMessageTimeoutW(window, recovery_take_desktop, GetCurrentProcessId(), 0,
                                            SMTO_ERRORONEXIT | SMTO_ABORTIFHUNG, 5000, &reply);
    if (delivered && reply) {
        // Explorer can close only after the manager holds the completion handle.
        if (PostMessageW(window, recovery_confirm_handoff, GetCurrentProcessId(), (LPARAM)reply))
            return (HANDLE)reply;
        DWORD error = GetLastError();
        CloseHandle((HANDLE)reply);
        SetLastError(error);
        return NULL;
    }
    if (delivered || !GetLastError()) SetLastError(ERROR_PROCESS_ABORTED);
    return NULL;
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
