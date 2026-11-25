#include "bar/bar.h"
#include "bar/accessibility.h"
#include "bar/menu.h"
#include "bar/tooltip.h"
#include <windowsx.h>
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
        wcscpy(s, L"Menu");
        strcpy(action, "menu");
    } else if (!strcmp(key, "bluetooth")) {
        wcscpy(s, L"Bluetooth devices");
        strcpy(action, "bluetooth");
    } else if (!strcmp(key,"display")) {
        swprintf(s,cap,L"%d×%d",b->screen.w,b->screen.h);
        strcpy(action,"display");
    } else if (!strcmp(key,"project")) {
        wcscpy(s,L"Projects");
        strcpy(action,"project");
    } else if (!strcmp(key,"clipboard")) {
        wcscpy(s,L"Clipboard history");
        strcpy(action,"clipboard");
    } else if (!strcmp(key,"notifications")) {
        wcscpy(s,L"Desktop notifications");
        strcpy(action,"notifications");
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
        strcpy(action, b->view.resize_mode ? "resize_mode" : "idle_inhibit");
    } else if (!strcmp(key, "clock")) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        if (b->calendar_date)
            GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &t, L"MMM d", s, cap, NULL);
        else
            swprintf(s, cap, L"%02u:%02u", t.wHour, t.wMinute);
        strcpy(action, "date_time");
    } else if (!strcmp(key, "battery")) {
        SYSTEM_POWER_STATUS p;
        if (GetSystemPowerStatus(&p) && p.BatteryFlag != 128 && p.BatteryLifePercent <= 100)
            swprintf(s, cap, L"%u%%  %ls", p.BatteryLifePercent, p.ACLineStatus == 1 ? L"Plugged in" : L"On battery");
        else
            wcscpy(s, L"Power");
        strcpy(action, "power_settings");
    } else if (!strcmp(key, "keyboard")) {
        HKL lang = GetKeyboardLayout(GetWindowThreadProcessId(w_bar_input_target(), NULL));
        if (GetLocaleInfoW(LOWORD((uintptr_t)lang), LOCALE_SISO639LANGNAME, s, cap))
            CharUpperBuffW(s, (DWORD)wcslen(s));
        else
            wcscpy(s, L"KB");
        strcpy(action, "languages");
    } else if (b->custom)
        b->custom(key, s, cap, action, 128);
    return (int)wcslen(s);
}

typedef struct w_bar_cell {
    wchar_t text[256], tooltip[256];
    const w_module_style *style;
    char action[128], key[56], instance[56];
    int width, workspace, offset;
    bar_icon icon;
    bool active, empty, label;
} w_bar_cell;

typedef struct w_bar_group {
    w_bar_cell *cells;
    int capacity, count, length;
} w_bar_group;

// Keep source and destination separate so repeated fields can be expanded safely.
static void w_template(const char *format, const wchar_t *value, wchar_t *out, size_t cap) {
    wchar_t pattern[192];
    if (!w_os_wide(format,pattern,count_of(pattern))) return;
    size_t used=0;
    for (const wchar_t *p=pattern; *p && used+1<cap;) {
        if (!wcsncmp(p,L"{text}",6)) {
            size_t n=min(wcslen(value),cap-1-used);
            wmemcpy(out+used,value,n);
            used+=n;
            p+=6;
        } else out[used++]=*p++;
    }
    if (used && IS_HIGH_SURROGATE(out[used-1])) --used;
    out[used]=0;
}

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
            cell->offset = (int)(p - spec - n);
            if (spaces) {
                cell->workspace = j+1;
                if (j >= b->config->bar_workspace_count && j != b->view.selected && !(b->view.occupied & (1 << j)))
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
            if (!strcmp(base, "clock")) {
                GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_LONGDATE, NULL, NULL, cell->tooltip,
                                count_of(cell->tooltip), NULL);
            }
            if (style) {
                if (*style->tooltip)
                    w_template(style->tooltip,cell->text,cell->tooltip,count_of(cell->tooltip));
                if (*style->action)
                    strcpy(cell->action, style->action);
                if (*style->format) {
                    wchar_t raw[count_of(cell->text)];
                    wcscpy(raw,cell->text);
                    w_template(style->format,raw,cell->text,count_of(cell->text));
                }
            }
            enum w_module_display display = style ? style->display : module_auto;
            bool formatted = style && *style->format;
            bool clock_lines = vertical && !strcmp(base, "clock") && !formatted && display != module_icon;
            if (clock_lines) {
                if (b->calendar_date) {
                    SYSTEMTIME t;
                    GetLocalTime(&t);
                    swprintf(cell->text, count_of(cell->text), L"%02u\n%02u", t.wMonth, t.wDay);
                } else
                    cell->text[2] = L'\n';
            }
            if (display != module_text && (display != module_auto || !formatted)) {
                static const struct w_module_icon {
                    const char *key;
                    bar_icon icon;
                } icons[] = {{"menu", icon_menu},    {"bluetooth", icon_bluetooth}, {"network", icon_network},
                             {"audio", icon_audio}, {"battery", icon_battery},     {"memory", icon_memory},
                             {"media", icon_play},  {"cpu", icon_cpu}, {"keyboard",icon_keyboard},
                             {"clock",icon_clock}, {"throughput",icon_traffic}, {"display",icon_display},
                             {"project",icon_project}, {"clipboard",icon_clipboard}, {"notifications",icon_bell},
                             {"mode",icon_resize}};
                for (size_t k = 0; b->icons.coverage && k < count_of(icons); ++k)
                    if (!strcmp(base, icons[k].key)) {
                        if (display == module_auto && (!strcmp(base,"clock") || !strcmp(base,"keyboard") ||
                                                      !strcmp(base,"throughput"))) break;
                        cell->icon = icons[k].icon;
                        if (cell->icon == icon_resize && !b->view.resize_mode)
                            cell->icon = icon_awake;
                        if (cell->icon == icon_audio && (b->audio_muted || b->audio_percent < 0))
                            cell->icon = icon_muted;
                        else if (cell->icon == icon_audio && b->audio_percent < 50)
                            cell->icon = icon_quiet;
                        if (cell->icon == icon_battery && !wcscmp(cell->text, L"Power"))
                            cell->icon = icon_plug;
                        if (cell->icon == icon_network)
                            cell->icon = b->network_icon ? b->network_icon : icon_network;
                        break;
                    }
            }
            cell->label = !cell->icon || display == module_icon_text;
            if (cell->icon && display == module_auto && !vertical)
                cell->label = !strcmp(base,"cpu") || !strcmp(base,"memory") || !strcmp(base,"display") ||
                              (!strcmp(base,"audio") && b->audio_percent >= 0) ||
                              (!strcmp(base,"battery") && cell->icon != icon_plug);
            if (cell->label && cell->icon && !formatted && strcmp(base,"cpu")) {
                // Keep the full measurement in the hover card.
                wchar_t *percent = wcschr(cell->text,L'%');
                if (percent) {
                    wchar_t *start = percent;
                    while (start>cell->text && start[-1] >= L'0' && start[-1] <= L'9') --start;
                    if (start != percent) {
                        percent[1]=0;
                        memmove(cell->text,start,(wcslen(start)+1)*sizeof(*start));
                    }
                }
            }
            SelectObject(dc, b->font);
            strcpy(cell->key, base);
            strcpy(cell->instance,key);
            SIZE size;
            GetTextExtentPoint32W(dc, cell->text, (int)wcslen(cell->text), &size);
            if (cell->icon && cell->label) {
                if (vertical) size.cy+=b->icons.size+MulDiv(3,b->dpi,96);
                else size.cx+=b->icons.size+MulDiv(5,b->dpi,96);
            } else if (cell->icon) size=(SIZE){b->icons.size,b->icons.size};
            cell->width = vertical ? size.cy * (clock_lines ? 2 : 1) + MulDiv(10,b->dpi,96) : size.cx + MulDiv(16, b->dpi, 96);
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

static COLORREF w_blend(COLORREF a, COLORREF b, double t) {
    return RGB(GetRValue(a)+(int)((GetRValue(b)-GetRValue(a))*t),
               GetGValue(a)+(int)((GetGValue(b)-GetGValue(a))*t),
               GetBValue(a)+(int)((GetBValue(b)-GetBValue(a))*t));
}

static void w_cell_colors(w_bar_window *b, struct w_bar_hit *hit, COLORREF paper, COLORREF ink, uint64_t now) {
    w_transition timing = b->config->motion[motion_modules];
    bool animate = b->motion_allowed && !b->high_contrast && timing.ms && hit->painted;
    hit->painted = true;
    if (animate && (paper != hit->target_paper || ink != hit->target_ink)) {
        hit->from_paper = hit->paper;
        hit->from_ink = hit->ink;
        if (!b->modules_ticking)
            b->modules_ticking = SetTimer(b->hwnd,bar_modules_timer,16,NULL) != 0;
        hit->start = b->modules_ticking ? now : 0;
    }
    hit->target_paper = paper;
    hit->target_ink = ink;
    if (animate && hit->start && now-hit->start < (unsigned)timing.ms) {
        double t = w_motion_progress(timing, now-hit->start);
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        hit->paper = w_blend(hit->from_paper,paper,t);
        hit->ink = w_blend(hit->from_ink,ink,t);
    } else {
        hit->start = 0;
        hit->paper = paper;
        hit->ink = ink;
    }
}

static void w_place_group(w_bar_window *b, RECT bounds, const w_bar_group *group, bool vertical, int pos, int end,
                          int source_base) {
    for (int i=0;i<group->count && pos<end;++i) {
        const w_bar_cell *cell=&group->cells[i];
        struct w_bar_hit *hit=&b->hits[b->hit_count++];
        if (strcmp(hit->instance,cell->instance) || hit->workspace!=cell->workspace)
            hit->painted=false;
        hit->box=bounds;
        if (vertical) {
            hit->box.top=pos;
            hit->box.bottom=min(pos+cell->width,end);
        } else {
            hit->box.left=pos;
            hit->box.right=min(pos+cell->width,end);
        }
        hit->workspace=cell->workspace;
        hit->source=source_base+cell->offset;
        hit->style=cell->style;
        hit->active=cell->active;
        hit->empty=cell->empty;
        hit->label=cell->label;
        hit->icon=cell->icon;
        strcpy(hit->module,cell->key);
        strcpy(hit->instance,cell->instance);
        strcpy(hit->action,cell->action);
        wcscpy(hit->tooltip,cell->tooltip);
        wcscpy(hit->text,cell->text);
        pos+=cell->width;
    }
}

static void w_paint_hit(w_bar_window *b,HDC dc,struct w_bar_hit *hit,COLORREF background,bool vertical,uint64_t now) {
    RECT box=hit->box;
    bool hovered=(b->pointer_inside && PtInRect(&box,b->pointer)) || w_bar_menu_at(b,hit);
    COLORREF paper=hit->active ? w_rgb_value(b->config->accent) :
                   hovered ? w_mix(b->config->background,b->config->foreground,12) : background;
    uint32_t color=hit->active ? b->config->background :
                   hit->style && hit->style->colored ? hit->style->color : b->config->foreground;
    COLORREF ink=hit->empty ? w_mix(b->config->background,color,55) : w_rgb_value(color);
    if (b->high_contrast) {
        paper=hit->active || hovered ? GetSysColor(COLOR_HIGHLIGHT) : background;
        ink=GetSysColor(hit->active || hovered ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT);
    }
    w_cell_colors(b,hit,paper,ink,now);
    if (hit->paper != background) {
        RECT pill=box;
        InflateRect(&pill,-MulDiv(2,b->dpi,96),-MulDiv(3,b->dpi,96));
        SetDCBrushColor(dc,hit->paper);
        HGDIOBJ pen=SelectObject(dc,GetStockObject(NULL_PEN)),brush=SelectObject(dc,GetStockObject(DC_BRUSH));
        int radius=MulDiv(8,b->dpi,96);
        RoundRect(dc,pill.left,pill.top,pill.right,pill.bottom,radius,radius);
        SelectObject(dc,brush);
        SelectObject(dc,pen);
    }
    SetTextColor(dc,hit->ink);
    RECT label=box,icon=box;
    if (hit->icon && hit->label) {
        if (vertical) {
            icon.top+=MulDiv(5,b->dpi,96);
            icon.bottom=icon.top+b->icons.size;
            label.top=icon.bottom;
        } else {
            icon.left+=MulDiv(8,b->dpi,96);
            icon.right=icon.left+b->icons.size;
            label.left=icon.right+MulDiv(5,b->dpi,96);
            label.right-=MulDiv(6,b->dpi,96);
        }
    }
    if (hit->icon && icon.right-icon.left>=b->icons.size && icon.bottom-icon.top>=b->icons.size &&
        icon.right<=box.right && icon.bottom<=box.bottom)
        w_icons_draw(&b->icons,dc,hit->icon,icon,hit->ink,hit->paper);
    if (hit->label && label.right>label.left && label.bottom>label.top) {
        UINT flags=DT_NOPREFIX|DT_CENTER;
        if (!wcschr(hit->text,L'\n')) flags|=DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS;
        DrawTextW(dc,hit->text,-1,&label,flags);
    }
}

static int w_hit_at(const w_bar_window *b, POINT p) {
    for (int i = 0; i < b->hit_count; ++i)
        if (PtInRect(&b->hits[i].box, p))
            return i;
    return -1;
}

static void w_hover(w_bar_window *b, int hit) {
    w_bar_tip_track(b,hit);
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

static void w_bar_layout(w_bar_window *b,HDC dc,RECT r,bool vertical);

static LRESULT w_paint_bar(w_bar_window *b, HWND w) {
    PAINTSTRUCT ps;
    HDC target = BeginPaint(w, &ps);
    RECT r;
    GetClientRect(w, &r);
    HDC dc = w_paint_buffer(b, target, r);
    COLORREF background=b->high_contrast ? GetSysColor(COLOR_WINDOW) : w_rgb_value(b->config->background);
    SetDCBrushColor(dc, background);
    FillRect(dc, &ps.rcPaint, GetStockObject(DC_BRUSH));
    HGDIOBJ old = SelectObject(dc, b->font ? b->font : GetStockObject(DEFAULT_GUI_FONT));
    SetBkMode(dc, TRANSPARENT);
    bool vertical = r.bottom > r.right;
    if (EqualRect(&r,&ps.rcPaint) || !b->hit_count) {
        w_bar_layout(b,dc,r,vertical);
        if (b->access) w_bar_access_layout(b);
    }
    if (b->pointer_inside && !b->pressed) {
        int hit=w_hit_at(b,b->pointer);
        w_hover(b,hit);
        if (b->tip.showing && EqualRect(&r,&ps.rcPaint)) w_bar_tip_update(b);
    }
    uint64_t now=GetTickCount64();
    for (int i=0; i<b->hit_count; ++i) {
        RECT dirty;
        if (IntersectRect(&dirty,&ps.rcPaint,&b->hits[i].box))
            w_paint_hit(b,dc,&b->hits[i],background,vertical,now);
    }
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

static void w_bar_layout(w_bar_window *b,HDC dc,RECT r,bool vertical) {
    int length = vertical ? r.bottom : r.right;
    int margin = MulDiv(6, b->dpi ? b->dpi : 96, 96);
    const char *groups[]={b->config->bar_left,b->config->bar_center,b->config->bar_right};
    int count=0;
    for (size_t i=0;i<count_of(groups);++i) {
        const char *p=groups[i];
        while (*(p+=strspn(p," "))) {
            size_t n=strcspn(p," ");
            bool spaces=n>=10 && !strncmp(p,"workspaces",10) && (n==10 || p[10]=='#');
            count+=spaces ? workspace_limit : 1;
            p+=n;
        }
    }
    b->hit_count=0;
    if (!count) return;
    // A named workspace instance may expand as other workspaces become occupied.
    // The three configuration strings bound the total. Retain storage through resizes.
    if (count>b->cell_capacity) {
        w_bar_cell *cells=malloc((size_t)count*sizeof(*cells));
        struct w_bar_hit *hits=calloc((size_t)count,sizeof(*hits));
        if (!cells || !hits) { free(cells); free(hits); return; }
        free(b->cells);
        free(b->hits);
        b->cells=cells;
        b->hits=hits;
        b->cell_capacity=count;
    }
    w_bar_group left = {.cells = b->cells, .capacity = count};
    w_measure(b, dc, b->config->bar_left, vertical, &left);
    w_bar_group center = {.cells = b->cells + left.count, .capacity = left.capacity - left.count};
    w_measure(b, dc, b->config->bar_center, vertical, &center);
    w_bar_group right = {.cells = center.cells + center.count, .capacity = center.capacity - center.count};
    w_measure(b, dc, b->config->bar_right, vertical, &right);
    if (!vertical && left.length+center.length+right.length>length-2*margin) {
        w_bar_group *groups[]={&right,&left,&center};
        for (size_t k=0;k<count_of(groups);k++)
            for (int i=0;i<groups[k]->count && left.length+center.length+right.length>length-2*margin;i++) {
                w_bar_cell *cell=&groups[k]->cells[i];
                if (!cell->icon || !cell->label || (cell->style && cell->style->display!=module_auto)) continue;
                cell->label=false;
                int width=b->icons.size+MulDiv(16,b->dpi,96);
                groups[k]->length+=width-cell->width;
                cell->width=width;
            }
    }
    // Keep status controls on the trailing edge. Shift the clock before clipping a group.
    int right_start=max(margin,length-margin-right.length);
    int middle=max(margin,min(max(margin+left.length,(length-center.length)/2),right_start-center.length));
    int center_end=min(right_start,middle+center.length);
    w_place_group(b,r,&left,vertical,margin,center.length ? middle : right_start,0);
    w_place_group(b,r,&center,vertical,middle,center_end,sizeof(b->config->bar_left));
    w_place_group(b,r,&right,vertical,right_start,length-margin,sizeof(b->config->bar_left)+sizeof(b->config->bar_center));
}

bool w_bar_anchor(const w_bar_window *b, int hit, w_popup_anchor *anchor) {
    if (hit < 0 || hit >= b->hit_count)
        return false;
    anchor->bar = b->hwnd;
    anchor->source = b->hits[hit].source;
    anchor->button = b->hits[hit].box;
    MapWindowPoints(b->hwnd, HWND_DESKTOP, (POINT *)&anchor->button, 2);
    w_str_copy(anchor->edge, sizeof(anchor->edge), b->docked_edge ? b->docked_edge : b->config->edge);
    return true;
}

void w_bar_invoke(w_bar_window *b, int hit, const char *action) {
    w_popup_anchor anchor;
    w_bar_remember_foreground();
    b->invoke(b->output, action, w_bar_anchor(b,hit,&anchor) ? &anchor : NULL);
}

bool w_bar_activate_hit(w_bar_window *b, int hit) {
    if (hit < 0 || hit >= b->hit_count) return false;
    const struct w_bar_hit *item = &b->hits[hit];
    w_bar_tip_hide(b);
    if (!(item->style && *item->style->action) && strcmp(item->module,"media") && w_bar_menu_open(b,hit))
        return true;
    char action[sizeof(item->action)];
    strcpy(action,item->action);
    w_bar_menu_close(NULL,true);
    w_bar_invoke(b,hit,*action ? action : NULL);
    return *action != 0;
}

static LRESULT CALLBACK w_bar_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    w_bar_window *b = (w_bar_window *)GetWindowLongPtrW(w, GWLP_USERDATA);
    if (msg == WM_NCCREATE) {
        SetWindowLongPtrW(w, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW *)lp)->lpCreateParams);
        return DefWindowProcW(w, msg, wp, lp);
    }
    if (!b)
        return DefWindowProcW(w, msg, wp, lp);
    if (msg == WM_GETOBJECT && (LONG)lp == OBJID_CLIENT && b->hwnd == w)
        return w_bar_access_object(b,wp);
    if (msg == bar_access_activate) {
        UpdateWindow(w);
        w_bar_access_activate(b,(LONG)wp);
        return 0;
    }
    if (msg == bar_access_event) {
        w_bar_access_notify(b,(DWORD)wp,(LONG)lp);
        return 0;
    }
    if (msg == WM_PAINT)
        return w_paint_bar(b, w);
    // A status or configuration change can move controls before the next WM_PAINT.
    if (msg==WM_LBUTTONDOWN || msg==WM_LBUTTONUP || msg==WM_RBUTTONUP || msg==WM_MBUTTONUP ||
        msg==WM_MOUSEWHEEL) UpdateWindow(w);
    if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN ||
        msg == WM_MOUSEWHEEL || msg == WM_CANCELMODE) w_bar_tip_hide(b);
    if (msg == WM_TIMER && (wp == bar_tip_show_timer || wp == bar_tip_hide_timer)) {
        w_bar_tip_timer(b,wp);
        return 0;
    }
    if (msg == WM_MOVE || msg == WM_SIZE || msg == WM_THEMECHANGED || msg == WM_SETTINGCHANGE ||
        msg == WM_SYSCOLORCHANGE) {
        if (msg == WM_MOVE || msg == WM_SIZE)
            w_bar_menu_close(b, false);
        if (msg == WM_SIZE) InvalidateRect(w,NULL,FALSE);
        if (msg != WM_MOVE && msg != WM_SIZE) {
            HIGHCONTRASTW hc={.cbSize=sizeof(hc)};
            b->high_contrast=SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(hc),&hc,0) && (hc.dwFlags&HCF_HIGHCONTRASTON);
            InvalidateRect(w,NULL,FALSE);
        }
        b->pointer_inside=false;
        w_hover(b,-1);
    }
    if (msg == WM_TIMER && wp == bar_modules_timer) {
        bool active = false;
        for (int i = 0; i < b->hit_count; i++)
            if (b->hits[i].start) {
                active = true;
                InvalidateRect(w,&b->hits[i].box,FALSE);
            }
        if (!active) {
            KillTimer(w,bar_modules_timer);
            b->modules_ticking = false;
        }
        return 0;
    }
    if (msg == WM_TIMER && wp == bar_fade_timer) {
        if (b->fade.start && !w_bar_fade_tick(w,&b->fade,b->motion_allowed) && !b->fade.target)
            ShowWindow(w,SW_HIDE);
        return 0;
    }
    if (msg == WM_SHOWWINDOW && !wp) {
        w_bar_tip_track(b,-1);
        KillTimer(w,bar_modules_timer);
        KillTimer(w,bar_fade_timer);
        b->modules_ticking = b->pointer_inside = false;
        b->fade = (w_layer_fade){0};
        b->hover = -1;
        b->hit_count = 0;
        if (b->access) w_bar_access_layout(b);
        if (b->hits) memset(b->hits,0,(size_t)b->cell_capacity*sizeof(*b->hits));
        w_bar_menu_close(b, true);
    }
    if (msg == WM_MOUSEMOVE && !b->pressed) {
        POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        b->pointer = p;
        if (!b->pointer_inside) {
            TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, w, 0};
            b->pointer_inside = TrackMouseEvent(&track)!=0;
        }
        w_hover(b, w_hit_at(b, p));
        w_bar_tip_move(b);
        return 0;
    }
    if (msg == WM_MOUSELEAVE) {
        b->pointer_inside = false;
        w_hover(b, -1);
        return 0;
    }
    if (msg == WM_DESTROY) {
        SetWindowLongPtrW(w,GWLP_USERDATA,0);
        w_bar_access_close(b);
        w_bar_tip_track(b,-1);
        if (IsWindow(b->tip.window)) DestroyWindow(b->tip.window);
        b->tip.window=NULL;
        w_free_buffer(b);
        DeleteObject(b->font);
        DeleteObject(b->heading_font);
        w_icons_free(&b->icons);
        free(b->hits);
        free(b->cells);
        b->hits=NULL;
        b->cells=NULL;
        b->cell_capacity=b->hit_count=0;
        return 0;
    }
    if (msg == WM_LBUTTONDOWN) {
        b->press = (POINT){GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int hit=w_hit_at(b,b->press);
        if (hit < 0) {
            w_bar_menu_close(NULL, true);
            w_bar_invoke(b,-1,NULL);
        }
        strcpy(b->press_instance,hit>=0 ? b->hits[hit].instance : "");
        b->press_workspace=hit>=0 ? b->hits[hit].workspace : 0;
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
            w_bar_menu_close(NULL, true);
            w_bar_invoke(b,-1,NULL);
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
        POINT start = b->press;
        ScreenToClient(w, &start);
        if (hit != w_hit_at(b, start) || hit<0 || strcmp(b->press_instance,b->hits[hit].instance) ||
            b->press_workspace!=b->hits[hit].workspace)
            return 0;
        w_bar_activate_hit(b,hit);
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
        if (hit < 0) {
            w_bar_menu_close(NULL, true);
            w_bar_invoke(b,-1,NULL);
            return 0;
        }
        const char *key = b->hits[hit].module, *action = NULL;
        const w_module_style *style=b->hits[hit].style;
        if (style && *(msg==WM_MBUTTONUP ? style->middle_click : style->right_click))
            action=msg==WM_MBUTTONUP ? style->middle_click : style->right_click;
        else if (!strcmp(key, "audio"))
            action = "mute";
        else if (msg == WM_MBUTTONUP && !strcmp(key,"menu")) action="bar_settings";
        else if (msg == WM_RBUTTONUP) {
            if (!strcmp(key, "media")) {
                w_bar_menu_open(b, hit);
                return 0;
            }
            if (!strcmp(key, "keyboard")) action = "languages";
            else if (!strcmp(key, "menu")) action = "settings";
            else if (!strcmp(key, "clock")) {
                b->calendar_date = !b->calendar_date;
                InvalidateRect(w, NULL, FALSE);
            }
            else if (!strcmp(key, "battery")) action = "power_settings";
            else if (!strcmp(key, "network") || !strcmp(key, "throughput")) action = "network_settings";
        }
        w_bar_menu_close(NULL, true);
        w_bar_invoke(b,hit,action);
        return 0;
    }
    if (msg == WM_MOUSEWHEEL) {
        POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(w, &p);
        int hit = w_hit_at(b, p);
        if (hit < 0)
            return 0;
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        if (b->wheel_hit != hit) {
            b->wheel_remainder = 0;
            b->wheel_hit = hit;
        }
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
                w_bar_invoke(b,hit,action);
        } else if (!strcmp(b->hits[hit].module, "workspaces")) {
            char select[40];
            int ws = (b->view.selected - steps) % workspace_limit;
            snprintf(select, sizeof(select), "workspace_select %d",
                     (ws + workspace_limit) % workspace_limit + 1);
            w_bar_invoke(b,hit,select);
        } else if (!strcmp(b->hits[hit].module, "keyboard")) {
            w_bar_cycle_layout(-steps);
        }
        return 0;
    }
    if (msg == WM_SETCURSOR && LOWORD(lp)==HTCLIENT) {
        POINT cursor;
        if (GetCursorPos(&cursor) && ScreenToClient(w,&cursor)) {
            int hit=w_hit_at(b,cursor);
            if (hit>=0 && (*b->hits[hit].action || !strcmp(b->hits[hit].module,"audio") ||
                (b->hits[hit].style && (*b->hits[hit].style->right_click || *b->hits[hit].style->middle_click)))) {
                SetCursor(LoadCursorW(NULL,IDC_HAND));
                return TRUE;
            }
        }
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
    b->tip.index = b->hover = b->audio_percent = -1;
    HIGHCONTRASTW hc={.cbSize=sizeof(hc)};
    b->high_contrast=SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(hc),&hc,0) && (hc.dwFlags&HCF_HIGHCONTRASTON);
    b->config = cfg;
    b->output = output;
    b->invoke = invoke;
    b->hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, wc.lpszClassName,
                              L"winwm bar", WS_POPUP, 0, 0, 1, 1, NULL, NULL, wc.hInstance, b);
    return b->hwnd != NULL;
}

w_rect w_bar_position(w_bar_window *b, w_rect screen, w_rect available, int dpi, bool hidden) {
    w_rect work = available, area = available;
    if (!b->hwnd)
        return work;
    b->screen = screen;
    if (dpi != b->dpi) {
        w_icons_resize(&b->icons, dpi);
        if (b->font)
            DeleteObject(b->font);
        DeleteObject(b->heading_font);
        b->dpi = dpi;
        b->font = CreateFontW(-MulDiv(10, dpi, 72), 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                              L"Segoe UI");
        b->heading_font=CreateFontW(-MulDiv(11,dpi,72),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
                                    OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        InvalidateRect(b->hwnd,NULL,FALSE);
    }
    int n = MulDiv(b->config->bar_height, dpi, 96);
    if (hidden) {
        if (IsWindowVisible(b->hwnd) && b->fade.target) {
            w_bar_tip_track(b,-1);
            w_bar_menu_close(b,true);
            if (GetCapture() == b->hwnd)
                ReleaseCapture();
            SetWindowLongPtrW(b->hwnd,GWL_EXSTYLE,GetWindowLongPtrW(b->hwnd,GWL_EXSTYLE)|WS_EX_TRANSPARENT);
            if (!w_bar_fade(b->hwnd,&b->fade,0,b->config->motion[motion_layers_out],b->motion_allowed))
                ShowWindow(b->hwnd,SW_HIDE);
        }
        return work;
    }
    const char *edge = b->docked_edge ? b->docked_edge : b->config->edge;
    if (!strcmp(edge, "left") || !strcmp(edge, "right")) {
        n = min(n, max(0,work.w-1));
        area.w = n;
        work.w -= n;
        if (!strcmp(edge, "left"))
            work.x += n;
        else
            area.x += work.w;
    } else {
        n = min(n, max(0,work.h-1));
        area.h = n;
        work.h -= n;
        if (!strcmp(edge, "top"))
            work.y += n;
        else
            area.y += work.h;
    }
    if (!IsWindowVisible(b->hwnd) || !b->fade.target) {
        SetWindowLongPtrW(b->hwnd,GWL_EXSTYLE,GetWindowLongPtrW(b->hwnd,GWL_EXSTYLE)&~(LONG_PTR)WS_EX_TRANSPARENT);
        w_bar_fade(b->hwnd,&b->fade,255,b->config->motion[motion_layers_in],b->motion_allowed);
    }
    SetWindowPos(b->hwnd, HWND_TOPMOST, area.x, area.y, area.w, area.h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return work;
}

void w_bar_invalidate(w_bar_window *b, const char *module) {
    // A content change can resize the cell and move its neighbours, even
    // when the module was clipped out of the previous layout.
    if (b->hwnd && w_config_has_module(b->config,module))
        InvalidateRect(b->hwnd, NULL, FALSE);
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
