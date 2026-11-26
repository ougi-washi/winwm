#pragma once
#include "win_platform/win_platform.h"
#include "theme/caption.h"
#include "theme/metrics.h"
#include "theme/native_controls.h"
#include "theme/control_hover.h"
#include <wchar.h>

////////////////
//~ Tools

// Keep native string storage, edit/caret behavior and accessibility while the
// theme paints dropdown rows. Owner draw must be requested at creation.
enum {
    w_tool_dropdown_list=CBS_DROPDOWNLIST|CBS_OWNERDRAWFIXED|CBS_HASSTRINGS,
    w_tool_dropdown_edit=CBS_DROPDOWN|CBS_OWNERDRAWFIXED|CBS_HASSTRINGS
};

int w_capture_run(const wchar_t *mode);
int w_settings_run(const wchar_t *page);
int w_clock_run(void);
int w_color_run(void);
int w_packages_run(const wchar_t *mode);
int w_releases_run(void);
int w_activity_run(void);
int w_project_run(const w_desktop_config *config);
int w_toolbar_run(void);
UINT w_tool_work_area(MONITORINFO *monitor);
bool w_tool_show(HWND window);
bool w_tool_native_dialog(void);
HMONITOR w_tool_popup_monitor(void);
void w_tool_popup_cleanup(void);
typedef struct { int width,height; } w_tool_feedback;
enum { w_tool_feedback_changed=WM_APP+0x393 };
void w_tool_track_feedback(HWND control,w_tool_feedback *flow);
int w_tool_feedback_height(HWND control,w_tool_feedback *flow,int width);
typedef enum w_input_page { input_languages, input_keyboards, input_methods } w_input_page;
int w_language_setup(w_input_page page);

static inline bool w_tool_repeated_dialog_key(const MSG *key) {
    return key->message == WM_KEYDOWN && (key->wParam == VK_RETURN || key->wParam == VK_ESCAPE) &&
           (HIWORD(key->lParam) & KF_REPEAT);
}

typedef struct {
    int row;
    bool open, accepted;
} w_tool_choice;

// A dropped native combo reports every highlighted row, even if Escape follows.
// Restore its committed row after Windows finishes closing the list.
static inline bool w_tool_choice_changed(w_tool_choice *choice, HWND combo, UINT event) {
    if (event == CBN_DROPDOWN) {
        choice->row = (int)SendMessageW(combo,CB_GETCURSEL,0,0);
        choice->open = true;
        choice->accepted = false;
    } else if (event == CBN_SELENDOK) {
        choice->accepted = true;
    } else if (event == CBN_CLOSEUP || (event == CBN_SELCHANGE && !choice->open)) {
        int row = (int)SendMessageW(combo,CB_GETCURSEL,0,0);
        if (event == CBN_CLOSEUP) {
            choice->open = false;
            if (!choice->accepted) {
                if (row != choice->row) SendMessageW(combo,CB_SETCURSEL,choice->row,0);
                return false;
            }
        }
        if (row >= 0 && row != choice->row) {
            choice->row = row;
            return true;
        }
    }
    return false;
}

static inline void w_tool_status(HWND control, const wchar_t *text) {
    wchar_t shown[512] = {0};
    size_t length = wcslen(text);
    if (length >= count_of(shown) || GetWindowTextW(control,shown,count_of(shown)) != (int)length || wcscmp(shown,text))
        SetWindowTextW(control,text);
}

static inline void w_tool_visible(HWND control,bool visible) {
    if (control && !!(GetWindowLongW(control,GWL_STYLE)&WS_VISIBLE)!=visible)
        ShowWindow(control,visible ? SW_SHOWNA : SW_HIDE);
}

typedef struct {
    w_tool_choice choice;
    wchar_t draft[48];
    DWORD selection;
} w_tool_edit_choice;

// Power durations and calendar years allow free text between preset values.
static inline bool w_tool_edit_choice_changed(w_tool_edit_choice *edit, HWND combo, UINT event) {
    if (event == CBN_DROPDOWN) {
        GetWindowTextW(combo,edit->draft,count_of(edit->draft));
        edit->selection = (DWORD)SendMessageW(combo,CB_GETEDITSEL,0,0);
    }
    bool changed = w_tool_choice_changed(&edit->choice,combo,event);
    if (event == CBN_CLOSEUP && !edit->choice.accepted) {
        w_tool_status(combo,edit->draft);
        SendMessageW(combo,CB_SETEDITSEL,0,edit->selection);
    }
    return changed;
}

// Defer painting to the message loop so layout changes appear together on resize.
static inline void w_tool_place(HWND control, int x, int y, int width, int height) {
    if (!control) return;
    SetWindowPos(control, NULL, x, y+w_theme_caption_height(GetParent(control)), max(1,width), max(1,height),
                 SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
}

// Win32 treats a combo's layout height as the height of the open list. Size
// its closed field independently, including the border provided by Windows.
static inline void w_tool_place_combo(HWND control, int x, int y, int width, int side, int rows) {
    if (!control) return;
    if (SendMessageW(control,CB_GETITEMHEIGHT,0,0)!=side) SendMessageW(control,CB_SETITEMHEIGHT,0,side);
    w_tool_place(control,x,y,width,side*(rows+1));
    RECT bounds;
    LRESULT field = SendMessageW(control,CB_GETITEMHEIGHT,(WPARAM)-1,0);
    if (field >= 0 && GetWindowRect(control,&bounds)) {
        int chrome = max(0,bounds.bottom-bounds.top-(int)field);
        int height = max(1,side-chrome);
        if (field != height) SendMessageW(control,CB_SETITEMHEIGHT,(WPARAM)-1,height);
    }
}

// The closed selector needs the native arrow, two border edges and the
// owner-drawn field's text margins. Its sibling receives the remaining width.
static inline int w_tool_selector_width(HWND combo,int text,UINT dpi) {
    int chrome=GetSystemMetricsForDpi(SM_CXVSCROLL,dpi)+
               2*(GetSystemMetricsForDpi(SM_CXEDGE,dpi)+GetSystemMetricsForDpi(SM_CXBORDER,dpi));
    COMBOBOXINFO info={.cbSize=sizeof(info)};
    RECT window;
    if (combo && GetDpiForWindow(combo)==dpi && GetComboBoxInfo(combo,&info) && GetWindowRect(combo,&window)) {
        int field=info.rcItem.right-info.rcItem.left;
        if (field>0) chrome=window.right-window.left-field;
    }
    return text+2*w_theme_spacing(dpi)+chrome;
}

static inline int w_tool_widest_option(HFONT font,const wchar_t *const *options,size_t count) {
    int width=0;
    for (size_t i=0;i<count;++i) {
        int ink=w_theme_text_width(font,options[i],DT_NOPREFIX);
        width=max(width,ink);
    }
    return width;
}

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

static inline void w_tool_fit_height(HWND window,int delta,bool center) {
    if (!delta) return;
    RECT old;
    GetWindowRect(window,&old);
    RECT next=old;
    if (center) OffsetRect(&next,0,-delta/2);
    next.bottom+=delta;
    next=w_tool_fit_to_monitor(next,MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST));
    if (!EqualRect(&old,&next))
        SetWindowPos(window,NULL,next.left,next.top,next.right-next.left,next.bottom-next.top,SWP_NOZORDER|SWP_NOACTIVATE);
}

// The caller reselects the message font and lays out its controls at the current DPI.
static inline bool w_tool_font_change(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DPICHANGED) {
        const RECT *suggested = (const RECT *)lp;
        RECT r = w_tool_fit_to_monitor(*suggested, MonitorFromRect(suggested, MONITOR_DEFAULTTONEAREST));
        SetWindowPos(window, NULL, r.left, r.top, r.right-r.left, r.bottom-r.top, SWP_NOZORDER);
        return true;
    }
    if (msg != WM_SETTINGCHANGE || wp != SPI_SETNONCLIENTMETRICS) return false;
    RedrawWindow(window,NULL,NULL,RDW_INVALIDATE|RDW_ALLCHILDREN);
    return true;
}

static inline void w_tool_minimum(HWND window, MINMAXINFO *limits, LONG width, LONG height) {
    RECT r = {0,0,width,height};
    w_theme_adjust_rect(&r,GetWindowLongW(window,GWL_STYLE),
                        GetWindowLongW(window,GWL_EXSTYLE),GetDpiForWindow(window));
    r = w_tool_fit_to_monitor(r,MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST));
    limits->ptMinTrackSize = (POINT){r.right-r.left, r.bottom-r.top};
}
