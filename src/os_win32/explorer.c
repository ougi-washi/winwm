#include "os_win32/os_win32.h"
#include <restartmanager.h>
#include <wchar.h>

////////////////
//~ Explorer

// Restart Manager receives one process identity, including creation time, and may report a veto.
DWORD w_os_exit_explorer(void) {
    HWND shell = GetShellWindow();
    if (!shell)
        return ERROR_NOT_FOUND;
    DWORD pid = 0, own_session, session, error = ERROR_INVALID_OWNER;
    GetWindowThreadProcessId(shell, &pid);
    if (!pid || !ProcessIdToSessionId(pid, &session) ||
        !ProcessIdToSessionId(GetCurrentProcessId(), &own_session) || own_session != session)
        return error;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (!process)
        return GetLastError();
    wchar_t path[os_path_cap], expected[os_path_cap];
    DWORD size = count_of(path);
    UINT len = GetSystemWindowsDirectoryW(expected, count_of(expected));
    RM_UNIQUE_PROCESS app = {.dwProcessId = pid};
    FILETIME end, kernel, user;
    if (len && len + 14 < count_of(expected) && QueryFullProcessImageNameW(process, 0, path, &size) &&
        !_wcsicmp(path, wcscat(expected, L"\\explorer.exe")) &&
        GetProcessTimes(process, &app.ProcessStartTime, &end, &kernel, &user)) {
        DWORD handle;
        wchar_t key[CCH_RM_SESSION_KEY + 1] = {0};
        error = RmStartSession(&handle, 0, key);
        if (error == ERROR_SUCCESS) {
            error = RmRegisterResources(handle, 0, NULL, 1, &app, 0, NULL);
            if (!error)
                error = RmShutdown(handle, 0, NULL);
            RmEndSession(handle);
            if (!error) {
                DWORD wait = WaitForSingleObject(process, 30000);
                if (wait != WAIT_OBJECT_0)
                    error = wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
            }
        }
    }
    CloseHandle(process);
    return error;
}
