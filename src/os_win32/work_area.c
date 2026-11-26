#include "os_win32/work_area.h"
#include <stdio.h>

// The recovery monitor keeps this mapping alive if the desktop process exits
// between publishing a work area and restoring it. Only the desktop writes
// while it is alive. The monitor restores after its process handle signals.
typedef struct w_work_area {
    wchar_t device[CCHDEVICENAME];
    RECT screen, original, available, previous, requested;
    LONG changed;
} w_work_area;

static w_work_area *areas;
static HANDLE mapping;
static LONG announcing, dirty;

bool w_work_areas_open(DWORD session_pid, bool recovery) {
    // Private and secure desktops share monitor geometry with the interactive
    // desktop. They must never write its system parameters.
    if (w_os_shell_desktop() != ERROR_SUCCESS) return false;
    wchar_t name[80];
    swprintf(name, count_of(name), L"Local\\winwm.workareas.%lu", session_pid);
    mapping = recovery ? CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                                            output_limit * sizeof(*areas), name)
                       : OpenFileMappingW(FILE_MAP_WRITE, FALSE, name);
    if (mapping) areas = MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, output_limit * sizeof(*areas));
    if (!areas && mapping) { CloseHandle(mapping); mapping = NULL; }
    return areas != NULL;
}

typedef struct w_docks { RECT screen, work, available; } w_docks;

// Explorer can leave rcWork unchanged after exit. Preserve a reservation
// backed by a visible toolbar on the edge, including stacked toolbars.
static BOOL CALLBACK w_other_dock(HWND w, LPARAM arg) {
    w_docks *d = (w_docks *)arg;
    if (!IsWindowVisible(w) || IsIconic(w) || w_os_same_process(w) || GetPropW(w, L"winwm.managed")) return TRUE;
    RECT r;
    if (!GetWindowRect(w, &r) || IsRectEmpty(&r) || !IntersectRect(&r, &r, &d->screen)) return TRUE;
    LONG width = r.right-r.left, height = r.bottom-r.top;
    if (width == height || w_os_cloaked(w)) return TRUE;
    wchar_t cls[80];
    if (!GetClassNameW(w, cls, count_of(cls)) || !wcscmp(cls,L"Shell_TrayWnd") ||
        !wcscmp(cls,L"Shell_SecondaryTrayWnd") || !wcscmp(cls,L"tooltips_class32") || !wcscmp(cls,L"#32768"))
        return TRUE;
    if (width > height) {
        if (r.bottom <= d->work.top) d->available.top = max(d->available.top,r.bottom);
        if (r.top >= d->work.bottom) d->available.bottom = min(d->available.bottom,r.top);
    } else {
        if (r.right <= d->work.left) d->available.left = max(d->available.left,r.right);
        if (r.left >= d->work.right) d->available.right = min(d->available.right,r.left);
    }
    return TRUE;
}

RECT w_work_area_available(int output, const MONITORINFOEXW *info) {
    if (!areas || (unsigned)output >= output_limit) return info->rcMonitor;
    w_work_area *a = &areas[output];
    bool same = !wcscmp(a->device,info->szDevice) && EqualRect(&a->screen,&info->rcMonitor);
    if (same && EqualRect(&a->requested,&info->rcWork)) return a->available;
    if (!same) {
        *a = (w_work_area){.screen=info->rcMonitor, .original=info->rcWork, .requested=info->rcWork};
        wcscpy(a->device,info->szDevice);
    } else {
        // Keep independent work-area changes when returning to Explorer.
        if (a->requested.left != info->rcWork.left) a->original.left = info->rcWork.left;
        if (a->requested.top != info->rcWork.top) a->original.top = info->rcWork.top;
        if (a->requested.right != info->rcWork.right) a->original.right = info->rcWork.right;
        if (a->requested.bottom != info->rcWork.bottom) a->original.bottom = info->rcWork.bottom;
    }
    w_docks docks = {info->rcMonitor, info->rcWork, info->rcMonitor};
    if (!EqualRect(&docks.screen,&docks.work)) EnumWindows(w_other_dock,(LPARAM)&docks);
    a->available = docks.available;
    return a->available;
}

static DWORD WINAPI w_announce_work(void *unused) {
    (void)unused;
    do {
        InterlockedExchange(&dirty,0);
        SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, SPI_SETWORKAREA, 0,
                            SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 150, NULL);
        InterlockedExchange(&announcing,0);
    } while (InterlockedCompareExchange(&dirty,0,0) && !InterlockedCompareExchange(&announcing,1,0));
    return 0;
}

static bool w_set_work(RECT work) {
    if (IsRectEmpty(&work) || !SystemParametersInfoW(SPI_SETWORKAREA,0,&work,0)) return false;
    // USER rejects PostMessage(WM_SETTINGCHANGE), even with a null lParam.
    // Notify on a worker so a hung recipient cannot hold up the input thread.
    InterlockedExchange(&dirty,1);
    if (!InterlockedCompareExchange(&announcing,1,0) &&
        !QueueUserWorkItem(w_announce_work,NULL,WT_EXECUTELONGFUNCTION)) InterlockedExchange(&announcing,0);
    return true;
}

void w_work_area_publish(int output, HMONITOR monitor, RECT work) {
    if (!areas || (unsigned)output >= output_limit) return;
    w_work_area *a = &areas[output];
    MONITORINFO info = {.cbSize=sizeof(info)};
    if (!*a->device || !GetMonitorInfoW(monitor,&info) || !EqualRect(&info.rcMonitor,&a->screen) ||
        IsRectEmpty(&work) || work.left<a->screen.left || work.top<a->screen.top ||
        work.right>a->screen.right || work.bottom>a->screen.bottom) return;
    if (EqualRect(&info.rcWork,&work)) { a->requested = work; return; }
    a->previous = info.rcWork;
    a->requested = work;
    InterlockedExchange(&a->changed, 1);
    if (!w_set_work(work)) a->requested = a->previous;
}

static BOOL CALLBACK w_restore_work(HMONITOR monitor, HDC dc, LPRECT rect, LPARAM context) {
    (void)dc; (void)rect; (void)context;
    MONITORINFOEXW m = {.cbSize=sizeof(m)};
    if (!GetMonitorInfoW(monitor,(MONITORINFO *)&m)) return TRUE;
    for (int i=0; i<output_limit; ++i) {
        w_work_area *a = &areas[i];
        if (!a->changed || wcscmp(a->device,m.szDevice) || !EqualRect(&a->screen,&m.rcMonitor)) continue;
        RECT r = m.rcWork;
        // Merge per edge, so an independent dock added just before shutdown
        // keeps its reservation. previous covers a crash during the SPI call.
        if (r.left==a->requested.left || r.left==a->previous.left) r.left=a->original.left;
        if (r.top==a->requested.top || r.top==a->previous.top) r.top=a->original.top;
        if (r.right==a->requested.right || r.right==a->previous.right) r.right=a->original.right;
        if (r.bottom==a->requested.bottom || r.bottom==a->previous.bottom) r.bottom=a->original.bottom;
        if (EqualRect(&r,&m.rcWork) || w_set_work(r)) InterlockedExchange(&a->changed,0);
        break;
    }
    return TRUE;
}

void w_work_areas_close(void) {
    if (areas) {
        EnumDisplayMonitors(NULL,NULL,w_restore_work,0);
        UnmapViewOfFile(areas);
        areas = NULL;
    }
    if (mapping) { CloseHandle(mapping); mapping = NULL; }
}
