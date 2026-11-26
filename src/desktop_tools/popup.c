#include "desktop_tools/entry.h"
#include "desktop_tools/tools.h"
#include "win_platform/layer.h"
#include "win_platform/popup.h"
#include "win_platform/appearance.h"
#include "theme/theme.h"
#include <commctrl.h>

enum { popup_cancel = WM_APP + 0x431, popup_check_focus, popup_font_changed };
static w_popup_anchor anchor;
static wchar_t placement_key[popup_event_chars];
static HANDLE cancelled, manager, cancel_wait, manager_wait;
static DWORD manager_pid;
static w_layer_fade fade;
static bool motion, framed, closing, released;
static HHOOK native_hook;
static HWINEVENTHOOK modal_hook;
static HWND dismissing;
static SIZE preferred;
static UINT preferred_dpi;
static bool placing;
static HFONT heading;
static UINT heading_dpi;

bool w_tools_set_popup(HWND owner, const wchar_t *key, HANDLE cancel) {
    wchar_t cls[32];
    anchor.bar=owner;
    if (!cancel || !GetClassNameW(owner, cls, count_of(cls)) ||
        wcscmp(cls,L"WinwmBar") || !w_popup_read(&anchor,key) ||
        WaitForSingleObject(cancel,0) != WAIT_TIMEOUT)
        return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(owner, &pid);
    manager = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!manager) return false;
    manager_pid = pid;
    lstrcpynW(placement_key,key,count_of(placement_key));
    cancelled = cancel;
    return true;
}

HMONITOR w_tool_popup_monitor(void) {
    return anchor.bar ? MonitorFromWindow(anchor.bar, MONITOR_DEFAULTTONEAREST) : NULL;
}

int w_tool_feedback_height(HWND control,w_tool_feedback *flow,int width) {
    if (!control) return 0;
    width=max(1,width);
    if (width!=flow->width) {
        int n=GetWindowTextLengthW(control);
        wchar_t local[512],*text=n<(int)count_of(local) ? local : malloc(((size_t)n+1)*sizeof(*text));
        if (!text) return flow->height;
        GetWindowTextW(control,text,n+1);
        flow->height=*text ? w_theme_wrapped_height((HFONT)SendMessageW(control,WM_GETFONT,0,0),text,width) : 0;
        flow->width=width;
        if (text!=local) free(text);
    }
    return flow->height;
}

static LRESULT CALLBACK w_feedback_proc(HWND w,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR data) {
    if (msg==WM_NCDESTROY) RemoveWindowSubclass(w,w_feedback_proc,id);
    LRESULT result=DefSubclassProc(w,msg,wp,lp);
    if (msg==WM_SETTEXT || msg==WM_SETFONT) {
        w_tool_feedback *flow=(w_tool_feedback *)data;
        int width=flow->width,old=flow->height;
        flow->width=0;
        if (width && w_tool_feedback_height(w,flow,width)!=old)
            PostMessageW(GetParent(w),w_tool_feedback_changed,0,0);
    }
    return result;
}

void w_tool_track_feedback(HWND control,w_tool_feedback *flow) {
    SetWindowSubclass(control,w_feedback_proc,1,(DWORD_PTR)flow);
}

static int w_heading_height(HWND window) {
    UINT dpi = GetDpiForWindow(window);
    return w_theme_role_line_height(dpi,text_heading)+2*w_theme_spacing(dpi)+1;
}

static void w_draw_heading(HWND w) {
    RECT bounds;
    if (!GetWindowRect(w,&bounds)) return;
    OffsetRect(&bounds,-bounds.left,-bounds.top);
    HDC dc = GetWindowDC(w);
    if (!dc) return;
    RECT band = {0,0,bounds.right,min(bounds.bottom,w_heading_height(w))};
    FillRect(dc,&band,w_theme_brush());
    HBRUSH brush = GetStockObject(DC_BRUSH);
    SetDCBrushColor(dc,w_theme_separator());
    w_theme_frame(dc,&bounds,GetDpiForWindow(w));
    SetDCBrushColor(dc,w_theme_separator());
    int padding = 2*w_theme_spacing(GetDpiForWindow(w));
    RECT divider = {padding,band.bottom-1,bounds.right-padding,band.bottom};
    FillRect(dc,&divider,brush);
    UINT dpi = GetDpiForWindow(w);
    if (!heading || heading_dpi != dpi) {
        HFONT next=w_theme_role_font(dpi,text_heading);
        if (next) {
            DeleteObject(heading);
            heading=next;
            heading_dpi=dpi;
        }
    }
    HGDIOBJ old = SelectObject(dc,heading ? heading : GetStockObject(DEFAULT_GUI_FONT));
    wchar_t title[160];
    GetWindowTextW(w,title,count_of(title));
    band.left = padding;
    band.right -= padding;
    SetTextColor(dc,w_theme_foreground());
    SetBkMode(dc,TRANSPARENT);
    w_theme_draw_text(dc,title,-1,&band,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
    SelectObject(dc,old);
    ReleaseDC(w,dc);
}

static VOID CALLBACK w_cancelled(void *param, BOOLEAN timed_out) {
    (void)timed_out;
    PostMessageW((HWND)param, popup_cancel, 0, 0);
}

static void w_stop_waits(void) {
    if (cancel_wait) UnregisterWaitEx(cancel_wait,INVALID_HANDLE_VALUE);
    if (manager_wait) UnregisterWaitEx(manager_wait,INVALID_HANDLE_VALUE);
    cancel_wait = manager_wait = NULL;
}

static bool w_owned_by(HWND window, HWND owner) {
    for (HWND w=window; w; w=GetWindow(w,GW_OWNER))
        if (w==owner) return true;
    return false;
}

static BOOL CALLBACK w_cancel_dialog(HWND dialog, LPARAM owner) {
    DWORD pid=0;
    GetWindowThreadProcessId(dialog,&pid);
    if (pid!=GetCurrentProcessId() || dialog==(HWND)owner || !IsWindowVisible(dialog) || !IsWindowEnabled(dialog) ||
        !w_owned_by(dialog,(HWND)owner)) return TRUE;
    wchar_t cls[24];
    HWND cancel=NULL;
    if (GetClassNameW(dialog,cls,count_of(cls)) && !wcscmp(cls,L"#32770")) {
        cancel=GetDlgItem(dialog,IDCANCEL);
        if (!cancel) cancel=GetDlgItem(dialog,IDNO);
    }
    if (cancel && IsWindowEnabled(cancel))
        PostMessageW(dialog,WM_COMMAND,MAKEWPARAM(GetDlgCtrlID(cancel),BN_CLICKED),(LPARAM)cancel);
    else
        PostMessageW(dialog,WM_CLOSE,0,0);
    return FALSE;
}

static void w_release_popup(void) {
    if (released) return;
    released = true;
    HWND events = FindWindowW(L"WinwmEvents",NULL);
    DWORD pid = 0;
    GetWindowThreadProcessId(events,&pid);
    if (pid==manager_pid) PostMessageW(events,winwm_popup_released,GetCurrentProcessId(),0);
}

static void CALLBACK w_modal_shown(HWINEVENTHOOK hook, DWORD event, HWND dialog, LONG object,
                                   LONG child, DWORD thread, DWORD time) {
    (void)hook; (void)event; (void)thread; (void)time;
    if (object==OBJID_WINDOW && child==CHILDID_SELF && dialog!=dismissing &&
        !(GetWindowLongPtrW(dialog,GWL_STYLE)&WS_CHILD) && w_owned_by(dialog,dismissing))
        PostMessageW(dismissing,popup_cancel,0,0);
}

static LRESULT CALLBACK w_popup_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
    (void)data;
    if (framed && msg==WM_SETTINGCHANGE && wp==SPI_SETNONCLIENTMETRICS) {
        heading_dpi=0;
        LRESULT result=DefSubclassProc(w,msg,wp,lp);
        // Query the minimum after the panel has measured its new control fonts.
        if (IsWindow(w)) PostMessageW(w,popup_font_changed,0,0);
        return result;
    }
    if (framed && (msg==winwm_motion_updated || msg==WM_SYSCOLORCHANGE ||
        (msg==WM_SETTINGCHANGE && (wp==SPI_SETCLIENTAREAANIMATION || w_appearance_setting(wp,lp))))) {
        motion=w_theme_motion_enabled();
        if (fade.start && !w_layer_fade_to(w,&fade,fade.target,w_theme_layer_timing(!closing),motion) && closing)
            PostMessageW(w,WM_CLOSE,0,0);
    }
    if ((msg==winwm_popup_moved || msg==winwm_theme_updated || msg==popup_font_changed) && !closing && preferred_dpi) {
        if (!w_popup_read(&anchor,placement_key)) { PostMessageW(w,popup_cancel,0,0);return 0; }
        RECT old;
        if (GetWindowRect(w,&old)) {
            UINT dpi=GetDpiForWindow(anchor.bar);
            int width=MulDiv(preferred.cx,dpi,preferred_dpi), height=MulDiv(preferred.cy,dpi,preferred_dpi);
            if (framed) {
                // A popup has a custom nonclient heading. Tool minima describe its client content.
                MINMAXINFO limits={0};
                SendMessageW(w,WM_GETMINMAXINFO,0,(LPARAM)&limits);
                UINT current_dpi=GetDpiForWindow(w);
                width=max(width,MulDiv(limits.ptMinTrackSize.x+2,dpi,current_dpi));
                int minimum=MulDiv(limits.ptMinTrackSize.y+w_heading_height(w)+1,dpi,current_dpi);
                // Fixed-height tools express their new row budget through both track limits.
                height=limits.ptMinTrackSize.y>0 && limits.ptMaxTrackSize.y==limits.ptMinTrackSize.y ?
                       minimum : max(height,minimum);
            }
            RECT next=w_popup_bounds(&anchor,w_popup_area(anchor.bar),width,height,w_theme_spacing(dpi));
            if (!EqualRect(&old,&next) || msg==popup_font_changed) {
                placing=true;
                UINT flags=SWP_NOACTIVATE|SWP_NOZORDER;
                if (msg==popup_font_changed) flags|=SWP_FRAMECHANGED;
                SetWindowPos(w,NULL,next.left,next.top,next.right-next.left,next.bottom-next.top,flags);
                placing=false;
            }
        }
        if (msg!=winwm_theme_updated) return 0;
    }
    if (framed && msg == WM_NCCALCSIZE) {
        RECT *r = wp ? &((NCCALCSIZE_PARAMS *)lp)->rgrc[0] : (RECT *)lp;
        InflateRect(r,-1,-1);
        r->top = min(r->bottom,r->top + w_heading_height(w)-1);
        return 0;
    }
    if (framed && (msg == WM_NCPAINT || msg == WM_NCACTIVATE)) {
        w_draw_heading(w);
        return msg == WM_NCACTIVATE;
    }
    if (framed && msg == WM_SETTEXT) {
        LRESULT result = DefSubclassProc(w,msg,wp,lp);
        w_draw_heading(w);
        return result;
    }
    if (msg == WM_WINDOWPOSCHANGING && !closing) {
        WINDOWPOS *pos = (WINDOWPOS *)lp;
        if (!(pos->flags & SWP_NOSIZE) || !(pos->flags & SWP_NOMOVE)) {
            if (!placing && !(pos->flags&SWP_NOSIZE)) {
                preferred=(SIZE){pos->cx,pos->cy};
                preferred_dpi=GetDpiForWindow(w);
            }
            RECT current; GetWindowRect(w,&current);
            RECT r = w_popup_bounds(&anchor,w_popup_area(anchor.bar),
                (pos->flags & SWP_NOSIZE) ? current.right-current.left : pos->cx,
                (pos->flags & SWP_NOSIZE) ? current.bottom-current.top : pos->cy,w_theme_spacing(GetDpiForWindow(anchor.bar)));
            pos->x=r.left; pos->y=r.top; pos->cx=r.right-r.left; pos->cy=r.bottom-r.top;
            pos->flags &= ~(SWP_NOSIZE|SWP_NOMOVE);
        }
    } else if ((msg == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE) || (msg == WM_ACTIVATEAPP && !wp)) {
        PostMessageW(w,popup_check_focus,0,0);
    } else if (msg == popup_check_focus) {
        DWORD pid = 0;
        HWND foreground=GetForegroundWindow();
        GetWindowThreadProcessId(foreground,&pid);
        // A combo, IME, or owned modal dialog can temporarily hold activation.
        if (pid && pid != GetCurrentProcessId() && !w_owned_by(foreground,w) && IsWindowEnabled(w) && !closing)
            PostMessageW(w,WM_CLOSE,0,0);
        return 0;
    } else if (msg == popup_cancel) {
        if (!dismissing) {
            dismissing=w;
            // Common dialogs can disable the root before publishing a prompt on another STA.
            modal_hook=SetWinEventHook(EVENT_OBJECT_SHOW,EVENT_OBJECT_SHOW,NULL,w_modal_shown,
                                       GetCurrentProcessId(),0,WINEVENT_OUTOFCONTEXT);
        }
        if (!IsWindowEnabled(w)) EnumWindows(w_cancel_dialog,(LPARAM)w);
        else if (!closing) {
            HWND cancel = framed ? NULL : GetDlgItem(w,IDCANCEL);
            if (cancel && IsWindowEnabled(cancel))
                PostMessageW(w,WM_COMMAND,MAKEWPARAM(IDCANCEL,BN_CLICKED),(LPARAM)cancel);
            else PostMessageW(w,WM_CLOSE,0,0);
        }
        return 0;
    } else if (msg == WM_ENABLE && dismissing==w) {
        PostMessageW(w,popup_cancel,0,0);
    } else if (msg == WM_CLOSE) {
        if (closing && fade.start) return 0;
        if (!closing) {
            closing = true;
            if (framed && IsWindowVisible(w)) {
                SetWindowLongPtrW(w,GWL_EXSTYLE,GetWindowLongPtrW(w,GWL_EXSTYLE)|WS_EX_TRANSPARENT);
                if (w_layer_fade_to(w,&fade,0,w_theme_layer_timing(false),motion)) return 0;
            }
        }
        LRESULT result = DefSubclassProc(w,msg,wp,lp);
        if (IsWindow(w) && IsWindowVisible(w)) {
            closing = false;
            if (framed) {
                SetWindowLongPtrW(w,GWL_EXSTYLE,GetWindowLongPtrW(w,GWL_EXSTYLE)&~(LONG_PTR)WS_EX_TRANSPARENT);
                w_layer_present(w,&fade);
            }
        } else w_release_popup();
        return result;
    } else if (msg == WM_TIMER && wp == layer_fade_timer) {
        if (!w_layer_fade_tick(w,&fade,motion) && closing) PostMessageW(w,WM_CLOSE,0,0);
        return 0;
    } else if (msg == WM_NCDESTROY) {
        if (modal_hook) UnhookWinEvent(modal_hook);
        modal_hook=NULL;
        dismissing=NULL;
        w_release_popup();
        w_stop_waits();
        if (GetPropW(anchor.bar,w_popup_child_property)==w) RemovePropW(anchor.bar,w_popup_child_property);
        RemovePropW(w,L"winwm.popup_bar");
        RemoveWindowSubclass(w,w_popup_proc,id);
    }
    return DefSubclassProc(w,msg,wp,lp);
}

static bool w_attach(HWND w, bool tool_frame) {
    if (tool_frame && (!IsWindow(anchor.bar) || WaitForSingleObject(cancelled,0) != WAIT_TIMEOUT ||
        WaitForSingleObject(manager,0) != WAIT_TIMEOUT)) return false;
    RECT size;
    if (tool_frame) w_theme_content_rect(w,&size); else GetWindowRect(w,&size);
    framed = tool_frame;
    if (!SetWindowSubclass(w,w_popup_proc,1,0)) return false;
    // Register before reading: a change racing startup is either in the property
    // snapshot or posted to this HWND after the manager publishes it.
    if (!SetPropW(anchor.bar,w_popup_child_property,w) || !w_popup_read(&anchor,placement_key) ||
        !SetPropW(w,L"winwm.popup_bar",anchor.bar) ||
        !RegisterWaitForSingleObject(&cancel_wait,cancelled,w_cancelled,w,INFINITE,WT_EXECUTEONLYONCE) ||
        !RegisterWaitForSingleObject(&manager_wait,manager,w_cancelled,w,INFINITE,WT_EXECUTEONLYONCE))
        return false;
    if (tool_frame)
        SetWindowLongPtrW(w,GWL_STYLE,WS_POPUP|WS_CLIPCHILDREN);
    int width=size.right-size.left + (tool_frame ? 2 : 0);
    int height=size.bottom-size.top + (tool_frame ? w_heading_height(w)+1 : 0);
    preferred=(SIZE){width,height};
    preferred_dpi=GetDpiForWindow(w);
    UINT dpi=GetDpiForWindow(anchor.bar);
    RECT r=w_popup_bounds(&anchor,w_popup_area(anchor.bar),MulDiv(width,dpi,preferred_dpi),
                          MulDiv(height,dpi,preferred_dpi),w_theme_spacing(dpi));
    placing=true;
    SetWindowPos(w,HWND_TOPMOST,r.left,r.top,r.right-r.left,r.bottom-r.top,
                 SWP_NOACTIVATE|SWP_FRAMECHANGED);
    placing=false;
    // Native modal creation is already in progress; its registered waits drive Cancel even
    // if another button was clicked between the launch and the first dialog activation.
    return !tool_frame || WaitForSingleObject(cancelled,0)==WAIT_TIMEOUT;
}

bool w_tool_show(HWND w) {
    w_theme_follow(w);
    if (anchor.bar) {
        if (!w_attach(w,true)) {
            PostMessageW(w,WM_CLOSE,0,0);
            return false;
        }
        w_theme_material(w);
        motion=w_theme_motion_enabled();
        w_layer_present(w,&fade);
    }
    ShowWindow(w,SW_SHOW);
    if (anchor.bar) SetForegroundWindow(w);
    return true;
}

static LRESULT CALLBACK w_native_activate(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_ACTIVATE) {
        HWND w = (HWND)wp;
        wchar_t cls[24];
        if (GetClassNameW(w,cls,count_of(cls)) && !wcscmp(cls,L"#32770")) {
            UnhookWindowsHookEx(native_hook);
            native_hook=NULL;
            if (!w_attach(w,false)) PostMessageW(w,WM_CLOSE,0,0);
        }
    }
    return CallNextHookEx(NULL,code,wp,lp);
}

bool w_tool_native_dialog(void) {
    if (!anchor.bar) return true;
    if (WaitForSingleObject(cancelled,0)!=WAIT_TIMEOUT) return false;
    native_hook=SetWindowsHookExW(WH_CBT,w_native_activate,NULL,GetCurrentThreadId());
    return native_hook!=NULL;
}

void w_tool_popup_cleanup(void) {
    if (native_hook) UnhookWindowsHookEx(native_hook);
    w_stop_waits();
    DeleteObject(heading);
    if (cancelled) CloseHandle(cancelled);
    if (manager) CloseHandle(manager);
}
