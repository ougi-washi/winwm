#include "bar/menu.h"
#include <stdio.h>
#include <commctrl.h>
#include <wchar.h>

////////////////
//~ Menu

typedef struct w_menu_row {
    wchar_t label[192];
    const char *action;
    HKL layout;
    bool checked;
} w_menu_row;

typedef struct w_flyout {
    w_bar_window *bar;
    HWND window, list, previous, language_target;
    DWORD language_thread;
    int count;
    bool closing, owns_memory;
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
    HWND target = menu->previous;
    bool had_focus = GetForegroundWindow() == menu->window;
    DestroyWindow(menu->window);
    if (restore && had_focus && target && IsWindowVisible(target))
        SetForegroundWindow(target);
}

void w_bar_menu_close(const w_bar_window *bar, bool restore) {
    if (open_menu && (!bar || bar == open_menu->bar))
        w_dismiss(open_menu, restore);
}

static void w_pick(w_flyout *menu, int index) {
    if (index < 0 || index + 1 >= menu->count)
        return;
    w_menu_row selected = menu->rows[index + 1];
    HWND target = menu->language_target;
    DWORD thread = menu->language_thread;
    w_bar_window *bar = menu->bar;
    if (selected.action && (!strcmp(selected.action, "volume_up") ||
                            !strcmp(selected.action, "volume_down") || !strcmp(selected.action, "mute"))) {
        bar->invoke(bar->output, selected.action);
        return;
    }
    w_dismiss(menu, true);
    if (selected.layout && IsWindow(target) && GetWindowThreadProcessId(target, NULL) == thread)
        PostMessageW(target, WM_INPUTLANGCHANGEREQUEST, 0, (LPARAM)selected.layout);
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
        if (wp == VK_RETURN || wp == VK_SPACE) {
            w_pick(menu, (int)SendMessageW(list, LB_GETCURSEL, 0, 0));
            return 0;
        }
    }
    if (message == WM_MOUSEMOVE) {
        LRESULT hit = SendMessageW(list, LB_ITEMFROMPOINT, 0, lp);
        if (!HIWORD(hit))
            SendMessageW(list, LB_SETCURSEL, LOWORD(hit), 0);
    }
    if (message == WM_LBUTTONUP) {
        LRESULT hit = SendMessageW(list, LB_ITEMFROMPOINT, 0, lp);
        LRESULT result = DefSubclassProc(list, message, wp, lp);
        if (menu == open_menu && !HIWORD(hit))
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
        r.left += w_scale(bar, 14);
        r.right -= w_scale(bar, 8);
        r.bottom = w_scale(bar, 38);
        HGDIOBJ old = SelectObject(dc, bar->font);
        SetTextColor(dc, w_color(cfg->accent));
        SetBkMode(dc, TRANSPARENT);
        DrawTextW(dc, menu->rows[0].label, -1, &r,
                  DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
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
        w_fill(item->hDC, r, w_color(selected ? cfg->accent : cfg->background));
        SetBkMode(item->hDC, TRANSPARENT);
        SetTextColor(item->hDC, w_color(selected ? cfg->background : cfg->foreground));
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
    w_str_copy(menu->module, sizeof(menu->module), bar->hits[hit].module);
    memcpy(menu->rows, rows, (size_t)count * sizeof(*rows));

    RECT anchor = bar->hits[hit].box;
    MapWindowPoints(bar->hwnd, HWND_DESKTOP, (POINT *)&anchor, 2);
    int margin = w_scale(bar, 5), row_height = w_scale(bar, 31), header = w_scale(bar, 38);
    HDC dc = GetDC(bar->hwnd);
    HGDIOBJ old = SelectObject(dc, bar->font);
    int width = w_scale(bar, 194);
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
        CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName, rows[0].label,
                        WS_POPUP | WS_CLIPCHILDREN, x, y, width, height, bar->hwnd, NULL, instance, menu);
    if (!popup) {
        free(menu);
        return;
    }
    menu->owns_memory = true;
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
    ShowWindow(popup, SW_SHOW);
    SetForegroundWindow(popup);
    if (IsWindow(popup))
        SetFocus(GetWindow(popup, GW_CHILD));
}

void w_bar_menu_audio_error(void) {
    if (w_bar_menu_active() && !strcmp(open_menu->module, "audio")) {
        wcscpy(open_menu->rows[0].label, L"Output rejected the change");
        SetWindowTextW(open_menu->window, open_menu->rows[0].label);
        InvalidateRect(open_menu->window, NULL, FALSE);
    }
}

void w_bar_menu_audio_changed(w_bar_window *b) {
    w_flyout *menu = open_menu;
    if (!menu || menu->bar != b || strcmp(menu->module, "audio"))
        return;
    if ((b->audio_percent >= 0) != (menu->count == 5)) {
        w_dismiss(menu, true);
        return;
    }
    swprintf(menu->rows[0].label, count_of(menu->rows[0].label), L"%ls  %d%%",
             b->audio_muted ? L"Muted" : L"Volume", b->audio_percent);
    SetWindowTextW(menu->window, menu->rows[0].label);
    const wchar_t *label = b->audio_muted ? L"Unmute" : L"Mute";
    if (menu->count == 5 && wcscmp(label, menu->rows[3].label)) {
        int selected = (int)SendMessageW(menu->list, LB_GETCURSEL, 0, 0);
        wcscpy(menu->rows[3].label, label);
        SendMessageW(menu->list, LB_DELETESTRING, 2, 0);
        SendMessageW(menu->list, LB_INSERTSTRING, 2, (LPARAM)label);
        SendMessageW(menu->list, LB_SETCURSEL, selected, 0);
    }
    InvalidateRect(menu->window, NULL, FALSE);
}

////////////////
//~ Language selection

static void w_language_menu(w_bar_window *b, int hit) {
    HWND target = w_bar_menu_active() ? open_menu->previous : GetForegroundWindow();
    if (!target)
        return;
    DWORD thread = GetWindowThreadProcessId(target, NULL);
    GUITHREADINFO gui = {.cbSize = sizeof(gui)};
    if (GetGUIThreadInfo(thread, &gui) && gui.hwndFocus)
        target = gui.hwndFocus;
    HKL active = GetKeyboardLayout(thread);
    int count = GetKeyboardLayoutList(0, NULL);
    if (count <= 0)
        return;
    HKL *layouts = calloc(count, sizeof(*layouts));
    w_menu_row *rows = calloc((size_t)count + 1, sizeof(*rows));
    if (layouts && rows) {
        wcscpy(rows[0].label, L"Input language");
        count = GetKeyboardLayoutList(count, layouts);
        for (int i = 0; i < count; ++i) {
            w_menu_row *row = &rows[i + 1];
            row->layout = layouts[i];
            row->checked = active == layouts[i];
            wchar_t language[128] = {0};
            LANGID id = LOWORD((uintptr_t)layouts[i]);
            GetLocaleInfoW(MAKELCID(id, SORT_DEFAULT), LOCALE_SLOCALIZEDDISPLAYNAME, language,
                           count_of(language));
            int variants = 0, variant = 0;
            for (int j = 0; j < count; ++j)
                if (LOWORD((uintptr_t)layouts[j]) == id) {
                    ++variants;
                    variant += j <= i;
                }
            if (!*language)
                swprintf(language, count_of(language), L"Keyboard %d", i + 1);
            if (variants > 1)
                swprintf(row->label, count_of(row->label), L"%ls  (%d/%d)", language, variant, variants);
            else
                wcscpy(row->label, language);
        }
        w_show(b, hit, rows, count + 1, target);
    }
    free(rows);
    free(layouts);
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
        wcsncpy(rows[0].label, b->hits[hit].tooltip, count_of(rows[0].label) - 1);
        if (b->audio_percent >= 0) {
            rows[n++] = (w_menu_row){.label = L"Volume up", .action = "volume_up"};
            rows[n++] = (w_menu_row){.label = L"Volume down", .action = "volume_down"};
            wcscpy(rows[n].label, b->audio_muted ? L"Unmute" : L"Mute");
            rows[n++].action = "mute";
        }
        rows[n++] = (w_menu_row){.label = L"Audio devices", .action = "audio"};
    } else if (!strcmp(key, "network")) {
        wcsncpy(rows[0].label, b->hits[hit].tooltip, count_of(rows[0].label) - 1);
        rows[n++] = (w_menu_row){.label = L"Wi-Fi profiles", .action = "wifi"};
    } else if (!strcmp(key, "bluetooth")) {
        wcscpy(rows[0].label, L"Bluetooth");
        rows[n++] = (w_menu_row){.label = L"Paired devices", .action = "bluetooth"};
    } else if (!strcmp(key, "battery")) {
        wcsncpy(rows[0].label, b->hits[hit].tooltip, count_of(rows[0].label) - 1);
        wcscpy(rows[n].label, b->view.inhibit ? L"Resume auto sleep" : L"Keep awake");
        rows[n++].action = "idle_inhibit";
    } else
        return false;
    w_show(b, hit, rows, n, NULL);
    return true;
}
