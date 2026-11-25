#include "theme/icon_button.h"
#include "theme/native_controls.h"
#include "theme/surface.h"
#include "win_platform/layer.h"
#include <commctrl.h>
#include <uxtheme.h>

typedef struct {
    HWND parent, tip, hover, target;
    bool keyboard, visible;
    w_layer_fade fade;
    UINT_PTR timer;
    ULONGLONG deadline;
    HFONT icons, tip_font;
    UINT tip_dpi;
    int icon_height;
    wchar_t text[512];
} w_button_panel;

static LRESULT CALLBACK w_button_proc(HWND,UINT,WPARAM,LPARAM,UINT_PTR,DWORD_PTR);
static LRESULT CALLBACK w_panel_proc(HWND,UINT,WPARAM,LPARAM,UINT_PTR,DWORD_PTR);

static const wchar_t glyph_property[]=L"winwm.button.glyph", panel_property[]=L"winwm.button.panel";
static const wchar_t labeled_property[]=L"winwm.button.labeled";

static w_button_panel *w_state(HWND parent) {
    return (w_button_panel *)GetPropW(parent,panel_property);
}

static TOOLINFOW w_tool(HWND parent, w_button_panel *s) {
    // The executable supports both classic and v6 common controls.
    return (TOOLINFOW){.cbSize=TTTOOLINFOW_V2_SIZE, .uFlags=TTF_TRACK|TTF_ABSOLUTE|TTF_TRANSPARENT,
                       .hwnd=parent, .uId=1, .lpszText=s->text};
}

static void w_hide(w_button_panel *s) {
    TOOLINFOW tool=w_tool(s->parent,s);
    s->visible=false;
    SendMessageW(s->tip,TTM_TRACKACTIVATE,FALSE,(LPARAM)&tool);
}

static void w_cancel(HWND parent, w_button_panel *s,bool animate) {
    if (s->timer) KillTimer(parent,s->timer);
    s->timer=0;
    s->target=NULL;
    if (s->visible && !w_layer_fade_to(s->tip,&s->fade,0,w_theme_layer_timing(false),animate && w_theme_motion_enabled()))
        w_hide(s);
}

static void w_arm(HWND parent, w_button_panel *s, HWND button, bool keyboard) {
    if (s->target==button && keyboard==s->keyboard) return;
    w_cancel(parent,s,true);
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
        w_cancel(parent,s,false);
        return;
    }
    POINT anchor={r.left+(r.right-r.left)/2,r.bottom};
    if (!s->keyboard) GetCursorPos(&anchor);
    MONITORINFO mi={.cbSize=sizeof(mi)};
    if (!GetMonitorInfoW(MonitorFromPoint(anchor,MONITOR_DEFAULTTONEAREST),&mi) ||
        !GetWindowTextW(button,s->text,count_of(s->text))) { w_cancel(parent,s,false); return; }
    if (GetPropW(button,labeled_property)) {
        // Keep the mnemonic in the native caption, and present its visible label in the tip.
        wchar_t *out=s->text;
        for (const wchar_t *in=s->text; *in; ++in) {
            if (*in==L'&') {
                ++in;
                if (!*in) break;
            }
            *out++=*in;
        }
        *out=0;
    }
    UINT dpi=GetDpiForWindow(button);
    int gap=w_theme_spacing(dpi);
    TOOLINFOW tool=w_tool(parent,s);
    if (s->tip_dpi!=dpi) {
        HFONT font=w_theme_message_font(dpi);
        if (font) {
            SendMessageW(s->tip,WM_SETFONT,(WPARAM)font,FALSE);
            DeleteObject(s->tip_font);
            s->tip_font=font;
            s->tip_dpi=dpi;
        }
    }
    w_native_tooltip_text(s->tip,s->text);
    RECT padding={gap,gap/2,gap,gap/2};
    SendMessageW(s->tip,TTM_SETMARGIN,0,(LPARAM)&padding);
    SendMessageW(s->tip,TTM_SETMAXTIPWIDTH,0,min(mi.rcWork.right-mi.rcWork.left,MulDiv(340,dpi,96)));
    SendMessageW(s->tip,TTM_SETTIPBKCOLOR,w_theme_background(),0);
    SendMessageW(s->tip,TTM_SETTIPTEXTCOLOR,w_theme_foreground(),0);
    SendMessageW(s->tip,TTM_UPDATETIPTEXTW,0,(LPARAM)&tool);
    int x=anchor.x+gap, y=anchor.y+(s->keyboard ? gap : gap*2);
    SendMessageW(s->tip,TTM_TRACKPOSITION,0,MAKELPARAM(x,y));
    if (!s->visible || !s->fade.target) {
        w_layer_fade_to(s->tip,&s->fade,255,w_theme_layer_timing(true),w_theme_motion_enabled());
        if (!s->visible) {
            s->visible=true;
            SendMessageW(s->tip,TTM_TRACKACTIVATE,TRUE,(LPARAM)&tool);
        }
    }
    if (s->target!=button) {
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
}

static void w_pointer(HWND parent, w_button_panel *s, HWND button, HWND receiver) {
    if (s->hover==button && (!button || !s->keyboard)) return;
    s->hover=button;
    if (button) {
        TRACKMOUSEEVENT track={sizeof(track),TME_LEAVE,receiver,0};
        TrackMouseEvent(&track);
        w_arm(parent,s,button,false);
    } else if (!s->keyboard) w_cancel(parent,s,true);
}

static LRESULT CALLBACK w_panel_proc(HWND parent,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR data) {
    w_button_panel *s=(w_button_panel *)data;
    if (msg==WM_MOUSEMOVE) w_pointer(parent,s,w_at_pointer(parent),parent);
    if (msg==WM_MOUSELEAVE && s->hover && !IsWindowEnabled(s->hover)) w_pointer(parent,s,NULL,parent);
    if ((msg==WM_ACTIVATE && LOWORD(wp)==WA_INACTIVE) || (msg==WM_SHOWWINDOW && !wp) ||
        msg==WM_MOVE || msg==WM_SIZE || msg==WM_CANCELMODE || msg==WM_SYSCOLORCHANGE ||
        msg==WM_THEMECHANGED || msg==winwm_theme_updated || (msg==WM_SETTINGCHANGE && wp==SPI_SETHIGHCONTRAST)) {
        s->hover=NULL;
        w_cancel(parent,s,false);
    }
    if (msg==WM_TIMER && s->timer && wp==s->timer) {
        // KillTimer leaves already queued messages from the previous target in the queue.
        if (GetTickCount64()<s->deadline) return 0;
        if (s->visible && s->fade.target) w_cancel(parent,s,true);
        else {
            w_present(parent,s);
            if (s->target && s->visible && s->fade.target) {
                s->deadline=GetTickCount64()+5000;
                if (!SetTimer(parent,s->timer,5000,NULL)) w_cancel(parent,s,false);
            }
        }
        return 0;
    }
    if (msg==WM_NCDESTROY) {
        w_cancel(parent,s,false);
        RemoveWindowSubclass(parent,w_panel_proc,id);
        RemovePropW(parent,panel_property);
        if (IsWindow(s->tip)) DestroyWindow(s->tip);
        if (s->icons) DeleteObject(s->icons);
        if (s->tip_font) DeleteObject(s->tip_font);
        free(s);
    }
    return DefSubclassProc(parent,msg,wp,lp);
}

static LRESULT CALLBACK w_button_proc(HWND button,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR data) {
    (void)data;
    HWND parent=GetParent(button);
    w_button_panel *s=w_state(parent);
    if (s) {
        if (msg==WM_ERASEBKGND) return TRUE;
        if (msg==WM_MOUSEMOVE) w_pointer(parent,s,button,button);
        if (msg==WM_MOUSELEAVE && s->hover==button) w_pointer(parent,s,NULL,button);
        if (msg==WM_SETFOCUS && GetKeyState(VK_LBUTTON)>=0) w_arm(parent,s,button,true);
        if (s->target==button && (msg==WM_LBUTTONDOWN || msg==WM_KEYDOWN || msg==WM_CANCELMODE ||
            msg==WM_NCDESTROY || (msg==WM_SHOWWINDOW && !wp) || (msg==WM_KILLFOCUS && s->keyboard)))
            w_cancel(parent,s,msg==WM_KILLFOCUS);
        // IsDialogMessage treats owner-drawn buttons as generic buttons. Claim
        // Enter on the focused action so it cannot activate another default.
        if (msg==WM_GETDLGCODE && wp==VK_RETURN && lp)
            return DefSubclassProc(button,msg,wp,lp) | DLGC_WANTMESSAGE;
        if (msg==WM_KEYDOWN && wp==VK_RETURN) {
            if (!(lp & (1L<<30))) SendMessageW(button,BM_CLICK,0,0);
            return 0;
        }
        if (msg==WM_CHAR && wp==L'\r') return 0;
        if (msg==WM_NCDESTROY && s->hover==button) s->hover=NULL;
        if (msg==WM_SETTEXT) {
            LRESULT result=DefSubclassProc(button,msg,wp,lp);
            if (s->visible && s->fade.target && s->target==button) w_present(parent,s);
            return result;
        }
    }
    if (msg==WM_NCDESTROY) {
        RemovePropW(button,glyph_property);
        RemovePropW(button,labeled_property);
        RemoveWindowSubclass(button,w_button_proc,id);
    }
    return DefSubclassProc(button,msg,wp,lp);
}

// The tooltip is a top-level window even when its buttons live inside a nested control.
static LRESULT CALLBACK w_tip_proc(HWND tip,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR data) {
    w_button_panel *s=(w_button_panel *)data;
    if (msg==WM_TIMER && wp==layer_fade_timer) {
        if (s->fade.start && !w_layer_fade_tick(tip,&s->fade,true) && !s->fade.target && s->visible) w_hide(s);
        return 0;
    }
    if (s->fade.start && (msg==winwm_motion_updated || msg==WM_SYSCOLORCHANGE ||
        (msg==WM_SETTINGCHANGE && (wp==SPI_SETCLIENTAREAANIMATION || wp==SPI_SETHIGHCONTRAST)))) {
        if (!w_layer_fade_to(tip,&s->fade,s->fade.target,w_theme_layer_timing(s->fade.target!=0),
                             w_theme_motion_enabled()) && !s->fade.target && s->visible) w_hide(s);
    }
    if (msg==WM_SETTINGCHANGE && wp==SPI_SETNONCLIENTMETRICS) {
        s->tip_dpi=0;
        if (s->visible && s->fade.target && s->target) w_present(s->parent,s);
    }
    if (msg==WM_NCDESTROY) RemoveWindowSubclass(tip,w_tip_proc,id);
    return DefSubclassProc(tip,msg,wp,lp);
}

static w_button_panel *w_panel(HWND parent) {
    w_button_panel *s=w_state(parent);
    if (s) return s;
    INITCOMMONCONTROLSEX controls={sizeof(controls),ICC_WIN95_CLASSES};
    if (!InitCommonControlsEx(&controls)) return NULL;
    s=calloc(1,sizeof(*s));
    if (!s) return NULL;
    s->parent=parent;
    s->tip=CreateWindowExW(WS_EX_TOPMOST|WS_EX_NOACTIVATE|WS_EX_TRANSPARENT,TOOLTIPS_CLASSW,NULL,
                           WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX|TTS_NOANIMATE|TTS_NOFADE,CW_USEDEFAULT,CW_USEDEFAULT,
                           CW_USEDEFAULT,CW_USEDEFAULT,parent,NULL,GetModuleHandleW(NULL),NULL);
    TOOLINFOW tool=w_tool(parent,s);
    if (!s->tip || !SendMessageW(s->tip,TTM_ADDTOOLW,0,(LPARAM)&tool) ||
        !SetWindowSubclass(s->tip,w_tip_proc,1,(DWORD_PTR)s) ||
        !SetPropW(parent,panel_property,(HANDLE)s) || !SetWindowSubclass(parent,w_panel_proc,1,(DWORD_PTR)s)) {
        RemovePropW(parent,panel_property);
        if (s->tip) DestroyWindow(s->tip);
        free(s);
        return NULL;
    }
    SetWindowTheme(s->tip,L"",L"");
    w_theme_material(s->tip);
    return s;
}

static bool w_set_button(HWND button, const wchar_t *glyph, const wchar_t *label, bool labeled) {
    if (!button || !glyph || !glyph[0] || (glyph[1] && glyph[2]) || !label || !*label) return false;
    wchar_t previous[512];
    GetWindowTextW(button,previous,count_of(previous));
    if (wcscmp(previous,label)) SetWindowTextW(button,label);
    HANDLE code=GetPropW(button,glyph_property), next=(HANDLE)(UINT_PTR)(DWORD)MAKELONG(glyph[0],glyph[1]);
    bool was_labeled=GetPropW(button,labeled_property)!=NULL;
    if (code==next && was_labeled==labeled) return true;
    if (!w_panel(GetParent(button)) || !SetPropW(button,glyph_property,next)) return false;
    if (!code && !SetWindowSubclass(button,w_button_proc,1,0)) {
        RemovePropW(button,glyph_property);
        return false;
    }
    if (labeled) SetPropW(button,labeled_property,(HANDLE)1);
    else RemovePropW(button,labeled_property);
    LONG_PTR style=GetWindowLongPtrW(button,GWL_STYLE);
    if ((style & BS_TYPEMASK)!=BS_OWNERDRAW) SendMessageW(button,BM_SETSTYLE,BS_OWNERDRAW,FALSE);
    InvalidateRect(button,NULL,FALSE);
    return true;
}

bool w_icon_button(HWND button,const wchar_t *glyph,const wchar_t *label) {
    return w_set_button(button,glyph,label,false);
}

bool w_labeled_button(HWND button,const wchar_t *glyph,const wchar_t *label) {
    return w_set_button(button,glyph,label,true);
}

int w_labeled_button_width(HFONT font,const wchar_t *label,UINT dpi) {
    int side=w_theme_button_size(dpi);
    RECT text={0,0,0,side};
    HDC dc=GetDC(NULL);
    if (dc) {
        HGDIOBJ prior=SelectObject(dc,font ? font : GetStockObject(DEFAULT_GUI_FONT));
        w_theme_draw_fitted_line(dc,label,-1,&text,DT_CALCRECT);
        SelectObject(dc,prior);
        ReleaseDC(NULL,dc);
    }
    return max(side,text.right+w_theme_icon_size(dpi)+3*w_theme_spacing(dpi));
}

bool w_icon_button_compact(HWND button) {
    return GetPropW(button,glyph_property)!=NULL && !GetPropW(button,labeled_property);
}

bool w_icon_button_draw(const DRAWITEMSTRUCT *item) {
    DWORD_PTR data=(DWORD_PTR)GetPropW(item->hwndItem,glyph_property);
    if (!data) return false;
    w_button_panel *s=w_state(GetParent(item->hwndItem));
    if (!s) return false;
    UINT dpi=GetDpiForWindow(item->hwndItem);
    int height=max(1,min(item->rcItem.bottom-item->rcItem.top,w_theme_icon_size(dpi)));
    if (!s->icons || height!=s->icon_height) {
        HFONT next=w_theme_font(L"Segoe MDL2 Assets",height,FW_NORMAL);
        if (!next) return false;
        if (s->icons) DeleteObject(s->icons);
        s->icons=next; s->icon_height=height;
    }
    HGDIOBJ prior=SelectObject(item->hDC,s->icons);
    wchar_t glyphs[]={LOWORD(data),HIWORD(data)};
    int count=glyphs[1] ? 2 : 1;
    WORD indices[2];
    bool available=GetGlyphIndicesW(item->hDC,glyphs,count,indices,GGI_MARK_NONEXISTING_GLYPHS)!=GDI_ERROR;
    for (int i=0; available && i<count; ++i) available=indices[i]!=0xffff && indices[i]!=0;
    RECT rect=item->rcItem;
    bool labeled=GetPropW(item->hwndItem,labeled_property)!=NULL;
    int gap=w_theme_spacing(dpi);
    if (labeled) {
        rect.left+=gap;
        rect.right=min(rect.right,rect.left+w_theme_icon_size(dpi));
    }
    if (available) {
        HFONT fitted=NULL;
        SIZE run;
        int width=max(1,rect.right-rect.left);
        if (count>1 && GetTextExtentPoint32W(item->hDC,glyphs,count,&run) && run.cx>width)
            fitted=w_theme_select_fitting_font(item->hDC,max(1,(int)((int64_t)run.cy*width/run.cx)));
        DrawTextW(item->hDC,glyphs,count,&rect,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
        if (fitted) SelectObject(item->hDC,fitted);
    }
    SelectObject(item->hDC,prior);
    if (available && labeled) {
        wchar_t caption[256];
        GetWindowTextW(item->hwndItem,caption,count_of(caption));
        rect.left=rect.right+gap; rect.right=item->rcItem.right-gap;
        w_theme_draw_fitted_line(item->hDC,caption,-1,&rect,DT_LEFT|DT_VCENTER|DT_END_ELLIPSIS|
                  ((item->itemState&ODS_NOACCEL) ? DT_HIDEPREFIX : 0));
    }
    return available;
}
