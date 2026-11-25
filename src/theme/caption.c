#include "theme/caption.h"
#include "theme/icon_button.h"
#include "theme/hover.h"
#include "theme/surface.h"
#include <commctrl.h>
#include <windowsx.h>

enum { close_action, maximize_action, minimize_action, caption_actions };
static const wchar_t caption_class[]=L"WinwmCaption", caption_property[]=L"winwm.caption";

typedef struct {
    HWND header, actions[caption_actions];
    HFONT font;
    UINT dpi;
    int height;
    RECT title;
    bool maximize_hover;
} w_caption;

static bool w_titled(DWORD style) {
    return !(style&WS_CHILD) && (style&(WS_CAPTION|WS_SYSMENU))==(WS_CAPTION|WS_SYSMENU);
}

static int w_height(UINT dpi) {
    int text=w_theme_role_line_height(dpi,text_heading);
    return max(w_theme_button_size(dpi),text)+w_theme_spacing(dpi);
}

int w_theme_caption_height(HWND w) {
    const w_caption *c=(w_caption *)GetPropW(w,caption_property);
    return c ? c->height : 0;
}

int w_theme_caption_width(HWND w) {
    const w_caption *c=(w_caption *)GetPropW(w,caption_property);
    if (!c) return 0;
    wchar_t title[256];
    GetWindowTextW(w,title,count_of(title));
    UINT dpi=GetDpiForWindow(w);
    int gap=w_theme_spacing(dpi),width=3*gap+w_theme_text_width(c->font,title,DT_NOPREFIX);
    for (int i=0;i<caption_actions;++i)
        if (c->actions[i]) width+=w_theme_button_size(dpi)+gap;
    return width;
}

void w_theme_content_rect(HWND w,RECT *r) {
    GetClientRect(w,r);
    r->bottom-=w_theme_caption_height(w);
}

void w_theme_adjust_rect(RECT *r,DWORD style,DWORD exstyle,UINT dpi) {
    RECT frame={0};
    AdjustWindowRectExForDpi(&frame,style,FALSE,exstyle,dpi);
    r->left+=frame.left; r->right+=frame.right; r->bottom+=frame.bottom;
    r->top+=w_titled(style) ? -frame.bottom-w_height(dpi) : frame.top;
}

static void w_layout(HWND w,w_caption *c) {
    RECT r;
    GetClientRect(w,&r);
    UINT dpi=GetDpiForWindow(w);
    int height=min(r.bottom,w_height(dpi));
    c->height=height;
    SetWindowPos(c->header,NULL,0,0,r.right,height,SWP_NOACTIVATE|SWP_NOZORDER);
    int side=min(min(height,r.right),w_theme_button_size(dpi)), gap=w_theme_spacing(dpi);
    int margin=min(gap,max(0,(r.right-side)/2)), right=r.right-margin;
    int y=(height-side)/2;
    DWORD style=GetWindowLongW(w,GWL_STYLE);
    for (int i=0; i<caption_actions; ++i) {
        HWND a=c->actions[i];
        if (!a) continue;
        bool visible=right>=side && (i==close_action ||
                     (style&(i==maximize_action ? WS_MAXIMIZEBOX : WS_MINIMIZEBOX)));
        if (!visible) {
            if (GetWindowLongW(a,GWL_STYLE)&WS_VISIBLE) ShowWindow(a,SW_HIDE);
            continue;
        }
        SetWindowPos(a,NULL,right-side,y,side,side,SWP_NOACTIVATE|SWP_NOZORDER|SWP_SHOWWINDOW);
        right-=side+gap;
    }
    right=max(0,right);
    c->title=(RECT){min(2*gap,right),0,right,height};
    if (dpi!=c->dpi) {
        HFONT next=w_theme_role_font(dpi,text_heading);
        if (next) { DeleteObject(c->font); c->font=next; c->dpi=dpi; }
    }
    if (c->actions[maximize_action]) {
        bool zoomed=IsZoomed(w);
        w_icon_button(c->actions[maximize_action],zoomed ? L"\uE923" : L"\uE922",zoomed ? L"Restore" : L"Maximize");
    }
    InvalidateRect(c->header,NULL,FALSE);
}

static LRESULT CALLBACK w_caption_button_proc(HWND button,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR data) {
    // Windows recognizes Snap Layouts on the top-level window's HTMAXBUTTON.
    if (msg==WM_NCHITTEST && data==maximize_action) return HTTRANSPARENT;
    // Native BUTTON focuses itself on mouse-down. A caption action must keep
    // the current editor focused, including its selection and IME composition.
    if (msg==WM_LBUTTONDOWN || msg==WM_LBUTTONDBLCLK) {
        SetCapture(button);
        if (GetCapture()==button) SendMessageW(button,BM_SETSTATE,TRUE,0);
        return 0;
    }
    if ((msg==WM_MOUSEMOVE || msg==WM_LBUTTONUP) && GetCapture()==button) {
        RECT bounds;
        GetClientRect(button,&bounds);
        bool inside=PtInRect(&bounds,(POINT){GET_X_LPARAM(lp),GET_Y_LPARAM(lp)});
        if (msg==WM_MOUSEMOVE) SendMessageW(button,BM_SETSTATE,inside,0);
        else {
            ReleaseCapture();
            if (inside) SendMessageW(GetParent(button),WM_COMMAND,MAKEWPARAM(GetDlgCtrlID(button),BN_CLICKED),(LPARAM)button);
        }
        return 0;
    }
    if (msg==WM_CAPTURECHANGED) SendMessageW(button,BM_SETSTATE,FALSE,0);
    if (msg==WM_CANCELMODE && GetCapture()==button) ReleaseCapture();
    if (msg==WM_NCDESTROY) RemoveWindowSubclass(button,w_caption_button_proc,id);
    return DefSubclassProc(button,msg,wp,lp);
}

static LRESULT CALLBACK w_header_proc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    HWND w=GetParent(h);
    w_caption *c=(w_caption *)GetWindowLongPtrW(h,GWLP_USERDATA);
    switch (msg) {
    case WM_NCCREATE:
        SetWindowLongPtrW(h,GWLP_USERDATA,(LONG_PTR)((CREATESTRUCTW *)lp)->lpCreateParams);
        break;
    case WM_ERASEBKGND: return TRUE;
    case WM_PAINT: case WM_PRINTCLIENT: {
        PAINTSTRUCT ps;
        HDC dc=msg==WM_PAINT ? BeginPaint(h,&ps) : (HDC)wp;
        RECT r;
        GetClientRect(h,&r);
        FillRect(dc,&r,w_theme_brush());
        r.top=max(0,r.bottom-1);
        int saved=SaveDC(dc);
        SetDCBrushColor(dc,w_theme_separator());
        FillRect(dc,&r,GetStockObject(DC_BRUSH));
        if (c) {
            wchar_t text[256];
            GetWindowTextW(w,text,count_of(text));
            SelectObject(dc,c->font ? c->font : GetStockObject(DEFAULT_GUI_FONT));
            SetBkMode(dc,TRANSPARENT);
            SetTextColor(dc,w_theme_foreground());
            w_theme_draw_fitted_line(dc,text,-1,&c->title,DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
        }
        RestoreDC(dc,saved);
        if (msg==WM_PAINT) EndPaint(h,&ps);
        return 0;
    }
    case WM_NCHITTEST: {
        POINT pt={GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
        ScreenToClient(h,&pt);
        HWND hit=ChildWindowFromPointEx(h,pt,CWP_SKIPINVISIBLE);
        return hit==h || (c && hit==c->actions[maximize_action]) ? HTTRANSPARENT : HTCLIENT;
    }
    case WM_DRAWITEM: return SendMessageW(w,msg,wp,lp);
    case WM_COMMAND:
        if (c && HIWORD(wp)==BN_CLICKED) {
            HWND a=(HWND)lp;
            UINT command=a==c->actions[close_action] ? SC_CLOSE :
                         a==c->actions[minimize_action] ? SC_MINIMIZE :
                         a==c->actions[maximize_action] ? (IsZoomed(w) ? SC_RESTORE : SC_MAXIMIZE) : 0;
            if (command && IsWindowEnabled(w) && IsWindowEnabled(a)) PostMessageW(w,WM_SYSCOMMAND,command,0);
        }
        return 0;
    }
    return DefWindowProcW(h,msg,wp,lp);
}

static LRESULT CALLBACK w_caption_proc(HWND w,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR data) {
    w_caption *c=(w_caption *)data;
    if (msg==WM_NCDESTROY || (msg==WM_STYLECHANGED && (int)wp==GWL_STYLE &&
                              !w_titled(((STYLESTRUCT *)lp)->styleNew))) {
        RemoveWindowSubclass(w,w_caption_proc,id);
        RemovePropW(w,caption_property);
        if (IsWindow(c->header)) DestroyWindow(c->header);
        DeleteObject(c->font);
        free(c);
        return DefSubclassProc(w,msg,wp,lp);
    }
    switch (msg) {
    case WM_NCCALCSIZE: {
        RECT *client=wp ? &((NCCALCSIZE_PARAMS *)lp)->rgrc[0] : (RECT *)lp;
        RECT outer=*client;
        DefSubclassProc(w,msg,wp,lp);
        // Preserve the native resize frame on every edge. Reclaim just the
        // caption row for the accessible child controls.
        client->top=min(client->bottom,outer.top+outer.bottom-client->bottom);
        return 0;
    }
    case WM_NCHITTEST: {
        POINT pt={GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
        RECT client;
        GetClientRect(w,&client);
        ScreenToClient(w,&pt);
        if (!PtInRect(&client,pt)) break;
        if (pt.y<c->height) {
            HWND hit=ChildWindowFromPointEx(c->header,pt,CWP_SKIPINVISIBLE);
            if (hit==c->header) return HTCAPTION;
            if (hit && hit==c->actions[maximize_action] && IsWindowEnabled(hit)) return HTMAXBUTTON;
        }
        return HTCLIENT;
    }
    case WM_NCMOUSEMOVE: case WM_NCMOUSELEAVE: {
        bool hot=msg==WM_NCMOUSEMOVE && wp==HTMAXBUTTON;
        if (hot!=c->maximize_hover) {
            HWND button=c->actions[maximize_action];
            RECT bounds;
            if (hot) {
                TRACKMOUSEEVENT leave={sizeof(leave),TME_NONCLIENT|TME_LEAVE,w,0};
                hot=TrackMouseEvent(&leave) && GetClientRect(button,&bounds);
            }
            c->maximize_hover=hot;
            w_hover_target(button,hot ? 0 : -1,hot ? &bounds : NULL);
        }
        // Default nonclient processing handles the system's Snap Layouts hover.
        break;
    }
    case WM_NCLBUTTONDOWN: case WM_NCLBUTTONDBLCLK:
        if (wp==HTMAXBUTTON && IsWindowEnabled(c->actions[maximize_action])) {
            HWND button=c->actions[maximize_action];
            POINT pt={GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
            ScreenToClient(button,&pt);
            return SendMessageW(button,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(pt.x,pt.y));
        }
        break;
    case WM_GETTITLEBARINFOEX: {
        TITLEBARINFOEX *info=(TITLEBARINFOEX *)lp;
        if (!info || info->cbSize!=sizeof(*info)) break;
        *info=(TITLEBARINFOEX){.cbSize=sizeof(*info)};
        GetWindowRect(c->header,&info->rcTitleBar);
        if (!IsWindowVisible(c->header)) info->rgstate[0]|=STATE_SYSTEM_INVISIBLE;
        if (!IsWindowEnabled(w)) info->rgstate[0]|=STATE_SYSTEM_UNAVAILABLE;
        info->rgstate[1]=info->rgstate[4]=STATE_SYSTEM_INVISIBLE;
        const int ids[]={5,3,2};
        for (int i=0; i<caption_actions; ++i) {
            HWND a=c->actions[i];
            if (!a || !IsWindowVisible(a)) info->rgstate[ids[i]]=STATE_SYSTEM_INVISIBLE;
            else {
                GetWindowRect(a,&info->rgrect[ids[i]]);
                if (!IsWindowEnabled(w) || !IsWindowEnabled(a)) info->rgstate[ids[i]]|=STATE_SYSTEM_UNAVAILABLE;
                if (SendMessageW(a,BM_GETSTATE,0,0)&BST_PUSHED) info->rgstate[ids[i]]|=STATE_SYSTEM_PRESSED;
                if (GetFocus()==a) info->rgstate[ids[i]]|=STATE_SYSTEM_FOCUSED;
            }
        }
        return 0;
    }
    case WM_SETTINGCHANGE:
        if (wp==SPI_SETNONCLIENTMETRICS) { c->dpi=0; w_layout(w,c); }
        break;
    case WM_FONTCHANGE:
        c->dpi=0;
        if (!IsIconic(w)) w_layout(w,c);
        break;
    case WM_SIZE:
    case WM_DPICHANGED:
    case winwm_theme_updated:
        if (!IsIconic(w)) w_layout(w,c);
        break;
    case WM_SETTEXT:
        InvalidateRect(c->header,NULL,FALSE);
        break;
    }
    return DefSubclassProc(w,msg,wp,lp);
}

void w_theme_caption_install(HWND w) {
    DWORD style=GetWindowLongW(w,GWL_STYLE);
    if (!w_titled(style) || GetPropW(w,caption_property)) return;
    HINSTANCE module=GetModuleHandleW(NULL);
    WNDCLASSW wc={.hInstance=module,.lpszClassName=caption_class,.lpfnWndProc=w_header_proc,
                  .hCursor=LoadCursorW(NULL,IDC_ARROW)};
    if (!RegisterClassW(&wc) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) return;
    w_caption *c=calloc(1,sizeof(*c));
    if (!c) return;
    c->header=CreateWindowW(caption_class,L"Title bar",WS_CHILD|WS_VISIBLE|WS_CLIPCHILDREN,
                           0,0,0,0,w,NULL,module,c);
    if (!c->header) { free(c); return; }
    const wchar_t *labels[]={L"Close",L"Maximize",L"Minimize"};
    const wchar_t *glyphs[]={L"\uE711",L"\uE922",L"\uE921"};
    for (int i=0; i<caption_actions; ++i) {
        if ((i==maximize_action && !(style&WS_MAXIMIZEBOX)) ||
            (i==minimize_action && !(style&WS_MINIMIZEBOX))) continue;
        c->actions[i]=CreateWindowW(L"BUTTON",labels[i],WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,
                                    0,0,0,0,c->header,(HMENU)(INT_PTR)(1+i),module,NULL);
        if (!c->actions[i] || !SetWindowSubclass(c->actions[i],w_caption_button_proc,1,(DWORD_PTR)i) ||
            !w_icon_button(c->actions[i],glyphs[i],labels[i])) goto failed;
    }
    if (!SetPropW(w,caption_property,c)) goto failed;
    if (!SetWindowSubclass(w,w_caption_proc,(UINT_PTR)c,(DWORD_PTR)c)) {
        RemovePropW(w,caption_property);
        goto failed;
    }
    SetWindowPos(w,NULL,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOOWNERZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
    w_layout(w,c);
    return;
failed:
    DestroyWindow(c->header);
    free(c);
}
