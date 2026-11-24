#include "os_win32/os_win32.h"
#include <wchar.h>

////////////////
//~ Work Area

typedef struct w_area_record {
    wchar_t device[CCHDEVICENAME];
    RECT screen, original, desired;
    LONG active;
} w_area_record;

typedef struct w_area_table {
    w_area_record records[output_limit];
} w_area_table;

static HANDLE mapping;
static w_area_table *table;
static const wchar_t mapping_name[] = L"Local\\winwm.work_areas.v1";

////////////////
//~ Work Area Functions

void w_os_area_open(bool host) {
    mapping = host ? CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(w_area_table),
                                        mapping_name)
                   : OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, mapping_name);
    if (mapping)
        table = MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(w_area_table));
}

void w_os_area_apply(HMONITOR mon, w_rect usable) {
    if (!table)
        return;
    MONITORINFOEXW info = {.cbSize = sizeof(info)};
    if (!GetMonitorInfoW(mon, (MONITORINFO *)&info))
        return;
    RECT desired = w_os_to_rect(usable);
    if (EqualRect(&desired, &info.rcWork))
        return;
    w_area_record *slot = NULL;
    for (int i = 0; i < output_limit; i++) {
        w_area_record *r = table->records + i;
        if (r->active && !wcscmp(r->device, info.szDevice) && EqualRect(&r->screen, &info.rcMonitor)) {
            slot = r;
            break;
        }
        if (!slot && !r->active)
            slot = r;
    }
    if (!slot)
        return;
    if (slot->active && !EqualRect(&slot->desired, &info.rcWork))
        return;
    if (!slot->active) {
        // Another shell may own a work area during a preview.
        if (!EqualRect(&info.rcWork, &info.rcMonitor))
            return;
        wcscpy(slot->device, info.szDevice);
        slot->screen = info.rcMonitor;
        slot->original = info.rcWork;
    }
    RECT previous = slot->desired;
    bool active = slot->active != 0;
    slot->desired = desired;
    InterlockedExchange(&slot->active, 1);
    if (!SystemParametersInfoW(SPI_SETWORKAREA, 0, &desired, 0)) {
        slot->desired = previous;
        InterlockedExchange(&slot->active, active);
    }
}

static BOOL CALLBACK w_restore_area(HMONITOR mon, HDC dc, LPRECT bounds, LPARAM unused) {
    (void)dc;
    (void)bounds;
    (void)unused;
    MONITORINFOEXW info = {.cbSize = sizeof(info)};
    if (!GetMonitorInfoW(mon, (MONITORINFO *)&info))
        return TRUE;
    for (int i = 0; i < output_limit; i++) {
        w_area_record *r = table->records + i;
        if (r->active && !wcscmp(r->device, info.szDevice) && EqualRect(&r->screen, &info.rcMonitor) &&
            EqualRect(&r->desired, &info.rcWork))
            SystemParametersInfoW(SPI_SETWORKAREA, 0, &r->original, 0);
    }
    return TRUE;
}

void w_os_area_restore(void) {
    if (table) {
        EnumDisplayMonitors(NULL, NULL, w_restore_area, 0);
        memset(table, 0, sizeof(*table));
    }
}

void w_os_area_close(void) {
    if (table)
        UnmapViewOfFile(table);
    if (mapping)
        CloseHandle(mapping);
    table = NULL;
    mapping = NULL;
}
