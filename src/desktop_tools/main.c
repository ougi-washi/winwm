#include "desktop_tools/entry.h"
#include "desktop_tools/tools.h"
#include <shellapi.h>
#include <powrprof.h>
#include <wchar.h>

////////////////
//~ Desktop Tools

int w_tools_run(const wchar_t *mode) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int result = 0;
    if (!wcscmp(mode, L"capture_region") || !wcscmp(mode, L"record") || !wcscmp(mode, L"capture_window"))
        result = w_capture_run(mode);
    else if (!wcscmp(mode, L"sleep") || !wcscmp(mode, L"restart")) {
        if (MessageBoxW(NULL, !wcscmp(mode, L"sleep") ? L"Put this PC to sleep?" : L"Restart this PC?",
                        L"Power", MB_OKCANCEL | MB_DEFBUTTON2) == IDOK) {
            if (!wcscmp(mode, L"sleep"))
                result = !SetSuspendState(FALSE, FALSE, FALSE);
            else {
                HANDLE token;
                if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
                    TOKEN_PRIVILEGES p = {.PrivilegeCount = 1};
                    LookupPrivilegeValueW(NULL, SE_SHUTDOWN_NAME, &p.Privileges[0].Luid);
                    p.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
                    AdjustTokenPrivileges(token, FALSE, &p, 0, NULL, NULL);
                    CloseHandle(token);
                }
                result = !ExitWindowsEx(EWX_REBOOT, SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_FLAG_PLANNED);
            }
        }
    } else
        result = w_settings_run(mode);
    return result;
}
