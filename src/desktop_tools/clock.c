#include "desktop_tools/tools.h"
#include "theme/theme.h"
#include <commctrl.h>
#include <stdio.h>

static HWND calendar, zone, clock_text, today, save_zone;
static DYNAMIC_TIME_ZONE_INFORMATION *zones;
static DWORD zone_count;
static HFONT font;

static void w_clock_tick(void) {
    SYSTEMTIME now;
    wchar_t time[80], date[128], text[224];
    GetLocalTime(&now);
    if (GetTimeFormatEx(NULL, 0, &now, NULL, time, count_of(time)) &&
        GetDateFormatEx(NULL, DATE_LONGDATE, &now, NULL, date, count_of(date), NULL)) {
        swprintf(text, count_of(text), L"%ls     %ls", time, date);
        SetWindowTextW(clock_text, text);
    }
}

static void w_clock_layout(HWND window) {
    UINT dpi = GetDpiForWindow(window);
    HFONT next = CreateFontW(-MulDiv(15, dpi, 96), 0,0,0, FW_NORMAL, FALSE,FALSE,FALSE,
                             DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
    HWND controls[] = {calendar, zone, clock_text, today, save_zone};
    for (size_t i = 0; i < count_of(controls); ++i)
        SendMessageW(controls[i], WM_SETFONT, (WPARAM)next, TRUE);
    DeleteObject(font);
    font = next;
    RECT cal = {0}, client;
    SendMessageW(calendar, MCM_GETMINREQRECT, 0, (LPARAM)&cal);
    GetClientRect(window, &client);
    int gap = MulDiv(12, dpi, 96), button = MulDiv(34, dpi, 96);
    int width = client.right - 2*gap, y = 2*gap + button;
    MoveWindow(clock_text, gap, gap, width, button, TRUE);
    MoveWindow(calendar, (client.right-cal.right)/2, y, cal.right, cal.bottom, TRUE);
    y += cal.bottom + gap;
    MoveWindow(zone, gap, y, width, 8*button, TRUE);
    y += gap + button;
    MoveWindow(today, gap, y, width/3, button, TRUE);
    MoveWindow(save_zone, 2*gap+width/3, y, width-width/3-gap, button, TRUE);
    RECT desired = {0, 0, client.right, y+button+gap};
    AdjustWindowRectExForDpi(&desired, GetWindowLongW(window, GWL_STYLE), FALSE,
                             GetWindowLongW(window, GWL_EXSTYLE), dpi);
    SetWindowPos(window, NULL, 0,0, desired.right-desired.left, desired.bottom-desired.top,
                 SWP_NOACTIVATE|SWP_NOMOVE|SWP_NOZORDER);
}

static void w_zone_choices(void) {
    DYNAMIC_TIME_ZONE_INFORMATION current = {0};
    GetDynamicTimeZoneInformation(&current);
    for (DWORD i = 0; ; ++i) {
        DYNAMIC_TIME_ZONE_INFORMATION entry;
        if (EnumDynamicTimeZoneInformation(i, &entry) != ERROR_SUCCESS) break;
        DYNAMIC_TIME_ZONE_INFORMATION *next = realloc(zones, (zone_count + 1) * sizeof(*zones));
        if (!next) break;
        zones = next;
        LRESULT row = SendMessageW(zone, CB_ADDSTRING, 0, (LPARAM)entry.StandardName);
        if (row < 0) continue;
        zones[zone_count] = entry;
        SendMessageW(zone, CB_SETITEMDATA, row, zone_count++);
        if (!wcscmp(current.TimeZoneKeyName, entry.TimeZoneKeyName))
            SendMessageW(zone, CB_SETCURSEL, row, 0);
    }
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
    if (msg == WM_COMMAND && today && (HWND)lp == today) {
        SYSTEMTIME now;
        GetLocalTime(&now);
        SendMessageW(calendar, MCM_SETCURSEL, 0, (LPARAM)&now);
        return 0;
    }
    if (msg == WM_COMMAND && (HWND)lp == zone && HIWORD(wp) == CBN_SELCHANGE) {
        EnableWindow(save_zone, TRUE);
        return 0;
    }
    if (msg == WM_COMMAND && LOWORD(wp) == IDCANCEL && !lp) {
        DestroyWindow(w);
        return 0;
    }
    if (msg == WM_COMMAND && (HWND)lp == save_zone) {
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
    if (msg == WM_DPICHANGED) {
        RECT *r = (RECT *)lp;
        SetWindowPos(w, NULL, r->left, r->top, r->right-r->left, r->bottom-r->top, SWP_NOZORDER);
        w_clock_layout(w);
        return 0;
    }
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
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
    int width = MulDiv(408, dpi, 96);
    HWND w = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"Date and time", WS_CAPTION|WS_SYSMENU,
                            (mon.rcWork.left+mon.rcWork.right-width)/2, (mon.rcWork.top+mon.rcWork.bottom-width)/2,
                            width, width, NULL, NULL, inst, NULL);
    if (!w) return 1;
    DWORD child = WS_CHILD|WS_VISIBLE|WS_TABSTOP;
    clock_text = CreateWindowW(L"STATIC", L"", WS_CHILD|WS_VISIBLE|SS_CENTER, 0,0,0,0,w,NULL,inst,NULL);
    calendar = CreateWindowW(MONTHCAL_CLASSW, L"Calendar", child|MCS_WEEKNUMBERS, 0,0,0,0,w,NULL,inst,NULL);
    zone = CreateWindowW(L"COMBOBOX", L"", child|CBS_DROPDOWNLIST|CBS_SORT|WS_VSCROLL, 0,0,0,0,w,NULL,inst,NULL);
    today = CreateWindowW(L"BUTTON", L"Today", child|BS_OWNERDRAW, 0,0,0,0,w,NULL,inst,NULL);
    save_zone = CreateWindowW(L"BUTTON", L"Apply time zone", child|BS_OWNERDRAW, 0,0,0,0,w,NULL,inst,NULL);
    w_zone_choices();
    w_clock_tick();
    w_clock_layout(w);
    SetTimer(w, 1, 1000, NULL);
    ShowWindow(w, SW_SHOW);
    SetFocus(calendar);
    MSG m;
    while (GetMessageW(&m,NULL,0,0) > 0) {
        if (m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE &&
            !SendMessageW(zone, CB_GETDROPPEDSTATE, 0, 0)) DestroyWindow(w);
        else if (!IsDialogMessageW(w,&m)) { TranslateMessage(&m); DispatchMessageW(&m); }
    }
    DeleteObject(font);
    free(zones);
    return 0;
}
