#include "desktop_tools/display.h"
#include "desktop_tools/tools.h"
#include "desktop_tools/rows.h"
#include "theme/icon_button.h"
#include "win_platform/percent_slider.h"
#include <commctrl.h>
#include <physicalmonitorenumerationapi.h>
#include <highlevelmonitorconfigurationapi.h>
#include <stdlib.h>
#include <wchar.h>

typedef struct w_display {
    wchar_t device[CCHDEVICENAME];
    wchar_t label[220];
    HANDLE physical;
    bool write_pending;
    DWORD requested;
} w_display;

static HWND panel, modes_list, message_line, preview_button, selector, slider, level;
static w_display displays[64];
static int display_count;
static DEVMODEW modes[1024], original;
static int mode_count, preview_row;
static wchar_t preview_device[CCHDEVICENAME];
static ULONGLONG deadline;
static DWORD minimum, maximum, brightness;
static bool adjustable, changing_mode;
static bool refresh_posted, read_requested, closing, first_scan;
static w_tool_choice display_choice = {.row=-1};
static bool topology_after_close;
static unsigned topology;
typedef struct {
    LONG refs;
    HANDLE physical;
    int display;
    unsigned topology;
    bool write, rejected, available;
    DWORD requested, minimum, brightness, maximum;
} w_brightness_job;
static w_brightness_job *brightness_job;
static HANDLE brightness_thread;
typedef struct {
    LONG refs;
    bool full;
    HMONITOR active;
    wchar_t previous[220], device[CCHDEVICENAME];
    w_display displays[count_of(displays)];
    int display_count, preferred;
    DEVMODEW modes[count_of(modes)], current;
    int mode_count;
} w_display_scan;
static w_display_scan *scan;
static HANDLE scan_thread;
static bool scan_queued, topology_queued;
enum { refresh_display = WM_APP + 60 };

static int w_selected_display(void) {
    return display_choice.row;
}

static bool w_same_mode(const DEVMODEW *a, const DEVMODEW *b) {
    return a->dmPelsWidth == b->dmPelsWidth && a->dmPelsHeight == b->dmPelsHeight &&
           a->dmDisplayFrequency == b->dmDisplayFrequency && a->dmDisplayFlags == b->dmDisplayFlags &&
           a->dmBitsPerPel == b->dmBitsPerPel && a->dmDisplayOrientation == b->dmDisplayOrientation;
}

static void w_clear_displays(void) {
    ++topology;
    for (int i = 0; i < display_count; ++i)
        if (displays[i].physical)
            DestroyPhysicalMonitor(displays[i].physical);
    display_count = 0;
}

static BOOL CALLBACK w_add_display(HMONITOR monitor, HDC dc, LPRECT rect, LPARAM arg) {
    (void)dc;
    (void)rect;
    w_display_scan *s = (w_display_scan *)arg;
    MONITORINFOEXW info = {.cbSize = sizeof(info)};
    if (!GetMonitorInfoW(monitor, (MONITORINFO *)&info))
        return TRUE;
    DWORD count = 0;
    PHYSICAL_MONITOR *physical = NULL;
    if (GetNumberOfPhysicalMonitorsFromHMONITOR(monitor, &count) && count) {
        physical = calloc(count, sizeof(*physical));
        if (physical && !GetPhysicalMonitorsFromHMONITOR(monitor, count, physical)) {
            free(physical);
            physical = NULL;
        }
    }
    for (DWORD i = 0; i < (physical ? count : 1); ++i) {
        HANDLE handle = physical ? physical[i].hPhysicalMonitor : NULL;
        wchar_t label[220];
        const wchar_t *source = wcsrchr(info.szDevice, L'\\');
        swprintf(label, count_of(label), L"%ls   %ls", source ? source + 1 : info.szDevice,
                 physical ? physical[i].szPhysicalMonitorDescription : L"");
        if (count > 1 && physical) {
            size_t n = wcslen(label);
            swprintf(label + n, count_of(label)-n, L"   (%lu)", i + 1);
        }
        if (s->display_count >= (int)count_of(s->displays)) {
            if (handle)
                DestroyPhysicalMonitor(handle);
            continue;
        }
        w_display *d = &s->displays[s->display_count++];
        d->physical=handle;
        wcscpy(d->label,label);
        wcscpy(d->device, info.szDevice);
        if (monitor == s->active && !i) s->preferred = s->display_count-1;
    }
    free(physical);
    return TRUE;
}

static void w_brightness_label(int value) {
    wchar_t s[48];
    if (value >= 0)
        swprintf(s, count_of(s), L"Brightness   %d%%", value);
    w_tool_status(level,value >= 0 ? s : L"Brightness unavailable");
}

static void w_show_brightness(void) {
    int value = adjustable ? (int)(((uint64_t)(brightness - minimum)*100 + (maximum - minimum)/2)/(maximum - minimum)) : 0;
    EnableWindow(slider, adjustable);
    value=w_percent_slider_sync(slider,value);
    w_brightness_label(adjustable ? value : -1);
}

static void w_release_brightness(w_brightness_job *job) {
    if (job && !InterlockedDecrement(&job->refs)) {
        if (job->physical) DestroyPhysicalMonitor(job->physical);
        free(job);
    }
}

static DWORD WINAPI w_brightness_worker(void *arg) {
    w_brightness_job *job = arg;
    DWORD caps, temperatures;
    if (job->write) job->rejected = !SetMonitorBrightness(job->physical,job->requested);
    job->available = (job->write || (GetMonitorCapabilities(job->physical,&caps,&temperatures) &&
                      (caps & MC_CAPS_BRIGHTNESS))) &&
                     GetMonitorBrightness(job->physical,&job->minimum,&job->brightness,&job->maximum) &&
                     job->maximum > job->minimum && job->brightness >= job->minimum && job->brightness <= job->maximum;
    w_release_brightness(job);
    return 0;
}

// A slow DDC/CI monitor must not block dismissal, mode preview rollback, or other controls.
static void w_next_brightness(void) {
    if (brightness_thread) return;
    int selected = w_selected_display(), i = 0;
    while (i < display_count && !displays[i].write_pending) ++i;
    if (i == display_count) {
        if (!read_requested || selected < 0 || selected >= display_count) return;
        i = selected;
    }
    if (i == selected) read_requested = false;
    w_display *d = &displays[i];
    if (!d->physical) return;
    w_brightness_job *job = calloc(1,sizeof(*job));
    if (job) {
        job->refs = 2;
        job->display = i;
        job->topology = topology;
        job->physical = d->physical;
        job->write = d->write_pending;
        job->requested = d->requested;
        brightness_thread = CreateThread(NULL,0,w_brightness_worker,job,0,NULL);
    }
    d->write_pending = false;
    if (brightness_thread) {
        d->physical = NULL; // The worker owns the monitor until its completion is consumed.
        brightness_job = job;
    } else {
        free(job);
        if (i==selected && !scan_thread) w_show_brightness();
        SetWindowTextW(message_line,L"Could not start the brightness request.");
    }
}

int w_display_pending(HANDLE handles[2]) {
    int n = 0;
    if (scan_thread) handles[n++] = scan_thread;
    if (brightness_thread) handles[n++] = brightness_thread;
    return n;
}

static void w_finish_brightness(void) {
    w_brightness_job *job = brightness_job;
    CloseHandle(brightness_thread);
    brightness_thread = NULL;
    brightness_job = NULL;
    if (job->topology == topology) {
        w_display *d = &displays[job->display];
        d->physical = job->physical;
        job->physical = NULL;
        if (job->display == w_selected_display() && !d->write_pending && !scan_thread) {
            read_requested = false;
            adjustable = job->available;
            minimum = job->minimum; brightness = job->brightness; maximum = job->maximum;
            w_show_brightness();
            if (job->rejected) SetWindowTextW(message_line,L"The monitor rejected the brightness change.");
        }
    }
    w_release_brightness(job);
    w_next_brightness();
    if (closing && !brightness_thread) DestroyWindow(panel);
}

static void w_release_scan(w_display_scan *s) {
    if (s && !InterlockedDecrement(&s->refs)) {
        for (int i = 0; i < s->display_count; ++i)
            if (s->displays[i].physical) DestroyPhysicalMonitor(s->displays[i].physical);
        free(s);
    }
}

static DWORD WINAPI w_scan_displays(void *arg) {
    w_display_scan *s = arg;
    if (s->full) {
        EnumDisplayMonitors(NULL,NULL,w_add_display,(LPARAM)s);
        for (int i = 0; i < s->display_count; ++i)
            if (!wcscmp(s->previous,s->displays[i].label)) { s->preferred = i; break; }
        if (s->display_count) wcscpy(s->device,s->displays[s->preferred].device);
    }
    s->current.dmSize = sizeof(s->current);
    if (*s->device && EnumDisplaySettingsW(s->device,ENUM_CURRENT_SETTINGS,&s->current)) {
        DEVMODEW mode = {.dmSize = sizeof(mode)};
        for (DWORD i = 0; s->mode_count < (int)count_of(s->modes) && EnumDisplaySettingsW(s->device,i,&mode); ++i)
            if (mode.dmBitsPerPel == s->current.dmBitsPerPel && mode.dmDisplayOrientation == s->current.dmDisplayOrientation)
                s->modes[s->mode_count++] = mode;
    }
    w_release_scan(s);
    return 0;
}

static void w_request_scan(bool full) {
    topology_queued |= full || (scan && scan->full);
    scan_queued = true;
    adjustable = false;
    EnableWindow(preview_button,FALSE);
    EnableWindow(slider,FALSE);
    if (scan_thread) return;
    scan_queued = false;
    int old = w_selected_display();
    scan = calloc(1,sizeof(*scan));
    if (scan) {
        scan->refs = 2;
        scan->full = topology_queued;
        scan->active = MonitorFromWindow(panel,MONITOR_DEFAULTTOPRIMARY);
        if (old >= 0 && old < display_count) {
            wcscpy(scan->previous,displays[old].label);
            if (!scan->full) wcscpy(scan->device,displays[old].device);
        }
        scan_thread = CreateThread(NULL,0,w_scan_displays,scan,0,NULL);
    }
    if (scan_thread) topology_queued = false;
    else {
        free(scan);
        scan = NULL;
        SetWindowTextW(message_line,L"Could not read the display configuration.");
    }
}

static void w_show_modes(const w_display_scan *s) {
    static wchar_t shown_device[CCHDEVICENAME];
    int before = (int)SendMessageW(modes_list,LB_GETCURSEL,0,0);
    bool same_display = !wcscmp(shown_device,s->device);
    bool restore = same_display && before >= 0 && before < mode_count;
    DEVMODEW previous = restore ? modes[before] : (DEVMODEW){0};
    lstrcpynW(shown_device,s->device,count_of(shown_device));
    int selected = -1, current = -1;
    w_tool_rows items = w_rows_begin(modes_list);
    mode_count = 0;
    for (int i = 0; i < s->mode_count; ++i) {
        const DEVMODEW *mode = &s->modes[i];
        bool active = w_same_mode(&s->current,mode);
        wchar_t label[128];
        swprintf(label,count_of(label),L"%lu × %lu   %lu Hz%ls%ls",mode->dmPelsWidth,mode->dmPelsHeight,
                 mode->dmDisplayFrequency,mode->dmDisplayFlags & DM_INTERLACED ? L"   Interlaced" : L"",
                 active ? L"   Current" : L"");
        if (w_rows_put(&items,label,mode_count) < 0) continue;
        if (active) current=mode_count;
        if (restore && w_same_mode(&previous,mode)) selected=mode_count;
        modes[mode_count++] = *mode;
    }
    if (selected < 0) selected = current >= 0 ? current : 0;
    w_rows_end(&items,selected,restore ? items.top : w_rows_show_selection);
    EnableWindow(preview_button,mode_count > 0);
    w_show_brightness();
    read_requested = true;
    w_next_brightness();
}

void w_display_finish(HANDLE completed) {
    if (completed == brightness_thread) { w_finish_brightness(); return; }
    w_display_scan *s = scan;
    CloseHandle(scan_thread);
    scan_thread = NULL;
    scan = NULL;
    if (!scan_queued && !closing && !deadline) {
        if (s->full) {
            bool same_choices = s->display_count == display_count;
            for (int i = 0; same_choices && i < display_count; ++i)
                same_choices = !wcscmp(displays[i].label,s->displays[i].label);
            if (!same_choices && display_choice.open) {
                topology_after_close = true;
                w_release_scan(s);
                return;
            }
            w_clear_displays();
            if (!same_choices) SendMessageW(selector,CB_RESETCONTENT,0,0);
            int preferred = 0;
            for (int i = 0; i < s->display_count; ++i) {
                if (!same_choices && SendMessageW(selector,CB_ADDSTRING,0,(LPARAM)s->displays[i].label) < 0) continue;
                if (i == s->preferred) preferred = display_count;
                displays[display_count++] = s->displays[i];
                s->displays[i].physical = NULL;
            }
            int row = display_count ? preferred : -1;
            if (!same_choices || row != display_choice.row) SendMessageW(selector,CB_SETCURSEL,row,0);
            display_choice.row = row;
        }
        int selected = w_selected_display();
        if (selected >= 0 && selected < display_count && wcscmp(displays[selected].device,s->device))
            scan_queued = true;
        else {
            if (selected < 0 || selected >= display_count) s->mode_count = 0;
            w_show_modes(s);
            if (first_scan) {
                first_scan = false;
                SetWindowTextW(message_line,display_count ? L"Select a resolution and refresh rate to preview." :
                                                           L"No displays available.");
            }
        }
    }
    w_release_scan(s);
    if (scan_queued && !closing && !deadline) w_request_scan(false);
}

static void w_end_preview(bool keep, bool closing) {
    if (!deadline)
        return;
    deadline = 0;
    KillTimer(panel, 1);
    changing_mode = true;
    bool restored = keep || ChangeDisplaySettingsExW(preview_device, &original, NULL, 0, NULL) == DISP_CHANGE_SUCCESSFUL;
    changing_mode = false;
    EnableWindow(selector, TRUE);
    EnableWindow(modes_list, TRUE);
    w_icon_button(preview_button,L"\uE7F4",L"Preview selected display mode");
    if (!closing) w_request_scan(true);
    SetWindowTextW(message_line, keep ? L"Display mode kept for this session." :
                   restored ? L"Original display mode restored." : L"Windows could not restore the previous display mode.");
}

void w_display_preview(void) {
    int index = w_selected_display(), row = (int)SendMessageW(modes_list, LB_GETCURSEL, 0, 0);
    if (deadline) {
        w_end_preview(GetTickCount64() < deadline && index >= 0 && index < display_count && row == preview_row &&
                      !wcscmp(displays[index].device, preview_device), false);
        return;
    }
    if (scan_thread || !IsWindowEnabled(preview_button) || row < 0 || row >= mode_count || index < 0 || index >= display_count)
        return;
    wcscpy(preview_device, displays[index].device);
    original = (DEVMODEW){.dmSize = sizeof(original)};
    if (!EnumDisplaySettingsW(preview_device, ENUM_CURRENT_SETTINGS, &original)) {
        SetWindowTextW(message_line, L"The display is no longer available.");
        return;
    }
    DEVMODEW *mode = &modes[row];
    if (w_same_mode(&original, mode)) {
        SetWindowTextW(message_line, L"This display mode is already active.");
        return;
    }
    // A resolution preview must preserve monitor position and desktop orientation.
    DEVMODEW requested = *mode;
    requested.dmFields &= DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_DISPLAYFLAGS;
    original.dmFields &= requested.dmFields;
    changing_mode = true;
    bool applied = ChangeDisplaySettingsExW(preview_device, &requested, NULL, CDS_TEST, NULL) == DISP_CHANGE_SUCCESSFUL &&
                   ChangeDisplaySettingsExW(preview_device, &requested, NULL, 0, NULL) == DISP_CHANGE_SUCCESSFUL;
    changing_mode = false;
    if (!applied) {
        SetWindowTextW(message_line, L"Display rejected the requested mode.");
        return;
    }
    preview_row = row;
    deadline = GetTickCount64() + 12000;
    EnableWindow(selector, FALSE);
    EnableWindow(modes_list, FALSE);
    w_icon_button(preview_button,L"\uE8FB",L"Keep this display mode");
    SetFocus(preview_button);
    SetWindowTextW(message_line, L"Keep this mode within 12 seconds or press Escape to restore.");
    if (!SetTimer(panel, 1, 12000, NULL))
        w_end_preview(false, false);
}

void w_display_create(HWND window, HWND list, HWND status, HWND apply) {
    panel = window;
    modes_list = list;
    message_line = status;
    preview_button = apply;
    HINSTANCE instance = GetModuleHandleW(NULL);
    w_icon_button(preview_button,L"\uE7F4",L"Preview selected display mode");
    selector = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | w_tool_dropdown_list | WS_VSCROLL,
                           12, 10, 520, 200, window, NULL, instance, NULL);
    SendMessageW(selector, CB_SETMINVISIBLE, 8, 0);
    level = CreateWindowW(L"STATIC", L"Brightness", WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE | SS_ENDELLIPSIS, 162, 306, 158, 26, window, NULL, instance, NULL);
    slider = CreateWindowW(TRACKBAR_CLASSW, L"Brightness", WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_NOTICKS,
                         320, 300, 212, 32, window, NULL, instance, NULL);
    w_percent_slider_init(slider,slider_preview);
    EnableWindow(preview_button,FALSE);
    EnableWindow(slider,FALSE);
    SetWindowTextW(status,L"Reading displays…");
}

void w_display_start(void) {
    first_scan = true;
    w_request_scan(true);
}

void w_display_layout(int pad, int width, int footer, int side, int gap,bool update_font) {
    static int ink;
    if (!ink || update_font) {
        HFONT font=(HFONT)SendMessageW(level,WM_GETFONT,0,0);
        const wchar_t *const captions[]={L"Brightness   100%",L"Brightness unavailable"};
        ink=w_tool_widest_option(font,captions,count_of(captions));
    }
    int caption=min(ink,max(1,(width-side-2*gap)/2));
    w_tool_place_combo(selector,pad,pad,width,side,6);
    w_tool_place(level,pad+side+gap,footer,caption,side);
    w_tool_place(slider,pad+side+caption+2*gap,footer,width-side-caption-2*gap,side);
}

bool w_display_slider(HWND control) { return control == slider; }

bool w_display_key(HWND focus,WPARAM key) {
    if (key == VK_ESCAPE && deadline) {
        w_end_preview(false,false);
        SetFocus(modes_list);
        return true;
    }
    if (key == VK_RETURN && focus == selector && !SendMessageW(selector,CB_GETDROPPEDSTATE,0,0)) {
        SetFocus(modes_list);
        return true;
    }
    return false;
}

bool w_display_message(UINT msg, WPARAM wp, LPARAM lp) {
    if (!panel)
        return false;
    if (msg == WM_COMMAND && selector && (HWND)lp == selector) {
        bool changed = w_tool_choice_changed(&display_choice,selector,HIWORD(wp));
        if (HIWORD(wp) == CBN_CLOSEUP && topology_after_close) {
            topology_after_close = false;
            if (!deadline) w_request_scan(true);
        } else if (changed && !deadline) w_request_scan(false);
    } else if (msg == WM_HSCROLL && (HWND)lp == slider && adjustable) {
        int percent = (int)SendMessageW(slider, TBM_GETPOS, 0, 0), i = w_selected_display();
        if (i < 0 || i >= display_count)
            return true;
        if (LOWORD(wp) == TB_THUMBTRACK)
            w_brightness_label(percent);
        else {
            DWORD value = minimum+(DWORD)(((uint64_t)(maximum - minimum)*percent + 50)/100);
            if (value != brightness || displays[i].write_pending ||
                (brightness_job && brightness_job->topology == topology && brightness_job->display == i)) {
                displays[i].requested = value;
                displays[i].write_pending = true;
                w_brightness_label(percent);
                w_next_brightness();
            } else w_show_brightness();
        }
    } else if (msg == WM_TIMER && wp == 1) {
        if (deadline && GetTickCount64() >= deadline)
            w_end_preview(false, false);
    } else if (msg == WM_DISPLAYCHANGE) {
        if (!deadline && !changing_mode && !refresh_posted)
            refresh_posted = PostMessageW(panel, refresh_display, 0, 0) != FALSE;
    } else if (msg == refresh_display) {
        refresh_posted = false;
        if (!deadline && !closing) w_request_scan(true);
    } else if (msg == WM_CLOSE) {
        bool writing = brightness_job && brightness_job->write;
        for (int i = 0; i < display_count; ++i) writing |= displays[i].write_pending;
        if (!writing) return false;
        w_end_preview(false,true);
        read_requested = false;
        closing = true;
        ShowWindow(panel,SW_HIDE);
    } else if (msg == WM_DESTROY) {
        w_end_preview(false, true);
        w_clear_displays();
        if (scan_thread) CloseHandle(scan_thread);
        scan_thread = NULL;
        w_release_scan(scan);
        scan = NULL;
        if (brightness_thread) CloseHandle(brightness_thread);
        brightness_thread = NULL;
        w_release_brightness(brightness_job);
        brightness_job = NULL;
        panel = NULL;
        return false;
    } else return false;
    return true;
}
