#define COBJMACROS
#include "notifications/view.h"
#include "theme/theme.h"
#include "theme/icon_button.h"
#include "theme/hover.h"
#include "theme/control_hover.h"
#include "theme/native_controls.h"
#include "theme/disclosure.h"
#include "win_platform/paint.h"
#include "win_platform/layer.h"
#include <commctrl.h>
#include <imm.h>
#include <oleacc.h>
#include <shellscalingapi.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <windowsx.h>

enum { max_entries=notice_history_max, max_toasts=notice_visible_max, expiry_timer=1, detail_timer=2,
       list_id=101, search_id, clear_id, dismiss_id, read_id, close_id, access_id };
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

typedef struct { HFONT regular, bold, small, title; UINT dpi; int body_line, heading_line, small_line; } w_fonts;
typedef struct {
    HWND hwnd, close, tracking;
    uint64_t id;
    HMONITOR monitor;
    w_layer_fade fade;
    w_fonts fonts;
    RECT title_box, body_box;
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
    HWND hwnd, search, list, read, detail_clip, detail, clear, dismiss, close, access, status, empty, previous;
    HMONITOR monitor;
    w_popup_anchor anchor;
    RECT heading, search_box, detail_box;
    IAccPropServices *accessibility;
    bool uninitialize_com;
    int row_height,opening_rows,detail_viewport,detail_gap,vertical_pad;
    bool read_in_search, defer_status;
    struct {
        int width, actions, columns, header, field, status, detail, detail_line, chrome;
        bool wrap;
    } fit;
    w_layer_fade fade;
    w_disclosure reveal;
    w_fonts fonts;
    uint64_t selected, displayed_item, displayed_revision, first_visible;
    uint64_t row_revisions[max_entries];
    int first_offset;
    bool expanded, filtering, closing, composing, list_dispatch, selection_changed;
} panel;
static wchar_t access_status[256];
static void (*request_access)(HWND button);

static void w_layout_panel(void);
static void w_position_panel(void);
static void w_place_read_button(void);
static void w_filter(bool reset);
static void w_set_details(bool open);
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
    if (f->title) DeleteObject(f->title);
    *f=(w_fonts){0};
}

static void w_fonts_at(HWND window,w_fonts *f,UINT dpi) {
    if (f->dpi==dpi) return;
    w_fonts next={.dpi=dpi,.regular=w_theme_role_font(dpi,text_body),
                  .bold=w_theme_role_font(dpi,text_emphasis),.small=w_theme_role_font(dpi,text_secondary)};
    if (window==panel.hwnd) next.title=w_theme_role_font(dpi,text_title);
    if (!next.regular || !next.bold || !next.small || (window==panel.hwnd && !next.title)) {
        w_fonts_free(&next);
        return;
    }
    for (HWND child=GetWindow(window,GW_CHILD); child; child=GetWindow(child,GW_HWNDNEXT))
        SendMessageW(child,WM_SETFONT,(WPARAM)(child==panel.status ? next.small : next.regular),FALSE);
    if (window==panel.hwnd && panel.detail) SendMessageW(panel.detail,WM_SETFONT,(WPARAM)next.regular,FALSE);
    next.body_line=w_theme_line_height(next.regular);
    next.heading_line=w_theme_line_height(next.bold);
    next.small_line=w_theme_line_height(next.small);
    if (window==panel.hwnd) panel.fit.width=0;
    w_fonts_free(f);
    *f=next;
}

static void w_text(HDC dc,RECT box,const wchar_t *text,HFONT font,COLORREF color,UINT flags) {
    if (font) SelectObject(dc,font);
    SetTextColor(dc,color);
    SetBkMode(dc,TRANSPARENT);
    w_theme_draw_text(dc,text,-1,&box,flags|DT_NOPREFIX);
}

static bool w_fade(HWND hwnd,w_layer_fade *fade,BYTE target) {
    w_transition timing={.ms=settings.fade_ms,.curve={.22,1,.36,1}};
    return w_layer_fade_to(hwnd,fade,target,timing,motion);
}

static bool w_retime_finished_exit(HWND hwnd,w_layer_fade *fade,bool closing) {
    if (!fade->start) return false;
    bool running=w_fade(hwnd,fade,closing ? 0 : 255);
    return closing && !running;
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
    w_hover_target(t->hwnd,-1,NULL);
    if (animate && w_fade(t->hwnd,&t->fade,0)) return;
    DestroyWindow(t->hwnd);
}

static void w_invalidate_matches(void) {
    if (match_count && w_notice_visible()) {
        int top=ListView_GetTopIndex(panel.list);
        RECT row;
        if (top>0 && top<match_count && ListView_GetItemRect(panel.list,top,&row,LVIR_BOUNDS)) {
            panel.first_visible=matches[top]->id;
            panel.first_offset=row.top;
        }
    }
    // A virtual list can request a row while a toast is being removed.
    match_count=0;
}

static void w_remove_record(int index) {
    w_entry *e=history[index];
    w_invalidate_matches();
    for (int i=0;i<max_toasts;++i) if (toasts[i].id==e->id) w_hide_toast(&toasts[i],false);
    memmove(history+index,history+index+1,(entry_count-index-1)*sizeof(*history));
    --entry_count;
    free(e);
}

static bool w_left(void) { return settings.corner==1 || settings.corner==3; }
static bool w_bottom(void) { return settings.corner>=2; }

static int w_toast_text(HDC dc,const wchar_t *text,HFONT font,int width,int lines,UINT dpi) {
    if (!*text) return 0;
    if (!dc) return px(20,dpi)*lines;
    HFONT previous=SelectObject(dc,font ? font : GetStockObject(DEFAULT_GUI_FONT));
    HFONT base=w_theme_select_family(dc,w_theme_text_family(text,-1));
    TEXTMETRICW metrics={0};
    GetTextMetricsW(dc,&metrics);
    int line=max(1,metrics.tmHeight);
    RECT measured={0,0,max(1,width),0};
    int height=lines==1 ? line : DrawTextW(dc,text,-1,&measured,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);
    if (base) SelectObject(dc,base);
    SelectObject(dc,previous);
    return max(line,min(height,line*lines));
}

static int w_toast_layout(w_toast *t,const w_entry *entry,int width,UINT dpi) {
    int gap=w_theme_spacing(dpi),pad=2*gap,y=gap+w_theme_button_size(dpi);
    HDC dc=GetDC(t->hwnd);
    int title=entry ? w_toast_text(dc,entry->title,t->fonts.bold,width-2*pad,*entry->body ? 1 : 3,dpi) : 0;
    int body=entry ? w_toast_text(dc,entry->body,t->fonts.regular,width-2*pad,3,dpi) : 0;
    if (dc) ReleaseDC(t->hwnd,dc);
    t->title_box=(RECT){pad,y,width-pad,y+title};
    y+=title+(title && body ? max(1,gap/2) : 0);
    t->body_box=(RECT){pad,y,width-pad,y+body};
    return y+body+(title || body ? pad : gap);
}

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
        w_fonts_at(t->hwnd,&t->fonts,dpi);
        int margin=min(px(settings.margin,dpi),max(0,(int)min(r.right-r.left,r.bottom-r.top)/4));
        int width=max(1,min(px(settings.width,dpi),(int)(r.right-r.left)-2*margin));
        w_entry *e=w_find(t->id);
        int gap=w_theme_spacing(dpi);
        int height=w_toast_layout(t,e,width,dpi);
        int available=(int)(r.bottom-r.top)-2*margin;
        if (!offsets[m]) height=min(height,max(1,available));
        if (height+offsets[m]>available || i>=settings.visible_limit) { w_hide_toast(t,false); continue; }
        int x=w_left() ? r.left+margin : r.right-margin-width;
        int y=w_bottom() ? r.bottom-margin-height-offsets[m] : r.top+margin+offsets[m];
        RECT bounds={x,y,x+width,y+height}, old;
        if (!IsWindowVisible(t->hwnd) || !GetWindowRect(t->hwnd,&old) || !EqualRect(&bounds,&old))
            SetWindowPos(t->hwnd,HWND_TOPMOST,x,y,width,height,SWP_NOACTIVATE|SWP_SHOWWINDOW);
        offsets[m]+=height+gap;
    }
}

static void w_expire(w_toast *t,UINT remaining) {
    t->remaining=remaining;
    t->deadline=GetTickCount64()+remaining;
    if (!SetTimer(t->hwnd,expiry_timer,max(1u,remaining),NULL)) w_hide_toast(t,false);
}

static void w_toast_pointer(w_toast *t, HWND receiver, UINT msg) {
    if (!t || t->closing) return;
    if (msg==WM_MOUSEMOVE && receiver!=t->tracking) {
        TRACKMOUSEEVENT track={sizeof(track),TME_LEAVE,receiver,0};
        if (!TrackMouseEvent(&track)) return;
        t->tracking=receiver;
        RECT bounds;
        if (receiver==t->hwnd && GetClientRect(t->hwnd,&bounds)) w_hover_target(t->hwnd,0,&bounds);
        else w_hover_target(t->hwnd,-1,NULL);
        if (!t->hover) {
            t->hover=true;
            ULONGLONG now=GetTickCount64();
            t->remaining=t->deadline>now ? (UINT)(t->deadline-now) : 1;
            KillTimer(t->hwnd,expiry_timer);
        }
    } else if (msg==WM_MOUSELEAVE && t->tracking==receiver) {
        t->tracking=NULL;
        w_hover_target(t->hwnd,-1,NULL);
        POINT point;
        if (GetCursorPos(&point) && GetAncestor(WindowFromPoint(point),GA_ROOT)==t->hwnd) return;
        t->hover=false;
        if (settings.duration_ms) w_expire(t,t->remaining);
    }
}

static LRESULT CALLBACK w_toast_close_proc(HWND button,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR ref) {
    if (msg==WM_MOUSEMOVE || msg==WM_MOUSELEAVE) w_toast_pointer((w_toast *)ref,button,msg);
    if (msg==WM_NCDESTROY) RemoveWindowSubclass(button,w_toast_close_proc,id);
    return DefSubclassProc(button,msg,wp,lp);
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
        slot->hwnd=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_TOPMOST|WS_EX_NOACTIVATE,
             toast_class,e->title,WS_POPUP|WS_CLIPCHILDREN,0,0,1,1,owner,NULL,GetModuleHandleW(NULL),slot);
        if (!slot->hwnd) { *slot=(w_toast){0}; return false; }
        slot->close=CreateWindowW(L"BUTTON",L"Dismiss notification",WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,
                                  0,0,1,1,slot->hwnd,(HMENU)(INT_PTR)close_id,GetModuleHandleW(NULL),NULL);
        if (!slot->close) { DestroyWindow(slot->hwnd); return false; }
        w_icon_button(slot->close,L"\xe711",L"Dismiss notification");
        SetWindowSubclass(slot->close,w_toast_close_proc,1,(DWORD_PTR)slot);
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
    if (GetCursorPos(&cursor)) {
        HWND hit=WindowFromPoint(cursor);
        if (hit==slot->hwnd || hit==slot->close) w_toast_pointer(slot,hit,WM_MOUSEMOVE);
    }
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
    int hover=w_hover_amount(t->hwnd,0);
    if (hover) {
        RECT face=r;
        InflateRect(&face,-1,-1);
        w_hover_plate(dc,&face,t->fonts.dpi,w_theme_background(),w_theme_mix(w_theme_background(),w_theme_accent(),12),
                      hover,t->pressed);
    }
    w_theme_frame(dc,&r,t->fonts.dpi);
    w_entry *e=w_find(t->id);if (!e) return;
    UINT dpi=t->fonts.dpi;
    int gap=w_theme_spacing(dpi),button=w_theme_button_size(dpi),pad=2*gap;
    RECT box={pad,gap,r.right-button-2*gap,gap+button};
    bool highlighted=hover && w_theme_high_contrast();
    COLORREF fg=highlighted ? w_theme_selection(true) : w_theme_foreground();
    w_text(dc,box,e->app,t->fonts.small,highlighted ? fg : w_theme_accent(),DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);
    RECT content={0,0,r.right,r.bottom-gap};
    if (IntersectRect(&box,&content,&t->title_box))
        w_text(dc,box,e->title,t->fonts.bold,fg,(*e->body ? DT_SINGLELINE : DT_WORDBREAK)|DT_END_ELLIPSIS);
    if (IntersectRect(&box,&content,&t->body_box))
        w_text(dc,box,e->body,t->fonts.regular,fg,DT_WORDBREAK|DT_END_ELLIPSIS);
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
    case WM_SETTINGCHANGE:
        if (wp!=SPI_SETNONCLIENTMETRICS) break;
        /* fall through */
    case WM_FONTCHANGE:
        t->fonts.dpi=0;
        w_theme_surface_message(w,msg,wp,lp);
        w_position_toasts();
        return 0;
    case WM_SIZE:case WM_DPICHANGED:case winwm_theme_updated:
        if (t->close) {
            UINT dpi=msg==WM_DPICHANGED ? HIWORD(wp) : GetDpiForWindow(w);
            RECT r;GetClientRect(w,&r);
            if (t->tracking==w && !t->closing) w_hover_target(w,0,&r);
            int gap=w_theme_spacing(dpi),button=w_theme_button_size(dpi);
            SetWindowPos(t->close,NULL,max(0,r.right-gap-button),gap,button,button,SWP_NOACTIVATE|SWP_NOZORDER);
            if (msg==WM_DPICHANGED) {
                w_fonts_at(t->hwnd,&t->fonts,dpi);
                InvalidateRect(w,NULL,FALSE);
            }
        }
        return 0;
    case WM_DRAWITEM:{
        LRESULT answer;
        if (w_theme_message(w,msg,wp,lp,&answer)) return answer;
        break;
    }
    case WM_COMMAND:
        if (LOWORD(wp)==close_id && HIWORD(wp)==BN_CLICKED) {
            w_hide_toast(t,true);w_position_toasts();return 0;
        }
        break;
    case WM_PRINTCLIENT:w_paint_toast(t,(HDC)wp);return 0;
    case WM_PAINT:{
        w_paint_frame frame;
        HDC dc=w_begin_frame(w,&frame);
        if (dc) w_paint_toast(t,dc);
        w_end_frame(w,&frame);
        return 0;
    }
    case WM_TIMER:
        if (wp==layer_fade_timer && !w_layer_fade_tick(w,&t->fade,motion) && t->closing) {
            DestroyWindow(w);w_position_toasts();
        } else if (wp==expiry_timer && settings.duration_ms && !t->hover && !t->closing && GetTickCount64()>=t->deadline) {
            w_finish_balloon(t,NIN_BALLOONTIMEOUT);
            w_hide_toast(t,true);w_position_toasts();
        }
        return 0;
    case WM_MOUSEMOVE:case WM_MOUSELEAVE:w_toast_pointer(t,w,msg);return 0;
    case WM_CAPTURECHANGED:
        if (t->pressed) { t->pressed=false; InvalidateRect(w,NULL,FALSE); }
        return 0;
    case WM_LBUTTONDOWN:
        if (!t->closing) { t->pressed=true;SetCapture(w);InvalidateRect(w,NULL,FALSE); }
        return 0;
    case WM_LBUTTONUP:
        if (t->pressed && !t->closing) {
            t->pressed=false;RECT r;GetClientRect(w,&r);
            InvalidateRect(w,NULL,FALSE);
            if (GetCapture()==w) ReleaseCapture();
            POINT cursor={GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
            if (!PtInRect(&r,cursor)) return 0;
            if (t->balloon_event) {
                w_finish_balloon(t,NIN_BALLOONUSERCLICK);
                w_hide_toast(t,true);w_position_toasts();
            } else {
                uint64_t id=t->id;HMONITOR monitor=t->monitor;
                w_notice_open(monitor,NULL);
                if (w_notice_visible()) {panel.selected=id;w_filter(false);w_set_details(true);}
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

static void w_list_width(void) {
    RECT client;
    GetClientRect(panel.list,&client);
    int width=max(1,client.right);
    if (ListView_GetColumnWidth(panel.list,0)!=width) ListView_SetColumnWidth(panel.list,0,width);
}

static void w_reveal_detail(void) {
    bool open=panel.expanded && panel.selected;
    int before=panel.reveal.value;
    if (motion && w_notice_visible()) w_disclosure_to(panel.hwnd,&panel.reveal,detail_timer,open);
    else w_disclosure_stop(panel.hwnd,&panel.reveal,detail_timer,open);
    bool visible=(GetWindowLongW(panel.detail_clip,GWL_STYLE)&WS_VISIBLE)!=0;
    if (before!=panel.reveal.value || visible!=(open || panel.reveal.value>0) || !panel.fit.width) {
        if (w_notice_visible()) w_position_panel();
        w_layout_panel();
    }
}

static void w_detail(void) {
    w_entry *e=w_find(panel.selected);
    bool expanded=panel.expanded && e;
    if (expanded && panel.displayed_revision!=e->revision) {
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
            DWORD start=0,end=0;
            int first_line=0;
            if (panel.displayed_item==e->id) {
                SendMessageW(panel.detail,EM_GETSEL,(WPARAM)&start,(LPARAM)&end);
                first_line=(int)SendMessageW(panel.detail,EM_GETFIRSTVISIBLELINE,0,0);
            }
            SetWindowTextW(panel.detail,s);free(s);
            SendMessageW(panel.detail,EM_SETSEL,start,end);
            int now=(int)SendMessageW(panel.detail,EM_GETFIRSTVISIBLELINE,0,0);
            if (now!=first_line) SendMessageW(panel.detail,EM_LINESCROLL,0,first_line-now);
            panel.displayed_item=e->id;
            panel.displayed_revision=e->revision;
            panel.fit.width=0;
        }
    } else if (!expanded && panel.displayed_revision && panel.displayed_item!=panel.selected) {
        panel.displayed_item=panel.displayed_revision=0;
        SetWindowTextW(panel.detail,L"");
        panel.fit.width=0;
    }
    EnableWindow(panel.dismiss,e!=NULL);
    EnableWindow(panel.clear,entry_count>0);
    if (!!(GetWindowLongW(panel.clear,GWL_STYLE)&WS_VISIBLE)!=(entry_count>0)) {
        panel.fit.width=0;
        ShowWindow(panel.dismiss,entry_count ? SW_SHOWNA : SW_HIDE);
        ShowWindow(panel.clear,entry_count ? SW_SHOWNA : SW_HIDE);
    }
    EnableWindow(panel.detail_clip,expanded);
    w_icon_button(panel.read,expanded ? L"\xe70d" : L"\xe76c",expanded ? L"Hide notification details" : L"Read selected notification");
    w_place_read_button();
    w_reveal_detail();
}

static void w_follow_selection(void) {
    int row=w_selected_row();
    uint64_t selected=(unsigned)row<(unsigned)match_count ? matches[row]->id : 0;
    if (panel.selected==selected) return;
    panel.selected=selected;
    if (!selected) {
        panel.expanded=false;
        HWND focus=GetFocus();
        if (focus==panel.detail || focus==panel.dismiss || focus==panel.read) SetFocus(panel.list);
    }
    w_detail();
}

static bool w_substring(const wchar_t *field,const wchar_t *word) {
    return FindStringOrdinal(FIND_FROMSTART,field,-1,word,-1,TRUE)>=0;
}

static void w_filter(bool reset) {
    if (!panel.list) return;
    HWND focus=GetFocus();
    wchar_t query[256],*words[128],*context=NULL;int n=0;
    GetWindowTextW(panel.search,query,count_of(query));
    for (wchar_t *p=wcstok_s(query,L" \r\t\n",&context);p;p=wcstok_s(NULL,L" \r\t\n",&context)) words[n++]=p;
    uint64_t selected=panel.selected;
    panel.filtering=true;
    match_count=0;
    int row=-1, first=-1, changed_first=-1, changed_last=-1;
    for (int i=0;i<entry_count;++i) {
        w_entry *e=history[i];bool match=true;
        for (int j=0;j<n && match;++j)
            match=w_substring(e->app,words[j]) || w_substring(e->title,words[j]) || w_substring(e->body,words[j]);
        if (match) {
            if (panel.row_revisions[match_count]!=e->revision) {
                if (changed_first<0) changed_first=match_count;
                changed_last=match_count;
                panel.row_revisions[match_count]=e->revision;
            }
            if (e->id==selected) row=match_count;
            if (e->id==panel.first_visible) first=match_count;
            matches[match_count++]=e;
        }
    }
    int previous_count=ListView_GetItemCount(panel.list);
    if (row<0 || reset) {
        row=match_count && (reset || selected || !previous_count) ? 0 : -1;
        panel.expanded=false;
    }
    if (previous_count!=match_count) {
        ListView_SetItemCountEx(panel.list,match_count,LVSICF_NOSCROLL);
        w_list_width();
    } else if (changed_first>=0) ListView_RedrawItems(panel.list,changed_first,changed_last);
    if (row>=0) {
        if (row!=w_selected_row())
            ListView_SetItemState(panel.list,row,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
        if (reset || selected!=matches[row]->id) ListView_EnsureVisible(panel.list,row,FALSE);
    } else if (w_selected_row()>=0) ListView_SetItemState(panel.list,-1,0,LVIS_SELECTED);
    if (!reset && first>=0) {
        RECT bounds;
        if (ListView_GetItemRect(panel.list,first,&bounds,LVIR_BOUNDS) && bounds.top!=panel.first_offset)
            ListView_Scroll(panel.list,0,bounds.top-panel.first_offset);
    }
    panel.first_visible=0;
    panel.selected=row>=0 ? matches[row]->id : 0;
    panel.filtering=false;
    if (!match_count) SetWindowTextW(panel.empty,n ? L"No matching notifications" : L"No notifications");
    ShowWindow(panel.empty,match_count ? SW_HIDE : SW_SHOWNA);
    ShowWindow(panel.list,match_count ? SW_SHOWNA : SW_HIDE);
    w_detail();
    if (focus && IsChild(panel.hwnd,focus) && w_notice_visible() &&
        (!IsWindowVisible(focus) || !IsWindowEnabled(focus) || (focus==panel.detail && !panel.expanded)))
        SetFocus(match_count ? panel.list : panel.search);
}

static void w_set_details(bool open) {
    int row=w_selected_row();
    if ((unsigned)row>=(unsigned)match_count) return;
    panel.selected=matches[row]->id;
    panel.expanded=open;
    if (!open) SetFocus(panel.list);
    w_detail();
    if (open) SetFocus(panel.detail);
}

static void w_dismiss_selected(void) {
    int row=w_selected_row();
    if ((unsigned)row>=(unsigned)match_count) return;
    int index=w_index(matches[row]->id);
    if (index<0) return;
    panel.selected=match_count>1 ? matches[row+1<match_count ? row+1 : row-1]->id : 0;
    w_remove_record(index);panel.opening_rows=min(panel.opening_rows,entry_count);
    w_filter(false);w_position_panel();w_position_toasts();
}

static void w_clear(void) {
    while (entry_count) w_remove_record(entry_count-1);
    panel.opening_rows=0;panel.selected=0;panel.expanded=false;w_filter(true);w_position_panel();
}

static void w_place(HWND w,int x,int y,int width,int height) {
    width=max(1,width); height=max(0,height);
    RECT from;
    if (GetWindowRect(w,&from)) {
        MapWindowPoints(NULL,GetParent(w),(POINT *)&from,2);
        if (from.left==x && from.top==y && from.right-x==width && from.bottom-y==height) return;
    }
    SetWindowPos(w,NULL,x,y,width,height,SWP_NOZORDER|SWP_NOACTIVATE);
}

// Measure on content, width or font changes. Animation ticks reuse the same
// line breaks, so the native edit keeps its scroll position during a reveal.
static void w_measure_history(int outer) {
    if (panel.fit.width==outer || !panel.empty || !panel.fonts.dpi) return;
    UINT dpi=panel.fonts.dpi;
    int gap=w_theme_spacing(dpi),button=w_theme_button_size(dpi),pad=2*gap;
    int width=max(1,outer-2*pad);
    panel.fit.width=outer;
    panel.fit.actions=entry_count ? 3 : 1;
    panel.fit.columns=max(1,min(panel.fit.actions,(width+gap)/(button+gap)));
    panel.fit.wrap=width<panel.fit.actions*(button+gap)+w_theme_text_width(panel.fonts.title,L"Notifications",DT_NOPREFIX);
    panel.fit.header=max(w_theme_line_height(panel.fonts.title),button);
    panel.fit.field=max(button,panel.fonts.body_line+2);
    panel.fit.status=request_access ? button : 0;
    if (*access_status) {
        RECT text={0,0,max(1,width-(request_access ? button+gap : 0)),0};
        HDC dc=GetDC(panel.hwnd);
        int height=panel.fonts.small_line;
        if (dc) {
            HFONT old=SelectObject(dc,panel.fonts.small);
            height=max(height,w_theme_draw_text(dc,access_status,-1,&text,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX));
            SelectObject(dc,old);
            ReleaseDC(panel.hwnd,dc);
        }
        panel.fit.status=max(panel.fit.status,height);
    }
    panel.fit.chrome=2*pad+panel.fit.header+gap+panel.fit.field+gap+
        (panel.fit.wrap ? ((panel.fit.actions+panel.fit.columns-1)/panel.fit.columns)*(button+gap) : 0)+
        (panel.fit.status ? panel.fit.status+gap : 0);
    int inner=max(1,width-2*gap);
    RECT current;
    GetWindowRect(panel.detail,&current);
    // A multiline EDIT does not establish its formatting rectangle when its
    // viewport is shorter than a text line, even after its width is correct.
    int edit_height=max(panel.fonts.body_line,current.bottom-current.top);
    if (current.right-current.left!=inner || current.bottom-current.top!=edit_height)
        w_place(panel.detail,0,0,inner,edit_height);
    int lines=w_clamp_int((int)SendMessageW(panel.detail,EM_GETLINECOUNT,0,0),1,8);
    HFONT actual=(HFONT)SendMessageW(panel.detail,WM_GETFONT,0,0);
    panel.fit.detail_line=w_theme_line_height(actual ? actual : panel.fonts.regular);
    panel.fit.detail=2*gap+lines*panel.fit.detail_line;
}

static void w_layout_panel(void) {
    if (!panel.empty) return;
    RECT r;GetClientRect(panel.hwnd,&r);
    UINT dpi=panel.fonts.dpi;int gap=w_theme_spacing(dpi),pad=2*gap,button=w_theme_button_size(dpi);
    int width=max(1,(int)r.right-2*pad);
    w_measure_history(r.right);
    RECT old_search=panel.search_box,old_detail=panel.detail_box;
    HWND tools[]={panel.close,panel.clear,panel.dismiss};
    int columns=max(1,panel.fit.columns);
    int actions=panel.fit.actions*(button+gap)-gap;
    bool wrapped=panel.fit.wrap;
    int top=panel.vertical_pad;
    panel.heading=(RECT){pad,top,wrapped ? pad+width : pad+width-actions-gap,top+panel.fit.header};
    if (wrapped) top+=panel.fit.header+gap;
    for (int i=0;i<panel.fit.actions;++i) {
        int x=pad+width-button-((int)i%columns)*(button+gap);
        int y=top+((int)i/columns)*(button+gap)+(wrapped ? 0 : (panel.fit.header-button)/2);
        w_place(tools[i],max(pad,x),y,button,button);
    }
    top+=wrapped ? ((panel.fit.actions-1)/columns+1)*(button+gap) : panel.fit.header+gap;
    int search_width=max(1,width-(panel.read_in_search ? button+gap : 0));
    panel.search_box=(RECT){pad,top,pad+search_width,top+panel.fit.field};
    int edit_height=panel.fonts.body_line;
    w_place(panel.search,pad+gap,top+(panel.fit.field-edit_height)/2,search_width-2*gap,edit_height);
    w_native_edit_frame(panel.search,&panel.search_box);
    top+=panel.fit.field+gap;
    int bottom=(int)r.bottom-panel.vertical_pad;
    bool status_visible=!panel.defer_status && *access_status, access_visible=!panel.defer_status && request_access;
    if (!!(GetWindowLongW(panel.status,GWL_STYLE)&WS_VISIBLE)!=status_visible)
        ShowWindow(panel.status,status_visible ? SW_SHOWNA : SW_HIDE);
    if (!!(GetWindowLongW(panel.access,GWL_STYLE)&WS_VISIBLE)!=access_visible)
        ShowWindow(panel.access,access_visible ? SW_SHOWNA : SW_HIDE);
    if (status_visible || access_visible) {
        int text_width=width-(request_access ? button+gap : 0);
        int height=panel.fit.status;
        bottom-=height;
        if (request_access) w_place(panel.access,pad+width-button,bottom+(height-button)/2,button,button);
        if (*access_status) w_place(panel.status,pad,bottom,text_width,height);
        bottom-=gap;
    }
    SetRectEmpty(&panel.detail_box);
    bool detail_visible=panel.expanded || panel.reveal.value>0;
    if (detail_visible) {
        int full=panel.detail_viewport;
        int height=MulDiv(full,panel.reveal.value,255);
        bottom-=height;
        if (height) panel.detail_box=(RECT){pad,bottom,pad+width,bottom+height};
        // The edit keeps its final wrapping width and viewport while this child
        // reveals its contents. Resizing the edit every frame moves its caret.
        w_place(panel.detail,0,0,width-2*gap,max(1,full-2*gap));
        w_place(panel.detail_clip,pad+gap,bottom+min(gap,height),width-2*gap,max(0,height-2*gap));
        bottom-=MulDiv(panel.detail_gap,panel.reveal.value,255);
    }
    if (!!(GetWindowLongW(panel.detail_clip,GWL_STYLE)&WS_VISIBLE)!=detail_visible)
        ShowWindow(panel.detail_clip,detail_visible ? SW_SHOWNA : SW_HIDE);
    w_place(panel.list,pad,top,width,bottom-top);w_place(panel.empty,pad,top,width,min(panel.row_height,bottom-top));
    w_list_width();
    w_place_read_button();
    if (!EqualRect(&old_search,&panel.search_box)) InvalidateRect(panel.hwnd,NULL,FALSE);
    else if (!EqualRect(&old_detail,&panel.detail_box)) {
        InvalidateRect(panel.hwnd,&old_detail,FALSE);
        InvalidateRect(panel.hwnd,&panel.detail_box,FALSE);
    }
}

static void w_panel_style(UINT dpi) {
    w_fonts_at(panel.hwnd,&panel.fonts,dpi);
    int height=max(w_theme_button_size(dpi),panel.fonts.small_line+panel.fonts.heading_line)+2*w_theme_spacing(dpi);
    if (height!=panel.row_height && w_native_list_row_height(panel.list,height)) panel.row_height=height;
    COLORREF background=w_theme_background();
    if (ListView_GetBkColor(panel.list)!=background) ListView_SetBkColor(panel.list,background);
    if (ListView_GetTextBkColor(panel.list)!=background) ListView_SetTextBkColor(panel.list,background);
}

static void w_position_panel(void) {
    if (!panel.hwnd) return;
    RECT work=w_work(&panel.monitor);
    UINT dpi=w_monitor_dpi(panel.monitor);
    int gap=w_theme_spacing(dpi),width=px(480,dpi),available;
    w_panel_style(dpi);
    RECT r;
    if (panel.anchor.bar) {
        if (!IsWindowVisible(panel.anchor.bar)) { w_close_panel(false);return; }
        RECT area=w_popup_area(panel.anchor.bar);
        r=w_popup_bounds(&panel.anchor,area,width,area.bottom-area.top,gap);
        width=r.right-r.left;
        available=r.bottom-r.top;
    } else {
        int margin=min(px(settings.margin,dpi),max(0,(int)min(work.right-work.left,work.bottom-work.top)/4));
        InflateRect(&work,-margin,-margin);
        width=max(1,min(width,work.right-work.left));
        available=work.bottom-work.top;
    }
    w_measure_history(width);
    int row=max(1,panel.row_height);
    int chrome=panel.fit.chrome-4*gap;
    // Spend outer vertical padding only after a complete history row fits.
    panel.vertical_pad=w_clamp_int((available-chrome-row)/2,0,2*gap);
    chrome+=2*panel.vertical_pad;
    int capacity=max(1,(min(available,px(554,dpi))-chrome)/row);
    int rows=w_clamp_int(panel.opening_rows,1,capacity);
    int collapsed=min(available,chrome+rows*row);
    int minimum=2*gap+panel.fit.detail_line;
    // Keep a readable line in the focused editor before allocating whole
    // history rows. On a short monitor its collapse action moves to Search.
    bool reading=panel.expanded || panel.reveal.value>0;
    panel.defer_status=reading && panel.fit.status && available-chrome<minimum;
    if (panel.defer_status) chrome-=panel.fit.status+gap;
    int room=max(0,available-chrome);
    int visible_rows=min(rows,max(0,(room-minimum-gap)/row));
    panel.read_in_search=panel.expanded && !visible_rows;
    panel.detail_gap=visible_rows ? gap : 0;
    panel.detail_viewport=min(panel.fit.detail,max(0,room-visible_rows*row-panel.detail_gap));
    int expanded=chrome+visible_rows*row+panel.detail_gap+panel.detail_viewport;
    int height=collapsed+MulDiv(expanded-collapsed,panel.reveal.value,255);
    if (panel.anchor.bar) r=w_popup_bounds(&panel.anchor,w_popup_area(panel.anchor.bar),width,height,gap);
    else {
        height=max(1,min(height,work.bottom-work.top));
        r.left=w_left()?work.left:work.right-width;r.top=w_bottom()?work.bottom-height:work.top;
        r.right=r.left+width;r.bottom=r.top+height;
    }
    RECT old;
    if (!GetWindowRect(panel.hwnd,&old) || !EqualRect(&r,&old))
        SetWindowPos(panel.hwnd,HWND_TOPMOST,r.left,r.top,r.right-r.left,r.bottom-r.top,SWP_NOACTIVATE);
}

static RECT w_row_disclosure(RECT row) {
    int size=w_theme_button_size(panel.fonts.dpi);
    row.right-=w_theme_spacing(panel.fonts.dpi);
    row.left=row.right-size;
    row.top+=(row.bottom-row.top-size)/2;
    row.bottom=row.top+size;
    return row;
}

// The selected row exposes one native, cursor-independent Invoke action.
static void w_place_read_button(void) {
    if (!panel.read) return;
    int i=w_selected_row();
    RECT row,client;
    bool show=(unsigned)i<(unsigned)match_count && ListView_GetItemRect(panel.list,i,&row,LVIR_BOUNDS);
    if (panel.read_in_search && panel.expanded && (unsigned)i<(unsigned)match_count) {
        int gap=w_theme_spacing(panel.fonts.dpi),button=w_theme_button_size(panel.fonts.dpi);
        w_place(panel.read,panel.search_box.right+gap,panel.search_box.top+(panel.fit.field-button)/2,button,button);
        show=true;
    } else if (show) {
        RECT button=w_row_disclosure(row);
        GetClientRect(panel.list,&client);
        show=button.top>=0 && button.bottom<=client.bottom && button.left>=0 && button.right<=client.right;
        if (show) {
            MapWindowPoints(panel.list,panel.hwnd,(POINT *)&button,2);
            w_place(panel.read,button.left,button.top,button.right-button.left,button.bottom-button.top);
        }
    }
    if (!!(GetWindowLongW(panel.read,GWL_STYLE)&WS_VISIBLE)!=show) {
        if (!show && GetFocus()==panel.read) SetFocus(match_count ? panel.list : panel.search);
        ShowWindow(panel.read,show ? SW_SHOWNA : SW_HIDE);
    }
}

static LRESULT w_draw_row(NMLVCUSTOMDRAW *draw) {
    if (draw->nmcd.dwDrawStage==CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
    if (draw->nmcd.dwDrawStage!=CDDS_ITEMPREPAINT) return CDRF_DODEFAULT;
    int i=(int)draw->nmcd.dwItemSpec;if ((unsigned)i>=(unsigned)match_count) return CDRF_SKIPDEFAULT;
    w_entry *e=matches[i];RECT r;
    if (!ListView_GetItemRect(panel.list,i,&r,LVIR_BOUNDS)) return CDRF_SKIPDEFAULT;
    UINT state=ListView_GetItemState(panel.list,i,LVIS_SELECTED|LVIS_FOCUSED);
    bool selected=(state&LVIS_SELECTED)!=0;
    RECT focus=r;
    HDC dc=draw->nmcd.hdc;int saved=SaveDC(dc);UINT dpi=panel.fonts.dpi;
    int hover=w_hover_amount(panel.list,i);
    bool highlighted=selected || (hover && w_theme_high_contrast());
    COLORREF fg=w_hover_row(dc,&r,selected,hover,false);
    if (selected) {
        RECT stripe=r;stripe.right=stripe.left+px(3,dpi);
        SetDCBrushColor(dc,w_theme_high_contrast() ? w_theme_selection(true) : w_theme_accent());
        FillRect(dc,&stripe,GetStockObject(DC_BRUSH));
    }
    int gap=w_theme_spacing(dpi);
    RECT toggle=w_row_disclosure(r);
    InflateRect(&r,-2*gap,-gap);
    r.right=max(r.left,toggle.left-gap);
    RECT app=r,date=r;app.bottom=app.top+panel.fonts.small_line;date.bottom=app.bottom;
    SIZE size={0};SelectObject(dc,panel.fonts.small);w_theme_text_extent(dc,e->stamp,(int)wcslen(e->stamp),&size);
    date.left=max(r.left+(r.right-r.left)/2,r.right-size.cx);app.right=date.left-gap;
    UINT flags=DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS;
    w_text(dc,app,e->app,panel.fonts.small,highlighted?fg:w_theme_accent(),flags);
    w_text(dc,date,e->stamp,panel.fonts.small,highlighted?fg:w_theme_hint(),flags|DT_RIGHT);
    r.top=app.bottom;w_text(dc,r,*e->title?e->title:e->body,panel.fonts.bold,fg,flags);
    if ((state&LVIS_FOCUSED) && GetFocus()==panel.list && !(SendMessageW(panel.list,WM_QUERYUISTATE,0,0)&UISF_HIDEFOCUS)) {
        InflateRect(&focus,-px(2,dpi),-px(2,dpi));
        DrawFocusRect(dc,&focus);
    }
    RestoreDC(dc,saved);
    return CDRF_SKIPDEFAULT;
}

static LRESULT CALLBACK w_search_proc(HWND w,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR data) {
    (void)id;(void)data;
    if (msg==WM_IME_STARTCOMPOSITION) panel.composing=true;
    if (msg==WM_IME_ENDCOMPOSITION || msg==WM_KILLFOCUS) panel.composing=false;
    return DefSubclassProc(w,msg,wp,lp);
}

static LRESULT CALLBACK w_clip_proc(HWND w,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR data) {
    (void)data;
    if (msg==WM_CTLCOLORSTATIC || msg==WM_CTLCOLOREDIT) return SendMessageW(panel.hwnd,msg,wp,lp);
    if (msg==WM_NCDESTROY) RemoveWindowSubclass(w,w_clip_proc,id);
    return DefSubclassProc(w,msg,wp,lp);
}

static LRESULT CALLBACK w_history_proc(HWND w,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR data) {
    (void)data;
    if (msg==WM_NCDESTROY) RemoveWindowSubclass(w,w_history_proc,id);
    // A native move between rows first clears the old selection. Reconcile once
    // the complete list operation returns, including bulk owner-data changes.
    bool nested=panel.list_dispatch;
    panel.list_dispatch=true;
    LRESULT result=DefSubclassProc(w,msg,wp,lp);
    panel.list_dispatch=nested;
    if (!nested && panel.selection_changed) {
        panel.selection_changed=false;
        w_follow_selection();
    }
    if (msg==WM_VSCROLL || msg==WM_MOUSEWHEEL || msg==LVM_SCROLL || msg==LVM_ENSUREVISIBLE ||
        msg==WM_KEYDOWN) w_place_read_button();
    return result;
}

static bool w_create_panel(void) {
    INITCOMMONCONTROLSEX controls={sizeof(controls),ICC_LISTVIEW_CLASSES};InitCommonControlsEx(&controls);
    panel.hwnd=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_TOPMOST|WS_EX_CONTROLPARENT,
        panel_class,L"Notifications",WS_POPUP|WS_CLIPCHILDREN,0,0,1,1,owner,NULL,GetModuleHandleW(NULL),NULL);
    if (!panel.hwnd) return false;
    panel.search=w_child(L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP,search_id);
    panel.list=w_child(WC_LISTVIEWW,L"Notification history",WS_CLIPSIBLINGS|LVS_OWNERDATA|LVS_REPORT|LVS_NOCOLUMNHEADER|
             LVS_SINGLESEL|LVS_SHOWSELALWAYS|WS_TABSTOP,list_id);
    panel.read=w_button(read_id,L"\xe76c",L"Read selected notification");
    panel.detail_clip=CreateWindowExW(WS_EX_CONTROLPARENT,L"STATIC",L"",WS_CHILD|WS_CLIPCHILDREN,
            0,0,1,0,panel.hwnd,NULL,GetModuleHandleW(NULL),NULL);
    if (panel.detail_clip) panel.detail=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_VISIBLE|ES_READONLY|
            ES_MULTILINE|ES_AUTOVSCROLL|WS_VSCROLL|WS_TABSTOP,0,0,1,1,panel.detail_clip,NULL,GetModuleHandleW(NULL),NULL);
    panel.clear=w_button(clear_id,L"\xe74d",L"Clear notification history");
    panel.dismiss=w_button(dismiss_id,L"\xecc9",L"Dismiss selected notification");
    panel.close=w_button(close_id,L"\xe711",L"Close notifications");
    panel.access=w_button(access_id,L"\xe713",L"Notification capture options");
    panel.status=w_child(L"STATIC",access_status,SS_LEFT|SS_NOPREFIX,0);
    panel.empty=w_child(L"STATIC",L"No notifications",SS_CENTER|SS_CENTERIMAGE,0);
    if (!panel.search||!panel.list||!panel.read||!panel.detail||!panel.clear||!panel.dismiss||
        !panel.close||!panel.access||!panel.status||!panel.empty) {
        DestroyWindow(panel.hwnd);return false;
    }
    SetWindowSubclass(panel.search,w_search_proc,1,0);
    SetWindowSubclass(panel.list,w_history_proc,1,0);
    SetWindowPos(panel.read,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_HIDEWINDOW);
    SetWindowSubclass(panel.detail_clip,w_clip_proc,1,0);
    HRESULT apartment=CoInitializeEx(NULL,COINIT_APARTMENTTHREADED);
    panel.uninitialize_com=SUCCEEDED(apartment);
    if (panel.uninitialize_com || apartment==RPC_E_CHANGED_MODE)
        CoCreateInstance(&CLSID_AccPropServices,NULL,CLSCTX_INPROC_SERVER,&IID_IAccPropServices,(void **)&panel.accessibility);
    if (panel.accessibility) {
        IAccPropServices_SetHwndPropStr(panel.accessibility,panel.list,OBJID_CLIENT,CHILDID_SELF,PROPID_ACC_DESCRIPTION,
                L"Press Enter or double-click to read. You can also invoke the selected notification's chevron. Press Escape to return to the list.");
        IAccPropServices_SetHwndPropStr(panel.accessibility,panel.detail,OBJID_CLIENT,CHILDID_SELF,PROPID_ACC_NAME,
                L"Notification details");
    }
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
    if (msg==WM_ERASEBKGND) return 1;
    LRESULT answer;
    if (w_theme_message(w,msg,wp,lp,&answer)) return answer;
    if (panel.list && (msg==WM_FONTCHANGE || (msg==WM_SETTINGCHANGE && wp==SPI_SETNONCLIENTMETRICS))) {
        panel.fonts.dpi=0;
        if (w_notice_visible()) w_position_panel();
        else w_panel_style(GetDpiForWindow(w));
        w_layout_panel();
    }
    switch (msg) {
    case WM_PRINTCLIENT:case WM_PAINT: {
        PAINTSTRUCT ps;HDC dc=msg==WM_PAINT ? BeginPaint(w,&ps) : (HDC)wp;
        RECT r;GetClientRect(w,&r);FillRect(dc,&r,w_theme_brush());
        w_theme_frame(dc,&r,panel.fonts.dpi);
        w_theme_input_frame(dc,&panel.search_box,w_hover_amount(panel.search,0),GetFocus()==panel.search,IsWindowEnabled(panel.search));
        if (!IsRectEmpty(&panel.detail_box)) w_theme_control_frame(dc,&panel.detail_box,panel.fonts.dpi);
        w_text(dc,panel.heading,L"Notifications",panel.fonts.title,w_theme_foreground(),DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);
        if (msg==WM_PAINT) EndPaint(w,&ps);
        return 0;
    }
    case WM_SIZE:w_layout_panel();return 0;
    case winwm_theme_updated:
        if (panel.list) {
            panel.fit.width=0;
            if (w_notice_visible()) w_position_panel();
            else w_panel_style(GetDpiForWindow(w));
            w_layout_panel();
        }
        return 0;
    case WM_LBUTTONDOWN: {
        POINT cursor={GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
        if (PtInRect(&panel.search_box,cursor)) SetFocus(panel.search);
        else if (panel.expanded && PtInRect(&panel.detail_box,cursor)) SetFocus(panel.detail);
        break;
    }
    case WM_DPICHANGED:if (panel.list) {w_panel_style(HIWORD(wp));w_layout_panel();}return 0;
    case WM_DISPLAYCHANGE:if (w_notice_visible() && !panel.anchor.bar) w_position_panel();return 0;
    case WM_COMMAND:
        if (LOWORD(wp)==search_id && HIWORD(wp)==EN_CHANGE) {w_filter(true);return 0;}
        if (HIWORD(wp)!=BN_CLICKED) break;
        switch (LOWORD(wp)) {
        case read_id:w_set_details(!panel.expanded);return 0;
        case clear_id:w_clear();SetFocus(panel.search);return 0;
        case dismiss_id:w_dismiss_selected();return 0;
        case close_id:w_notice_close(true);return 0;
        case access_id:if (request_access) request_access(panel.access);return 0;
        }
        break;
    case WM_NOTIFY: {
        NMHDR *note=(NMHDR*)lp;if (note->hwndFrom!=panel.list) break;
        if (note->code==NM_CUSTOMDRAW) return w_draw_row((NMLVCUSTOMDRAW*)lp);
        if (note->code==LVN_ENDSCROLL) { w_place_read_button();return 0; }
        if (note->code==LVN_GETDISPINFOW) {
            LVITEMW *item=&((NMLVDISPINFOW*)lp)->item;
            if ((item->mask&LVIF_TEXT) && (unsigned)item->iItem<(unsigned)match_count && item->pszText && item->cchTextMax>0) {
                w_entry *e=matches[item->iItem];wchar_t label[1536];
                swprintf(label,count_of(label),L"%ls  ·  %.1000ls  ·  %ls",e->app,*e->title?e->title:e->body,e->stamp);
                lstrcpynW(item->pszText,label,item->cchTextMax);
            }
            return 0;
        }
        if ((note->code==LVN_ITEMCHANGED || note->code==LVN_ODSTATECHANGED) && !panel.filtering && match_count) {
            if (panel.list_dispatch) panel.selection_changed=true;
            else w_follow_selection();
        }
        if ((note->code==NM_DBLCLK || note->code==LVN_ITEMACTIVATE) &&
            (unsigned)((NMITEMACTIVATE*)lp)->iItem<(unsigned)match_count) w_set_details(true);
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(wp)==WA_INACTIVE && w_notice_visible() && lp && !w_notice_contains((HWND)lp) &&
            (HWND)lp!=panel.anchor.bar) w_close_panel(false);
        break;
    case WM_CLOSE:w_notice_close(true);return 0;
    case winwm_motion_updated:w_reveal_detail();return 0;
    case WM_SETTINGCHANGE:
        if (wp==SPI_SETCLIENTAREAANIMATION || wp==SPI_SETHIGHCONTRAST) w_reveal_detail();
        break;
    case WM_SHOWWINDOW:
        if (!wp) {
            KillTimer(w,layer_fade_timer);
            panel.fade=(w_layer_fade){0};
            panel.expanded=false;
            w_disclosure_stop(w,&panel.reveal,detail_timer,false);
            w_layout_panel();
        }
        break;
    case WM_TIMER:
        if (wp==detail_timer && panel.reveal.running) {
            int before=panel.reveal.value;
            w_disclosure_tick(w,&panel.reveal,detail_timer);
            if (before!=panel.reveal.value) { w_position_panel(); w_layout_panel(); }
            return 0;
        }
        if (wp==layer_fade_timer && panel.fade.start && !w_layer_fade_tick(w,&panel.fade,motion) && panel.closing) ShowWindow(w,SW_HIDE);
        return 0;
    case WM_DESTROY:
        w_disclosure_stop(w,&panel.reveal,detail_timer,false);
        if (panel.accessibility) {
            IAccPropServices_ClearHwndProps(panel.accessibility,panel.list,OBJID_CLIENT,CHILDID_SELF,&PROPID_ACC_DESCRIPTION,1);
            IAccPropServices_ClearHwndProps(panel.accessibility,panel.detail,OBJID_CLIENT,CHILDID_SELF,&PROPID_ACC_NAME,1);
            IAccPropServices_Release(panel.accessibility);
        }
        if (panel.uninitialize_com) CoUninitialize();
        break;
    case WM_NCDESTROY:
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
        ((root==ImmGetDefaultIMEWnd(panel.search) || (GetClassLongPtrW(root,GCL_STYLE)&CS_IME)) &&
         (panel.composing || w_native_edit_ime_active(panel.search))))) return true;
    for (int i=0;i<max_toasts;++i) if (toasts[i].hwnd==root) return true;
    return false;
}

static void w_close_panel(bool restore) {
    if (!w_notice_visible()) return;
    panel.closing=true;
    bool focused=GetForegroundWindow()==panel.hwnd;
    SetWindowLongPtrW(panel.hwnd,GWL_EXSTYLE,GetWindowLongPtrW(panel.hwnd,GWL_EXSTYLE)|WS_EX_TRANSPARENT|WS_EX_NOACTIVATE);
    if (restore && focused && panel.previous && IsWindowVisible(panel.previous) && !IsIconic(panel.previous))
        SetForegroundWindow(panel.previous);
    if (GetForegroundWindow()==panel.hwnd || !w_fade(panel.hwnd,&panel.fade,0)) ShowWindow(panel.hwnd,SW_HIDE);
    panel.previous=NULL;
}

bool w_notice_close(bool restore_focus) {
    if (!w_notice_visible()) return false;
    w_close_panel(restore_focus);return true;
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
    if (msg->hwnd==panel.search && (panel.composing || w_native_ime_key(msg))) return false;
    if (msg->message==WM_KEYDOWN) {
        if ((msg->lParam & (1L<<30)) && (msg->wParam==VK_ESCAPE || msg->wParam==VK_RETURN ||
            (msg->wParam==VK_DELETE && msg->hwnd==panel.list))) return true;
        if (msg->wParam==VK_ESCAPE) {
            if (panel.expanded) w_set_details(false);
            else w_notice_close(true);
            return true;
        }
        if (msg->hwnd==panel.list && msg->wParam==VK_DELETE) {w_dismiss_selected();return true;}
        if ((msg->hwnd==panel.list || msg->hwnd==panel.search) && msg->wParam==VK_RETURN) {
            w_set_details(true);return true;
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
    if (anchor && !IsWindowVisible(anchor->bar)) return;
    bool was_open=w_notice_visible();
    if (was_open && anchor && w_popup_same_source(anchor,&panel.anchor)) {
        w_notice_close(true);return;
    }
    if (!panel.hwnd && !w_create_panel()) return;
    if (!was_open) panel.previous=GetForegroundWindow();
    panel.monitor=monitor?monitor:w_target_monitor();panel.anchor=anchor ? *anchor : (w_popup_anchor){0};
    panel.closing=false;
    SetWindowLongPtrW(panel.hwnd,GWL_EXSTYLE,GetWindowLongPtrW(panel.hwnd,GWL_EXSTYLE)&~(WS_EX_NOACTIVATE|WS_EX_TRANSPARENT));
    for (int i=0;i<max_toasts;++i) w_hide_toast(&toasts[i],false);
    if (!was_open) {
        panel.opening_rows=entry_count;
        w_disclosure_stop(panel.hwnd,&panel.reveal,detail_timer,false);
        panel.selected=0;panel.expanded=false;SetWindowTextW(panel.search,L"");
    }
    w_filter(!was_open);w_position_panel();
    w_layer_present(panel.hwnd,&panel.fade);
    ShowWindow(panel.hwnd,SW_SHOW);
    SetForegroundWindow(panel.hwnd);
    SetFocus(panel.search);
}

const w_popup_anchor *w_notice_anchor(void) {
    return w_notice_visible() && panel.anchor.bar ? &panel.anchor : NULL;
}

void w_notice_reanchor(const w_popup_anchor *anchor) {
    if (!w_notice_anchor()) return;
    if (!anchor) { w_close_panel(false);return; }
    panel.anchor=*anchor;
    panel.monitor=MonitorFromWindow(anchor->bar,MONITOR_DEFAULTTONEAREST);
    w_position_panel();
}

void w_notice_configure(HWND host,const w_notification_config *config,bool motion_allowed) {
    if (!owner) w_register();
    int old_duration=settings.duration_ms;
    bool retime=settings.fade_ms!=config->fade_ms || motion!=motion_allowed;
    owner=host;settings=*config;motion=motion_allowed;
    bool trimmed=entry_count>settings.history_limit;
    while (entry_count>settings.history_limit) w_remove_record(entry_count-1);
    for (int i=0;i<max_toasts;++i) {
        w_toast *t=&toasts[i];if (!t->hwnd) continue;
        if (!settings.enabled || (retime && w_retime_finished_exit(t->hwnd,&t->fade,t->closing))) {
            w_hide_toast(t,false);
            continue;
        }
        if (!t->closing && old_duration!=settings.duration_ms) {
            KillTimer(t->hwnd,expiry_timer);
            t->remaining=settings.duration_ms;
            if (t->remaining && !t->hover) w_expire(t,t->remaining);
        }
    }
    if (panel.hwnd) {
        w_reveal_detail();
        if (retime && w_retime_finished_exit(panel.hwnd,&panel.fade,panel.closing))
            ShowWindow(panel.hwnd,SW_HIDE);
        if (trimmed && IsWindowVisible(panel.hwnd)) w_filter(false);
    }
    w_notice_reposition();
}

void w_notice_access(const wchar_t *status,void (*request)(HWND button)) {
    if (!status) status=L"";
    if (request_access==request && !wcscmp(status,access_status)) return;
    lstrcpynW(access_status,status,count_of(access_status));request_access=request;
    if (panel.hwnd) {
        SetWindowTextW(panel.status,access_status);
        panel.fit.width=0;
        if (w_notice_visible()) w_position_panel();
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
        if (panel.hwnd && IsWindowVisible(panel.hwnd)) w_filter(false);
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
    w_invalidate_matches();
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
    if (panel.hwnd && IsWindowVisible(panel.hwnd)) {
        if (batching) filter_pending=true;
        else w_filter(false);
    }
    if (!visible && toast && settings.enabled) shown=w_show_toast(e,item);
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
    if (w_notice_visible() && !panel.anchor.bar) w_position_panel();
    w_position_toasts();
}

void w_notice_shutdown(void) {
    if (panel.hwnd) DestroyWindow(panel.hwnd);
    for (int i=0;i<max_toasts;++i) w_hide_toast(&toasts[i],false);
    while (entry_count) free(history[--entry_count]);
    owner=NULL;read_work=NULL;request_access=NULL;access_status[0]=0;match_count=0;
    batching=false;filter_pending=false;
}
