#include "bar/tooltip.h"
#include "bar/menu.h"
#include "theme/theme.h"
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

static COLORREF w_rgb(uint32_t value) { return RGB(value >> 16, value >> 8, value); }

static void w_tip_paint(w_bar_window *b, HDC dc, RECT r) {
    const w_desktop_config *c = b->config;
    COLORREF fg = b->high_contrast ? GetSysColor(COLOR_INFOTEXT) : w_rgb(c->foreground);
    COLORREF bg = b->high_contrast ? GetSysColor(COLOR_INFOBK) : w_rgb(c->background);
    COLORREF accent = b->high_contrast ? fg : w_rgb(c->accent);
    SetDCBrushColor(dc,bg);
    FillRect(dc,&r,GetStockObject(DC_BRUSH));
    SetDCBrushColor(dc,b->high_contrast ? fg : RGB((GetRValue(accent)+GetRValue(bg))/2,
                             (GetGValue(accent)+GetGValue(bg))/2,(GetBValue(accent)+GetBValue(bg))/2));
    FrameRect(dc,&r,GetStockObject(DC_BRUSH));
    SetBkMode(dc,TRANSPARENT);
    HFONT previous = SelectObject(dc,b->heading_font);
    const wchar_t *parts[] = {b->tip.heading,b->tip.body,b->tip.help};
    for (int i=0;i<3;i++) {
        if (!*parts[i]) continue;
        SelectObject(dc,i ? b->font : b->heading_font);
        SetTextColor(dc,i == 2 ? accent : fg);
        DrawTextW(dc,parts[i],-1,&b->tip.parts[i],DT_WORDBREAK|DT_NOPREFIX|DT_EDITCONTROL|DT_END_ELLIPSIS);
    }
    SelectObject(dc,previous);
}

static LRESULT CALLBACK w_tip_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    w_bar_window *b=(w_bar_window *)GetWindowLongPtrW(w,GWLP_USERDATA);
    if (msg == WM_NCCREATE)
        SetWindowLongPtrW(w,GWLP_USERDATA,(LONG_PTR)((CREATESTRUCTW *)lp)->lpCreateParams);
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    if (msg == WM_ERASEBKGND) return 1;
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

static POINT w_tip_position(w_bar_window *b, POINT anchor, RECT area, int width, int height) {
    RECT bar;
    GetWindowRect(b->hwnd,&bar);
    int gap=MulDiv(7,b->dpi,96), x=anchor.x+gap*2,y=anchor.y+gap*2;
    if (bar.right-bar.left > bar.bottom-bar.top) {
        y=bar.bottom+gap+height<=area.bottom ? bar.bottom+gap : bar.top-gap-height;
        if (x+width>area.right) x=anchor.x-gap*2-width;
    } else {
        x=bar.right+gap+width<=area.right ? bar.right+gap : bar.left-gap-width;
        if (y+height>area.bottom) y=anchor.y-gap*2-height;
    }
    b->tip.anchor=anchor;
    return (POINT){max(area.left,min(x,area.right-width)),max(area.top,min(y,area.bottom-height))};
}

void w_bar_tip_move(w_bar_window *b) {
    if (!b->tip.showing) return;
    POINT p=b->pointer;
    if (!ClientToScreen(b->hwnd,&p)) return;
    int step=max(1,MulDiv(6,b->dpi,96));
    if (abs(p.x-b->tip.anchor.x)<step && abs(p.y-b->tip.anchor.y)<step) return;
    RECT box;
    MONITORINFO mi={.cbSize=sizeof(mi)};
    if (!GetWindowRect(b->tip.window,&box) ||
        !GetMonitorInfoW(MonitorFromPoint(p,MONITOR_DEFAULTTONEAREST),&mi)) return;
    POINT pos=w_tip_position(b,p,mi.rcWork,box.right-box.left,box.bottom-box.top);
    if (box.left!=pos.x || box.top!=pos.y)
        SetWindowPos(b->tip.window,NULL,pos.x,pos.y,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
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
    POINT anchor = b->pointer;
    if (!ClientToScreen(b->hwnd,&anchor)) return;
    MONITORINFO mi = {.cbSize=sizeof(mi)};
    if (!GetMonitorInfoW(MonitorFromPoint(anchor,MONITOR_DEFAULTTONEAREST),&mi)) return;
    RECT area = mi.rcWork;
    int pad=MulDiv(12,b->dpi,96), gap=MulDiv(7,b->dpi,96);
    int limit=max(1,min(MulDiv(336,b->dpi,96),area.right-area.left-2*pad));
    const wchar_t *text[]={heading,body,help};
    HDC dc=GetDC(b->tip.window);
    if (!dc) return;
    HFONT previous=SelectObject(dc,b->font);
    int width=0,height=pad;
    for (int i=0;i<3;i++) {
        RECT line={0,0,limit,0};
        if (!*text[i]) continue;
        SelectObject(dc,i ? b->font : b->heading_font);
        DrawTextW(dc,text[i],-1,&line,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX|DT_EDITCONTROL);
        b->tip.parts[i]=(RECT){pad,height,pad+limit,height+line.bottom};
        width=max(width,min(limit,line.right));
        height+=line.bottom+gap;
    }
    SelectObject(dc,previous);
    ReleaseDC(b->tip.window,dc);
    width=min(width+pad*2,area.right-area.left);
    height=min(height-gap+pad,area.bottom-area.top);
    for (int i=0;i<3;i++) b->tip.parts[i].right=width-pad;
    POINT pos=w_tip_position(b,anchor,area,width,height);
    wchar_t accessible[count_of(heading)+count_of(body)+count_of(help)+2];
    swprintf(accessible,count_of(accessible),L"%ls\n%ls\n%ls",heading,body,help);
    SetWindowTextW(b->tip.window,accessible);
    SetWindowPos(b->tip.window,HWND_TOPMOST,pos.x,pos.y,width,height,SWP_SHOWWINDOW|SWP_NOACTIVATE);
    InvalidateRect(b->tip.window,NULL,FALSE);
    b->tip.showing=true;
}

void w_bar_tip_hide(w_bar_window *b) {
    if (b->tip.pending) KillTimer(b->hwnd,bar_tip_show_timer);
    if (b->tip.showing) {
        KillTimer(b->hwnd,bar_tip_hide_timer);
        ShowWindow(b->tip.window,SW_HIDE);
    }
    b->tip.showing = b->tip.pending = false;
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
