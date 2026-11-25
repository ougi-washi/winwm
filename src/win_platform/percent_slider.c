#include "win_platform/percent_slider.h"
#include <windowsx.h>

static LRESULT CALLBACK w_slider_input(HWND slider, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
    (void)data;
    if (msg == WM_LBUTTONDOWN || ((msg == WM_MOUSEMOVE || msg == WM_LBUTTONUP) && GetCapture() == slider)) {
        if (msg == WM_LBUTTONDOWN) {
            SetFocus(slider);
            SetCapture(slider);
        }
        RECT r;
        GetClientRect(slider,&r);
        int pad = min(r.right/2,MulDiv(10,GetDpiForWindow(slider),96));
        int value = w_clamp_int(MulDiv(GET_X_LPARAM(lp)-pad,100,max(1,r.right-2*pad)),0,100);
        if (SendMessageW(slider,TBM_GETPOS,0,0) != value) {
            SendMessageW(slider,TBM_SETPOS,TRUE,value);
            SendMessageW(GetParent(slider),WM_HSCROLL,MAKEWPARAM(TB_THUMBTRACK,value),(LPARAM)slider);
        }
        if (msg == WM_LBUTTONUP && GetCapture()==slider) ReleaseCapture();
        return 0;
    }
    if (msg == WM_CANCELMODE && GetCapture()==slider) ReleaseCapture();
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(slider,w_slider_input,id);
    return DefSubclassProc(slider,msg,wp,lp);
}

bool w_percent_slider_init(HWND slider) {
    if (!slider || !SetWindowSubclass(slider,w_slider_input,1,0)) return false;
    SendMessageW(slider,TBM_SETRANGE,FALSE,MAKELPARAM(0,100));
    SendMessageW(slider,TBM_SETPAGESIZE,0,5);
    return true;
}

LRESULT w_percent_slider_draw(HWND slider, NMCUSTOMDRAW *draw, COLORREF paper, COLORREF track_color,
                              COLORREF accent, COLORREF thumb) {
    if (draw->dwDrawStage != CDDS_PREPAINT) return CDRF_DODEFAULT;
    RECT r;
    GetClientRect(slider,&r);
    HDC dc = draw->hdc;
    int saved = SaveDC(dc);
    HBRUSH brush = GetStockObject(DC_BRUSH);
    SetDCBrushColor(dc,paper);
    FillRect(dc,&r,brush);
    UINT dpi = GetDpiForWindow(slider);
    int pad = min(r.right/2,MulDiv(10,dpi,96)), middle = r.bottom/2, half = max(1,MulDiv(2,dpi,96));
    RECT track = {pad,middle-half,r.right-pad,middle+half};
    SetDCBrushColor(dc,track_color);
    FillRect(dc,&track,brush);
    if (IsWindowEnabled(slider)) {
        int percent = (int)SendMessageW(slider,TBM_GETPOS,0,0);
        track.right = pad+MulDiv(track.right-pad,percent,100);
        SetDCBrushColor(dc,accent);
        FillRect(dc,&track,brush);
        int radius = max(1,MulDiv(6,dpi,96));
        SelectObject(dc,GetStockObject(NULL_PEN));
        SelectObject(dc,brush);
        SetDCBrushColor(dc,thumb);
        Ellipse(dc,track.right-radius,middle-radius,track.right+radius+1,middle+radius+1);
        if (GetFocus()==slider && !(SendMessageW(slider,WM_QUERYUISTATE,0,0)&UISF_HIDEFOCUS))
            DrawFocusRect(dc,&r);
    }
    RestoreDC(dc,saved);
    return CDRF_SKIPDEFAULT;
}
