#include "desktop_tools/display.h"
#include <commctrl.h>
#include <physicalmonitorenumerationapi.h>
#include <highlevelmonitorconfigurationapi.h>
#include <stdlib.h>
#include <wchar.h>

typedef struct w_display {
    wchar_t device[CCHDEVICENAME];
    HANDLE physical;
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
enum { refresh_display = WM_APP + 60 };

static int w_selected_display(void) {
    return (int)SendMessageW(selector, CB_GETCURSEL, 0, 0);
}

static bool w_same_mode(const DEVMODEW *a, const DEVMODEW *b) {
    return a->dmPelsWidth == b->dmPelsWidth && a->dmPelsHeight == b->dmPelsHeight &&
           a->dmDisplayFrequency == b->dmDisplayFrequency && a->dmDisplayFlags == b->dmDisplayFlags &&
           a->dmBitsPerPel == b->dmBitsPerPel && a->dmDisplayOrientation == b->dmDisplayOrientation;
}

static void w_clear_displays(void) {
    for (int i = 0; i < display_count; ++i)
        if (displays[i].physical)
            DestroyPhysicalMonitor(displays[i].physical);
    display_count = 0;
    SendMessageW(selector, CB_RESETCONTENT, 0, 0);
}

static BOOL CALLBACK w_add_display(HMONITOR monitor, HDC dc, LPRECT rect, LPARAM active) {
    (void)dc;
    (void)rect;
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
        if (display_count >= (int)count_of(displays) || SendMessageW(selector, CB_ADDSTRING, 0, (LPARAM)label) < 0) {
            if (handle)
                DestroyPhysicalMonitor(handle);
            continue;
        }
        w_display *d = &displays[display_count++];
        wcscpy(d->device, info.szDevice);
        d->physical = handle;
        if (monitor == (HMONITOR)active && !i)
            SendMessageW(selector, CB_SETCURSEL, display_count - 1, 0);
    }
    free(physical);
    return TRUE;
}

static void w_brightness_label(int value) {
    wchar_t s[48];
    if (value >= 0)
        swprintf(s, count_of(s), L"Brightness   %d%%", value);
    SetWindowTextW(level, value >= 0 ? s : L"Brightness unavailable");
}

static void w_show_brightness(void) {
    int value = adjustable ? (int)(((uint64_t)(brightness - minimum)*100 + (maximum - minimum)/2)/(maximum - minimum)) : 0;
    SendMessageW(slider, TBM_SETPOS, TRUE, value);
    EnableWindow(slider, adjustable);
    w_brightness_label(adjustable ? value : -1);
}

static void w_read_brightness(HANDLE h) {
    DWORD caps, temperatures;
    adjustable = h && GetMonitorCapabilities(h, &caps, &temperatures) && (caps & MC_CAPS_BRIGHTNESS) &&
                 GetMonitorBrightness(h, &minimum, &brightness, &maximum) && maximum > minimum &&
                 brightness >= minimum && brightness <= maximum;
    w_show_brightness();
}

static void w_display_modes(void) {
    int display = w_selected_display(), selected = -1;
    mode_count = 0;
    SendMessageW(modes_list, LB_RESETCONTENT, 0, 0);
    DEVMODEW current = {.dmSize = sizeof(current)};
    if (display >= 0 && display < display_count &&
        EnumDisplaySettingsW(displays[display].device, ENUM_CURRENT_SETTINGS, &current)) {
        DEVMODEW mode = {.dmSize = sizeof(mode)};
        for (DWORD j = 0; mode_count < (int)count_of(modes) && EnumDisplaySettingsW(displays[display].device, j, &mode); ++j) {
            if (mode.dmBitsPerPel != current.dmBitsPerPel || mode.dmDisplayOrientation != current.dmDisplayOrientation)
                continue;
            bool active = w_same_mode(&current, &mode);
            wchar_t s[128];
            swprintf(s, count_of(s), L"%lu × %lu   %lu Hz%ls%ls", mode.dmPelsWidth, mode.dmPelsHeight, mode.dmDisplayFrequency,
                     mode.dmDisplayFlags & DM_INTERLACED ? L"   Interlaced" : L"", active ? L"   Current" : L"");
            if (SendMessageW(modes_list, LB_ADDSTRING, 0, (LPARAM)s) < 0)
                continue;
            SendMessageW(modes_list, LB_SETITEMDATA, mode_count, mode_count);
            if (active)
                selected = mode_count;
            modes[mode_count++] = mode;
        }
    }
    SendMessageW(modes_list, LB_SETCURSEL, selected >= 0 ? selected : 0, 0);
    EnableWindow(preview_button, mode_count > 0);
    w_read_brightness(display >= 0 && display < display_count ? displays[display].physical : NULL);
}

static void w_display_refresh(void) {
    int old = w_selected_display();
    wchar_t previous[220] = {0};
    if (old >= 0 && old < display_count)
        SendMessageW(selector, CB_GETLBTEXT, old, (LPARAM)previous);
    w_clear_displays();
    EnumDisplayMonitors(NULL, NULL, w_add_display, (LPARAM)MonitorFromWindow(panel, MONITOR_DEFAULTTOPRIMARY));
    if (*previous) {
        int index = (int)SendMessageW(selector, CB_FINDSTRINGEXACT, -1, (LPARAM)previous);
        if (index >= 0)
            SendMessageW(selector, CB_SETCURSEL, index, 0);
    }
    if (w_selected_display() < 0)
        SendMessageW(selector, CB_SETCURSEL, 0, 0);
    w_display_modes();
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
    SetWindowTextW(preview_button, L"Preview");
    if (!closing) w_display_refresh();
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
    if (row < 0 || row >= mode_count || index < 0 || index >= display_count)
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
    SetWindowTextW(preview_button, L"Keep");
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
    MoveWindow(list, 12, 50, 520, 198, FALSE);
    selector = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                           12, 10, 520, 200, window, NULL, instance, NULL);
    SendMessageW(selector, CB_SETMINVISIBLE, 8, 0);
    level = CreateWindowW(L"STATIC", L"Brightness", WS_CHILD | WS_VISIBLE, 162, 306, 158, 26, window, NULL, instance, NULL);
    slider = CreateWindowW(TRACKBAR_CLASSW, L"Brightness", WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_NOTICKS,
                         320, 300, 212, 32, window, NULL, instance, NULL);
    SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessageW(slider, TBM_SETPAGESIZE, 0, 5);
    w_display_refresh();
    SetWindowTextW(status, L"Select a resolution and refresh rate to preview.");
}

bool w_display_stretches(HWND control) {
    return control == selector || control == slider;
}

bool w_display_message(UINT msg, WPARAM wp, LPARAM lp) {
    if (!panel)
        return false;
    if (msg == WM_COMMAND && (HWND)lp == selector && HIWORD(wp) == CBN_SELCHANGE) {
        if (!deadline) w_display_modes();
    } else if (msg == WM_HSCROLL && (HWND)lp == slider && adjustable) {
        int percent = (int)SendMessageW(slider, TBM_GETPOS, 0, 0), i = w_selected_display();
        if (i < 0 || i >= display_count)
            return true;
        if (LOWORD(wp) == TB_THUMBTRACK)
            w_brightness_label(percent);
        else {
            DWORD value = minimum+(DWORD)(((uint64_t)(maximum - minimum)*percent + 50)/100);
            if (value != brightness) {
                bool ok = SetMonitorBrightness(displays[i].physical, value) != FALSE;
                w_read_brightness(displays[i].physical);
                if (!ok)
                    SetWindowTextW(message_line, L"The monitor rejected the brightness change.");
            } else w_show_brightness();
        }
    } else if (msg == WM_TIMER && wp == 1) {
        if (deadline && GetTickCount64() >= deadline)
            w_end_preview(false, false);
    } else if (msg == WM_DISPLAYCHANGE) {
        if (!deadline && !changing_mode)
            PostMessageW(panel, refresh_display, 0, 0);
    } else if (msg == refresh_display) {
        if (!deadline) w_display_refresh();
    } else if (msg == WM_DESTROY) {
        w_end_preview(false, true);
        w_clear_displays();
        panel = NULL;
        return false;
    } else return false;
    return true;
}
