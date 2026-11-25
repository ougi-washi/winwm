#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <stdio.h>
#include "win_platform/layer.h"
#include "win_platform/material.h"

// Use real USER32 windows, regions, timers and alpha. Substitute only system
// preferences and the compositor so the same transitions run on RDP and CI.
static bool transparency=true, contrast, supported, dark;
static unsigned attempts;

static LSTATUS read_transparency(HKEY key,LPCWSTR path,LPCWSTR name,DWORD flags,LPDWORD type,PVOID value,LPDWORD size) {
    if (!wcscmp(name,L"EnableTransparency")) {
        *(DWORD *)value=transparency;
        return ERROR_SUCCESS;
    }
    return RegGetValueW(key,path,name,flags,type,value,size);
}

static BOOL read_preference(UINT action,UINT size,PVOID value,UINT flags) {
    if (action==SPI_GETHIGHCONTRAST) {
        ((HIGHCONTRASTW *)value)->dwFlags=contrast ? HCF_HIGHCONTRASTON : 0;
        return TRUE;
    }
    if (action==SPI_GETDROPSHADOW) { *(BOOL *)value=TRUE; return TRUE; }
    return SystemParametersInfoW(action,size,value,flags);
}

static int read_metric(int index) { return index==SM_REMOTESESSION ? 0 : GetSystemMetrics(index); }
static HRESULT composition(BOOL *value) { *value=TRUE; return S_OK; }
static HRESULT extend_glass(HWND window,const MARGINS *margins) { (void)window; (void)margins; return S_OK; }
static HRESULT attribute(HWND window,DWORD attr,const void *value,DWORD size) {
    if (attr==38 && *(const int *)value==3) { ++attempts; return supported ? S_OK : E_INVALIDARG; }
    if (attr==20) { dark=*(const BOOL *)value!=FALSE; return S_OK; }
    return DwmSetWindowAttribute(window,attr,value,size);
}

#define RegGetValueW read_transparency
#define SystemParametersInfoW read_preference
#define GetSystemMetrics read_metric
#define DwmIsCompositionEnabled composition
#define DwmSetWindowAttribute attribute
#define DwmExtendFrameIntoClientArea extend_glass
#include "win_platform/material.c"

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr,"Material test failed at line %d: %s\n",__LINE__,#expr); return false; } } while (0)

static int alpha_of(HWND window) {
    if (!(GetWindowLongPtrW(window,GWL_EXSTYLE)&WS_EX_LAYERED)) return 255;
    BYTE alpha=0;
    DWORD flags=0;
    return GetLayeredWindowAttributes(window,NULL,&alpha,&flags) && (flags&LWA_ALPHA) ? alpha : -1;
}

static bool run(HWND panel,HWND dock) {
    w_panel_material cfg={.opacity=204,.blur=true,.shadow=true};
    w_material_apply(panel,&cfg,RGB(14,19,22),16,material_panel);
    w_material_apply(dock,&cfg,RGB(14,19,22),16,material_dock);
    CHECK(w_material_role_of(dock)==material_dock && w_material_role_of(panel)==material_panel);
    HRGN shape=CreateRectRgn(0,0,0,0);
    CHECK(shape && GetWindowRgn(panel,shape)!=ERROR && !PtInRegion(shape,0,0) && PtInRegion(shape,50,30));
    CHECK(GetWindowRgn(dock,shape)==ERROR);
    DeleteObject(shape);
    CHECK(attempts==0 && !FindWindowW(L"WinwmMaterialShadow",NULL));
    ShowWindow(dock,SW_SHOWNOACTIVATE);
    CHECK(attempts==1 && alpha_of(dock)==204 && !FindWindowW(L"WinwmMaterial",NULL));
    ShowWindow(panel,SW_SHOWNOACTIVATE);
    HWND shadow=FindWindowW(L"WinwmMaterialShadow",NULL);
    CHECK(attempts==2 && shadow && IsWindowVisible(shadow) && !IsWindowEnabled(shadow));
    CHECK(SendMessageW(shadow,WM_NCHITTEST,0,0)==HTTRANSPARENT && GetWindow(panel,GW_HWNDNEXT)==shadow);
    DWORD flags=0;
    CHECK(GetLayeredWindowAttributes(shadow,NULL,NULL,&flags) && (flags&LWA_COLORKEY));

    SetWindowPos(panel,NULL,31,47,180,90,SWP_NOZORDER|SWP_NOACTIVATE);
    RECT panel_rect,back_rect;
    CHECK(GetWindowRect(panel,&panel_rect) && GetWindowRect(shadow,&back_rect) && EqualRect(&panel_rect,&back_rect));
    SendMessageW(panel,WM_SETTINGCHANGE,0,(LPARAM)L"ImmersiveColorSet");
    SendMessageW(panel,WM_THEMECHANGED,0,0);
    CHECK(attempts==2 && FindWindowW(L"WinwmMaterialShadow",NULL)==shadow);
    ShowWindow(panel,SW_HIDE);
    CHECK(!IsWindowVisible(shadow));
    ShowWindow(panel,SW_SHOWNOACTIVATE);
    CHECK(IsWindowVisible(shadow) && attempts==2);

    supported=true;
    SendMessageW(dock,WM_DISPLAYCHANGE,32,MAKELPARAM(1280,720));
    HWND dock_back=FindWindowW(L"WinwmMaterial",NULL);
    CHECK(attempts==3 && dock_back && IsWindowVisible(dock_back));
    ShowWindow(dock,SW_HIDE);
    SendMessageW(panel,WM_DWMCOMPOSITIONCHANGED,0,0);
    HWND acrylic=FindWindowW(L"WinwmMaterialShadow",NULL);
    CHECK(attempts==4 && dark && acrylic && !(GetWindowLongPtrW(acrylic,GWL_EXSTYLE)&WS_EX_LAYERED));
    w_material_apply(panel,&cfg,RGB(240,238,241),16,material_panel);
    CHECK(!dark && FindWindowW(L"WinwmMaterialShadow",NULL)==acrylic);
    CHECK(w_material_alpha(panel,128) && alpha_of(panel)==102);
    CHECK(alpha_of(acrylic)==MulDiv(255*128,255-204,255*255-128*204));
    CHECK(w_material_alpha(panel,255) && attempts==5 && alpha_of(panel)==204);
    CHECK(!(GetWindowLongPtrW(acrylic,GWL_EXSTYLE)&WS_EX_LAYERED));
    CHECK(w_material_alpha(panel,128));
    supported=false;
    CHECK(w_material_alpha(panel,255) && attempts==6 && GetLayeredWindowAttributes(acrylic,NULL,NULL,&flags) && (flags&LWA_COLORKEY));

    transparency=false;
    SendMessageW(panel,WM_SETTINGCHANGE,0,(LPARAM)L"EnableTransparency");
    CHECK(!(GetWindowLongPtrW(panel,GWL_EXSTYLE)&WS_EX_LAYERED) && w_material_alpha(panel,128) && alpha_of(panel)==128);
    contrast=true;
    SendMessageW(panel,WM_SETTINGCHANGE,SPI_SETHIGHCONTRAST,0);
    CHECK(alpha_of(panel)==255 && !FindWindowW(L"WinwmMaterialShadow",NULL));
    CHECK(w_material_alpha(panel,0) && alpha_of(panel)==0 && w_material_alpha(panel,64) && alpha_of(panel)==255);
    contrast=false;
    SendMessageW(panel,WM_SETTINGCHANGE,SPI_SETHIGHCONTRAST,0);
    CHECK(alpha_of(panel)==64);

    w_layer_fade fade={.value=64};
    w_transition linear={.ms=1000,.curve={0,0,1,1}};
    CHECK(w_layer_fade_to(panel,&fade,0,linear,true) && fade.timing.ms==MulDiv(1000,64,255));
    uint64_t start=fade.start;
    CHECK(w_layer_fade_to(panel,&fade,0,linear,true) && fade.start==start);
    fade.start=GetTickCount64()-100;
    CHECK(w_layer_fade_tick(panel,&fade,true) && fade.value<64 && fade.value>0);
    BYTE current=fade.value;
    CHECK(w_layer_fade_to(panel,&fade,255,linear,true) && fade.from==current && fade.timing.ms==MulDiv(1000,255-current,255));
    CHECK(!w_layer_fade_to(panel,&fade,255,linear,false) && !fade.start && fade.value==255 && !KillTimer(panel,layer_fade_timer));
    fade.value=1;
    linear.ms=50;
    CHECK(!w_layer_fade_to(panel,&fade,0,linear,true) && !fade.start && alpha_of(panel)==0);
    return true;
}

int wmain(void) {
    wchar_t name[64];
    swprintf(name,64,L"WinwmMaterialTest-%lu-%llu",GetCurrentProcessId(),GetTickCount64());
    HDESK original=GetThreadDesktop(GetCurrentThreadId());
    HDESK desktop=CreateDesktopW(name,NULL,NULL,0,GENERIC_ALL,NULL);
    if (!desktop || !SetThreadDesktop(desktop)) { fprintf(stderr,"Private desktop setup failed (%lu)\n",GetLastError()); return 1; }
    WNDCLASSW wc={.hInstance=GetModuleHandleW(NULL),.lpszClassName=L"WinwmTestPanelMaterial",.lpfnWndProc=DefWindowProcW};
    if (!RegisterClassW(&wc)) return 1;
    HWND panel=CreateWindowExW(0,wc.lpszClassName,L"",WS_POPUP,3,5,100,80,NULL,NULL,wc.hInstance,NULL);
    HWND dock=CreateWindowExW(0,wc.lpszClassName,L"",WS_POPUP,0,0,300,32,NULL,NULL,wc.hInstance,NULL);
    bool ok=panel && dock && run(panel,dock);
    if (panel) DestroyWindow(panel);
    if (dock) DestroyWindow(dock);
    if (FindWindowW(L"WinwmMaterialShadow",NULL) || FindWindowW(L"WinwmMaterial",NULL)) ok=false;
    UnregisterClassW(wc.lpszClassName,wc.hInstance);
    SetThreadDesktop(original);
    CloseDesktop(desktop);
    if (ok) puts("Panel and dock shape, compositor recovery, acrylic fade, accessibility and interruption: PASS");
    return ok ? 0 : 1;
}
