#include "notifications/view.h"
#include "theme/theme.h"
#include "theme/icon_button.h"
#include "win_platform/layer.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <imm.h>
#include <shellscalingapi.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <windowsx.h>

enum { max_entries=notice_history_max, max_toasts=notice_visible_max, expiry_timer=1, list_id=101, search_id, clear_id,
       dismiss_id, expand_id, close_id, access_id };
static const wchar_t panel_class[]=L"WinwmNotifications", toast_class[]=L"WinwmNotificationToast";
typedef struct {
    uint64_t id, revision;
    DWORD native_id;
    bool has_native_id, await_echo;
    FILETIME time, native_time;
    wchar_t *app, *title, *body;
    wchar_t stamp[96];
    wchar_t strings[];
} w_entry;

typedef struct { HFONT regular, bold, small; UINT dpi; } w_fonts;
typedef struct {
    HWND hwnd;
    uint64_t id;
    HMONITOR monitor;
    w_layer_fade fade;
    w_fonts fonts;
    ULONGLONG deadline;
    uint64_t balloon;
    void (*balloon_event)(uint64_t,UINT);
    UINT remaining;
    bool hover, closing, pressed;
} w_toast;

static w_notification_config settings;
static HWND owner;
static bool (*read_work)(HMONITOR,RECT *);
static bool motion;
static uint64_t serial;
static w_entry *history[max_entries], *matches[max_entries];
static int entry_count, match_count;
static bool batching, filter_pending;
static w_toast toasts[max_toasts];
static struct {
    HWND hwnd, search, list, detail, clear, dismiss, expand, close, access, status, empty, previous;
    HMONITOR monitor;
    w_popup_anchor anchor;
    RECT anchor_bar;
    HIMAGELIST spacing;
    w_layer_fade fade;
    w_fonts fonts;
    uint64_t selected, displayed;
    bool expanded, filtering, closing, composing;
} panel;
static wchar_t access_status[256];
static void (*request_access)(HWND button);

static void w_layout_panel(void);
static void w_filter(bool reset);
static void w_close_panel(bool restore);
static LRESULT CALLBACK w_panel_proc(HWND,UINT,WPARAM,LPARAM);
static LRESULT CALLBACK w_toast_proc(HWND,UINT,WPARAM,LPARAM);

static int px(int dip,UINT dpi) { return MulDiv(dip,dpi ? dpi : 96,96); }

static UINT w_monitor_dpi(HMONITOR monitor) {
    UINT x=96,y=96;
    GetDpiForMonitor(monitor,MDT_EFFECTIVE_DPI,&x,&y);
    return x;
}

static HMONITOR w_target_monitor(void) {
    HWND active=GetForegroundWindow();
    if (active && !w_os_desktop_surface(active))
        return MonitorFromWindow(active,MONITOR_DEFAULTTONEAREST);
    POINT p={0};
    GetCursorPos(&p);
    return MonitorFromPoint(p,MONITOR_DEFAULTTONEAREST);
}

static RECT w_work(HMONITOR *monitor) {
    MONITORINFO m={.cbSize=sizeof(m)};
    if (!*monitor || !GetMonitorInfoW(*monitor,&m)) {
        *monitor=w_target_monitor();
        GetMonitorInfoW(*monitor,&m);
    }
    RECT work;
    return read_work && read_work(*monitor,&work) && !IsRectEmpty(&work) ? work : m.rcWork;
}

void w_notice_work_area(bool (*read)(HMONITOR,RECT *)) { read_work=read; }

static void w_fonts_free(w_fonts *f) {
    if (f->regular) DeleteObject(f->regular);
    if (f->bold) DeleteObject(f->bold);
    if (f->small) DeleteObject(f->small);
    *f=(w_fonts){0};
}

static HFONT w_font(int size, int weight,UINT dpi) {
    return CreateFontW(-px(size,dpi),0,0,0,weight,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
}

static void w_fonts_at(w_fonts *f,UINT dpi) {
    if (f->dpi==dpi) return;
    w_fonts_free(f);
    f->dpi=dpi; f->regular=w_font(15,FW_NORMAL,dpi); f->bold=w_font(16,FW_SEMIBOLD,dpi);
    f->small=w_font(13,FW_NORMAL,dpi);
}

static void w_text(HDC dc,RECT box,const wchar_t *text,HFONT font,COLORREF color,UINT flags) {
    if (font) SelectObject(dc,font);
    SetTextColor(dc,color);
    SetBkMode(dc,TRANSPARENT);
    DrawTextW(dc,text,-1,&box,flags|DT_NOPREFIX);
}

static bool w_fade(HWND hwnd,w_layer_fade *fade,BYTE target) {
    w_transition timing={.ms=settings.fade_ms,.curve={.22,1,.36,1}};
    return w_layer_fade_to(hwnd,fade,target,timing,motion);
}

static void w_register(void) {
    HINSTANCE inst=GetModuleHandleW(NULL);
    WNDCLASSW wc={.hInstance=inst,.lpfnWndProc=w_toast_proc,.lpszClassName=toast_class,
                  .hCursor=LoadCursorW(NULL,IDC_ARROW)};
    RegisterClassW(&wc);
    wc.lpfnWndProc=w_panel_proc; wc.lpszClassName=panel_class;
    RegisterClassW(&wc);
}

static int w_index(uint64_t id) {
    for (int i=0;i<entry_count;++i) if (history[i]->id==id) return i;
    return -1;
}

static w_entry *w_find(uint64_t id) {
    int i=w_index(id);
    return i<0 ? NULL : history[i];
}

static size_t w_length(const wchar_t *s,size_t bound) {
    size_t n=wcsnlen(s,bound);
    if (n && IS_HIGH_SURROGATE(s[n-1])) --n;
    return n;
}

static w_entry *w_copy(const w_notice_item *item) {
    const wchar_t *app=item->app && *item->app ? item->app : L"winwm";
    const wchar_t *title=item->title ? item->title : L"", *body=item->body ? item->body : L"";
    size_t a=w_length(app,256), t=w_length(title,1024), b=w_length(body,16384);
    if (!t && !b) return NULL;
    w_entry *e=calloc(1,sizeof(*e)+(a+t+b+3)*sizeof(wchar_t));
    if (!e) return NULL;
    e->app=e->strings; e->title=e->app+a+1; e->body=e->title+t+1;
    wmemcpy(e->app,app,a); wmemcpy(e->title,title,t); wmemcpy(e->body,body,b);
    e->native_id=item->native_id; e->time=item->time;
    e->has_native_id=item->has_native_id;
    e->await_echo=!item->has_native_id && item->await_echo;
    if (!(e->time.dwLowDateTime|e->time.dwHighDateTime)) GetSystemTimeAsFileTime(&e->time);
    e->native_time=e->time;
    SYSTEMTIME utc,local;
    if (FileTimeToSystemTime(&e->time,&utc) && SystemTimeToTzSpecificLocalTime(NULL,&utc,&local)) {
        wchar_t date[64]={0},time[32]={0};
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT,DATE_SHORTDATE,&local,NULL,date,count_of(date),NULL);
        GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT,TIME_NOSECONDS,&local,NULL,time,count_of(time));
        swprintf(e->stamp,count_of(e->stamp),L"%ls  %ls",date,time);
    }
    return e;
}

static void w_finish_balloon(w_toast *t,UINT event) {
    if (t->balloon_event) t->balloon_event(t->balloon,event);
    t->balloon_event=NULL;
    t->balloon=0;
}

static void w_hide_toast(w_toast *t,bool animate) {
    if (!t->hwnd) return;
    w_finish_balloon(t,NIN_BALLOONHIDE);
    KillTimer(t->hwnd,expiry_timer);
    t->closing=true;
    if (animate && w_fade(t->hwnd,&t->fade,0)) return;
    DestroyWindow(t->hwnd);
}

static void w_remove_record(int index) {
    w_entry *e=history[index];
    for (int i=0;i<max_toasts;++i) if (toasts[i].id==e->id) w_hide_toast(&toasts[i],false);
    memmove(history+index,history+index+1,(entry_count-index-1)*sizeof(*history));
    --entry_count;
    free(e);
}

static void w_round(HWND hwnd) {
    DWORD corners=2;
    DwmSetWindowAttribute(hwnd,33,&corners,sizeof(corners));
}

static bool w_left(void) { return settings.corner==1 || settings.corner==3; }
static bool w_bottom(void) { return settings.corner>=2; }

static void w_position_toasts(void) {
    w_toast *order[max_toasts]; int count=0;
    for (int i=0;i<max_toasts;++i) if (toasts[i].hwnd) {
        int j=count++;
        while (j && order[j-1]->id<toasts[i].id) { order[j]=order[j-1]; --j; }
        order[j]=&toasts[i];
    }
    HMONITOR monitors[max_toasts]; int offsets[max_toasts],used=0;
    for (int i=0;i<count;++i) {
        w_toast *t=order[i];
        RECT r=w_work(&t->monitor);
        int m=0;while (m<used && monitors[m]!=t->monitor) ++m;
        if (m==used) { monitors[used]=t->monitor; offsets[used++]=0; }
        UINT dpi=w_monitor_dpi(t->monitor);
        w_fonts_at(&t->fonts,dpi);
        int margin=min(px(settings.margin,dpi),max(0,(int)min(r.right-r.left,r.bottom-r.top)/4));
        int width=max(1,min(px(settings.width,dpi),(int)(r.right-r.left)-2*margin));
        w_entry *e=w_find(t->id);
        int height=px(e && *e->title && *e->body ? 110 : 78,dpi);
        int available=(int)(r.bottom-r.top)-2*margin;
        if (!offsets[m]) height=min(height,max(1,available));
        if (height+offsets[m]>available || i>=settings.visible_limit) { w_hide_toast(t,false); continue; }
        int x=w_left() ? r.left+margin : r.right-margin-width;
        int y=w_bottom() ? r.bottom-margin-height-offsets[m] : r.top+margin+offsets[m];
        SetWindowPos(t->hwnd,HWND_TOPMOST,x,y,width,height,SWP_NOACTIVATE|SWP_SHOWWINDOW);
        offsets[m]+=height+px(8,dpi);
    }
}

static void w_expire(w_toast *t,UINT remaining) {
    t->remaining=remaining;
    t->deadline=GetTickCount64()+remaining;
    if (!SetTimer(t->hwnd,expiry_timer,max(1u,remaining),NULL)) w_hide_toast(t,false);
}

static bool w_show_toast(w_entry *e,const w_notice_item *item) {
    w_toast *slot=NULL;
    for (int i=0;i<max_toasts;++i) {
        w_toast *t=&toasts[i];
        if (t->hwnd && t->id==e->id) { slot=t; break; }
        if (!slot || (!t->hwnd && slot->hwnd) || (t->hwnd && slot->hwnd && t->id<slot->id)) slot=t;
    }
    if (slot->hwnd && slot->id!=e->id) w_hide_toast(slot,false);
    if (!slot->hwnd) {
        slot->id=e->id; slot->monitor=w_target_monitor();
        slot->hwnd=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_TOPMOST|WS_EX_NOACTIVATE|WS_EX_LAYERED,
             toast_class,e->title,WS_POPUP,0,0,1,1,owner,NULL,GetModuleHandleW(NULL),slot);
        if (!slot->hwnd) { *slot=(w_toast){0}; return false; }
        w_round(slot->hwnd);
        w_theme_material(slot->hwnd);
    }
    slot->closing=false;
    SetWindowTextW(slot->hwnd,e->title);
    slot->remaining=settings.duration_ms;
    w_fade(slot->hwnd,&slot->fade,255);
    w_position_toasts();
    if (!slot->hwnd) return false;
    slot->balloon=item->balloon;
    slot->balloon_event=item->balloon_event;
    if (slot->balloon_event) slot->balloon_event(slot->balloon,NIN_BALLOONSHOW);
    InvalidateRect(slot->hwnd,NULL,FALSE);
    if (settings.duration_ms && !slot->hover) w_expire(slot,(UINT)settings.duration_ms);
    POINT cursor;
    if (GetCursorPos(&cursor) && WindowFromPoint(cursor)==slot->hwnd)
        SendMessageW(slot->hwnd,WM_MOUSEMOVE,0,0);
    return true;
}

void w_notice_cancel_balloon(uint64_t token) {
    if (!token) return;
    for (int i=0;i<max_toasts;++i)
        if (toasts[i].hwnd && toasts[i].balloon==token) {
            w_hide_toast(&toasts[i],false);
            w_position_toasts();
            return;
        }
}

static void w_paint_toast(w_toast *t,HDC dc) {
    RECT r;GetClientRect(t->hwnd,&r);
    FillRect(dc,&r,w_theme_brush());
    SetDCBrushColor(dc,w_theme_separator());FrameRect(dc,&r,GetStockObject(DC_BRUSH));
    w_entry *e=w_find(t->id);if (!e) return;
    UINT dpi=t->fonts.dpi;
    int pad=px(14,dpi), row=px(19,dpi); RECT box={pad,px(10,dpi),r.right-px(40,dpi),px(10,dpi)+row};
    w_text(dc,box,e->app,t->fonts.small,w_theme_accent(),DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);
    RECT close={r.right-px(34,dpi),px(4,dpi),r.right-px(4,dpi),px(34,dpi)};
    w_text(dc,close,L"×",t->fonts.bold,w_theme_hint(),DT_CENTER|DT_SINGLELINE|DT_VCENTER);
    box.top=box.bottom+px(3,dpi);box.right=r.right-pad;
    if (*e->title) {
        box.bottom=*e->body ? box.top+px(23,dpi) : r.bottom-px(8,dpi);
        w_text(dc,box,e->title,t->fonts.bold,w_theme_foreground(),*e->body ? DT_SINGLELINE|DT_END_ELLIPSIS : DT_WORDBREAK|DT_END_ELLIPSIS);
        box.top=box.bottom+px(2,dpi);
    }
    if (*e->body) {box.bottom=r.bottom-px(7,dpi);w_text(dc,box,e->body,t->fonts.regular,w_theme_foreground(),DT_WORDBREAK|DT_END_ELLIPSIS);}
}

static LRESULT CALLBACK w_toast_proc(HWND w,UINT msg,WPARAM wp,LPARAM lp) {
    w_toast *t=(w_toast*)GetWindowLongPtrW(w,GWLP_USERDATA);
    if (msg==WM_NCCREATE) {
        t=((CREATESTRUCTW*)lp)->lpCreateParams;t->hwnd=w;SetWindowLongPtrW(w,GWLP_USERDATA,(LONG_PTR)t);
    }
    if (!t) return DefWindowProcW(w,msg,wp,lp);
    switch (msg) {
    case WM_MOUSEACTIVATE:return MA_NOACTIVATE;
    case WM_ERASEBKGND:return 1;
    case WM_DPICHANGED:
        w_fonts_at(&t->fonts,HIWORD(wp));
        InvalidateRect(w,NULL,FALSE);
        return 0;
    case WM_PAINT:{ PAINTSTRUCT ps;HDC dc=BeginPaint(w,&ps);w_paint_toast(t,dc);EndPaint(w,&ps);return 0; }
    case WM_TIMER:
        if (wp==layer_fade_timer && !w_layer_fade_tick(w,&t->fade,motion) && t->closing) {
            DestroyWindow(w);w_position_toasts();
        } else if (wp==expiry_timer && !t->hover && !t->closing) {
            w_finish_balloon(t,NIN_BALLOONTIMEOUT);
            w_hide_toast(t,true);w_position_toasts();
        }
        return 0;
    case WM_MOUSEMOVE:
        if (!t->hover && !t->closing) {
            TRACKMOUSEEVENT track={sizeof(track),TME_LEAVE,w,0};
            if (TrackMouseEvent(&track)) {
                t->hover=true;
                ULONGLONG now=GetTickCount64();
                t->remaining=t->deadline>now ? (UINT)(t->deadline-now) : 1;
                KillTimer(w,expiry_timer);
            }
        }
        return 0;
    case WM_MOUSELEAVE:
        t->hover=false;
        if (!t->closing && settings.duration_ms) w_expire(t,t->remaining);
        return 0;
    case WM_CAPTURECHANGED:t->pressed=false;return 0;
    case WM_LBUTTONDOWN:
        if (!t->closing) { t->pressed=true;SetCapture(w); }
        return 0;
    case WM_LBUTTONUP:
        if (t->pressed && !t->closing) {
            t->pressed=false;RECT r;GetClientRect(w,&r);
            if (GetCapture()==w) ReleaseCapture();
            POINT cursor={GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
            if (!PtInRect(&r,cursor)) return 0;
            if (GET_X_LPARAM(lp)>=r.right-px(36,t->fonts.dpi) && GET_Y_LPARAM(lp)<px(36,t->fonts.dpi)) {
                w_hide_toast(t,true);w_position_toasts();
            } else if (t->balloon_event) {
                w_finish_balloon(t,NIN_BALLOONUSERCLICK);
                w_hide_toast(t,true);w_position_toasts();
            } else {
                uint64_t id=t->id;HMONITOR monitor=t->monitor;
                w_notice_open(monitor,NULL);
                if (w_notice_visible()) {panel.selected=id;panel.expanded=true;w_filter(false);SetFocus(panel.detail);}
            }
        }
        return 0;
    case WM_NCDESTROY:
        w_fonts_free(&t->fonts);*t=(w_toast){0};SetWindowLongPtrW(w,GWLP_USERDATA,0);break;
    }
    return DefWindowProcW(w,msg,wp,lp);
}

static HWND w_child(const wchar_t *cls,const wchar_t *caption,DWORD style,int id) {
    return CreateWindowExW(0,cls,caption,WS_CHILD|WS_VISIBLE|style,0,0,1,1,panel.hwnd,
                           (HMENU)(INT_PTR)id,GetModuleHandleW(NULL),NULL);
}

static HWND w_button(int id,const wchar_t *glyph,const wchar_t *label) {
    HWND b=w_child(L"BUTTON",label,BS_OWNERDRAW|WS_TABSTOP,id);
    if (b) w_icon_button(b,glyph,label);
    return b;
}

static int w_selected_row(void) { return ListView_GetNextItem(panel.list,-1,LVNI_SELECTED); }

static void w_detail(void) {
    w_entry *e=w_find(panel.selected);
    bool expanded=panel.expanded && e;
    if (expanded && panel.displayed!=e->revision) {
        size_t n=wcslen(e->app)+wcslen(e->stamp)+wcslen(e->title)+wcslen(e->body)+16;
        wchar_t *s=malloc(2*n*sizeof(*s));
        if (s) {
            int printed=swprintf(s,n,L"%ls  ·  %ls\r\n",e->app,e->stamp);
            wchar_t *out=s+max(0,printed);
            const wchar_t *parts[]={e->title,*e->body && *e->title ? L"\n\n" : L"",e->body};
            for (size_t i=0;i<count_of(parts);++i) {
                for (const wchar_t *p=parts[i];*p;++p) {
                    if (*p==L'\r' && p[1]==L'\n') ++p;
                    if (*p==L'\r' || *p==L'\n') { *out++=L'\r';*out++=L'\n'; }
                    else *out++=*p;
                }
            }
            *out=0;
            SetWindowTextW(panel.detail,s);free(s);
            SendMessageW(panel.detail,EM_SETSEL,0,0);
            panel.displayed=e->revision;
        }
    } else if (!expanded && panel.displayed) {
        panel.displayed=0;
        SetWindowTextW(panel.detail,L"");
    }
    ShowWindow(panel.detail,expanded ? SW_SHOWNA : SW_HIDE);
    EnableWindow(panel.expand,e!=NULL);
    EnableWindow(panel.dismiss,e!=NULL);
    EnableWindow(panel.clear,entry_count>0);
    w_layout_panel();
}

static bool w_substring(const wchar_t *field,const wchar_t *word) {
    return FindStringOrdinal(FIND_FROMSTART,field,-1,word,-1,TRUE)>=0;
}

static void w_filter(bool reset) {
    if (!panel.list) return;
    wchar_t query[256],*words[128],*context=NULL;int n=0;
    GetWindowTextW(panel.search,query,count_of(query));
    for (wchar_t *p=wcstok_s(query,L" \r\t\n",&context);p;p=wcstok_s(NULL,L" \r\t\n",&context)) words[n++]=p;
    uint64_t selected=panel.selected;
    panel.filtering=true;
    match_count=0;
    int row=-1;
    for (int i=0;i<entry_count;++i) {
        w_entry *e=history[i];bool match=true;
        for (int j=0;j<n && match;++j)
            match=w_substring(e->app,words[j]) || w_substring(e->title,words[j]) || w_substring(e->body,words[j]);
        if (match) {
            if (e->id==selected) row=match_count;
            matches[match_count++]=e;
        }
    }
    if (row<0 || reset) { row=match_count ? 0 : -1;panel.expanded=false; }
    ListView_SetItemCountEx(panel.list,match_count,LVSICF_NOSCROLL|LVSICF_NOINVALIDATEALL);
    ListView_SetItemState(panel.list,-1,0,LVIS_SELECTED|LVIS_FOCUSED);
    if (row>=0) {
        ListView_SetItemState(panel.list,row,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
        if (reset || selected!=matches[row]->id) ListView_EnsureVisible(panel.list,row,FALSE);
    }
    panel.selected=row>=0 ? matches[row]->id : 0;
    panel.filtering=false;
    SetWindowTextW(panel.empty,n ? L"No matching notifications" : L"No notifications");
    ShowWindow(panel.empty,match_count ? SW_HIDE : SW_SHOWNA);
    ShowWindow(panel.list,match_count ? SW_SHOWNA : SW_HIDE);
    InvalidateRect(panel.list,NULL,FALSE);
    w_detail();
}

static void w_toggle_details(void) {
    if (!panel.selected) return;
    panel.expanded=!panel.expanded;
    w_detail();
    SetFocus(panel.expanded ? panel.detail : panel.list);
}

static void w_dismiss_selected(void) {
    int index=w_index(panel.selected);
    if (index<0) return;
    int row=w_selected_row();
    panel.selected=match_count>1 && row>=0 ? matches[row+1<match_count ? row+1 : row-1]->id : 0;
    w_remove_record(index);w_filter(false);w_position_toasts();
}

static void w_clear(void) {
    while (entry_count) w_remove_record(entry_count-1);
    panel.selected=0;panel.expanded=false;w_filter(true);
}

static void w_place(HWND w,int x,int y,int width,int height) {
    SetWindowPos(w,NULL,x,y,max(1,width),max(1,height),SWP_NOZORDER|SWP_NOACTIVATE);
}

static void w_layout_panel(void) {
    if (!panel.empty) return;
    RECT r;GetClientRect(panel.hwnd,&r);
    UINT dpi=panel.fonts.dpi;int pad=px(12,dpi),gap=px(6,dpi),button=px(30,dpi);
    int width=(int)r.right-2*pad, x=(int)r.right-pad-button;
    HWND tools[]={panel.close,panel.clear,panel.dismiss,panel.expand};
    for (int i=0;i<4;i++,x-=button+gap) w_place(tools[i],x,pad,button,button);
    int top=pad+button+gap;
    w_place(panel.search,pad,top,width,px(31,dpi));top+=px(31,dpi)+gap;
    int bottom=(int)r.bottom-pad;
    if (request_access) {
        bottom-=px(32,dpi);w_place(panel.access,pad,bottom,width,px(32,dpi));bottom-=gap;
    }
    if (*access_status) {
        int height=px(width<px(360,dpi) ? 43 : 34,dpi);
        bottom-=height;w_place(panel.status,pad,bottom,width,height);bottom-=gap;
    }
    if (panel.expanded && panel.selected) {
        int height=max(1,(bottom-top)*2/5);bottom-=height;w_place(panel.detail,pad,bottom,width,height);bottom-=gap;
    }
    w_place(panel.list,pad,top,width,bottom-top);w_place(panel.empty,pad,top,width,bottom-top);
    RECT inner;GetClientRect(panel.list,&inner);
    if (ListView_GetColumnWidth(panel.list,0)!=max(1,inner.right))
        ListView_SetColumnWidth(panel.list,0,max(1,inner.right));
}

static void w_panel_style(UINT dpi) {
    if (panel.fonts.dpi!=dpi) {
        w_fonts_at(&panel.fonts,dpi);
        HWND controls[]={panel.search,panel.list,panel.detail,panel.access,panel.empty,panel.status};
        for (size_t i=0;i<count_of(controls);++i)
            SendMessageW(controls[i],WM_SETFONT,(WPARAM)(controls[i]==panel.status ? panel.fonts.small : panel.fonts.regular),FALSE);
        HIMAGELIST image=ImageList_Create(1,px(51,dpi),ILC_COLOR32,1,0);
        if (image) {
            ListView_SetImageList(panel.list,image,LVSIL_SMALL);
            if (panel.spacing) ImageList_Destroy(panel.spacing);
            panel.spacing=image;
        }
    }
    ListView_SetBkColor(panel.list,w_theme_background());ListView_SetTextBkColor(panel.list,w_theme_background());
}

static void w_position_panel(void) {
    if (!panel.hwnd) return;
    RECT work=w_work(&panel.monitor);
    UINT dpi=w_monitor_dpi(panel.monitor);
    int gap=px(8,dpi),width=px(480,dpi),height=px(554,dpi);
    RECT r;
    if (panel.anchor.bar) {
        RECT bar;
        if (!IsWindowVisible(panel.anchor.bar) || !GetWindowRect(panel.anchor.bar,&bar) ||
            !EqualRect(&bar,&panel.anchor_bar)) { w_close_panel(false);return; }
        r=w_popup_bounds(&panel.anchor,w_popup_area(panel.anchor.bar),width,height,gap);
    } else {
        int margin=min(px(settings.margin,dpi),max(0,(int)min(work.right-work.left,work.bottom-work.top)/4));
        InflateRect(&work,-margin,-margin);
        width=max(1,min(width,work.right-work.left));height=max(1,min(height,work.bottom-work.top));
        r.left=w_left()?work.left:work.right-width;r.top=w_bottom()?work.bottom-height:work.top;
        r.right=r.left+width;r.bottom=r.top+height;
    }
    w_panel_style(dpi);
    SetWindowPos(panel.hwnd,HWND_TOPMOST,r.left,r.top,r.right-r.left,r.bottom-r.top,SWP_NOACTIVATE);
    w_layout_panel();
}

static LRESULT w_draw_row(NMLVCUSTOMDRAW *draw) {
    if (draw->nmcd.dwDrawStage==CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
    if (draw->nmcd.dwDrawStage!=CDDS_ITEMPREPAINT) return CDRF_DODEFAULT;
    int i=(int)draw->nmcd.dwItemSpec;if ((unsigned)i>=(unsigned)match_count) return CDRF_SKIPDEFAULT;
    w_entry *e=matches[i];RECT r;
    if (!ListView_GetItemRect(panel.list,i,&r,LVIR_BOUNDS)) return CDRF_SKIPDEFAULT;
    bool selected=(ListView_GetItemState(panel.list,i,LVIS_SELECTED)&LVIS_SELECTED)!=0;
    HDC dc=draw->nmcd.hdc;int saved=SaveDC(dc);UINT dpi=panel.fonts.dpi;
    COLORREF fg=selected ? w_theme_selection(true) : w_theme_foreground();
    SetDCBrushColor(dc,selected?w_theme_selection(false):w_theme_background());
    FillRect(dc,&r,GetStockObject(DC_BRUSH));
    if (selected) {
        RECT stripe=r;stripe.right=stripe.left+px(3,dpi);
        SetDCBrushColor(dc,w_theme_accent());FillRect(dc,&stripe,GetStockObject(DC_BRUSH));
    }
    InflateRect(&r,-px(10,dpi),-px(3,dpi));
    RECT app=r,date=r;app.bottom=app.top+px(19,dpi);date.bottom=app.bottom;
    SIZE size={0};SelectObject(dc,panel.fonts.small);GetTextExtentPoint32W(dc,e->stamp,(int)wcslen(e->stamp),&size);
    date.left=max(r.left+(r.right-r.left)/2,r.right-size.cx);app.right=date.left-px(8,dpi);
    UINT flags=DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS;
    w_text(dc,app,e->app,panel.fonts.small,selected?fg:w_theme_accent(),flags);
    w_text(dc,date,e->stamp,panel.fonts.small,selected?fg:w_theme_hint(),flags|DT_RIGHT);
    r.top=app.bottom;w_text(dc,r,*e->title?e->title:e->body,panel.fonts.bold,fg,flags);
    RestoreDC(dc,saved);
    return CDRF_SKIPDEFAULT;
}

static LRESULT CALLBACK w_search_proc(HWND w,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR data) {
    (void)id;(void)data;
    if (msg==WM_IME_STARTCOMPOSITION) panel.composing=true;
    if (msg==WM_IME_ENDCOMPOSITION || msg==WM_KILLFOCUS) panel.composing=false;
    return DefSubclassProc(w,msg,wp,lp);
}

static bool w_create_panel(void) {
    INITCOMMONCONTROLSEX controls={sizeof(controls),ICC_LISTVIEW_CLASSES};InitCommonControlsEx(&controls);
    panel.hwnd=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_TOPMOST|WS_EX_LAYERED|WS_EX_CONTROLPARENT,
        panel_class,L"Notifications",WS_POPUP|WS_CLIPCHILDREN,0,0,1,1,owner,NULL,GetModuleHandleW(NULL),NULL);
    if (!panel.hwnd) return false;
    w_round(panel.hwnd);
    panel.search=w_child(L"EDIT",L"",ES_AUTOHSCROLL|WS_BORDER|WS_TABSTOP,search_id);
    panel.list=w_child(WC_LISTVIEWW,L"Notification history",LVS_OWNERDATA|LVS_REPORT|LVS_NOCOLUMNHEADER|
             LVS_SINGLESEL|LVS_SHOWSELALWAYS|LVS_SHAREIMAGELISTS|WS_TABSTOP,list_id);
    panel.detail=w_child(L"EDIT",L"",ES_READONLY|ES_MULTILINE|ES_AUTOVSCROLL|WS_VSCROLL|WS_BORDER|WS_TABSTOP,0);
    panel.clear=w_button(clear_id,L"\xe74d",L"Clear notification history");
    panel.dismiss=w_button(dismiss_id,L"\xe894",L"Dismiss selected notification");
    panel.expand=w_button(expand_id,L"\xe70d",L"Show or hide notification details");
    panel.close=w_button(close_id,L"\xe711",L"Close notifications");
    panel.access=w_child(L"BUTTON",L"Capture options",WS_TABSTOP|BS_OWNERDRAW,access_id);
    panel.status=w_child(L"STATIC",access_status,SS_LEFT|SS_NOPREFIX,0);
    panel.empty=w_child(L"STATIC",L"No notifications",SS_CENTER|SS_CENTERIMAGE,0);
    if (!panel.search||!panel.list||!panel.detail||!panel.clear||!panel.dismiss||!panel.expand||
        !panel.close||!panel.access||!panel.status||!panel.empty) {
        DestroyWindow(panel.hwnd);return false;
    }
    SetWindowSubclass(panel.search,w_search_proc,1,0);
    SendMessageW(panel.search,EM_SETLIMITTEXT,255,0);
    SendMessageW(panel.search,EM_SETCUEBANNER,TRUE,(LPARAM)L"Search notifications");
    SendMessageW(panel.detail,EM_SETLIMITTEXT,40000,0);
    ListView_SetExtendedListViewStyle(panel.list,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER);
    LVCOLUMNW col={.mask=LVCF_WIDTH|LVCF_TEXT,.cx=1,.pszText=L"Notification"};
    ListView_InsertColumn(panel.list,0,&col);
    ShowWindow(panel.access,request_access?SW_SHOWNA:SW_HIDE);
    ShowWindow(panel.status,*access_status?SW_SHOWNA:SW_HIDE);
    return true;
}

static LRESULT CALLBACK w_panel_proc(HWND w,UINT msg,WPARAM wp,LPARAM lp) {
    LRESULT answer;
    if (w_theme_message(w,msg,wp,lp,&answer)) return answer;
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;HDC dc=BeginPaint(w,&ps);RECT r;GetClientRect(w,&r);FillRect(dc,&r,w_theme_brush());
        SetDCBrushColor(dc,w_theme_separator());FrameRect(dc,&r,GetStockObject(DC_BRUSH));
        UINT dpi=panel.fonts.dpi;r.left=px(13,dpi);r.top=px(10,dpi);r.bottom=px(43,dpi);r.right-=px(163,dpi);
        w_text(dc,r,L"Notifications",panel.fonts.bold,w_theme_foreground(),DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);
        EndPaint(w,&ps);return 0;
    }
    case WM_SIZE:w_layout_panel();return 0;
    case WM_DPICHANGED:if (panel.list) {w_panel_style(HIWORD(wp));w_layout_panel();}return 0;
    case WM_DISPLAYCHANGE:if (w_notice_visible()) w_position_panel();return 0;
    case WM_COMMAND:
        if (LOWORD(wp)==search_id && HIWORD(wp)==EN_CHANGE) {w_filter(true);return 0;}
        if (HIWORD(wp)!=BN_CLICKED) break;
        switch (LOWORD(wp)) {
        case clear_id:w_clear();SetFocus(panel.search);return 0;
        case dismiss_id:w_dismiss_selected();return 0;
        case expand_id:w_toggle_details();return 0;
        case close_id:w_notice_close();return 0;
        case access_id:if (request_access) request_access(panel.access);return 0;
        }
        break;
    case WM_NOTIFY: {
        NMHDR *note=(NMHDR*)lp;if (note->hwndFrom!=panel.list) break;
        if (note->code==NM_CUSTOMDRAW) return w_draw_row((NMLVCUSTOMDRAW*)lp);
        if (note->code==LVN_GETDISPINFOW) {
            LVITEMW *item=&((NMLVDISPINFOW*)lp)->item;
            if ((item->mask&LVIF_TEXT) && (unsigned)item->iItem<(unsigned)match_count && item->pszText && item->cchTextMax>0) {
                w_entry *e=matches[item->iItem];wchar_t label[1536];
                swprintf(label,count_of(label),L"%ls  ·  %.1000ls  ·  %ls",e->app,*e->title?e->title:e->body,e->stamp);
                lstrcpynW(item->pszText,label,item->cchTextMax);
            }
            return 0;
        }
        if (note->code==LVN_ITEMCHANGED && !panel.filtering) {
            int i=w_selected_row();if ((unsigned)i<(unsigned)match_count && panel.selected!=matches[i]->id) {
                panel.selected=matches[i]->id;w_detail();
            }
        }
        if (note->code==NM_DBLCLK && ((NMITEMACTIVATE*)lp)->iItem>=0) w_toggle_details();
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(wp)==WA_INACTIVE && w_notice_visible() && lp && !w_notice_contains((HWND)lp) &&
            (HWND)lp!=panel.anchor.bar) w_close_panel(false);
        break;
    case WM_CLOSE:w_notice_close();return 0;
    case WM_TIMER:
        if (wp==layer_fade_timer && !w_layer_fade_tick(w,&panel.fade,motion) && panel.closing) ShowWindow(w,SW_HIDE);
        return 0;
    case WM_NCDESTROY:
        if (panel.spacing) ImageList_Destroy(panel.spacing);
        w_fonts_free(&panel.fonts);memset(&panel,0,sizeof(panel));match_count=0;break;
    }
    return DefWindowProcW(w,msg,wp,lp);
}

bool w_notice_visible(void) {
    return panel.hwnd && !panel.closing && IsWindowVisible(panel.hwnd);
}

bool w_notice_contains(HWND hit) {
    if (!hit) return false;
    HWND root=GetAncestor(hit,GA_ROOT);
    if (!root) return false;
    if (panel.hwnd && (root==panel.hwnd || GetWindow(root,GW_OWNER)==panel.hwnd ||
        (panel.composing && (root==ImmGetDefaultIMEWnd(panel.search) || (GetClassLongPtrW(root,GCL_STYLE)&CS_IME))))) return true;
    for (int i=0;i<max_toasts;++i) if (toasts[i].hwnd==root) return true;
    return false;
}

static void w_close_panel(bool restore) {
    if (!w_notice_visible()) return;
    panel.closing=true;
    panel.filtering=true;match_count=0;
    ListView_SetItemCountEx(panel.list,0,LVSICF_NOSCROLL);
    panel.filtering=false;
    bool focused=GetForegroundWindow()==panel.hwnd;
    SetWindowLongPtrW(panel.hwnd,GWL_EXSTYLE,GetWindowLongPtrW(panel.hwnd,GWL_EXSTYLE)|WS_EX_TRANSPARENT|WS_EX_NOACTIVATE);
    if (restore && focused && panel.previous && IsWindowVisible(panel.previous) && !IsIconic(panel.previous))
        SetForegroundWindow(panel.previous);
    if (GetForegroundWindow()==panel.hwnd || !w_fade(panel.hwnd,&panel.fade,0)) ShowWindow(panel.hwnd,SW_HIDE);
    panel.previous=NULL;
}

bool w_notice_close(void) {
    if (!w_notice_visible()) return false;
    w_close_panel(true);return true;
}

void w_notice_outside(HWND hit) {
    if (!w_notice_visible() || w_notice_contains(hit)) return;
    POINT cursor;
    if (panel.anchor.bar && GetAncestor(hit,GA_ROOT)==panel.anchor.bar &&
        GetCursorPos(&cursor) && PtInRect(&panel.anchor.button,cursor)) return;
    w_close_panel(false);
}

bool w_notice_message(MSG *msg) {
    if (!w_notice_visible() || (msg->hwnd!=panel.hwnd && !IsChild(panel.hwnd,msg->hwnd))) return false;
    if (panel.composing && msg->hwnd==panel.search) return false;
    if (msg->message==WM_KEYDOWN) {
        if (msg->wParam==VK_ESCAPE) {
            if (panel.expanded) {panel.expanded=false;w_detail();SetFocus(panel.list);}
            else w_notice_close();
            return true;
        }
        if (msg->hwnd==panel.list && msg->wParam==VK_DELETE) {w_dismiss_selected();return true;}
        if ((msg->hwnd==panel.list || msg->hwnd==panel.search) && msg->wParam==VK_RETURN) {
            w_toggle_details();return true;
        }
        if (msg->hwnd==panel.search && msg->wParam==VK_DOWN && match_count) {SetFocus(panel.list);return true;}
        if (msg->wParam=='F' && GetKeyState(VK_CONTROL)<0) {
            SetFocus(panel.search);SendMessageW(panel.search,EM_SETSEL,0,-1);return true;
        }
    }
    return IsDialogMessageW(panel.hwnd,msg)!=FALSE;
}

void w_notice_open(HMONITOR monitor,const w_popup_anchor *anchor) {
    if (!owner) return;
    RECT bar={0};
    if (anchor && (!IsWindowVisible(anchor->bar) || !GetWindowRect(anchor->bar,&bar))) anchor=NULL;
    bool was_open=w_notice_visible();
    if (was_open && anchor && anchor->bar==panel.anchor.bar && anchor->source==panel.anchor.source) {
        w_notice_close();return;
    }
    if (!panel.hwnd && !w_create_panel()) return;
    if (!was_open) panel.previous=GetForegroundWindow();
    panel.monitor=monitor?monitor:w_target_monitor();panel.anchor=anchor ? *anchor : (w_popup_anchor){0};
    panel.anchor_bar=bar;
    panel.closing=false;
    SetWindowLongPtrW(panel.hwnd,GWL_EXSTYLE,GetWindowLongPtrW(panel.hwnd,GWL_EXSTYLE)&~(WS_EX_NOACTIVATE|WS_EX_TRANSPARENT));
    for (int i=0;i<max_toasts;++i) w_hide_toast(&toasts[i],false);
    if (!was_open) {
        panel.selected=0;panel.expanded=false;SetWindowTextW(panel.search,L"");
    }
    w_filter(!was_open);w_position_panel();
    if (!was_open) panel.fade.value=0;
    w_fade(panel.hwnd,&panel.fade,255);
    ShowWindow(panel.hwnd,SW_SHOW);
    SetForegroundWindow(panel.hwnd);
    SetFocus(panel.search);
}

void w_notice_configure(HWND host,const w_notification_config *config,bool motion_allowed) {
    if (!owner) w_register();
    int old_duration=settings.duration_ms;
    owner=host;settings=*config;motion=motion_allowed;
    while (entry_count>settings.history_limit) w_remove_record(entry_count-1);
    for (int i=0;i<max_toasts;++i) {
        w_toast *t=&toasts[i];if (!t->hwnd) continue;
        if (!settings.enabled || (t->closing && !motion)) w_hide_toast(t,false);
        else if (!t->closing) {
            if (old_duration!=settings.duration_ms) {
                KillTimer(t->hwnd,expiry_timer);
                t->remaining=settings.duration_ms;
                if (t->remaining && !t->hover) w_expire(t,t->remaining);
            }
            if (!motion) w_fade(t->hwnd,&t->fade,255);
        }
    }
    if (panel.hwnd) {
        if (panel.closing && !motion) ShowWindow(panel.hwnd,SW_HIDE);
        else if (w_notice_visible() && !motion) w_fade(panel.hwnd,&panel.fade,255);
        if (w_notice_visible()) {w_filter(false);InvalidateRect(panel.hwnd,NULL,TRUE);}
    }
    w_notice_reposition();
}

void w_notice_access(const wchar_t *status,void (*request)(HWND button)) {
    lstrcpynW(access_status,status?status:L"",count_of(access_status));request_access=request;
    if (panel.hwnd) {
        SetWindowTextW(panel.status,access_status);
        ShowWindow(panel.access,request_access?SW_SHOWNA:SW_HIDE);
        ShowWindow(panel.status,*access_status?SW_SHOWNA:SW_HIDE);
        w_layout_panel();
    }
}

static uint64_t w_ticks(FILETIME time) {
    return (uint64_t)time.dwHighDateTime<<32 | time.dwLowDateTime;
}

static bool w_adopt_local(const w_entry *native) {
    uint64_t best=20000000,stamp=w_ticks(native->time);
    w_entry *match=NULL;
    for (int i=0;i<entry_count;++i) {
        w_entry *old=history[i];
        uint64_t when=w_ticks(old->time),distance=when>stamp ? when-stamp : stamp-when;
        if (old->await_echo && distance<=best && !_wcsicmp(old->app,native->app) &&
            !wcscmp(old->title,native->title) && !wcscmp(old->body,native->body)) {
            match=old;best=distance;
        }
    }
    if (match) {
        match->await_echo=false;
        match->has_native_id=true;
        match->native_id=native->native_id;
        match->native_time=native->time;
    }
    return match!=NULL;
}

void w_notice_batch(bool begin) {
    batching=begin;
    if (!begin && filter_pending) {
        filter_pending=false;
        if (w_notice_visible()) w_filter(false);
    }
}

void w_notice_add(const w_notice_item *item,bool toast) {
    if (!owner || !item || settings.history_limit<=0) return;
    w_entry *e=w_copy(item);if (!e) return;
    int previous=-1;
    if (e->has_native_id) for (int i=0;i<entry_count;++i) {
        w_entry *old=history[i];
        if (!old->has_native_id || old->native_id!=e->native_id ||
            CompareFileTime(&old->native_time,&e->native_time)) continue;
        if (!wcscmp(e->app,old->app) && !wcscmp(e->title,old->title) && !wcscmp(e->body,old->body)) {
            free(e);return;
        }
        previous=i;break;
    }
    if (previous<0 && item->own_native && e->has_native_id && !item->confirmed_receipt && w_adopt_local(e)) {
        free(e);return;
    }
    if (previous<0 && entry_count==settings.history_limit &&
        CompareFileTime(&history[entry_count-1]->time,&e->time)>0) {free(e);return;}
    bool visible=w_notice_visible();
    if (visible) match_count=0; // The virtual list must never return a replaced or evicted row.
    uint64_t existing=0;
    if (previous>=0) {
        existing=history[previous]->id;
        free(history[previous]);
        memmove(history+previous,history+previous+1,(--entry_count-previous)*sizeof(*history));
    }
    int pos=0;while (pos<entry_count && CompareFileTime(&history[pos]->time,&e->time)>0) ++pos;
    if (entry_count==settings.history_limit) w_remove_record(entry_count-1);
    e->revision=++serial;
    e->id=existing?existing:e->revision;
    memmove(history+pos+1,history+pos,(entry_count-pos)*sizeof(*history));history[pos]=e;++entry_count;
    bool shown=false;
    if (visible) {
        if (batching) filter_pending=true;
        else w_filter(false);
    }
    else if (toast && settings.enabled) shown=w_show_toast(e,item);
    else if (existing) {
        for (int i=0;i<max_toasts;++i) if (toasts[i].hwnd && toasts[i].id==existing) {
            SetWindowTextW(toasts[i].hwnd,e->title);
            w_position_toasts();
            if (toasts[i].hwnd) InvalidateRect(toasts[i].hwnd,NULL,FALSE);
            break;
        }
    }
    if (item->balloon_event && !shown) item->balloon_event(item->balloon,NIN_BALLOONHIDE);
}

int w_notice_count(void) { return entry_count; }

void w_notice_reposition(void) {
    if (w_notice_visible()) w_position_panel();
    w_position_toasts();
}

void w_notice_shutdown(void) {
    if (panel.hwnd) DestroyWindow(panel.hwnd);
    for (int i=0;i<max_toasts;++i) w_hide_toast(&toasts[i],false);
    while (entry_count) free(history[--entry_count]);
    owner=NULL;read_work=NULL;request_access=NULL;access_status[0]=0;match_count=0;
    batching=false;filter_pending=false;
}
