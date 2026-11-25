#include "bar/tooltip.h"
#include "bar/menu.h"
#include "theme/theme.h"
#include "win_platform/appearance.h"
#include <stdio.h>

static const w_bar_description descriptions[] = {
    {"menu", L"Menu", L"Open menu", L"Desktop settings", NULL},
    {"workspaces", L"Workspace", L"Switch workspace", NULL, L"Cycle workspaces"},
    {"title", L"Focused window", L"Switch window", NULL, NULL},
    {"mode", L"Desktop mode", NULL, NULL, NULL},
    {"clock", L"Clock", L"Calendar and time zone", L"Toggle date", NULL},
    {"keyboard", L"Keyboard", L"Choose layout", L"Input settings", L"Cycle layouts"},
    {"audio", L"Sound", L"Volume and devices", NULL, L"Adjust volume"},
    {"bluetooth", L"Bluetooth", L"Manage devices", NULL, NULL},
    {"network", L"Network", L"Wi-Fi networks", L"Network adapters", NULL},
    {"throughput", L"Network traffic", L"Wi-Fi networks", L"Network adapters", NULL},
    {"battery", L"Power", L"Power menu", L"Power settings", NULL},
    {"cpu", L"CPU", L"Activity monitor", NULL, NULL},
    {"memory", L"Memory", L"Activity monitor", NULL, NULL},
    {"media", L"Media", L"Play or pause", L"Playback controls", NULL},
    {"display", L"Display", L"Display settings", NULL, NULL},
    {"project", L"Projects", L"Open project folder", NULL, NULL},
    {"clipboard", L"Clipboard history", L"Browse clipboard history", NULL, NULL},
    {"notifications", L"Desktop notifications", L"View notifications", NULL, NULL},
    {"tray", L"System tray", L"Toggle drawer", NULL, L"Browse applications"},
};

const w_bar_description *w_bar_describe(const char *module) {
    for (size_t i=0; i<count_of(descriptions); ++i)
        if (!strcmp(module,descriptions[i].module)) return &descriptions[i];
    return NULL;
}

static void w_hint(wchar_t *out, size_t cap, const wchar_t *gesture, const wchar_t *label, const char *command) {
    wchar_t name[128];
    if (command && *command && w_os_wide(command,name,count_of(name))) label = name;
    if (!label || !*label) return;
    size_t used = wcslen(out);
    const wchar_t *prefix = used ? L"\n" : L"";
    if (used+wcslen(gesture)+wcslen(label)+4 < cap)
        swprintf(out+used,cap-used,L"%ls%ls  ·  %ls",prefix,gesture,label);
}

static void w_tip_text(w_bar_window *b, wchar_t *heading, wchar_t *body, wchar_t *help) {
    const struct w_bar_hit *hit = &b->hits[b->tip.index];
    const w_module_style *s = hit->style;
    const wchar_t *left = NULL, *right = NULL, *wheel = NULL;
    const w_bar_description *info = w_bar_describe(hit->module);
    if (info) {
        wcscpy(heading,info->heading);
        left = info->click;
        right = info->right;
        wheel = info->wheel;
    }
    if (!strcmp(hit->module,"tray")) {
        if (hit->tray_key) {
            size_t first=wcscspn(hit->tooltip,L"\r\n");
            wmemcpy(heading,hit->tooltip,first);
            heading[first]=0;
            if (!first) wcscpy(heading,L"Application");
            const wchar_t *detail=hit->tooltip+first;
            detail+=wcsspn(detail,L"\r\n");
            wcscpy(body,detail);
            w_hint(help,320,L"Click",L"Activate application",NULL);
            w_hint(help,320,L"Right-click",L"Application menu",NULL);
        } else if (hit->tray_more) {
            wcscpy(heading,L"More applications");
            wcscpy(body,hit->tooltip);
            w_hint(help,320,L"Click",L"Next page",NULL);
        } else {
            wcscpy(body,hit->tooltip);
            if (b->tray.total) w_hint(help,320,L"Hover",L"Show applications",NULL);
        }
        if (b->tray.total>b->tray.page && b->tray.open)
            w_hint(help,320,L"Scroll",L"Browse applications",NULL);
        return;
    }
    if (!*heading) {
        const char *name = !strncmp(hit->module,"custom/",7) ? hit->module+7 : hit->module;
        w_os_wide(name,heading,96);
    }
    if (hit->workspace)
        swprintf(heading,96,L"Workspace %d",hit->workspace);
    if (!s || !*s->tooltip) {
        if (b->detail && strcmp(hit->module,"clipboard"))
            b->detail(hit->module,body,512);
        if (!*body && !strcmp(hit->module,"display")) {
            swprintf(body,512,L"%d × %d pixels  ·  %d%% scaling",b->screen.w,b->screen.h,MulDiv(b->dpi,100,96));
        }
        if (!*body && !strcmp(hit->module,"keyboard")) {
            HKL layout=GetKeyboardLayout(GetWindowThreadProcessId(w_bar_input_target(),NULL));
            if (layout && b->tip.keyboard_layout!=layout) {
                b->tip.keyboard_layout=layout;
                w_bar_layout_label(layout,b->tip.keyboard_name,count_of(b->tip.keyboard_name));
            }
            if (layout) wcscpy(body,b->tip.keyboard_name);
        }
        if (!*body && !strcmp(hit->module,"battery")) {
            SYSTEM_POWER_STATUS p;
            if (GetSystemPowerStatus(&p)) {
                const wchar_t *source=p.ACLineStatus==1 ? L"Plugged in" :
                                      p.ACLineStatus==0 ? L"On battery" : L"Power source unknown";
                if (p.BatteryFlag!=128 && p.BatteryLifePercent<=100) {
                    swprintf(body,512,L"%u%%  ·  %ls%ls",p.BatteryLifePercent,source,
                             p.BatteryFlag!=255 && (p.BatteryFlag&8) ? L"  ·  Charging" : L"");
                    if (p.ACLineStatus==0 && p.BatteryLifeTime!=MAXDWORD) {
                        size_t n=wcslen(body);
                        swprintf(body+n,512-n,L"\n%lu h %lu min remaining",p.BatteryLifeTime/3600,p.BatteryLifeTime/60%60);
                    }
                } else wcscpy(body,source);
            }
        }
        if (hit->workspace)
            swprintf(body,512,L"%ls  ·  %ls",hit->text,
                     hit->active ? L"Active" : hit->empty ? L"Empty" : L"Occupied");
    }
    if (!*body) wcscpy(body,hit->tooltip);
    if (!_wcsicmp(heading,body)) *body = 0;
    if (!strcmp(hit->module,"mode"))
        left = b->view.resize_mode ? L"Exit resize mode" : L"Allow automatic sleep";
    const wchar_t *toggle_mute = NULL;
    if (!strcmp(hit->module,"audio")) {
        if (b->audio_percent >= 0) toggle_mute = b->audio_muted ? L"Unmute output" : L"Mute output";
        else wheel = NULL;
        right = toggle_mute;
    }
    w_hint(help,320,L"Click",left,s && *s->action ? s->action : left ? NULL : hit->action);
    w_hint(help,320,L"Right-click",right,s ? s->right_click : NULL);
    w_hint(help,320,L"Middle-click",!strcmp(hit->module,"menu") ? L"Configure toolbar" :
           toggle_mute,
           s ? s->middle_click : NULL);
    if (s && (*s->scroll_up || *s->scroll_down)) {
        w_hint(help,320,L"Scroll up",wheel,s->scroll_up);
        w_hint(help,320,L"Scroll down",wheel,s->scroll_down);
    } else w_hint(help,320,L"Scroll",wheel,NULL);
}

static HFONT w_tip_font(const w_bar_window *b,int part) {
    if (!part && b->heading_font) return b->heading_font;
    return b->font ? b->font : GetStockObject(DEFAULT_GUI_FONT);
}

static void w_tip_paint(w_bar_window *b, HDC dc, RECT r) {
    COLORREF fg = w_theme_foreground();
    COLORREF bg = w_theme_background();
    COLORREF accent = w_theme_accent();
    SetDCBrushColor(dc,bg);
    FillRect(dc,&r,GetStockObject(DC_BRUSH));
    w_theme_frame(dc,&r,b->dpi);
    SetBkMode(dc,TRANSPARENT);
    HFONT previous = GetCurrentObject(dc,OBJ_FONT);
    const wchar_t *parts[] = {b->tip.heading,b->tip.body,b->tip.help};
    for (int i=0;i<3;i++) {
        if (!*parts[i]) continue;
        SelectObject(dc,w_tip_font(b,i));
        SetTextColor(dc,i == 2 ? accent : fg);
        w_theme_draw_text(dc,parts[i],-1,&b->tip.parts[i],DT_WORDBREAK|DT_NOPREFIX|DT_EDITCONTROL|DT_END_ELLIPSIS);
    }
    SelectObject(dc,previous);
}

static void w_tip_layout(w_bar_window *b);

static bool w_tip_fade(w_bar_window *b,bool entering) {
    return w_layer_fade_to(b->tip.window,&b->tip.fade,entering ? 255 : 0,
                           w_theme_layer_timing(entering),w_theme_motion_enabled());
}

static LRESULT CALLBACK w_tip_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    w_bar_window *b=(w_bar_window *)GetWindowLongPtrW(w,GWLP_USERDATA);
    if (msg == WM_NCCREATE)
        SetWindowLongPtrW(w,GWLP_USERDATA,(LONG_PTR)((CREATESTRUCTW *)lp)->lpCreateParams);
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    if (msg == WM_ERASEBKGND) return 1;
    if (b && (msg==WM_FONTCHANGE || msg==winwm_theme_updated)) {
        if (msg==WM_FONTCHANGE) w_theme_fonts_changed();
        if (b->tip.showing) w_tip_layout(b);
        return 0;
    }
    if (b && msg==WM_TIMER && wp==layer_fade_timer) {
        if (b->tip.fade.start && !w_layer_fade_tick(w,&b->tip.fade,true) && !b->tip.showing) ShowWindow(w,SW_HIDE);
        return 0;
    }
    if (b && b->tip.fade.start && (msg==winwm_motion_updated || msg==WM_SYSCOLORCHANGE ||
        (msg==WM_SETTINGCHANGE && (wp==SPI_SETCLIENTAREAANIMATION || w_appearance_setting(wp,lp))))) {
        if (!w_tip_fade(b,b->tip.showing) && !b->tip.showing) ShowWindow(w,SW_HIDE);
    }
    if (b && msg==WM_SHOWWINDOW && !wp) {
        KillTimer(w,layer_fade_timer);
        b->tip.fade=(w_layer_fade){0};
        b->tip.showing=false;
    }
    if (b && (msg == WM_PRINTCLIENT || msg == WM_PAINT)) {
        PAINTSTRUCT ps;
        HDC dc = msg == WM_PAINT ? BeginPaint(w,&ps) : (HDC)wp;
        RECT r;
        GetClientRect(w,&r);
        w_tip_paint(b,dc,r);
        if (msg == WM_PAINT) EndPaint(w,&ps);
        return 0;
    }
    return DefWindowProcW(w,msg,wp,lp);
}

static HWND w_tip_create(w_bar_window *b) {
    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSW cls = {.lpszClassName=L"WinwmBarTooltip",.hInstance=inst,.lpfnWndProc=w_tip_proc};
    RegisterClassW(&cls);
    HWND tip=CreateWindowExW(WS_EX_NOACTIVATE|WS_EX_TRANSPARENT|WS_EX_TOOLWINDOW|WS_EX_TOPMOST,
                            cls.lpszClassName,L"",WS_POPUP,0,0,0,0,b->hwnd,NULL,inst,b);
    if (tip) w_theme_material(tip);
    return tip;
}

void w_bar_tip_move(w_bar_window *b) {
    if (!b->tip.showing) return;
    w_popup_anchor anchor;
    RECT box;
    MONITORINFO mi={.cbSize=sizeof(mi)};
    if (!w_bar_anchor(b,b->tip.index,&anchor) || !GetWindowRect(b->tip.window,&box) ||
        !GetMonitorInfoW(MonitorFromWindow(b->hwnd,MONITOR_DEFAULTTONEAREST),&mi)) return;
    RECT pos=w_popup_bounds(&anchor,mi.rcWork,box.right-box.left,box.bottom-box.top,w_theme_spacing(b->dpi));
    if (!EqualRect(&box,&pos)) SetWindowPos(b->tip.window,NULL,pos.left,pos.top,pos.right-pos.left,pos.bottom-pos.top,
                                           SWP_NOZORDER|SWP_NOACTIVATE);
}

static void w_tip_layout(w_bar_window *b) {
    w_popup_anchor anchor;
    MONITORINFO mi = {.cbSize=sizeof(mi)};
    if (!w_bar_anchor(b,b->tip.index,&anchor) || !GetMonitorInfoW(MonitorFromWindow(b->hwnd,MONITOR_DEFAULTTONEAREST),&mi)) return;
    int gap=w_theme_spacing(b->dpi), pad=2*gap;
    RECT area=w_popup_bounds(&anchor,mi.rcWork,mi.rcWork.right-mi.rcWork.left,mi.rcWork.bottom-mi.rcWork.top,gap);
    int limit=max(1,min(MulDiv(336,b->dpi,96),area.right-area.left-2*pad));
    const wchar_t *text[]={b->tip.heading,b->tip.body,b->tip.help};
    HDC dc=GetDC(b->tip.window);
    if (!dc) return;
    HFONT previous=GetCurrentObject(dc,OBJ_FONT);
    int width=0,height=pad;
    for (int i=0;i<3;i++) {
        RECT line={0,0,limit,0};
        if (!*text[i]) continue;
        SelectObject(dc,w_tip_font(b,i));
        w_theme_draw_text(dc,text[i],-1,&line,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX|DT_EDITCONTROL);
        b->tip.parts[i]=(RECT){pad,height,pad+limit,height+line.bottom};
        width=max(width,min(limit,line.right));
        height+=line.bottom+gap;
    }
    SelectObject(dc,previous);
    ReleaseDC(b->tip.window,dc);
    width=min(width+pad*2,area.right-area.left);
    height=min(height-gap+pad,area.bottom-area.top);
    for (int i=0;i<3;i++) {
        b->tip.parts[i].right=width-pad;
        b->tip.parts[i].bottom=min(b->tip.parts[i].bottom,height-pad);
    }
    RECT pos=w_popup_bounds(&anchor,mi.rcWork,width,height,gap);
    if (!b->tip.showing) {
        b->tip.showing=true;
        if (!IsWindowVisible(b->tip.window)) b->tip.fade.value=0;
        w_tip_fade(b,true);
    }
    SetWindowPos(b->tip.window,HWND_TOPMOST,pos.left,pos.top,width,height,SWP_SHOWWINDOW|SWP_NOACTIVATE);
    InvalidateRect(b->tip.window,NULL,FALSE);
}

static void w_tip_present(w_bar_window *b, bool first) {
    wchar_t heading[count_of(b->tip.heading)]={0},body[512]={0},help[320]={0};
    w_tip_text(b,heading,body,help);
    if (!first && !wcscmp(heading,b->tip.heading) && !wcscmp(body,b->tip.body) && !wcscmp(help,b->tip.help))
        return;
    wcscpy(b->tip.heading,heading);
    wcscpy(b->tip.body,body);
    wcscpy(b->tip.help,help);
    if (!b->tip.window) b->tip.window = w_tip_create(b);
    if (!b->tip.window) return;
    wchar_t accessible[count_of(heading)+count_of(body)+count_of(help)+2];
    swprintf(accessible,count_of(accessible),L"%ls\n%ls\n%ls",heading,body,help);
    SetWindowTextW(b->tip.window,accessible);
    w_tip_layout(b);
}

void w_bar_tip_hide(w_bar_window *b) {
    if (b->tip.pending) KillTimer(b->hwnd,bar_tip_show_timer);
    if (b->tip.showing) {
        b->tip.showing=false;
        KillTimer(b->hwnd,bar_tip_hide_timer);
        if (!IsWindowVisible(b->hwnd) || !w_tip_fade(b,false)) ShowWindow(b->tip.window,SW_HIDE);
    }
    b->tip.pending=false;
}

void w_bar_tip_track(w_bar_window *b, int index) {
    if (index < 0 || index >= b->hit_count || !b->config->bar_tooltip_duration || b->hits[index].tray_hide_tip ||
        (b->hits[index].style && b->hits[index].style->hide_tooltip)) {
        w_bar_tip_hide(b);
        b->tip.index = -1;
        return;
    }
    const struct w_bar_hit *hit = &b->hits[index];
    if (index == b->tip.index && hit->workspace == b->tip.workspace && !strcmp(hit->instance,b->tip.instance))
        return;
    w_bar_tip_hide(b);
    b->tip.index = index;
    b->tip.workspace = hit->workspace;
    strcpy(b->tip.instance,hit->instance);
    if (w_bar_menu_active() || b->pressed) return;
    int delay=b->config->bar_tooltip_delay;
    b->tip.deadline=GetTickCount64()+delay;
    b->tip.pending=SetTimer(b->hwnd,bar_tip_show_timer,max(1,delay),NULL)!=0;
}

void w_bar_tip_update(w_bar_window *b) {
    if (b->tip.showing) w_tip_present(b,false);
}

void w_bar_tip_timer(w_bar_window *b, UINT_PTR timer) {
    if (GetTickCount64() < b->tip.deadline) return;
    if (timer == bar_tip_hide_timer) { if (b->tip.showing) w_bar_tip_hide(b); return; }
    if (!b->tip.pending) return;
    KillTimer(b->hwnd,bar_tip_show_timer);
    b->tip.pending=false;
    POINT p;
    int index=b->tip.index;
    if (!b->pointer_inside || b->pressed || w_bar_menu_active() || index<0 || index>=b->hit_count ||
        !GetCursorPos(&p) || WindowFromPoint(p)!=b->hwnd || !ScreenToClient(b->hwnd,&p) ||
        !PtInRect(&b->hits[index].box,p)) return;
    b->pointer=p;
    w_tip_present(b,true);
    b->tip.deadline=GetTickCount64()+b->config->bar_tooltip_duration;
    if (b->tip.showing && !SetTimer(b->hwnd,bar_tip_hide_timer,b->config->bar_tooltip_duration,NULL)) w_bar_tip_hide(b);
}
