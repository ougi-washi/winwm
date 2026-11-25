#include "desktop/desktop.h"
#include <dwmapi.h>
#include <math.h>

static bool ticking;
static const UINT place_flags = SWP_ASYNCWINDOWPOS | SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER;

static bool w_start_timer(void) {
    if (!ticking)
        ticking = SetTimer(desk.root, animation_timer, 16, NULL) != 0;
    return ticking;
}

void w_effects_preferences(void) {
    BOOL enabled = TRUE;
    HIGHCONTRASTW contrast = {.cbSize = sizeof(contrast)};
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0);
    SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0);
    desk.high_contrast = (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
    desk.motion_allowed = !desk.high_contrast && (desk.config.animations == motion_on ||
                          (desk.config.animations == motion_system && enabled));
    for (int i = 0; i < output_limit; i++)
        desk.outputs[i].bar.motion_allowed = desk.motion_allowed;
    if (ticking && !desk.motion_allowed)
        w_effects_tick();
}

static int w_lerp(int a, int b, double t) {
    return (int)lround(a + ((double)b-a)*t);
}

void w_effects_opacity(int slot, bool focused, bool entering) {
    w_window_state *s = &desk.windows[slot];
    if (s->direct_surface)
        return;
    BYTE inactive = desk.high_contrast ? 255 : desk.config.inactive_alpha;
    BYTE target = focused ? 255 : inactive;
    if (s->decorated && !entering && target == s->opacity.target && s->alpha == inactive)
        return;
    s->alpha = inactive;
    w_transition timing = desk.config.motion[entering ? motion_fade_in : motion_fade_switch];
    BYTE from = entering ? min(70,target) : s->opacity.value;
    s->opacity.target = target;
    s->opacity.start = 0;
    if (desk.motion_allowed && timing.ms && from != target && !desk.model.windows[slot].fullscreen &&
        w_start_timer() && w_os_window_opacity(s->hwnd,from,true)) {
        s->opacity.value = s->opacity.from = from;
        s->opacity.timing = timing;
        s->opacity.start = GetTickCount64();
    } else {
        s->opacity.value = target;
        w_os_window_opacity(s->hwnd,target,inactive != 255);
    }
}

static uint32_t w_color_at(uint32_t a, uint32_t b, double t) {
    t = fmax(0, fmin(1, t));
    return (w_lerp(a >> 16, b >> 16, t) << 16) |
           (w_lerp((a >> 8) & 255, (b >> 8) & 255, t) << 8) |
           w_lerp(a & 255, b & 255, t);
}

static bool w_place(HWND hwnd, w_rect from, w_rect to) {
    UINT flags = place_flags;
    if (from.w == to.w && from.h == to.h) flags |= SWP_NOSIZE;
    if (from.x == to.x && from.y == to.y) flags |= SWP_NOMOVE;
    if ((flags & (SWP_NOMOVE | SWP_NOSIZE)) == (SWP_NOMOVE | SWP_NOSIZE)) return true;
    return SetWindowPos(hwnd, NULL, to.x, to.y, to.w, to.h, flags) != 0;
}

bool w_effects_move(int slot, w_rect target, enum motion_kind kind, int direction) {
    w_window_state *s = &desk.windows[slot];
    w_transition timing = desk.config.motion[kind];
    s->motion.start = 0;
    if (!desk.motion_allowed || !timing.ms || IsHungAppWindow(s->hwnd))
        return false;
    w_rect from = w_os_bounds(s->hwnd);
    w_rect actual = from;
    if (kind == motion_move) {
        RECT to = w_os_to_rect(target);
        if (MonitorFromWindow(s->hwnd, MONITOR_DEFAULTTONEAREST) != MonitorFromRect(&to, MONITOR_DEFAULTTONEAREST))
            return false;
    }
    w_output_state *output = &desk.outputs[desk.model.windows[slot].output];
    if (kind == motion_open && timing.style >= style_slide) {
        from = target;
        int edge = timing.style;
        if (edge == style_slide) {
            w_rect work = output->work;
            int distances[] = {target.y-work.y, work.y+work.h-target.y-target.h,
                               target.x-work.x, work.x+work.w-target.x-target.w};
            int nearest = 0;
            for (int i = 1; i < 4; i++)
                if (distances[i] < distances[nearest])
                    nearest = i;
            edge = style_top + nearest;
        }
        int distance = MulDiv(48, output->dpi, 96);
        if (edge == style_left || edge == style_right)
            from.x += (edge == style_left ? -1 : 1) * min(distance, target.w/3);
        else
            from.y += (edge == style_top ? -1 : 1) * min(distance, target.h/3);
    } else if (kind == motion_open) {
        from = target;
        int percent = timing.percent < 0 ? 95 : timing.percent;
        from.w = max(1, MulDiv(target.w, percent, 100));
        from.h = max(1, MulDiv(target.h, percent, 100));
        from.x += (target.w-from.w)/2;
        from.y += (target.h-from.h)/2;
    } else if (kind == motion_workspace) {
        from = target;
        bool vertical = timing.style == style_slidevert;
        int extent = vertical ? target.h : target.w;
        int distance = timing.percent < 0 ? min(extent/8, MulDiv(48,output->dpi,96)) : MulDiv(extent,timing.percent,100);
        if (vertical)
            from.y += direction * distance;
        else
            from.x += direction * distance;
    }
    if (s->direct_surface) {
        // A framebuffer resize can clear the client until its next present.
        // Resize once and animate translation without rebuilding the swapchain.
        if (kind == motion_open && timing.style < style_slide)
            return false;
        from.w = target.w;
        from.h = target.h;
    }
    if (from.w <= 0 || from.h <= 0 || w_rect_equal(from, target) || !w_start_timer())
        return false;
    if (!w_place(s->hwnd, actual, from))
        return false;
    s->motion.from = from;
    s->motion.last = from;
    s->motion.target = target;
    s->motion.timing = timing;
    s->motion.start = GetTickCount64();
    return true;
}

static LRESULT CALLBACK w_outline_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE)
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW *)lp)->lpCreateParams);
    if (msg == WM_NCHITTEST)
        return HTTRANSPARENT;
    if (msg == WM_MOUSEACTIVATE)
        return MA_NOACTIVATE;
    if (msg == WM_ERASEBKGND)
        return TRUE;
    if (msg == WM_PAINT) {
        w_window_state *s = (void *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT r;
        GetClientRect(hwnd, &r);
        uint32_t a = s->outline.color[0], b = s->outline.color[1];
        SetDCBrushColor(dc, RGB(a >> 16, a >> 8, a));
        if (a == b)
            FillRect(dc, &ps.rcPaint, GetStockObject(DC_BRUSH));
        else {
            double radians = s->outline.angle * (3.141592653589793 / 180);
            double dx = cos(radians)*r.right, dy = sin(radians)*r.bottom;
            double low = fmin(0, dx) + fmin(0, dy), span = fabs(dx) + fabs(dy);
            TRIVERTEX vertices[4];
            for (int i = 0; i < 4; i++) {
                double distance = (i & 1 ? dx : 0) + (i & 2 ? dy : 0);
                uint32_t c = w_color_at(a, b, span ? (distance-low)/span : 0);
                vertices[i] = (TRIVERTEX){i & 1 ? r.right : 0, i & 2 ? r.bottom : 0,
                                          ((c >> 16) & 255)*257, ((c >> 8) & 255)*257, (c & 255)*257, 0};
            }
            GRADIENT_TRIANGLE triangles[] = {{0,1,2}, {1,2,3}};
            if (!GradientFill(dc, vertices, 4, triangles, 2, GRADIENT_FILL_TRIANGLE))
                FillRect(dc, &ps.rcPaint, GetStockObject(DC_BRUSH));
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void w_outline_hide(int slot) {
    w_window_state *s = &desk.windows[slot];
    if (s->outline.hwnd)
        DestroyWindow(s->outline.hwnd);
    memset(&s->outline, 0, sizeof(s->outline));
}

void w_effects_hide(int slot) {
    desk.windows[slot].motion.start = 0;
    if (desk.windows[slot].opacity.start)
        desk.windows[slot].decorated = false;
    desk.windows[slot].opacity.start = 0;
    w_outline_hide(slot);
}

static bool w_shape(HWND hwnd, int w, int h, int edge, int radius) {
    HRGN outer = radius ? CreateRoundRectRgn(0, 0, w+1, h+1, 2*(radius+edge), 2*(radius+edge))
                        : CreateRectRgn(0, 0, w, h);
    HRGN inner = radius ? CreateRoundRectRgn(edge, edge, w-edge+1, h-edge+1, 2*radius, 2*radius)
                        : CreateRectRgn(edge, edge, w-edge, h-edge);
    bool ok = outer && inner && CombineRgn(outer, outer, inner, RGN_DIFF) != ERROR &&
              SetWindowRgn(hwnd, outer, FALSE);
    if (!ok && outer)
        DeleteObject(outer);
    if (inner)
        DeleteObject(inner);
    return ok;
}

void w_outline_position(int slot) {
    w_window_state *s = &desk.windows[slot];
    HWND ring = s->outline.hwnd;
    if (!ring)
        return;
    if (!IsWindowVisible(s->hwnd) || IsIconic(s->hwnd) || IsZoomed(s->hwnd) || w_os_cloaked(s->hwnd)) {
        ShowWindow(ring, SW_HIDE);
        return;
    }
    RECT r;
    if (FAILED(DwmGetWindowAttribute(s->hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))) &&
        !GetWindowRect(s->hwnd, &r))
        return;
    int dpi = desk.outputs[desk.model.windows[slot].output].dpi;
    int edge = max(1, MulDiv(desk.config.border_size, dpi, 96));
    int radius = MulDiv(desk.config.rounding, dpi, 96);
    w_rect bounds = {r.left-edge, r.top-edge, r.right-r.left+2*edge, r.bottom-r.top+2*edge};
    bool reshape = s->outline.bounds.w != bounds.w || s->outline.bounds.h != bounds.h ||
                   s->outline.width != edge || s->outline.radius != radius;
    if (reshape && !w_shape(ring, bounds.w, bounds.h, edge, radius)) {
        ShowWindow(ring, SW_HIDE);
        return;
    }
    bool ordered = GetWindow(s->hwnd, GW_HWNDNEXT) == ring;
    if (!ordered || reshape || !w_rect_equal(s->outline.bounds, bounds) || !IsWindowVisible(ring)) {
        UINT flags = SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW | (ordered ? SWP_NOZORDER : 0);
        if (SetWindowPos(ring, s->hwnd, bounds.x, bounds.y, bounds.w, bounds.h, flags)) {
            s->outline.bounds = bounds;
            s->outline.width = edge;
            s->outline.radius = radius;
            if (reshape)
                InvalidateRect(ring, NULL, FALSE);
        }
    }
}

bool w_outline_sync(int slot, bool focused) {
    w_window_state *s = &desk.windows[slot];
    if (!s->outline.hwnd) {
        HINSTANCE instance = GetModuleHandleW(NULL);
        static ATOM atom;
        if (!atom) {
            WNDCLASSW wc = {.lpfnWndProc = w_outline_proc, .hInstance = instance, .lpszClassName = L"WinwmOutline"};
            atom = RegisterClassW(&wc);
        }
        if (!atom)
            return false;
        HWND ring = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT,
                                    MAKEINTATOM(atom), NULL, WS_POPUP, 0,0,0,0, NULL, NULL, instance, s);
        if (!ring)
            return false;
        if (!SetLayeredWindowAttributes(ring, 0, 255, LWA_ALPHA)) {
            DestroyWindow(ring);
            return false;
        }
        s->outline.hwnd = ring;
    }
    uint32_t a = focused ? desk.config.accent : desk.config.inactive_border;
    w_border_gradient gradient = focused ? desk.config.active_gradient : desk.config.inactive_gradient;
    if (!focused && s->custom_border) {
        a = GetRValue(s->border)*65536u + GetGValue(s->border)*256u + GetBValue(s->border);
        gradient.enabled = false;
    }
    if (desk.high_contrast) {
        COLORREF system = GetSysColor(focused ? COLOR_HIGHLIGHT : COLOR_WINDOWFRAME);
        a = (GetRValue(system) << 16) | (GetGValue(system) << 8) | GetBValue(system);
        gradient.enabled = false;
    }
    uint32_t b = gradient.enabled ? gradient.end : a;
    if (!gradient.enabled)
        gradient.angle = s->outline.angle;
    if (!s->outline.initialized || a != s->outline.target[0] || b != s->outline.target[1] ||
        gradient.angle != s->outline.angle) {
        s->outline.target[0] = a;
        s->outline.target[1] = b;
        s->outline.angle = gradient.angle;
        s->outline.timing = desk.config.motion[motion_border];
        if (s->outline.initialized && desk.motion_allowed && s->outline.timing.ms && w_start_timer()) {
            memcpy(s->outline.from, s->outline.color, sizeof(s->outline.color));
            s->outline.start = GetTickCount64();
        } else {
            s->outline.start = 0;
            memcpy(s->outline.color, s->outline.target, sizeof(s->outline.color));
        }
        s->outline.initialized = true;
        InvalidateRect(s->outline.hwnd, NULL, FALSE);
    }
    w_outline_position(slot);
    return true;
}

void w_effects_tick(void) {
    ULONGLONG now = GetTickCount64();
    bool more = false;
    for (int i = 0; i < window_limit; i++) {
        w_window_state *s = &desk.windows[i];
        if (!s->motion.start && !s->outline.start && !s->opacity.start)
            continue;
        DWORD pid = 0;
        if (!desk.model.windows[i].live || !GetWindowThreadProcessId(s->hwnd, &pid) || pid != s->pid ||
            !GetPropW(s->hwnd, L"winwm.managed") || !IsWindowVisible(s->hwnd) || IsIconic(s->hwnd)) {
            w_effects_hide(i);
            s->has_issued = false;
            continue;
        }
        if (s->opacity.start) {
            uint64_t elapsed = now-s->opacity.start;
            bool done = !desk.motion_allowed || elapsed >= (unsigned)s->opacity.timing.ms;
            double t = done ? 1 : w_motion_progress(s->opacity.timing,elapsed);
            BYTE alpha = (BYTE)w_clamp_int(w_lerp(s->opacity.from,s->opacity.target,t),1,255);
            if (done)
                alpha = s->opacity.target;
            if ((done || alpha != s->opacity.value) &&
                !w_os_window_opacity(s->hwnd,alpha,!done || s->alpha != 255))
                done = true;
            s->opacity.value = alpha;
            if (done)
                s->opacity.start = 0;
        }
        if (s->motion.start && IsZoomed(s->hwnd)) {
            s->motion.start = 0;
            s->has_issued = false;
            w_desktop_schedule();
        }
        if (s->motion.start) {
            uint64_t elapsed = now-s->motion.start;
            bool done = !desk.motion_allowed || elapsed >= (unsigned)s->motion.timing.ms || IsHungAppWindow(s->hwnd);
            double t = done ? 1 : w_motion_progress(s->motion.timing, elapsed);
            w_rect a = s->motion.from, b = s->motion.target;
            w_rect next = {w_lerp(a.x,b.x,t), w_lerp(a.y,b.y,t), w_lerp(a.w,b.w,t), w_lerp(a.h,b.h,t)};
            next.w = max(1,next.w);
            next.h = max(1,next.h);
            if (!w_place(s->hwnd, s->motion.last, next)) {
                s->has_issued = false;
                w_desktop_schedule();
                s->motion.start = 0;
            } else if (done)
                s->motion.start = 0;
            s->motion.last = next;
        }
        if (s->outline.start) {
            uint64_t elapsed = now-s->outline.start;
            bool done = !desk.motion_allowed || elapsed >= (unsigned)s->outline.timing.ms;
            double t = done ? 1 : w_motion_progress(s->outline.timing, elapsed);
            for (int j = 0; j < 2; j++)
                s->outline.color[j] = w_color_at(s->outline.from[j], s->outline.target[j], t);
            InvalidateRect(s->outline.hwnd, NULL, FALSE);
            if (done)
                s->outline.start = 0;
        }
        more |= s->motion.start || s->outline.start || s->opacity.start;
    }
    if (!more) {
        KillTimer(desk.root, animation_timer);
        ticking = false;
    }
}
