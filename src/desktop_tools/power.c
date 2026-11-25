#include "desktop_tools/power.h"
#include "theme/icon_button.h"
#include <powrprof.h>
#include <errno.h>
#include <stdio.h>
#include <wctype.h>

// These exports are absent from older MinGW headers.
DWORD WINAPI PowerWriteACValueIndex(HKEY, const GUID *, const GUID *, const GUID *, DWORD);
DWORD WINAPI PowerWriteDCValueIndex(HKEY, const GUID *, const GUID *, const GUID *, DWORD);

static HWND plans_box, activate, timers, duration, message, save;
typedef struct {
    GUID id;
    wchar_t name[256];
} w_power_plan;
typedef struct {
    DWORD seconds;
    bool available;
} w_timer_value;
typedef struct {
    LONG refs;
    w_power_plan *plans;
    size_t count;
    GUID active;
    size_t initial;
    w_timer_value values[4];
} w_power_scan;
static w_power_plan *plans;
static size_t plan_count;
static HANDLE scan_thread;
static w_power_scan *scan;
static DWORD original;
static bool editing, readable;
static const GUID *const groups[] = {&GUID_VIDEO_SUBGROUP, &GUID_SLEEP_SUBGROUP};
static const GUID *const settings[] = {&GUID_VIDEO_POWERDOWN_TIMEOUT, &GUID_STANDBY_TIMEOUT};
static const wchar_t *const names[] = {L"Turn off display", L"Put computer to sleep"};

static GUID *w_plan(void) {
    LRESULT i = SendMessageW(plans_box, CB_GETCURSEL, 0, 0);
    return i >= 0 && (size_t)i < plan_count ? &plans[i].id : NULL;
}

static int w_timer_id(void) {
    return (int)SendMessageW(timers, LB_GETCURSEL, 0, 0);
}

static DWORD w_read(const GUID *plan, int id, DWORD *seconds) {
    if (!plan || id < 0 || id >= 4) return ERROR_INVALID_PARAMETER;
    return (id < 2 ? PowerReadACValueIndex : PowerReadDCValueIndex)(
        NULL, plan, groups[id % 2], settings[id % 2], seconds);
}

static void w_interval(DWORD seconds, wchar_t *s, size_t cap) {
    if (!seconds) wcscpy(s, L"Never");
    else if (seconds % 60) swprintf(s, cap, L"%lu sec", seconds);
    else swprintf(s, cap, L"%lu min", seconds / 60);
}

static bool w_seconds(const wchar_t *s, DWORD *out) {
    while (iswspace(*s)) ++s;
    if (!_wcsicmp(s, L"Never")) { *out = 0; return true; }
    if (*s < L'0' || *s > L'9') return false;
    wchar_t *end;
    errno = 0;
    unsigned long minutes = wcstoul(s, &end, 10);
    if (errno == ERANGE) return false;
    while (iswspace(*end)) ++end;
    if (!_wcsicmp(end, L"sec")) { *out = minutes; return true; }
    if ((*end && _wcsicmp(end, L"min")) || minutes > MAXDWORD / 60) return false;
    *out = minutes * 60;
    return true;
}

static bool w_active(const GUID *plan) {
    GUID *active = NULL;
    bool same = plan && PowerGetActiveScheme(NULL, &active) == ERROR_SUCCESS && IsEqualGUID(plan, active);
    LocalFree(active);
    return same;
}

static void w_show_timer(w_timer_value value, bool active) {
    GUID *plan = w_plan();
    readable = value.available;
    original = value.seconds;
    wchar_t text[40] = {0};
    if (readable) w_interval(original, text, count_of(text));
    editing = true;
    SetWindowTextW(duration, text);
    editing = false;
    EnableWindow(duration, readable);
    EnableWindow(save, FALSE);
    EnableWindow(activate, plan && !active);
    SetWindowTextW(activate, active ? L"Active plan" : L"Use plan");
    SetWindowTextW(message, !plan ? L"No power plans available." :
                   readable ? L"Choose a timeout or enter a duration in minutes." :
                              L"This timer is unavailable on this system.");
}

static w_timer_value w_timer_value_for(GUID *plan, int id) {
    w_timer_value value = {0};
    value.available = w_read(plan,id,&value.seconds) == ERROR_SUCCESS;
    return value;
}

void w_power_selected(void) {
    if (scan_thread) return;
    GUID *plan = w_plan();
    w_show_timer(w_timer_value_for(plan,w_timer_id()),w_active(plan));
}

static void w_show_timers(int selected, const w_timer_value *values, bool active) {
    GUID *plan = w_plan();
    SendMessageW(timers, LB_RESETCONTENT, 0, 0);
    for (int i = 0; plan && i < 4; ++i) {
        wchar_t value[40] = L"Unavailable", text[160];
        if (values[i].available) w_interval(values[i].seconds,value,count_of(value));
        swprintf(text, count_of(text), L"%ls    ·    %ls    ·    %ls",
                 i < 2 ? L"Plugged in" : L"On battery", names[i%2], value);
        SendMessageW(timers, LB_ADDSTRING, 0, (LPARAM)text);
    }
    SendMessageW(timers, LB_SETCURSEL, selected, 0);
    int id = w_timer_id();
    w_show_timer(id >= 0 && id < 4 ? values[id] : (w_timer_value){0},active);
}

static void w_load_timers(int selected) {
    GUID *plan = w_plan();
    w_timer_value values[4];
    for (int i = 0; i < 4; ++i) values[i] = w_timer_value_for(plan,i);
    w_show_timers(selected,values,w_active(plan));
}

void w_power_save(void) {
    int id = w_timer_id();
    GUID *plan = w_plan();
    wchar_t text[48];
    GetWindowTextW(duration, text, count_of(text));
    DWORD next, now;
    if (!readable || !w_seconds(text, &next)) {
        SetWindowTextW(message, L"Enter minutes, seconds with “sec”, or Never.");
        return;
    }
    if (w_read(plan, id, &now) != ERROR_SUCCESS || now != original) {
        w_load_timers(id);
        SetWindowTextW(message, L"The timer changed elsewhere. Review its current value.");
        return;
    }
    if (next == original) return;
    DWORD error = (id < 2 ? PowerWriteACValueIndex : PowerWriteDCValueIndex)(
        NULL, plan, groups[id % 2], settings[id % 2], next);
    bool pending = !error && w_active(plan) && PowerSetActiveScheme(NULL, plan) != ERROR_SUCCESS;
    w_load_timers(id);
    SetWindowTextW(message, error == ERROR_ACCESS_DENIED ? L"Windows policy denied this timer change." :
                   error ? L"Windows could not save this timer." :
                   pending ? L"Timer saved. Reactivate the plan to apply it." : L"Timer saved.");
}

static void w_release_scan(w_power_scan *s) {
    if (s && !InterlockedDecrement(&s->refs)) {
        free(s->plans);
        free(s);
    }
}

bool w_power_message(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DESTROY) {
        if (scan_thread) CloseHandle(scan_thread);
        scan_thread = NULL;
        w_release_scan(scan);
        scan = NULL;
        free(plans);
        return false;
    }
    if (msg != WM_COMMAND) return false;
    HWND from = (HWND)lp;
    if (from == plans_box && HIWORD(wp) == CBN_SELCHANGE) {
        w_load_timers(0);
    } else if (from == activate && HIWORD(wp) == BN_CLICKED) {
        GUID *plan = w_plan();
        DWORD error = plan ? PowerSetActiveScheme(NULL, plan) : ERROR_NOT_FOUND;
        w_load_timers(w_timer_id());
        SetWindowTextW(message, error ? L"Windows could not activate this plan." : L"Power plan active.");
    } else if (from == duration && !editing && (HIWORD(wp) == CBN_EDITCHANGE || HIWORD(wp) == CBN_SELCHANGE)) {
        wchar_t text[48];
        if (HIWORD(wp) == CBN_SELCHANGE) {
            LRESULT row = SendMessageW(duration, CB_GETCURSEL, 0, 0);
            if (row < 0 || SendMessageW(duration, CB_GETLBTEXT, row, (LPARAM)text) < 0) return true;
        } else GetWindowTextW(duration, text, count_of(text));
        DWORD value;
        EnableWindow(save, readable && w_seconds(text, &value) && value != original);
    } else return false;
    return true;
}

void w_power_create(HWND panel, HWND list, HWND status, HWND apply) {
    timers = list; message = status; save = apply;
    HINSTANCE inst = GetModuleHandleW(NULL);
    DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP;
    MoveWindow(timers, 12, 52, 520, 196, FALSE);
    MoveWindow(save, 210, 300, 30, 30, FALSE);
    w_icon_button(save,L"\uE74E",L"Save selected power timer");
    plans_box = CreateWindowW(L"COMBOBOX", L"Power plan", style | CBS_DROPDOWNLIST | WS_VSCROLL,
                              12, 12, 360, 240, panel, NULL, inst, NULL);
    activate = CreateWindowW(L"BUTTON", L"Use plan", style | BS_OWNERDRAW,
                             383, 10, 149, 32, panel, NULL, inst, NULL);
    duration = CreateWindowW(L"COMBOBOX", L"Timeout", style | CBS_DROPDOWN | WS_VSCROLL,
                             12, 302, 186, 200, panel, NULL, inst, NULL);
    SendMessageW(duration, CB_LIMITTEXT, 38, 0);
    const DWORD minutes[] = {0, 1, 2, 5, 10, 15, 30, 60, 120};
    for (size_t i = 0; i < count_of(minutes); ++i) {
        wchar_t val[40]; w_interval(minutes[i]*60, val, count_of(val));
        SendMessageW(duration, CB_ADDSTRING, 0, (LPARAM)val);
    }
    EnableWindow(activate,FALSE);
    EnableWindow(duration,FALSE);
    EnableWindow(save,FALSE);
    SetWindowTextW(message,L"Reading power plans…");
}

static DWORD WINAPI w_scan_plans(void *arg) {
    w_power_scan *s = arg;
    GUID *active = NULL;
    if (PowerGetActiveScheme(NULL,&active) == ERROR_SUCCESS && active) s->active = *active;
    LocalFree(active);
    for (DWORD i = 0; ; ++i) {
        GUID id;
        DWORD bytes = sizeof(id);
        if (PowerEnumerate(NULL,NULL,NULL,ACCESS_SCHEME,i,(BYTE *)&id,&bytes) != ERROR_SUCCESS) break;
        wchar_t name[256]; bytes = sizeof(name);
        if (PowerReadFriendlyName(NULL,&id,NULL,NULL,(BYTE *)name,&bytes) != ERROR_SUCCESS) continue;
        w_power_plan *next = realloc(s->plans,(s->count+1)*sizeof(*next));
        if (!next) break;
        s->plans = next;
        if (IsEqualGUID(&s->active,&id)) s->initial = s->count;
        next[s->count].id = id;
        lstrcpynW(next[s->count++].name,name,count_of(next[0].name));
    }
    if (s->count)
        for (int i=0; i<4; ++i) s->values[i] = w_timer_value_for(&s->plans[s->initial].id,i);
    w_release_scan(s);
    return 0;
}

void w_power_start(void) {
    scan = calloc(1,sizeof(*scan));
    if (scan) {
        scan->refs = 2;
        scan_thread = CreateThread(NULL,0,w_scan_plans,scan,0,NULL);
    }
    if (!scan_thread) {
        free(scan);
        scan = NULL;
        SetWindowTextW(message,L"Could not read power plans.");
    }
}

HANDLE w_power_pending(void) { return scan_thread; }

void w_power_finish(void) {
    CloseHandle(scan_thread);
    scan_thread = NULL;
    plans = scan->plans;
    scan->plans = NULL;
    GUID initial = scan->count ? plans[scan->initial].id : (GUID){0};
    int selected = 0;
    for (size_t i = 0; i < scan->count; ++i) {
        if (SendMessageW(plans_box,CB_ADDSTRING,0,(LPARAM)plans[i].name) < 0) continue;
        if (IsEqualGUID(&scan->active,&plans[i].id)) selected = (int)plan_count;
        if (plan_count != i) plans[plan_count] = plans[i];
        ++plan_count;
    }
    SendMessageW(plans_box,CB_SETCURSEL,selected,0);
    GUID *shown = w_plan();
    if (!shown || IsEqualGUID(shown,&initial))
        w_show_timers(0,scan->values,shown && IsEqualGUID(shown,&scan->active));
    else w_load_timers(0);
    w_release_scan(scan);
    scan = NULL;
}
