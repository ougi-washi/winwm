#include "bar/bar.h"
#include "bar/menu.h"
#include <windowsx.h>
#include <commctrl.h>
#include <stdio.h>
#include <wchar.h>

////////////////
//~ Bar

static COLORREF w_rgb_value(uint32_t n) {
    return RGB(n >> 16, (n >> 8) & 255, n & 255);
}

static int w_format_module(w_bar_window *b, const char *key, wchar_t *s, int cap, char *action) {
    if (!strncmp(key, "custom/", 7) && b->custom)
        b->custom(key, s, cap, action, 128);
    else if (!strcmp(key, "menu")) {
        wcscpy(s, L"Run");
        strcpy(action, "launcher");
    } else if (!strcmp(key, "bluetooth")) {
        wcscpy(s, L"Bluetooth");
        strcpy(action, "bluetooth");
    } else if (!strcmp(key, "audio")) {
        if (b->audio_percent < 0)
            wcscpy(s, L"No audio output");
        else
            swprintf(s, cap, L"%ls  %d%%", b->audio_muted ? L"Muted" : L"Volume", b->audio_percent);
    } else if (!strcmp(key, "title")) {
        wcsncpy(s, b->view.title, cap - 1);
        strcpy(action, "switcher");
    } else if (!strcmp(key, "mode")) {
        wcscpy(s, b->view.resize_mode ? L"RESIZE" : b->view.inhibit ? L"AWAKE" : L"");
        strcpy(action, "resize_mode");
    } else if (!strcmp(key, "clock")) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        if (b->calendar_date)
            GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &t, L"MMM d", s, cap, NULL);
        else
            swprintf(s, cap, L"%02u:%02u", t.wHour, t.wMinute);
        strcpy(action, "settings");
    } else if (!strcmp(key, "battery")) {
        SYSTEM_POWER_STATUS p;
        if (GetSystemPowerStatus(&p) && p.BatteryFlag != 128 && p.BatteryLifePercent <= 100)
            swprintf(s, cap, L"%u%% %ls", p.BatteryLifePercent, p.ACLineStatus == 1 ? L"AC" : L"BAT");
        strcpy(action, "settings");
    } else if (!strcmp(key, "keyboard")) {
        HKL lang = GetKeyboardLayout(GetWindowThreadProcessId(GetForegroundWindow(), NULL));
        if (GetLocaleInfoW(LOWORD((uintptr_t)lang), LOCALE_SISO639LANGNAME, s, cap))
            CharUpperBuffW(s, (DWORD)wcslen(s));
        else
            wcscpy(s, L"KB");
        strcpy(action, "shortcuts");
    } else if (!strcmp(key, "memory")) {
        MEMORYSTATUSEX ms = {.dwLength = sizeof(ms)};
        if (GlobalMemoryStatusEx(&ms))
            swprintf(s, cap, L"%lu%% RAM", ms.dwMemoryLoad);
        strcpy(action, "open_tasks");
    } else if (b->custom)
        b->custom(key, s, cap, action, 128);
    return (int)wcslen(s);
}

typedef struct w_bar_cell {
    wchar_t text[256], tooltip[256];
    const w_module_style *style;
    char action[128], key[56];
    int width;
    bar_icon icon;
    bool active, empty;
} w_bar_cell;

typedef struct w_bar_group {
    w_bar_cell *cells;
    int capacity, count, length;
} w_bar_group;

static void w_measure(w_bar_window *b, HDC dc, const char *spec, bool vertical, w_bar_group *g) {
    w_bar_cell *cells = g->cells;
    int used = 0, length = 0;
    for (const char *p = spec; *p && used < g->capacity;) {
        while (*p == ' ')
            ++p;
        size_t n = strcspn(p, " ");
        char key[56] = {0};
        if (!n || n >= sizeof(key))
            break;
        memcpy(key, p, n);
        p += n;
        const w_module_style *style = NULL;
        for (int m = 0; m < b->config->style_count; m++)
            if (!strcmp(key, b->config->styles[m].name))
                style = &b->config->styles[m];
        char base[56];
        strcpy(base, key);
        char *tag = strchr(base, '#');
        if (tag)
            *tag = 0;
        bool spaces = !strcmp(base, "workspaces");
        for (int j = 0; j < (spaces ? workspace_limit : 1) && used < g->capacity; j++) {
            w_bar_cell *cell = &cells[used];
            *cell = (w_bar_cell){0};
            if (spaces) {
                if (j >= 5 && j != b->view.selected && !(b->view.occupied & (1 << j)))
                    continue;
                if (!b->config->workspace_names[j][0] ||
                    !w_os_wide(b->config->workspace_names[j], cell->text, count_of(cell->text)))
                    swprintf(cell->text, count_of(cell->text), L"%d", j + 1);
                snprintf(cell->action, sizeof(cell->action), "workspace_select %d", j + 1);
                cell->active = j == b->view.selected;
                cell->empty = !cell->active && !(b->view.occupied & (1 << j));
            } else if (!w_format_module(b, base, cell->text, count_of(cell->text), cell->action))
                continue;
            cell->style = style;
            wcscpy(cell->tooltip, cell->text);
            if (!strcmp(base, "keyboard")) {
                HKL layout = GetKeyboardLayout(GetWindowThreadProcessId(GetForegroundWindow(), NULL));
                GetLocaleInfoW(LOWORD((uintptr_t)layout), LOCALE_SLOCALIZEDDISPLAYNAME, cell->tooltip,
                               count_of(cell->tooltip));
            } else if (!strcmp(base, "clock")) {
                GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_LONGDATE, NULL, NULL, cell->tooltip,
                                count_of(cell->tooltip), NULL);
            }
            if (style) {
                if (*style->tooltip)
                    w_os_wide(style->tooltip, cell->tooltip, count_of(cell->tooltip));
                if (*style->action)
                    strcpy(cell->action, style->action);
                if (*style->format) {
                    wchar_t format[128];
                    if (w_os_wide(style->format, format, count_of(format))) {
                        wchar_t *field = wcsstr(format, L"{text}"), out[512];
                        if (field) {
                            *field = 0;
                            swprintf(out, count_of(out), L"%ls%ls%ls", format, cell->text, field + 6);
                        } else
                            wcscpy(out, format);
                        wcsncpy(cell->text, out, count_of(cell->text) - 1);
                        cell->text[count_of(cell->text) - 1] = 0;
                    }
                }
            }
            bool clock_lines = vertical && !strcmp(base, "clock") && (!style || !*style->format);
            if (clock_lines) {
                if (b->calendar_date) {
                    SYSTEMTIME t;
                    GetLocalTime(&t);
                    swprintf(cell->text, count_of(cell->text), L"%02u\n%02u", t.wMonth, t.wDay);
                } else
                    cell->text[2] = L'\n';
            }
            if (!style || !*style->format) {
                static const struct w_module_icon {
                    const char *key;
                    bar_icon icon;
                } icons[] = {{"menu", icon_run},    {"bluetooth", icon_bluetooth}, {"network", icon_network},
                             {"audio", icon_audio}, {"battery", icon_battery},     {"memory", icon_memory},
                             {"media", icon_play},  {"cpu", icon_memory}};
                for (size_t k = 0; b->icons.coverage && k < count_of(icons); ++k)
                    if (!strcmp(base, icons[k].key)) {
                        cell->icon = icons[k].icon;
                        if (cell->icon == icon_audio && (b->audio_muted || b->audio_percent < 0))
                            cell->icon = icon_muted;
                        break;
                    }
            }
            SelectObject(dc, b->font);
            strcpy(cell->key, base);
            SIZE size;
            GetTextExtentPoint32W(dc, cell->text, (int)wcslen(cell->text), &size);
            if (cell->icon)
                size = (SIZE){b->icons.size, b->icons.size};
            cell->width = vertical ? size.cy * (clock_lines ? 2 : 1) + 10 : size.cx + MulDiv(16, b->dpi, 96);
            cell->width = min(cell->width, MulDiv(160, b->dpi, 96));
            length += cell->width;
            used++;
        }
    }
    g->count = used;
    g->length = length;
}

static COLORREF w_mix(uint32_t base, uint32_t overlay, int percent) {
    int a = 100 - percent;
    return RGB((((base >> 16) & 255) * a + ((overlay >> 16) & 255) * percent) / 100,
               (((base >> 8) & 255) * a + ((overlay >> 8) & 255) * percent) / 100,
               ((base & 255) * a + (overlay & 255) * percent) / 100);
}

static void w_draw_group(w_bar_window *b, HDC dc, RECT r, const w_bar_group *g, bool vertical, int pos, int end) {
    for (int i = 0; i < g->count && pos < end; i++) {
        const w_bar_cell *cell = &g->cells[i];
        RECT box = r;
        if (vertical) {
            box.top = pos;
            box.bottom = min(end, pos + cell->width);
        } else {
            box.left = pos;
            box.right = min(end, pos + cell->width);
        }
        COLORREF paper = w_rgb_value(b->config->background);
        if (cell->active || b->hover == b->hit_count) {
            paper = cell->active ? w_rgb_value(b->config->accent)
                                 : w_mix(b->config->background, b->config->foreground, 12);
            SetDCBrushColor(dc, paper);
            RECT pill = box;
            InflateRect(&pill, -2, -MulDiv(5, b->dpi, 96));
            HGDIOBJ pen = SelectObject(dc, GetStockObject(NULL_PEN));
            HGDIOBJ brush = SelectObject(dc, GetStockObject(DC_BRUSH));
            RoundRect(dc, pill.left, pill.top, pill.right, pill.bottom, 8, 8);
            SelectObject(dc, brush);
            SelectObject(dc, pen);
        }
        uint32_t ink = cell->active                          ? b->config->background
                       : cell->style && cell->style->colored ? cell->style->color
                                                             : b->config->foreground;
        SetTextColor(dc, cell->empty ? w_mix(b->config->background, ink, 55) : w_rgb_value(ink));
        SelectObject(dc, b->font);
        UINT flags = DT_CENTER | DT_NOPREFIX;
        if (!wcschr(cell->text, L'\n'))
            flags |= DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS;
        if (cell->icon)
            w_icons_draw(&b->icons, dc, cell->icon, box, GetTextColor(dc), paper);
        else
            DrawTextW(dc, cell->text, -1, &box, flags);
        if (b->hit_count < (int)count_of(b->hits)) {
            strcpy(b->hits[b->hit_count].module, cell->key);
            b->hits[b->hit_count].style = cell->style;
            wcscpy(b->hits[b->hit_count].tooltip, cell->tooltip);
            b->hits[b->hit_count].box = box;
            strcpy(b->hits[b->hit_count++].action, cell->action);
        }
        pos += cell->width;
    }
}

static int w_hit_at(const w_bar_window *b, POINT p) {
    for (int i = 0; i < b->hit_count; ++i)
        if (PtInRect(&b->hits[i].box, p))
            return i;
    return -1;
}

static void w_hover(w_bar_window *b, int hit) {
    if (hit == b->hover)
        return;
    int previous = b->hover;
    b->wheel_remainder = 0;
    b->hover = hit;
    if (previous >= 0 && previous < b->hit_count)
        InvalidateRect(b->hwnd, &b->hits[previous].box, FALSE);
    if (hit >= 0)
        InvalidateRect(b->hwnd, &b->hits[hit].box, FALSE);
}

static void w_free_buffer(w_bar_window *b) {
    if (b->buffer_dc) {
        SelectObject(b->buffer_dc, b->previous_bitmap);
        DeleteObject(b->buffer);
        DeleteDC(b->buffer_dc);
        b->buffer_dc = NULL;
    }
}

////////////////
//~ Bar painting

static HDC w_paint_buffer(w_bar_window *b, HDC target, RECT r) {
    if (b->buffer_dc && b->buffer_size.cx == r.right && b->buffer_size.cy == r.bottom)
        return b->buffer_dc;
    w_free_buffer(b);
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bitmap = dc ? CreateCompatibleBitmap(target, r.right, r.bottom) : NULL;
    if (!bitmap) {
        if (dc)
            DeleteDC(dc);
        return target;
    }
    b->buffer_dc = dc;
    b->buffer = bitmap;
    b->previous_bitmap = SelectObject(dc, bitmap);
    b->buffer_size = (SIZE){r.right, r.bottom};
    return dc;
}

static LRESULT w_paint_bar(w_bar_window *b, HWND w) {
    PAINTSTRUCT ps;
    HDC target = BeginPaint(w, &ps);
    RECT r;
    GetClientRect(w, &r);
    HDC dc = w_paint_buffer(b, target, r);
    SetDCBrushColor(dc, w_rgb_value(b->config->background));
    FillRect(dc, &r, GetStockObject(DC_BRUSH));
    HGDIOBJ old = SelectObject(dc, b->font ? b->font : GetStockObject(DEFAULT_GUI_FONT));
    SetBkMode(dc, TRANSPARENT);
    bool vertical = r.bottom > r.right;
    int length = vertical ? r.bottom : r.right;
    int margin = MulDiv(6, b->dpi ? b->dpi : 96, 96);
    w_bar_cell cells[48];
    w_bar_group left = {.cells = cells, .capacity = count_of(cells)};
    w_measure(b, dc, b->config->bar_left, vertical, &left);
    w_bar_group center = {.cells = cells + left.count, .capacity = left.capacity - left.count};
    w_measure(b, dc, b->config->bar_center, vertical, &center);
    w_bar_group right = {.cells = center.cells + center.count, .capacity = center.capacity - center.count};
    w_measure(b, dc, b->config->bar_right, vertical, &right);
    int middle = max(margin, (length - center.length) / 2);
    int center_end = min(length - margin, middle + center.length);
    int right_start = max(center.length ? center_end : margin, length - margin - right.length);
    b->hit_count = 0;
    w_draw_group(b, dc, r, &left, vertical, margin, center.length ? middle : right_start);
    w_draw_group(b, dc, r, &center, vertical, middle, center_end);
    w_draw_group(b, dc, r, &right, vertical, right_start, length - margin);
    SelectObject(dc, old);
    if (dc != target) {
        RECT dirty = ps.rcPaint;
        BitBlt(target, dirty.left, dirty.top, dirty.right - dirty.left, dirty.bottom - dirty.top, dc,
               dirty.left, dirty.top, SRCCOPY);
    }
    EndPaint(w, &ps);
    w_os_mark(L"bar_paint");
    return 0;
}

static LRESULT CALLBACK w_bar_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    w_bar_window *b = (w_bar_window *)GetWindowLongPtrW(w, GWLP_USERDATA);
    if (msg == WM_NCCREATE) {
        SetWindowLongPtrW(w, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW *)lp)->lpCreateParams);
        return DefWindowProcW(w, msg, wp, lp);
    }
    if (!b)
        return DefWindowProcW(w, msg, wp, lp);
    if (msg == WM_PAINT)
        return w_paint_bar(b, w);
    if (msg == WM_SHOWWINDOW && !wp)
        w_bar_menu_close(b, true);
    if (msg == WM_MOUSEMOVE && !b->pressed) {
        POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, w, 0};
        TrackMouseEvent(&track);
        w_hover(b, w_hit_at(b, p));
        return 0;
    }
    if (msg == WM_MOUSELEAVE) {
        w_hover(b, -1);
        return 0;
    }
    if (msg == WM_DESTROY) {
        w_free_buffer(b);
        DeleteObject(b->font);
        w_icons_free(&b->icons);
        return 0;
    }
    if (msg == WM_NOTIFY && ((NMHDR *)lp)->code == TTN_GETDISPINFOW) {
        NMTTDISPINFOW *tip = (NMTTDISPINFOW *)lp;
        POINT p;
        GetCursorPos(&p);
        ScreenToClient(w, &p);
        tip->lpszText = L"";
        for (int i = 0; i < b->hit_count; i++)
            if (PtInRect(&b->hits[i].box, p)) {
                tip->lpszText = b->hits[i].tooltip;
                break;
            }
        return 0;
    }
    if (msg == WM_LBUTTONDOWN) {
        b->press = (POINT){GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ClientToScreen(w, &b->press);
        b->pressed = true;
        SetCapture(w);
        return 0;
    }
    if ((msg == WM_MOUSEMOVE || msg == WM_LBUTTONUP) && b->pressed) {
        POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ClientToScreen(w, &p);
        b->dragging |= abs(p.x - b->press.x) >= GetSystemMetrics(SM_CXDRAG) ||
                       abs(p.y - b->press.y) >= GetSystemMetrics(SM_CYDRAG);
        if (msg == WM_MOUSEMOVE)
            return 0;
        bool drag = b->dragging;
        ReleaseCapture();
        if (drag) {
            w_bar_menu_close(b, true);
            w_rect s = b->screen;
            int distance[] = {abs(p.y - s.y), abs(p.y - (s.y + s.h - 1)), abs(p.x - s.x),
                              abs(p.x - (s.x + s.w - 1))};
            static const char *edges[] = {"top", "bottom", "left", "right"};
            int nearest = 0;
            for (int i = 1; i < 4; i++)
                if (distance[i] < distance[nearest])
                    nearest = i;
            b->docked_edge = edges[nearest];
            if (b->dock)
                b->dock(b->output);
            return 0;
        }
        ScreenToClient(w, &p);
        int hit = w_hit_at(b, p);
        bool custom_action = hit >= 0 && b->hits[hit].style && *b->hits[hit].style->action;
        if (hit >= 0 && !custom_action && w_bar_menu_open(b, hit))
            return 0;
        w_bar_menu_close(NULL, true);
        if (hit >= 0 && *b->hits[hit].action)
            b->invoke(b->output, b->hits[hit].action);
        return 0;
    }
    if (msg == WM_CANCELMODE && GetCapture() == w)
        ReleaseCapture();
    if (msg == WM_CAPTURECHANGED || msg == WM_CANCELMODE) {
        b->dragging = b->pressed = false;
        return 0;
    }
    if (msg == WM_MBUTTONUP || msg == WM_RBUTTONUP) {
        int hit = w_hit_at(b, (POINT){GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        if (hit >= 0 && !strcmp(b->hits[hit].module, "audio"))
            b->invoke(b->output, "mute");
        else if (hit >= 0 && msg == WM_RBUTTONUP && !strcmp(b->hits[hit].module, "menu"))
            b->invoke(b->output, "settings");
        return 0;
    }
    if (msg == WM_MOUSEWHEEL) {
        POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(w, &p);
        int hit = w_hit_at(b, p);
        if (hit < 0)
            return 0;
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        b->wheel_remainder += delta;
        int steps = b->wheel_remainder / WHEEL_DELTA;
        b->wheel_remainder %= WHEEL_DELTA;
        if (!steps)
            return 0;
        const w_module_style *style = b->hits[hit].style;
        const char *action = style ? (steps > 0 ? style->scroll_up : style->scroll_down) : "";
        if (!*action && !strcmp(b->hits[hit].module, "audio"))
            action = steps > 0 ? "volume_up" : "volume_down";
        if (*action) {
            for (int i = 0; i < abs(steps); i++)
                b->invoke(b->output, action);
        } else if (!strcmp(b->hits[hit].module, "workspaces")) {
            char select[40];
            int ws = (b->view.selected - steps) % workspace_limit;
            snprintf(select, sizeof(select), "workspace_select %d",
                     (ws + workspace_limit) % workspace_limit + 1);
            b->invoke(b->output, select);
        }
        return 0;
    }
    if (msg == WM_MOUSEACTIVATE)
        return MA_NOACTIVATE;
    if (msg == WM_ERASEBKGND)
        return 1;
    return DefWindowProcW(w, msg, wp, lp);
}

////////////////
//~ Bar lifecycle

bool w_bar_create(w_bar_window *b, int output, const w_desktop_config *cfg, bar_action invoke) {
    WNDCLASSW wc = {.hInstance = GetModuleHandleW(NULL),
                    .lpszClassName = L"WinwmBar",
                    .lpfnWndProc = w_bar_proc,
                    .hCursor = LoadCursorW(NULL, IDC_ARROW)};
    RegisterClassW(&wc);
    b->hover = b->audio_percent = -1;
    b->config = cfg;
    b->output = output;
    b->invoke = invoke;
    b->hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, wc.lpszClassName,
                              L"winwm bar", WS_POPUP, 0, 0, 1, 1, NULL, NULL, wc.hInstance, b);
    if (b->hwnd) {
        INITCOMMONCONTROLSEX controls = {sizeof(controls), ICC_WIN95_CLASSES};
        InitCommonControlsEx(&controls);
        HWND tips = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_ALWAYSTIP,
                                    CW_USEDEFAULT, 0, 0, 0, b->hwnd, NULL, wc.hInstance, NULL);
        TOOLINFOW info = {.cbSize = sizeof(info),
                          .uFlags = TTF_SUBCLASS | TTF_IDISHWND,
                          .hwnd = b->hwnd,
                          .uId = (UINT_PTR)b->hwnd,
                          .lpszText = LPSTR_TEXTCALLBACKW};
        if (tips)
            SendMessageW(tips, TTM_ADDTOOLW, 0, (LPARAM)&info);
    }
    return b->hwnd != NULL;
}

w_rect w_bar_position(w_bar_window *b, w_rect screen, int dpi, bool hidden) {
    w_rect work = screen, area = screen;
    b->screen = screen;
    if (dpi != b->dpi) {
        w_icons_resize(&b->icons, dpi);
        if (b->font)
            DeleteObject(b->font);
        b->dpi = dpi;
        b->font = CreateFontW(-MulDiv(10, dpi, 72), 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                              L"Segoe UI");
    }
    int n = MulDiv(b->config->bar_height, dpi, 96);
    if (hidden) {
        ShowWindow(b->hwnd, SW_HIDE);
        return work;
    }
    const char *edge = b->docked_edge ? b->docked_edge : b->config->edge;
    if (!strcmp(edge, "left") || !strcmp(edge, "right")) {
        area.w = n;
        work.w -= n;
        if (!strcmp(edge, "left"))
            work.x += n;
        else
            area.x += work.w;
    } else {
        area.h = n;
        work.h -= n;
        if (!strcmp(edge, "top"))
            work.y += n;
        else
            area.y += work.h;
    }
    SetWindowPos(b->hwnd, HWND_TOPMOST, area.x, area.y, area.w, area.h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return work;
}

void w_bar_invalidate(w_bar_window *b, const char *module) {
    for (int i = 0; i < b->hit_count; i++)
        if (!strcmp(module, b->hits[i].module))
            InvalidateRect(b->hwnd, &b->hits[i].box, FALSE);
}

void w_bar_update(w_bar_window *b, const w_bar_view *v) {
    if (memcmp(&b->view, v, sizeof(*v))) {
        b->view = *v;
        InvalidateRect(b->hwnd, NULL, FALSE);
    }
}

void w_bar_set_audio(w_bar_window *b, int percent, bool muted) {
    if (b->audio_percent != percent || b->audio_muted != muted) {
        b->audio_percent = percent;
        b->audio_muted = muted;
        w_bar_invalidate(b, "audio");
        w_bar_menu_audio_changed(b);
    }
}
