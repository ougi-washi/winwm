#include "desktop_tools/entry.h"
#include "desktop_tools/tools.h"
#include "desktop_tools/adapter.h"
#include "theme/theme.h"
#include <powrprof.h>
#include <shellscalingapi.h>
#include <wchar.h>

////////////////
//~ Desktop Tools

UINT w_tool_work_area(MONITORINFO *info) {
    STARTUPINFOW startup = {.cb = sizeof(startup)};
    GetStartupInfoW(&startup);
    HMONITOR monitor = NULL;
    if (startup.dwFlags & STARTF_USEPOSITION)
        monitor = MonitorFromPoint((POINT){(LONG)startup.dwX, (LONG)startup.dwY}, MONITOR_DEFAULTTONULL);
    if (!monitor) monitor = MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY);
    GetMonitorInfoW(monitor, info);
    UINT dpi = 96, y = 96;
    if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi, &y))) dpi = 96;
    return dpi;
}

int w_tools_run(const wchar_t *mode) {
    if (!wcsncmp(mode,L"dns-",4)) return (int)w_adapter_apply_dns(mode+4);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (!wcscmp(mode,L"bar_settings")) return w_toolbar_run();
    if (!wcscmp(mode,L"capture_window") || !wcsncmp(mode,L"capture_window-",15))
        return w_capture_run(mode);
    w_desktop_config *cfg = calloc(1, sizeof(*cfg));
    if (cfg) {
        w_config_defaults(cfg);
        w_os_cache_read(cfg);
        w_theme_colors(cfg->background, cfg->foreground, cfg->accent);
        if (!wcscmp(mode, L"project")) {
            int result = w_project_run(cfg);
            free(cfg);
            return result;
        }
        free(cfg);
    } else if (!wcscmp(mode, L"project")) return 1;
    if (!wcscmp(mode, L"languages")) return w_language_setup(input_languages);
    if (!wcscmp(mode, L"keyboard_settings")) return w_language_setup(input_keyboards);
    if (!wcscmp(mode, L"input_methods")) return w_language_setup(input_methods);
    if (!wcscmp(mode, L"date_time"))
        return w_clock_run();
    if (!wcscmp(mode, L"pick_color"))
        return w_color_run();
    if (!wcscmp(mode,L"packages") || !wcscmp(mode,L"updates") || !wcscmp(mode,L"google_japanese_input"))
        return w_packages_run(mode);
    if (!wcscmp(mode, L"activity"))
        return w_activity_run();
    int result = 0;
    if (!wcscmp(mode, L"capture_region") || !wcscmp(mode, L"record"))
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
