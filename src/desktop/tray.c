#include "desktop/tray.h"
#include "theme/menu.h"
#include "desktop/desktop.h"
#include "tray/host.h"
#include <objbase.h>
#include <shellapi.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

enum w_tray_state { tray_normal, tray_minimized, tray_withdrawn };
typedef struct {
    HWND hwnd;
    DWORD pid, thread;
    ATOM atom;
    HANDLE process;
    HMONITOR monitor;
    int workspace;
    w_window_id managed;
    uint64_t key;
    enum w_tray_state state;
    HICON icon;
    bool observed, marked, associated, requested_icon, window_icon;
    unsigned match_strength;
    size_t match_row;
    bool ambiguous;
    wchar_t title[256], executable[128];
} w_tracked;

typedef struct { uint64_t key; size_t index; uint64_t restore; } w_tray_row;
static w_tracked *tracked;
static w_tray_row *rows;
static size_t tracked_count, tracked_cap, row_count, row_cap;
static uint64_t serial;
static bool pending, scanning;
static const uint64_t window_key = UINT64_C(1)<<63;
static const wchar_t marker[] = WINWM_TRAY_WINDOW_PROPERTY;

static void w_schedule(void) {
    if (!pending && !scanning && desk.root)
        pending = PostMessageW(desk.root,msg_tray_refresh,0,0) != 0;
}

static w_tracked *w_by_hwnd(HWND hwnd) {
    for (size_t i=0; i<tracked_count; ++i) if (tracked[i].hwnd == hwnd) return &tracked[i];
    return NULL;
}

static w_tracked *w_by_key(uint64_t key) {
    for (size_t i=0; i<tracked_count; ++i) if (tracked[i].key == key) return &tracked[i];
    return NULL;
}

static bool w_identity(const w_tracked *t) {
    DWORD pid = 0;
    if (GetWindowThreadProcessId(t->hwnd,&pid) != t->thread || pid != t->pid ||
        (ATOM)GetClassLongPtrW(t->hwnd,GCW_ATOM) != t->atom) return false;
    if (t->process && WaitForSingleObject(t->process,0) != WAIT_TIMEOUT) return false;
    if (t->marked && (uintptr_t)GetPropW(t->hwnd,marker) != t->key) return false;
    return true;
}

static void w_remove(w_tracked *t) {
    if (t->marked && (uintptr_t)GetPropW(t->hwnd,marker) == t->key) RemovePropW(t->hwnd,marker);
    if (t->icon) DestroyIcon(t->icon);
    if (t->process) CloseHandle(t->process);
    size_t index = (size_t)(t-tracked);
    --tracked_count;
    memmove(t,t+1,(tracked_count-index)*sizeof(*t));
}

static int w_monitor_output(HMONITOR monitor) {
    for (int i=0; i<output_limit; ++i)
        if (desk.outputs[i].live && desk.outputs[i].monitor == monitor) return i;
    return -1;
}

static w_tracked *w_add(HWND hwnd, DWORD pid, DWORD thread) {
    if (tracked_count == tracked_cap) {
        size_t cap = tracked_cap ? tracked_cap*2 : 32;
        w_tracked *p = realloc(tracked,cap*sizeof(*p));
        if (!p) return NULL;
        tracked = p;
        tracked_cap = cap;
    }
    w_tracked *t = &tracked[tracked_count++];
    *t = (w_tracked){.hwnd=hwnd,.pid=pid,.thread=thread,.atom=(ATOM)GetClassLongPtrW(hwnd,GCW_ATOM),
                     .key=window_key | ++serial,.managed={-1,0}};
    return t;
}

static bool w_classify(w_tracked *t, int slot) {
    HWND hwnd = t->hwnd;
    bool visible = IsWindowVisible(hwnd) != 0, managed_hide = w_os_shell_hidden(hwnd);
    if (slot >= 0 && desk.windows[slot].pid == t->pid && GetPropW(hwnd,L"winwm.managed")) {
        const w_model_window *window = &desk.model.windows[slot];
        t->managed = (w_window_id){slot,window->generation};
        t->monitor = desk.outputs[window->output].monitor;
        t->workspace = window->workspace;
        t->observed |= desk.windows[slot].presented || managed_hide;
    } else {
        t->managed = (w_window_id){-1,0};
        if (!t->monitor || (visible && !IsIconic(hwnd))) {
            HMONITOR monitor = MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST);
            int output = w_monitor_output(monitor);
            if (output >= 0) {
                t->monitor = monitor;
                t->workspace = desk.model.selected[output];
            }
        }
    }
    t->observed |= visible;
    // Window manager hides (workspace, tab, scratchpad) and DWM cloaks still
    // represent an application on a desktop. Only native minimize/withdraw is a tray fallback.
    bool iconic = IsIconic(hwnd) != 0;
    bool previously_minimized = t->managed.slot >= 0 && desk.model.windows[t->managed.slot].minimized;
    enum w_tray_state state;
    if (managed_hide) state = previously_minimized || (iconic && t->state == tray_minimized) ? tray_minimized : tray_normal;
    else if (w_os_cloaked(hwnd)) state = tray_normal;
    else if (iconic) state = tray_minimized;
    else state = !visible && t->observed ? tray_withdrawn : tray_normal;
    bool changed = state != t->state;
    t->state = state;
    return changed;
}

typedef struct w_icon_job {
    struct w_icon_job *next;
    uint64_t key;
    HWND notify;
    HICON icon;
    wchar_t path[os_path_cap];
} w_icon_job;
static SRWLOCK icon_lock;
static w_icon_job *finished_icons;
static HWND icon_target;

static DWORD WINAPI w_executable_icon(void *arg) {
    w_icon_job *job = arg;
    SHFILEINFOW file = {0};
    HRESULT apartment = CoInitializeEx(NULL,COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (SUCCEEDED(apartment)) {
        if (SHGetFileInfoW(job->path,0,&file,sizeof(file),SHGFI_ICON|SHGFI_SMALLICON)) job->icon = file.hIcon;
        CoUninitialize();
    }
    AcquireSRWLockExclusive(&icon_lock);
    if (job->icon && icon_target == job->notify && PostMessageW(job->notify,msg_tray_refresh,0,0)) {
        job->next = finished_icons;
        finished_icons = job;
        job = NULL;
    }
    ReleaseSRWLockExclusive(&icon_lock);
    if (job) { if (job->icon) DestroyIcon(job->icon); free(job); }
    return 0;
}

static void w_icons_done(void) {
    AcquireSRWLockExclusive(&icon_lock);
    w_icon_job *jobs = finished_icons;
    finished_icons = NULL;
    ReleaseSRWLockExclusive(&icon_lock);
    while (jobs) {
        w_icon_job *job = jobs;
        jobs = job->next;
        w_tracked *t = w_by_key(job->key);
        if (t && !t->window_icon) {
            if (t->icon) DestroyIcon(t->icon);
            t->icon = job->icon;
        } else DestroyIcon(job->icon);
        free(job);
    }
}

static void CALLBACK w_icon_reply(HWND hwnd, UINT msg, ULONG_PTR key, LRESULT result) {
    (void)msg;
    w_tracked *t = w_by_key(key);
    if (!t || t->hwnd != hwnd || !result || !w_identity(t)) return;
    HICON copy = CopyIcon((HICON)result);
    if (copy) {
        if (t->icon) DestroyIcon(t->icon);
        t->icon = copy;
        t->window_icon = true;
        w_schedule();
    }
}

static void w_details(w_tracked *t) {
    GetWindowTextW(t->hwnd,t->title,(int)count_of(t->title));
    if (t->requested_icon) return;
    t->requested_icon = true;
    if (t->managed.slot < 0)
        t->marked = SetPropW(t->hwnd,marker,(HANDLE)(uintptr_t)t->key) != 0;
    t->process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,t->pid);
    HICON cls = (HICON)GetClassLongPtrW(t->hwnd,GCLP_HICONSM);
    if (!cls) cls = (HICON)GetClassLongPtrW(t->hwnd,GCLP_HICON);
    if (cls) t->icon = CopyIcon(cls);
    wchar_t path[os_path_cap];
    DWORD size = count_of(path);
    if (t->process && QueryFullProcessImageNameW(t->process,0,path,&size)) {
        const wchar_t *base = wcsrchr(path,L'\\');
        lstrcpynW(t->executable,base ? base+1 : path,(int)count_of(t->executable));
        if (!t->icon) {
            w_icon_job *job = calloc(1,sizeof(*job));
            if (job) {
                job->notify=desk.root;
                job->key=t->key;
                wcscpy(job->path,path);
                if (!QueueUserWorkItem(w_executable_icon,job,WT_EXECUTEDEFAULT)) free(job);
            }
        }
    }
    // A hung application must not stall input or layout.
    SendMessageCallbackW(t->hwnd,WM_GETICON,ICON_SMALL2,0,w_icon_reply,(ULONG_PTR)t->key);
}

static void w_observe(HWND hwnd, DWORD event) {
    w_tracked *t = w_by_hwnd(hwnd);
    if (t && (event == EVENT_OBJECT_DESTROY || event == EVENT_OBJECT_CREATE || !w_identity(t))) {
        w_remove(t);
        t = NULL;
        w_schedule();
    }
    if (event == EVENT_OBJECT_DESTROY) return;
    int slot = w_desktop_find(hwnd);
    bool added = false;
    if (!t) {
        DWORD pid = 0;
        if (slot >= 0 && GetPropW(hwnd,L"winwm.managed")) pid = desk.windows[slot].pid;
        else if (!w_os_candidate(hwnd,&pid)) return;
        DWORD actual = 0, thread = GetWindowThreadProcessId(hwnd,&actual);
        if (!thread || pid != actual) return;
        t = w_add(hwnd,pid,thread);
        if (!t) return;
        added = true;
    }
    bool changed = w_classify(t,slot);
    bool title = event == EVENT_OBJECT_NAMECHANGE && t->state != tray_normal;
    if (t->state != tray_normal && (changed || added || title)) w_details(t);
    if (added || changed || title) w_schedule();
}

void w_desktop_tray_observe(HWND hwnd, DWORD event) {
    switch (event) {
    case 0: break;
    case EVENT_OBJECT_DESTROY: w_tray_prune(hwnd); break;
    case EVENT_OBJECT_CREATE: case EVENT_OBJECT_SHOW: case EVENT_OBJECT_HIDE:
    case EVENT_OBJECT_CLOAKED: case EVENT_OBJECT_UNCLOAKED:
    case EVENT_SYSTEM_MINIMIZESTART: case EVENT_SYSTEM_MINIMIZEEND:
    case EVENT_SYSTEM_MOVESIZEEND: case EVENT_SYSTEM_FOREGROUND: break;
    case EVENT_OBJECT_NAMECHANGE: {
        w_tracked *t = w_by_hwnd(hwnd);
        if (!t || t->state == tray_normal) return;
        break;
    }
    default: return;
    }
    w_observe(hwnd,event);
}

void w_desktop_tray_scan(bool begin) {
    scanning = begin;
    if (!begin) {
        for (size_t i=tracked_count; i>0; --i)
            if (!w_identity(&tracked[i-1])) w_remove(&tracked[i-1]);
        w_schedule();
    }
}

void w_desktop_tray_update(void) {
    pending = false;
    w_icons_done();
    size_t native = w_tray_count();
    if (native > INT_MAX || tracked_count > (size_t)INT_MAX-native) return;
    size_t needed = native+tracked_count;
    if (needed > row_cap) {
        w_tray_row *next = realloc(rows,needed*sizeof(*rows));
        if (!next) return;
        rows = next;
        row_cap = needed;
    }
    size_t previous_count = row_count;
    row_count = 0;
    for (size_t j=0; j<tracked_count; ++j) {
        tracked[j].associated = false;
        tracked[j].match_strength = 0;
        tracked[j].ambiguous = false;
    }
    for (size_t i=0; i<native; ++i) {
        w_tray_icon icon;
        if (!w_tray_get(i,&icon) || icon.hidden || !icon.token) continue;
        bool present = false;
        size_t siblings = 0;
        w_tracked *single = NULL;
        for (size_t j=0; j<tracked_count; ++j) {
            w_tracked *t = &tracked[j];
            if (t->pid != icon.pid) continue;
            if (t->state == tray_normal) { present=true; break; }
            single = t;
            ++siblings;
        }
        if (present) continue;
        size_t row = row_count++;
        rows[row] = (w_tray_row){.key=icon.token,.index=i};
        // A helper HWND commonly owns a single-window application's icon.
        // For multi-window processes, trust only an actual HWND relationship.
        if (!siblings) continue;
        HWND owner = w_os_app_window(icon.owner);
        w_tracked *candidate = w_by_hwnd(owner);
        if (!candidate || candidate->pid != icon.pid) candidate = w_by_hwnd(icon.owner);
        unsigned strength = 2;
        if (!candidate || candidate->pid != icon.pid) {
            if (siblings != 1) continue;
            candidate = single;
            strength = 1;
        }
        if (strength > candidate->match_strength) {
            candidate->match_strength = strength;
            candidate->match_row = row;
            candidate->ambiguous = false;
        } else if (strength == candidate->match_strength) candidate->ambiguous = true;
    }
    for (size_t j=0; j<tracked_count; ++j) {
        w_tracked *t = &tracked[j];
        if (t->state == tray_normal) continue;
        t->associated = t->match_strength && (t->state == tray_withdrawn || !t->ambiguous);
        if (!t->associated) rows[row_count++] = (w_tray_row){.key=t->key,.index=j};
        else if (t->state == tray_minimized) rows[t->match_row].restore = t->key;
    }
    for (int i=0; i<output_limit; ++i) {
        w_bar_window *bar = &desk.outputs[i].bar;
        if (desk.outputs[i].live && (row_count != previous_count || bar->tray.open || bar->tray.start))
            w_bar_invalidate(bar,"tray");
    }
}

static int w_count(void) { return (int)row_count; }

static bool w_read(int index, w_bar_tray_item *out) {
    if (!out || (unsigned)index >= row_count) return false;
    w_tray_row row = rows[index];
    *out = (w_bar_tray_item){.key=row.key};
    if (!(row.key & window_key)) {
        w_tray_icon icon;
        if (!w_tray_get(row.index,&icon) || icon.token != row.key || icon.hidden) return false;
        out->icon = icon.icon;
        const wchar_t *label = *icon.tip ? icon.tip : L"Background application";
        swprintf(out->tooltip,count_of(out->tooltip),L"%ls%ls",label,
                 row.restore ? L"\nMinimized · Click to restore" : L"");
        return true;
    }
    if (row.index >= tracked_count || tracked[row.index].key != row.key) return false;
    const w_tracked *t = &tracked[row.index];
    if (t->state == tray_normal) return false;
    out->icon = t->icon;
    const wchar_t *label = *t->title ? t->title : *t->executable ? t->executable : L"Application";
    swprintf(out->tooltip,count_of(out->tooltip),L"%.214ls\n%ls",label,
             t->state == tray_minimized ? L"Minimized · Click to restore" : L"Hidden · Click to show");
    return true;
}

static w_tracked *w_restorable(uint64_t key, bool combined) {
    w_tracked *t = w_by_key(key);
    if (!t) return NULL;
    if (!w_identity(t)) { w_remove(t); w_schedule(); return NULL; }
    int slot = w_desktop_find(t->hwnd);
    if (t->managed.slot >= 0 && (!w_model_valid(&desk.model,t->managed) || slot != t->managed.slot)) {
        w_schedule();
        return NULL;
    }
    if (w_classify(t,slot)) w_schedule();
    if (t->state == tray_normal || (combined && t->state != tray_minimized) ||
        t->associated != combined || (!t->marked && !t->process && t->managed.slot < 0))
        return NULL;
    return t;
}

static bool w_restore(uint64_t key, bool combined) {
    w_tracked *t = w_restorable(key,combined);
    if (!t) return false;
    if (t->managed.slot >= 0) {
        int slot = t->managed.slot;
        if (t->state == tray_withdrawn && !ShowWindowAsync(t->hwnd,SW_SHOW)) return false;
        desk.model.windows[slot].withdrawn = false;
        desk.model.windows[slot].minimized = t->state == tray_minimized || IsIconic(t->hwnd);
        w_desktop_activate(slot);
    } else {
        if (!ShowWindowAsync(t->hwnd,t->state == tray_minimized ? SW_RESTORE : SW_SHOW)) return false;
        int output = w_monitor_output(t->monitor);
        if (output < 0) output = w_monitor_output(MonitorFromWindow(t->hwnd,MONITOR_DEFAULTTONEAREST));
        if (output >= 0) {
            desk.model.output = output;
            w_model_workspace(&desk.model,t->workspace);
            w_desktop_schedule();
        }
        SetForegroundWindow(t->hwnd);
    }
    return true;
}

static void w_activate(uint64_t key, UINT event, POINT screen) {
    size_t i = 0;
    while (i < row_count && rows[i].key != key) ++i;
    if (i == row_count) return;
    if (!(key & window_key)) {
        w_tray_row row = rows[i];
        if (row.restore && (event == WM_LBUTTONUP || event == NIN_KEYSELECT)) {
            w_tray_icon icon;
            if (!w_tray_get(row.index,&icon) || icon.token != key || icon.hidden) return;
            if (w_restore(row.restore,true)) return;
        }
        enum w_tray_action action;
        switch (event) {
        case WM_LBUTTONUP: action=w_tray_primary; break;
        case WM_RBUTTONUP: action=w_tray_context; break;
        case WM_MBUTTONUP: action=w_tray_middle; break;
        case WM_LBUTTONDBLCLK: action=w_tray_double; break;
        case NIN_KEYSELECT: action=w_tray_keyboard; break;
        default: return;
        }
        w_tray_action(key,action,screen);
        return;
    }
    if (event == WM_RBUTTONUP) {
        if (!w_restorable(key,false)) return;
        const w_theme_menu_item restore={1,L"Restore window",L"\xe923",false};
        UINT selected = w_theme_track_menu(desk.root,screen,TPM_RIGHTBUTTON,NULL,&restore,1);
        if (selected == 1) w_restore(key,false);
    } else if (event == WM_LBUTTONUP || event == WM_LBUTTONDBLCLK || event == NIN_KEYSELECT) w_restore(key,false);
}

bool w_desktop_tray_locate(uint64_t key, RECT *bounds) {
    POINT cursor;
    int output = GetCursorPos(&cursor)
               ? w_monitor_output(MonitorFromPoint(cursor,MONITOR_DEFAULTTONEAREST)) : -1;
    if (output >= 0 && w_bar_tray_bounds(&desk.outputs[output].bar,key,bounds)) return true;
    for (int i=0; i<output_limit; ++i)
        if (i != output && desk.outputs[i].live && w_bar_tray_bounds(&desk.outputs[i].bar,key,bounds)) return true;
    if (output >= 0 && w_bar_tray_bounds(&desk.outputs[output].bar,0,bounds)) return true;
    for (int i=0; i<output_limit; ++i)
        if (i != output && desk.outputs[i].live && w_bar_tray_bounds(&desk.outputs[i].bar,0,bounds)) return true;
    return false;
}

void w_desktop_tray_bind(w_bar_window *bar) {
    bar->tray_count = w_count;
    bar->tray_read = w_read;
    bar->tray_activate = w_activate;
    AcquireSRWLockExclusive(&icon_lock);
    icon_target = desk.root;
    ReleaseSRWLockExclusive(&icon_lock);
}

void w_desktop_tray_stop(void) {
    AcquireSRWLockExclusive(&icon_lock);
    icon_target = NULL;
    ReleaseSRWLockExclusive(&icon_lock);
    w_icons_done();
    while (tracked_count) w_remove(&tracked[tracked_count-1]);
    free(tracked);
    free(rows);
    tracked = NULL;
    rows = NULL;
    tracked_cap = row_cap = row_count = 0;
    pending = false;
}
