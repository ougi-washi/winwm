#pragma once
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>

typedef struct w_popup_anchor {
    HWND bar;
    RECT button;
    char edge[8];
    int source, occurrence, workspace;
    char instance[56];
} w_popup_anchor;

static inline bool w_popup_same_source(const w_popup_anchor *a,const w_popup_anchor *b) {
    return a->bar==b->bar && a->occurrence==b->occurrence && a->workspace==b->workspace &&
           !strcmp(a->instance,b->instance);
}

enum { popup_event_chars = 80, winwm_popup_released = WM_APP + 0x434, winwm_popup_moved = WM_APP + 0x438 };
static const wchar_t w_popup_child_property[]=L"winwm.popup.child";
static inline void w_popup_event_name(wchar_t name[popup_event_chars], DWORD pid, DWORD serial) {
    swprintf(name, popup_event_chars, L"Local\\winwm.toolbar.%lu.%lu", pid, serial);
}

// Every toolbar target spans the bar's short axis. Store its two nonnegative
// client offsets and dock edge in one x64 window property, so a starting child
// reads one coherent placement, even on a desktop with negative screen origins.
static inline bool w_popup_publish(const w_popup_anchor *a,const wchar_t *key) {
    RECT span=a->button;
    MapWindowPoints(NULL,a->bar,(POINT *)&span,2);
    unsigned edge=!strcmp(a->edge,"top") ? 0 : !strcmp(a->edge,"bottom") ? 1 : !strcmp(a->edge,"left") ? 2 : 3;
    LONG first=edge<2 ? span.left : span.top, last=edge<2 ? span.right : span.bottom;
    if (first<0 || last<=first) return false;
    uint64_t packed=(uint32_t)first | ((uint64_t)last<<31) | ((uint64_t)edge<<62);
    return SetPropW(a->bar,key,(HANDLE)(uintptr_t)packed)!=FALSE;
}

static inline bool w_popup_read(w_popup_anchor *a,const wchar_t *key) {
    uint64_t packed=(uintptr_t)GetPropW(a->bar,key);
    if (!packed || !IsWindowVisible(a->bar) || !GetClientRect(a->bar,&a->button)) return false;
    unsigned edge=(unsigned)(packed>>62);
    LONG first=(LONG)(packed&INT32_MAX),last=(LONG)((packed>>31)&INT32_MAX);
    if (edge<2) { a->button.left=first; a->button.right=last; }
    else { a->button.top=first; a->button.bottom=last; }
    MapWindowPoints(a->bar,NULL,(POINT *)&a->button,2);
    strcpy(a->edge,edge==0 ? "top" : edge==1 ? "bottom" : edge==2 ? "left" : "right");
    return true;
}

static inline RECT w_popup_area(HWND bar) {
    MONITORINFO monitor = {.cbSize = sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(bar, MONITOR_DEFAULTTONEAREST), &monitor);
    return GetPropW(bar, L"winwm.preview") ? monitor.rcWork : monitor.rcMonitor;
}

// Captionless resizable popups paint to the edge but still use Win32's modal
// resize loop and keyboard Size command. Reserve only the exposed resize grip.
static inline LRESULT w_popup_resize_hit(HWND window, POINT client_point) {
    RECT area;
    GetClientRect(window,&area);
    if (!PtInRect(&area,client_point)) return HTNOWHERE;
    UINT dpi=GetDpiForWindow(window);
    int pad=GetSystemMetricsForDpi(SM_CXPADDEDBORDER,dpi);
    int x=min(area.right/2,GetSystemMetricsForDpi(SM_CXSIZEFRAME,dpi)+pad);
    int y=min(area.bottom/2,GetSystemMetricsForDpi(SM_CYSIZEFRAME,dpi)+pad);
    bool left=client_point.x<x, right=client_point.x>=area.right-x;
    if (client_point.y<y) return left ? HTTOPLEFT : right ? HTTOPRIGHT : HTTOP;
    if (client_point.y>=area.bottom-y) return left ? HTBOTTOMLEFT : right ? HTBOTTOMRIGHT : HTBOTTOM;
    return left ? HTLEFT : right ? HTRIGHT : HTCLIENT;
}

// Align to the near end of the button and keep the panel on the desktop side of the bar.
static inline RECT w_popup_bounds(const w_popup_anchor *anchor, RECT area, int width, int height, int gap) {
    RECT button = anchor->button, bar;
    if (!GetWindowRect(anchor->bar, &bar)) bar = button;
    BOOL left = !strcmp(anchor->edge, "left"), right = !strcmp(anchor->edge, "right");
    BOOL bottom = !strcmp(anchor->edge, "bottom");
    if (left) area.left = min(area.right - 1, max(area.left, bar.right + gap));
    else if (right) area.right = max(area.left + 1, min(area.right, bar.left - gap));
    else if (bottom) area.bottom = max(area.top + 1, min(area.bottom, bar.top - gap));
    else area.top = min(area.bottom - 1, max(area.top, bar.bottom + gap));
    width = max(1, min(width, area.right - area.left));
    height = max(1, min(height, area.bottom - area.top));
    LONG x, y;
    if (left || right) {
        x = left ? area.left : area.right - width;
        y = button.top - area.top <= area.bottom - button.bottom ? button.top : button.bottom - height;
    } else {
        x = button.left - area.left <= area.right - button.right ? button.left : button.right - width;
        y = bottom ? area.bottom - height : area.top;
    }
    x = max(area.left, min(x, area.right - width));
    y = max(area.top, min(y, area.bottom - height));
    return (RECT){x, y, x + width, y + height};
}
