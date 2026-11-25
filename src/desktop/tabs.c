#include "desktop/desktop.h"
#include "theme/theme.h"
#include <commctrl.h>
#include <stdlib.h>
#include <windowsx.h>

enum { tab_activate = WM_APP + 1 };

typedef struct w_tab_item {
    w_window_id window;
    unsigned title_revision;
} w_tab_item;

typedef struct w_tab_strip {
    HWND host, control;
    int output, count, hot, dpi, width, height;
    COLORREF background, foreground, accent;
    HFONT font;
    bool syncing, queued;
    w_window_id requested;
    w_tab_item items[window_limit];
} w_tab_strip;

static void w_request_tab(w_tab_strip *strip, int index) {
    if (strip->syncing || (unsigned)index >= (unsigned)strip->count)
        return;
    strip->requested = strip->items[index].window;
    if (!strip->queued)
        strip->queued = PostMessageW(strip->host, tab_activate, 0, 0) != 0;
}

static void w_activate_tab(w_tab_strip *strip) {
    strip->queued = false;
    w_window_id id = strip->requested;
    if (!w_model_valid(&desk.model, id))
        return;
    w_model_window *window = &desk.model.windows[id.slot];
    int o = strip->output, ws = desk.model.selected[o];
    if (desk.outputs[o].tabs == strip->host && window->output == o && window->workspace == ws &&
        desk.model.spaces[o][ws].layout == layout_tabs && !window->floating &&
        w_model_visible(&desk.model, id.slot))
        w_desktop_activate(id.slot);
}

static void w_paint_tabs(w_tab_strip *strip, HDC dc) {
    RECT client, clip;
    GetClientRect(strip->control, &client);
    GetClipBox(dc, &clip);
    FillRect(dc, &clip, w_theme_brush());
    int saved = SaveDC(dc);
    SelectObject(dc, strip->font ? strip->font : GetStockObject(DEFAULT_GUI_FONT));
    SetBkMode(dc, TRANSPARENT);
    HBRUSH brush = GetStockObject(DC_BRUSH);
    int selected = TabCtrl_GetCurSel(strip->control);
    int padding = MulDiv(10, strip->dpi, 96);
    for (int i = 0; i < strip->count; ++i) {
        RECT box;
        if (!TabCtrl_GetItemRect(strip->control, i, &box) || box.right <= clip.left || box.left >= clip.right)
            continue;
        box.top = client.top;
        box.bottom = client.bottom;
        bool active = i == selected;
        if (active || i == strip->hot) {
            SetDCBrushColor(dc, w_theme_selection(false));
            FillRect(dc, &box, brush);
        }
        if (active) {
            RECT line = box;
            line.bottom = min(box.bottom, box.top + max(1, MulDiv(2, strip->dpi, 96)));
            SetDCBrushColor(dc, w_theme_accent());
            FillRect(dc, &line, brush);
        }
        if (i + 1 < strip->count) {
            RECT divider = {box.right - 1, box.top + box.bottom / 4, box.right, box.bottom - box.bottom / 4};
            SetDCBrushColor(dc, w_theme_separator());
            FillRect(dc, &divider, brush);
        }
        w_window_id id = strip->items[i].window;
        if (!w_model_valid(&desk.model, id))
            continue;
        SetTextColor(dc, active || i == strip->hot ? w_theme_selection(true) : w_theme_foreground());
        InflateRect(&box, -padding, 0);
        DrawTextW(dc, desk.windows[id.slot].tab_title, -1, &box,
                  DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    RestoreDC(dc, saved);
}

static LRESULT CALLBACK w_tab_control(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR subclass,
                                      DWORD_PTR data) {
    (void)subclass;
    w_tab_strip *strip = (w_tab_strip *)data;
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(hwnd, &paint);
        w_paint_tabs(strip, dc);
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_PRINTCLIENT:
        w_paint_tabs(strip, (HDC)wp);
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_MOUSEMOVE: {
        TCHITTESTINFO hit = {.pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}};
        int index = TabCtrl_HitTest(hwnd, &hit);
        if (index != strip->hot) {
            strip->hot = index;
            TRACKMOUSEEVENT leave = {.cbSize = sizeof(leave), .dwFlags = TME_LEAVE, .hwndTrack = hwnd};
            TrackMouseEvent(&leave);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        break;
    }
    case WM_MOUSELEAVE:
        strip->hot = -1;
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK w_tab_host(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    w_tab_strip *strip = (w_tab_strip *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW *create = (CREATESTRUCTW *)lp;
        strip = calloc(1, sizeof(*strip));
        if (!strip)
            return -1;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)strip);
        strip->host = hwnd;
        strip->output = (int)(INT_PTR)create->lpCreateParams;
        strip->hot = -1;
        strip->control = CreateWindowExW(WS_EX_NOACTIVATE, WC_TABCONTROLW, L"Window tabs",
            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | TCS_FIXEDWIDTH | TCS_FOCUSNEVER | TCS_TOOLTIPS,
            0, 0, create->cx, create->cy, hwnd, NULL, create->hInstance, NULL);
        if (!strip->control || !SetWindowSubclass(strip->control, w_tab_control, 1, (DWORD_PTR)strip))
            return -1;
        return 0;
    }
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_SIZE:
        if (strip && strip->control)
            SetWindowPos(strip->control, NULL, 0, 0, LOWORD(lp), HIWORD(lp), SWP_NOACTIVATE | SWP_NOZORDER);
        return 0;
    case WM_NOTIFY: {
        NMHDR *note = (NMHDR *)lp;
        if (!strip)
            break;
        if (note->hwndFrom == strip->control) {
            if (note->code == TCN_SELCHANGE)
                w_request_tab(strip, TabCtrl_GetCurSel(strip->control));
            if (note->code == NM_CLICK) {
                POINT pt;
                if (GetCursorPos(&pt) && ScreenToClient(strip->control, &pt)) {
                    TCHITTESTINFO hit = {.pt = pt};
                    int index = TabCtrl_HitTest(strip->control, &hit);
                    if (index == TabCtrl_GetCurSel(strip->control))
                        w_request_tab(strip, index);
                }
            }
        } else if (note->code == TTN_GETDISPINFOW && note->idFrom < (UINT_PTR)strip->count) {
            w_window_id id = strip->items[note->idFrom].window;
            if (w_model_valid(&desk.model, id))
                ((NMTTDISPINFOW *)lp)->lpszText = desk.windows[id.slot].tab_title;
        }
        return 0;
    }
    case tab_activate:
        if (strip)
            w_activate_tab(strip);
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        free(strip);
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HWND w_create_tabs(int output, w_rect bounds) {
    INITCOMMONCONTROLSEX controls = {.dwSize = sizeof(controls), .dwICC = ICC_TAB_CLASSES | ICC_UPDOWN_CLASS};
    if (!InitCommonControlsEx(&controls))
        return NULL;
    WNDCLASSW cls = {.hInstance = GetModuleHandleW(NULL), .lpfnWndProc = w_tab_host,
        .lpszClassName = L"WinwmTabs", .hCursor = LoadCursorW(NULL, IDC_ARROW)};
    if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return NULL;
    return CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, cls.lpszClassName, L"Window tabs",
        WS_POPUP | WS_CLIPCHILDREN, bounds.x, bounds.y, bounds.w, bounds.h, NULL, NULL, cls.hInstance,
        (void *)(INT_PTR)output);
}

HWND w_desktop_tabs_update(int output, const int *slots, int count, int selected, w_rect bounds) {
    w_output_state *o = &desk.outputs[output];
    HWND host = o->tabs;
    if (!count || bounds.w < 1 || bounds.h < 1) {
        if (host)
            DestroyWindow(host);
        return NULL;
    }
    if (!host && !(host = w_create_tabs(output, bounds)))
        return NULL;
    w_tab_strip *strip = (w_tab_strip *)GetWindowLongPtrW(host, GWLP_USERDATA);
    HWND control = strip->control;
    strip->syncing = true;
    bool dimensions = strip->dpi != o->dpi || strip->width != bounds.w || strip->height != bounds.h ||
                      strip->count != count;
    bool repaint = dimensions || strip->background != w_theme_background() ||
                   strip->foreground != w_theme_foreground() || strip->accent != w_theme_accent();
    if (strip->font != o->bar.font || strip->dpi != o->dpi) {
        strip->font = o->bar.font;
        SendMessageW(control, WM_SETFONT, (WPARAM)strip->font, FALSE);
        repaint = true;
    }
    strip->dpi = o->dpi;
    strip->width = bounds.w;
    strip->height = bounds.h;
    strip->background = w_theme_background();
    strip->foreground = w_theme_foreground();
    strip->accent = w_theme_accent();
    while (strip->count > count)
        TabCtrl_DeleteItem(control, --strip->count);
    int current = -1;
    for (int i = 0; i < count; ++i) {
        int slot = slots[i];
        w_window_id id = {slot, desk.model.windows[slot].generation};
        unsigned revision = desk.windows[slot].tab_title_revision;
        w_tab_item *item = &strip->items[i];
        if (slot == selected)
            current = i;
        if (i < strip->count && item->window.slot == slot && item->window.generation == id.generation &&
            item->title_revision == revision)
            continue;
        // The native control parses ampersands as mnemonics in its accessible name.
        wchar_t name[2 * count_of(desk.windows[slot].tab_title)], *end = name;
        for (const wchar_t *p = desk.windows[slot].tab_title; *p; ++p) {
            if (*p == L'&') *end++ = L'&';
            *end++ = *p;
        }
        *end = 0;
        TCITEMW tab = {.mask = TCIF_TEXT, .pszText = name};
        if (i == strip->count) {
            if (TabCtrl_InsertItem(control, i, &tab) < 0) {
                DestroyWindow(host);
                return NULL;
            }
            ++strip->count;
        } else {
            TabCtrl_SetItem(control, i, &tab);
        }
        *item = (w_tab_item){id, revision};
        repaint = true;
    }
    bool shown = IsWindowVisible(host) != 0;
    RECT rect;
    if (!GetWindowRect(host, &rect) || rect.left != bounds.x || rect.top != bounds.y ||
        rect.right != bounds.x + bounds.w || rect.bottom != bounds.y + bounds.h)
        SetWindowPos(host, NULL, bounds.x, bounds.y, bounds.w, bounds.h, SWP_NOACTIVATE | SWP_NOZORDER);
    if (dimensions) {
        int width = max(MulDiv(80, o->dpi, 96), (bounds.w - 4) / count);
        TabCtrl_SetItemSize(control, min(width, 32767), max(1, bounds.h - 2));
        // Item-size changes reset native scrolling. Reapply the selected item
        // after layout so that its caption stays on screen at the new width.
        TabCtrl_SetCurSel(control, -1);
    }
    if (dimensions || current != TabCtrl_GetCurSel(control)) {
        TabCtrl_SetCurSel(control, current);
        repaint = true;
    }
    if (repaint)
        InvalidateRect(control, NULL, FALSE);
    strip->syncing = false;
    if (!shown)
        SetWindowPos(host, HWND_TOP, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    return host;
}
