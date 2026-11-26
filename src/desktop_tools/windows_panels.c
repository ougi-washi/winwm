#define COBJMACROS
#include "desktop_tools/tools.h"
#include "theme/icon_button.h"
#include "theme/theme.h"
#include <wuapi.h>
#include <wscapi.h>
#include <netfw.h>
#include <shellapi.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <stdio.h>

// Each --tool process owns one window. Service queries may block on policy,
// networking or RPC, so publish their results back to the window thread.
enum panel { settings_home, windows_update, windows_security };
enum query { update_history, available_updates, security_health, quick_scan, signature_update };
enum { row_limit = 50 };
static enum panel page;
static HWND panel, list, primary, secondary, scan, signatures, status;
static HFONT font;
static w_tool_feedback feedback;
static HANDLE worker;
static struct {
    enum query kind;
    wchar_t rows[row_limit][512], summary[160];
    int count;
} result;

static const struct { const wchar_t *label, *mode; } settings[] = {
    {L"Display", L"display"}, {L"Sound", L"audio"}, {L"Wi-Fi", L"wifi"},
    {L"Network adapters", L"network_settings"}, {L"Bluetooth", L"bluetooth"},
    {L"Power", L"power_settings"}, {L"Date and time", L"date_time"},
    {L"Languages", L"languages"}, {L"Keyboard layouts", L"keyboard_settings"},
    {L"Wallpaper", L"wallpaper"}, {L"Windows Update", L"windows_update"},
    {L"Windows Security", L"windows_security"}
};

static HRESULT w_history(IUpdateSearcher *searcher) {
    LONG total = 0;
    HRESULT hr = IUpdateSearcher_GetTotalHistoryCount(searcher, &total);
    if (FAILED(hr)) return hr;
    if (!total) {
        wcscpy(result.summary, L"No update history is available.");
        return S_OK;
    }
    IUpdateHistoryEntryCollection *entries = NULL;
    LONG count = min(total, row_limit);
    hr = IUpdateSearcher_QueryHistory(searcher, 0, count, &entries);
    if (FAILED(hr)) return hr;
    static const wchar_t *const states[] = {L"Pending", L"In progress", L"Succeeded", L"Partial", L"Failed", L"Aborted"};
    for (LONG i = 0; i < count && SUCCEEDED(hr); ++i) {
        IUpdateHistoryEntry *entry = NULL;
        BSTR title = NULL;
        OperationResultCode state;
        hr = IUpdateHistoryEntryCollection_get_Item(entries, i, &entry);
        if (SUCCEEDED(hr)) hr = IUpdateHistoryEntry_get_Title(entry, &title);
        if (SUCCEEDED(hr)) hr = IUpdateHistoryEntry_get_ResultCode(entry, &state);
        if (SUCCEEDED(hr)) swprintf(result.rows[result.count++], 512, L"%ls  ·  %.455ls",
                                    (unsigned)state < count_of(states) ? states[state] : L"Unknown", title ? title : L"");
        SysFreeString(title);
        if (entry) IUpdateHistoryEntry_Release(entry);
    }
    IUpdateHistoryEntryCollection_Release(entries);
    swprintf(result.summary, count_of(result.summary), L"Recent update history (%d of %ld)", result.count, total);
    return hr;
}

static HRESULT w_available(IUpdateSearcher *searcher) {
    ISearchResult *search = NULL;
    IUpdateCollection *updates = NULL;
    BSTR criteria = SysAllocString(L"IsInstalled=0 and IsHidden=0");
    if (!criteria) return E_OUTOFMEMORY;
    HRESULT hr = IUpdateSearcher_Search(searcher, criteria, &search);
    SysFreeString(criteria);
    OperationResultCode state = orcNotStarted;
    LONG total = 0;
    if (SUCCEEDED(hr)) hr = ISearchResult_get_ResultCode(search, &state);
    if (SUCCEEDED(hr) && state != orcSucceeded && state != orcSucceededWithErrors) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = ISearchResult_get_Updates(search, &updates);
    if (SUCCEEDED(hr)) hr = IUpdateCollection_get_Count(updates, &total);
    for (LONG i = 0; i < min(total, row_limit) && SUCCEEDED(hr); ++i) {
        IUpdate *update = NULL;
        BSTR title = NULL;
        hr = IUpdateCollection_get_Item(updates, i, &update);
        if (SUCCEEDED(hr)) hr = IUpdate_get_Title(update, &title);
        if (SUCCEEDED(hr)) lstrcpynW(result.rows[result.count++], title ? title : L"", 512);
        SysFreeString(title);
        if (update) IUpdate_Release(update);
    }
    if (updates) IUpdateCollection_Release(updates);
    if (search) ISearchResult_Release(search);
    swprintf(result.summary, count_of(result.summary), L"%ls%ld updates available (%d shown).",
             state == orcSucceededWithErrors ? L"Partial search. " : L"", total, result.count);
    return hr;
}

static void w_security_health(void) {
    static const struct { DWORD provider; const wchar_t *label; } providers[] = {
        {WSC_SECURITY_PROVIDER_ANTIVIRUS, L"Antivirus"}, {WSC_SECURITY_PROVIDER_FIREWALL, L"Firewall"},
        {WSC_SECURITY_PROVIDER_AUTOUPDATE_SETTINGS, L"Automatic updates"},
        {WSC_SECURITY_PROVIDER_USER_ACCOUNT_CONTROL, L"User Account Control"}
    };
    HMODULE service = LoadLibraryExW(L"wscapi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    __typeof__(&WscGetSecurityProviderHealth) get_health =
        service ? (void *)GetProcAddress(service, "WscGetSecurityProviderHealth") : NULL;
    const wchar_t *states[] = {L"Healthy", L"Not monitored", L"Needs attention", L"Snoozed"};
    for (size_t i = 0; i < count_of(providers); ++i) {
        WSC_SECURITY_PROVIDER_HEALTH health;
        HRESULT hr = get_health ? get_health(providers[i].provider, &health) : E_NOTIMPL;
        const wchar_t *value = hr == S_OK && (unsigned)health < count_of(states) ? states[health] : L"Status unavailable";
        swprintf(result.rows[result.count++], 512, L"%ls  ·  %ls", providers[i].label, value);
    }
    if (service) FreeLibrary(service);

    // Each profile is independent. Windows may have multiple active profiles.
    static const struct { NET_FW_PROFILE_TYPE2 type; const wchar_t *name; } profiles[] = {
        {NET_FW_PROFILE2_DOMAIN, L"Domain"}, {NET_FW_PROFILE2_PRIVATE, L"Private"}, {NET_FW_PROFILE2_PUBLIC, L"Public"}
    };
    INetFwPolicy2 *firewall = NULL;
    long active = 0;
    CoCreateInstance(&CLSID_NetFwPolicy2, NULL, CLSCTX_INPROC_SERVER, &IID_INetFwPolicy2, (void **)&firewall);
    if (firewall) INetFwPolicy2_get_CurrentProfileTypes(firewall, &active);
    for (size_t i = 0; i < count_of(profiles); ++i) {
        VARIANT_BOOL enabled = VARIANT_FALSE;
        HRESULT hr = firewall ? INetFwPolicy2_get_FirewallEnabled(firewall, profiles[i].type, &enabled) : E_NOTIMPL;
        swprintf(result.rows[result.count++], 512, L"%ls firewall%ls  ·  %ls", profiles[i].name,
                 (active & profiles[i].type) ? L" (active)" : L"",
                 FAILED(hr) ? L"Status unavailable" : enabled ? L"On" : L"Off");
    }
    if (firewall) INetFwPolicy2_Release(firewall);
    wcscpy(result.summary, L"Protection status and local firewall profiles.");
}

// The service maintains InstallLocation as the platform is serviced. The
// inbox copy in Program Files covers machines without a platform update.
static bool w_defender_file(const wchar_t *path, int length, size_t cap) {
    if (length <= 0 || (size_t)length >= cap) return false;
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static bool w_defender_executable(wchar_t *exe, size_t cap) {
    wchar_t location[os_path_cap];
    DWORD bytes = sizeof(location);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows Defender", L"InstallLocation",
                     RRF_RT_REG_SZ, NULL, location, &bytes) == ERROR_SUCCESS &&
        wcslen(location) >= 3 && location[1] == L':' && location[2] == L'\\') {
        size_t len = wcslen(location);
        int n = swprintf(exe, cap, L"%ls%lsMpCmdRun.exe", location, location[len - 1] == L'\\' ? L"" : L"\\");
        if (w_defender_file(exe, n, cap)) return true;
    }
    PWSTR program_files = NULL;
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_ProgramFiles, KF_FLAG_DEFAULT, NULL, &program_files))) return false;
    int n = swprintf(exe, cap, L"%ls\\Windows Defender\\MpCmdRun.exe", program_files);
    CoTaskMemFree(program_files);
    return w_defender_file(exe, n, cap);
}

static void w_defender_action(void) {
    bool scanning = result.kind == quick_scan;
    wchar_t exe[os_path_cap], directory[os_path_cap];
    if (!w_defender_executable(exe, count_of(exe))) {
        wcscpy(result.summary, L"Microsoft Defender Antivirus is unavailable on this PC.");
        return;
    }
    wcscpy(directory, exe);
    *wcsrchr(directory, L'\\') = 0;
    SHELLEXECUTEINFOW launch = {.cbSize=sizeof(launch), .hwnd=panel, .lpVerb=L"runas", .lpFile=exe,
        .lpParameters=scanning ? L"-Scan -ScanType 1" : L"-SignatureUpdate", .lpDirectory=directory,
        .nShow=SW_HIDE, .fMask=SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI};
    DWORD code = ERROR_SUCCESS;
    BOOL started = ShellExecuteExW(&launch);
    if (!started || !launch.hProcess) {
        code = started ? ERROR_INVALID_HANDLE : GetLastError();
        if (code == ERROR_CANCELLED) wcscpy(result.summary, L"Microsoft Defender action canceled.");
        else swprintf(result.summary, count_of(result.summary), L"Could not start Microsoft Defender (0x%08lX).", code);
        return;
    }
    bool completed = WaitForSingleObject(launch.hProcess, INFINITE) == WAIT_OBJECT_0 && GetExitCodeProcess(launch.hProcess, &code);
    CloseHandle(launch.hProcess);
    if (!completed) { wcscpy(result.summary, L"Could not read the Microsoft Defender result."); return; }
    if (code == ERROR_SUCCESS) wcscpy(result.summary, scanning ? L"Microsoft Defender quick scan finished." :
                                                                L"Microsoft Defender security intelligence is up to date.");
    else if (scanning && code == 2) wcscpy(result.summary, L"Defender reported an unresolved threat or a scan error.");
    else swprintf(result.summary, count_of(result.summary), L"Microsoft Defender could not complete the %ls (0x%08lX).",
                  scanning ? L"scan" : L"update", code);
}

static DWORD WINAPI w_query(void *unused) {
    (void)unused;
    bool defender_action = result.kind == quick_scan || result.kind == signature_update;
    HRESULT apartment = CoInitializeEx(NULL, defender_action ? COINIT_APARTMENTTHREADED : COINIT_MULTITHREADED), hr = apartment;
    if (SUCCEEDED(hr)) {
        if (defender_action) w_defender_action();
        else if (result.kind == security_health) w_security_health();
        else {
            IUpdateSession *session = NULL;
            IUpdateSearcher *searcher = NULL;
            hr = CoCreateInstance(&CLSID_UpdateSession, NULL, CLSCTX_INPROC_SERVER, &IID_IUpdateSession, (void **)&session);
            if (SUCCEEDED(hr)) hr = IUpdateSession_CreateUpdateSearcher(session, &searcher);
            if (SUCCEEDED(hr)) hr = result.kind == update_history ? w_history(searcher) : w_available(searcher);
            if (searcher) IUpdateSearcher_Release(searcher);
            if (session) IUpdateSession_Release(session);
        }
        CoUninitialize();
    }
    if (FAILED(hr)) swprintf(result.summary, count_of(result.summary), L"%ls is unavailable (0x%08lX).",
                             page == windows_security ? L"Windows Security" : L"Windows Update", (DWORD)hr);
    return 0;
}

static void w_start_query(enum query kind) {
    if (worker) return;
    result.kind = kind;
    result.count = 0;
    worker = CreateThread(NULL, 0, w_query, NULL, 0, NULL);
    SetWindowTextW(status, !worker ? L"Could not start the query." : kind == available_updates ? L"Checking for updates…"
                           : kind == quick_scan ? L"Running Microsoft Defender quick scan…"
                           : kind == signature_update ? L"Updating Microsoft Defender security intelligence…"
                           : kind == update_history ? L"Reading update history…" : L"Reading protection status…");
    if (worker) {
        if (kind != quick_scan && kind != signature_update) SendMessageW(list, LB_RESETCONTENT, 0, 0);
        EnableWindow(primary, FALSE);
        if (page == windows_update) EnableWindow(secondary, FALSE);
        if (scan) EnableWindow(scan, FALSE);
        if (signatures) EnableWindow(signatures, FALSE);
    }
}

static void w_open_setting(void) {
    int i = (int)SendMessageW(list, LB_GETCURSEL, 0, 0);
    if ((unsigned)i >= count_of(settings)) return;
    wchar_t self[os_path_cap], mode[64];
    swprintf(mode, count_of(mode), L"--tool-%ls", settings[i].mode);
    RECT r;
    GetWindowRect(panel, &r);
    POINT origin = {(r.left + r.right) / 2, (r.top + r.bottom) / 2};
    HANDLE child = w_os_self(self, count_of(self)) ? w_os_launch_positioned(self, mode, origin) : NULL;
    if (child) CloseHandle(child);
    else SetWindowTextW(status, L"Could not open the selected setting.");
}

static void w_finish_query(void) {
    CloseHandle(worker);
    worker = NULL;
    for (int i = 0; i < result.count; ++i) SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)result.rows[i]);
    SetWindowTextW(status, result.summary);
    EnableWindow(primary, TRUE);
    if (secondary) EnableWindow(secondary, TRUE);
    if (scan) EnableWindow(scan, TRUE);
    if (signatures) EnableWindow(signatures, TRUE);
}

static bool w_open_firewall(void) {
    wchar_t system[MAX_PATH], mmc[MAX_PATH], console[MAX_PATH];
    UINT length = GetSystemDirectoryW(system, count_of(system));
    if (!length || length + count_of(L"\\mmc.exe") > count_of(mmc)) return false;
    swprintf(mmc, count_of(mmc), L"%ls\\mmc.exe", system);
    swprintf(console, count_of(console), L"%ls\\wf.msc", system);
    return w_os_run_executable(mmc, console, NULL, false);
}

static void w_layout(bool update_font) {
    if (!status) return;
    UINT dpi = GetDpiForWindow(panel);
    if (update_font) {
        HFONT next = w_theme_message_font(dpi);
        if (next) {
            for (HWND w = GetWindow(panel, GW_CHILD); w; w = GetWindow(w, GW_HWNDNEXT))
                SendMessageW(w, WM_SETFONT, (WPARAM)next, TRUE);
            DeleteObject(font);
            font = next;
        }
    }
    RECT r;
    w_theme_content_rect(panel, &r);
    int gap = w_theme_spacing(dpi), pad = 2 * gap, side = w_theme_button_size(dpi);
    int width = r.right - 2 * pad;
    HWND actions[] = {primary, secondary, scan, signatures};
    int count = signatures ? 4 : secondary ? 2 : 1;
    int buttons = count * side + (count - 1) * gap;
    int text_width = max(1, width - buttons - gap);
    int footer = max(side, w_tool_feedback_height(status, &feedback, text_width));
    int bottom = r.bottom - pad - side;
    w_tool_place(list, pad, pad, width, r.bottom - 2 * pad - footer - gap);
    SendMessageW(list, LB_SETITEMHEIGHT, 0, side);
    w_tool_place(status, pad, r.bottom - pad - footer, text_width, footer);
    for (int i = 0; i < count; ++i) w_tool_place(actions[i], pad + width - buttons + i * (side + gap), bottom, side, side);
}

static LRESULT CALLBACK w_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    LRESULT handled;
    if (w_theme_message(w, msg, wp, lp, &handled)) return handled;
    switch (msg) {
    case WM_GETMINMAXINFO:
        w_tool_minimum(w, (MINMAXINFO *)lp, MulDiv(360, GetDpiForWindow(w), 96), 5 * w_theme_button_size(GetDpiForWindow(w)));
        return 0;
    case WM_SIZE: w_layout(false); return 0;
    case w_tool_feedback_changed: w_layout(false); return 0;
    case WM_DPICHANGED: case WM_SETTINGCHANGE:
        if (w_tool_font_change(w, msg, wp, lp)) w_layout(true);
        break;
    case WM_COMMAND:
        if (page == settings_home && ((HWND)lp == primary || ((HWND)lp == list && HIWORD(wp) == LBN_DBLCLK)))
            w_open_setting();
        else if ((HWND)lp == primary) w_start_query(page == windows_update ? available_updates : security_health);
        else if (scan && (HWND)lp == scan) w_start_query(quick_scan);
        else if (signatures && (HWND)lp == signatures) w_start_query(signature_update);
        else if (secondary && (HWND)lp == secondary) {
            if (page == windows_update) w_start_query(update_history);
            else if (!w_open_firewall())
                SetWindowTextW(status, L"Could not open firewall settings.");
        }
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

int w_windows_panel_run(const wchar_t *mode) {
    page = !wcscmp(mode, L"windows_settings") ? settings_home : !wcscmp(mode, L"windows_update") ? windows_update : windows_security;
    const wchar_t *titles[] = {L"Windows Settings", L"Windows Update", L"Windows Security"};
    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSW wc = {.lpfnWndProc=w_proc, .hInstance=inst, .lpszClassName=L"WinwmWindowsPanel", .hCursor=LoadCursorW(NULL, IDC_ARROW)};
    if (!RegisterClassW(&wc)) return 1;
    MONITORINFO monitor = {.cbSize=sizeof(monitor)};
    UINT dpi = w_tool_work_area(&monitor);
    RECT r = monitor.rcWork;
    int width = min(MulDiv(560, dpi, 96), r.right - r.left), height = min(MulDiv(380, dpi, 96), r.bottom - r.top);
    panel = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, titles[page], WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
            (r.left + r.right - width)/2, (r.top + r.bottom - height)/2, width, height, NULL, NULL, inst, NULL);
    if (!panel) return 1;
    DWORD child = WS_CHILD | WS_VISIBLE | WS_TABSTOP;
    list = CreateWindowW(L"LISTBOX", titles[page], child | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT |
                         LBS_HASSTRINGS | LBS_OWNERDRAWFIXED, 0,0,1,1, panel, NULL, inst, NULL);
    primary = CreateWindowW(L"BUTTON", page == settings_home ? L"Open" : page == windows_update ? L"Check for updates" : L"Refresh",
                            child | BS_OWNERDRAW, 0,0,1,1, panel, (HMENU)101, inst, NULL);
    if (page != settings_home)
        secondary = CreateWindowW(L"BUTTON", page == windows_update ? L"Update history" : L"Firewall settings",
                                  child | BS_OWNERDRAW, 0,0,1,1, panel, (HMENU)102, inst, NULL);
    if (page == windows_security) {
        scan = CreateWindowW(L"BUTTON", L"Quick scan", child | BS_OWNERDRAW, 0,0,1,1, panel, (HMENU)103, inst, NULL);
        signatures = CreateWindowW(L"BUTTON", L"Update security intelligence", child | BS_OWNERDRAW,
                                   0,0,1,1, panel, (HMENU)104, inst, NULL);
    }
    status = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_NOPREFIX, 0,0,1,1, panel, NULL, inst, NULL);
    if (!list || !primary || !status || (page != settings_home && !secondary) ||
        (page == windows_security && (!scan || !signatures))) return 1;
    w_tool_track_feedback(status, &feedback);
    w_icon_button(primary, page == settings_home ? L"\uE8A7" : L"\uE72C",
                  page == settings_home ? L"Open selected setting" : page == windows_update ? L"Check for updates" : L"Refresh protection status");
    if (secondary) w_icon_button(secondary, page == windows_update ? L"\uE81C" : L"\uEA18",
                                 page == windows_update ? L"Update history" : L"Firewall settings");
    if (scan) w_icon_button(scan, L"\uE721", L"Run Microsoft Defender quick scan");
    if (signatures) w_icon_button(signatures, L"\uE896", L"Update Microsoft Defender security intelligence");
    w_layout(true);
    if (page == settings_home) {
        for (size_t i = 0; i < count_of(settings); ++i) SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)settings[i].label);
        SendMessageW(list, LB_SETCURSEL, 0, 0);
    } else w_start_query(page == windows_update ? update_history : security_health);
    w_tool_show(panel);
    SetFocus(list);
    MSG msg;
    for (;;) {
        DWORD wait = MsgWaitForMultipleObjectsEx(worker ? 1 : 0, &worker, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (wait == WAIT_FAILED) break;
        if (worker && wait == WAIT_OBJECT_0) { w_finish_query(); continue; }
        if (!PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) continue;
        if (msg.message == WM_QUIT) break;
        if (w_tool_repeated_dialog_key(&msg)) continue;
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) DestroyWindow(panel);
        else if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN && msg.hwnd == list && page == settings_home)
            w_open_setting();
        else if (!IsDialogMessageW(panel, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    if (worker) CloseHandle(worker);
    DeleteObject(font);
    return 0;
}
