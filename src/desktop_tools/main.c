#include "desktop_tools/entry.h"
#include "desktop_tools/tools.h"
#include "theme/theme.h"
#include <shellapi.h>
#include <powrprof.h>
#include <ws2tcpip.h>
#include <netioapi.h>
#include <wchar.h>

////////////////
//~ Desktop Tools

static bool w_has_wifi(void) {
    MIB_IF_TABLE2 *table = NULL;
    bool found = false;
    if (GetIfTable2(&table) == NO_ERROR) {
        for (ULONG i = 0; i < table->NumEntries; i++)
            if (table->Table[i].Type == IF_TYPE_IEEE80211) {
                found = true;
                break;
            }
        FreeMibTable(table);
    }
    return found;
}

static const wchar_t *w_settings_uri(const wchar_t *mode) {
    static const struct { const wchar_t *name, *uri; } pages[] = {
        {L"audio", L"ms-settings:sound"}, {L"volume_mixer", L"ms-settings:apps-volume"},
        {L"bluetooth", L"ms-settings:bluetooth"}, {L"date_time", L"ms-settings:dateandtime"},
        {L"power_settings", L"ms-settings:powersleep"}, {L"network_settings", L"ms-settings:network-status"}
    };
    if (!wcscmp(mode, L"wifi"))
        return w_has_wifi() ? L"ms-settings:network-wifi" : L"ms-settings:network-status";
    for (size_t i = 0; i < count_of(pages); i++)
        if (!wcscmp(mode, pages[i].name))
            return pages[i].uri;
    return NULL;
}

int w_tools_run(const wchar_t *mode) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    w_desktop_config *cfg = calloc(1, sizeof(*cfg));
    if (cfg) {
        w_config_defaults(cfg);
        w_os_cache_read(cfg);
        w_theme_colors(cfg->background, cfg->foreground, cfg->accent);
        free(cfg);
    }
    if (!wcscmp(mode, L"languages") || !wcscmp(mode, L"keyboard_settings"))
        return w_language_setup(!wcscmp(mode, L"keyboard_settings"));
    const wchar_t *uri = w_settings_uri(mode);
    if (uri && FindWindowW(L"Shell_TrayWnd", NULL)) {
        HRESULT apartment = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        SHELLEXECUTEINFOW open = {.cbSize = sizeof(open), .fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI,
                                  .lpVerb = L"open", .lpFile = uri, .nShow = SW_SHOWNORMAL};
        bool opened = ShellExecuteExW(&open) != 0;
        if (SUCCEEDED(apartment))
            CoUninitialize();
        if (opened)
            return 0;
    }
    if (!wcscmp(mode, L"audio") || !wcscmp(mode, L"date_time")) {
        if (w_os_launch_provider("%SystemRoot%\\System32\\control.exe",
                                 !wcscmp(mode, L"audio") ? "mmsys.cpl" : "timedate.cpl"))
            return 0;
        if (!wcscmp(mode, L"date_time")) {
            MessageBoxW(NULL, L"Windows could not open Date and Time.", L"winwm", MB_OK | MB_ICONINFORMATION);
            return 1;
        }
    }
    if (!wcscmp(mode, L"volume_mixer") && w_os_launch_provider("%SystemRoot%\\System32\\SndVol.exe", NULL))
        return 0;
    if (!wcscmp(mode, L"wifi") && !wcscmp(uri, L"ms-settings:network-status"))
        return w_settings_run(L"network_settings");
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
