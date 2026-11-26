#include "win_platform/material.h"
#include "win_platform/appearance.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <stdlib.h>

typedef struct {
    HWND panel, backing;
    enum w_material_role role;
    w_panel_material config;
    COLORREF tint;
    BYTE opacity, fade;
    int rounding, shape_radius, shape_width, shape_height, panel_alpha, corner_preference;
    bool blur, blur_unavailable, shadow, contrast, framed, layered_owned, panel_layered, busy, shaped, presented;
} w_material;

static const wchar_t *const surface_classes[]={L"WinwmMaterial",L"WinwmMaterialShadow"};
static const wchar_t material_property[]=L"winwm.panel.material";
static const UINT_PTR material_id=0x574d;
static LRESULT CALLBACK w_panel_proc(HWND,UINT,WPARAM,LPARAM,UINT_PTR,DWORD_PTR);

static w_material *w_attached(HWND window) {
    return (w_material *)GetPropW(window,material_property);
}

enum w_material_role w_material_role_of(HWND window) {
    w_material *m=w_attached(window);
    return m ? m->role : material_unattached;
}

static bool w_native_frame(HWND window) {
    LONG_PTR style=GetWindowLongPtrW(window,GWL_STYLE);
    if (style&WS_CAPTION) return true;
    if (!(style&WS_THICKFRAME)) return false;
    // A captionless popup can retain WS_THICKFRAME for native resizing while
    // claiming the full window rectangle in WM_NCCALCSIZE.
    RECT bounds,client;
    return !GetWindowRect(window,&bounds) || !GetClientRect(window,&client) ||
           bounds.right-bounds.left!=client.right || bounds.bottom-bounds.top!=client.bottom;
}

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

// DWM's corner preference controls native frames. Borderless, layered GDI
// popups need an explicit window region; the region is also their hit-test mask.
static bool w_shape_panel(w_material *m) {
    RECT bounds;
    if (!GetWindowRect(m->panel,&bounds)) return false;
    int width=bounds.right-bounds.left, height=bounds.bottom-bounds.top;
    if (width<=0 || height<=0) return false;
    bool framed=m->framed;
    int radius=framed ? -1 : min(min(width,height)/2,MulDiv(m->rounding,GetDpiForWindow(m->panel),96));
    DWORD corners=!framed || !m->rounding ? 1 : m->rounding<=4 ? 3 : 2;
    if (m->corner_preference==(int)corners && m->shape_radius==radius && (radius<=0 ||
        (m->shape_width==width && m->shape_height==height))) return false;
    if (radius>0) {
        // GDI's region rasterizer excludes the last occupied row/column at
        // these coordinates; the extra unit aligns the mask with RoundRect.
        HRGN region=CreateRoundRectRgn(0,0,width+1,height+1,2*radius,2*radius);
        if (!region) return false;
        if (!SetWindowRgn(m->panel,region,TRUE)) { DeleteObject(region); return false; }
        m->shaped=true;
    } else if (m->shaped) {
        if (!SetWindowRgn(m->panel,NULL,TRUE)) return false;
        m->shaped=false;
    }
    if (m->corner_preference!=(int)corners) {
        DwmSetWindowAttribute(m->panel,33,&corners,sizeof(corners));
        m->corner_preference=(int)corners;
    }
    m->shape_radius=radius; m->shape_width=width; m->shape_height=height;
    return true;
}

static bool w_acrylic(w_material *m) {
    int transient=3; // DWMSBT_TRANSIENTWINDOW
    MARGINS glass={-1,-1,-1,-1};
    BOOL dark=GetRValue(m->tint)*299+GetGValue(m->tint)*587+GetBValue(m->tint)*114<128000;
    DwmSetWindowAttribute(m->backing,20,&dark,sizeof(dark));
    if (SUCCEEDED(DwmSetWindowAttribute(m->backing,38,&transient,sizeof(transient))) &&
        SUCCEEDED(DwmExtendFrameIntoClientArea(m->backing,&glass))) return true;
    int none=1;
    DwmSetWindowAttribute(m->backing,38,&none,sizeof(none));
    m->blur_unavailable=true;
    m->blur=false;
    return false;
}

static bool w_set_layered(HWND window,LONG_PTR style,bool enabled) {
    LONG_PTR next=enabled ? style|WS_EX_LAYERED : style & ~(LONG_PTR)WS_EX_LAYERED;
    SetLastError(0);
    return SetWindowLongPtrW(window,GWL_EXSTYLE,next)!=0 || GetLastError()==0;
}

static bool w_new_surface(w_material *m) {
    HINSTANCE module=GetModuleHandleW(NULL);
    const wchar_t *name=surface_classes[m->shadow];
    WNDCLASSW wc={.hInstance=module,.lpszClassName=name,.lpfnWndProc=w_surface_proc,
                  .style=m->shadow ? CS_DROPSHADOW : 0,.hbrBackground=(HBRUSH)GetStockObject(BLACK_BRUSH)};
    if (!RegisterClassW(&wc) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) return false;
    DWORD ex=WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_TRANSPARENT;
    if (!m->blur) ex|=WS_EX_LAYERED;
    if (GetWindowLongPtrW(m->panel,GWL_EXSTYLE)&WS_EX_TOPMOST) ex|=WS_EX_TOPMOST;
    // Siblings can be adjacent in z order. A popup owned by the panel itself
    // would be forced above its owner.
    m->backing=CreateWindowExW(ex,name,L"",WS_POPUP|WS_DISABLED,0,0,0,0,GetWindow(m->panel,GW_OWNER),NULL,module,m);
    if (!m->backing) return false;
    BOOL disable_transitions=TRUE;
    DwmSetWindowAttribute(m->backing,DWMWA_TRANSITIONS_FORCEDISABLED,&disable_transitions,sizeof(disable_transitions));
    if (m->blur && !w_acrylic(m) && !m->shadow) {
        w_drop_surface(m);
        return false;
    }
    if (!m->blur && ((!(ex&WS_EX_LAYERED) && !w_set_layered(m->backing,ex,true)) ||
                     !SetLayeredWindowAttributes(m->backing,0,255,LWA_COLORKEY))) {
        w_drop_surface(m);
        return false;
    }
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

static void w_sync(w_material *m) {
    if (m->busy) return;
    m->busy=true;
    bool reshape=w_shape_panel(m);
    // A DWM backdrop needs ordinary window redirection. During an animated
    // transition only the foreground's alpha changes; the backdrop stays hidden.
    if ((!m->blur && !m->shadow) || !m->presented || !m->opacity || m->fade!=255 ||
        !IsWindowVisible(m->panel) || IsIconic(m->panel)) {
        if (m->backing && IsWindowVisible(m->backing)) ShowWindow(m->backing,SW_HIDE);
    } else {
        RECT target,old;
        bool fresh=m->backing==NULL;
        bool have_bounds=m->framed && SUCCEEDED(DwmGetWindowAttribute(m->panel,DWMWA_EXTENDED_FRAME_BOUNDS,
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
            if ((flags&(SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_SHOWWINDOW))!=(SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER)) {
                SetWindowPos(m->backing,m->panel,target.left,target.top,target.right-target.left,target.bottom-target.top,flags);
                if (flags&SWP_SHOWWINDOW) UpdateWindow(m->backing);
            }
        }
    }
    m->busy=false;
}

static bool w_panel_alpha(w_material *m,BYTE alpha) {
    LONG_PTR ex=GetWindowLongPtrW(m->panel,GWL_EXSTYLE);
    bool layered=(ex&WS_EX_LAYERED)!=0;
    if (m->panel_alpha==alpha && m->panel_layered==layered) return true;
    bool ok=true,redraw=false;
    if (!layered) {
        m->layered_owned=false;
        if (alpha<255) {
            ok=w_set_layered(m->panel,ex,true);
            layered=m->layered_owned=ok;
        }
    }
    if (layered && (!m->layered_owned || alpha<255))
        ok=SetLayeredWindowAttributes(m->panel,0,alpha,LWA_ALPHA)!=0;
    if (m->layered_owned && (!ok || alpha==255)) {
        if (w_set_layered(m->panel,ex,false)) {
            layered=m->layered_owned=false;
            redraw=true;
        } else ok=false;
    }
    m->panel_alpha=ok ? alpha : -1;
    m->panel_layered=layered;
    if (redraw)
        RedrawWindow(m->panel,NULL,NULL,RDW_INVALIDATE|RDW_ERASE|RDW_FRAME|RDW_ALLCHILDREN|(m->presented ? RDW_UPDATENOW : 0));
    return ok;
}

bool w_material_alpha(HWND window,BYTE fade) {
    w_material *m=w_attached(window);
    if (!m) return SetLayeredWindowAttributes(window,0,fade,LWA_ALPHA)!=0;
    BYTE previous=m->fade;
    m->fade=fade;
    BYTE alpha=m->contrast ? (fade ? 255 : 0) : (BYTE)MulDiv(m->opacity,fade,255);
    if (previous==255 && fade<255) w_sync(m);
    bool ok=w_panel_alpha(m,alpha);
    if (previous<255 && fade==255 && w_attached(window)==m) w_sync(m);
    return ok;
}

static void w_preferences(w_material *m) {
    HIGHCONTRASTW hc={.cbSize=sizeof(hc)};
    m->contrast=SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(hc),&hc,0) && (hc.dwFlags&HCF_HIGHCONTRASTON);
    BOOL shadow=FALSE,composed=FALSE;
    DWORD transparency=1,size=sizeof(transparency);
    RegGetValueW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"EnableTransparency",RRF_RT_REG_DWORD,NULL,&transparency,&size);
    m->opacity=m->contrast || !transparency ? 255 : (BYTE)max(0,min(m->config.opacity,255));
    // Remote sessions can accept the DWM attribute while drawing a flat system
    // fallback color. Keep the actual desktop visible through the normal alpha.
    m->blur=m->config.blur && !m->blur_unavailable && m->opacity>0 && m->opacity<255 && !m->contrast && transparency &&
            !GetSystemMetrics(SM_REMOTESESSION) &&
            SUCCEEDED(DwmIsCompositionEnabled(&composed)) && composed;
    // Standard frames already receive the system's nonclient shadow.
    m->framed=w_native_frame(m->panel);
    m->shadow=m->config.shadow && !m->framed && !m->contrast &&
              SystemParametersInfoW(SPI_GETDROPSHADOW,0,&shadow,0) && shadow;
}

static void w_refresh(w_material *m,COLORREF tint) {
    bool old_blur=m->blur,old_shadow=m->shadow;
    w_preferences(m);
    if (old_blur!=m->blur || old_shadow!=m->shadow) w_drop_surface(m);
    else if (m->backing && m->blur && m->tint!=tint) {
        BOOL dark=GetRValue(tint)*299+GetGValue(tint)*587+GetBValue(tint)*114<128000;
        DwmSetWindowAttribute(m->backing,20,&dark,sizeof(dark));
    }
    m->tint=tint;
    HWND panel=m->panel;
    w_material_alpha(panel,m->fade);
    if (w_attached(panel)==m) w_sync(m);
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
    if (message==WM_WINDOWPOSCHANGING && (((WINDOWPOS *)lp)->flags&SWP_HIDEWINDOW)) {
        m->presented=false;
        if (m->backing) ShowWindow(m->backing,SW_HIDE);
    }
    if (message==WM_WINDOWPOSCHANGED) {
        // Default processing delivers WM_SIZE and moves child controls. Paint
        // the finished layout before revealing its separate backdrop HWND.
        UINT flags=((WINDOWPOS *)lp)->flags;
        LRESULT result=DefSubclassProc(w,message,wp,lp);
        if (w_attached(w)!=m) return result;
        if ((flags&SWP_SHOWWINDOW) && !m->presented && IsWindowVisible(w)) {
            w_sync(m);
            RedrawWindow(w,NULL,NULL,RDW_INVALIDATE|RDW_ERASE|RDW_FRAME|RDW_ALLCHILDREN|RDW_UPDATENOW);
            if (w_attached(w)!=m) return result;
            m->presented=IsWindowVisible(w)!=FALSE;
        }
        w_sync(m);
        return result;
    }
    if (message==WM_DISPLAYCHANGE || message==WM_DWMCOMPOSITIONCHANGED) m->blur_unavailable=false;
    if (message==WM_DWMCOMPOSITIONCHANGED) {
        // The old compositor's backdrop, glass margins and corner preferences
        // need to be established again, even if the system settings match.
        m->corner_preference=-1;
        w_drop_surface(m);
    }
    if (message==WM_SYSCOLORCHANGE || message==WM_DWMCOMPOSITIONCHANGED || message==WM_DISPLAYCHANGE || message==WM_THEMECHANGED ||
        (message==WM_SETTINGCHANGE && (wp==SPI_SETDROPSHADOW || w_appearance_setting(wp,lp))))
        w_refresh(m,m->tint);
    if (message==WM_DPICHANGED) w_sync(m);
    return DefSubclassProc(w,message,wp,lp);
}

void w_material_apply(HWND window,const w_panel_material *material,COLORREF tint,int rounding,enum w_material_role role) {
    if (!material || !window || (GetWindowLongPtrW(window,GWL_STYLE)&WS_CHILD)) return;
    w_panel_material appearance=*material;
    if (role==material_dock) {
        rounding=0;
        appearance.shadow=false;
    }
    w_material *m=w_attached(window);
    if (!m) {
        m=calloc(1,sizeof(*m));
        if (!m) return;
        m->panel=window; m->fade=255; m->shape_radius=-2; m->panel_alpha=-1;
        m->presented=IsWindowVisible(window)!=FALSE;
        if (!SetPropW(window,material_property,m)) { free(m); return; }
        if (!SetWindowSubclass(window,w_panel_proc,material_id,(DWORD_PTR)m)) {
            RemovePropW(window,material_property); free(m); return;
        }
    } else if (m->config.opacity==appearance.opacity && m->config.blur==appearance.blur &&
               m->config.shadow==appearance.shadow && m->rounding==rounding && m->tint==tint && m->role==role) {
        // Framing can change while a live panel becomes an anchored popup.
        if (m->framed==w_native_frame(window)) return;
    }
    if (appearance.blur!=m->config.blur) m->blur_unavailable=false;
    m->role=role;
    m->config=appearance;
    m->rounding=rounding;
    w_refresh(m,tint);
}
