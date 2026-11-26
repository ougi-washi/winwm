#include "bar/menu.h"
#include "theme/icon_button.h"
#include "theme/hover.h"
#include "theme/control_hover.h"
#include "theme/theme.h"
#include "win_platform/percent_slider.h"
#include "win_platform/appearance.h"
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
    HFONT font, heading, symbols;
    COLORREF surface, ink, accent, border;
    DWORD language_thread;
    int count, pressed_row, button, gap, dpi, glyph_size, header_height, row_height, percent_width;
    w_popup_anchor anchor;
    RECT area;
    bar_icon icon;
    bool closing, owns_memory, motion_enabled, pointer_selection;
    w_layer_fade fade;
    char module[56];
    w_menu_row rows[];
} w_flyout;

static w_flyout *open_menu;
static HWND fading_menu;
static HWND last_foreground;
static DWORD last_foreground_thread;

////////////////
//~ Menu Functions

static void w_menu_colors(w_flyout *menu) {
    menu->surface = w_theme_background();
    menu->ink = w_theme_foreground();
    menu->accent = w_theme_accent();
    menu->border = w_theme_separator();
}

bool w_bar_menu_active(void) {
    return open_menu && !open_menu->closing;
}

bool w_bar_menu_at(const w_bar_window *bar, const struct w_bar_hit *hit) {
    return open_menu && open_menu->bar == bar && open_menu->anchor.source == hit->source &&
           open_menu->anchor.workspace==hit->workspace && !strcmp(open_menu->anchor.instance,hit->instance);
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
        !w_layer_fade_to(hwnd,&menu->fade,0,w_theme_layer_timing(false),menu->motion_enabled))
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

static void w_release_row(w_flyout *menu) {
    int previous=menu->pressed_row;
    menu->pressed_row=-1;
    RECT bounds;
    if (previous>=0 && SendMessageW(menu->list,LB_GETITEMRECT,previous,(LPARAM)&bounds)!=LB_ERR)
        InvalidateRect(menu->list,&bounds,FALSE);
}

static LRESULT CALLBACK w_flyout_control(HWND control, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id,
                                       DWORD_PTR data) {
    w_flyout *menu = (w_flyout *)data;
    if (message == WM_KEYDOWN) {
        if (control==menu->list && menu->pointer_selection) {
            menu->pointer_selection=false;
            InvalidateRect(control,NULL,FALSE);
        }
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
    if (control == menu->list && (message == WM_MOUSEMOVE || message == WM_LBUTTONDOWN || message==WM_LBUTTONUP)) {
        LRESULT hit = SendMessageW(control, LB_ITEMFROMPOINT, 0, lp);
        int row=(int)LOWORD(hit);
        RECT bounds;
        bool enabled=!HIWORD(hit) && row<menu->count-1 && !menu->rows[row+1].disabled &&
            SendMessageW(control,LB_GETITEMRECT,row,(LPARAM)&bounds)!=LB_ERR &&
            PtInRect(&bounds,(POINT){GET_X_LPARAM(lp),GET_Y_LPARAM(lp)});
        if (message == WM_LBUTTONDOWN) {
            menu->pressed_row=enabled ? row : -1;
            if (enabled) InvalidateRect(control,&bounds,FALSE);
        } else if (message==WM_LBUTTONUP) {
            bool pick=enabled && menu->pressed_row==row;
            w_release_row(menu);
            LRESULT result=DefSubclassProc(control,message,wp,lp);
            if (pick && menu==open_menu) w_pick(menu,row);
            return result;
        } else if (enabled) {
            bool keyboard=!menu->pointer_selection;
            menu->pointer_selection=true;
            if (SendMessageW(control,LB_GETCURSEL,0,0)!=row) SendMessageW(control,LB_SETCURSEL,row,0);
            else if (keyboard) InvalidateRect(control,&bounds,FALSE);
        }
    }
    if (control == menu->list && (message == WM_CANCELMODE || message == WM_CAPTURECHANGED)) w_release_row(menu);
    if (message == WM_NCDESTROY)
        RemoveWindowSubclass(control, w_flyout_control, id);
    return DefSubclassProc(control, message, wp, lp);
}

static void w_fill(HDC dc, RECT r, COLORREF value) {
    SetDCBrushColor(dc, value);
    FillRect(dc, &r, GetStockObject(DC_BRUSH));
}

// Owner drawing supplies the glyphs. Keep their meaning in the native list's
// accessible text as well, including when the keyboard focus is on another row.
static void w_row_name(const w_menu_row *row, wchar_t text[256]) {
    swprintf(text,256,L"%ls%ls%ls",row->label,row->checked ? (row->layout ? L" (current layout)" : L" (on)") : L"",
             row->disabled ? L" (unavailable)" : L"");
}

static void w_volume_header(w_flyout *menu, int percent) {
    wchar_t label[64];
    if (percent < 0)
        wcscpy(label, L"No audio output");
    else
        swprintf(label, count_of(label), L"%ls   %d%%",
                 menu->bar->audio_muted ? L"Muted" : L"Output volume", percent);
    if (!wcscmp(menu->rows[0].label,label)) return;
    wcscpy(menu->rows[0].label,label);
    SetWindowTextW(menu->window, label);
    RECT header;
    GetClientRect(menu->window,&header);
    header.bottom = menu->header_height + menu->gap;
    InvalidateRect(menu->window, &header, FALSE);
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

static HWND w_flyout_button(w_flyout *menu, const wchar_t *glyph, const wchar_t *label) {
    HWND button = CreateWindowW(L"BUTTON", label, WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,
                                0, 0, 0, 0, menu->window, NULL, GetModuleHandleW(NULL), NULL);
    if (!button || !SetWindowSubclass(button,w_flyout_control,1,(DWORD_PTR)menu)) return NULL;
    w_icon_button(button,glyph,label);
    return button;
}

static int w_transport_columns(int width, int button, int gap) {
    return max(1,min((int)count_of(playback),(width-gap)/(button+gap)));
}

static void w_flyout_fonts(w_flyout *menu) {
    w_bar_window *bar=menu->bar;
    if (menu->dpi!=bar->dpi || !menu->font || !menu->heading) {
        HFONT font=w_bar_create_label_font(bar->dpi);
        HFONT heading=w_theme_role_font(bar->dpi,text_heading);
        if (font && heading) {
            if (menu->list) SendMessageW(menu->list,WM_SETFONT,(WPARAM)font,FALSE);
            DeleteObject(menu->font);
            DeleteObject(menu->heading);
            menu->font=font;
            menu->heading=heading;
            menu->dpi=bar->dpi;
        } else {
            DeleteObject(font);
            DeleteObject(heading);
        }
    }
    int size=w_theme_icon_size(bar->dpi);
    if (menu->glyph_size!=size) {
        HFONT symbols=w_theme_font(L"Segoe MDL2 Assets",size,FW_NORMAL);
        if (symbols) {
            DeleteObject(menu->symbols);
            menu->symbols=symbols;
            menu->glyph_size=size;
        }
    }
}

static RECT w_flyout_bounds(w_flyout *menu) {
    w_bar_window *bar = menu->bar;
    int gap = menu->gap = w_theme_spacing(bar->dpi), button = menu->button = w_theme_button_size(bar->dpi);
    bool audio = !strcmp(menu->module,"audio"), media = !strcmp(menu->module,"media");
    int width=audio ? 7*button+3*gap : media ? (int)count_of(playback)*(button+gap)+gap : button+4*gap;
    int options_width=0;
    HFONT font=menu->font ? menu->font : bar->font,heading=menu->heading ? menu->heading : bar->heading_font;
    menu->header_height=max(button,w_theme_line_height(heading));
    menu->row_height=max(button,w_theme_line_height(font));
    menu->percent_width=w_theme_text_width(heading,L"100%",DT_NOPREFIX);
    HDC dc = GetDC(bar->hwnd);
    if (dc) {
        HGDIOBJ old = SelectObject(dc,font ? font : GetStockObject(DEFAULT_GUI_FONT));
        int glyph=w_theme_icon_size(bar->dpi);
        for (int i = 1; i < menu->count; ++i) {
            SIZE size;
            if (w_theme_text_extent(dc,menu->rows[i].label,(int)wcslen(menu->rows[i].label),&size))
                options_width=max(options_width,size.cx+5*gap+glyph+(menu->rows[i].opens_panel ? glyph+gap : 0));
        }
        const wchar_t *title=audio && bar->audio_percent>=0 ? L"Output volume" : menu->rows[0].label;
        int header=w_theme_text_width(heading,title,DT_NOPREFIX)+4*gap+(menu->icon ? bar->icons.size+gap : 0);
        if (audio && bar->audio_percent>=0) header+=menu->percent_width+gap;
        width=max(width,max(header,options_width));
        SelectObject(dc,old);
        ReleaseDC(bar->hwnd,dc);
    }
    RECT area = menu->area = w_popup_area(bar->hwnd);
    RECT available = w_popup_bounds(&menu->anchor,area,area.right-area.left,area.bottom-area.top,gap);
    int columns = w_transport_columns(available.right-available.left,button,gap);
    int controls = media ? ((int)count_of(playback)+columns-1)/columns : audio;
    int chrome=menu->header_height+2*gap+controls*(button+gap);
    int room=max(0,available.bottom-available.top-chrome);
    int rows=min(min(10,menu->count-1),room/menu->row_height);
    if (rows<menu->count-1) width=max(width,options_width+GetSystemMetricsForDpi(SM_CXVSCROLL,bar->dpi));
    int height=chrome+(rows ? rows*menu->row_height : menu->count>1 ? room : 0);
    return w_popup_bounds(&menu->anchor,area,width,height,gap);
}

void w_bar_menu_reanchor(w_bar_window *bar) {
    w_flyout *menu = open_menu;
    if (!menu || menu->bar != bar) return;
    w_popup_anchor next;
    if (!w_bar_resolve_anchor(bar,&menu->anchor,&next)) {
        w_dismiss(menu,false);
        return;
    }
    RECT area=w_popup_area(bar->hwnd);
    bool moved=menu->dpi!=bar->dpi || !EqualRect(&next.button,&menu->anchor.button) ||
               !EqualRect(&area,&menu->area) || strcmp(next.edge,menu->anchor.edge);
    menu->anchor=next;
    if (moved) {
        if (menu->dpi!=bar->dpi) w_flyout_fonts(menu);
        RECT r=w_flyout_bounds(menu);
        SetWindowPos(menu->window,NULL,r.left,r.top,r.right-r.left,r.bottom-r.top,SWP_NOACTIVATE|SWP_NOZORDER);
    }
}

static void w_layout_flyout(w_flyout *menu) {
    if (!menu->list) return;
    int dpi=menu->bar->dpi;
    bool text_changed=menu->dpi != dpi;
    w_flyout_fonts(menu);
    if (text_changed || menu->gap != w_theme_spacing(dpi) || menu->button != w_theme_button_size(dpi)) {
        RECT next = w_flyout_bounds(menu);
        RECT old;
        if (GetWindowRect(menu->window,&old) && !EqualRect(&old,&next)) {
            bool resizing=old.right-old.left!=next.right-next.left || old.bottom-old.top!=next.bottom-next.top;
            if (SetWindowPos(menu->window,NULL,next.left,next.top,next.right-next.left,next.bottom-next.top,
                             SWP_NOZORDER|SWP_NOACTIVATE) && resizing) return;
        }
    }
    RECT r;
    GetClientRect(menu->window,&r);
    int gap = menu->gap, button = menu->button, y = menu->header_height+gap;
    UINT flags = SWP_NOZORDER|SWP_NOACTIVATE;
    if (menu->slider) {
        SetWindowPos(menu->mute,NULL,gap,y,button,button,flags);
        SetWindowPos(menu->slider,NULL,button+2*gap,y,max(1,r.right-button-3*gap),button,flags);
        y += button+gap;
    } else if (menu->transport[0]) {
        int count = (int)count_of(playback);
        int columns = w_transport_columns(r.right,button,gap);
        for (int i = 0; i < count; ++i) {
            int row = i/columns, items = min(columns,count-row*columns);
            int x = max(0,(r.right-(button+gap)*items+gap)/2) + (i%columns)*(button+gap);
            SetWindowPos(menu->transport[i],NULL,x,y+row*(button+gap),button,button,flags);
        }
        y += ((count+columns-1)/columns)*(button+gap);
    }
    SetWindowPos(menu->list,NULL,gap,y,max(1,r.right-2*gap),max(1,r.bottom-y-gap),flags);
    SendMessageW(menu->list,LB_SETITEMHEIGHT,0,menu->row_height);
    InvalidateRect(menu->window,NULL,FALSE);
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
    if (message==winwm_motion_updated || message==WM_SYSCOLORCHANGE ||
        (message==WM_SETTINGCHANGE && (wp==SPI_SETCLIENTAREAANIMATION || w_appearance_setting(wp,lp)))) {
        menu->motion_enabled=w_theme_motion_enabled();
        if (menu->fade.start && !w_layer_fade_to(w,&menu->fade,menu->fade.target,
            w_theme_layer_timing(!menu->closing),menu->motion_enabled) && menu->closing) {
            DestroyWindow(w);
            return 0;
        }
    }
    switch (message) {
    case WM_FONTCHANGE:
        w_theme_surface_message(w,message,wp,lp);
        menu->dpi=0;
        if (!menu->closing) w_layout_flyout(menu);
        return 0;
    case WM_SHOWWINDOW:
        if (wp) w_theme_native_controls(w);
        break;
    case WM_SIZE:
        w_layout_flyout(menu);
        return 0;
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        // The owner reanchors from its new monitor geometry during the next bar layout.
        w_bar_invalidate(bar,NULL);
        return 0;
    case winwm_theme_updated:
        w_menu_colors(menu);
        if (!menu->closing) w_layout_flyout(menu);
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
    case WM_SETTINGCHANGE:
        if (wp==SPI_SETNONCLIENTMETRICS) {
            menu->dpi=0;
            w_layout_flyout(menu);
            return 0;
        }
        if (!w_appearance_setting(wp,lp)) break;
        /* fall through */
    case WM_SYSCOLORCHANGE:
        w_menu_colors(menu);
        RedrawWindow(w,NULL,NULL,RDW_INVALIDATE|RDW_ALLCHILDREN);
        break;
    case WM_TIMER:
        if (wp == layer_fade_timer && menu->fade.start &&
            !w_layer_fade_tick(w,&menu->fade,menu->motion_enabled) && menu->closing)
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
            return w_percent_slider_draw(menu->slider,(NMCUSTOMDRAW *)lp,menu->surface,menu->border,menu->accent,menu->ink,
                                         w_native_slider_feedback(menu->slider));
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
        w_menu_colors(menu);
        PAINTSTRUCT ps;
        HDC dc = message == WM_PAINT ? BeginPaint(w,&ps) : (HDC)wp;
        RECT r;
        GetClientRect(w, &r);
        w_fill(dc,r,menu->surface);
        w_theme_frame(dc,&r,bar->dpi);
        RECT list_rect;
        if (menu->list && GetWindowRect(menu->list,&list_rect)) {
            MapWindowPoints(HWND_DESKTOP,w,(POINT *)&list_rect,2);
            RECT line = {r.left+2*menu->gap,list_rect.top-1,r.right-2*menu->gap,list_rect.top};
            w_fill(dc,line,menu->border);
        }
        r.left += 2*menu->gap;
        r.right -= 2*menu->gap;
        r.bottom = menu->header_height + menu->gap;
        HFONT heading_font=menu->heading ? menu->heading : menu->font;
        HGDIOBJ old = SelectObject(dc,heading_font ? heading_font : GetStockObject(DEFAULT_GUI_FONT));
        SetTextColor(dc, menu->ink);
        SetBkMode(dc, TRANSPARENT);
        if (menu->icon && bar->icons.size > 0) {
            RECT glyph = r;
            glyph.right = glyph.left + bar->icons.size;
            w_icons_draw(&bar->icons,dc,menu->icon,glyph,menu->accent);
            r.left = glyph.right + menu->gap;
        }
        const wchar_t *heading = menu->rows[0].label;
        if (menu->slider && bar->audio_percent >= 0 && wcschr(heading, L'%')) {
            wchar_t percentage[12];
            swprintf(percentage, count_of(percentage), L"%d%%", (int)SendMessageW(menu->slider, TBM_GETPOS, 0, 0));
            SetTextColor(dc, menu->accent);
            w_theme_draw_text(dc, percentage, -1, &r, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
            SetTextColor(dc, menu->ink);
            r.right-=menu->percent_width+menu->gap;
            heading = bar->audio_muted ? L"Output muted" : L"Output volume";
        }
        w_theme_draw_text(dc, heading, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
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
        bool selected = (item->itemState & ODS_SELECTED) && !menu->pointer_selection;
        int hover=row->disabled ? 0 : w_hover_amount(menu->list,item->itemID);
        COLORREF ink=w_hover_row(item->hDC,&r,selected,hover,!row->disabled && menu->pressed_row==(int)item->itemID);
        SetBkMode(item->hDC, TRANSPARENT);
        if (row->disabled) ink=GetSysColor(COLOR_GRAYTEXT);
        SetTextColor(item->hDC, selected || row->disabled || (hover && w_theme_high_contrast()) ? ink : menu->accent);
        r.left = item->rcItem.left + menu->gap;
        r.right = item->rcItem.right - menu->gap;
        wchar_t symbol = row->checked ? 0xe73e : row->glyph;
        if (menu->symbols) {
            HGDIOBJ old=SelectObject(item->hDC,menu->symbols);
            if (symbol) DrawTextW(item->hDC,&symbol,1,&r,DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX);
            if (row->opens_panel) DrawTextW(item->hDC,L"\xe76c",1,&r,DT_SINGLELINE|DT_VCENTER|DT_RIGHT);
            SelectObject(item->hDC,old);
        }
        SetTextColor(item->hDC,ink);
        r.left += w_theme_icon_size(bar->dpi) + menu->gap;
        if (row->opens_panel) r.right -= w_theme_icon_size(bar->dpi) + menu->gap;
        w_theme_draw_control_line(menu->list,item->hDC,row->label,&r,DT_VCENTER|DT_NOPREFIX|DT_END_ELLIPSIS);
        if ((item->itemState & (ODS_FOCUS|ODS_NOFOCUSRECT)) == ODS_FOCUS &&
            !(SendMessageW(menu->list,WM_QUERYUISTATE,0,0)&UISF_HIDEFOCUS))
            DrawFocusRect(item->hDC,&item->rcItem);
        return TRUE;
    }
    case WM_NCDESTROY:
        if (open_menu == menu)
            open_menu = NULL;
        if (fading_menu == w)
            fading_menu = NULL;
        SetWindowLongPtrW(w, GWLP_USERDATA, 0);
        DeleteObject(menu->font);
        DeleteObject(menu->heading);
        DeleteObject(menu->symbols);
        if (menu->owns_memory)
            free(menu);
        break;
    }
    return DefWindowProcW(w, message, wp, lp);
}

static void w_show(w_bar_window *bar, int hit, const w_menu_row *rows, int count, HWND language_target) {
    w_popup_anchor source;
    if (count < 2 || !w_bar_anchor(bar,hit,&source))
        return;
    HWND previous = w_bar_remember_foreground();
    if (open_menu) {
        bool same = w_popup_same_source(&open_menu->anchor,&source);
        w_dismiss(open_menu, true);
        if (same)
            return;
    }
    if (fading_menu) DestroyWindow(fading_menu);
    w_flyout *menu = calloc(1, sizeof(*menu) + (size_t)count * sizeof(*rows));
    if (!menu)
        return;
    menu->bar = bar;
    menu->motion_enabled = bar->motion_allowed;
    menu->anchor=source;
    menu->previous = previous;
    bar->invoke(bar->output,NULL,&menu->anchor);
    w_menu_colors(menu);
    menu->language_target = language_target;
    if (language_target)
        menu->language_thread = GetWindowThreadProcessId(language_target, NULL);
    menu->count = count;
    menu->pressed_row = -1;
    menu->icon = bar->hits[hit].icon;
    w_str_copy(menu->module, sizeof(menu->module), bar->hits[hit].module);
    memcpy(menu->rows, rows, (size_t)count * sizeof(*rows));

    bool audio = !strcmp(menu->module, "audio");
    bool media = !strcmp(menu->module, "media");
    RECT bounds = w_flyout_bounds(menu);
    int width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
    HINSTANCE instance = GetModuleHandleW(NULL);
    WNDCLASSW wc = {.lpfnWndProc = w_flyout_proc,
                    .hInstance = instance,
                    .hCursor = LoadCursorW(NULL, IDC_ARROW),
                    .lpszClassName = L"WinwmBarFlyout"};
    RegisterClassW(&wc);
    HWND popup =
        CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName, rows[0].label,
                        WS_POPUP | WS_CLIPCHILDREN, bounds.left, bounds.top, width, height, bar->hwnd, NULL, instance, menu);
    if (!popup) {
        free(menu);
        return;
    }
    menu->owns_memory = true;
    w_theme_material(popup);
    w_flyout_fonts(menu);
    if (audio) {
        menu->mute = w_flyout_button(menu,bar->audio_muted ? L"\xe74f" : L"\xe767",w_mute_label(bar));
        if (!menu->mute) { DestroyWindow(popup); return; }
        EnableWindow(menu->mute,bar->audio_percent >= 0);
        INITCOMMONCONTROLSEX init = {sizeof(init), ICC_BAR_CLASSES};
        InitCommonControlsEx(&init);
        menu->slider = CreateWindowExW(0, TRACKBAR_CLASSW, L"Output volume",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS,
                        0,0,0,0,popup,NULL,instance,NULL);
        if (!w_percent_slider_init(menu->slider,slider_live) || !SetWindowSubclass(menu->slider, w_flyout_control, 1, (DWORD_PTR)menu)) {
            DestroyWindow(popup);
            return;
        }
        w_percent_slider_sync(menu->slider,bar->audio_percent);
        EnableWindow(menu->slider, bar->audio_percent >= 0);
    } else if (media) {
        for (size_t i = 0; i < count_of(playback); ++i) {
            menu->transport[i] = w_flyout_button(menu,playback[i].glyph,playback[i].label);
            if (!menu->transport[i]) { DestroyWindow(popup); return; }
        }
    }
    menu->list = CreateWindowExW(0, L"LISTBOX", rows[0].label,
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_OWNERDRAWFIXED |
                                     LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
                                 0, 0, 0, 0, popup, NULL,
                                 instance, NULL);
    if (!menu->list || !SetWindowSubclass(menu->list, w_flyout_control, 1, (DWORD_PTR)menu)) {
        DestroyWindow(popup);
        return;
    }
    SendMessageW(menu->list, WM_SETFONT, (WPARAM)(menu->font ? menu->font : GetStockObject(DEFAULT_GUI_FONT)), FALSE);
    w_layout_flyout(menu);
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
    w_layer_present(popup,&menu->fade);
    ShowWindow(popup, SW_SHOW);
    SetForegroundWindow(popup);
    if (open_menu == menu && IsWindow(popup))
        SetFocus(media ? menu->transport[1] : audio && IsWindowEnabled(menu->slider) ? menu->slider : menu->list);
}

void w_bar_menu_audio_error(void) {
    if (w_bar_menu_active() && !strcmp(open_menu->module, "audio")) {
        w_percent_slider_sync(open_menu->slider,open_menu->bar->audio_percent);
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
        w_percent_slider_sync(menu->slider,b->audio_percent);
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
