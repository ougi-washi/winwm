#include "win_platform/percent_slider.h"
#include <windowsx.h>

// All consumers lay the trackbar out at the configured action height. Derive
// both its painted thumb and its interactive endpoints from that same space.
typedef struct { RECT bounds; int pad, radius, half; } w_slider_geometry;
static const wchar_t preview_origin[] = L"WinwmSliderPreviewOrigin";

static w_slider_geometry w_geometry(HWND slider) {
    w_slider_geometry g={0};
    GetClientRect(slider,&g.bounds);
    int side=min(g.bounds.bottom,g.bounds.right);
    g.radius=max(1,MulDiv(side,6,32));
    g.pad=min(g.bounds.right/2,max(g.radius,MulDiv(side,10,32)));
    g.half=max(1,MulDiv(side,2,32));
    return g;
}

static LRESULT CALLBACK w_slider_input(HWND slider, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
    if (msg == WM_LBUTTONDOWN || ((msg == WM_MOUSEMOVE || msg == WM_LBUTTONUP) && GetCapture()==slider)) {
        if (msg == WM_LBUTTONDOWN) {
            if (!IsWindowEnabled(slider) || GetCapture()==slider) return 0;
            SetFocus(slider);
            if (!IsWindow(slider)) return 0;
            if (data==slider_preview) {
                // Offset zero so that a missing property always means no pending preview.
                INT_PTR origin=1+SendMessageW(slider,TBM_GETPOS,0,0);
                if (!SetPropW(slider,preview_origin,(HANDLE)origin)) return 0;
            }
            SetCapture(slider);
            if (GetCapture()!=slider) { RemovePropW(slider,preview_origin); return 0; }
        }
        w_slider_geometry g=w_geometry(slider);
        int value=w_clamp_int(MulDiv(GET_X_LPARAM(lp)-g.pad,100,max(1,g.bounds.right-2*g.pad)),0,100);
        if (SendMessageW(slider,TBM_GETPOS,0,0) != value) {
            SendMessageW(slider,TBM_SETPOS,TRUE,value);
            SendMessageW(GetParent(slider),WM_HSCROLL,MAKEWPARAM(TB_THUMBTRACK,value),(LPARAM)slider);
        }
        if (msg == WM_LBUTTONUP && GetCapture()==slider) {
            RemovePropW(slider,preview_origin);
            ReleaseCapture();
            if (IsWindow(slider)) SendMessageW(GetParent(slider),WM_HSCROLL,MAKEWPARAM(TB_ENDTRACK,0),(LPARAM)slider);
        }
        return 0;
    }
    if ((msg == WM_CANCELMODE || (msg==WM_ENABLE && !wp)) && GetCapture()==slider) ReleaseCapture();
    if (msg == WM_CAPTURECHANGED && data==slider_preview) {
        INT_PTR origin=(INT_PTR)RemovePropW(slider,preview_origin);
        if (origin && SendMessageW(slider,TBM_GETPOS,0,0)!=origin-1) {
            SendMessageW(slider,TBM_SETPOS,TRUE,origin-1);
            SendMessageW(GetParent(slider),WM_HSCROLL,MAKEWPARAM(TB_THUMBTRACK,origin-1),(LPARAM)slider);
        }
    }
    if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(slider,w_slider_input,id);
        RemovePropW(slider,preview_origin);
    }
    return DefSubclassProc(slider,msg,wp,lp);
}

bool w_percent_slider_init(HWND slider, w_slider_mode mode) {
    if (!slider) return false;
    if (!SetWindowSubclass(slider,w_slider_input,1,(DWORD_PTR)mode)) return false;
    SendMessageW(slider,TBM_SETRANGE,FALSE,MAKELPARAM(0,100));
    SendMessageW(slider,TBM_SETPAGESIZE,0,5);
    return true;
}

int w_percent_slider_sync(HWND slider, int value) {
    value=w_clamp_int(value,0,100);
    int shown=(int)SendMessageW(slider,TBM_GETPOS,0,0);
    if (GetPropW(slider,preview_origin)) {
        // A worker can finish during a later drag. Update the cancellation target
        // without moving the thumb under the user's pointer.
        if (SetPropW(slider,preview_origin,(HANDLE)(INT_PTR)(value+1))) return shown;
        SendMessageW(slider,WM_CANCELMODE,0,0);
        shown=(int)SendMessageW(slider,TBM_GETPOS,0,0);
    }
    if (shown!=value) SendMessageW(slider,TBM_SETPOS,TRUE,value);
    return value;
}

static COLORREF w_tint(COLORREF base,COLORREF color,int alpha) {
    return RGB(GetRValue(base)+MulDiv((int)GetRValue(color)-GetRValue(base),alpha,255),
               GetGValue(base)+MulDiv((int)GetGValue(color)-GetGValue(base),alpha,255),
               GetBValue(base)+MulDiv((int)GetBValue(color)-GetBValue(base),alpha,255));
}

LRESULT w_percent_slider_draw(HWND slider, NMCUSTOMDRAW *draw, COLORREF paper, COLORREF track_color,
                              COLORREF accent, COLORREF thumb, w_slider_feedback feedback) {
    if (draw->dwDrawStage != CDDS_PREPAINT) return CDRF_DODEFAULT;
    w_slider_geometry g=w_geometry(slider);
    RECT r=g.bounds;
    HDC dc = draw->hdc;
    int saved = SaveDC(dc);
    HBRUSH brush = GetStockObject(DC_BRUSH);
    SetDCBrushColor(dc,paper);
    FillRect(dc,&r,brush);
    int pad=g.pad, middle=r.bottom/2;
    RECT track={pad,middle-g.half,r.right-pad,middle+g.half};
    SetDCBrushColor(dc,track_color);
    FillRect(dc,&track,brush);
    if (IsWindowEnabled(slider)) {
        int percent = (int)SendMessageW(slider,TBM_GETPOS,0,0);
        track.right = pad+MulDiv(track.right-pad,percent,100);
        SetDCBrushColor(dc,accent);
        FillRect(dc,&track,brush);
        int radius=g.radius;
        SelectObject(dc,GetStockObject(NULL_PEN));
        SelectObject(dc,brush);
        int hover=GetCapture()==slider ? 255 : feedback.amount;
        if (hover) {
            int halo=min(g.pad,g.radius+max(1,MulDiv(3,GetDpiForWindow(slider),96)));
            int x=track.right;
            if (feedback.shadow && GetCapture()!=slider) {
                int drop=max(1,MulDiv(1,GetDpiForWindow(slider),96));
                SetDCBrushColor(dc,w_tint(paper,RGB(0,0,0),hover/3));
                Ellipse(dc,x-halo,middle-halo+drop,x+halo+1,middle+halo+drop+1);
            }
            SetDCBrushColor(dc,feedback.contrast ? GetSysColor(COLOR_HIGHLIGHT) : w_tint(paper,accent,hover/2));
            Ellipse(dc,x-halo,middle-halo,x+halo+1,middle+halo+1);
        }
        SetDCBrushColor(dc,thumb);
        Ellipse(dc,track.right-radius,middle-radius,track.right+radius+1,middle+radius+1);
        if (GetFocus()==slider && !(SendMessageW(slider,WM_QUERYUISTATE,0,0)&UISF_HIDEFOCUS))
            DrawFocusRect(dc,&r);
    }
    RestoreDC(dc,saved);
    return CDRF_SKIPDEFAULT;
}
