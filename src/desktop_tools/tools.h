#pragma once
#include "win_platform/win_platform.h"

////////////////
//~ Tools

int w_capture_run(const wchar_t *mode);
int w_settings_run(const wchar_t *page);
int w_clock_run(void);
int w_color_run(void);
int w_activity_run(void);
int w_project_run(const w_desktop_config *config);
UINT w_tool_work_area(MONITORINFO *monitor);
int w_language_setup(bool keyboards);

static inline RECT w_tool_fit_to_monitor(RECT bounds, HMONITOR monitor) {
    MONITORINFO info = {.cbSize = sizeof(info)};
    if (!GetMonitorInfoW(monitor, &info)) return bounds;
    RECT area = info.rcWork;
    LONG width = min(bounds.right-bounds.left, area.right-area.left);
    LONG height = min(bounds.bottom-bounds.top, area.bottom-area.top);
    LONG left = max(area.left, min(bounds.left, area.right-width));
    LONG top = max(area.top, min(bounds.top, area.bottom-height));
    return (RECT){left, top, left+width, top+height};
}

static inline void w_tool_resize_for_dpi(HWND window, const RECT *suggested) {
    RECT r = w_tool_fit_to_monitor(*suggested, MonitorFromRect(suggested, MONITOR_DEFAULTTONEAREST));
    SetWindowPos(window, NULL, r.left, r.top, r.right-r.left, r.bottom-r.top, SWP_NOZORDER);
}

static inline void w_tool_minimum(HWND window, MINMAXINFO *limits, LONG width, LONG height) {
    RECT r = w_tool_fit_to_monitor((RECT){0,0,width,height}, MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST));
    limits->ptMinTrackSize = (POINT){r.right-r.left, r.bottom-r.top};
}
