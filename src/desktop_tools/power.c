#include "desktop_tools/power.h"
#include "desktop_tools/tools.h"
#include "desktop_tools/rows.h"
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
    bool failed;
    w_power_plan *plans;
    size_t count;
    GUID active, requested;
    size_t initial;
    w_timer_value values[4];
} w_power_scan;
static w_power_plan *plans;
static size_t plan_count;
static HANDLE scan_thread;
static w_power_scan *scan;
static DWORD original;
static bool editing, readable, dirty, scan_stale;
static w_tool_choice plan_choice = {.row=-1};
static w_tool_edit_choice duration_edit = {.choice.row=-1};
static bool refresh_after_close;
enum { power_refresh_timer = 31 };
static const GUID *const groups[] = {&GUID_VIDEO_SUBGROUP, &GUID_SLEEP_SUBGROUP};
static const GUID *const settings[] = {&GUID_VIDEO_POWERDOWN_TIMEOUT, &GUID_STANDBY_TIMEOUT};
static const wchar_t *const names[] = {L"Turn off display", L"Put computer to sleep"};

static GUID *w_plan(void) {
    int i = plan_choice.row;
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

static void w_show_timer(w_timer_value value, bool active, bool passive) {
    GUID *plan = w_plan();
    bool changed = readable != value.available || original != value.seconds;
    readable = value.available;
    wchar_t text[48] = {0};
    DWORD candidate;
    if (passive && dirty) {
        GetWindowTextW(duration,text,count_of(text));
        EnableWindow(save,readable && w_seconds(text,&candidate) && candidate != original);
    } else {
        dirty = false;
        original = value.seconds;
        if (readable) w_interval(original,text,count_of(text));
        editing = true;
        w_tool_status(duration,text);
        editing = false;
        EnableWindow(save,FALSE);
    }
    EnableWindow(duration, readable);
    EnableWindow(activate, plan && !active);
    w_icon_button(activate,L"\uE8FB",active ? L"Selected power plan is active" : L"Activate selected power plan");
    if (!passive || (changed && !dirty)) w_tool_status(message, !plan ? L"No power plans available." :
                   readable ? L"Choose a timeout or enter a duration in minutes." :
                              L"This timer is unavailable on this system.");
}

static w_timer_value w_timer_value_for(GUID *plan, int id) {
    w_timer_value value = {0};
    value.available = w_read(plan,id,&value.seconds) == ERROR_SUCCESS;
    return value;
}

void w_power_selected(void) {
    if (scan_thread && !plan_count) return;
    GUID *plan = w_plan();
    w_show_timer(w_timer_value_for(plan,w_timer_id()),w_active(plan),false);
}

static void w_show_timers(int selected, const w_timer_value *values, bool active, bool passive) {
    GUID *plan = w_plan();
    w_tool_rows items = w_rows_begin(timers);
    for (int i = 0; plan && i < 4; ++i) {
        wchar_t value[40] = L"Unavailable", text[160];
        if (values[i].available) w_interval(values[i].seconds,value,count_of(value));
        swprintf(text, count_of(text), L"%ls    ·    %ls    ·    %ls",
                 i < 2 ? L"Plugged in" : L"On battery", names[i%2], value);
        w_rows_put(&items,text,i);
    }
    w_rows_end(&items,selected,items.top);
    int id = w_timer_id();
    w_show_timer(id >= 0 && id < 4 ? values[id] : (w_timer_value){0},active,passive);
}

static void w_load_timers(int selected, bool preserve_edit) {
    GUID *plan = w_plan();
    w_timer_value values[4];
    for (int i = 0; i < 4; ++i) values[i] = w_timer_value_for(plan,i);
    w_show_timers(selected,values,w_active(plan),preserve_edit);
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
        w_load_timers(id,false);
        SetWindowTextW(message, L"The timer changed elsewhere. Review its current value.");
        return;
    }
    if (next == original) return;
    DWORD error = (id < 2 ? PowerWriteACValueIndex : PowerWriteDCValueIndex)(
        NULL, plan, groups[id % 2], settings[id % 2], next);
    if (!error && scan_thread) scan_stale = true;
    bool pending = !error && w_active(plan) && PowerSetActiveScheme(NULL, plan) != ERROR_SUCCESS;
    w_load_timers(id,error != ERROR_SUCCESS);
    SetWindowTextW(message, error == ERROR_ACCESS_DENIED ? L"Windows policy denied this timer change." :
                   error ? L"Windows could not save this timer." :
                   pending ? L"Timer saved. Reactivate the plan to apply it." : L"Timer saved.");
}

bool w_power_enter(HWND focus) {
    if (focus == timers) {
        if (IsWindowEnabled(duration)) {
            SetFocus(duration);
            SendMessageW(duration,CB_SETEDITSEL,0,MAKELPARAM(0,-1));
        }
    } else if (focus == duration || IsChild(duration,focus)) {
        if (SendMessageW(duration,CB_GETDROPPEDSTATE,0,0)) return false;
        if (dirty && readable) w_power_save();
    } else if (focus == plans_box) {
        if (SendMessageW(plans_box,CB_GETDROPPEDSTATE,0,0)) return false;
        SetFocus(timers);
    } else return false;
    return true;
}

static void w_release_scan(w_power_scan *s) {
    if (s && !InterlockedDecrement(&s->refs)) {
        free(s->plans);
        free(s);
    }
}

bool w_power_message(UINT msg, WPARAM wp, LPARAM lp) {
    if ((msg == WM_TIMER && wp == power_refresh_timer) || msg == WM_POWERBROADCAST) {
        HWND panel = GetParent(plans_box);
        if (panel && IsWindowVisible(panel) && !IsIconic(panel) && IsWindowEnabled(panel)) w_power_start();
        return msg == WM_TIMER;
    }
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
    UINT event = HIWORD(wp);
    if (from == plans_box && plans_box) {
        if (w_tool_choice_changed(&plan_choice,plans_box,event)) w_load_timers(0,false);
    } else if (from == activate && HIWORD(wp) == BN_CLICKED) {
        GUID *plan = w_plan();
        DWORD error = plan ? PowerSetActiveScheme(NULL, plan) : ERROR_NOT_FOUND;
        if (!error && scan_thread) scan_stale = true;
        w_load_timers(w_timer_id(),true);
        SetWindowTextW(message, error ? L"Windows could not activate this plan." : L"Power plan active.");
    } else if (from == duration && duration && !editing) {
        wchar_t text[48];
        editing = true;
        bool chosen = w_tool_edit_choice_changed(&duration_edit,duration,event);
        editing = false;
        if (chosen || (event == CBN_SELCHANGE && !duration_edit.choice.open)) {
            LRESULT row = SendMessageW(duration, CB_GETCURSEL, 0, 0);
            if (row < 0 || SendMessageW(duration, CB_GETLBTEXT, row, (LPARAM)text) < 0) return true;
        } else GetWindowTextW(duration, text, count_of(text));
        if (!duration_edit.choice.open && (chosen || event == CBN_CLOSEUP || event == CBN_EDITCHANGE || event == CBN_SELCHANGE)) {
            DWORD value;
            bool valid = w_seconds(text,&value);
            dirty = !valid || value != original;
            EnableWindow(save,readable && valid && dirty);
        }
    } else return false;
    if (event == CBN_CLOSEUP && refresh_after_close) w_power_start();
    return true;
}

void w_power_create(HWND panel, HWND list, HWND status, HWND apply) {
    timers = list; message = status; save = apply;
    HINSTANCE inst = GetModuleHandleW(NULL);
    DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP;
    w_icon_button(save,L"\uE74E",L"Save selected power timer");
    plans_box = CreateWindowW(L"COMBOBOX", L"Power plan", style | w_tool_dropdown_list | WS_VSCROLL,
                              12, 12, 360, 240, panel, NULL, inst, NULL);
    activate = CreateWindowW(L"BUTTON", L"Use plan", style | BS_OWNERDRAW,
                             383, 10, 149, 32, panel, NULL, inst, NULL);
    duration = CreateWindowW(L"COMBOBOX", L"Timeout", style | w_tool_dropdown_edit | WS_VSCROLL,
                             12, 302, 186, 200, panel, NULL, inst, NULL);
    SendMessageW(duration, CB_LIMITTEXT, 38, 0);
    const DWORD minutes[] = {0, 1, 2, 5, 10, 15, 30, 60, 120};
    for (size_t i = 0; i < count_of(minutes); ++i) {
        wchar_t val[40]; w_interval(minutes[i]*60, val, count_of(val));
        SendMessageW(duration, CB_ADDSTRING, 0, (LPARAM)val);
    }
    w_icon_button(activate,L"\uE8FB",L"Activate selected power plan");
    EnableWindow(activate,FALSE);
    EnableWindow(duration,FALSE);
    EnableWindow(save,FALSE);
    SetTimer(panel,power_refresh_timer,10000,NULL);
    SetWindowTextW(message,L"Reading power plans…");
}

void w_power_layout(int pad, int width, int footer, int side, int gap) {
    w_tool_place_combo(plans_box,pad,pad,width-side-gap,side,6);
    w_tool_place(activate,pad+width-side,pad,side,side);
    w_tool_place_combo(duration,pad,footer,width-side-gap,side,5);
    w_tool_place(save,pad+width-side,footer,side,side);
}

static DWORD WINAPI w_scan_plans(void *arg) {
    w_power_scan *s = arg;
    GUID *active = NULL;
    if (PowerGetActiveScheme(NULL,&active) == ERROR_SUCCESS && active) s->active = *active;
    LocalFree(active);
    for (DWORD i = 0; ; ++i) {
        GUID id;
        DWORD bytes = sizeof(id);
        DWORD result = PowerEnumerate(NULL,NULL,NULL,ACCESS_SCHEME,i,(BYTE *)&id,&bytes);
        if (result != ERROR_SUCCESS) { s->failed = result != ERROR_NO_MORE_ITEMS; break; }
        wchar_t name[256]; bytes = sizeof(name);
        if (PowerReadFriendlyName(NULL,&id,NULL,NULL,(BYTE *)name,&bytes) != ERROR_SUCCESS) continue;
        w_power_plan *next = realloc(s->plans,(s->count+1)*sizeof(*next));
        if (!next) { s->failed = true; break; }
        s->plans = next;
        if (IsEqualGUID(&s->active,&id)) s->initial = s->count;
        next[s->count].id = id;
        lstrcpynW(next[s->count++].name,name,count_of(next[0].name));
    }
    for (size_t i=0; i<s->count; ++i)
        if (IsEqualGUID(&s->requested,&s->plans[i].id)) { s->initial = i; break; }
    if (s->count)
        for (int i=0; i<4; ++i) s->values[i] = w_timer_value_for(&s->plans[s->initial].id,i);
    w_release_scan(s);
    return 0;
}

void w_power_start(void) {
    if (plan_choice.open || duration_edit.choice.open) {
        refresh_after_close = true;
        return;
    }
    refresh_after_close = false;
    if (scan_thread) return;
    scan = calloc(1,sizeof(*scan));
    if (scan) {
        scan->refs = 2;
        GUID *selected = w_plan();
        if (selected) scan->requested = *selected;
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
    if (scan_stale || plan_choice.open || duration_edit.choice.open) {
        // Read again after the local write or after a dropdown is closed.
        scan_stale = false;
        w_release_scan(scan);
        scan = NULL;
        w_power_start();
        return;
    }
    if (scan->failed && plan_count) {
        w_release_scan(scan);
        scan = NULL;
        return;
    }
    GUID *previous = w_plan();
    GUID initial = scan->count ? scan->plans[scan->initial].id : (GUID){0};
    GUID keep = previous ? *previous : initial;
    int timer = w_timer_id(), selected = -1, fallback = 0;
    bool same = scan->count == plan_count;
    for (size_t i=0; same && i<plan_count; ++i)
        same = IsEqualGUID(&plans[i].id,&scan->plans[i].id) && !wcscmp(plans[i].name,scan->plans[i].name);
    if (!same) SendMessageW(plans_box,CB_RESETCONTENT,0,0);
    free(plans);
    plans = scan->plans;
    scan->plans = NULL;
    plan_count = 0;
    for (size_t i = 0; i < scan->count; ++i) {
        if (!same && SendMessageW(plans_box,CB_ADDSTRING,0,(LPARAM)plans[i].name) < 0) continue;
        if (IsEqualGUID(&keep,&plans[i].id)) selected = (int)plan_count;
        if (IsEqualGUID(&scan->active,&plans[i].id)) fallback = (int)plan_count;
        if (plan_count != i) plans[plan_count] = plans[i];
        ++plan_count;
    }
    bool passive = selected >= 0 && timer >= 0;
    if (selected < 0) selected = plan_count ? fallback : -1;
    plan_choice.row = selected;
    if (SendMessageW(plans_box,CB_GETCURSEL,0,0) != selected) SendMessageW(plans_box,CB_SETCURSEL,selected,0);
    GUID *shown = w_plan();
    if (!shown || IsEqualGUID(shown,&initial))
        w_show_timers(passive ? timer : 0,scan->values,shown && IsEqualGUID(shown,&scan->active),passive);
    else if (!passive) w_load_timers(0,false);
    else {
        // The user switched plans while this worker was reading the previous plan.
        w_release_scan(scan);
        scan = NULL;
        w_power_start();
        return;
    }
    w_release_scan(scan);
    scan = NULL;
}
