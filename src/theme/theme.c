#include "theme/surface.h"
#include "theme/hover.h"
#include "theme/native_controls.h"
#include "win_platform/material.h"
#include "win_platform/appearance.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <wchar.h>

////////////////
//~ Theme

static COLORREF surface = RGB(16, 17, 22), ink = RGB(228, 229, 234), active = RGB(145, 170, 247);
typedef struct {
    uint32_t background, foreground, accent;
    int rounding;
    w_panel_material material;
    w_panel_metrics metrics;
    enum motion_preference animation;
    w_transition enter, leave;
} w_theme_profile;
static w_theme_profile profile={.background=0x101116,.foreground=0xe4e5ea,.accent=0x91aaf7,.rounding=2};
static UINT_PTR profile_revision;
enum { follows_panel=1, follows_overlay, changed_paint=1, changed_layout=2, changed_motion=4 };
static HBRUSH brush;
static bool high_contrast;
static BOOL drop_shadows=TRUE;
static bool configured;
static const wchar_t tool_property[]=L"winwm.theme.tool";
static const wchar_t revision_property[]=L"winwm.theme.revision.v1";
static const wchar_t mapping_property[]=L"winwm.theme.mapping";
static UINT theme_changed;

////////////////
//~ Theme Functions

static COLORREF w_color(uint32_t rgb) {
    return RGB(rgb >> 16, rgb >> 8, rgb);
}

COLORREF w_theme_mix(COLORREF a, COLORREF b, int percent) {
    return RGB((GetRValue(a)*(100-percent) + GetRValue(b)*percent)/100,
               (GetGValue(a)*(100-percent) + GetGValue(b)*percent)/100,
               (GetBValue(a)*(100-percent) + GetBValue(b)*percent)/100);
}

HBRUSH w_theme_brush(void) {
    if (!brush)
        brush = CreateSolidBrush(surface);
    return brush;
}

COLORREF w_theme_background(void) { return surface; }
COLORREF w_theme_foreground(void) { return ink; }
int w_theme_radius(UINT dpi) { return MulDiv(profile.rounding,dpi ? dpi : 96,96); }
int w_theme_control_radius(UINT dpi) { return MulDiv(min(2,profile.rounding),dpi ? dpi : 96,96); }

COLORREF w_theme_separator(void) {
    return high_contrast ? ink : w_theme_mix(surface,active,42);
}

static void w_outline(HDC dc,const RECT *bounds,int radius,COLORREF edge) {
    int saved=SaveDC(dc);
    SelectObject(dc,GetStockObject(DC_PEN));
    SelectObject(dc,GetStockObject(NULL_BRUSH));
    SetDCPenColor(dc,edge);
    int curve=min(min(bounds->right-bounds->left,bounds->bottom-bounds->top),2*radius);
    if (curve>0) RoundRect(dc,bounds->left,bounds->top,bounds->right,bounds->bottom,curve,curve);
    else Rectangle(dc,bounds->left,bounds->top,bounds->right,bounds->bottom);
    RestoreDC(dc,saved);
}

void w_theme_frame(HDC dc,const RECT *bounds,UINT dpi) {
    w_outline(dc,bounds,w_theme_radius(dpi),w_theme_separator());
}

void w_theme_control_frame(HDC dc,const RECT *bounds,UINT dpi) {
    w_outline(dc,bounds,w_theme_control_radius(dpi),w_theme_separator());
}

void w_theme_input_frame(HDC dc,const RECT *bounds,int hover,bool focused,bool enabled) {
    COLORREF edge=w_theme_mix(surface,ink,enabled ? 38 : 20);
    if (high_contrast) edge=GetSysColor(!enabled ? COLOR_GRAYTEXT : focused ? COLOR_HIGHLIGHT : COLOR_WINDOWTEXT);
    else if (focused && enabled) edge=active;
    else if (enabled && hover) edge=w_theme_mix(edge,active,MulDiv(w_clamp_int(hover,0,255),65,255));
    w_outline(dc,bounds,0,edge);
}

COLORREF w_theme_accent(void) {
    return high_contrast ? ink : active;
}

COLORREF w_theme_hint(void) {
    return high_contrast ? ink : w_theme_mix(surface, ink, 72);
}

COLORREF w_theme_selection(bool foreground) {
    if (high_contrast)
        return GetSysColor(foreground ? COLOR_HIGHLIGHTTEXT : COLOR_HIGHLIGHT);
    return foreground ? ink : w_theme_mix(surface, active, 24);
}

static void w_theme_colors(uint32_t background, uint32_t foreground, uint32_t accent) {
    HIGHCONTRASTW hc = {.cbSize = sizeof(hc)};
    SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0);
    bool contrast = (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
    high_contrast = contrast;
    SystemParametersInfoW(SPI_GETDROPSHADOW,0,&drop_shadows,0);
    COLORREF next = contrast ? GetSysColor(COLOR_WINDOW) : w_color(background);
    if (next != surface && brush) {
        DeleteObject(brush);
        brush = NULL;
    }
    surface = next;
    ink = contrast ? GetSysColor(COLOR_WINDOWTEXT) : w_color(foreground);
    active = contrast ? GetSysColor(COLOR_HIGHLIGHT) : w_color(accent);
}

void w_theme_material(HWND window) {
    if (configured && window) {
        enum w_material_role role=w_material_role_of(window);
        w_material_apply(window,&profile.material,surface,profile.rounding,role==material_unattached ? material_panel : role);
    }
}

void w_theme_dock_material(HWND window) {
    if (configured) w_material_apply(window,&profile.material,surface,profile.rounding,material_dock);
}

static void w_frame_colors(HWND window) {
    BOOL dark=!high_contrast && GetRValue(surface)*299+GetGValue(surface)*587+GetBValue(surface)*114<128000;
    COLORREF edge=high_contrast ? 0xffffffffu :
                  GetActiveWindow()==window ? active : w_theme_mix(surface,ink,24);
    DwmSetWindowAttribute(window,20,&dark,sizeof(dark));
    DwmSetWindowAttribute(window,34,&edge,sizeof(edge));
}

void w_theme_native_controls(HWND window) {
    w_native_controls_theme(window, high_contrast, surface);
}

// Keep user-sized windows in place while they still fit their controls.
// A larger metric can raise a panel's minimum even when its outer frame and
// DPI stay the same. Windows enforces WM_GETMINMAXINFO on an interactive
// resize, but not on a configuration notification.
static void w_layout_window(HWND w,bool refresh_frame) {
    UINT flags=SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOOWNERZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED;
    RECT before,client,bounds={0};
    GetClientRect(w,&before);
    if ((GetWindowLongW(w,GWL_STYLE)&WS_THICKFRAME) && !IsZoomed(w) && GetWindowRect(w,&bounds)) {
        MINMAXINFO limits={0};
        SendMessageW(w,WM_GETMINMAXINFO,0,(LPARAM)&limits);
        LONG width=bounds.right-bounds.left, height=bounds.bottom-bounds.top;
        MONITORINFO monitor={.cbSize=sizeof(monitor)};
        if ((width<limits.ptMinTrackSize.x || height<limits.ptMinTrackSize.y) &&
            GetMonitorInfoW(MonitorFromWindow(w,MONITOR_DEFAULTTONEAREST),&monitor)) {
            RECT area=monitor.rcWork;
            LONG next_w=min(area.right-area.left,max(width,limits.ptMinTrackSize.x));
            LONG next_h=min(area.bottom-area.top,max(height,limits.ptMinTrackSize.y));
            LONG left=max(area.left,min(bounds.left,area.right-next_w));
            LONG top=max(area.top,min(bounds.top,area.bottom-next_h));
            if (left!=bounds.left || top!=bounds.top) flags&=~SWP_NOMOVE;
            if (next_w!=width || next_h!=height) flags&=~SWP_NOSIZE;
            bounds=(RECT){left,top,left+next_w,top+next_h};
        }
    }
    if (!refresh_frame && (flags&SWP_NOSIZE)) return;
    SetWindowPos(w,NULL,bounds.left,bounds.top,bounds.right-bounds.left,bounds.bottom-bounds.top,flags);
    if (refresh_frame && GetClientRect(w,&client) && EqualRect(&client,&before))
        SendMessageW(w,WM_SIZE,IsZoomed(w) ? SIZE_MAXIMIZED : SIZE_RESTORED,
                     MAKELPARAM(client.right-client.left,client.bottom-client.top));
}

static LRESULT CALLBACK w_size_on_restore(HWND w,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR unused) {
    (void)unused;
    if (msg==WM_SIZE && wp==SIZE_RESTORED && !IsIconic(w) && !IsZoomed(w)) {
        RemoveWindowSubclass(w,w_size_on_restore,id);
        LRESULT result=DefSubclassProc(w,msg,wp,lp);
        if (IsWindow(w)) w_layout_window(w,true);
        return result;
    }
    if (msg==WM_NCDESTROY) RemoveWindowSubclass(w,w_size_on_restore,id);
    return DefSubclassProc(w,msg,wp,lp);
}

static void w_resize_panel(HWND window,bool refresh_frame) {
    if ((GetWindowLongW(window,GWL_STYLE)&WS_THICKFRAME) && (IsZoomed(window) || IsIconic(window)))
        SetWindowSubclass(window,w_size_on_restore,1,0);
    if (!IsIconic(window)) w_layout_window(window,refresh_frame);
}

static BOOL CALLBACK w_refresh_panel(HWND window,LPARAM arg) {
    if (w_material_role_of(window)!=material_unattached) {
        if (arg&changed_motion) SendMessageW(window,winwm_motion_updated,0,0);
        // Finishing an exit after a reduced-motion change can destroy the flyout here.
        if (!(arg&changed_paint) || !IsWindow(window)) return TRUE;
        w_frame_colors(window);
        w_theme_material(window);
        w_theme_native_controls(window);
        SendMessageW(window,winwm_theme_updated,0,0);
        if (!IsWindow(window)) return TRUE;
        if (arg&changed_layout) w_resize_panel(window,true);
        RedrawWindow(window,NULL,NULL,RDW_INVALIDATE|RDW_ERASE|RDW_FRAME|RDW_ALLCHILDREN);
    } else if ((arg&changed_paint) && (UINT_PTR)GetPropW(window,tool_property)==follows_overlay)
        SendMessageW(window,winwm_theme_updated,0,0);
    return TRUE;
}

static bool w_same_transition(const w_transition *a,const w_transition *b) {
    return a->ms==b->ms && a->style==b->style && a->percent==b->percent &&
           !memcmp(&a->curve,&b->curve,sizeof(a->curve));
}

static void w_set_theme(const w_theme_profile *next) {
    bool materials=!configured || next->material.opacity!=profile.material.opacity ||
                   next->material.blur!=profile.material.blur || next->material.shadow!=profile.material.shadow ||
                   profile.rounding!=next->rounding;
    bool layout=w_theme_metrics_set(next->metrics);
    bool motion=!configured || profile.animation!=next->animation ||
                !w_same_transition(&profile.enter,&next->enter) || !w_same_transition(&profile.leave,&next->leave);
    if (materials || layout || motion || profile.background!=next->background ||
        profile.foreground!=next->foreground || profile.accent!=next->accent) ++profile_revision;
    bool previous_contrast=high_contrast;
    COLORREF previous_surface=surface,previous_ink=ink,previous_accent=active;
    profile=*next;
    w_theme_colors(profile.background,profile.foreground,profile.accent);
    configured=true;
    if (motion || previous_contrast!=high_contrast) w_hover_preferences_changed();
    bool paint=materials || layout || previous_contrast!=high_contrast || previous_surface!=surface ||
               previous_ink!=ink || previous_accent!=active;
    if (paint || motion) EnumThreadWindows(GetCurrentThreadId(),w_refresh_panel,
                (paint ? changed_paint : 0)|(layout ? changed_layout : 0)|(motion ? changed_motion : 0));
}

void w_theme_configure(const w_desktop_config *config) {
    w_theme_profile next={config->background,config->foreground,config->accent,config->rounding,
                          config->panel_material,config->panel_metrics,config->animations,
                          config->motion[motion_layers_in],config->motion[motion_layers_out]};
    w_set_theme(&next);
}

w_transition w_theme_layer_timing(bool entering) { return entering ? profile.enter : profile.leave; }

bool w_theme_motion_enabled(void) {
    HIGHCONTRASTW hc={.cbSize=sizeof(hc)};
    BOOL animate=TRUE;
    SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(hc),&hc,0);
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&animate,0);
    return !(hc.dwFlags&HCF_HIGHCONTRASTON) &&
           (profile.animation==motion_on || (profile.animation==motion_system && animate));
}

bool w_theme_configured(void) { return configured; }
bool w_theme_high_contrast(void) { return high_contrast; }
bool w_theme_button_shadows(void) { return !high_contrast && drop_shadows && (!configured || profile.material.shadow); }

static BOOL CALLBACK w_notify_tool(HWND window,LPARAM unused) {
    (void)unused;
    if (GetPropW(window,tool_property)) PostMessageW(window,theme_changed,0,0);
    return TRUE;
}

static void w_mapping_name(HWND manager,UINT_PTR revision,wchar_t name[96]) {
    DWORD pid=0;
    GetWindowThreadProcessId(manager,&pid);
    swprintf(name,96,L"Local\\Winwm.Theme.v1.%lu.%llu.%llu",pid,
             (unsigned long long)(UINT_PTR)manager,(unsigned long long)revision);
}

static LRESULT CALLBACK w_publisher(HWND w,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR unused) {
    (void)unused;
    if (msg==WM_NCDESTROY) {
        RemovePropW(w,revision_property);
        RemoveWindowSubclass(w,w_publisher,id);
        CloseHandle(RemovePropW(w,mapping_property));
    }
    return DefSubclassProc(w,msg,wp,lp);
}

// Publish one complete immutable snapshot. A newly opened tool can read it
// before the asynchronous profile cache write. No reader waits for the manager.
void w_theme_publish(HWND manager) {
    if (!manager || !profile_revision || (UINT_PTR)GetPropW(manager,revision_property)==profile_revision) return;
    wchar_t name[96];
    w_mapping_name(manager,profile_revision,name);
    HANDLE mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,NULL,PAGE_READWRITE,0,sizeof(profile),name);
    if (!mapping) return;
    if (GetLastError()==ERROR_ALREADY_EXISTS) { CloseHandle(mapping); return; }
    void *view=MapViewOfFile(mapping,FILE_MAP_WRITE,0,0,sizeof(profile));
    if (!view) { CloseHandle(mapping); return; }
    memcpy(view,&profile,sizeof(profile));
    UnmapViewOfFile(view);
    HANDLE previous=GetPropW(manager,mapping_property);
    if (!previous && !SetWindowSubclass(manager,w_publisher,1,0)) { CloseHandle(mapping); return; }
    if (!SetPropW(manager,mapping_property,mapping) || !SetPropW(manager,revision_property,(HANDLE)profile_revision)) {
        if (previous) SetPropW(manager,mapping_property,previous);
        else {
            RemovePropW(manager,mapping_property);
            RemoveWindowSubclass(manager,w_publisher,1);
        }
        CloseHandle(mapping);
        return;
    }
    if (previous) CloseHandle(previous);
    if (!theme_changed) theme_changed=RegisterWindowMessageW(L"winwm.theme.changed");
    if (theme_changed) EnumWindows(w_notify_tool,0);
}

bool w_theme_read_published(void) {
    HWND manager=FindWindowW(L"WinwmEvents",NULL);
    UINT_PTR revision=manager ? (UINT_PTR)GetPropW(manager,revision_property) : 0;
    if (!revision) return false;
    wchar_t name[96];
    w_mapping_name(manager,revision,name);
    HANDLE mapping=OpenFileMappingW(FILE_MAP_READ,FALSE,name);
    if (!mapping) return false; // A newer publication also posts a new notification.
    const w_theme_profile *view=MapViewOfFile(mapping,FILE_MAP_READ,0,0,sizeof(profile));
    bool mapped=view!=NULL;
    if (mapped) {
        w_set_theme(view);
        UnmapViewOfFile(view);
    }
    CloseHandle(mapping);
    return mapped;
}

static LRESULT CALLBACK w_subscriber(HWND w,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR kind) {
    if (msg==theme_changed && theme_changed) {
        w_theme_read_published();
        return 0;
    }
    if (kind==follows_panel && msg==WM_SETTINGCHANGE && wp==SPI_SETNONCLIENTMETRICS &&
        (GetWindowLongW(w,GWL_STYLE)&WS_THICKFRAME)) {
        // The panel refreshes its cached text widths in its own window procedure.
        LRESULT result=DefSubclassProc(w,msg,wp,lp);
        if (IsWindow(w)) w_resize_panel(w,false);
        return result;
    }
    if (kind==follows_overlay && (msg==WM_THEMECHANGED || msg==WM_SYSCOLORCHANGE ||
        (msg==WM_SETTINGCHANGE && w_appearance_setting(wp,lp)))) {
        w_theme_colors(profile.background,profile.foreground,profile.accent);
        SendMessageW(w,winwm_theme_updated,0,0);
    }
    if (msg==WM_NCDESTROY) {
        RemovePropW(w,tool_property);
        RemoveWindowSubclass(w,w_subscriber,id);
    }
    return DefSubclassProc(w,msg,wp,lp);
}

static void w_subscribe(HWND window,UINT_PTR kind) {
    if (!theme_changed) theme_changed=RegisterWindowMessageW(L"winwm.theme.changed");
    if (theme_changed && SetWindowSubclass(window,w_subscriber,1,kind) && SetPropW(window,tool_property,(HANDLE)kind))
        w_theme_read_published();
}

void w_theme_follow(HWND tool) {
    w_subscribe(tool,follows_panel);
    w_theme_native_controls(tool);
}

void w_theme_follow_overlay(HWND window) { w_subscribe(window,follows_overlay); }

bool w_theme_surface_message(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if ((msg==WM_SHOWWINDOW && !wp) || (msg==WM_ENABLE && !wp) || msg==WM_CANCELMODE)
        w_hover_clear_children(w);
    if (msg==WM_SETTINGCHANGE && (wp==SPI_SETCLIENTAREAANIMATION || wp==SPI_SETHIGHCONTRAST))
        w_hover_preferences_changed();
    if (msg==WM_FONTCHANGE) {
        w_theme_fonts_changed();
        w_native_controls_font_changed(w);
        RedrawWindow(w,NULL,NULL,RDW_INVALIDATE|RDW_ERASE|RDW_FRAME|RDW_ALLCHILDREN);
    } else if (msg == WM_CREATE) {
        w_frame_colors(w);
        w_theme_material(w);
    } else if (msg == WM_SHOWWINDOW && wp) {
        w_theme_native_controls(w);
    } else if (msg == WM_SYSCOLORCHANGE || msg == WM_THEMECHANGED ||
               (msg == WM_SETTINGCHANGE && w_appearance_setting(wp,lp))) {
        w_theme_colors(profile.background,profile.foreground,profile.accent);
        w_frame_colors(w);
        w_theme_material(w);
        w_theme_native_controls(w);
        RedrawWindow(w,NULL,NULL,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN);
    } else if (msg == WM_ACTIVATE) {
        COLORREF edge = high_contrast ? 0xffffffffu : LOWORD(wp) == WA_INACTIVE ? w_theme_mix(surface, ink, 24) : active;
        DwmSetWindowAttribute(w, 34, &edge, sizeof(edge));
    }
    return false;
}
