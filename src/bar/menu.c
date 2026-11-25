#include "bar/menu.h"
#include "theme/icon_button.h"
#include "theme/theme.h"
#include "win_platform/percent_slider.h"
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
    wchar_t glyph;
    bool checked, disabled, opens_panel;
} w_menu_row;

typedef struct w_playback_button {
    const wchar_t *glyph, *label;
    const char *action;
} w_playback_button;

static const w_playback_button playback[] = {
    {L"\xe892", L"Previous track", "media_previous"},
    {L"\xe768\xe769", L"Play or pause", "media_play"},
    {L"\xe893", L"Next track", "media_next"},
    {L"\xe71a", L"Stop playback", "media_stop"},
};

typedef struct w_flyout {
    w_bar_window *bar;
    HWND window, list, slider, previous, language_target;
    HWND mute, transport[count_of(playback)];
    HFONT heading, symbols;
    COLORREF surface, ink, accent, border, selected, selected_ink;
    DWORD language_thread;
    int count, pressed_row, source;
    w_popup_anchor anchor;
    bar_icon icon;
    bool closing, owns_memory;
    w_layer_fade fade;
    char module[56], instance[56];
    w_menu_row rows[];
} w_flyout;

static w_flyout *open_menu;
static HWND fading_menu;
static HWND last_foreground;
static DWORD last_foreground_thread;

////////////////
//~ Menu Functions

static COLORREF w_color(uint32_t rgb) {
    return RGB(rgb >> 16, rgb >> 8, rgb);
}

static int w_scale(const w_bar_window *bar, int pixels) {
    return MulDiv(pixels, bar->dpi, 96);
}

static void w_menu_colors(w_flyout *menu) {
    const w_desktop_config *cfg = menu->bar->config;
    HIGHCONTRASTW hc = {.cbSize = sizeof(hc)};
    bool contrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(hc),&hc,0) && (hc.dwFlags & HCF_HIGHCONTRASTON);
    menu->surface = contrast ? GetSysColor(COLOR_WINDOW) : w_color(cfg->background);
    menu->ink = contrast ? GetSysColor(COLOR_WINDOWTEXT) : w_color(cfg->foreground);
    menu->accent = contrast ? menu->ink : w_color(cfg->accent);
    menu->border = contrast ? menu->ink : w_color(cfg->inactive_border);
    menu->selected = contrast ? GetSysColor(COLOR_HIGHLIGHT) :
        RGB((3*GetRValue(menu->surface)+GetRValue(menu->accent))/4,
            (3*GetGValue(menu->surface)+GetGValue(menu->accent))/4,
            (3*GetBValue(menu->surface)+GetBValue(menu->accent))/4);
    menu->selected_ink = contrast ? GetSysColor(COLOR_HIGHLIGHTTEXT) : menu->ink;
}

bool w_bar_menu_active(void) {
    return open_menu && !open_menu->closing;
}

bool w_bar_menu_at(const w_bar_window *bar, const struct w_bar_hit *hit) {
    return open_menu && open_menu->bar == bar && open_menu->source == hit->source &&
           !strcmp(open_menu->instance,hit->instance);
}

HWND w_bar_remember_foreground(void) {
    if (open_menu) return open_menu->previous;
    HWND foreground = GetForegroundWindow();
    bool transient = false;
    for (HWND w = foreground; w && !transient; w = GetWindow(w,GW_OWNER))
        transient = GetPropW(w,L"winwm.popup_bar") != NULL || w == fading_menu;
    if (foreground && !transient && w_os_same_process(foreground)) {
        wchar_t name[48];
        transient = GetClassNameW(foreground,name,count_of(name)) && !wcscmp(name,L"WinwmPalette");
    }
    // Switching toolbar panels must keep the input layout directed at the application.
    if (transient)
        return last_foreground && GetWindowThreadProcessId(last_foreground,NULL) == last_foreground_thread &&
               IsWindowVisible(last_foreground) ? last_foreground : NULL;
    last_foreground = foreground;
    last_foreground_thread = foreground ? GetWindowThreadProcessId(foreground,NULL) : 0;
    return foreground;
}

static void w_dismiss(w_flyout *menu, bool restore) {
    if (!menu || menu->closing)
        return;
    menu->closing = true;
    if (open_menu == menu)
        open_menu = NULL;
    InvalidateRect(menu->bar->hwnd, NULL, FALSE);
    HWND target = menu->previous;
    HWND hwnd = menu->window;
    fading_menu = hwnd;
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

bool w_bar_menu_close(const w_bar_window *bar, bool restore) {
    if (!open_menu || (bar && bar != open_menu->bar))
        return false;
    w_dismiss(open_menu, restore);
    return true;
}

void w_bar_menu_outside(HWND hit) {
    if (!open_menu) return;
    HWND root = GetAncestor(hit,GA_ROOT);
    if (root == open_menu->window) return;
    wchar_t name[48];
    if (root && GetClassNameW(root,name,count_of(name)) && !wcscmp(name,L"WinwmBar")) return;
    w_dismiss(open_menu,false);
}

static void w_pick(w_flyout *menu, int index) {
    if (menu->closing || index < 0 || index + 1 >= menu->count)
        return;
    w_menu_row selected = menu->rows[index + 1];
    if (selected.disabled)
        return;
    HWND target = menu->language_target;
    DWORD thread = menu->language_thread;
    w_bar_window *bar = menu->bar;
    w_popup_anchor anchor = menu->anchor;
    for (int i = 0; i < bar->hit_count; i++)
        if (w_bar_menu_at(bar,&bar->hits[i])) {
            w_bar_anchor(bar,i,&anchor);
            break;
        }
    w_dismiss(menu, true);
    if (selected.layout) {
        if (!IsWindow(target) || GetWindowThreadProcessId(target, NULL) != thread ||
            !PostMessageW(target, WM_INPUTLANGCHANGEREQUEST, 0, (LPARAM)selected.layout))
            bar->invoke(bar->output, "notify Focus an application before switching input layout", &anchor);
    }
    else if (selected.action)
        bar->invoke(bar->output, selected.action, &anchor);
}

static LRESULT CALLBACK w_flyout_control(HWND control, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id,
                                       DWORD_PTR data) {
    w_flyout *menu = (w_flyout *)data;
    if (message == WM_KEYDOWN) {
        if (wp == VK_ESCAPE) {
            w_dismiss(menu, true);
            return 0;
        }
        if (wp == VK_TAB) {
            HWND next = GetNextDlgTabItem(menu->window, control, GetKeyState(VK_SHIFT) < 0);
            if (next) SetFocus(next);
            return 0;
        }
        if (control == menu->list && (wp == VK_RETURN || wp == VK_SPACE)) {
            w_pick(menu, (int)SendMessageW(control, LB_GETCURSEL, 0, 0));
            return 0;
        }
        if (control != menu->list && control != menu->slider && wp == VK_RETURN) {
            if (!(lp & (1L << 30))) SendMessageW(control,BM_CLICK,0,0);
            return 0;
        }
    }
    if (control == menu->list && (message == WM_MOUSEMOVE || message == WM_LBUTTONDOWN)) {
        LRESULT hit = SendMessageW(control, LB_ITEMFROMPOINT, 0, lp);
        if (message == WM_LBUTTONDOWN)
            menu->pressed_row = HIWORD(hit) ? -1 : (int)LOWORD(hit);
        else if (!HIWORD(hit))
            SendMessageW(control, LB_SETCURSEL, LOWORD(hit), 0);
    }
    if (control == menu->list && message == WM_LBUTTONUP) {
        LRESULT hit = SendMessageW(control, LB_ITEMFROMPOINT, 0, lp);
        int pressed = menu->pressed_row;
        menu->pressed_row = -1;
        LRESULT result = DefSubclassProc(control, message, wp, lp);
        if (menu == open_menu && !HIWORD(hit) && pressed == (int)LOWORD(hit))
            w_pick(menu, LOWORD(hit));
        return result;
    }
    if (control == menu->list && (message == WM_CANCELMODE || message == WM_CAPTURECHANGED))
        menu->pressed_row = -1;
    if (message == WM_NCDESTROY)
        RemoveWindowSubclass(control, w_flyout_control, id);
    return DefSubclassProc(control, message, wp, lp);
}

static void w_fill(HDC dc, RECT r, COLORREF value) {
    SetDCBrushColor(dc, value);
    FillRect(dc, &r, GetStockObject(DC_BRUSH));
}

static void w_rounded_fill(HDC dc, RECT r, COLORREF color, int radius) {
    if (!radius) {
        w_fill(dc,r,color);
        return;
    }
    HGDIOBJ brush = SelectObject(dc,GetStockObject(DC_BRUSH));
    HGDIOBJ pen = SelectObject(dc,GetStockObject(NULL_PEN));
    SetDCBrushColor(dc,color);
    RoundRect(dc,r.left,r.top,r.right,r.bottom,2*radius,2*radius);
    SelectObject(dc,pen);
    SelectObject(dc,brush);
}

// Owner drawing supplies the glyphs. Keep their meaning in the native list's
// accessible text as well, including when the keyboard focus is on another row.
static void w_row_name(const w_menu_row *row, wchar_t text[256]) {
    swprintf(text,256,L"%ls%ls%ls",row->label,row->checked ? (row->layout ? L" (current layout)" : L" (on)") : L"",
             row->disabled ? L" (unavailable)" : L"");
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
    menu->bar->invoke(menu->bar->output, action, &menu->anchor);
}

static const wchar_t *w_mute_label(const w_bar_window *bar) {
    return bar->audio_percent < 0 ? L"No audio output" : bar->audio_muted ? L"Unmute output" : L"Mute output";
}

static HWND w_flyout_button(w_flyout *menu, const wchar_t *glyph, const wchar_t *label,
                            int x, int y, int width, int height) {
    HWND button = CreateWindowW(L"BUTTON", label, WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,
                                x, y, width, height, menu->window, NULL, GetModuleHandleW(NULL), NULL);
    if (!button || !SetWindowSubclass(button,w_flyout_control,1,(DWORD_PTR)menu)) return NULL;
    w_icon_button(button,glyph,label);
    return button;
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
    switch (message) {
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        // The anchor and its fonts belong to the bar's output.
        if (menu == open_menu) w_dismiss(menu,true);
        return 0;
    case WM_COMMAND:
        if (menu->closing || HIWORD(wp) != BN_CLICKED || !lp) break;
        if ((HWND)lp == menu->mute && IsWindowEnabled(menu->mute)) {
            bar->invoke(bar->output,"mute",&menu->anchor);
            return 0;
        }
        for (size_t i = 0; i < count_of(playback); ++i) {
            if ((HWND)lp == menu->transport[i]) {
                bar->invoke(bar->output,playback[i].action,&menu->anchor);
                return 0;
            }
        }
        break;
    case WM_SYSCOLORCHANGE:
    case WM_SETTINGCHANGE:
        w_menu_colors(menu);
        RedrawWindow(w,NULL,NULL,RDW_INVALIDATE|RDW_ALLCHILDREN);
        break;
    case WM_TIMER:
        if (wp == bar_fade_timer && menu->fade.start &&
            !w_bar_fade_tick(w,&menu->fade,bar->motion_allowed) && menu->closing)
            DestroyWindow(w);
        return 0;
    case WM_CLOSE:
        w_dismiss(menu,true);
        return 0;
    case WM_HSCROLL:
        if (!menu->closing && menu->slider && (HWND)lp == menu->slider && IsWindowEnabled(menu->slider) &&
            LOWORD(wp) != TB_ENDTRACK)
            w_request_volume(menu, (int)SendMessageW(menu->slider, TBM_GETPOS, 0, 0));
        return 0;
    case WM_NOTIFY:
        if (lp && menu->slider && ((NMHDR *)lp)->hwndFrom == menu->slider && ((NMHDR *)lp)->code == NM_CUSTOMDRAW)
            return w_percent_slider_draw(menu->slider,(NMCUSTOMDRAW *)lp,menu->surface,menu->border,menu->accent,menu->ink);
        break;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE)
            w_dismiss(menu, false);
        return 0;
    case WM_CTLCOLORLISTBOX:
        SetDCBrushColor((HDC)wp, menu->surface);
        return (LRESULT)GetStockObject(DC_BRUSH);
    case WM_ERASEBKGND:
        return TRUE;
    case WM_PAINT:
    case WM_PRINTCLIENT: {
        PAINTSTRUCT ps;
        HDC dc = message == WM_PAINT ? BeginPaint(w,&ps) : (HDC)wp;
        RECT r;
        GetClientRect(w, &r);
        w_fill(dc, r, menu->border);
        InflateRect(&r, -1, -1);
        w_rounded_fill(dc,r,menu->surface,w_scale(bar,min(12,bar->config->rounding)));
        RECT list_rect;
        if (menu->list && GetWindowRect(menu->list,&list_rect)) {
            MapWindowPoints(HWND_DESKTOP,w,(POINT *)&list_rect,2);
            RECT line = {r.left+w_scale(bar,14),list_rect.top-1,r.right-w_scale(bar,14),list_rect.top};
            w_fill(dc,line,menu->border);
        }
        r.left += w_scale(bar, 16);
        r.right -= w_scale(bar, 16);
        r.bottom = w_scale(bar, 38);
        HGDIOBJ old = SelectObject(dc, menu->heading ? menu->heading : bar->font);
        SetTextColor(dc, menu->ink);
        SetBkMode(dc, TRANSPARENT);
        if (menu->icon && bar->icons.size > 0) {
            RECT glyph = r;
            glyph.right = glyph.left + bar->icons.size;
            w_icons_draw(&bar->icons,dc,menu->icon,glyph,menu->accent,menu->surface);
            r.left = glyph.right + w_scale(bar,9);
        }
        const wchar_t *heading = menu->rows[0].label;
        if (menu->slider && bar->audio_percent >= 0 && wcschr(heading, L'%')) {
            wchar_t percentage[12];
            swprintf(percentage, count_of(percentage), L"%d%%", (int)SendMessageW(menu->slider, TBM_GETPOS, 0, 0));
            SetTextColor(dc, menu->accent);
            DrawTextW(dc, percentage, -1, &r, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
            SetTextColor(dc, menu->ink);
            r.right -= w_scale(bar, 52);
            heading = bar->audio_muted ? L"Output muted" : L"Output volume";
        }
        DrawTextW(dc, heading, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        SelectObject(dc, old);
        if (message == WM_PAINT) EndPaint(w,&ps);
        return 0;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *item = (DRAWITEMSTRUCT *)lp;
        if (item->CtlType == ODT_BUTTON) {
            LRESULT result;
            if (w_theme_message(w,message,wp,lp,&result)) return result;
        }
        if (item->CtlType != ODT_LISTBOX || item->itemID >= (UINT)(menu->count - 1))
            return FALSE;
        w_menu_row *row = &menu->rows[item->itemID + 1];
        RECT r = item->rcItem;
        bool selected = !!(item->itemState & ODS_SELECTED);
        w_fill(item->hDC, r, menu->surface);
        if (selected) {
            InflateRect(&r, -w_scale(bar, 2), -w_scale(bar, 2));
            w_rounded_fill(item->hDC,r,menu->selected,w_scale(bar,min(7,bar->config->rounding)));
        }
        SetBkMode(item->hDC, TRANSPARENT);
        COLORREF ink = row->disabled ? GetSysColor(COLOR_GRAYTEXT) : selected ? menu->selected_ink : menu->ink;
        SetTextColor(item->hDC, selected || row->disabled ? ink : menu->accent);
        HGDIOBJ old = SelectObject(item->hDC, menu->symbols ? menu->symbols : bar->font);
        r.left = item->rcItem.left + w_scale(bar,12);
        r.right = item->rcItem.right - w_scale(bar,12);
        wchar_t symbol = row->checked ? 0xe73e : row->glyph;
        if (symbol) DrawTextW(item->hDC,&symbol,1,&r,DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX);
        if (row->opens_panel) DrawTextW(item->hDC,L"\xe76c",1,&r,DT_SINGLELINE|DT_VCENTER|DT_RIGHT);
        SetTextColor(item->hDC,ink);
        SelectObject(item->hDC,bar->font);
        r.left += w_scale(bar,26);
        if (row->opens_panel) r.right -= w_scale(bar,22);
        DrawTextW(item->hDC, row->label, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        if ((item->itemState & (ODS_FOCUS|ODS_NOFOCUSRECT)) == ODS_FOCUS &&
            !(SendMessageW(menu->list,WM_QUERYUISTATE,0,0)&UISF_HIDEFOCUS))
            DrawFocusRect(item->hDC,&item->rcItem);
        SelectObject(item->hDC, old);
        return TRUE;
    }
    case WM_NCDESTROY:
        if (open_menu == menu)
            open_menu = NULL;
        if (fading_menu == w)
            fading_menu = NULL;
        SetWindowLongPtrW(w, GWLP_USERDATA, 0);
        DeleteObject(menu->heading);
        DeleteObject(menu->symbols);
        if (menu->owns_memory)
            free(menu);
        break;
    }
    return DefWindowProcW(w, message, wp, lp);
}

static void w_show(w_bar_window *bar, int hit, const w_menu_row *rows, int count, HWND language_target) {
    if (count < 2)
        return;
    HWND previous = w_bar_remember_foreground();
    if (open_menu) {
        bool same = w_bar_menu_at(bar,&bar->hits[hit]);
        w_dismiss(open_menu, true);
        if (same)
            return;
    }
    if (fading_menu) DestroyWindow(fading_menu);
    w_flyout *menu = calloc(1, sizeof(*menu) + (size_t)count * sizeof(*rows));
    if (!menu)
        return;
    menu->bar = bar;
    w_bar_anchor(bar,hit,&menu->anchor);
    menu->previous = previous;
    bar->invoke(bar->output,NULL,&menu->anchor);
    w_menu_colors(menu);
    menu->language_target = language_target;
    if (language_target)
        menu->language_thread = GetWindowThreadProcessId(language_target, NULL);
    menu->count = count;
    menu->pressed_row = -1;
    menu->source = bar->hits[hit].source;
    menu->icon = bar->hits[hit].icon;
    w_str_copy(menu->module, sizeof(menu->module), bar->hits[hit].module);
    w_str_copy(menu->instance, sizeof(menu->instance), bar->hits[hit].instance);
    memcpy(menu->rows, rows, (size_t)count * sizeof(*rows));

    bool audio = !strcmp(menu->module, "audio");
    bool media = !strcmp(menu->module, "media");
    int margin = w_scale(bar, 6), row_height = w_scale(bar, 35), header = w_scale(bar, audio || media ? 76 : 38);
    HDC dc = GetDC(bar->hwnd);
    HGDIOBJ old = SelectObject(dc, bar->font);
    int width = w_scale(bar, audio ? 288 : 236);
    for (int i = 0; i < count; ++i) {
        SIZE size = {0};
        GetTextExtentPoint32W(dc, rows[i].label, (int)wcslen(rows[i].label), &size);
        width = max(width, size.cx + w_scale(bar, 84));
    }
    SelectObject(dc, old);
    ReleaseDC(bar->hwnd, dc);
    int height = header + min(10, count - 1) * row_height + margin + 1;
    RECT bounds = w_popup_bounds(&menu->anchor,w_popup_area(bar->hwnd),width,height,margin);
    width = bounds.right - bounds.left;
    height = bounds.bottom - bounds.top;
    HINSTANCE instance = GetModuleHandleW(NULL);
    WNDCLASSW wc = {.lpfnWndProc = w_flyout_proc,
                    .hInstance = instance,
                    .hCursor = LoadCursorW(NULL, IDC_ARROW),
                    .lpszClassName = L"WinwmBarFlyout"};
    RegisterClassW(&wc);
    HWND popup =
        CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName, rows[0].label,
                        WS_POPUP | WS_CLIPCHILDREN, bounds.left, bounds.top, width, height, bar->hwnd, NULL, instance, menu);
    if (!popup) {
        free(menu);
        return;
    }
    menu->owns_memory = true;
    int radius = w_scale(bar,min(12,bar->config->rounding));
    if (radius) {
        HRGN shape = CreateRoundRectRgn(0,0,width+1,height+1,2*radius,2*radius);
        if (shape && !SetWindowRgn(popup,shape,FALSE)) DeleteObject(shape);
    }
    menu->heading = CreateFontW(-w_scale(bar, 15), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    menu->symbols = CreateFontW(-w_scale(bar,15),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,0,L"Segoe MDL2 Assets");
    if (audio) {
        int x = margin, y = w_scale(bar,36), size = min(w_scale(bar,34),max(1,width/4));
        menu->mute = w_flyout_button(menu,bar->audio_muted ? L"\xe74f" : L"\xe767",w_mute_label(bar),
                                     x,y,size,w_scale(bar,34));
        if (!menu->mute) { DestroyWindow(popup); return; }
        EnableWindow(menu->mute,bar->audio_percent >= 0);
        INITCOMMONCONTROLSEX init = {sizeof(init), ICC_BAR_CLASSES};
        InitCommonControlsEx(&init);
        x += size + margin;
        menu->slider = CreateWindowExW(0, TRACKBAR_CLASSW, L"Output volume",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS,
                        x, y, max(1,width - margin - x), w_scale(bar, 34),
                        popup, NULL, instance, NULL);
        if (!w_percent_slider_init(menu->slider) || !SetWindowSubclass(menu->slider, w_flyout_control, 1, (DWORD_PTR)menu)) {
            DestroyWindow(popup);
            return;
        }
        SendMessageW(menu->slider, TBM_SETPOS, TRUE, max(0, bar->audio_percent));
        EnableWindow(menu->slider, bar->audio_percent >= 0);
    } else if (media) {
        int n = (int)count_of(playback), span = max(n,width-(n+1)*margin);
        for (int i = 0; i < n; ++i) {
            int x = (i+1)*margin + i*span/n;
            menu->transport[i] = w_flyout_button(menu,playback[i].glyph,playback[i].label,x,w_scale(bar,36),
                                                 (i+1)*span/n - i*span/n,w_scale(bar,34));
            if (!menu->transport[i]) { DestroyWindow(popup); return; }
        }
    }
    menu->list = CreateWindowExW(0, L"LISTBOX", rows[0].label,
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_OWNERDRAWFIXED |
                                     LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
                                 margin, header, max(1,width - 2 * margin), max(1,height - header - margin), popup, NULL,
                                 instance, NULL);
    if (!menu->list || !SetWindowSubclass(menu->list, w_flyout_control, 1, (DWORD_PTR)menu)) {
        DestroyWindow(popup);
        return;
    }
    SendMessageW(menu->list, WM_SETFONT, (WPARAM)bar->font, FALSE);
    SendMessageW(menu->list, LB_SETITEMHEIGHT, 0, row_height);
    int selected = 0;
    for (int i = 1; i < count; ++i) {
        wchar_t text[256];
        w_row_name(&rows[i],text);
        SendMessageW(menu->list, LB_ADDSTRING, 0, (LPARAM)text);
        if (rows[i].checked)
            selected = i - 1;
    }
    if (rows[selected+1].disabled) {
        for (int i=1; i<count; ++i) if (!rows[i].disabled) { selected=i-1; break; }
    }
    SendMessageW(menu->list, LB_SETCURSEL, selected, 0);
    open_menu = menu;
    InvalidateRect(bar->hwnd,&bar->hits[hit].box,FALSE);
    w_bar_fade(popup,&menu->fade,255,bar->config->motion[motion_layers_in],bar->motion_allowed);
    ShowWindow(popup, SW_SHOW);
    SetForegroundWindow(popup);
    if (open_menu == menu && IsWindow(popup))
        SetFocus(media ? menu->transport[1] : audio && IsWindowEnabled(menu->slider) ? menu->slider : menu->list);
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
    if (!available && (GetFocus() == menu->mute || GetFocus() == menu->slider))
        SetFocus(menu->list);
    w_icon_button(menu->mute,b->audio_muted ? L"\xe74f" : L"\xe767",w_mute_label(b));
    EnableWindow(menu->mute,available);
    EnableWindow(menu->slider, available);
    if (!available || GetCapture() != menu->slider) {
        SendMessageW(menu->slider, TBM_SETPOS, TRUE, max(0, b->audio_percent));
        w_volume_header(menu, b->audio_percent);
    }
}

////////////////
//~ Language selection

HWND w_bar_input_target(void) {
    HWND target = w_bar_remember_foreground();
    if (!target)
        return NULL;
    DWORD thread = GetWindowThreadProcessId(target, NULL);
    GUITHREADINFO gui = {.cbSize = sizeof(gui)};
    if (GetGUIThreadInfo(thread, &gui) && gui.hwndFocus && GetAncestor(gui.hwndFocus,GA_ROOT) == target)
        target = gui.hwndFocus;
    return target;
}

// Windows encodes alternate layouts as Fxxx handles; their registry entries
// identify the matching variant with "Layout Id" (for example US Dvorak).
void w_bar_layout_label(HKL layout, wchar_t *out, size_t cap) {
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
    w_menu_row *rows = calloc((size_t)count + 5, sizeof(*rows));
    if (rows) {
        wcscpy(rows[0].label, L"Languages & input");
        count = layouts ? GetKeyboardLayoutList(count, layouts) : 0;
        for (int i = 0; i < count; ++i) {
            w_menu_row *row = &rows[i + 1];
            row->layout = layouts[i];
            row->checked = active == layouts[i];
            row->disabled = !target;
            w_bar_layout_label(layouts[i], row->label, count_of(row->label));
        }
        rows[count + 1] = (w_menu_row){.label=L"Add or remove languages", .action="languages", .glyph=0xe710, .opens_panel=true};
        rows[count + 2] = (w_menu_row){.label=L"Manage keyboard layouts", .action="keyboard_settings", .glyph=0xe765, .opens_panel=true};
        rows[count + 3] = (w_menu_row){.label=L"Input methods and typing", .action="input_methods", .glyph=0xe8d2, .opens_panel=true};
        rows[count + 4] = (w_menu_row){.label=L"Get Google Japanese Input", .action="google_japanese_input", .glyph=0xe896, .opens_panel=true};
        w_show(b, hit, rows, count + 5, target);
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
    w_menu_row rows[9] = {0};
    int n = 1;
    if (!strcmp(key, "audio")) {
        if (b->audio_percent < 0)
            wcscpy(rows[0].label, L"No audio output");
        else
            swprintf(rows[0].label, count_of(rows[0].label), L"%ls   %d%%",
                     b->audio_muted ? L"Muted" : L"Output volume", b->audio_percent);
        rows[n++] = (w_menu_row){.label=L"Output devices", .action="audio", .glyph=0xe7f5, .opens_panel=true};
        rows[n++] = (w_menu_row){.label=L"Microphones", .action="microphone", .glyph=0xe720, .opens_panel=true};
        rows[n++] = (w_menu_row){.label=L"App volume mixer", .action="volume_mixer", .glyph=0xe9e9, .opens_panel=true};
    } else if (!strcmp(key, "battery")) {
        wcscpy(rows[0].label,L"Power and session");
        rows[n++] = (w_menu_row){.label=L"Keep awake", .checked=b->view.inhibit, .action="idle_inhibit", .glyph=0xe7e8};
        rows[n++] = (w_menu_row){.label=L"Power plans and timeouts", .action="power_settings", .glyph=0xe83e, .opens_panel=true};
        rows[n++] = (w_menu_row){.label=L"Configure toolbar", .action="bar_settings", .glyph=0xe713, .opens_panel=true};
        rows[n++] = (w_menu_row){.label=L"Lock", .action="lock", .glyph=0xe72e};
        rows[n++] = (w_menu_row){.label=L"Sleep\u2026", .action="sleep", .glyph=0xe708};
        rows[n++] = (w_menu_row){.label=L"Restart\u2026", .action="restart", .glyph=0xe777};
        rows[n++] = (w_menu_row){.label=L"Sign out\u2026", .action="confirm_sign_out", .glyph=0xf3b1};
    } else if (!strcmp(key, "media")) {
        wcscpy(rows[0].label, L"Playback");
        rows[n++] = (w_menu_row){.label=L"App volume mixer", .action="volume_mixer", .glyph=0xe9e9, .opens_panel=true};
    } else
        return false;
    w_show(b, hit, rows, n, NULL);
    return true;
}
