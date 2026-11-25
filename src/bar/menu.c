#include "bar/menu.h"
#include <stdio.h>
#include <commctrl.h>
#include <windowsx.h>
#include <wchar.h>

////////////////
//~ Menu

typedef struct w_menu_row {
    wchar_t label[192];
    const char *action;
    HKL layout;
    bool checked, disabled;
} w_menu_row;

typedef struct w_flyout {
    w_bar_window *bar;
    HWND window, list, slider, previous, language_target;
    HFONT heading;
    DWORD language_thread;
    int count, pressed_row;
    bool closing, owns_memory;
    w_layer_fade fade;
    char module[56];
    w_menu_row rows[];
} w_flyout;

static w_flyout *open_menu;

////////////////
//~ Menu Functions

static COLORREF w_color(uint32_t rgb) {
    return RGB(rgb >> 16, rgb >> 8, rgb);
}

static int w_scale(const w_bar_window *bar, int pixels) {
    return MulDiv(pixels, bar->dpi, 96);
}

bool w_bar_menu_active(void) {
    return open_menu && !open_menu->closing;
}

static void w_dismiss(w_flyout *menu, bool restore) {
    if (!menu || menu->closing)
        return;
    menu->closing = true;
    if (open_menu == menu)
        open_menu = NULL;
    HWND target = menu->previous;
    HWND hwnd = menu->window;
    bool had_focus = GetForegroundWindow() == hwnd;
    SetWindowLongPtrW(hwnd,GWL_EXSTYLE,GetWindowLongPtrW(hwnd,GWL_EXSTYLE)|WS_EX_TRANSPARENT|WS_EX_NOACTIVATE);
    HWND capture = GetCapture();
    if (capture == hwnd || IsChild(hwnd,capture))
        ReleaseCapture();
    if (restore && had_focus && target && IsWindowVisible(target))
        SetForegroundWindow(target);
    if (GetForegroundWindow() == hwnd ||
        !w_bar_fade(hwnd,&menu->fade,0,menu->bar->config->motion[motion_layers_out],menu->bar->motion_allowed))
        DestroyWindow(hwnd);
}

void w_bar_menu_close(const w_bar_window *bar, bool restore) {
    if (open_menu && (!bar || bar == open_menu->bar))
        w_dismiss(open_menu, restore);
}

static void w_pick(w_flyout *menu, int index) {
    if (index < 0 || index + 1 >= menu->count)
        return;
    w_menu_row selected = menu->rows[index + 1];
    if (selected.disabled)
        return;
    HWND target = menu->language_target;
    DWORD thread = menu->language_thread;
    w_bar_window *bar = menu->bar;
    if (selected.action && !strcmp(selected.action, "mute")) {
        bar->invoke(bar->output, selected.action);
        return;
    }
    w_dismiss(menu, true);
    if (selected.layout) {
        if (!IsWindow(target) || GetWindowThreadProcessId(target, NULL) != thread ||
            !PostMessageW(target, WM_INPUTLANGCHANGEREQUEST, 0, (LPARAM)selected.layout))
            bar->invoke(bar->output, "notify Focus an application before switching input layout");
    }
    else if (selected.action)
        bar->invoke(bar->output, selected.action);
}

static LRESULT CALLBACK w_list_proc(HWND list, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id,
                                  DWORD_PTR data) {
    (void)id;
    w_flyout *menu = (w_flyout *)data;
    if (message == WM_KEYDOWN) {
        if (wp == VK_ESCAPE) {
            w_dismiss(menu, true);
            return 0;
        }
        if (wp == VK_TAB && menu->slider && IsWindowEnabled(menu->slider)) {
            SetFocus(menu->slider);
            return 0;
        }
        if (wp == VK_RETURN || wp == VK_SPACE) {
            w_pick(menu, (int)SendMessageW(list, LB_GETCURSEL, 0, 0));
            return 0;
        }
    }
    if (message == WM_MOUSEMOVE || message == WM_LBUTTONDOWN) {
        LRESULT hit = SendMessageW(list, LB_ITEMFROMPOINT, 0, lp);
        if (message == WM_LBUTTONDOWN)
            menu->pressed_row = HIWORD(hit) ? -1 : (int)LOWORD(hit);
        else if (!HIWORD(hit))
            SendMessageW(list, LB_SETCURSEL, LOWORD(hit), 0);
    }
    if (message == WM_LBUTTONUP) {
        LRESULT hit = SendMessageW(list, LB_ITEMFROMPOINT, 0, lp);
        LRESULT result = DefSubclassProc(list, message, wp, lp);
        if (menu == open_menu && !HIWORD(hit) && menu->pressed_row == (int)LOWORD(hit))
            w_pick(menu, LOWORD(hit));
        return result;
    }
    if (message == WM_NCDESTROY)
        RemoveWindowSubclass(list, w_list_proc, 1);
    return DefSubclassProc(list, message, wp, lp);
}

static void w_fill(HDC dc, RECT r, COLORREF value) {
    SetDCBrushColor(dc, value);
    FillRect(dc, &r, GetStockObject(DC_BRUSH));
}

static void w_volume_header(w_flyout *menu, int percent) {
    wchar_t *label = menu->rows[0].label;
    if (percent < 0)
        wcscpy(label, L"No audio output");
    else
        swprintf(label, count_of(menu->rows[0].label), L"%ls   %d%%",
                 menu->bar->audio_muted ? L"Muted" : L"Output volume", percent);
    SetWindowTextW(menu->window, label);
    InvalidateRect(menu->window, NULL, FALSE);
}

static void w_request_volume(w_flyout *menu, int percent) {
    char action[24];
    snprintf(action, sizeof(action), "volume_set %d", percent);
    w_volume_header(menu, percent);
    menu->bar->invoke(menu->bar->output, action);
}

static LRESULT CALLBACK w_slider_proc(HWND slider, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
    (void)id;
    w_flyout *menu = (w_flyout *)data;
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
        w_dismiss(menu, true);
        return 0;
    }
    if (msg == WM_KEYDOWN && wp == VK_TAB) {
        SetFocus(menu->list);
        return 0;
    }
    if (msg == WM_LBUTTONDOWN || ((msg == WM_MOUSEMOVE || msg == WM_LBUTTONUP) && GetCapture() == slider)) {
        if (msg == WM_LBUTTONDOWN) {
            SetFocus(slider);
            SetCapture(slider);
        }
        RECT r;
        GetClientRect(slider, &r);
        int pad = w_scale(menu->bar, 10);
        int next = w_clamp_int(MulDiv(GET_X_LPARAM(lp) - pad, 100, max(1, r.right - 2 * pad)), 0, 100);
        if (next != (int)SendMessageW(slider, TBM_GETPOS, 0, 0)) {
            SendMessageW(slider, TBM_SETPOS, TRUE, next);
            w_request_volume(menu, next);
        }
        if (msg == WM_LBUTTONUP)
            ReleaseCapture();
        return 0;
    }
    return DefSubclassProc(slider, msg, wp, lp);
}

static LRESULT w_draw_slider(w_flyout *menu, NMCUSTOMDRAW *draw) {
    if (draw->dwDrawStage != CDDS_PREPAINT)
        return CDRF_DODEFAULT;
    const w_desktop_config *cfg = menu->bar->config;
    RECT r;
    GetClientRect(menu->slider, &r);
    w_fill(draw->hdc, r, w_color(cfg->background));
    int pad = w_scale(menu->bar, 10), middle = r.bottom / 2, half = max(1, w_scale(menu->bar, 2));
    RECT track = {pad, middle - half, r.right - pad, middle + half};
    w_fill(draw->hdc, track, w_color(cfg->inactive_border));
    if (IsWindowEnabled(menu->slider)) {
        int percent = (int)SendMessageW(menu->slider, TBM_GETPOS, 0, 0);
        track.right = pad + MulDiv(track.right - pad, percent, 100);
        w_fill(draw->hdc, track, w_color(cfg->accent));
        int radius = w_scale(menu->bar, 6);
        HGDIOBJ pen = SelectObject(draw->hdc, GetStockObject(NULL_PEN));
        HGDIOBJ brush = SelectObject(draw->hdc, GetStockObject(DC_BRUSH));
        SetDCBrushColor(draw->hdc, w_color(cfg->foreground));
        Ellipse(draw->hdc, track.right-radius, middle-radius, track.right+radius+1, middle+radius+1);
        SelectObject(draw->hdc, brush);
        SelectObject(draw->hdc, pen);
        if (GetFocus() == menu->slider && !(SendMessageW(menu->slider, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS))
            DrawFocusRect(draw->hdc, &r);
    }
    return CDRF_SKIPDEFAULT;
}

////////////////
//~ Flyout window

static LRESULT CALLBACK w_flyout_proc(HWND w, UINT message, WPARAM wp, LPARAM lp) {
    w_flyout *menu = (w_flyout *)GetWindowLongPtrW(w, GWLP_USERDATA);
    if (message == WM_NCCREATE) {
        menu = ((CREATESTRUCTW *)lp)->lpCreateParams;
        SetWindowLongPtrW(w, GWLP_USERDATA, (LONG_PTR)menu);
        menu->window = w;
        return DefWindowProcW(w, message, wp, lp);
    }
    if (!menu)
        return DefWindowProcW(w, message, wp, lp);
    w_bar_window *bar = menu->bar;
    const w_desktop_config *cfg = bar->config;
    switch (message) {
    case WM_TIMER:
        if (wp == bar_fade_timer && menu->fade.start &&
            !w_bar_fade_tick(w,&menu->fade,bar->motion_allowed) && menu->closing)
            DestroyWindow(w);
        return 0;
    case WM_CLOSE:
        w_dismiss(menu,true);
        return 0;
    case WM_HSCROLL:
        if ((HWND)lp == menu->slider && LOWORD(wp) != TB_ENDTRACK)
            w_request_volume(menu, (int)SendMessageW(menu->slider, TBM_GETPOS, 0, 0));
        return 0;
    case WM_NOTIFY:
        if (((NMHDR *)lp)->hwndFrom == menu->slider && ((NMHDR *)lp)->code == NM_CUSTOMDRAW)
            return w_draw_slider(menu, (NMCUSTOMDRAW *)lp);
        break;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE)
            w_dismiss(menu, false);
        return 0;
    case WM_CTLCOLORLISTBOX:
        SetDCBrushColor((HDC)wp, w_color(cfg->background));
        return (LRESULT)GetStockObject(DC_BRUSH);
    case WM_ERASEBKGND:
        return TRUE;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        RECT r;
        GetClientRect(w, &r);
        w_fill(dc, r, w_color(cfg->inactive_border));
        InflateRect(&r, -1, -1);
        w_fill(dc, r, w_color(cfg->background));
        r.left += w_scale(bar, 16);
        r.right -= w_scale(bar, 16);
        r.bottom = w_scale(bar, 38);
        HGDIOBJ old = SelectObject(dc, menu->heading ? menu->heading : bar->font);
        SetTextColor(dc, w_color(cfg->foreground));
        SetBkMode(dc, TRANSPARENT);
        const wchar_t *heading = menu->rows[0].label;
        if (menu->slider && bar->audio_percent >= 0 && wcschr(heading, L'%')) {
            wchar_t percentage[12];
            swprintf(percentage, count_of(percentage), L"%d%%", (int)SendMessageW(menu->slider, TBM_GETPOS, 0, 0));
            SetTextColor(dc, w_color(cfg->accent));
            DrawTextW(dc, percentage, -1, &r, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
            SetTextColor(dc, w_color(cfg->foreground));
            r.right -= w_scale(bar, 52);
            heading = bar->audio_muted ? L"Output muted" : L"Output volume";
        }
        DrawTextW(dc, heading, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        SelectObject(dc, old);
        EndPaint(w, &ps);
        return 0;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *item = (DRAWITEMSTRUCT *)lp;
        if (item->itemID >= (UINT)(menu->count - 1))
            return FALSE;
        w_menu_row *row = &menu->rows[item->itemID + 1];
        RECT r = item->rcItem;
        bool selected = !!(item->itemState & ODS_SELECTED);
        w_fill(item->hDC, r, w_color(cfg->background));
        if (selected) {
            InflateRect(&r, -w_scale(bar, 2), -w_scale(bar, 2));
            w_fill(item->hDC, r, w_color(cfg->inactive_border));
        }
        SetBkMode(item->hDC, TRANSPARENT);
        SetTextColor(item->hDC, row->disabled ? GetSysColor(COLOR_GRAYTEXT) :
                     w_color(row->checked ? cfg->accent : cfg->foreground));
        HGDIOBJ old = SelectObject(item->hDC, bar->font);
        r.left += w_scale(bar, 10);
        if (row->checked)
            DrawTextW(item->hDC, L"\x2713", -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        r.left += w_scale(bar, 16);
        r.right -= w_scale(bar, 6);
        DrawTextW(item->hDC, row->label, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        SelectObject(item->hDC, old);
        return TRUE;
    }
    case WM_NCDESTROY:
        if (open_menu == menu)
            open_menu = NULL;
        SetWindowLongPtrW(w, GWLP_USERDATA, 0);
        DeleteObject(menu->heading);
        if (menu->owns_memory)
            free(menu);
        break;
    }
    return DefWindowProcW(w, message, wp, lp);
}

static void w_show(w_bar_window *bar, int hit, const w_menu_row *rows, int count, HWND language_target) {
    if (count < 2)
        return;
    if (open_menu) {
        bool same = open_menu->bar == bar && !strcmp(open_menu->module, bar->hits[hit].module);
        w_dismiss(open_menu, true);
        if (same)
            return;
    }
    w_flyout *menu = calloc(1, sizeof(*menu) + (size_t)count * sizeof(*rows));
    if (!menu)
        return;
    menu->bar = bar;
    menu->previous = GetForegroundWindow();
    menu->language_target = language_target;
    if (language_target)
        menu->language_thread = GetWindowThreadProcessId(language_target, NULL);
    menu->count = count;
    menu->pressed_row = -1;
    w_str_copy(menu->module, sizeof(menu->module), bar->hits[hit].module);
    memcpy(menu->rows, rows, (size_t)count * sizeof(*rows));

    RECT anchor = bar->hits[hit].box;
    MapWindowPoints(bar->hwnd, HWND_DESKTOP, (POINT *)&anchor, 2);
    bool audio = !strcmp(menu->module, "audio");
    int margin = w_scale(bar, 6), row_height = w_scale(bar, 35), header = w_scale(bar, audio ? 76 : 38);
    HDC dc = GetDC(bar->hwnd);
    HGDIOBJ old = SelectObject(dc, bar->font);
    int width = w_scale(bar, audio ? 288 : 236);
    for (int i = 0; i < count; ++i) {
        SIZE size = {0};
        GetTextExtentPoint32W(dc, rows[i].label, (int)wcslen(rows[i].label), &size);
        width = max(width, size.cx + w_scale(bar, 60));
    }
    SelectObject(dc, old);
    ReleaseDC(bar->hwnd, dc);
    w_rect screen = bar->screen;
    width = min(width, screen.w);
    int height = min(screen.h, header + min(10, count - 1) * row_height + margin + 1);
    const char *edge = bar->docked_edge ? bar->docked_edge : bar->config->edge;
    int x = anchor.left < screen.x + screen.w / 2 ? anchor.left : anchor.right - width;
    int y = anchor.bottom + margin;
    if (!strcmp(edge, "bottom"))
        y = anchor.top - height - margin;
    if (!strcmp(edge, "left") || !strcmp(edge, "right")) {
        x = !strcmp(edge, "left") ? anchor.right + margin : anchor.left - width - margin;
        y = anchor.top;
    }
    x = w_clamp_int(x, screen.x, screen.x + screen.w - width);
    y = w_clamp_int(y, screen.y, screen.y + screen.h - height);
    HINSTANCE instance = GetModuleHandleW(NULL);
    WNDCLASSW wc = {.lpfnWndProc = w_flyout_proc,
                    .hInstance = instance,
                    .hCursor = LoadCursorW(NULL, IDC_ARROW),
                    .lpszClassName = L"WinwmBarFlyout"};
    RegisterClassW(&wc);
    HWND popup =
        CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName, rows[0].label,
                        WS_POPUP | WS_CLIPCHILDREN, x, y, width, height, bar->hwnd, NULL, instance, menu);
    if (!popup) {
        free(menu);
        return;
    }
    menu->owns_memory = true;
    menu->heading = CreateFontW(-w_scale(bar, 15), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    if (audio) {
        INITCOMMONCONTROLSEX init = {sizeof(init), ICC_BAR_CLASSES};
        InitCommonControlsEx(&init);
        menu->slider = CreateWindowExW(0, TRACKBAR_CLASSW, L"Output volume",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS,
                        margin, w_scale(bar, 36), width - 2 * margin, w_scale(bar, 34),
                        popup, NULL, instance, NULL);
        if (!menu->slider || !SetWindowSubclass(menu->slider, w_slider_proc, 1, (DWORD_PTR)menu)) {
            DestroyWindow(popup);
            return;
        }
        SendMessageW(menu->slider, TBM_SETRANGE, FALSE, MAKELPARAM(0, 100));
        SendMessageW(menu->slider, TBM_SETPOS, TRUE, max(0, bar->audio_percent));
        EnableWindow(menu->slider, bar->audio_percent >= 0);
    }
    menu->list = CreateWindowExW(0, L"LISTBOX", rows[0].label,
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_OWNERDRAWFIXED |
                                     LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
                                 margin, header, width - 2 * margin, height - header - margin, popup, NULL,
                                 instance, NULL);
    if (!menu->list || !SetWindowSubclass(menu->list, w_list_proc, 1, (DWORD_PTR)menu)) {
        DestroyWindow(popup);
        return;
    }
    SendMessageW(menu->list, WM_SETFONT, (WPARAM)bar->font, FALSE);
    SendMessageW(menu->list, LB_SETITEMHEIGHT, 0, row_height);
    int selected = 0;
    for (int i = 1; i < count; ++i) {
        SendMessageW(menu->list, LB_ADDSTRING, 0, (LPARAM)rows[i].label);
        if (rows[i].checked)
            selected = i - 1;
    }
    SendMessageW(menu->list, LB_SETCURSEL, selected, 0);
    open_menu = menu;
    w_bar_fade(popup,&menu->fade,255,bar->config->motion[motion_layers_in],bar->motion_allowed);
    ShowWindow(popup, SW_SHOW);
    SetForegroundWindow(popup);
    if (IsWindow(popup))
        SetFocus(menu->slider && IsWindowEnabled(menu->slider) ? menu->slider : menu->list);
}

void w_bar_menu_audio_error(void) {
    if (w_bar_menu_active() && !strcmp(open_menu->module, "audio")) {
        SendMessageW(open_menu->slider, TBM_SETPOS, TRUE, max(0, open_menu->bar->audio_percent));
        wcscpy(open_menu->rows[0].label, L"Output rejected the change");
        SetWindowTextW(open_menu->window, open_menu->rows[0].label);
        InvalidateRect(open_menu->window, NULL, FALSE);
    }
}

void w_bar_menu_audio_changed(w_bar_window *b) {
    w_flyout *menu = open_menu;
    if (!menu || menu->bar != b || strcmp(menu->module, "audio"))
        return;
    bool available = b->audio_percent >= 0;
    EnableWindow(menu->slider, available);
    if (!available || GetCapture() != menu->slider) {
        SendMessageW(menu->slider, TBM_SETPOS, TRUE, max(0, b->audio_percent));
        w_volume_header(menu, b->audio_percent);
    }
    w_menu_row *mute = &menu->rows[1];
    mute->disabled = !available;
    const wchar_t *label = b->audio_muted ? L"Unmute" : L"Mute";
    if (wcscmp(label, mute->label)) {
        int selected = (int)SendMessageW(menu->list, LB_GETCURSEL, 0, 0);
        wcscpy(mute->label, label);
        SendMessageW(menu->list, LB_DELETESTRING, 0, 0);
        SendMessageW(menu->list, LB_INSERTSTRING, 0, (LPARAM)label);
        SendMessageW(menu->list, LB_SETCURSEL, selected, 0);
    }
    InvalidateRect(menu->list, NULL, FALSE);
    InvalidateRect(menu->window, NULL, FALSE);
}

////////////////
//~ Language selection

HWND w_bar_input_target(void) {
    HWND target = w_bar_menu_active() ? open_menu->previous : GetForegroundWindow();
    if (!target)
        return NULL;
    DWORD thread = GetWindowThreadProcessId(target, NULL);
    GUITHREADINFO gui = {.cbSize = sizeof(gui)};
    if (GetGUIThreadInfo(thread, &gui) && gui.hwndFocus)
        target = gui.hwndFocus;
    return target;
}

// Windows encodes alternate layouts as Fxxx handles; their registry entries
// identify the matching variant with "Layout Id" (for example US Dvorak).
static void w_layout_label(HKL layout, wchar_t *out, size_t cap) {
    DWORD value = (DWORD)(uintptr_t)layout;
    WORD physical = HIWORD(value);
    wchar_t language[96] = {0}, name[96] = {0}, key[KL_NAMELENGTH];
    GetLocaleInfoW(LOWORD(value), LOCALE_SLOCALIZEDDISPLAYNAME, language, count_of(language));
    swprintf(key, count_of(key), L"%08lx", (physical & 0xf000) == 0xe000 ? value : (DWORD)physical);
    HKEY layouts;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Keyboard Layouts", 0,
                      KEY_READ, &layouts) == ERROR_SUCCESS) {
        if ((physical & 0xf000) == 0xf000) {
            wchar_t candidate[KL_NAMELENGTH], id[16];
            for (DWORD i = 0; ; i++) {
                DWORD length = count_of(candidate);
                LSTATUS result = RegEnumKeyExW(layouts, i, candidate, &length, NULL, NULL, NULL, NULL);
                if (result == ERROR_MORE_DATA)
                    continue;
                if (result != ERROR_SUCCESS)
                    break;
                DWORD bytes = sizeof(id);
                if (RegGetValueW(layouts, candidate, L"Layout Id", RRF_RT_REG_SZ, NULL, id, &bytes) == ERROR_SUCCESS &&
                    wcstoul(id, NULL, 16) == (physical & 0xfff)) {
                    wcscpy(key, candidate);
                    break;
                }
            }
        }
        w_os_keyboard_name(layouts, key, name, count_of(name));
        RegCloseKey(layouts);
    }
    if (*language && *name && wcscmp(language, name))
        swprintf(out, cap, L"%.88ls  \u00b7  %.88ls", language, name);
    else if (*language || *name)
        swprintf(out, cap, L"%ls", *name ? name : language);
    else
        swprintf(out, cap, L"Keyboard %08lX", value);
}

static void w_language_menu(w_bar_window *b, int hit) {
    HWND target = w_bar_input_target();
    HKL active = target ? GetKeyboardLayout(GetWindowThreadProcessId(target, NULL)) : NULL;
    int count = GetKeyboardLayoutList(0, NULL);
    HKL *layouts = count > 0 ? calloc(count, sizeof(*layouts)) : NULL;
    w_menu_row *rows = calloc((size_t)count + 3, sizeof(*rows));
    if (rows) {
        wcscpy(rows[0].label, L"Keyboard layouts");
        count = layouts ? GetKeyboardLayoutList(count, layouts) : 0;
        for (int i = 0; i < count; ++i) {
            w_menu_row *row = &rows[i + 1];
            row->layout = layouts[i];
            row->checked = active == layouts[i];
            row->disabled = !target;
            w_layout_label(layouts[i], row->label, count_of(row->label));
        }
        rows[count + 1] = (w_menu_row){.label = L"Add languages and keyboards\u2026", .action = "languages"};
        rows[count + 2] = (w_menu_row){.label = L"Set up keyboard layouts\u2026", .action = "keyboard_settings"};
        w_show(b, hit, rows, count + 3, target);
    }
    free(rows);
    free(layouts);
}

void w_bar_cycle_layout(int steps) {
    HWND target = w_bar_input_target();
    int count = GetKeyboardLayoutList(0, NULL);
    if (!target || count < 2)
        return;
    HKL active = GetKeyboardLayout(GetWindowThreadProcessId(target, NULL));
    HKL *layouts = calloc(count, sizeof(*layouts));
    if (layouts) {
        count = GetKeyboardLayoutList(count, layouts);
        for (int i = 0; i < count; i++)
            if (layouts[i] == active) {
                int next = (i + steps % count + count) % count;
                w_bar_menu_close(NULL, true);
                PostMessageW(target, WM_INPUTLANGCHANGEREQUEST, 0, (LPARAM)layouts[next]);
                break;
            }
        free(layouts);
    }
}

bool w_bar_menu_open(w_bar_window *b, int hit) {
    const char *key = b->hits[hit].module;
    if (!strcmp(key, "keyboard")) {
        w_language_menu(b, hit);
        return true;
    }
    if (!strcmp(key, "clock")) {
        w_bar_menu_close(NULL, true);
        b->calendar_date = !b->calendar_date;
        InvalidateRect(b->hwnd, NULL, FALSE);
        return true;
    }
    w_menu_row rows[5] = {0};
    int n = 1;
    if (!strcmp(key, "audio")) {
        if (b->audio_percent < 0)
            wcscpy(rows[0].label, L"No audio output");
        else
            swprintf(rows[0].label, count_of(rows[0].label), L"%ls   %d%%",
                     b->audio_muted ? L"Muted" : L"Output volume", b->audio_percent);
        wcscpy(rows[n].label, b->audio_muted ? L"Unmute" : L"Mute");
        rows[n].disabled = b->audio_percent < 0;
        rows[n++].action = "mute";
        rows[n++] = (w_menu_row){.label = L"Volume mixer\u2026", .action = "volume_mixer"};
        rows[n++] = (w_menu_row){.label = L"Choose sound devices\u2026", .action = "audio"};
    } else if (!strcmp(key, "battery")) {
        wcsncpy(rows[0].label, b->hits[hit].tooltip, count_of(rows[0].label) - 1);
        rows[n++] = (w_menu_row){.label = L"Keep awake", .checked = b->view.inhibit, .action = "idle_inhibit"};
        rows[n++] = (w_menu_row){.label = L"Power settings\u2026", .action = "power_settings"};
    } else
        return false;
    w_show(b, hit, rows, n, NULL);
    return true;
}
