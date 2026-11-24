#define COBJMACROS
#include "catalog/catalog.h"
#include <shlobj.h>
#include <shobjidl.h>
#include <propkey.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
enum { catalog_limit = 512 };

////////////////
//~ Catalog

typedef struct w_catalog_item {
    wchar_t name[160], target[1024], args[512], directory[512];
    uint32_t packaged;
} w_catalog_item;

typedef struct w_catalog_state {
    w_catalog_item items[catalog_limit];
    int count;
} w_catalog_state;

static w_catalog_state *store;
static LONG started;
static HWND target_window;
static UINT ready_message;

////////////////
//~ Catalog Functions

static void w_walk(w_catalog_state *c, IShellLinkW *link, IPersistFile *persist, const wchar_t *folder,
                 int depth) {
    if (depth > 12 || c->count >= catalog_limit)
        return;
    wchar_t pattern[os_path_cap];
    if (swprintf(pattern, count_of(pattern), L"%ls\\*", folder) < 0)
        return;
    WIN32_FIND_DATAW f;
    HANDLE listing = FindFirstFileW(pattern, &f);
    if (listing == INVALID_HANDLE_VALUE)
        return;
    do {
        if (f.cFileName[0] == L'.' || f.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            continue;
        wchar_t path[os_path_cap];
        if (swprintf(path, count_of(path), L"%ls\\%ls", folder, f.cFileName) < 0)
            continue;
        if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            w_walk(c, link, persist, path, depth + 1);
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
        c->items[c->count++] = entry;
    } while (c->count < catalog_limit && FindNextFileW(listing, &f));
    FindClose(listing);
}

static void w_packaged_items(w_catalog_state *c) {
    IShellItem *apps = NULL;
    IEnumShellItems *all = NULL;
    if (SUCCEEDED(SHGetKnownFolderItem(&FOLDERID_AppsFolder, KF_FLAG_DEFAULT, NULL, &IID_IShellItem,
                                       (void **)&apps))) {
        IShellItem_BindToHandler(apps, NULL, &BHID_EnumItems, &IID_IEnumShellItems, (void **)&all);
        IShellItem_Release(apps);
    }
    if (!all)
        return;
    IShellItem *item;
    while (c->count < catalog_limit && IEnumShellItems_Next(all, 1, &item, NULL) == S_OK) {
        IShellItem2 *details = NULL;
        PWSTR id = NULL, name = NULL;
        if (SUCCEEDED(IShellItem_QueryInterface(item, &IID_IShellItem2, (void **)&details)) &&
            SUCCEEDED(IShellItem2_GetString(details, &PKEY_AppUserModel_ID, &id)) && id && wcschr(id, L'!') &&
            SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_NORMALDISPLAY, &name)) && name &&
            wcslen(id) < count_of(c->items[0].target) && wcslen(name) < count_of(c->items[0].name)) {
            w_catalog_item *e = &c->items[c->count++];
            e->packaged = true;
            wcscpy(e->name, name);
            wcscpy(e->target, id);
        }
        CoTaskMemFree(id);
        CoTaskMemFree(name);
        if (details)
            IShellItem2_Release(details);
        IShellItem_Release(item);
    }
    IEnumShellItems_Release(all);
}

static SRWLOCK catalog_lock;
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
        head[2] <= catalog_limit) {
        c = calloc(1, sizeof(*c));
        if (c) {
            c->count = (int)head[2];
            bool valid = fread(c->items, sizeof(w_catalog_item), c->count, f) == (size_t)c->count;
            for (int i = 0; valid && i < c->count; i++) {
                w_catalog_item *e = &c->items[i];
                valid =
                    e->packaged <= 1 && *e->name && *e->target && wmemchr(e->name, 0, count_of(e->name)) &&
                    wmemchr(e->target, 0, count_of(e->target)) && wmemchr(e->args, 0, count_of(e->args)) &&
                    wmemchr(e->directory, 0, count_of(e->directory));
            }
            if (!valid) {
                free(c);
                c = NULL;
            }
        }
    }
    fclose(f);
    return c;
}

static void w_publish(w_catalog_state *c) {
    AcquireSRWLockExclusive(&catalog_lock);
    w_catalog_state *old = store;
    store = c;
    ReleaseSRWLockExclusive(&catalog_lock);
    free(old);
    PostMessageW(target_window, ready_message, 0, 0);
}

static void w_save_cache(const wchar_t *path, const w_catalog_state *c) {
    wchar_t tmp[os_path_cap + 32];
    swprintf(tmp, count_of(tmp), L"%ls.%lu.tmp", path, GetCurrentProcessId());
    FILE *f = _wfopen(tmp, L"wb");
    if (!f)
        return;
    uint32_t header[] = {0x4e444131, sizeof(w_catalog_item), (uint32_t)c->count};
    bool ok = fwrite(header, sizeof(header), 1, f) == 1 &&
              fwrite(c->items, sizeof(w_catalog_item), c->count, f) == (size_t)c->count;
    if (fclose(f) != 0 || !ok || !MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING))
        DeleteFileW(tmp);
}

////////////////
//~ Catalog indexing

static DWORD WINAPI w_index_worker(void *unused) {
    (void)unused;
    wchar_t cache[os_path_cap] = {0};
    if (w_cache_path(cache)) {
        w_catalog_state *saved = w_read_cache(cache);
        if (saved)
            w_publish(saved);
    }
    w_catalog_state *c = calloc(1, sizeof(*c));
    if (!c)
        return 0;
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(hr)) {
        IShellLinkW *link = NULL;
        IPersistFile *persist = NULL;
        if (SUCCEEDED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW,
                                       (void **)&link)) &&
            SUCCEEDED(IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&persist))) {
            const KNOWNFOLDERID *folders[] = {&FOLDERID_Programs, &FOLDERID_CommonPrograms};
            for (size_t i = 0; i < count_of(folders); i++) {
                PWSTR path = NULL;
                if (SUCCEEDED(SHGetKnownFolderPath(folders[i], 0, NULL, &path))) {
                    w_walk(c, link, persist, path, 0);
                    CoTaskMemFree(path);
                }
            }
            IPersistFile_Release(persist);
        }
        if (link)
            IShellLinkW_Release(link);
        w_packaged_items(c);
        CoUninitialize();
    }
    if (SUCCEEDED(hr)) {
        if (*cache)
            w_save_cache(cache, c);
        w_publish(c);
    } else
        free(c);
    return 0;
}

void w_catalog_begin(HWND w, UINT msg) {
    if (InterlockedCompareExchange(&started, 1, 0))
        return;
    target_window = w;
    ready_message = msg;
    if (!QueueUserWorkItem(w_index_worker, NULL, WT_EXECUTELONGFUNCTION))
        InterlockedExchange(&started, 0);
}

////////////////
//~ Catalog queries

int w_catalog_count(void) {
    AcquireSRWLockShared(&catalog_lock);
    int n = store ? store->count : 0;
    ReleaseSRWLockShared(&catalog_lock);
    return n;
}

bool w_catalog_label(int i, char *out, int cap) {
    AcquireSRWLockShared(&catalog_lock);
    bool ok = store && (unsigned)i < (unsigned)store->count && w_os_utf8(store->items[i].name, out, cap);
    ReleaseSRWLockShared(&catalog_lock);
    return ok;
}

static DWORD WINAPI w_launch_entry(void *ptr) {
    w_catalog_item *e = ptr;
    if (e->packaged) {
        if (SUCCEEDED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED))) {
            IApplicationActivationManager *am = NULL;
            if (SUCCEEDED(CoCreateInstance(&CLSID_ApplicationActivationManager, NULL, CLSCTX_LOCAL_SERVER,
                                           &IID_IApplicationActivationManager, (void **)&am))) {
                DWORD pid;
                IApplicationActivationManager_ActivateApplication(am, e->target, NULL, AO_NONE, &pid);
                IApplicationActivationManager_Release(am);
            }
            CoUninitialize();
        }
    } else {
        char name[4096];
        wchar_t resolved[os_path_cap], line[os_path_cap + 520];
        if (w_os_utf8(e->target, name, sizeof(name)) && w_os_provider(name, resolved, count_of(resolved)) &&
            swprintf(line, count_of(line), L"\"%ls\" %ls", resolved, e->args) > 0) {
            STARTUPINFOW si = {.cb = sizeof(si)};
            PROCESS_INFORMATION pi = {0};
            if (CreateProcessW(resolved, line, NULL, NULL, FALSE, 0, NULL,
                               *e->directory ? e->directory : NULL, &si, &pi)) {
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
            }
        }
    }
    free(e);
    return 0;
}

bool w_catalog_launch(const char *name) {
    wchar_t wide[256];
    if (!w_os_wide(name, wide, count_of(wide)))
        return false;
    w_catalog_item *entry = NULL;
    AcquireSRWLockShared(&catalog_lock);
    for (int i = 0; store && i < store->count; i++)
        if (!_wcsicmp(store->items[i].name, wide)) {
            entry = malloc(sizeof(*entry));
            if (entry)
                *entry = store->items[i];
            break;
        }
    ReleaseSRWLockShared(&catalog_lock);
    if (!entry)
        return false;
    if (QueueUserWorkItem(w_launch_entry, entry, WT_EXECUTEDEFAULT))
        return true;
    free(entry);
    return false;
}
