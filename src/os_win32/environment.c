#include "os_win32/environment.h"
#include <userenv.h>
#include <stdlib.h>
#include <wchar.h>

typedef struct {
    HKEY root;
    const wchar_t *path, *parent;
    BOOL subtree;
    HKEY key;
    HANDLE event;
} w_env_watch;

static w_env_watch watches[] = {
    {.root=HKEY_LOCAL_MACHINE,.path=L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment",
     .parent=L"SYSTEM\\CurrentControlSet\\Control\\Session Manager"},
    {.root=HKEY_CURRENT_USER,.path=L"Environment",.parent=L""},
    {.root=HKEY_CURRENT_USER,.path=L"Volatile Environment",.parent=L"",.subtree=TRUE}
};
enum { watch_count = sizeof(watches)/sizeof(*watches) };
static SRWLOCK lifetime_lock, scan_lock;
static HANDLE worker, quit, requested, token;
static HWND target;
static UINT changed_message;
static wchar_t *previous;
static bool initialized, retry;

// An environment key can be deleted and recreated while winwm is running.
// Arm the parent for key creation before retrying the missing child.
static void w_watch(w_env_watch *watch) {
    if (watch->key) RegCloseKey(watch->key);
    watch->key = NULL;
    ResetEvent(watch->event);
    HKEY key = NULL;
    if (RegOpenKeyExW(watch->root,watch->path,0,KEY_NOTIFY,&key) != ERROR_SUCCESS) {
        if (RegOpenKeyExW(watch->root,watch->parent,0,KEY_NOTIFY,&key) != ERROR_SUCCESS) return;
        if (RegNotifyChangeKeyValue(key,FALSE,REG_NOTIFY_CHANGE_NAME|REG_NOTIFY_THREAD_AGNOSTIC,
                                    watch->event,TRUE) != ERROR_SUCCESS) {
            RegCloseKey(key);
            return;
        }
        watch->key = key;
        if (RegOpenKeyExW(watch->root,watch->path,0,KEY_NOTIFY,&key) != ERROR_SUCCESS) return;
        RegCloseKey(watch->key);
        ResetEvent(watch->event);
    }
    watch->key = key;
    DWORD flags = REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_THREAD_AGNOSTIC;
    if (watch->subtree) flags |= REG_NOTIFY_CHANGE_NAME;
    LSTATUS status = RegNotifyChangeKeyValue(key,watch->subtree,flags,watch->event,TRUE);
    if (status != ERROR_SUCCESS) {
        RegCloseKey(key);
        watch->key = NULL;
        // A simultaneous deletion can invalidate the newly opened key.
        if (status == ERROR_KEY_DELETED) SetEvent(watch->event);
    }
}

static bool w_same_name(const wchar_t *a, size_t n, const wchar_t *b, size_t m) {
    return n == m && CompareStringOrdinal(a,(int)n,b,(int)m,TRUE) == CSTR_EQUAL;
}

static const wchar_t *w_find(const wchar_t *block, const wchar_t *name, size_t n) {
    for (const wchar_t *p = block; p && *p; p += wcslen(p)+1) {
        const wchar_t *eq = wcschr(p,L'=');
        if (eq && w_same_name(p,(size_t)(eq-p),name,n)) return eq+1;
    }
    return NULL;
}

typedef struct {
    const wchar_t *start, *identity;
    int length, chars, index;
} w_path_part;

typedef struct {
    w_path_part *parts, *sorted;
    int count;
} w_path_list;

static int w_path_order(const void *left, const void *right) {
    const w_path_part *a = left, *b = right;
    if (!a->chars || !b->chars) return a->chars-b->chars;
    return CompareStringOrdinal(a->identity,a->chars,b->identity,b->chars,TRUE)-CSTR_EQUAL;
}

static int w_path_position(const void *left, const void *right) {
    int order = w_path_order(left,right);
    const w_path_part *a = left, *b = right;
    return order ? order : a->index-b->index;
}

const wchar_t *w_environment_path_end(const wchar_t *start) {
    start += wcsspn(start,L" \t");
    if (*start == L'"') {
        const wchar_t *close = wcschr(start+1,L'"');
        if (close) {
            const wchar_t *after = close+1+wcsspn(close+1,L" \t");
            if (!*after || *after == L';') return after;
        }
    }
    return start+wcscspn(start,L";");
}

static bool w_split_path(w_path_list *list, const wchar_t *path, bool sort) {
    if (!path || !*path) return true;
    int count = 1;
    for (const wchar_t *p = path; *p; ++p) if (*p == L';') ++count;
    list->parts = malloc((size_t)count*(sort ? 2 : 1)*sizeof(*list->parts));
    if (!list->parts) return false;
    for (const wchar_t *p = path;;) {
        const wchar_t *delimiter = w_environment_path_end(p), *end = delimiter;
        const wchar_t *start = p+wcsspn(p,L" \t");
        while (end > start && (end[-1] == L' ' || end[-1] == L'\t')) --end;
        if (end-start >= 2 && *start == L'"' && end[-1] == L'"') { ++start; --end; }
        int index = list->count++;
        list->parts[index] = (w_path_part){p,start,(int)(delimiter-p),(int)(end-start),index};
        if (!*delimiter) break;
        p = delimiter+1;
    }
    if (sort) {
        list->sorted = list->parts+count;
        memcpy(list->sorted,list->parts,(size_t)list->count*sizeof(*list->parts));
        qsort(list->sorted,(size_t)list->count,sizeof(*list->parts),w_path_position);
    }
    return true;
}

static int w_path_index(const w_path_list *list, w_path_part part, int before) {
    // Prefer the matching occurrence before the component already visited to
    // the right. Repeated persistent directories can surround a local override.
    part.index = before;
    int low = 0, high = list->count;
    while (low < high) {
        int mid = low+(high-low)/2;
        if (w_path_position(&list->sorted[mid],&part) < 0) low = mid+1;
        else high = mid;
    }
    if (low && !w_path_order(&part,&list->sorted[low-1])) return list->sorted[low-1].index;
    if (low < list->count && !w_path_order(&part,&list->sorted[low])) return list->sorted[low].index;
    return -1;
}

// Keep launcher-specific PATH components immediately before their next surviving
// registry entry. Registry order wins when the user moves persistent entries.
static bool w_merge_path(const wchar_t *old, const wchar_t *active, const wchar_t *next, wchar_t **out) {
    w_path_list base = {0}, local = {0}, fresh = {0};
    int *links = NULL;
    bool ok = false;
    if (!w_split_path(&base,old,true) || !w_split_path(&local,active,false) || !w_split_path(&fresh,next,true))
        goto done;
    links = calloc((size_t)fresh.count+1+local.count,sizeof(*links));
    if (!links) goto done;
    int *following = links+fresh.count+1, anchor = fresh.count, prefix = 0;
    while (prefix < local.count && w_path_index(&base,local.parts[prefix],base.count) < 0 &&
           w_path_index(&fresh,local.parts[prefix],fresh.count) < 0) ++prefix;
    size_t size = 0;
    for (int i = local.count-1; i >= 0; --i) {
        const w_path_part *p = &local.parts[i];
        int at = w_path_index(&fresh,*p,anchor);
        if (at >= 0) anchor = at;
        else if (w_path_index(&base,*p,base.count) < 0) {
            int slot = i < prefix ? 0 : anchor;
            following[i] = links[slot];
            links[slot] = i+1;
            size += p->length+1;
        }
    }
    if (next && *next) size += wcslen(next)+1;
    if (!size && !next) { ok = true; goto done; }
    wchar_t *result = calloc(size ? size : 1,sizeof(*result));
    if (!result) goto done;
    wchar_t *dst = result;
    for (int i = 0; i <= fresh.count; ++i) {
        for (int j = links[i]; j; j = following[j-1]) {
            w_path_part p = local.parts[j-1];
            wmemcpy(dst,p.start,p.length);
            dst += p.length;
            *dst++ = L';';
        }
        if (i < fresh.count) {
            w_path_part p = fresh.parts[i];
            wmemcpy(dst,p.start,p.length);
            dst += p.length;
            *dst++ = L';';
        }
    }
    if (dst > result) dst[-1] = 0;
    *out = result;
    ok = true;
done:
    free(links);
    free(base.parts);
    free(local.parts);
    free(fresh.parts);
    return ok;
}

static bool w_equal(const wchar_t *a, const wchar_t *b) {
    return a && b ? !wcscmp(a,b) : a == b;
}

static bool w_apply(const wchar_t *entry, const wchar_t *active, const wchar_t *old, const wchar_t *next,
                    bool *changed) {
    // A launch override survives unrelated settings changes. An explicit edit
    // or deletion of this same persistent variable takes effect immediately.
    if (previous && w_equal(old,next)) return true;
    size_t length = (size_t)(wcschr(entry,L'=')-entry);
    const wchar_t *current = w_find(active,entry,length);
    if (w_equal(current,next)) return true;
    bool ok = true;
    wchar_t *merged = NULL;
    if (w_same_name(entry,length,L"PATH",4)) {
        if (current && !w_equal(current,old)) {
            if (!w_merge_path(old,current,next,&merged)) return false;
            next = merged;
        }
    } else if (!previous && current) return true;
    if (!w_equal(current,next)) {
        wchar_t *name = malloc((length+1)*sizeof(*name));
        if (name) {
            wmemcpy(name,entry,length);
            name[length] = 0;
            ok = SetEnvironmentVariableW(name,next) != FALSE;
            if (ok) *changed = true;
            free(name);
        } else ok = false;
    }
    free(merged);
    return ok;
}

static void w_sync(void) {
    AcquireSRWLockExclusive(&scan_lock);
    bool first = !initialized, dirty = first || retry;
    if (WaitForSingleObject(requested,0) == WAIT_OBJECT_0) {
        ResetEvent(requested);
        dirty = true;
    }
    for (int i = 0; i < watch_count; ++i) {
        if ((dirty && !watches[i].key) || WaitForSingleObject(watches[i].event,0) == WAIT_OBJECT_0) {
            w_watch(&watches[i]);
            dirty = true;
        }
    }
    bool changed = false;
    void *block = NULL;
    if (dirty) retry = true;
    if (dirty && CreateEnvironmentBlock(&block,token,FALSE)) {
        wchar_t *active = GetEnvironmentStringsW();
        if (active) {
            bool ok = true;
            for (const wchar_t *p = block; *p; p += wcslen(p)+1) {
                const wchar_t *eq = wcschr(p,L'=');
                if (eq && eq != p && !w_apply(p,active,w_find(previous,p,(size_t)(eq-p)),eq+1,&changed)) ok = false;
            }
            for (const wchar_t *p = previous; p && *p; p += wcslen(p)+1) {
                const wchar_t *eq = wcschr(p,L'=');
                if (eq && eq != p && !w_find(block,p,(size_t)(eq-p)) && !w_apply(p,active,eq+1,NULL,&changed)) ok = false;
            }
            FreeEnvironmentStringsW(active);
            if (ok) {
                if (previous) DestroyEnvironmentBlock(previous);
                previous = block;
                retry = false;
            } else DestroyEnvironmentBlock(block);
        } else DestroyEnvironmentBlock(block);
    }
    initialized = true;
    if ((first || changed) && target && changed_message) PostMessageW(target,changed_message,changed,0);
    ReleaseSRWLockExclusive(&scan_lock);
}

static DWORD WINAPI w_monitor(void *unused) {
    (void)unused;
    HANDLE handles[2+watch_count] = {quit,requested};
    for (int i = 0; i < watch_count; ++i) handles[2+i] = watches[i].event;
    do {
        w_sync();
        DWORD wait = WaitForMultipleObjects(2+watch_count,handles,FALSE,INFINITE);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_FAILED) break;
    } while (true);
    return 0;
}

static void w_close(void) {
    if (worker) CloseHandle(worker);
    if (quit) CloseHandle(quit);
    if (requested) CloseHandle(requested);
    if (token) CloseHandle(token);
    if (previous) DestroyEnvironmentBlock(previous);
    for (int i = 0; i < watch_count; ++i) {
        if (watches[i].key) RegCloseKey(watches[i].key);
        if (watches[i].event) CloseHandle(watches[i].event);
        watches[i].key = NULL;
        watches[i].event = NULL;
    }
    worker = quit = requested = token = NULL;
    previous = NULL;
    initialized = retry = false;
}

bool w_environment_start(HWND window, UINT message) {
    AcquireSRWLockExclusive(&lifetime_lock);
    if (!worker) {
        target = window;
        changed_message = message;
        quit = CreateEventW(NULL,TRUE,FALSE,NULL);
        requested = CreateEventW(NULL,TRUE,FALSE,NULL);
        bool ok = quit && requested && OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY|TOKEN_DUPLICATE,&token);
        for (int i = 0; ok && i < watch_count; ++i) {
            watches[i].event = CreateEventW(NULL,TRUE,FALSE,NULL);
            ok = watches[i].event != NULL;
        }
        if (ok) worker = CreateThread(NULL,0,w_monitor,NULL,0,NULL);
        if (!worker) w_close();
    }
    bool running = worker != NULL;
    ReleaseSRWLockExclusive(&lifetime_lock);
    return running;
}

void w_environment_for_launch(void) {
    AcquireSRWLockShared(&lifetime_lock);
    if (worker) w_sync();
    ReleaseSRWLockShared(&lifetime_lock);
}

void w_environment_refresh(void) {
    AcquireSRWLockShared(&lifetime_lock);
    if (worker) SetEvent(requested);
    ReleaseSRWLockShared(&lifetime_lock);
}

void w_environment_stop(void) {
    AcquireSRWLockExclusive(&lifetime_lock);
    if (worker) {
        SetEvent(quit);
        WaitForSingleObject(worker,INFINITE);
        w_close();
    }
    ReleaseSRWLockExclusive(&lifetime_lock);
}
