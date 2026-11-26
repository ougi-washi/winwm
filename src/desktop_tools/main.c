#include "desktop_tools/entry.h"
#include "desktop_tools/tools.h"
#include "desktop_tools/adapter.h"
#include "install/install.h"
#include "theme/dialog.h"
#include "theme/theme.h"
#include "os_win32/environment.h"
#include <powrprof.h>
#include <shellscalingapi.h>
#include <wchar.h>

////////////////
//~ Desktop Tools

UINT w_tool_work_area(MONITORINFO *info) {
    STARTUPINFOW startup = {.cb = sizeof(startup)};
    GetStartupInfoW(&startup);
    HMONITOR monitor = w_tool_popup_monitor();
    bool anchored = monitor != NULL;
    if (!monitor && (startup.dwFlags & STARTF_USEPOSITION))
        monitor = MonitorFromPoint((POINT){(LONG)startup.dwX, (LONG)startup.dwY}, MONITOR_DEFAULTTONULL);
    if (!monitor) monitor = MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY);
    GetMonitorInfoW(monitor, info);
    if (anchored) info->rcWork = info->rcMonitor;
    UINT dpi = 96, y = 96;
    if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi, &y))) dpi = 96;
    return dpi;
}

static int w_run_tool(const wchar_t *mode) {
    if (!wcsncmp(mode,L"dns-",4)) return (int)w_adapter_apply_dns(mode+4);
    if (!wcscmp(mode,L"bar_settings")) return w_toolbar_run();
    if (!wcscmp(mode,L"capture_window") || !wcsncmp(mode,L"capture_window-",15))
        return w_capture_run(mode);
    bool project=!wcscmp(mode,L"project");
    if (project || !w_theme_read_published()) {
        w_desktop_config *cfg = malloc(sizeof(*cfg));
        if (cfg) {
            w_config_defaults(cfg);
            char diagnostic[256];
            if (!w_os_cache_read(cfg) && !w_os_config_load(cfg,diagnostic,sizeof(diagnostic)))
                w_config_defaults(cfg);
            w_theme_configure(cfg);
            if (project) {
                w_environment_start(NULL,0);
                int result = w_project_run(cfg);
                free(cfg);
                return result;
            }
            free(cfg);
        } else if (project) return 1;
    }
    if ((!wcscmp(mode,L"sleep") || !wcscmp(mode,L"restart") || !wcscmp(mode,L"wallpaper")) &&
        !w_tool_native_dialog()) return 0;
    if (!wcscmp(mode, L"languages")) return w_language_setup(input_languages);
    if (!wcscmp(mode, L"keyboard_settings")) return w_language_setup(input_keyboards);
    if (!wcscmp(mode, L"input_methods")) return w_language_setup(input_methods);
    if (!wcscmp(mode, L"date_time"))
        return w_clock_run();
    if (!wcscmp(mode, L"pick_color"))
        return w_color_run();
    if (!wcscmp(mode,L"packages") || !wcscmp(mode,L"updates") || !wcscmp(mode,L"chocolatey") || !wcscmp(mode,L"google_japanese_input")) {
        w_environment_start(NULL,0);
        return w_packages_run(mode);
    }
    if (!wcscmp(mode, L"activity"))
        return w_activity_run();
    if (!wcsncmp(mode, L"open_with-", 10)) return w_open_with_list(mode + 10);
    if (!wcscmp(mode, L"windows_settings") || !wcscmp(mode, L"windows_update") || !wcscmp(mode, L"windows_security"))
        return w_windows_panel_run(mode);
    if (!wcscmp(mode,L"update_winwm")) return w_releases_run();
    if (!wcscmp(mode,L"install_winwm") || !wcscmp(mode,L"uninstall_winwm")) {
        if (!w_tool_native_dialog()) return ERROR_CANCELLED;
        return (int)w_install_run(!wcscmp(mode,L"install_winwm") ? op_install : op_uninstall,true);
    }
    int result = 0;
    if (!wcscmp(mode, L"capture_region") || !wcscmp(mode, L"record"))
        result = w_capture_run(mode);
    else if (!wcscmp(mode, L"sleep") || !wcscmp(mode, L"restart")) {
        if (w_dialog(NULL, !wcscmp(mode, L"sleep") ? L"Put this PC to sleep?" : L"Restart this PC?",
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

int w_tools_run(const wchar_t *mode) {
    int result = w_run_tool(mode);
    w_tool_popup_cleanup();
    w_environment_stop();
    return result;
}
