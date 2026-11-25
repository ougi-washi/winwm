#include "theme/native_controls.h"
#include "desktop/desktop.h"
#include "theme/icon_button.h"
#include "theme/hover.h"
#include "win_platform/paint.h"
#include "theme/theme.h"
#include <commctrl.h>
#include <stdlib.h>
#include <windowsx.h>

enum { tab_previous = 1, tab_next, tab_activate = WM_APP + 1 };

typedef struct w_tab_item {
    w_window_id window;
    unsigned title_revision;
} w_tab_item;

typedef struct w_tab_measure {
    uint32_t generation;
    unsigned revision;
    int width;
} w_tab_measure;

typedef struct w_tab_strip {
    HWND host, control, previous, next;
    int output, count, first, visible, rendered, selected, dpi, width, height, side, wheel_delta;
    COLORREF background, foreground, accent;
    HFONT font;
    int font_dpi, gap;
    bool syncing, queued, single_arrow, pointer_inside;
    POINT pointer;
    w_window_id requested;
    w_window_id items[window_limit];
    w_tab_measure labels[window_limit];
    w_tab_item rows[window_limit];
} w_tab_strip;

static void w_request_tab(w_tab_strip *strip, int index) {
    if (strip->syncing || (unsigned)index >= (unsigned)strip->count)
        return;
    strip->requested = strip->items[index];
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

static void w_invalidate_item(w_tab_strip *strip, int index) {
    RECT box;
    if (index >= 0 && TabCtrl_GetItemRect(strip->control, index, &box)) {
        box.top = 0;
        box.bottom = strip->height;
        InvalidateRect(strip->control, &box, FALSE);
    }
}

static bool w_same_tab(w_window_id left, w_window_id right) {
    return left.slot == right.slot && left.generation == right.generation;
}

static INT_PTR w_tab_key(w_window_id id) {
    return (INT_PTR)id.generation*window_limit+id.slot;
}

static void w_tab_hover(w_tab_strip *strip) {
    RECT box;
    TCHITTESTINFO hit={.pt=strip->pointer};
    int row=strip->pointer_inside ? TabCtrl_HitTest(strip->control,&hit) : -1;
    if (row>=0 && row<strip->rendered && TabCtrl_GetItemRect(strip->control,row,&box)) {
        box.top=0;
        box.bottom=strip->height;
        w_hover_target(strip->control,w_tab_key(strip->rows[row].window),&box);
    } else w_hover_target(strip->control,-1,NULL);
}

static bool w_tab_bounds(HWND control,INT_PTR key,RECT *bounds,void *data) {
    w_tab_strip *strip=data;
    for (int row=0;row<strip->rendered;++row) {
        if (w_tab_key(strip->rows[row].window)!=key) continue;
        if (!TabCtrl_GetItemRect(control,row,bounds)) return false;
        bounds->top=0;
        bounds->bottom=strip->height;
        return true;
    }
    return false;
}

static bool w_render_page(w_tab_strip *strip) {
    HWND control = strip->control;
    while (strip->rendered > strip->visible)
        TabCtrl_DeleteItem(control, --strip->rendered);
    for (int i = 0; i < strip->visible; ++i) {
        w_window_id id = strip->items[strip->first + i];
        w_tab_item *row = &strip->rows[i];
        unsigned revision = desk.windows[id.slot].tab_title_revision;
        if (i < strip->rendered && w_same_tab(row->window, id) && row->title_revision == revision)
            continue;
        // The native control parses ampersands as mnemonics in its accessible name.
        wchar_t name[2 * count_of(desk.windows[id.slot].tab_title)], *end = name;
        for (const wchar_t *p = desk.windows[id.slot].tab_title; *p; ++p) {
            if (*p == L'&') *end++ = L'&';
            *end++ = *p;
        }
        *end = 0;
        TCITEMW tab = {.mask = TCIF_TEXT, .pszText = name};
        if (i == strip->rendered) {
            if (TabCtrl_InsertItem(control, i, &tab) < 0) return false;
            ++strip->rendered;
        } else if (!TabCtrl_SetItem(control, i, &tab)) return false;
        *row = (w_tab_item){id, revision};
        w_invalidate_item(strip, i);
    }
    int current = strip->selected - strip->first;
    if (current < 0 || current >= strip->visible) current = -1;
    int before = TabCtrl_GetCurSel(control);
    if (before != current) {
        TabCtrl_SetCurSel(control, current);
        w_invalidate_item(strip, before);
        w_invalidate_item(strip, current);
    }
    bool earlier = strip->first > 0, later = strip->first + strip->visible < strip->count;
    if (!!IsWindowEnabled(strip->previous) != earlier) EnableWindow(strip->previous, earlier);
    if (!!IsWindowEnabled(strip->next) != (strip->single_arrow || later))
        EnableWindow(strip->next, strip->single_arrow || later);
    w_hover_relayout(control,w_tab_bounds,strip);
    if (strip->pointer_inside) w_tab_hover(strip);
    return true;
}

static void w_scroll_tabs(w_tab_strip *strip, int direction) {
    int last = strip->count - strip->visible;
    int first = strip->first + direction;
    if (strip->single_arrow) {
        first %= last + 1;
        if (first < 0) first += last + 1;
    }
    first = max(0, min(first, last));
    if (strip->first != first) {
        strip->first = first;
        w_render_page(strip);
    }
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
    int padding = 2*w_theme_spacing(strip->dpi);
    for (int i = 0; i < strip->rendered; ++i) {
        RECT box;
        if (!TabCtrl_GetItemRect(strip->control, i, &box) || box.right <= clip.left || box.left >= clip.right)
            continue;
        box.top = client.top;
        box.bottom = client.bottom;
        bool active = i == selected;
        int hover=w_hover_amount(strip->control,w_tab_key(strip->rows[i].window));
        COLORREF ink=w_hover_row(dc,&box,active,hover,false);
        if (active) {
            RECT line = box;
            line.bottom = min(box.bottom, box.top + max(1, MulDiv(2, strip->dpi, 96)));
            SetDCBrushColor(dc, w_theme_high_contrast() ? ink : w_theme_accent());
            FillRect(dc, &line, brush);
        }
        if (i + 1 < strip->rendered) {
            int inset = (box.bottom-box.top)/4;
            RECT divider = {box.right - 1, box.top + inset, box.right, box.bottom - inset};
            SetDCBrushColor(dc, w_theme_separator());
            FillRect(dc, &divider, brush);
        }
        w_window_id id = strip->rows[i].window;
        if (!w_model_valid(&desk.model, id))
            continue;
        SetTextColor(dc,ink);
        InflateRect(&box, -padding, 0);
        w_theme_draw_fitted_line(dc, desk.windows[id.slot].tab_title, -1, &box,
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
        w_paint_frame frame;
        HDC dc=w_begin_frame(hwnd,&frame);
        if (dc) w_paint_tabs(strip,dc);
        w_end_frame(hwnd,&frame);
        return 0;
    }
    case WM_PRINTCLIENT:
        w_paint_tabs(strip, (HDC)wp);
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_MOUSEWHEEL:
        strip->wheel_delta += GET_WHEEL_DELTA_WPARAM(wp);
        w_scroll_tabs(strip, -strip->wheel_delta / WHEEL_DELTA);
        strip->wheel_delta %= WHEEL_DELTA;
        return 0;
    case WM_KEYDOWN: {
        int index = strip->selected;
        if (strip->queued) {
            for (int i = 0; i < strip->count; ++i)
                if (w_same_tab(strip->requested, strip->items[i])) { index = i; break; }
        }
        if (wp == VK_HOME) index = 0;
        else if (wp == VK_END) index = strip->count - 1;
        else if (wp == VK_LEFT) index = max(0, index - 1);
        else if (wp == VK_RIGHT) index = min(strip->count - 1, index + 1);
        else break;
        w_request_tab(strip, index);
        return 0;
    }
    case WM_MOUSEMOVE: {
        strip->pointer=(POINT){GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
        if (!strip->pointer_inside) {
            TRACKMOUSEEVENT leave = {.cbSize = sizeof(leave), .dwFlags = TME_LEAVE, .hwndTrack = hwnd};
            strip->pointer_inside=TrackMouseEvent(&leave)!=FALSE;
        }
        w_tab_hover(strip);
        break;
    }
    case WM_MOUSELEAVE:
        strip->pointer_inside=false;
        w_tab_hover(strip);
        break;
    case WM_SHOWWINDOW:
    case WM_ENABLE:
        if (!wp) { strip->pointer_inside=false; w_hover_clear(hwnd); }
        break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK w_tab_host(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    w_tab_strip *strip = (w_tab_strip *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (strip && (msg==WM_FONTCHANGE || (msg==WM_SETTINGCHANGE && wp==SPI_SETNONCLIENTMETRICS))) {
        strip->font_dpi=0;
        if (desk.root) w_desktop_schedule();
    }
    LRESULT result;
    if (w_theme_message(hwnd, msg, wp, lp, &result)) return result;
    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW *create = (CREATESTRUCTW *)lp;
        strip = calloc(1, sizeof(*strip));
        if (!strip)
            return -1;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)strip);
        strip->host = hwnd;
        strip->output = (int)(INT_PTR)create->lpCreateParams;
        strip->selected = -1;
        strip->control = CreateWindowExW(WS_EX_NOACTIVATE, WC_TABCONTROLW, L"Window tabs",
            WS_CHILD | WS_VISIBLE | TCS_FIXEDWIDTH | TCS_MULTILINE | TCS_RAGGEDRIGHT | TCS_FOCUSNEVER | TCS_TOOLTIPS,
            0, 0, create->cx, create->cy, hwnd, NULL, create->hInstance, NULL);
        if (!strip->control || !SetWindowSubclass(strip->control, w_tab_control, 1, (DWORD_PTR)strip))
            return -1;
        DWORD style = WS_CHILD | WS_TABSTOP | BS_OWNERDRAW;
        strip->previous = CreateWindowW(L"BUTTON", NULL, style, 0, 0, 0, 0, hwnd, (HMENU)tab_previous, create->hInstance, NULL);
        strip->next = CreateWindowW(L"BUTTON", NULL, style, 0, 0, 0, 0, hwnd, (HMENU)tab_next, create->hInstance, NULL);
        if (!strip->previous || !strip->next ||
            !w_icon_button(strip->previous, L"\xe76b", L"Scroll tabs left") ||
            !w_icon_button(strip->next, L"\xe76c", L"Scroll tabs right"))
            return -1;
        HWND tip=TabCtrl_GetToolTips(strip->control);
        if (tip) w_theme_material(tip);
        return 0;
    }
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_COMMAND:
        if (strip && HIWORD(wp) == BN_CLICKED && ((HWND)lp == strip->previous || (HWND)lp == strip->next)) {
            w_scroll_tabs(strip, (HWND)lp == strip->previous ? -1 : 1);
            return 0;
        }
        break;
    case WM_NOTIFY: {
        NMHDR *note = (NMHDR *)lp;
        if (!strip)
            break;
        if (note->hwndFrom == strip->control) {
            if (note->code == TCN_SELCHANGE) {
                int index = TabCtrl_GetCurSel(strip->control);
                if (index >= 0) w_request_tab(strip, strip->first + index);
            }
            if (note->code == NM_CLICK) {
                POINT pt;
                if (GetCursorPos(&pt) && ScreenToClient(strip->control, &pt)) {
                    TCHITTESTINFO hit = {.pt = pt};
                    int index = TabCtrl_HitTest(strip->control, &hit);
                    if (index >= 0 && index == TabCtrl_GetCurSel(strip->control))
                        w_request_tab(strip, strip->first + index);
                }
            }
        } else if (note->code == TTN_GETDISPINFOW && note->idFrom < (UINT_PTR)strip->rendered) {
            w_window_id id = strip->rows[note->idFrom].window;
            if (w_model_valid(&desk.model, id)) {
                ((NMTTDISPINFOW *)lp)->lpszText = desk.windows[id.slot].tab_title;
                w_native_tooltip_text(note->hwndFrom,desk.windows[id.slot].tab_title);
            }
        }
        return 0;
    }
    case tab_activate:
        if (strip)
            w_activate_tab(strip);
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        if (strip) DeleteObject(strip->font);
        free(strip);
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HWND w_create_tabs(int output, w_rect bounds) {
    INITCOMMONCONTROLSEX controls = {.dwSize = sizeof(controls), .dwICC = ICC_TAB_CLASSES};
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
    int side = min(w_theme_button_size(o->dpi), bounds.h);
    int gap=w_theme_spacing(o->dpi);
    bool dimensions = strip->dpi != o->dpi || strip->gap!=gap || strip->width != bounds.w || strip->height != bounds.h ||
                      strip->side != side || strip->count != count;
    bool repaint = dimensions || strip->background != w_theme_background() ||
                   strip->foreground != w_theme_foreground() || strip->accent != w_theme_accent();
    bool remeasure=strip->height!=bounds.h;
    if (strip->font_dpi != o->dpi) {
        HFONT next=w_bar_create_label_font(o->dpi);
        if (next) {
            SendMessageW(control, WM_SETFONT, (WPARAM)next, FALSE);
            HWND tip=TabCtrl_GetToolTips(control);
            if (tip) SendMessageW(tip,WM_SETFONT,(WPARAM)next,FALSE);
            DeleteObject(strip->font);
            strip->font = next;
            strip->font_dpi = o->dpi;
            dimensions=repaint=remeasure=true;
        }
    }
    w_window_id old_first = strip->count ? strip->items[strip->first] : (w_window_id){.slot=-1};
    w_window_id old_active = strip->selected >= 0 ? strip->items[strip->selected] : (w_window_id){.slot=-1};
    bool active_was_visible = strip->selected >= strip->first && strip->selected < strip->first + strip->visible;
    strip->selected = -1;
    int first = strip->first;
    if (remeasure) memset(strip->labels,0,sizeof(strip->labels));
    HDC measure=NULL;
    HGDIOBJ old_font=NULL;
    int longest=0;
    for (int i = 0; i < count; ++i) {
        int slot = slots[i];
        w_window_id id = {slot, desk.model.windows[slot].generation};
        w_tab_measure *label=&strip->labels[slot];
        unsigned revision=desk.windows[slot].tab_title_revision;
        if (label->generation!=id.generation || label->revision!=revision) {
            if (!measure && (measure=GetDC(control)))
                old_font=SelectObject(measure,strip->font ? strip->font : GetStockObject(DEFAULT_GUI_FONT));
            if (measure) {
                RECT extent={.bottom=bounds.h};
                w_theme_draw_fitted_line(measure,desk.windows[slot].tab_title,-1,&extent,DT_CALCRECT|DT_NOPREFIX);
                *label=(w_tab_measure){id.generation,revision,extent.right};
            }
        }
        longest=max(longest,label->width);
        strip->items[i] = id;
        if (w_same_tab(old_first, id)) first = i;
        if (slot == selected) strip->selected = i;
    }
    if (measure) {
        SelectObject(measure,old_font);
        ReleaseDC(control,measure);
    }
    strip->count = count;
    strip->dpi = o->dpi;
    strip->width = bounds.w;
    strip->height = bounds.h;
    strip->side = side;
    strip->gap=gap;
    strip->background = w_theme_background();
    strip->foreground = w_theme_foreground();
    strip->accent = w_theme_accent();
    bool shown = IsWindowVisible(host) != 0;
    RECT rect;
    if (!GetWindowRect(host, &rect) || rect.left != bounds.x || rect.top != bounds.y ||
        rect.right != bounds.x + bounds.w || rect.bottom != bounds.y + bounds.h)
        SetWindowPos(host, NULL, bounds.x, bounds.y, bounds.w, bounds.h, SWP_NOACTIVATE | SWP_NOZORDER);
    // The common control starts a second row if the cells exactly fill the
    // strip between its two edges. Keep the boundary pixel inside the strip.
    int inset = 2*GetSystemMetricsForDpi(SM_CXEDGE,strip->dpi)+1;
    // Keep equal native cells. Bound the text budget of a long window title
    // so that one document path cannot displace all its neighbouring tabs.
    int minimum=max(side,4*gap+min(longest,2*side));
    int arrows = 0;
    if (count > 1 && count > (bounds.w - inset) / minimum)
        arrows = min(2, max(0, bounds.w - inset - 1) / side);
    int viewport = bounds.w - arrows * side;
    int visible=min(count,max(1,(viewport-inset)/minimum));
    if (dimensions || visible!=strip->visible) {
        strip->visible=visible;
        repaint=true;
        strip->single_arrow = arrows == 1;
        UINT flags = SWP_NOACTIVATE | SWP_NOZORDER;
        SetWindowPos(control, NULL, 0, 0, viewport, bounds.h, flags);
        SetWindowPos(strip->previous, NULL, viewport, (bounds.h-side)/2, side, side,
                     flags | (arrows == 2 ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
        SetWindowPos(strip->next, NULL, bounds.w-side, (bounds.h-side)/2, side, side,
                     flags | (arrows ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
        int width = max(1, (viewport - inset) / strip->visible);
        TabCtrl_SetItemSize(control, min(width, 32767), max(1, bounds.h - 2));
    }
    // Keep the left tab by identity during a reorder. Keep an active tab in
    // view until the user explicitly pages away from it.
    if (strip->selected >= 0 && (!w_same_tab(old_active,strip->items[strip->selected]) ||
        active_was_visible)) {
        first = min(first, strip->selected);
        first = max(first, strip->selected - strip->visible + 1);
    }
    first = max(0, min(first, count - strip->visible));
    strip->first = first;
    if (!w_render_page(strip)) {
        DestroyWindow(host);
        return NULL;
    }
    if (repaint)
        InvalidateRect(control, NULL, FALSE);
    strip->syncing = false;
    if (!shown)
        SetWindowPos(host, HWND_TOP, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    return host;
}
