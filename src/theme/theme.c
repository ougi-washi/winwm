#include "theme/theme.h"
#include <dwmapi.h>
#include <richedit.h>

////////////////
//~ Theme

static COLORREF surface = RGB(16, 17, 22), ink = RGB(228, 229, 234), active = RGB(145, 170, 247);
static HBRUSH brush;

////////////////
//~ Theme Functions

static COLORREF w_color(uint32_t rgb) {
    return RGB(rgb >> 16, rgb >> 8, rgb);
}

HBRUSH w_theme_brush(void) {
    if (!brush)
        brush = CreateSolidBrush(surface);
    return brush;
}

void w_theme_colors(uint32_t background, uint32_t foreground, uint32_t accent) {
    if (w_color(background) != surface && brush) {
        DeleteObject(brush);
        brush = NULL;
    }
    surface = w_color(background);
    ink = w_color(foreground);
    active = w_color(accent);
}

void w_theme_rich_edit(HWND edit) {
    CHARFORMAT2W format = {.cbSize = sizeof(format), .dwMask = CFM_COLOR, .crTextColor = ink};
    SendMessageW(edit, EM_SETBKGNDCOLOR, 0, surface);
    SendMessageW(edit, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&format);
}

bool w_theme_message(HWND w, UINT msg, WPARAM wp, LPARAM lp, LRESULT *result) {
    *result = 0;
    if (msg == WM_CREATE) {
        BOOL enabled = TRUE;
        DwmSetWindowAttribute(w, 20, &enabled, sizeof(enabled));
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
        if (item->CtlType != ODT_BUTTON)
            return false;
        wchar_t text[96];
        GetWindowTextW(item->hwndItem, text, count_of(text));
        bool selected = (item->itemState & (ODS_SELECTED | ODS_FOCUS)) != 0;
        SetDCBrushColor(item->hDC, selected ? active : RGB(41, 46, 66));
        FillRect(item->hDC, &item->rcItem, (HBRUSH)GetStockObject(DC_BRUSH));
        SetBkMode(item->hDC, TRANSPARENT);
        SetTextColor(item->hDC, selected ? surface : ink);
        DrawTextW(item->hDC, text, -1, &item->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        *result = TRUE;
        return true;
    }
    return false;
}
