#include "desktop/desktop.h"
#include "theme/theme.h"
#include <limits.h>
#include <stdlib.h>

enum drag_kind { drag_idle, drag_splits, drag_float, drag_reposition };
static struct {
    enum drag_kind kind;
    w_window_id source;
    int output, workspace;
    bool floating, moved, left, top;
    POINT origin, pointer, minimum, maximum;
    UINT dpi;
    w_rect initial;
    w_split_grab grips[2];
} grab;

static HWND indicator;
static bool preview_pending;
static w_rect shown;
static COLORREF indicator_color=CLR_INVALID;
static bool indicator_contrast;

bool w_desktop_resizing(void) {
    return grab.kind == drag_splits;
}

bool w_desktop_dragging_float(int slot) {
    return grab.kind != drag_idle && grab.floating && grab.source.slot == slot &&
           w_model_valid(&desk.model, grab.source);
}

static void w_queue_preview(void) {
    if (!preview_pending)
        preview_pending = PostMessageW(desk.root, msg_drag_preview, 0, 0) != 0;
}

static bool w_valid_grab(void) {
    if (grab.kind == drag_idle || !desk.outputs[grab.output].live || desk.model.selected[grab.output] != grab.workspace)
        return false;
    if (grab.source.slot < 0)
        return grab.kind == drag_splits;
    if (!w_model_valid(&desk.model, grab.source) || !w_model_visible(&desk.model, grab.source.slot))
        return false;
    const w_model_window *w = &desk.model.windows[grab.source.slot];
    return w->output == grab.output && w->workspace == grab.workspace && w->floating == grab.floating &&
           !w->fullscreen && desk.windows[grab.source.slot].wants_visible;
}

static LRESULT CALLBACK w_indicator_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCHITTEST)
        return HTTRANSPARENT;
    if (msg == WM_MOUSEACTIVATE)
        return MA_NOACTIVATE;
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT r;GetClientRect(hwnd,&r);
        SetDCBrushColor(dc,indicator_color ^ 0xffffffu);
        FillRect(dc, &ps.rcPaint, GetStockObject(DC_BRUSH));
        int saved=SaveDC(dc);
        SelectObject(dc,GetStockObject(DC_PEN));
        SelectObject(dc,GetStockObject(indicator_contrast ? NULL_BRUSH : DC_BRUSH));
        SetDCPenColor(dc,indicator_color);
        SetDCBrushColor(dc,indicator_color);
        int curve=2*w_theme_radius(GetDpiForWindow(hwnd));
        if (curve) RoundRect(dc,0,0,r.right,r.bottom,curve,curve);
        else Rectangle(dc,0,0,r.right,r.bottom);
        RestoreDC(dc,saved);
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void w_desktop_drag_preview(void) {
    preview_pending = false;
    if (grab.kind != drag_idle && !w_valid_grab())
        w_desktop_cancel_drag();
    w_rect next;
    if (grab.kind != drag_reposition || grab.floating || !grab.moved || !w_valid_grab() ||
        !w_desktop_tile_drop(grab.source.slot, grab.pointer, &next, false)) {
        if (indicator && IsWindowVisible(indicator))
            ShowWindow(indicator, SW_HIDE);
        return;
    }
    if (!indicator) {
        HINSTANCE instance = GetModuleHandleW(NULL);
        WNDCLASSW wc = {.lpfnWndProc=w_indicator_proc, .hInstance=instance, .lpszClassName=L"WinwmDropPreview"};
        RegisterClassW(&wc);
        indicator = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT,
                                    wc.lpszClassName, NULL, WS_POPUP, 0,0,0,0, NULL,NULL,instance,NULL);
        if (!indicator)
            return;
    }
    COLORREF color=desk.high_contrast ? GetSysColor(COLOR_HIGHLIGHT) : w_theme_accent();
    if (color!=indicator_color || desk.high_contrast!=indicator_contrast) {
        if (!SetLayeredWindowAttributes(indicator,color ^ 0xffffffu,desk.high_contrast ? 255 : 82,LWA_ALPHA|LWA_COLORKEY))
            return;
        indicator_color=color;
        indicator_contrast=desk.high_contrast;
        InvalidateRect(indicator,NULL,FALSE);
    }
    if (!w_rect_equal(next, shown) || !IsWindowVisible(indicator)) {
        bool resized=next.w!=shown.w || next.h!=shown.h;
        shown = next;
        SetWindowPos(indicator, HWND_TOPMOST, next.x, next.y, next.w, next.h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        if (resized) InvalidateRect(indicator,NULL,FALSE);
    }
}

void w_desktop_cancel_drag(void) {
    grab.kind = drag_idle;
    if (indicator && IsWindowVisible(indicator))
        ShowWindow(indicator, SW_HIDE);
}

static void w_default_limits(UINT dpi) {
    grab.dpi = dpi ? dpi : 96;
    grab.minimum = (POINT){max(1, GetSystemMetricsForDpi(SM_CXMINTRACK, grab.dpi)),
                           max(1, GetSystemMetricsForDpi(SM_CYMINTRACK, grab.dpi))};
    grab.maximum = (POINT){max(grab.minimum.x, GetSystemMetricsForDpi(SM_CXMAXTRACK, grab.dpi)),
                           max(grab.minimum.y, GetSystemMetricsForDpi(SM_CYMAXTRACK, grab.dpi))};
}

// Query the client on the message loop, outside the low-level mouse callback.
void w_desktop_drag_limits(w_window_id id) {
    if (grab.kind != drag_float || id.slot != grab.source.slot || id.generation != grab.source.generation ||
        !w_valid_grab())
        return;
    HWND hwnd = desk.windows[id.slot].hwnd;
    MINMAXINFO limits = {.ptMinTrackSize=grab.minimum, .ptMaxTrackSize=grab.maximum};
    DWORD_PTR ignored;
    if (SendMessageTimeoutW(hwnd, WM_GETMINMAXINFO, 0, (LPARAM)&limits, SMTO_ABORTIFHUNG | SMTO_BLOCK, 35, &ignored)) {
        grab.minimum.x = max(1, limits.ptMinTrackSize.x);
        grab.minimum.y = max(1, limits.ptMinTrackSize.y);
        grab.maximum.x = max(grab.minimum.x, limits.ptMaxTrackSize.x);
        grab.maximum.y = max(grab.minimum.y, limits.ptMaxTrackSize.y);
    }
}

static void w_drag_float_to(w_rect bounds) {
    int slot = grab.source.slot;
    w_model_window *w = &desk.model.windows[slot];
    int output = w_desktop_output_for_bounds(grab.output, bounds);
    if (output != grab.output) {
        // These are already destination coordinates, including negative monitor origins.
        w_model_move(&desk.model, slot, output, desk.model.selected[output]);
        grab.output = output;
        grab.workspace = w->workspace;
        desk.windows[slot].displaced_from = 0;
        if (desk.model.focus == slot)
            w_model_focus(&desk.model, slot);
    }
    w_desktop_place_floating(slot, bounds);
}

static void w_update(POINT p, bool release) {
    int dx = p.x - grab.origin.x, dy = p.y - grab.origin.y;
    if (!grab.moved) {
        if (!dx && !dy)
            return;
        if (grab.kind == drag_reposition && !grab.floating &&
            abs(dx) < GetSystemMetricsForDpi(SM_CXDRAG, grab.dpi) &&
            abs(dy) < GetSystemMetricsForDpi(SM_CYDRAG, grab.dpi))
            return;
        grab.moved = true;
    }
    if (grab.kind == drag_splits) {
        bool changed = false;
        if (p.x != grab.pointer.x)
            changed = w_layout_drag_split(&desk.model, grab.output, grab.workspace, &grab.grips[0], dx);
        if (p.y != grab.pointer.y)
            changed |= w_layout_drag_split(&desk.model, grab.output, grab.workspace, &grab.grips[1], dy);
        grab.pointer = p;
        if (changed)
            w_desktop_schedule();
        return;
    }
    if (!grab.floating) {
        grab.pointer = p;
        if (release)
            w_desktop_drop_tile(grab.source.slot, p);
        else
            w_queue_preview();
        return;
    }
    w_rect r = grab.initial;
    grab.pointer = p;
    HWND hwnd = desk.windows[grab.source.slot].hwnd;
    if (grab.kind == drag_float) {
        UINT dpi = GetDpiForWindow(hwnd);
        if (dpi && dpi != grab.dpi) {
            w_default_limits(dpi);
            PostMessageW(desk.root, msg_drag_limits, grab.source.slot, grab.source.generation);
        }
        r = w_layout_drag_corner(r, dx, dy, grab.left, grab.top, grab.minimum.x, grab.minimum.y,
                                 grab.maximum.x, grab.maximum.y);
    } else {
        r = w_layout_drag_position(r, w_os_bounds(hwnd), grab.origin.x-r.x, grab.origin.y-r.y, p.x, p.y);
    }
    w_drag_float_to(r);
    if (release)
        desk.windows[grab.source.slot].native_move = move_released;
}

void w_desktop_drag_client_resized(int slot) {
    if (grab.kind != drag_reposition || !grab.moved || !w_desktop_dragging_float(slot) || !w_valid_grab())
        return;
    w_rect actual = w_os_bounds(desk.windows[slot].hwnd);
    w_rect requested = desk.model.windows[slot].floating_bounds;
    if (actual.w != requested.w || actual.h != requested.h)
        w_update(grab.pointer, false);
}

bool w_desktop_drag(enum input_drag_event event, POINT point) {
    if (event == input_drag_cancel) {
        w_desktop_cancel_drag();
        return false;
    }
    if (event == input_drag_move || event == input_drag_end) {
        if (!w_valid_grab()) {
            w_desktop_cancel_drag();
            return false;
        }
        w_update(point, event == input_drag_end);
        if (event == input_drag_end)
            w_desktop_cancel_drag();
        return true;
    }
    w_desktop_cancel_drag();
    if (event == input_drag_edge && !desk.config.resize_on_border)
        return false;
    HWND hit = GetAncestor(WindowFromPoint(point), GA_ROOT);
    int slot = w_desktop_find(hit), output = -1;
    if (slot >= 0) {
        if (!desk.windows[slot].wants_visible || !w_model_visible(&desk.model, slot) ||
            desk.model.windows[slot].fullscreen || IsZoomed(hit) || !IsWindowEnabled(hit))
            return false;
        output = desk.model.windows[slot].output;
    } else if (event == input_drag_edge && w_os_desktop_surface(hit)) {
        HMONITOR monitor = MonitorFromPoint(point, MONITOR_DEFAULTTONULL);
        for (int o=0; o<output_limit; o++)
            if (desk.outputs[o].live && desk.outputs[o].monitor == monitor)
                output = o;
    }
    if (output < 0)
        return false;
    grab.output = output;
    grab.workspace = desk.model.selected[output];
    grab.source = (w_window_id){slot, slot >= 0 ? desk.model.windows[slot].generation : 0};
    grab.origin = grab.pointer = point;
    grab.moved = false;
    grab.floating = slot >= 0 && desk.model.windows[slot].floating;
    grab.dpi = desk.outputs[output].dpi;

    if (event == input_drag_reposition || grab.floating) {
        if (grab.floating && event != input_drag_reposition &&
            (event != input_drag_inside || !(GetWindowLongPtrW(hit, GWL_STYLE) & WS_THICKFRAME)))
            return false;
        grab.initial = w_os_bounds(hit);
        if (grab.initial.w < 1 || grab.initial.h < 1)
            return false;
        grab.kind = event == input_drag_reposition ? drag_reposition : drag_float;
        grab.left = point.x < grab.initial.x + grab.initial.w/2;
        grab.top = point.y < grab.initial.y + grab.initial.h/2;
        if (grab.kind == drag_float) {
            w_default_limits(GetDpiForWindow(hit));
            PostMessageW(desk.root, msg_drag_limits, slot, grab.source.generation);
        }
    } else {
        int edge = desk.borders_hidden ? 0 : desk.config.border_size;
        int tolerance = MulDiv(max(4, edge) + desk.config.gap/2, grab.dpi, 96);
        if (!w_layout_grab_splits(&desk.model, output, slot, point.x, point.y, tolerance,
                                  event == input_drag_inside, grab.grips))
            return false;
        grab.kind = drag_splits;
    }
    if (slot >= 0)
        PostMessageW(desk.root, msg_pointer_select, slot, grab.source.generation);
    return true;
}
