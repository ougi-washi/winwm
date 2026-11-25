#include "theme/theme.h"
#include "theme/control_hover.h"
#include "theme/icon_button.h"
#include "theme/caption.h"
#include "theme/hover.h"
#include <commctrl.h>
#include "win_platform/paint.h"
#include <windowsx.h>

static COLORREF w_disabled_text(void) {
    return w_theme_high_contrast() ? GetSysColor(COLOR_GRAYTEXT) :
           w_theme_mix(w_theme_background(),w_theme_foreground(),55);
}

static int w_luma(COLORREF color) {
    return 299*GetRValue(color)+587*GetGValue(color)+114*GetBValue(color);
}

static const wchar_t button_tracking[]=L"winwm.button.tracking";
enum { button_inside=1, button_buffered=2 };

static LRESULT CALLBACK w_motion_button(HWND button,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR flags) {
    bool inside=(flags&button_inside)!=0;
    if (msg==WM_MOUSEMOVE || msg==WM_LBUTTONDOWN || msg==WM_LBUTTONDBLCLK) {
        RECT r;
        GetClientRect(button,&r);
        bool hit=IsWindowEnabled(button) && PtInRect(&r,(POINT){GET_X_LPARAM(lp),GET_Y_LPARAM(lp)});
        if (hit && !inside) {
            TRACKMOUSEEVENT track={sizeof(track),TME_LEAVE,button,0};
            hit=TrackMouseEvent(&track)!=FALSE;
        }
        if (hit!=inside) SetWindowSubclass(button,w_motion_button,id,flags^button_inside);
        w_hover_target(button,hit ? 0 : -1,hit ? &r : NULL);
    } else if (msg==WM_MOUSELEAVE || (msg==WM_CAPTURECHANGED && lp)) {
        if (inside) {
            SetWindowSubclass(button,w_motion_button,id,flags&~(DWORD_PTR)button_inside);
            w_hover_target(button,-1,NULL);
        }
    } else if (msg==WM_WINDOWPOSCHANGED && inside &&
               (((WINDOWPOS *)lp)->flags&(SWP_NOMOVE|SWP_NOSIZE))!=(SWP_NOMOVE|SWP_NOSIZE)) {
        // Resize and DPI relayouts can change the hit area under a stationary pointer.
        POINT p;
        RECT r;
        GetClientRect(button,&r);
        bool hit=GetCursorPos(&p) && WindowFromPoint(p)==button && ScreenToClient(button,&p) && PtInRect(&r,p);
        if (!hit) SetWindowSubclass(button,w_motion_button,id,flags&~(DWORD_PTR)button_inside);
        w_hover_target(button,hit ? 0 : -1,hit ? &r : NULL);
    } else if (msg==WM_CANCELMODE || (msg==WM_ENABLE && !wp) || (msg==WM_SHOWWINDOW && !wp)) {
        if (inside) SetWindowSubclass(button,w_motion_button,id,flags&~(DWORD_PTR)button_inside);
        w_hover_clear(button);
    } else if (msg==WM_ERASEBKGND) return TRUE;
    else if (msg==WM_PAINT && !wp && (flags&button_buffered) && GetUpdateRect(button,NULL,FALSE)) {
        // Commit the face, shadow, glyph and focus cue together at each opacity.
        w_paint_frame frame;
        HDC dc=w_begin_frame(button,&frame);
        if (dc) DefSubclassProc(button,WM_PRINTCLIENT,(WPARAM)dc,PRF_CLIENT);
        w_end_frame(button,&frame);
        return 0;
    } else if (msg==WM_NCDESTROY) {
        RemovePropW(button,button_tracking);
        RemoveWindowSubclass(button,w_motion_button,id);
        if (flags&button_buffered) BufferedPaintUnInit();
    }
    return DefSubclassProc(button,msg,wp,lp);
}

// Install on first owner draw. This also covers plain buttons without an icon
// or tooltip, including the days in the calendar grid.
static int w_button_opacity(HWND button) {
    if (!GetPropW(button,button_tracking)) {
        bool buffered=SUCCEEDED(BufferedPaintInit());
        if (!SetPropW(button,button_tracking,(HANDLE)1) ||
            !SetWindowSubclass(button,w_motion_button,1,buffered ? button_buffered : 0)) {
            RemovePropW(button,button_tracking);
            if (buffered) BufferedPaintUnInit();
            return 0;
        }
    }
    return w_hover_amount(button,0);
}

COLORREF w_theme_button_face(const DRAWITEMSTRUCT *item,bool selected,bool flat) {
    UINT dpi=GetDpiForWindow(item->hwndItem);
    bool disabled=(item->itemState&(ODS_DISABLED|ODS_GRAYED))!=0;
    bool pressed=!disabled && (item->itemState&ODS_SELECTED)!=0;
    int amount=w_button_opacity(item->hwndItem);
    COLORREF surface=w_theme_background(), ink=w_theme_foreground(), active=w_theme_accent();
    bool contrast=w_theme_high_contrast();
    if (contrast && (item->itemState&ODS_HOTLIGHT)) amount=255;
    if (disabled) amount=0;
    COLORREF base=selected ? w_theme_selection(false) : flat ? surface : w_theme_mix(surface,ink,10);
    COLORREF hot=selected ? w_theme_mix(base,ink,14) : w_theme_selection(false);
    COLORREF caption=selected ? w_theme_selection(true) : ink;
    if (contrast) {
        base=GetSysColor(selected ? COLOR_HIGHLIGHT : COLOR_BTNFACE);
        hot=active=GetSysColor(COLOR_HIGHLIGHT);
        caption=GetSysColor(selected || amount || pressed ? COLOR_HIGHLIGHTTEXT : COLOR_BTNTEXT);
    } else if (pressed && abs(w_luma(active)-w_luma(surface))>abs(w_luma(active)-w_luma(ink))) caption=surface;
    HDC dc=item->hDC;
    RECT box=item->rcItem;
    FillRect(dc,&box,w_theme_brush());
    if (base!=surface || contrast) {
        int saved=SaveDC(dc);
        SelectObject(dc,GetStockObject(DC_BRUSH));
        SelectObject(dc,GetStockObject(DC_PEN));
        SetDCBrushColor(dc,base);
        SetDCPenColor(dc,contrast ? GetSysColor(disabled ? COLOR_GRAYTEXT : COLOR_BTNTEXT) : base);
        int curve=min(min(box.right-box.left,box.bottom-box.top),w_theme_control_radius(dpi)*2);
        if (curve>0) RoundRect(dc,box.left,box.top,box.right,box.bottom,curve,curve);
        else Rectangle(dc,box.left,box.top,box.right,box.bottom);
        RestoreDC(dc,saved);
    }
    w_hover_plate(dc,&box,dpi,base,pressed ? active : hot,amount,pressed);
    if (contrast && !disabled && (pressed || (selected && amount))) {
        int inset=max(1,MulDiv(pressed ? 2 : 1,dpi,96));
        InflateRect(&box,-inset,-inset);
        COLORREF old=SetDCBrushColor(dc,caption);
        FrameRect(dc,&box,GetStockObject(DC_BRUSH));
        SetDCBrushColor(dc,old);
    }
    return disabled ? w_disabled_text() : caption;
}

bool w_theme_message(HWND w, UINT msg, WPARAM wp, LPARAM lp, LRESULT *result) {
    *result = 0;
    if (w_theme_surface_message(w,msg,wp,lp)) return true;
    if (msg==WM_CREATE) w_theme_caption_install(w);
    if (msg == WM_MEASUREITEM && ((MEASUREITEMSTRUCT *)lp)->CtlType == ODT_LISTBOX) {
        ((MEASUREITEMSTRUCT *)lp)->itemHeight = w_theme_button_size(GetDpiForWindow(w));
        *result = TRUE;
        return true;
    } else if (msg == WM_ERASEBKGND) {
        RECT r;
        GetClientRect(w, &r);
        FillRect((HDC)wp, &r, w_theme_brush());
        *result = 1;
        return true;
    } else if (msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORLISTBOX || msg == WM_CTLCOLORSTATIC ||
               msg == WM_CTLCOLORBTN || msg == WM_CTLCOLORDLG) {
        SetBkColor((HDC)wp, w_theme_background());
        SetTextColor((HDC)wp, msg!=WM_CTLCOLORDLG && !IsWindowEnabled((HWND)lp)
                              ? w_disabled_text() : w_theme_foreground());
        *result = (LRESULT)w_theme_brush();
        return true;
    } else if (msg == WM_DRAWITEM) {
        COLORREF active=w_theme_accent();
        bool high_contrast=w_theme_high_contrast();
        DRAWITEMSTRUCT *item = (DRAWITEMSTRUCT *)lp;
        if (item->CtlType==ODT_COMBOBOX && w_native_draw_combo(item)) {
            *result=TRUE;
            return true;
        }
        if (item->CtlType == ODT_LISTBOX) {
            RECT row = item->rcItem;
            bool disabled = (item->itemState & (ODS_DISABLED|ODS_GRAYED)) != 0;
            bool selected = !disabled && (item->itemState & ODS_SELECTED) != 0;
            HDC dc = item->hDC;
            int saved=SaveDC(dc);
            if (item->itemID==(UINT)-1) {
                FillRect(dc,&row,w_theme_brush());
                RestoreDC(dc,saved);
                *result=TRUE;
                return true;
            }
            int amount=disabled ? 0 : w_hover_amount(item->hwndItem,item->itemID);
            COLORREF ink=w_hover_row(dc,&row,selected,amount,false);
            if (selected) {
                RECT mark = row;
                mark.right = mark.left + MulDiv(3, GetDpiForWindow(w), 96);
                SetDCBrushColor(dc, high_contrast ? w_theme_selection(true) : active);
                FillRect(dc, &mark, GetStockObject(DC_BRUSH));
            }
            wchar_t label[os_path_cap];
            LRESULT n = SendMessageW(item->hwndItem, LB_GETTEXTLEN, item->itemID, 0);
            if (n >= 0 && n < (LRESULT)count_of(label) &&
                SendMessageW(item->hwndItem, LB_GETTEXT, item->itemID, (LPARAM)label) != LB_ERR) {
                InflateRect(&row, -2*w_theme_spacing(GetDpiForWindow(w)), 0);
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc,disabled ? w_disabled_text() : ink);
                w_theme_draw_control_line(item->hwndItem,dc,label,&row,DT_LEFT|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
            }
            if ((item->itemState & (ODS_FOCUS | ODS_NOFOCUSRECT)) == ODS_FOCUS)
                DrawFocusRect(dc, &item->rcItem);
            RestoreDC(dc,saved);
            *result = TRUE;
            return true;
        }
        if (item->CtlType != ODT_BUTTON)
            return false;
        wchar_t text[96];
        GetWindowTextW(item->hwndItem, text, count_of(text));
        bool focused = (item->itemState & (ODS_FOCUS|ODS_NOFOCUSRECT)) == ODS_FOCUS;
        RECT box = item->rcItem;
        int saved = SaveDC(item->hDC);
        HFONT button_font=(HFONT)SendMessageW(item->hwndItem,WM_GETFONT,0,0);
        if (button_font) SelectObject(item->hDC,button_font);
        COLORREF caption=w_theme_button_face(item,false,w_icon_button_compact(item->hwndItem));
        SetBkMode(item->hDC, TRANSPARENT);
        SetTextColor(item->hDC,caption);
        if (!w_icon_button_draw(item))
            w_theme_draw_fitted_line(item->hDC,text,-1,&item->rcItem,DT_CENTER|DT_VCENTER|
                      DT_END_ELLIPSIS | ((item->itemState & ODS_NOACCEL) ? DT_HIDEPREFIX : 0));
        if (focused) {
            int inset=max(1,w_theme_spacing(GetDpiForWindow(item->hwndItem))/2);
            InflateRect(&box,-inset,-inset);
            DrawFocusRect(item->hDC, &box);
        }
        RestoreDC(item->hDC, saved);
        *result = TRUE;
        return true;
    }
    return false;
}
