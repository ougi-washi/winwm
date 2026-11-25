#define COBJMACROS
#include "catalog/catalog.h"
#include <shlobj.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <propkey.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <io.h>

////////////////
//~ Catalog

typedef struct w_catalog_item {
    wchar_t name[160], target[1024], args[512], directory[512];
    uint32_t packaged;
} w_catalog_item;

typedef struct w_catalog_row {
    w_catalog_item app;
    int icon, variant;
} w_catalog_row;

struct w_catalog_state {
    w_catalog_row *rows;
    int count, capacity;
    LONG refs;
    HANDLE images;
    bool failed;
};

static w_catalog_state *store;
static LONG started;
static HWND target_window;
static UINT ready_message;
static SRWLOCK catalog_lock;
static ULONGLONG refresh_after;

////////////////
//~ Catalog Functions

static w_catalog_state *w_new_catalog(void) {
    w_catalog_state *c = calloc(1, sizeof(*c));
    if (c) c->refs = 1;
    return c;
}

void w_catalog_release(w_catalog_state *c) {
    if (c && InterlockedDecrement(&c->refs) == 0) {
        free(c->rows);
        free(c);
    }
}

w_catalog_state *w_catalog_acquire(void) {
    AcquireSRWLockShared(&catalog_lock);
    w_catalog_state *c = store;
    if (c) InterlockedIncrement(&c->refs);
    ReleaseSRWLockShared(&catalog_lock);
    return c;
}

static void w_append(w_catalog_state *c, const w_catalog_item *item, LPCWSTR icon_path, UINT flags) {
    if (c->count == c->capacity) {
        int capacity = c->capacity ? c->capacity * 2 : 64;
        w_catalog_row *next = realloc(c->rows, (size_t)capacity * sizeof(*next));
        if (!next) { c->failed = true; return; }
        c->rows = next;
        c->capacity = capacity;
    }
    w_catalog_row *row = &c->rows[c->count++];
    *row = (w_catalog_row){.app = *item};
    if (icon_path) {
        SHFILEINFOW info = {0};
        HANDLE list = (HANDLE)SHGetFileInfoW(icon_path, 0, &info, sizeof(info),
                              SHGFI_SYSICONINDEX | SHGFI_SMALLICON | flags);
        if (list) {
            c->images = list;
            row->icon = info.iIcon + 1;
        }
    }
}

static int w_compare_apps(const void *a, const void *b) {
    const w_catalog_item *x = &((const w_catalog_row *)a)->app, *y = &((const w_catalog_row *)b)->app;
    int order = _wcsicmp(x->name, y->name);
    if (!order) order = (int)x->packaged - (int)y->packaged;
    if (!order) order = _wcsicmp(x->target, y->target);
    if (!order) order = wcscmp(x->args, y->args);
    return order ? order : _wcsicmp(x->directory, y->directory);
}

static void w_sort_apps(w_catalog_state *c) {
    if (c->count < 2) return;
    qsort(c->rows, c->count, sizeof(*c->rows), w_compare_apps);
    int unique = 1, first = 0;
    for (int i = 1; i < c->count; ++i) {
        if (!w_compare_apps(&c->rows[unique-1], &c->rows[i])) {
            if (!c->rows[unique-1].icon) c->rows[unique-1].icon = c->rows[i].icon;
            continue;
        }
        if (unique != i) c->rows[unique] = c->rows[i];
        if (_wcsicmp(c->rows[first].app.name, c->rows[unique].app.name)) first = unique;
        else {
            c->rows[first].variant = 1;
            c->rows[unique].variant = unique-first+1;
        }
        ++unique;
    }
    c->count = unique;
}

static bool w_walk(w_catalog_state *c, IShellLinkW *link, IPersistFile *persist, const wchar_t *folder,
                   int depth) {
    if (depth > 32 || c->failed)
        return !c->failed;
    wchar_t pattern[os_path_cap];
    if (swprintf(pattern, count_of(pattern), L"%ls\\*", folder) < 0)
        return false;
    WIN32_FIND_DATAW f;
    HANDLE listing = FindFirstFileW(pattern, &f);
    if (listing == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        return err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND;
    }
    bool complete = true;
    do {
        if (f.cFileName[0] == L'.' || f.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            continue;
        wchar_t path[os_path_cap];
        if (swprintf(path, count_of(path), L"%ls\\%ls", folder, f.cFileName) < 0)
            continue;
        if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!w_walk(c, link, persist, path, depth + 1)) complete = false;
            continue;
        }
        wchar_t *ext = wcsrchr(f.cFileName, L'.');
        if (!ext || _wcsicmp(ext, L".lnk"))
            continue;
        if (FAILED(IPersistFile_Load(persist, path, STGM_READ)))
            continue;
        w_catalog_item entry = {0};
        WIN32_FIND_DATAW ignored;
        if (FAILED(IShellLinkW_GetPath(link, entry.target, count_of(entry.target), &ignored, SLGP_RAWPATH)) ||
            !*entry.target)
            continue;
        const wchar_t *base = wcsrchr(entry.target, L'\\');
        base = base ? base + 1 : entry.target;
        const wchar_t *suffix = wcsrchr(base, L'.');
        if (!suffix || _wcsicmp(suffix, L".exe") || !_wcsicmp(base, L"explorer.exe"))
            continue;
        IShellLinkW_GetArguments(link, entry.args, count_of(entry.args));
        IShellLinkW_GetWorkingDirectory(link, entry.directory, count_of(entry.directory));
        *ext = 0;
        wcsncpy(entry.name, f.cFileName, count_of(entry.name) - 1);
        w_append(c, &entry, path, 0);
    } while (!c->failed && FindNextFileW(listing, &f));
    if (c->failed || GetLastError() != ERROR_NO_MORE_FILES) complete = false;
    FindClose(listing);
    return complete;
}

static bool w_packaged_items(w_catalog_state *c) {
    IShellItem *apps = NULL;
    IEnumShellItems *all = NULL;
    if (SUCCEEDED(SHGetKnownFolderItem(&FOLDERID_AppsFolder, KF_FLAG_DEFAULT, NULL, &IID_IShellItem,
                                       (void **)&apps))) {
        IShellItem_BindToHandler(apps, NULL, &BHID_EnumItems, &IID_IEnumShellItems, (void **)&all);
        IShellItem_Release(apps);
    }
    if (!all)
        return false;
    IShellItem *item;
    HRESULT hr = S_OK;
    while (!c->failed && (hr = IEnumShellItems_Next(all, 1, &item, NULL)) == S_OK) {
        IShellItem2 *details = NULL;
        PWSTR id = NULL, name = NULL;
        w_catalog_item app = {.packaged = true};
        if (SUCCEEDED(IShellItem_QueryInterface(item, &IID_IShellItem2, (void **)&details)) &&
            SUCCEEDED(IShellItem2_GetString(details, &PKEY_AppUserModel_ID, &id)) && id && wcschr(id, L'!') &&
            SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_NORMALDISPLAY, &name)) && name &&
            wcslen(id) < count_of(app.target) && wcslen(name) < count_of(app.name)) {
            wcscpy(app.name, name);
            wcscpy(app.target, id);
            PIDLIST_ABSOLUTE pidl = NULL;
            SHGetIDListFromObject((IUnknown *)item, &pidl);
            w_append(c, &app, (LPCWSTR)pidl, SHGFI_PIDL);
            CoTaskMemFree(pidl);
        }
        CoTaskMemFree(id);
        CoTaskMemFree(name);
        if (details)
            IShellItem2_Release(details);
        IShellItem_Release(item);
    }
    IEnumShellItems_Release(all);
    return hr == S_FALSE;
}

static bool w_cache_path(wchar_t *path) {
    return w_os_data_path(path, os_path_cap, L"apps.v1.bin", true);
}

static w_catalog_state *w_read_cache(const wchar_t *path) {
    FILE *f = _wfopen(path, L"rb");
    if (!f)
        return NULL;
    uint32_t head[3];
    w_catalog_state *c = NULL;
    if (fread(head, sizeof(head), 1, f) == 1 && head[0] == 0x4e444131 && head[1] == sizeof(w_catalog_item) &&
        _filelengthi64(_fileno(f)) == (int64_t)(sizeof(head) + (uint64_t)head[2] * sizeof(w_catalog_item)) &&
        head[2] <= 65536) {
        c = w_new_catalog();
        if (c) {
            bool valid = true;
            for (unsigned i = 0; valid && !c->failed && i < head[2]; i++) {
                w_catalog_item item, *e = &item;
                valid = fread(e, sizeof(*e), 1, f) == 1 &&
                    e->packaged <= 1 && *e->name && *e->target && wmemchr(e->name, 0, count_of(e->name)) &&
                    wmemchr(e->target, 0, count_of(e->target)) && wmemchr(e->args, 0, count_of(e->args)) &&
                    wmemchr(e->directory, 0, count_of(e->directory));
                if (valid) w_append(c, e, NULL, 0);
            }
            if (!valid || c->failed) {
                w_catalog_release(c);
                c = NULL;
            }
        }
    }
    fclose(f);
    return c;
}

static void w_publish(w_catalog_state *c) {
    w_sort_apps(c);
    AcquireSRWLockExclusive(&catalog_lock);
    w_catalog_state *old = store;
    store = c;
    ReleaseSRWLockExclusive(&catalog_lock);
    w_catalog_release(old);
    PostMessageW(target_window, ready_message, 0, 0);
}

static void w_save_cache(const wchar_t *path, const w_catalog_state *c) {
    wchar_t tmp[os_path_cap + 32];
    swprintf(tmp, count_of(tmp), L"%ls.%lu.tmp", path, GetCurrentProcessId());
    FILE *f = _wfopen(tmp, L"wb");
    if (!f)
        return;
    uint32_t header[] = {0x4e444131, sizeof(w_catalog_item), (uint32_t)c->count};
    bool ok = fwrite(header, sizeof(header), 1, f) == 1;
    for (int i = 0; ok && i < c->count; ++i)
        ok = fwrite(&c->rows[i].app, sizeof(w_catalog_item), 1, f) == 1;
    if (fclose(f) != 0 || !ok || !MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING))
        DeleteFileW(tmp);
}

////////////////
//~ Catalog indexing

static void w_keep_missing_sources(w_catalog_state *next, const w_catalog_state *current,
                                   bool shortcuts_complete, bool packaged_complete) {
    if (!current) return;
    for (int i = 0; i < current->count && !next->failed; i++) {
        const w_catalog_row *old = &current->rows[i];
        if (old->app.packaged ? packaged_complete : shortcuts_complete) continue;
        w_append(next, &old->app, NULL, 0);
        if (!next->failed) next->rows[next->count-1].icon = old->icon;
    }
    if (!next->images) next->images = current->images;
}

static DWORD WINAPI w_index_worker(void *unused) {
    (void)unused;
    wchar_t cache[os_path_cap] = {0};
    w_catalog_state *current = w_catalog_acquire();
    if (w_cache_path(cache) && !current) {
        w_catalog_state *saved = w_read_cache(cache);
        if (saved)
            w_publish(saved);
    }
    w_catalog_release(current);
    w_catalog_state *c = w_new_catalog();
    if (!c) {
        InterlockedExchange(&started, 0);
        return 0;
    }
    bool shortcuts_complete = false, packaged_complete = false;
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(hr)) {
        IShellLinkW *link = NULL;
        IPersistFile *persist = NULL;
        if (SUCCEEDED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW,
                                       (void **)&link)) &&
            SUCCEEDED(IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&persist))) {
            const KNOWNFOLDERID *folders[] = {&FOLDERID_Programs, &FOLDERID_CommonPrograms};
            shortcuts_complete = true;
            for (size_t i = 0; i < count_of(folders); i++) {
                PWSTR path = NULL;
                HRESULT folder = SHGetKnownFolderPath(folders[i], KF_FLAG_DONT_VERIFY, NULL, &path);
                if (SUCCEEDED(folder)) {
                    if (!w_walk(c, link, persist, path, 0)) shortcuts_complete = false;
                    CoTaskMemFree(path);
                } else shortcuts_complete = false;
            }
            IPersistFile_Release(persist);
        }
        if (link)
            IShellLinkW_Release(link);
        packaged_complete = w_packaged_items(c);
        CoUninitialize();
    }
    // Shell namespaces and redirected Start menus can become temporarily unavailable.
    // Preserve entries from any incomplete source along with its process-wide shell icons.
    if (SUCCEEDED(hr) && (!shortcuts_complete || !packaged_complete) && !c->failed) {
        current = w_catalog_acquire();
        w_keep_missing_sources(c,current,shortcuts_complete,packaged_complete);
        w_catalog_release(current);
    }
    if (SUCCEEDED(hr) && !c->failed) {
        if (*cache && shortcuts_complete && packaged_complete)
            w_save_cache(cache, c);
        w_publish(c);
    } else
        w_catalog_release(c);
    InterlockedExchange(&started, 0);
    return 0;
}

void w_catalog_begin(HWND w, UINT msg) {
    if (GetTickCount64() < refresh_after || InterlockedCompareExchange(&started, 1, 0))
        return;
    target_window = w;
    ready_message = msg;
    if (!QueueUserWorkItem(w_index_worker, NULL, WT_EXECUTELONGFUNCTION))
        InterlockedExchange(&started, 0);
    else
        refresh_after = GetTickCount64() + 60000;
}

////////////////
//~ Catalog queries

int w_catalog_count(const w_catalog_state *c) {
    return c ? c->count : 0;
}

uint64_t w_catalog_identity(const w_catalog_state *c, int i) {
    const w_catalog_item *app = &c->rows[i].app;
    uint64_t hash = UINT64_C(14695981039346656037) ^ app->packaged;
    const wchar_t *fields[] = {app->name, app->target, app->args, app->directory};
    for (size_t j = 0; j < count_of(fields); ++j) {
        const wchar_t *p = fields[j];
        do {
            hash = (hash ^ (uint16_t)*p) * UINT64_C(1099511628211);
        } while (*p++);
    }
    return hash ? hash : 1;
}

HANDLE w_catalog_images(const w_catalog_state *c) {
    return c ? c->images : NULL;
}

void w_catalog_label(const w_catalog_state *c, int i, wchar_t *out, int cap, int *icon) {
    const w_catalog_row *r = &c->rows[i];
    *icon = r->icon;
    if (!r->variant) swprintf(out, cap, L"%ls", r->app.name);
    else {
        const wchar_t *file = wcsrchr(r->app.target, L'\\');
        swprintf(out, cap, L"%.150ls  ·  %.80ls (%d)", r->app.name, file ? file+1 : r->app.target, r->variant);
    }
}

typedef struct w_catalog_launch_job {
    w_catalog_item item;
    bool admin;
} w_catalog_launch_job;

static DWORD WINAPI w_launch_entry(void *ptr) {
    w_catalog_launch_job *job = ptr;
    const w_catalog_item *e = &job->item;
    bool launched = false;
    DWORD error = ERROR_SUCCESS;
    if (job->admin && e->packaged) {
        HRESULT apartment = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        wchar_t name[count_of(e->target) + 32];
        PIDLIST_ABSOLUTE item = NULL;
        swprintf(name, count_of(name), L"shell:AppsFolder\\%ls", e->target);
        if (SUCCEEDED(SHParseDisplayName(name, NULL, &item, 0, NULL))) {
            SHELLEXECUTEINFOW info = {.cbSize=sizeof(info), .lpIDList=item, .lpVerb=L"runas",
                .fMask=SEE_MASK_INVOKEIDLIST | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI, .nShow=SW_SHOWNORMAL};
            launched = ShellExecuteExW(&info) != FALSE;
            if (!launched) error = GetLastError();
            CoTaskMemFree(item);
        }
        if (SUCCEEDED(apartment)) CoUninitialize();
    } else if (e->packaged) {
        if (SUCCEEDED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED))) {
            IApplicationActivationManager *am = NULL;
            if (SUCCEEDED(CoCreateInstance(&CLSID_ApplicationActivationManager, NULL, CLSCTX_LOCAL_SERVER,
                                           &IID_IApplicationActivationManager, (void **)&am))) {
                DWORD pid;
                CoAllowSetForegroundWindow((IUnknown *)am, NULL);
                launched = SUCCEEDED(IApplicationActivationManager_ActivateApplication(am, e->target, NULL, AO_NONE, &pid));
                IApplicationActivationManager_Release(am);
            }
            CoUninitialize();
        }
    } else {
        char name[4096];
        wchar_t resolved[os_path_cap], line[os_path_cap + 520];
        if (w_os_utf8(e->target, name, sizeof(name)) && w_os_provider(name, resolved, count_of(resolved))) {
            if (job->admin) {
                launched = w_os_run_as_admin(resolved, *e->args ? e->args : NULL, *e->directory ? e->directory : NULL);
                if (!launched) error = GetLastError();
            } else if (swprintf(line, count_of(line), L"\"%ls\" %ls", resolved, e->args) > 0) {
                STARTUPINFOW si = {.cb = sizeof(si)};
                PROCESS_INFORMATION pi = {0};
                if (CreateProcessW(resolved, line, NULL, NULL, FALSE, 0, NULL,
                                   *e->directory ? e->directory : NULL, &si, &pi)) {
                    AllowSetForegroundWindow(pi.dwProcessId);
                    launched = true;
                    CloseHandle(pi.hProcess);
                    CloseHandle(pi.hThread);
                }
            }
        }
    }
    if (!launched && error != ERROR_CANCELLED) PostMessageW(target_window, ready_message, job->admin ? 2 : 1, 0);
    free(job);
    return 0;
}

bool w_catalog_launch_item(const w_catalog_state *c, int index, bool admin) {
    if (!c || (unsigned)index >= (unsigned)c->count) return false;
    w_catalog_launch_job *entry = malloc(sizeof(*entry));
    if (!entry)
        return false;
    entry->item = c->rows[index].app;
    entry->admin = admin;
    if (QueueUserWorkItem(w_launch_entry, entry, WT_EXECUTEDEFAULT))
        return true;
    free(entry);
    return false;
}

bool w_catalog_launch(const char *name, bool admin) {
    wchar_t wide[256];
    if (!w_os_wide(name, wide, count_of(wide))) return false;
    w_catalog_state *c = w_catalog_acquire();
    bool ok = false;
    for (int i = 0; i < w_catalog_count(c); ++i)
        if (!_wcsicmp(c->rows[i].app.name, wide)) {
            ok = w_catalog_launch_item(c, i, admin);
            break;
        }
    w_catalog_release(c);
    return ok;
}
