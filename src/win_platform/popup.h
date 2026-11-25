#pragma once
#include <windows.h>
#include <string.h>
#include <wchar.h>

typedef struct w_popup_anchor {
    HWND bar;
    RECT button;
    char edge[8];
    int source;
} w_popup_anchor;

enum { popup_event_chars = 80, winwm_popup_released = WM_APP + 0x434 };
static inline void w_popup_event_name(wchar_t name[popup_event_chars], DWORD pid, DWORD serial) {
    swprintf(name, popup_event_chars, L"Local\\winwm.toolbar.%lu.%lu", pid, serial);
}

static inline RECT w_popup_area(HWND bar) {
    MONITORINFO monitor = {.cbSize = sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(bar, MONITOR_DEFAULTTONEAREST), &monitor);
    return GetPropW(bar, L"winwm.preview") ? monitor.rcWork : monitor.rcMonitor;
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
