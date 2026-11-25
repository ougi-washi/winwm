#define COBJMACROS
#include "desktop_tools/calendar.h"
#include "desktop_tools/tools.h"
#include "theme/icon_button.h"
#include "theme/disclosure.h"
#include "theme/theme.h"
#include <commctrl.h>
#include <oleacc.h>
#include <stdio.h>

// A six-week Gregorian month. Native buttons retain keyboard focus and expose
// a full localized date to accessibility clients, even for adjacent months.
enum { first_year = 1601, last_year = 9999, day_id = 200, days = 42 };
static HWND panel, previous, month_box, year_box, next, heading, weekdays[7], weeks[6], cells[days];
static SYSTEMTIME selected;
static int grid_start, today = -1, current = -1, first_weekday, minimum_week_days;
static int first_year_option;
static int month_ink,year_ink;
static bool setting_fields, editing, locale_pending;
enum { editor_timer=1 };
static w_disclosure editor;
static w_tool_choice month_choice;
static w_tool_edit_choice year_edit = {.choice.row=-1};
static wchar_t numbers[days][16];
static const LONGLONG day_ticks = 864000000000LL;
static IAccPropServices *accessibility;
static HRESULT apartment = E_FAIL;

static void w_heading_description(void) {
    if (accessibility)
        IAccPropServices_SetHwndPropStr(accessibility,heading,OBJID_CLIENT,CHILDID_SELF,PROPID_ACC_DESCRIPTION,
            editing ? L"Month and year fields are expanded." : L"Expand the month and year fields.");
}

static int w_ordinal(const SYSTEMTIME *date) {
    SYSTEMTIME midnight = {.wYear=date->wYear,.wMonth=date->wMonth,.wDay=date->wDay};
    FILETIME file;
    if (!SystemTimeToFileTime(&midnight,&file)) return -1;
    ULARGE_INTEGER value = {.LowPart=file.dwLowDateTime,.HighPart=file.dwHighDateTime};
    return (int)(value.QuadPart/day_ticks);
}

static SYSTEMTIME w_date(int ordinal) {
    SYSTEMTIME result = {0};
    if (ordinal >= 0) {
        ULARGE_INTEGER value = {.QuadPart=(ULONGLONG)ordinal*day_ticks};
        FILETIME file = {.dwLowDateTime=value.LowPart,.dwHighDateTime=value.HighPart};
        FileTimeToSystemTime(&file,&result);
    }
    return result;
}

static int w_month_length(int year,int month) {
    const int lengths[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    return lengths[month-1] + (month == 2 && year%4 == 0 && (year%100 != 0 || year%400 == 0));
}

static int w_week_offset(int ordinal) {
    // Day zero is a Monday, as is weekday zero in Windows locale settings.
    return ((ordinal-first_weekday)%7+7)%7;
}

static int w_first_week(int year) {
    int y = year-1;
    int jan1 = 365*(y-1600)+(y/4-400)-(y/100-16)+(y/400-4);
    int offset = w_week_offset(jan1);
    return jan1-offset+(7-offset < minimum_week_days ? 7 : 0);
}

static int w_week_number(int start) {
    int year = w_date(max(0,start+6)).wYear;
    int first = w_first_week(year);
    if (start < first) first = w_first_week(year-1);
    return (start-first)/7+1;
}

static void w_year_options(int year) {
    if (first_year_option && year >= first_year_option && year <= first_year_option+20) return;
    first_year_option = max(first_year,min(last_year-20,year-10));
    SendMessageW(year_box,CB_RESETCONTENT,0,0);
    for (int i=0;i<=20;++i) {
        wchar_t number[8];
        swprintf(number,count_of(number),L"%d",first_year_option+i);
        SendMessageW(year_box,CB_ADDSTRING,0,(LPARAM)number);
    }
}

static void w_update_dates(bool sync_fields) {
    if (!panel || !cells[days-1]) return;
    static int drawn_today = -1;
    int previous_start = grid_start, previous_day = current;
    int ordinal = w_ordinal(&selected);
    int month_start = ordinal+1-selected.wDay;
    grid_start = month_start-w_week_offset(month_start);
    current = ordinal-grid_start;
    bool new_grid = previous_day<0 || previous_start!=grid_start;
    if (sync_fields) {
        setting_fields = true;
        w_year_options(selected.wYear);
        month_choice.row = selected.wMonth-1;
        if (SendMessageW(month_box,CB_GETCURSEL,0,0) != month_choice.row)
            SendMessageW(month_box,CB_SETCURSEL,month_choice.row,0);
        wchar_t year[8];
        swprintf(year,count_of(year),L"%u",selected.wYear);
        w_tool_status(year_box,year);
        year_edit.choice.row = (int)SendMessageW(year_box,CB_GETCURSEL,0,0);
        setting_fields = false;
    }
    if (new_grid && !editing) {
        wchar_t title[120];
        if (GetDateFormatEx(NULL,DATE_YEARMONTH,&selected,NULL,title,count_of(title),NULL))
            w_labeled_button(heading,L"\uE70D",title);
    }
    EnableWindow(previous,selected.wYear!=first_year || selected.wMonth!=1);
    EnableWindow(next,selected.wYear!=last_year || selected.wMonth!=12);
    for (int row=0;new_grid && row<6;++row) {
        wchar_t number[8];
        swprintf(number,count_of(number),L"%d",w_week_number(grid_start+row*7));
        w_tool_status(weeks[row],number);
    }
    for (int i=0;i<days;++i) {
        if (!new_grid && i!=current && i!=previous_day && grid_start+i!=drawn_today && grid_start+i!=today) continue;
        SYSTEMTIME date = w_date(grid_start+i);
        bool valid = date.wYear >= first_year && date.wYear <= last_year;
        if (!!(GetWindowLongW(cells[i],GWL_STYLE)&WS_VISIBLE) != valid)
            ShowWindow(cells[i],valid ? SW_SHOWNA : SW_HIDE);
        if (!valid) continue;
        wchar_t full[160] = L"", label[200];
        if (!GetDateFormatEx(NULL,DATE_LONGDATE,&date,NULL,full,count_of(full),NULL))
            swprintf(full,count_of(full),L"%04u-%02u-%02u",date.wYear,date.wMonth,date.wDay);
        if (!GetDateFormatEx(NULL,0,&date,L"d",numbers[i],count_of(numbers[i]),NULL))
            swprintf(numbers[i],count_of(numbers[i]),L"%u",date.wDay);
        swprintf(label,count_of(label),L"%ls%ls%ls",full,grid_start+i == today ? L", today" : L"",i == current ? L", selected" : L"");
        w_tool_status(cells[i],label);
        LONG_PTR style = GetWindowLongPtrW(cells[i],GWL_STYLE);
        if (!!(style&WS_TABSTOP) != (i == current))
            SetWindowLongPtrW(cells[i],GWL_STYLE,i == current ? style|WS_TABSTOP : style&~(LONG_PTR)WS_TABSTOP);
        InvalidateRect(cells[i],NULL,FALSE);
    }
    drawn_today = today;
}

static void w_select_month(int year,int month,int day,bool sync_fields) {
    if (year<first_year || year>last_year || month<1 || month>12) return;
    selected = (SYSTEMTIME){.wYear=(WORD)year,.wMonth=(WORD)month,.wDay=(WORD)min(day,w_month_length(year,month))};
    w_update_dates(sync_fields);
}

static void w_move_month(int delta) {
    int m = selected.wYear*12+selected.wMonth-1+delta;
    w_select_month(m/12,m%12+1,selected.wDay,true);
}

void w_calendar_today(const SYSTEMTIME *now,bool select) {
    if (!panel) return;
    int day = w_ordinal(now);
    if (day<0 || (!select && day==today)) return;
    today = day;
    if (select) selected = *now;
    w_update_dates(select);
}

void w_calendar_focus(void) {
    if (current>=0) SetFocus(cells[current]);
}

bool w_calendar_dropdown(void) {
    return SendMessageW(month_box,CB_GETDROPPEDSTATE,0,0) || SendMessageW(year_box,CB_GETDROPPEDSTATE,0,0);
}

static void w_measure_editor(void) {
    HFONT font=(HFONT)SendMessageW(month_box,WM_GETFONT,0,0);
    month_ink=0;
    for (int i=0;i<12;i++) {
        wchar_t name[80];
        LRESULT length=SendMessageW(month_box,CB_GETLBTEXTLEN,i,0);
        if (length<0 || length>=(LRESULT)count_of(name)) continue;
        if (SendMessageW(month_box,CB_GETLBTEXT,i,(LPARAM)name)==CB_ERR) continue;
        int width=w_theme_text_width(font,name,DT_NOPREFIX);
        month_ink=max(month_ink,width);
    }
    year_ink=w_theme_text_width((HFONT)SendMessageW(year_box,WM_GETFONT,0,0),L"8888",DT_NOPREFIX);
}

static void w_locale(void) {
    if (month_choice.open || year_edit.choice.open) {
        locale_pending = true;
        return;
    }
    locale_pending = false;
    DWORD weekday = 0, rule = 0;
    GetLocaleInfoEx(NULL,LOCALE_IFIRSTDAYOFWEEK|LOCALE_RETURN_NUMBER,(LPWSTR)&weekday,2);
    GetLocaleInfoEx(NULL,LOCALE_IFIRSTWEEKOFYEAR|LOCALE_RETURN_NUMBER,(LPWSTR)&rule,2);
    first_weekday = weekday%7;
    minimum_week_days = rule==1 ? 7 : rule==2 ? 4 : 1;
    for (int i=0;i<7;++i) {
        wchar_t name[48] = L"";
        GetLocaleInfoEx(NULL,LOCALE_SSHORTESTDAYNAME1+(first_weekday+i)%7,name,count_of(name));
        w_tool_status(weekdays[i],name);
    }
    setting_fields = true;
    SendMessageW(month_box,CB_RESETCONTENT,0,0);
    for (int m=1;m<=12;++m) {
        wchar_t name[80] = L"";
        if (!GetCalendarInfoEx(NULL,CAL_GREGORIAN,NULL,CAL_SMONTHNAME1+m-1,name,count_of(name),NULL))
            swprintf(name,count_of(name),L"%d",m);
        SendMessageW(month_box,CB_ADDSTRING,0,(LPARAM)name);
    }
    month_choice.row = selected.wMonth-1;
    SendMessageW(month_box,CB_SETCURSEL,month_choice.row,0);
    setting_fields = false;
    int previous_width=month_ink;
    w_measure_editor();
    if (month_ink!=previous_width) PostMessageW(GetParent(panel),w_calendar_names_changed,0,0);
    current = -1; // Reformat every accessible date and weekday after a locale change.
    w_update_dates(!editing);
}

int w_calendar_width(UINT dpi) {
    int side=w_theme_button_size(dpi),gap=w_theme_spacing(dpi);
    int editor_width=3*side+4*gap+w_tool_selector_width(month_box,month_ink,dpi)+w_tool_selector_width(year_box,year_ink,dpi);
    return max(8*side,editor_width);
}

int w_calendar_height(UINT dpi) {
    return 7*w_theme_button_size(dpi)+w_theme_spacing(dpi)+w_theme_role_line_height(dpi,text_body);
}

static void w_layout_editor(int width,UINT dpi,bool resized) {
    int side=w_theme_button_size(dpi),gap=w_theme_spacing(dpi),left=side+gap;
    int header_width=max(side,width-2*left),travel=header_width-side;
    int revealed=MulDiv(travel,editor.value,255);
    int fields=max(2,travel-2*gap),year=min(fields/2,w_tool_selector_width(year_box,year_ink,dpi));
    int month=fields-year;
    if (resized) {
        w_tool_place(previous,0,0,side,side);
        w_tool_place(next,width-side,0,side,side);
    }
    w_tool_place(heading,left+revealed,0,header_width-revealed,side);
    HWND controls[]={month_box,year_box};
    int offsets[]={0,month+gap},widths[]={month,year};
    for (size_t i=0;i<count_of(controls);++i) {
        HWND field=controls[i];
        int visible=w_clamp_int(revealed-offsets[i],0,widths[i]);
        RECT clip={0,0,visible,side};
        bool show=editing || editor.value;
        if (!show && (GetWindowLongW(field,GWL_STYLE)&WS_VISIBLE)) ShowWindow(field,SW_HIDE);
        if (resized) w_tool_place_combo(field,left+offsets[i],0,widths[i],side,8);
        w_disclosure_clip(field,show && visible<widths[i] ? &clip : NULL);
        if (show && !(GetWindowLongW(field,GWL_STYLE)&WS_VISIBLE)) ShowWindow(field,SW_SHOWNA);
    }
}

static void w_layout_month(void) {
    if (!cells[days-1]) return;
    RECT r;
    GetClientRect(panel,&r);
    UINT dpi=GetDpiForWindow(panel);
    int side=w_theme_button_size(dpi),gap=w_theme_spacing(dpi);
    int weekday_height=w_theme_role_line_height(dpi,text_body),header=side+gap;
    w_layout_editor(r.right,dpi,true);
    int cell = max(1,min(side,min(r.right/8,(r.bottom-header-weekday_height)/6)));
    int x = (r.right-8*cell)/2;
    int y = header+weekday_height;
    for (int i=0;i<7;++i) w_tool_place(weekdays[i],x+(i+1)*cell,header,cell,weekday_height);
    for (int row=0;row<6;++row) {
        w_tool_place(weeks[row],x,y+row*cell,cell,cell);
        for (int col=0;col<7;++col) w_tool_place(cells[row*7+col],x+(col+1)*cell,y+row*cell,cell,cell);
    }
}

void w_calendar_place(int x,int y,int width,int height) {
    w_tool_place(panel,x,y,width,height);
    w_layout_month();
}

static void w_edit_month(bool open) {
    editing = open;
    w_heading_description();
    if (!open) w_calendar_focus();
    EnableWindow(month_box,open);
    EnableWindow(year_box,open);
    w_disclosure_to(panel,&editor,editor_timer,open);
    RECT r;
    GetClientRect(panel,&r);
    w_layout_editor(r.right,GetDpiForWindow(panel),false);
    if (open) w_icon_button(heading,L"\uE8FB",L"Done choosing month and year");
    else {
        wchar_t title[120]=L"Choose month and year";
        GetDateFormatEx(NULL,DATE_YEARMONTH,&selected,NULL,title,count_of(title),NULL);
        w_labeled_button(heading,L"\uE70D",title);
    }
    if (open) SetFocus(month_box);
}

bool w_calendar_finish_edit(void) {
    if (!editing) return false;
    w_edit_month(false);
    return true;
}

bool w_calendar_enter(HWND control) {
    if (control==heading) {
        w_edit_month(!editing);
        return true;
    }
    return editing && (control==month_box || control==year_box || IsChild(year_box,control)) &&
           !w_calendar_dropdown() && w_calendar_finish_edit();
}

static LRESULT CALLBACK w_day_proc(HWND w,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR data) {
    int i = (int)data;
    if (msg == WM_GETDLGCODE)
        return DefSubclassProc(w,msg,wp,lp)|DLGC_WANTARROWS|(wp==VK_RETURN ? DLGC_WANTMESSAGE : 0);
    if (msg == WM_KEYDOWN) {
        int day = grid_start+i, delta = 0;
        switch (wp) {
        case VK_RETURN: SendMessageW(w,BM_CLICK,0,0); return 0;
        case VK_LEFT: delta = -1; break;
        case VK_RIGHT: delta = 1; break;
        case VK_UP: delta = -7; break;
        case VK_DOWN: delta = 7; break;
        case VK_HOME: delta = 1-selected.wDay; break;
        case VK_END: delta = w_month_length(selected.wYear,selected.wMonth)-selected.wDay; break;
        case VK_PRIOR: case VK_NEXT:
            w_move_month((wp==VK_PRIOR ? -1 : 1)*(GetKeyState(VK_CONTROL)<0 ? 12 : 1));
            w_calendar_focus();
            return 0;
        default: return DefSubclassProc(w,msg,wp,lp);
        }
        SYSTEMTIME date = w_date(day+delta);
        w_select_month(date.wYear,date.wMonth,date.wDay,true);
        w_calendar_focus();
        return 0;
    }
    if (msg==WM_NCDESTROY) RemoveWindowSubclass(w,w_day_proc,id);
    return DefSubclassProc(w,msg,wp,lp);
}

static void w_draw_day(const DRAWITEMSTRUCT *item,int i) {
    HDC dc = item->hDC;
    RECT r = item->rcItem;
    int saved = SaveDC(dc);
    bool active = i==current || (item->itemState&ODS_SELECTED);
    SYSTEMTIME date = w_date(grid_start+i);
    COLORREF ink=w_theme_button_face(item,i==current,true);
    SetTextColor(dc,!active && date.wMonth!=selected.wMonth && !w_theme_high_contrast() ? w_theme_hint() : ink);
    SetBkMode(dc,TRANSPARENT);
    w_theme_draw_control_line(item->hwndItem,dc,numbers[i],&r,DT_CENTER|DT_VCENTER|DT_NOPREFIX);
    if (grid_start+i==today) {
        int center = (r.left+r.right)/2, mark = max(2,(r.right-r.left)/6);
        RECT line = {center-mark,r.bottom-3,center+mark,r.bottom-1};
        SetDCBrushColor(dc,w_theme_high_contrast() || (item->itemState&ODS_SELECTED) ? ink : w_theme_accent());
        FillRect(dc,&line,GetStockObject(DC_BRUSH));
    }
    if ((item->itemState&(ODS_FOCUS|ODS_NOFOCUSRECT))==ODS_FOCUS) DrawFocusRect(dc,&r);
    RestoreDC(dc,saved);
}

static void w_field_names(bool clear) {
    COMBOBOXINFO info = {.cbSize=sizeof(info)};
    GetComboBoxInfo(year_box,&info);
    HWND fields[] = {month_box,year_box,info.hwndItem};
    const wchar_t *names[] = {L"Month",L"Year",L"Year"};
    if (clear && accessibility)
        IAccPropServices_ClearHwndProps(accessibility,heading,OBJID_CLIENT,CHILDID_SELF,&PROPID_ACC_DESCRIPTION,1);
    else if (!clear) w_heading_description();
    for (size_t i=0;accessibility && i<count_of(fields);++i) {
        if (!fields[i]) continue;
        if (clear) IAccPropServices_ClearHwndProps(accessibility,fields[i],OBJID_CLIENT,CHILDID_SELF,&PROPID_ACC_NAME,1);
        else IAccPropServices_SetHwndPropStr(accessibility,fields[i],OBJID_CLIENT,CHILDID_SELF,PROPID_ACC_NAME,names[i]);
    }
}

static LRESULT CALLBACK w_calendar_proc(HWND w,UINT msg,WPARAM wp,LPARAM lp) {
    if ((msg==WM_TIMER && wp==editor_timer) || msg==winwm_motion_updated || (msg==WM_SHOWWINDOW && !wp)) {
        if (editor.running) {
            if (msg==WM_SHOWWINDOW) w_disclosure_stop(w,&editor,editor_timer,editing);
            else if (msg==WM_TIMER) w_disclosure_tick(w,&editor,editor_timer);
            else w_disclosure_to(w,&editor,editor_timer,editing);
            RECT r;
            GetClientRect(w,&r);
            w_layout_editor(r.right,GetDpiForWindow(w),false);
        }
        return 0;
    }
    if (msg==WM_DRAWITEM) {
        DRAWITEMSTRUCT *item = (DRAWITEMSTRUCT *)lp;
        if (item->CtlID>=day_id && item->CtlID<day_id+days) w_draw_day(item,item->CtlID-day_id);
        else {
            LRESULT result;
            if (!w_theme_message(w,msg,wp,lp,&result)) return DefWindowProcW(w,msg,wp,lp);
        }
        return TRUE;
    }
    if (msg==WM_COMMAND && !setting_fields) {
        HWND from = (HWND)lp;
        int event = HIWORD(wp), i = LOWORD(wp)-day_id;
        if (i>=0 && i<days && event==BN_CLICKED) {
            SYSTEMTIME date = w_date(grid_start+i);
            w_select_month(date.wYear,date.wMonth,date.wDay,true);
            w_calendar_focus();
        } else if (from==heading && event==BN_CLICKED) w_edit_month(!editing);
        else if (from==previous && event==BN_CLICKED) w_move_month(-1);
        else if (from==next && event==BN_CLICKED) w_move_month(1);
        else if (from==month_box && w_tool_choice_changed(&month_choice,month_box,event))
            w_select_month(selected.wYear,month_choice.row+1,selected.wDay,true);
        else if (from==year_box) {
            setting_fields = true;
            if (event==CBN_DROPDOWN && (selected.wYear<=first_year_option || selected.wYear>=first_year_option+20)) {
                wchar_t text[8];
                GetWindowTextW(year_box,text,count_of(text));
                DWORD range=(DWORD)SendMessageW(year_box,CB_GETEDITSEL,0,0);
                first_year_option=0;
                w_year_options(selected.wYear);
                SetWindowTextW(year_box,text);
                SendMessageW(year_box,CB_SETEDITSEL,0,range);
            }
            bool chosen = w_tool_edit_choice_changed(&year_edit,year_box,event);
            setting_fields = false;
            if (chosen) w_select_month(first_year_option+year_edit.choice.row,selected.wMonth,selected.wDay,true);
            else if (!year_edit.choice.open && (event==CBN_EDITCHANGE || event==CBN_KILLFOCUS)) {
                wchar_t text[8];
                GetWindowTextW(year_box,text,count_of(text));
                int year = wcslen(text)==4 && wcsspn(text,L"0123456789")==4 ? _wtoi(text) : 0;
                if (year>=first_year && year<=last_year && year!=selected.wYear)
                    w_select_month(year,selected.wMonth,selected.wDay,event!=CBN_EDITCHANGE);
                else if (event==CBN_KILLFOCUS && (year<first_year || year>last_year)) w_update_dates(true);
            }
        }
        if (event==CBN_CLOSEUP && locale_pending) w_locale();
        return 0;
    }
    if (msg==WM_SETFONT) {
        for (HWND c=GetWindow(w,GW_CHILD);c;c=GetWindow(c,GW_HWNDNEXT)) SendMessageW(c,msg,wp,lp);
        if (month_box && year_box) w_measure_editor();
        return 0;
    }
    if (msg==WM_SETTINGCHANGE && cells[days-1]) { w_locale(); return 0; }
    if (msg==WM_DESTROY) {
        w_disclosure_stop(w,&editor,editor_timer,false);
        w_field_names(true);
        if (accessibility) IAccPropServices_Release(accessibility);
        if (SUCCEEDED(apartment)) CoUninitialize();
        accessibility = NULL;
        apartment = E_FAIL;
    }
    if (msg==WM_ERASEBKGND || msg==WM_CTLCOLORSTATIC || msg==WM_CTLCOLOREDIT || msg==WM_CTLCOLORLISTBOX || msg==WM_CTLCOLORBTN) {
        LRESULT result;
        if (w_theme_message(w,msg,wp,lp,&result)) {
            if (msg==WM_CTLCOLORSTATIC && (HWND)lp!=heading) SetTextColor((HDC)wp,w_theme_hint());
            return result;
        }
    }
    return DefWindowProcW(w,msg,wp,lp);
}

static HWND w_child(const wchar_t *kind,const wchar_t *name,DWORD style,int id) {
    return CreateWindowW(kind,name,style|WS_CHILD|WS_VISIBLE,0,0,1,1,panel,(HMENU)(INT_PTR)id,GetModuleHandleW(NULL),NULL);
}

HWND w_calendar_create(HWND parent) {
    WNDCLASSW wc={.lpfnWndProc=w_calendar_proc,.hInstance=GetModuleHandleW(NULL),.lpszClassName=L"WinwmMonth",
                  .hCursor=LoadCursorW(NULL,IDC_ARROW)};
    RegisterClassW(&wc);
    panel=CreateWindowExW(WS_EX_CONTROLPARENT,wc.lpszClassName,L"Calendar",WS_CHILD|WS_VISIBLE|WS_CLIPCHILDREN,
                          0,0,1,1,parent,NULL,wc.hInstance,NULL);
    if (!panel) return NULL;
    apartment=CoInitializeEx(NULL,COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(apartment) || apartment==RPC_E_CHANGED_MODE)
        CoCreateInstance(&CLSID_AccPropServices,NULL,CLSCTX_INPROC_SERVER,&IID_IAccPropServices,(void **)&accessibility);
    DWORD tab=WS_TABSTOP, button=BS_OWNERDRAW;
    previous=w_child(L"BUTTON",L"Previous month",tab|button,100);
    heading=w_child(L"BUTTON",L"Choose month and year",tab|button,104);
    month_box=w_child(L"COMBOBOX",L"Month",tab|w_tool_dropdown_list|WS_VSCROLL,101);
    year_box=w_child(L"COMBOBOX",L"Year",tab|w_tool_dropdown_edit|CBS_AUTOHSCROLL|WS_VSCROLL,102);
    next=w_child(L"BUTTON",L"Next month",tab|button,103);
    if (!previous || !next || !month_box || !year_box || !heading) return NULL;
    w_field_names(false);
    SendMessageW(year_box,CB_LIMITTEXT,4,0);
    w_icon_button(previous,L"\uE76B",L"Previous month");
    w_icon_button(next,L"\uE76C",L"Next month");
    ShowWindow(month_box,SW_HIDE);
    ShowWindow(year_box,SW_HIDE);
    for (int i=0;i<7;++i)
        if (!(weekdays[i]=w_child(L"STATIC",L"",SS_CENTER|SS_CENTERIMAGE|SS_NOPREFIX,110+i))) return NULL;
    for (int i=0;i<6;++i)
        if (!(weeks[i]=w_child(L"STATIC",L"",SS_CENTER|SS_CENTERIMAGE|SS_NOPREFIX,120+i))) return NULL;
    for (int i=0;i<days;++i) {
        cells[i]=w_child(L"BUTTON",L"",button,day_id+i);
        if (!cells[i] || !SetWindowSubclass(cells[i],w_day_proc,1,i)) return NULL;
    }
    GetLocalTime(&selected);
    today = w_ordinal(&selected);
    w_locale();
    return panel;
}
