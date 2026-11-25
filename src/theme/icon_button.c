#include "theme/icon_button.h"
#include "theme/theme.h"
#include <commctrl.h>
#include <uxtheme.h>

typedef struct {
    HWND tip, hover, target;
    bool keyboard, visible;
    UINT_PTR timer;
    ULONGLONG deadline;
    HFONT icons;
    UINT dpi;
    wchar_t text[512];
} w_button_panel;

static LRESULT CALLBACK w_button_proc(HWND,UINT,WPARAM,LPARAM,UINT_PTR,DWORD_PTR);
static LRESULT CALLBACK w_panel_proc(HWND,UINT,WPARAM,LPARAM,UINT_PTR,DWORD_PTR);

static const wchar_t glyph_property[]=L"winwm.button.glyph", panel_property[]=L"winwm.button.panel";

static w_button_panel *w_state(HWND parent) {
    return (w_button_panel *)GetPropW(parent,panel_property);
}

static TOOLINFOW w_tool(HWND parent, w_button_panel *s) {
    // The executable supports both classic and v6 common controls.
    return (TOOLINFOW){.cbSize=TTTOOLINFOW_V2_SIZE, .uFlags=TTF_TRACK|TTF_ABSOLUTE|TTF_TRANSPARENT,
                       .hwnd=parent, .uId=1, .lpszText=s->text};
}

static void w_cancel(HWND parent, w_button_panel *s) {
    if (s->timer) KillTimer(parent,s->timer);
    s->timer=0;
    s->target=NULL;
    if (s->visible) {
        TOOLINFOW tool=w_tool(parent,s);
        SendMessageW(s->tip,TTM_TRACKACTIVATE,FALSE,(LPARAM)&tool);
        s->visible=false;
    }
}

static void w_hover(w_button_panel *s, HWND next) {
    if (s->hover==next) return;
    if (s->hover) InvalidateRect(s->hover,NULL,FALSE);
    s->hover=next;
    if (next) InvalidateRect(next,NULL,FALSE);
}

static void w_arm(HWND parent, w_button_panel *s, HWND button, bool keyboard) {
    if (s->target==button && keyboard==s->keyboard) return;
    w_cancel(parent,s);
    s->keyboard=keyboard;
    s->target=button;
    UINT delay=GetDoubleClickTime();
    s->deadline=GetTickCount64()+delay;
    s->timer=SetTimer(parent,(UINT_PTR)s,delay,NULL);
    if (!s->timer) s->target=NULL;
}

static HWND w_at_pointer(HWND parent) {
    POINT p;
    if (!GetCursorPos(&p)) return NULL;
    HWND hit=WindowFromPoint(p);
    // Disabled buttons send mouse messages to their parent.
    if (hit==parent) {
        ScreenToClient(parent,&p);
        hit=ChildWindowFromPointEx(parent,p,CWP_SKIPINVISIBLE|CWP_SKIPTRANSPARENT);
    }
    return hit && GetParent(hit)==parent && GetPropW(hit,glyph_property) ? hit : NULL;
}

static void w_present(HWND parent, w_button_panel *s) {
    HWND button=s->target;
    RECT r;
    if (!button || !IsWindowVisible(button) || !GetWindowRect(button,&r) ||
        (s->keyboard ? GetFocus()!=button : w_at_pointer(parent)!=button)) {
        w_cancel(parent,s);
        return;
    }
    POINT anchor={r.left+(r.right-r.left)/2,r.bottom};
    if (!s->keyboard) GetCursorPos(&anchor);
    MONITORINFO mi={.cbSize=sizeof(mi)};
    if (!GetMonitorInfoW(MonitorFromPoint(anchor,MONITOR_DEFAULTTONEAREST),&mi) ||
        !GetWindowTextW(button,s->text,count_of(s->text))) { w_cancel(parent,s); return; }
    UINT dpi=GetDpiForWindow(button);
    int gap=MulDiv(8,dpi,96);
    TOOLINFOW tool=w_tool(parent,s);
    RECT padding={gap,gap/2,gap,gap/2};
    SendMessageW(s->tip,TTM_SETMARGIN,0,(LPARAM)&padding);
    SendMessageW(s->tip,TTM_SETMAXTIPWIDTH,0,min(mi.rcWork.right-mi.rcWork.left,MulDiv(340,dpi,96)));
    SendMessageW(s->tip,TTM_SETTIPBKCOLOR,w_theme_background(),0);
    SendMessageW(s->tip,TTM_SETTIPTEXTCOLOR,w_theme_foreground(),0);
    SendMessageW(s->tip,TTM_UPDATETIPTEXTW,0,(LPARAM)&tool);
    int x=anchor.x+gap, y=anchor.y+(s->keyboard ? gap : gap*2);
    SendMessageW(s->tip,TTM_TRACKPOSITION,0,MAKELPARAM(x,y));
    if (!s->visible) SendMessageW(s->tip,TTM_TRACKACTIVATE,TRUE,(LPARAM)&tool);
    if (s->target!=button) {
        SendMessageW(s->tip,TTM_TRACKACTIVATE,FALSE,(LPARAM)&tool);
        return;
    }
    RECT bounds;
    if (GetWindowRect(s->tip,&bounds)) {
        int width=bounds.right-bounds.left, height=bounds.bottom-bounds.top;
        if (y+height>mi.rcWork.bottom) y=(s->keyboard ? r.top : anchor.y)-height-gap;
        x=max(mi.rcWork.left,min(x,mi.rcWork.right-width));
        y=max(mi.rcWork.top,min(y,mi.rcWork.bottom-height));
        SetWindowPos(s->tip,HWND_TOPMOST,x,y,0,0,SWP_NOACTIVATE|SWP_NOSIZE);
    }
    s->visible=true;
}

static void w_pointer(HWND parent, w_button_panel *s, HWND button, HWND receiver) {
    if (s->hover==button && (!button || !s->keyboard)) return;
    w_hover(s,button);
    if (button) {
        TRACKMOUSEEVENT track={sizeof(track),TME_LEAVE,receiver,0};
        TrackMouseEvent(&track);
        w_arm(parent,s,button,false);
    } else if (!s->keyboard) w_cancel(parent,s);
}

static LRESULT CALLBACK w_panel_proc(HWND parent,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR data) {
    w_button_panel *s=(w_button_panel *)data;
    if (msg==WM_MOUSEMOVE) w_pointer(parent,s,w_at_pointer(parent),parent);
    if (msg==WM_MOUSELEAVE && s->hover && !IsWindowEnabled(s->hover)) w_pointer(parent,s,NULL,parent);
    if ((msg==WM_ACTIVATE && LOWORD(wp)==WA_INACTIVE) || (msg==WM_SHOWWINDOW && !wp) ||
        msg==WM_MOVE || msg==WM_SIZE || msg==WM_CANCELMODE || msg==WM_SYSCOLORCHANGE ||
        msg==WM_THEMECHANGED || (msg==WM_SETTINGCHANGE && wp==SPI_SETHIGHCONTRAST)) {
        w_hover(s,NULL);
        w_cancel(parent,s);
    }
    if (msg==WM_TIMER && s->timer && wp==s->timer) {
        // KillTimer leaves already queued messages from the previous target in the queue.
        if (GetTickCount64()<s->deadline) return 0;
        if (s->visible) w_cancel(parent,s);
        else {
            w_present(parent,s);
            if (s->visible) {
                s->deadline=GetTickCount64()+5000;
                if (!SetTimer(parent,s->timer,5000,NULL)) w_cancel(parent,s);
            }
        }
        return 0;
    }
    if (msg==WM_NCDESTROY) {
        w_cancel(parent,s);
        RemoveWindowSubclass(parent,w_panel_proc,id);
        RemovePropW(parent,panel_property);
        if (IsWindow(s->tip)) DestroyWindow(s->tip);
        if (s->icons) DeleteObject(s->icons);
        free(s);
    }
    return DefSubclassProc(parent,msg,wp,lp);
}

static LRESULT CALLBACK w_button_proc(HWND button,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR data) {
    (void)data;
    HWND parent=GetParent(button);
    w_button_panel *s=w_state(parent);
    if (s) {
        if (msg==WM_MOUSEMOVE) w_pointer(parent,s,button,button);
        if (msg==WM_MOUSELEAVE && s->hover==button) w_pointer(parent,s,NULL,button);
        if (msg==WM_SETFOCUS && GetKeyState(VK_LBUTTON)>=0) w_arm(parent,s,button,true);
        if (s->target==button && (msg==WM_LBUTTONDOWN || msg==WM_KEYDOWN || msg==WM_CANCELMODE ||
            msg==WM_NCDESTROY || (msg==WM_SHOWWINDOW && !wp) || (msg==WM_KILLFOCUS && s->keyboard)))
            w_cancel(parent,s);
        if (msg==WM_NCDESTROY && s->hover==button) s->hover=NULL;
        if (msg==WM_SETTEXT) {
            LRESULT result=DefSubclassProc(button,msg,wp,lp);
            if (s->visible && s->target==button) w_present(parent,s);
            return result;
        }
    }
    if (msg==WM_NCDESTROY) {
        RemovePropW(button,glyph_property);
        RemoveWindowSubclass(button,w_button_proc,id);
    }
    return DefSubclassProc(button,msg,wp,lp);
}

static w_button_panel *w_panel(HWND parent) {
    w_button_panel *s=w_state(parent);
    if (s) return s;
    INITCOMMONCONTROLSEX controls={sizeof(controls),ICC_WIN95_CLASSES};
    if (!InitCommonControlsEx(&controls)) return NULL;
    s=calloc(1,sizeof(*s));
    if (!s) return NULL;
    s->tip=CreateWindowExW(WS_EX_TOPMOST|WS_EX_NOACTIVATE|WS_EX_TRANSPARENT,TOOLTIPS_CLASSW,NULL,
                           WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,CW_USEDEFAULT,CW_USEDEFAULT,
                           CW_USEDEFAULT,CW_USEDEFAULT,parent,NULL,GetModuleHandleW(NULL),NULL);
    TOOLINFOW tool=w_tool(parent,s);
    if (!s->tip || !SendMessageW(s->tip,TTM_ADDTOOLW,0,(LPARAM)&tool) ||
        !SetPropW(parent,panel_property,(HANDLE)s) || !SetWindowSubclass(parent,w_panel_proc,1,(DWORD_PTR)s)) {
        RemovePropW(parent,panel_property);
        if (s->tip) DestroyWindow(s->tip);
        free(s);
        return NULL;
    }
    SetWindowTheme(s->tip,L"",L"");
    return s;
}

bool w_icon_button(HWND button, const wchar_t *glyph, const wchar_t *label) {
    if (!button || !glyph || !glyph[0] || (glyph[1] && glyph[2]) || !label || !*label) return false;
    wchar_t previous[512];
    GetWindowTextW(button,previous,count_of(previous));
    if (wcscmp(previous,label)) SetWindowTextW(button,label);
    HANDLE code=GetPropW(button,glyph_property), next=(HANDLE)(UINT_PTR)(DWORD)MAKELONG(glyph[0],glyph[1]);
    if (code==next) return true;
    if (!w_panel(GetParent(button)) || !SetPropW(button,glyph_property,next)) return false;
    if (!code && !SetWindowSubclass(button,w_button_proc,1,0)) {
        RemovePropW(button,glyph_property);
        return false;
    }
    LONG_PTR style=GetWindowLongPtrW(button,GWL_STYLE);
    if ((style & BS_TYPEMASK)!=BS_OWNERDRAW) SendMessageW(button,BM_SETSTYLE,BS_OWNERDRAW,FALSE);
    InvalidateRect(button,NULL,FALSE);
    return true;
}

bool w_icon_button_hot(HWND button) {
    w_button_panel *s=w_state(GetParent(button));
    return s && s->hover==button;
}

bool w_icon_button_draw(const DRAWITEMSTRUCT *item) {
    DWORD_PTR data=(DWORD_PTR)GetPropW(item->hwndItem,glyph_property);
    if (!data) return false;
    w_button_panel *s=w_state(GetParent(item->hwndItem));
    if (!s) return false;
    UINT dpi=GetDpiForWindow(item->hwndItem);
    if (!s->icons || dpi!=s->dpi) {
        HFONT next=CreateFontW(-MulDiv(17,dpi,96),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
                               0,0,CLEARTYPE_QUALITY,0,L"Segoe MDL2 Assets");
        if (!next) return false;
        if (s->icons) DeleteObject(s->icons);
        s->icons=next; s->dpi=dpi;
    }
    HGDIOBJ prior=SelectObject(item->hDC,s->icons);
    wchar_t glyphs[]={LOWORD(data),HIWORD(data)};
    int count=glyphs[1] ? 2 : 1;
    WORD indices[2];
    bool available=GetGlyphIndicesW(item->hDC,glyphs,count,indices,GGI_MARK_NONEXISTING_GLYPHS)!=GDI_ERROR;
    for (int i=0; available && i<count; ++i) available=indices[i]!=0xffff && indices[i]!=0;
    RECT rect=item->rcItem;
    if (available) DrawTextW(item->hDC,glyphs,count,&rect,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
    SelectObject(item->hDC,prior);
    return available;
}
