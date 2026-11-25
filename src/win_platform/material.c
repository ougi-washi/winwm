#include "win_platform/material.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <stdlib.h>

typedef struct {
    HWND panel, backing;
    w_panel_material config;
    COLORREF tint;
    BYTE opacity, fade, backing_alpha;
    bool blur, shadow, contrast, layered_owned, busy;
} w_material;

static const wchar_t *const surface_classes[]={L"WinwmMaterial",L"WinwmMaterialShadow"};
static const wchar_t material_property[]=L"winwm.panel.material";
static const UINT_PTR material_id=0x574d;
static LRESULT CALLBACK w_panel_proc(HWND,UINT,WPARAM,LPARAM,UINT_PTR,DWORD_PTR);

static w_material *w_attached(HWND window) {
    return (w_material *)GetPropW(window,material_property);
}

bool w_material_attached(HWND window) { return w_attached(window)!=NULL; }

static LRESULT CALLBACK w_surface_proc(HWND w,UINT message,WPARAM wp,LPARAM lp) {
    switch (message) {
    case WM_NCCREATE:
        SetWindowLongPtrW(w,GWLP_USERDATA,(LONG_PTR)((CREATESTRUCTW *)lp)->lpCreateParams);
        break;
    case WM_NCDESTROY: {
        w_material *m=(w_material *)GetWindowLongPtrW(w,GWLP_USERDATA);
        if (m && m->backing==w) m->backing=NULL;
        break;
    }
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return TRUE;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc=BeginPaint(w,&ps);
        // Black is transparent in an extended DWM frame and is the color key
        // for a shadow-only surface.
        FillRect(dc,&ps.rcPaint,(HBRUSH)GetStockObject(BLACK_BRUSH));
        EndPaint(w,&ps);
        return 0;
    }
    }
    return DefWindowProcW(w,message,wp,lp);
}

static void w_drop_surface(w_material *m) {
    if (m->backing) DestroyWindow(m->backing);
    m->backing=NULL;
}

static BYTE w_back_alpha(const w_material *m) {
    if (!m->blur || m->fade==255) return m->fade;
    // Foreground coverage is opacity * fade. Scale the remaining native
    // backdrop coverage so their composition fades linearly over the desktop.
    return (BYTE)MulDiv(255*m->fade,255-m->opacity,255*255-m->fade*m->opacity);
}

static bool w_surface_alpha(w_material *m) {
    BYTE alpha=w_back_alpha(m);
    LONG_PTR ex=GetWindowLongPtrW(m->backing,GWL_EXSTYLE);
    // Keep the resting acrylic surface on DWM's ordinary, non-layered
    // redirection path. Uniform window alpha is only needed during a fade.
    if (m->blur && alpha==255) {
        if (ex&WS_EX_LAYERED) {
            SetWindowLongPtrW(m->backing,GWL_EXSTYLE,ex & ~(LONG_PTR)WS_EX_LAYERED);
            MARGINS glass={-1,-1,-1,-1};
            DwmExtendFrameIntoClientArea(m->backing,&glass);
            InvalidateRect(m->backing,NULL,FALSE);
        }
    } else {
        if (!(ex&WS_EX_LAYERED)) SetWindowLongPtrW(m->backing,GWL_EXSTYLE,ex|WS_EX_LAYERED);
        if (!SetLayeredWindowAttributes(m->backing,0,alpha,LWA_ALPHA|(m->blur ? 0 : LWA_COLORKEY))) return false;
    }
    m->backing_alpha=alpha;
    return true;
}

static bool w_new_surface(w_material *m) {
    HINSTANCE module=GetModuleHandleW(NULL);
    const wchar_t *name=surface_classes[m->shadow];
    WNDCLASSW wc={.hInstance=module,.lpszClassName=name,.lpfnWndProc=w_surface_proc,
                  .style=m->shadow ? CS_DROPSHADOW : 0};
    if (!RegisterClassW(&wc) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) return false;
    DWORD ex=WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_TRANSPARENT;
    if (!m->blur) ex|=WS_EX_LAYERED;
    if (GetWindowLongPtrW(m->panel,GWL_EXSTYLE)&WS_EX_TOPMOST) ex|=WS_EX_TOPMOST;
    // Siblings can be adjacent in z order. A popup owned by the panel itself
    // would be forced above its owner.
    m->backing=CreateWindowExW(ex,name,L"",WS_POPUP|WS_DISABLED,0,0,0,0,GetWindow(m->panel,GW_OWNER),NULL,module,m);
    if (!m->backing) return false;
    if (m->blur) {
        int transient=3; // DWMSBT_TRANSIENTWINDOW
        MARGINS glass={-1,-1,-1,-1};
        if (FAILED(DwmSetWindowAttribute(m->backing,38,&transient,sizeof(transient))) ||
            FAILED(DwmExtendFrameIntoClientArea(m->backing,&glass))) {
            // Windows versions without desktop acrylic still support alpha and shadows.
            int none=1;
            DwmSetWindowAttribute(m->backing,38,&none,sizeof(none));
            m->blur=false;
            if (!m->shadow) { w_drop_surface(m); return false; }
        } else {
            BOOL dark=GetRValue(m->tint)*299+GetGValue(m->tint)*587+GetBValue(m->tint)*114<128000;
            DwmSetWindowAttribute(m->backing,20,&dark,sizeof(dark));
        }
    }
    if (!w_surface_alpha(m)) { w_drop_surface(m); return false; }
    return true;
}

static void w_shape(w_material *m) {
    HRGN region=CreateRectRgn(0,0,0,0);
    if (region) {
        if (GetWindowRgn(m->panel,region)!=ERROR) {
            if (!SetWindowRgn(m->backing,region,FALSE)) DeleteObject(region);
        } else { DeleteObject(region); SetWindowRgn(m->backing,NULL,FALSE); }
    }
    int corners;
    if (SUCCEEDED(DwmGetWindowAttribute(m->panel,33,&corners,sizeof(corners))))
        DwmSetWindowAttribute(m->backing,33,&corners,sizeof(corners));
}

static void w_sync(w_material *m,bool reshape) {
    if (m->busy) return;
    m->busy=true;
    if ((!m->blur && !m->shadow) || !m->opacity || !m->fade || !IsWindowVisible(m->panel) || IsIconic(m->panel)) {
        if (m->backing && IsWindowVisible(m->backing)) ShowWindow(m->backing,SW_HIDE);
    } else {
        RECT target,old;
        bool fresh=m->backing==NULL;
        bool framed=(GetWindowLongPtrW(m->panel,GWL_STYLE)&(WS_THICKFRAME|WS_CAPTION))!=0;
        bool have_bounds=framed && SUCCEEDED(DwmGetWindowAttribute(m->panel,DWMWA_EXTENDED_FRAME_BOUNDS,
                                                                   &target,sizeof(target)));
        if ((have_bounds || GetWindowRect(m->panel,&target)) && !IsRectEmpty(&target) &&
            (!fresh || w_new_surface(m))) {
            UINT flags=SWP_NOACTIVATE|SWP_NOOWNERZORDER;
            if (GetWindowRect(m->backing,&old)) {
                if (old.left==target.left && old.top==target.top) flags|=SWP_NOMOVE;
                if (old.right-old.left==target.right-target.left && old.bottom-old.top==target.bottom-target.top)
                    flags|=SWP_NOSIZE;
            }
            if (GetWindow(m->panel,GW_HWNDNEXT)==m->backing) flags|=SWP_NOZORDER;
            if (!IsWindowVisible(m->backing)) flags|=SWP_SHOWWINDOW;
            if (fresh || reshape || !(flags&SWP_NOSIZE)) w_shape(m);
            if ((flags&(SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_SHOWWINDOW))!=(SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER))
                SetWindowPos(m->backing,m->panel,target.left,target.top,target.right-target.left,target.bottom-target.top,flags);
        }
    }
    m->busy=false;
}

bool w_material_alpha(HWND window,BYTE fade) {
    w_material *m=w_attached(window);
    if (!m) return SetLayeredWindowAttributes(window,0,fade,LWA_ALPHA)!=0;
    BYTE previous=m->fade;
    m->fade=fade;
    BYTE alpha=m->contrast ? (fade ? 255 : 0) : (BYTE)MulDiv(m->opacity,fade,255);
    LONG_PTR ex=GetWindowLongPtrW(window,GWL_EXSTYLE);
    bool ok=true;
    if (alpha<255 && !(ex&WS_EX_LAYERED)) {
        SetLastError(0);
        ok=SetWindowLongPtrW(window,GWL_EXSTYLE,ex|WS_EX_LAYERED) || GetLastError()==0;
        if (ok) { m->layered_owned=true; ex|=WS_EX_LAYERED; }
    }
    if (ok && (ex&WS_EX_LAYERED)) {
        ok=SetLayeredWindowAttributes(window,0,alpha,LWA_ALPHA)!=0;
        if ((!ok || alpha==255) && m->layered_owned) {
            SetWindowLongPtrW(window,GWL_EXSTYLE,ex & ~(LONG_PTR)WS_EX_LAYERED);
            m->layered_owned=false;
        }
    }
    if (m->backing && w_back_alpha(m)!=m->backing_alpha) w_surface_alpha(m);
    if ((!previous)!=(!fade)) w_sync(m,false);
    return ok;
}

static void w_preferences(w_material *m) {
    HIGHCONTRASTW hc={.cbSize=sizeof(hc)};
    m->contrast=SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(hc),&hc,0) && (hc.dwFlags&HCF_HIGHCONTRASTON);
    BOOL shadow=FALSE,composed=FALSE;
    DWORD transparency=1,size=sizeof(transparency);
    RegGetValueW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"EnableTransparency",RRF_RT_REG_DWORD,NULL,&transparency,&size);
    m->opacity=m->contrast ? 255 : (BYTE)max(0,min(m->config.opacity,255));
    m->blur=m->config.blur && m->opacity>0 && m->opacity<255 && !m->contrast && transparency &&
            SUCCEEDED(DwmIsCompositionEnabled(&composed)) && composed;
    // Standard frames already receive the system's nonclient shadow.
    bool framed=(GetWindowLongPtrW(m->panel,GWL_STYLE)&(WS_THICKFRAME|WS_CAPTION))!=0;
    m->shadow=m->config.shadow && !framed && !m->contrast &&
              SystemParametersInfoW(SPI_GETDROPSHADOW,0,&shadow,0) && shadow;
}

static void w_refresh(w_material *m,COLORREF tint) {
    bool old_blur=m->blur,old_shadow=m->shadow;
    w_preferences(m);
    if (old_blur!=m->blur || old_shadow!=m->shadow || (m->blur && m->tint!=tint)) w_drop_surface(m);
    m->tint=tint;
    w_material_alpha(m->panel,m->fade);
    w_sync(m,true);
}

static LRESULT CALLBACK w_panel_proc(HWND w,UINT message,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR ref) {
    w_material *m=(w_material *)ref;
    if (message==WM_NCDESTROY) {
        RemovePropW(w,material_property);
        RemoveWindowSubclass(w,w_panel_proc,id);
        w_drop_surface(m);
        free(m);
        return DefSubclassProc(w,message,wp,lp);
    }
    if (message==WM_WINDOWPOSCHANGING && (((WINDOWPOS *)lp)->flags&SWP_HIDEWINDOW) && m->backing)
        ShowWindow(m->backing,SW_HIDE);
    if (message==WM_SETTINGCHANGE || message==WM_DWMCOMPOSITIONCHANGED || message==WM_THEMECHANGED)
        w_refresh(m,m->tint);
    if (message==WM_WINDOWPOSCHANGED) w_sync(m,false);
    return DefSubclassProc(w,message,wp,lp);
}

void w_material_apply(HWND window,const w_panel_material *material,COLORREF tint) {
    if (!material || !window || (GetWindowLongPtrW(window,GWL_STYLE)&WS_CHILD)) return;
    w_material *m=w_attached(window);
    if (!m) {
        m=calloc(1,sizeof(*m));
        if (!m) return;
        m->panel=window; m->fade=255;
        if (!SetPropW(window,material_property,m)) { free(m); return; }
        if (!SetWindowSubclass(window,w_panel_proc,material_id,(DWORD_PTR)m)) {
            RemovePropW(window,material_property); free(m); return;
        }
    }
    m->config=*material;
    w_refresh(m,tint);
}
