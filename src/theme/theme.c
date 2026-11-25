#include "theme/theme.h"
#include <dwmapi.h>

////////////////
//~ Theme

static COLORREF surface = RGB(16, 17, 22), ink = RGB(228, 229, 234), active = RGB(145, 170, 247);
static HBRUSH brush;
static bool high_contrast;

////////////////
//~ Theme Functions

static COLORREF w_color(uint32_t rgb) {
    return RGB(rgb >> 16, rgb >> 8, rgb);
}

static COLORREF w_tint(COLORREF a, COLORREF b, int percent) {
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

COLORREF w_theme_separator(void) {
    return w_tint(surface, active, 55);
}

COLORREF w_theme_accent(void) {
    return high_contrast ? ink : active;
}

COLORREF w_theme_hint(void) {
    return high_contrast ? ink : w_tint(surface, ink, 72);
}

COLORREF w_theme_selection(bool foreground) {
    if (high_contrast)
        return GetSysColor(foreground ? COLOR_HIGHLIGHTTEXT : COLOR_HIGHLIGHT);
    return foreground ? ink : w_tint(surface, active, 24);
}

void w_theme_colors(uint32_t background, uint32_t foreground, uint32_t accent) {
    HIGHCONTRASTW hc = {.cbSize = sizeof(hc)};
    SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0);
    bool contrast = (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
    high_contrast = contrast;
    COLORREF next = contrast ? GetSysColor(COLOR_WINDOW) : w_color(background);
    if (next != surface && brush) {
        DeleteObject(brush);
        brush = NULL;
    }
    surface = next;
    ink = contrast ? GetSysColor(COLOR_WINDOWTEXT) : w_color(foreground);
    active = contrast ? GetSysColor(COLOR_HIGHLIGHT) : w_color(accent);
}

bool w_theme_message(HWND w, UINT msg, WPARAM wp, LPARAM lp, LRESULT *result) {
    *result = 0;
    if (msg == WM_CREATE) {
        BOOL enabled = TRUE;
        DwmSetWindowAttribute(w, 20, &enabled, sizeof(enabled));
    } else if (msg == WM_ACTIVATE) {
        COLORREF edge = LOWORD(wp) == WA_INACTIVE ? w_tint(surface, ink, 24) : active;
        DwmSetWindowAttribute(w, 34, &edge, sizeof(edge));
    } else if (msg == WM_MEASUREITEM && ((MEASUREITEMSTRUCT *)lp)->CtlType == ODT_LISTBOX) {
        ((MEASUREITEMSTRUCT *)lp)->itemHeight = MulDiv(30, GetDpiForWindow(w), 96);
        *result = TRUE;
        return true;
    } else if (msg == WM_ERASEBKGND) {
        RECT r;
        GetClientRect(w, &r);
        FillRect((HDC)wp, &r, w_theme_brush());
        *result = 1;
        return true;
    } else if (msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORLISTBOX || msg == WM_CTLCOLORSTATIC) {
        SetBkColor((HDC)wp, surface);
        SetTextColor((HDC)wp, ink);
        *result = (LRESULT)w_theme_brush();
        return true;
    } else if (msg == WM_DRAWITEM) {
        DRAWITEMSTRUCT *item = (DRAWITEMSTRUCT *)lp;
        if (item->CtlType == ODT_LISTBOX) {
            RECT row = item->rcItem;
            bool selected = (item->itemState & ODS_SELECTED) != 0;
            HDC dc = item->hDC;
            SetDCBrushColor(dc, selected ? w_theme_selection(false) : surface);
            FillRect(dc, &row, GetStockObject(DC_BRUSH));
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
                InflateRect(&row, -MulDiv(12, GetDpiForWindow(w), 96), 0);
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, selected ? w_theme_selection(true) : ink);
                DrawTextW(dc, label, -1, &row, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            }
            if ((item->itemState & (ODS_FOCUS | ODS_NOFOCUSRECT)) == ODS_FOCUS)
                DrawFocusRect(dc, &item->rcItem);
            *result = TRUE;
            return true;
        }
        if (item->CtlType != ODT_BUTTON)
            return false;
        wchar_t text[96];
        GetWindowTextW(item->hwndItem, text, count_of(text));
        bool pressed = (item->itemState & ODS_SELECTED) != 0;
        bool focused = (item->itemState & ODS_FOCUS) != 0;
        RECT box = item->rcItem;
        FillRect(item->hDC, &box, w_theme_brush());
        int saved = SaveDC(item->hDC);
        SelectObject(item->hDC, GetStockObject(DC_BRUSH));
        SelectObject(item->hDC, GetStockObject(DC_PEN));
        COLORREF fill = pressed ? active : w_tint(surface, focused ? active : ink, 11);
        COLORREF edge = pressed || focused ? active : w_tint(surface, ink, 28);
        SetDCBrushColor(item->hDC, high_contrast ? GetSysColor(pressed ? COLOR_HIGHLIGHT : COLOR_BTNFACE) : fill);
        SetDCPenColor(item->hDC, high_contrast ? GetSysColor(COLOR_BTNTEXT) : edge);
        int curve = MulDiv(9, GetDpiForWindow(w), 96);
        RoundRect(item->hDC, box.left, box.top, box.right, box.bottom, curve, curve);
        SetBkMode(item->hDC, TRANSPARENT);
        COLORREF caption = high_contrast ? GetSysColor(pressed ? COLOR_HIGHLIGHTTEXT : COLOR_BTNTEXT) : pressed ? surface : ink;
        SetTextColor(item->hDC, (item->itemState & ODS_DISABLED) ? GetSysColor(COLOR_GRAYTEXT) : caption);
        DrawTextW(item->hDC, text, -1, &item->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE |
                  ((item->itemState & ODS_NOACCEL) ? DT_HIDEPREFIX : 0));
        if (focused && !(item->itemState & ODS_NOFOCUSRECT)) {
            InflateRect(&box, -3, -3);
            DrawFocusRect(item->hDC, &box);
        }
        RestoreDC(item->hDC, saved);
        *result = TRUE;
        return true;
    }
    return false;
}
