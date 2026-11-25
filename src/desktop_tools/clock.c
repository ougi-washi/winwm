#include "desktop_tools/tools.h"
#include "theme/theme.h"
#include "theme/icon_button.h"
#include <commctrl.h>
#include <stdio.h>
#include <uxtheme.h>

static HWND calendar, zone, clock_text, date_text, zone_label, today, save_zone;
static DYNAMIC_TIME_ZONE_INFORMATION *zones;
static DWORD zone_count;
static HFONT font, time_font;

enum { zone_list_ready = WM_APP + 1 };
typedef struct {
    LONG refs, cancelled;
    HWND window;
    DYNAMIC_TIME_ZONE_INFORMATION *rows;
    DWORD count;
} w_zone_query;
static w_zone_query *zone_query;

static void w_calendar_colors(void) {
    HIGHCONTRASTW hc = {.cbSize=sizeof(hc)};
    bool contrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(hc),&hc,0) && (hc.dwFlags&HCF_HIGHCONTRASTON);
    COLORREF paper = contrast ? GetSysColor(COLOR_WINDOW) : w_theme_background();
    SendMessageW(calendar,MCM_SETCOLOR,MCSC_BACKGROUND,paper);
    SendMessageW(calendar,MCM_SETCOLOR,MCSC_MONTHBK,paper);
    SendMessageW(calendar,MCM_SETCOLOR,MCSC_TEXT,contrast ? GetSysColor(COLOR_WINDOWTEXT) : w_theme_foreground());
    SendMessageW(calendar,MCM_SETCOLOR,MCSC_TRAILINGTEXT,contrast ? GetSysColor(COLOR_GRAYTEXT) : w_theme_hint());
    // The classic calendar also uses the title background ink for weekday and week-number labels.
    SendMessageW(calendar,MCM_SETCOLOR,MCSC_TITLEBK,contrast ? GetSysColor(COLOR_HIGHLIGHT) : w_theme_accent());
    SendMessageW(calendar,MCM_SETCOLOR,MCSC_TITLETEXT,contrast ? GetSysColor(COLOR_HIGHLIGHTTEXT) : paper);
}

static void w_clock_tick(void) {
    SYSTEMTIME now;
    wchar_t time[80], date[128];
    GetLocalTime(&now);
    if (GetTimeFormatEx(NULL, 0, &now, NULL, time, count_of(time)))
        SetWindowTextW(clock_text, time);
    if (GetDateFormatEx(NULL, DATE_LONGDATE, &now, NULL, date, count_of(date), NULL))
        SetWindowTextW(date_text, date);
}

static void w_clock_layout(HWND window) {
    UINT dpi = GetDpiForWindow(window);
    HFONT next = CreateFontW(-MulDiv(15, dpi, 96), 0,0,0, FW_NORMAL, FALSE,FALSE,FALSE,
                             DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
    HWND controls[] = {calendar, zone, date_text, zone_label, today, save_zone};
    for (size_t i = 0; i < count_of(controls); ++i)
        SendMessageW(controls[i], WM_SETFONT, (WPARAM)next, TRUE);
    DeleteObject(font);
    font = next;
    next = CreateFontW(-MulDiv(26, dpi, 96), 0,0,0, FW_SEMIBOLD, FALSE,FALSE,FALSE,
                       DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
    SendMessageW(clock_text, WM_SETFONT, (WPARAM)next, TRUE);
    DeleteObject(time_font);
    time_font = next;
    RECT cal = {0}, client;
    SendMessageW(calendar, MCM_GETMINREQRECT, 0, (LPARAM)&cal);
    GetClientRect(window, &client);
    int gap = MulDiv(12, dpi, 96), button = MulDiv(34, dpi, 96);
    int width = max(1, client.right - 2*gap), line = MulDiv(22, dpi, 96);
    int y = 2*gap + button + line;
    MoveWindow(clock_text, gap, gap, width-button-gap, button, TRUE);
    MoveWindow(today, gap+width-button, gap, button, button, TRUE);
    MoveWindow(date_text, gap, gap+button, width, line, TRUE);
    MoveWindow(calendar, (client.right-cal.right)/2, y, cal.right, cal.bottom, TRUE);
    y += cal.bottom + gap;
    MoveWindow(zone_label, gap, y, width, line, TRUE);
    y += line;
    MoveWindow(zone, gap, y, width-button-gap, 8*button, TRUE);
    MoveWindow(save_zone, gap+width-button, y, button, button, TRUE);
    RECT desired = {0, 0, client.right, y+button+gap};
    AdjustWindowRectExForDpi(&desired, GetWindowLongW(window, GWL_STYLE), FALSE,
                             GetWindowLongW(window, GWL_EXSTYLE), dpi);
    SetWindowPos(window, NULL, 0,0, desired.right-desired.left, desired.bottom-desired.top,
                 SWP_NOACTIVATE|SWP_NOMOVE|SWP_NOZORDER);
}

static void w_release_zone_query(w_zone_query *query) {
    if (!InterlockedDecrement(&query->refs)) {
        free(query->rows);
        free(query);
    }
}

static DWORD WINAPI w_enumerate_zones(void *arg) {
    w_zone_query *query = arg;
    DWORD capacity = 0;
    while (!InterlockedCompareExchange(&query->cancelled,0,0)) {
        DYNAMIC_TIME_ZONE_INFORMATION entry;
        DWORD error = EnumDynamicTimeZoneInformation(query->count,&entry);
        if (error != ERROR_SUCCESS) {
            if (error != ERROR_NO_MORE_ITEMS) query->count = 0;
            break;
        }
        if (query->count == capacity) {
            capacity = capacity ? capacity * 2 : 64;
            void *rows = realloc(query->rows,(size_t)capacity * sizeof(entry));
            if (!rows) { query->count = 0; break; }
            query->rows = rows;
        }
        query->rows[query->count++] = entry;
    }
    if (!InterlockedCompareExchange(&query->cancelled,0,0))
        PostMessageW(query->window,zone_list_ready,0,(LPARAM)query);
    w_release_zone_query(query);
    return 0;
}

static void w_load_zones(HWND window) {
    if (zone_count || zone_query) return;
    w_zone_query *query = calloc(1,sizeof(*query));
    if (query) {
        query->refs = 2;
        query->window = window;
        if (QueueUserWorkItem(w_enumerate_zones,query,WT_EXECUTEDEFAULT)) {
            zone_query = query;
            SetWindowTextW(zone_label,L"Loading time zones…");
            return;
        }
        free(query);
    }
    SetWindowTextW(zone_label,L"Time zones unavailable");
}

static void w_zone_choices(w_zone_query *query) {
    zone_query = NULL;
    zone_count = query->count;
    zones = query->rows;
    query->rows = NULL;
    w_release_zone_query(query);
    if (!zone_count) {
        free(zones);
        zones = NULL;
        SetWindowTextW(zone_label,L"Time zones unavailable");
        return;
    }
    SetWindowTextW(zone_label,L"Time zone");
    DYNAMIC_TIME_ZONE_INFORMATION current = {0};
    GetDynamicTimeZoneInformation(&current);
    // The native list fixes its height when opened (often with just the
    // current zone). Reopen it at the new item count for the first click.
    bool dropped = SendMessageW(zone,CB_GETDROPPEDSTATE,0,0) != 0;
    if (dropped) SendMessageW(zone,CB_SHOWDROPDOWN,FALSE,0);
    SendMessageW(zone, WM_SETREDRAW, FALSE, 0);
    SendMessageW(zone, CB_RESETCONTENT, 0, 0);
    SendMessageW(zone, CB_INITSTORAGE, zone_count, zone_count * sizeof(current.StandardName));
    for (DWORD i = 0; i < zone_count; ++i) {
        LRESULT row = SendMessageW(zone, CB_ADDSTRING, 0, (LPARAM)zones[i].StandardName);
        if (row < 0) continue;
        SendMessageW(zone, CB_SETITEMDATA, row, i);
        if (!wcscmp(current.TimeZoneKeyName, zones[i].TimeZoneKeyName))
            SendMessageW(zone, CB_SETCURSEL, row, 0);
    }
    SendMessageW(zone, WM_SETREDRAW, TRUE, 0);
    if (dropped) SendMessageW(zone,CB_SHOWDROPDOWN,TRUE,0);
    InvalidateRect(zone, NULL, TRUE);
    EnableWindow(save_zone, FALSE);
}

static BOOL w_zone_apply(const DYNAMIC_TIME_ZONE_INFORMATION *value) {
    HANDLE token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return FALSE;
    TOKEN_PRIVILEGES desired = {.PrivilegeCount = 1}, old = {0};
    DWORD bytes = sizeof(old);
    BOOL ok = LookupPrivilegeValueW(NULL, SE_TIME_ZONE_NAME, &desired.Privileges[0].Luid);
    desired.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (ok) ok = AdjustTokenPrivileges(token, FALSE, &desired, sizeof(old), &old, &bytes) &&
                 GetLastError() == ERROR_SUCCESS;
    if (ok) {
        ok = SetDynamicTimeZoneInformation(value);
        AdjustTokenPrivileges(token, FALSE, &old, 0, NULL, NULL);
    }
    CloseHandle(token);
    return ok;
}

static LRESULT CALLBACK w_clock_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    LRESULT result;
    if (w_theme_message(w, msg, wp, lp, &result)) return result;
    if (msg == zone_list_ready) {
        if (zone_query && zone_query == (w_zone_query *)lp) w_zone_choices(zone_query);
        return 0;
    }
    if (msg == WM_COMMAND && today && (HWND)lp == today && HIWORD(wp) == BN_CLICKED) {
        SYSTEMTIME now;
        GetLocalTime(&now);
        SendMessageW(calendar, MCM_SETCURSEL, 0, (LPARAM)&now);
        return 0;
    }
    if (msg == WM_COMMAND && (HWND)lp == zone && HIWORD(wp) == CBN_SELCHANGE) {
        EnableWindow(save_zone, zone_count != 0);
        return 0;
    }
    if (msg == WM_COMMAND && zone && (HWND)lp == zone &&
        (HIWORD(wp) == CBN_SETFOCUS || HIWORD(wp) == CBN_DROPDOWN)) {
        w_load_zones(w);
        return 0;
    }
    if (msg == WM_COMMAND && LOWORD(wp) == IDCANCEL && !lp) {
        DestroyWindow(w);
        return 0;
    }
    if (msg == WM_COMMAND && save_zone && (HWND)lp == save_zone && HIWORD(wp) == BN_CLICKED) {
        LRESULT row = SendMessageW(zone, CB_GETCURSEL, 0, 0);
        LRESULT id = SendMessageW(zone, CB_GETITEMDATA, row, 0);
        if (id >= 0 && (DWORD)id < zone_count && w_zone_apply(&zones[id])) {
            EnableWindow(save_zone, FALSE);
            SendNotifyMessageW(HWND_BROADCAST, WM_TIMECHANGE, 0, 0);
            w_clock_tick();
        } else MessageBoxW(w, L"Windows could not apply this time zone with your account permissions.",
                           L"Time zone", MB_OK | MB_ICONINFORMATION);
        return 0;
    }
    if (msg == WM_TIMER || msg == WM_TIMECHANGE) { w_clock_tick(); return 0; }
    if (calendar && (msg == WM_SYSCOLORCHANGE || msg == WM_THEMECHANGED || msg == WM_SETTINGCHANGE))
        w_calendar_colors();
    if (msg == WM_DPICHANGED) {
        RECT *r = (RECT *)lp;
        SetWindowPos(w, NULL, r->left, r->top, r->right-r->left, r->bottom-r->top, SWP_NOZORDER);
        w_clock_layout(w);
        return 0;
    }
    if (msg == WM_DESTROY) {
        if (zone_query) {
            InterlockedExchange(&zone_query->cancelled,1);
            w_release_zone_query(zone_query);
            zone_query = NULL;
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

int w_clock_run(void) {
    INITCOMMONCONTROLSEX common = {sizeof(common), ICC_DATE_CLASSES};
    InitCommonControlsEx(&common);
    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSW wc = {.lpfnWndProc = w_clock_proc, .hInstance = inst, .lpszClassName = L"WinwmCalendar",
                    .hCursor = LoadCursorW(NULL, IDC_ARROW)};
    RegisterClassW(&wc);
    MONITORINFO mon = {.cbSize = sizeof(mon)};
    UINT dpi = w_tool_work_area(&mon);
    int width = MulDiv(340, dpi, 96);
    HWND w = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"Date and time", WS_CAPTION|WS_SYSMENU,
                            (mon.rcWork.left+mon.rcWork.right-width)/2, (mon.rcWork.top+mon.rcWork.bottom-width)/2,
                            width, width, NULL, NULL, inst, NULL);
    if (!w) return 1;
    DWORD child = WS_CHILD|WS_VISIBLE|WS_TABSTOP;
    clock_text = CreateWindowW(L"STATIC", L"", WS_CHILD|WS_VISIBLE|SS_CENTERIMAGE|SS_ENDELLIPSIS, 0,0,0,0,w,NULL,inst,NULL);
    date_text = CreateWindowW(L"STATIC", L"", WS_CHILD|WS_VISIBLE|SS_CENTERIMAGE|SS_ENDELLIPSIS, 0,0,0,0,w,NULL,inst,NULL);
    zone_label = CreateWindowW(L"STATIC", L"Time zone", WS_CHILD|WS_VISIBLE|SS_CENTERIMAGE, 0,0,0,0,w,NULL,inst,NULL);
    calendar = CreateWindowW(MONTHCAL_CLASSW, L"Calendar", child|MCS_WEEKNUMBERS|MCS_NOTODAY, 0,0,0,0,w,NULL,inst,NULL);
    zone = CreateWindowW(L"COMBOBOX", L"", child|CBS_DROPDOWNLIST|CBS_SORT|WS_VSCROLL, 0,0,0,0,w,NULL,inst,NULL);
    today = CreateWindowW(L"BUTTON", L"Today", child|BS_OWNERDRAW, 0,0,0,0,w,NULL,inst,NULL);
    save_zone = CreateWindowW(L"BUTTON", L"Apply time zone", child|BS_OWNERDRAW, 0,0,0,0,w,NULL,inst,NULL);
    if (!calendar || !zone || !clock_text || !date_text || !zone_label || !today || !save_zone) { DestroyWindow(w); return 1; }
    SetWindowTheme(calendar,L"",L"");
    w_calendar_colors();
    w_icon_button(today,L"\uE8BF",L"Go to today");
    w_icon_button(save_zone,L"\uE8FB",L"Apply selected time zone");
    DYNAMIC_TIME_ZONE_INFORMATION current = {0};
    if (GetDynamicTimeZoneInformation(&current) != TIME_ZONE_ID_INVALID) {
        SendMessageW(zone, CB_ADDSTRING, 0, (LPARAM)current.StandardName);
        SendMessageW(zone, CB_SETCURSEL, 0, 0);
    }
    EnableWindow(save_zone, FALSE);
    w_clock_tick();
    w_clock_layout(w);
    SetTimer(w, 1, 1000, NULL);
    if (w_tool_show(w)) SetFocus(calendar);
    MSG m;
    while (GetMessageW(&m,NULL,0,0) > 0) {
        if (m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE &&
            !SendMessageW(zone, CB_GETDROPPEDSTATE, 0, 0)) DestroyWindow(w);
        else if (!IsDialogMessageW(w,&m)) { TranslateMessage(&m); DispatchMessageW(&m); }
    }
    DeleteObject(font);
    DeleteObject(time_font);
    free(zones);
    return 0;
}
