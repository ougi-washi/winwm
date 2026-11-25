#include "theme/dialog.h"
#include "desktop_tools/tools.h"
#include "desktop_tools/calendar.h"
#include "theme/theme.h"
#include "theme/icon_button.h"
#include <commctrl.h>
#include <stdio.h>

static HWND calendar, zone, clock_text, date_text, zone_label, today, save_zone;
static DYNAMIC_TIME_ZONE_INFORMATION *zones;
static DWORD zone_count;
static HFONT font, time_font;
static UINT font_dpi;
static int date_width, time_width;
static bool zone_pending;
static w_tool_choice zone_choice = {.row=-1};
static DYNAMIC_TIME_ZONE_INFORMATION active_zone;

enum { zone_list_ready = WM_APP + 1 };
typedef struct {
    LONG refs, cancelled;
    HWND window;
    DYNAMIC_TIME_ZONE_INFORMATION *rows;
    DWORD count;
} w_zone_query;
static w_zone_query *zone_query;

static void w_clock_tick(void) {
    SYSTEMTIME now;
    wchar_t time[80], date[128];
    GetLocalTime(&now);
    w_calendar_today(&now,false);
    wchar_t previous[128];
    GetWindowTextW(clock_text,previous,count_of(previous));
    if (GetTimeFormatEx(NULL, 0, &now, NULL, time, count_of(time)) && wcscmp(previous,time))
        SetWindowTextW(clock_text, time);
    GetWindowTextW(date_text,previous,count_of(previous));
    if (GetDateFormatEx(NULL, DATE_LONGDATE, &now, NULL, date, count_of(date), NULL) && wcscmp(previous,date)) {
        SetWindowTextW(date_text, date);
        date_width=0;
        if (font_dpi) PostMessageW(GetParent(date_text),w_calendar_names_changed,0,0);
    }
}

static void w_clock_layout(HWND window) {
    static bool sizing;
    if (sizing) return;
    UINT dpi = GetDpiForWindow(window);
    if (font_dpi != dpi) {
        HFONT next = w_theme_message_font(dpi);
        HFONT heading = w_theme_role_font(dpi,text_title);
        if (next && heading) {
            HWND controls[] = {calendar, zone, date_text, zone_label, today, save_zone};
            for (size_t i = 0; i < count_of(controls); ++i)
                SendMessageW(controls[i], WM_SETFONT, (WPARAM)next, FALSE);
            SendMessageW(clock_text,WM_SETFONT,(WPARAM)heading,FALSE);
            DeleteObject(font);
            DeleteObject(time_font);
            font = next;
            time_font = heading;
            font_dpi = dpi;
            date_width=time_width=0;
        } else {
            DeleteObject(next);
            DeleteObject(heading);
        }
    }
    RECT client, outer;
    w_theme_content_rect(window, &client);
    int gap = w_theme_spacing(dpi), pad = 2*gap, button = w_theme_button_size(dpi);
    int line = w_theme_role_line_height(dpi,text_body);
    int time_height=max(button,w_theme_role_line_height(dpi,text_title));
    int chrome = 2*(pad+gap+line)+time_height+button;
    if (!date_width) {
        wchar_t value[128];
        GetWindowTextW(date_text,value,count_of(value));
        date_width=w_theme_text_width(font,value,DT_NOPREFIX);
    }
    if (!time_width) {
        SYSTEMTIME sample;
        GetLocalTime(&sample);
        sample.wMinute=sample.wSecond=58;
        for (int hour=11;hour<=23;hour+=12) {
            wchar_t value[80];
            sample.wHour=(WORD)hour;
            if (GetTimeFormatEx(NULL,0,&sample,NULL,value,count_of(value))) {
                int measured=w_theme_text_width(time_font,value,DT_NOPREFIX);
                time_width=max(time_width,measured);
            }
        }
    }
    GetWindowRect(window,&outer);
    int content_width=w_calendar_width(dpi);
    content_width=max(content_width,max(date_width,time_width+button+gap));
    int width = content_width+2*pad+outer.right-outer.left-client.right;
    int height = chrome+w_calendar_height(dpi)+outer.bottom-outer.top-client.bottom;
    RECT fitted = w_tool_fit_to_monitor((RECT){outer.left,outer.top,outer.left+width,outer.top+height},
                                        MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST));
    if (!EqualRect(&outer,&fitted)) {
        sizing = true;
        SetWindowPos(window,NULL,fitted.left,fitted.top,fitted.right-fitted.left,fitted.bottom-fitted.top,
                     SWP_NOACTIVATE|SWP_NOZORDER);
        sizing = false;
        w_theme_content_rect(window,&client);
    }
    width = max(1,client.right-2*pad);
    int month_height = max(1,client.bottom-chrome);
    int y = pad + gap + time_height + line;
    w_tool_place(clock_text, pad, pad, width-button-gap, time_height);
    w_tool_place(today, pad+width-button, pad+(time_height-button)/2, button, button);
    w_tool_place(date_text, pad, pad+time_height, width, line);
    w_calendar_place(pad,y,width,month_height);
    y += month_height + gap;
    w_tool_place(zone_label, pad, y, width, line);
    y += line;
    w_tool_place_combo(zone, pad, y, width-(zone_pending ? button+gap : 0), button, 7);
    w_tool_place(save_zone, pad+width-button, y, button, button);
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
            EnableWindow(zone,FALSE);
            SetWindowTextW(zone_label,L"Loading time zones…");
            return;
        }
        free(query);
    }
    SetWindowTextW(zone_label,L"Time zones unavailable");
}

static bool w_same_zone(const DYNAMIC_TIME_ZONE_INFORMATION *left,const DYNAMIC_TIME_ZONE_INFORMATION *right) {
    return *left->TimeZoneKeyName || *right->TimeZoneKeyName ? !_wcsicmp(left->TimeZoneKeyName,right->TimeZoneKeyName) :
                                                               !_wcsicmp(left->StandardName,right->StandardName);
}

static bool w_zone_active(LRESULT row) {
    LRESULT id = row<0 ? CB_ERR : SendMessageW(zone,CB_GETITEMDATA,row,0);
    if (id<0 || (DWORD)id>=zone_count) return false;
    return w_same_zone(&zones[id],&active_zone);
}

static void w_zone_action(bool pending) {
    if (pending==zone_pending) return;
    zone_pending=pending;
    if (!pending && GetFocus()==save_zone) SetFocus(zone);
    w_clock_layout(GetParent(zone));
    ShowWindow(save_zone,pending ? SW_SHOWNA : SW_HIDE);
}

static void w_sync_zone(const DYNAMIC_TIME_ZONE_INFORMATION *active) {
    active_zone = *active;
    if (!zone_count || zone_choice.open) return;
    int selected = zone_choice.row;
    if (w_zone_active(selected)) w_zone_action(false);
    if (!zone_pending && !w_zone_active(selected)) {
        LRESULT count = SendMessageW(zone,CB_GETCOUNT,0,0), row = 0;
        while (row<count && !w_zone_active(row)) ++row;
        zone_choice.row = row<count ? (int)row : -1;
        SendMessageW(zone,CB_SETCURSEL,zone_choice.row,0);
    }
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
        EnableWindow(zone,TRUE);
        return;
    }
    SetWindowTextW(zone_label,L"Time zone");
    DYNAMIC_TIME_ZONE_INFORMATION current = {0};
    GetDynamicTimeZoneInformation(&current);
    SendMessageW(zone, WM_SETREDRAW, FALSE, 0);
    SendMessageW(zone, CB_RESETCONTENT, 0, 0);
    SendMessageW(zone, CB_INITSTORAGE, zone_count, zone_count * sizeof(current.StandardName));
    for (DWORD i = 0; i < zone_count; ++i) {
        LRESULT row = SendMessageW(zone, CB_ADDSTRING, 0, (LPARAM)zones[i].StandardName);
        if (row < 0) continue;
        SendMessageW(zone, CB_SETITEMDATA, row, i);
    }
    w_sync_zone(&current);
    SendMessageW(zone, WM_SETREDRAW, TRUE, 0);
    EnableWindow(zone,TRUE);
    InvalidateRect(zone,NULL,FALSE);
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
        w_calendar_finish_edit();
        w_calendar_today(&now,true);
        w_calendar_focus();
        return 0;
    }
    if (msg == WM_COMMAND && zone && (HWND)lp == zone) {
        UINT event = HIWORD(wp);
        if (w_tool_choice_changed(&zone_choice,zone,event)) {
            w_zone_action(zone_count && !w_zone_active(zone_choice.row));
        }
        if (event == CBN_CLOSEUP) w_sync_zone(&active_zone);
        if (event == CBN_SETFOCUS || event == CBN_DROPDOWN) w_load_zones(w);
        return 0;
    }
    if (msg == WM_COMMAND && LOWORD(wp) == IDCANCEL && !lp) {
        DestroyWindow(w);
        return 0;
    }
    if (msg == WM_COMMAND && zone_pending && (HWND)lp == save_zone && HIWORD(wp) == BN_CLICKED) {
        int row = zone_choice.row;
        LRESULT id = SendMessageW(zone, CB_GETITEMDATA, row, 0);
        if (id >= 0 && (DWORD)id < zone_count && w_zone_apply(&zones[id])) {
            w_zone_action(false);
            w_sync_zone(&zones[id]);
            SendNotifyMessageW(HWND_BROADCAST, WM_TIMECHANGE, 0, 0);
            w_clock_tick();
        } else w_dialog(w, L"Windows could not apply this time zone with your account permissions.",
                           L"Time zone", MB_OK | MB_ICONINFORMATION);
        return 0;
    }
    if ((msg == WM_TIMER && wp == 1) || msg == WM_TIMECHANGE) {
        if (msg==WM_TIMECHANGE) {
            DYNAMIC_TIME_ZONE_INFORMATION active;
            if (GetDynamicTimeZoneInformation(&active)!=TIME_ZONE_ID_INVALID && !w_same_zone(&active,&active_zone))
                w_sync_zone(&active);
        }
        if (IsWindowVisible(w) && !IsIconic(w)) w_clock_tick();
        return 0;
    }
    if (msg == WM_SIZE && wp != SIZE_MINIMIZED && save_zone) { w_clock_layout(w); return 0; }
    if (msg==w_calendar_names_changed && save_zone) { w_clock_layout(w); return 0; }
    if (msg==WM_SETTINGCHANGE && !wp && calendar && (!lp || !lstrcmpiW((LPCWSTR)lp,L"intl"))) {
        time_width=0;
        SendMessageW(calendar,msg,wp,lp);
        w_clock_tick();
        w_clock_layout(w);
    }
    if (calendar && (msg==winwm_motion_updated ||
        (msg==WM_SETTINGCHANGE && (wp==SPI_SETCLIENTAREAANIMATION || wp==SPI_SETHIGHCONTRAST))))
        SendMessageW(calendar,winwm_motion_updated,0,0);
    if (w_tool_font_change(w,msg,wp,lp)) {
        if (msg == WM_SETTINGCHANGE) font_dpi = 0;
        if (save_zone) w_clock_layout(w);
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
    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSW wc = {.lpfnWndProc = w_clock_proc, .hInstance = inst, .lpszClassName = L"WinwmCalendar",
                    .hCursor = LoadCursorW(NULL, IDC_ARROW)};
    RegisterClassW(&wc);
    MONITORINFO mon = {.cbSize = sizeof(mon)};
    UINT dpi = w_tool_work_area(&mon);
    int width = MulDiv(340, dpi, 96);
    HWND w = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"Date and time", WS_CAPTION|WS_SYSMENU|WS_CLIPCHILDREN,
                            (mon.rcWork.left+mon.rcWork.right-width)/2, (mon.rcWork.top+mon.rcWork.bottom-width)/2,
                            width, width, NULL, NULL, inst, NULL);
    if (!w) return 1;
    DWORD child = WS_CHILD|WS_VISIBLE|WS_TABSTOP;
    clock_text = CreateWindowW(L"STATIC", L"", WS_CHILD|WS_VISIBLE|SS_CENTERIMAGE|SS_ENDELLIPSIS, 0,0,0,0,w,NULL,inst,NULL);
    date_text = CreateWindowW(L"STATIC", L"", WS_CHILD|WS_VISIBLE|SS_CENTERIMAGE|SS_ENDELLIPSIS, 0,0,0,0,w,NULL,inst,NULL);
    zone_label = CreateWindowW(L"STATIC", L"Time zone", WS_CHILD|WS_VISIBLE|SS_CENTERIMAGE, 0,0,0,0,w,NULL,inst,NULL);
    calendar = w_calendar_create(w);
    zone = CreateWindowW(L"COMBOBOX", L"", child|w_tool_dropdown_list|CBS_SORT|WS_VSCROLL, 0,0,0,0,w,NULL,inst,NULL);
    today = CreateWindowW(L"BUTTON", L"Today", child|BS_OWNERDRAW, 0,0,0,0,w,NULL,inst,NULL);
    save_zone = CreateWindowW(L"BUTTON", L"Apply time zone", WS_CHILD|WS_TABSTOP|BS_OWNERDRAW, 0,0,0,0,w,NULL,inst,NULL);
    if (!calendar || !zone || !clock_text || !date_text || !zone_label || !today || !save_zone) { DestroyWindow(w); return 1; }
    w_icon_button(today,L"\uE8BF",L"Go to today");
    w_icon_button(save_zone,L"\uE8FB",L"Apply selected time zone");
    DYNAMIC_TIME_ZONE_INFORMATION current = {0};
    if (GetDynamicTimeZoneInformation(&current) != TIME_ZONE_ID_INVALID) {
        SendMessageW(zone, CB_ADDSTRING, 0, (LPARAM)current.StandardName);
        SendMessageW(zone, CB_SETCURSEL, 0, 0);
    }
    w_clock_tick();
    w_clock_layout(w);
    SetTimer(w, 1, 1000, NULL);
    if (w_tool_show(w)) {
        w_clock_layout(w);
        w_calendar_focus();
        w_load_zones(w);
    }
    MSG m;
    while (GetMessageW(&m,NULL,0,0) > 0) {
        if (w_native_ime_key(&m)) { TranslateMessage(&m); DispatchMessageW(&m); continue; }
        if (w_tool_repeated_dialog_key(&m)) continue;
        if (m.message==WM_KEYDOWN && m.wParam==VK_RETURN) {
            if (w_calendar_enter(m.hwnd)) continue;
            if (m.hwnd==zone && zone_pending && !SendMessageW(zone,CB_GETDROPPEDSTATE,0,0)) {
                SendMessageW(save_zone,BM_CLICK,0,0);
                continue;
            }
        }
        if (m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE &&
            !w_calendar_dropdown() && !SendMessageW(zone, CB_GETDROPPEDSTATE, 0, 0)) {
            if (!w_calendar_finish_edit()) SendMessageW(w,WM_CLOSE,0,0);
        }
        else if (!IsDialogMessageW(w,&m)) { TranslateMessage(&m); DispatchMessageW(&m); }
    }
    DeleteObject(font);
    DeleteObject(time_font);
    free(zones);
    return 0;
}
