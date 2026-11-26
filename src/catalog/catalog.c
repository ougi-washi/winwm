#define COBJMACROS
#include "catalog/catalog.h"
#include "catalog/metadata.h"
#include <shlobj.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <propkey.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

////////////////
//~ Catalog

typedef struct w_catalog_row {
    const wchar_t *name, *target, *label, *keywords; // One allocation, starting at name.
    PIDLIST_ABSOLUTE task;
    uint64_t identity;
    FILETIME modified;
    uint64_t file_size;
    bool packaged;
    unsigned path_order; // Nonzero for command aliases, in PATH and extension order.
    int icon, variant;
} w_catalog_row;

struct w_catalog_state {
    w_catalog_row *rows;
    int count, capacity;
    LONG refs;
    HANDLE images;
    bool failed, persisted;
};

static w_catalog_state *store;
static HWND target_window;
static UINT ready_message;
static SRWLOCK catalog_lock;
static HANDLE changed;
static LONG dirty, stopping;
static ULONGLONG check_after;
enum { index_commands=1, index_shell=2, index_all=3 };

////////////////
//~ Catalog Functions

static w_catalog_state *w_new_catalog(void) {
    w_catalog_state *c = calloc(1, sizeof(*c));
    if (c) c->refs = 1;
    return c;
}

static void w_free_row(w_catalog_row *row) {
    free((void *)row->name);
    CoTaskMemFree(row->task);
}

void w_catalog_release(w_catalog_state *c) {
    if (c && InterlockedDecrement(&c->refs) == 0) {
        for (int i = 0; i < c->count; ++i) w_free_row(&c->rows[i]);
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

static void w_append(w_catalog_state *c, w_catalog_row row) {
    if (c->count == c->capacity) {
        int capacity = c->capacity ? c->capacity * 2 : 64;
        w_catalog_row *next = realloc(c->rows, (size_t)capacity * sizeof(*next));
        if (!next) { c->failed = true; return; }
        c->rows = next;
        c->capacity = capacity;
    }
    if (!row.keywords) row.keywords=L"";
    if (!row.label) row.label=row.name;
    size_t names=wcslen(row.name)+1, targets=wcslen(row.target)+1, labels=wcslen(row.label)+1;
    wchar_t *strings=malloc((names+targets+labels+wcslen(row.keywords)+1)*sizeof(*strings));
    if (!strings) { c->failed = true; return; }
    if (row.task && !(row.task=ILCloneFull(row.task))) { free(strings); c->failed=true; return; }
    row.name=wcscpy(strings,row.name);
    row.target=wcscpy(strings+names,row.target);
    row.label=wcscpy(strings+names+targets,row.label);
    row.keywords=wcscpy(strings+names+targets+labels,row.keywords);
    uint64_t hash = UINT64_C(14695981039346656037) ^ (row.task ? 2 : row.packaged);
    for (size_t i = 0; i < names+targets; ++i)
        hash = (hash ^ (uint16_t)strings[i]) * UINT64_C(1099511628211);
    row.identity=hash ? hash : 1;
    row.variant=0;
    c->rows[c->count++]=row;
}

static void w_add_shell_item(w_catalog_state *c, IShellItem2 *item, w_catalog_row row, LPCWSTR icon_path, UINT flags) {
    SHFILEINFOW info={0};
    HANDLE list=icon_path ? (HANDLE)SHGetFileInfoW(icon_path,0,&info,sizeof(info),
                                    SHGFI_SYSICONINDEX|SHGFI_SMALLICON|flags) : NULL;
    if (list) { c->images=list; row.icon=info.iIcon+1; }
    if (*info.szDisplayName && (flags & SHGFI_DISPLAYNAME)) row.name=info.szDisplayName;
    w_catalog_metadata meta={0};
    if (item) w_catalog_shell_metadata(item,row.packaged ? row.target : NULL,row.task!=NULL,&meta);
    wchar_t label[catalog_label_cap];
    w_catalog_caption(label,count_of(label),row.name,meta.process);
    row.label=label;
    row.keywords=meta.keywords;
    w_append(c,row);
}

static int w_compare_apps(const void *a, const void *b) {
    const w_catalog_row *x = a, *y = b;
    int order = _wcsicmp(x->name, y->name);
    if (!order && !!x->task != !!y->task) return x->task ? 1 : -1;
    if (!order && x->path_order != y->path_order) return x->path_order < y->path_order ? -1 : 1;
    if (!order) order = (int)x->packaged - (int)y->packaged;
    return order ? order : _wcsicmp(x->target, y->target);
}

static int w_compare_captions(const void *a, const void *b) {
    const w_catalog_row *x=a, *y=b;
    int order=_wcsicmp(x->label,y->label);
    return order ? order : w_compare_apps(a,b);
}

static void w_sort_apps(w_catalog_state *c) {
    if (c->count < 2) return;
    // Resolve command shadowing by the original alias before sorting the visible captions.
    qsort(c->rows,c->count,sizeof(*c->rows),w_compare_apps);
    int unique = 1;
    for (int i = 1; i < c->count; ++i) {
        if ((!_wcsicmp(c->rows[unique-1].name,c->rows[i].name) && c->rows[i].path_order) ||
            !w_compare_apps(&c->rows[unique-1], &c->rows[i])) {
            if (!c->rows[unique-1].icon) c->rows[unique-1].icon = c->rows[i].icon;
            w_free_row(&c->rows[i]);
            continue;
        }
        if (unique != i) c->rows[unique] = c->rows[i];
        ++unique;
    }
    c->count = unique;
    qsort(c->rows,c->count,sizeof(*c->rows),w_compare_captions);
    for (int i=1, first=0; i<c->count; ++i) {
        if (_wcsicmp(c->rows[first].label,c->rows[i].label)) first=i;
        else { c->rows[first].variant=1; c->rows[i].variant=i-first+1; }
    }
}

static bool w_walk(w_catalog_state *c, const wchar_t *folder, int depth) {
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
        if (!wcscmp(f.cFileName, L".") || !wcscmp(f.cFileName, L".."))
            continue;
        wchar_t path[os_path_cap];
        if (swprintf(path, count_of(path), L"%ls\\%ls", folder, f.cFileName) < 0)
            continue;
        if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!(f.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && !w_walk(c, path, depth + 1))
                complete = false;
            continue;
        }
        wchar_t *ext = wcsrchr(f.cFileName, L'.');
        if (!ext || (_wcsicmp(ext, L".lnk") && _wcsicmp(ext, L".url")))
            continue;
        // Preserve the shortcut itself. Its destination can be a shell item,
        // a registered document, a packaged app or an executable with arguments.
        *ext = 0;
        IShellItem2 *item=NULL;
        SHCreateItemFromParsingName(path,NULL,&IID_IShellItem2,(void **)&item);
        w_add_shell_item(c,item,(w_catalog_row){.name=f.cFileName,.target=path},path,SHGFI_DISPLAYNAME);
        if (item) IShellItem2_Release(item);
    } while (!c->failed && FindNextFileW(listing, &f));
    if (c->failed || GetLastError() != ERROR_NO_MORE_FILES) complete = false;
    FindClose(listing);
    return complete;
}

static bool w_shell_items(w_catalog_state *c, bool tasks) {
    IShellItem *apps = NULL;
    IEnumShellItems *all = NULL;
    HRESULT opened=tasks ? SHCreateItemFromParsingName(L"shell:::{ED7BA470-8E54-465E-825C-99712043E01C}",
                                   NULL,&IID_IShellItem,(void **)&apps) :
                           SHGetKnownFolderItem(&FOLDERID_AppsFolder,KF_FLAG_DEFAULT,NULL,&IID_IShellItem,(void **)&apps);
    if (SUCCEEDED(opened)) {
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
        if (SUCCEEDED(IShellItem_QueryInterface(item, &IID_IShellItem2, (void **)&details)) &&
            (tasks ? SUCCEEDED(IShellItem_GetDisplayName(item,SIGDN_DESKTOPABSOLUTEPARSING,&id)) :
                     SUCCEEDED(IShellItem2_GetString(details,&PKEY_AppUserModel_ID,&id)) && id && wcschr(id,L'!')) &&
            id && *id &&
            SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_NORMALDISPLAY, &name)) && name &&
            *name && wcslen(id) < os_path_cap && wcslen(name) < MAX_PATH) {
            PIDLIST_ABSOLUTE pidl = NULL;
            SHGetIDListFromObject((IUnknown *)item, &pidl);
            if (!tasks || pidl) w_add_shell_item(c,details,(w_catalog_row){.name=name,.target=id,
                .packaged=!tasks,.task=tasks ? pidl : NULL},(LPCWSTR)pidl,SHGFI_PIDL);
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

enum { cache_v3 = 0x4e444133, cache_limit = 65536 };

static bool w_read_string(FILE *f, wchar_t *out, size_t cap, uint16_t len) {
    if (len>=cap || fread(out,sizeof(*out),len,f)!=len || wmemchr(out,0,len)) return false;
    out[len]=0;
    return true;
}

static w_catalog_state *w_read_cache(const wchar_t *path) {
    FILE *f = _wfopen(path,L"rb");
    if (!f) return NULL;
    uint32_t header[3];
    w_catalog_state *c = NULL;
    if (fread(header,sizeof(header),1,f)==1 && header[0]==cache_v3 && !header[1] && header[2]<=cache_limit) {
        c=w_new_catalog();
        if (c) {
            for (unsigned i=0;i<header[2] && !c->failed;++i) {
                wchar_t name[MAX_PATH], target[os_path_cap];
                wchar_t label[catalog_label_cap], keywords[catalog_keywords_cap];
                uint16_t lengths[5];
                if (fread(lengths,sizeof(lengths),1,f)!=1 || lengths[4]>1 || !lengths[0] || !lengths[1] || !lengths[2] ||
                    !w_read_string(f,name,count_of(name),lengths[0]) || !w_read_string(f,target,count_of(target),lengths[1]) ||
                    !w_read_string(f,label,count_of(label),lengths[2]) || !w_read_string(f,keywords,count_of(keywords),lengths[3])) {
                    c->failed=true;
                    break;
                }
                w_append(c,(w_catalog_row){.name=name,.target=target,.label=label,.keywords=keywords,.packaged=lengths[4]!=0});
            }
            if (c->failed || fgetc(f)!=EOF || ferror(f)) {
                w_catalog_release(c);
                c=NULL;
            } else c->persisted=true;
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
    w_catalog_release(old);
    PostMessageW(target_window, ready_message, 0, 0);
}

static bool w_save_cache(const wchar_t *path, const w_catalog_state *c) {
    if (c->count > cache_limit) return false;
    wchar_t tmp[os_path_cap + 32];
    swprintf(tmp, count_of(tmp), L"%ls.%lu.tmp", path, GetCurrentProcessId());
    FILE *f = _wfopen(tmp, L"wb");
    if (!f) return false;
    uint32_t header[] = {cache_v3, 0, (uint32_t)c->count};
    bool ok = fwrite(header, sizeof(header), 1, f) == 1;
    for (int i = 0; ok && i < c->count; ++i) {
        const w_catalog_row *row = &c->rows[i];
        uint16_t sizes[] = {(uint16_t)wcslen(row->name),(uint16_t)wcslen(row->target),
                            (uint16_t)wcslen(row->label),(uint16_t)wcslen(row->keywords),row->packaged};
        ok = fwrite(sizes,sizeof(sizes),1,f) == 1 &&
             fwrite(row->name,sizeof(wchar_t),sizes[0],f) == sizes[0] &&
             fwrite(row->target,sizeof(wchar_t),sizes[1],f) == sizes[1] &&
             fwrite(row->label,sizeof(wchar_t),sizes[2],f) == sizes[2] &&
             fwrite(row->keywords,sizeof(wchar_t),sizes[3],f) == sizes[3];
    }
    if (fclose(f) != 0 || !ok || !MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp);
        return false;
    }
    return true;
}

////////////////
//~ Catalog indexing

static void w_keep_missing_sources(w_catalog_state *next, const w_catalog_state *current,
                                   bool shortcuts_complete, bool packaged_complete) {
    if (!current) return;
    for (int i = 0; i < current->count && !next->failed; i++) {
        const w_catalog_row *old = &current->rows[i];
        if (old->packaged ? packaged_complete : shortcuts_complete) continue;
        w_append(next,*old);
    }
    if (!next->images) next->images = current->images;
}

typedef struct w_catalog_watch {
    struct w_catalog_watch *next;
    OVERLAPPED request;
    DWORD data[1024];
    HANDLE dir;
    int source;
    bool recursive, seen;
    wchar_t *child;
    wchar_t path[];
} w_catalog_watch;

typedef struct {
    HANDLE port;
    w_catalog_watch *watches;
    unsigned outstanding;
    int retry;
    w_catalog_state *shell, *tasks, *commands;
    wchar_t cache[os_path_cap];
    wchar_t extensions[32768];
} w_index;

static bool w_watch_read(w_catalog_watch *watch) {
    ZeroMemory(&watch->request,sizeof(watch->request));
    return ReadDirectoryChangesW(watch->dir,watch->data,sizeof(watch->data),watch->recursive,
        (*watch->child ? FILE_NOTIFY_CHANGE_DIR_NAME : FILE_NOTIFY_CHANGE_FILE_NAME|FILE_NOTIFY_CHANGE_LAST_WRITE) |
        (watch->recursive ? FILE_NOTIFY_CHANGE_DIR_NAME : 0),NULL,&watch->request,NULL) != FALSE;
}

// The child name restricts a parent subscription to creation, deletion and rename of
// one PATH directory. Windows keeps an open directory handle valid across a rename.
static DWORD w_arm_folder(w_index *index,const wchar_t *path,bool recursive,int source,const wchar_t *child) {
    for (w_catalog_watch *w=index->watches;w;w=w->next) {
        if (w->source==source && w->recursive==recursive && !_wcsicmp(w->path,path) &&
            !_wcsicmp(w->child,child)) {
            w->seen=true;
            return ERROR_SUCCESS;
        }
    }
    HANDLE dir=CreateFileW(path,FILE_LIST_DIRECTORY,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
            NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OVERLAPPED,NULL);
    if (dir==INVALID_HANDLE_VALUE) return GetLastError();
    size_t chars=wcslen(path)+1;
    w_catalog_watch *watch=calloc(1,sizeof(*watch)+(chars+wcslen(child)+1)*sizeof(wchar_t));
    DWORD err=ERROR_NOT_ENOUGH_MEMORY;
    if (watch) {
        watch->source=source;
        watch->recursive=recursive;
        watch->child=watch->path+chars;
        watch->seen=true;
        watch->dir=dir;
        wcscpy(watch->path,path);
        wcscpy(watch->child,child);
        if (CreateIoCompletionPort(dir,index->port,(ULONG_PTR)watch,0) && w_watch_read(watch)) {
            watch->next=index->watches;
            index->watches=watch;
            ++index->outstanding;
            return ERROR_SUCCESS;
        }
        err=GetLastError();
        free(watch);
    }
    CloseHandle(dir);
    return err;
}

static void w_watch_folder(w_index *index,const wchar_t *root,bool recursive,int source) {
    wchar_t path[os_path_cap], child[MAX_PATH];
    DWORD err=w_arm_folder(index,root,recursive,source,L"");
    if (err && err!=ERROR_PATH_NOT_FOUND && err!=ERROR_FILE_NOT_FOUND) index->retry |= source;
    if (wcslen(root)>=count_of(path)) return;
    wcscpy(path,root);
    for (;;) {
        wchar_t *slash=wcsrchr(path,L'\\');
        if (!slash || !slash[1] || wcslen(slash+1)>=count_of(child)) return;
        // UNC shares and drive roots have no parent directory to subscribe to.
        if (path[0]==L'\\' && path[1]==L'\\' && wcschr(path+2,L'\\')==slash) {
            if (err) index->retry |= source;
            return;
        }
        wcscpy(child,slash+1);
        if (slash==path+2 && path[1]==L':') slash[1]=0;
        else *slash=0;
        err=w_arm_folder(index,path,false,source,child);
        if (!err) return;
        if (err!=ERROR_FILE_NOT_FOUND && err!=ERROR_PATH_NOT_FOUND) { index->retry |= source; return; }
    }
}
// One outstanding read owns each removed watch until its completion reaches the port.
static void w_retire_watch(w_catalog_watch *w) {
    CloseHandle(w->dir);
    w->dir=INVALID_HANDLE_VALUE;
}

static void w_sweep_watches(w_index *index, int source) {
    w_catalog_watch **link=&index->watches;
    while (*link) {
        w_catalog_watch *w=*link;
        if ((w->source & source) && !w->seen) {
            *link=w->next;
            w_retire_watch(w);
        } else link=&w->next;
    }
}

static void w_copy_rows(w_catalog_state *next,const w_catalog_state *source) {
    if (!source) return;
    for (int i=0;i<source->count && !next->failed;++i) {
        w_append(next,source->rows[i]);
    }
    if (!next->images) next->images=source->images;
}

static bool w_same_rows(const w_catalog_state *a,const w_catalog_state *b,bool icons) {
    if (!a || a->count!=b->count || (icons && a->images!=b->images)) return false;
    for (int i=0;i<a->count;++i) {
        const w_catalog_row *old=&a->rows[i], *row=&b->rows[i];
        if (old->identity!=row->identity || (icons && old->icon!=row->icon) ||
            wcscmp(old->target,row->target) || wcscmp(old->name,row->name) ||
            wcscmp(old->label,row->label) || wcscmp(old->keywords,row->keywords)) return false;
    }
    return true;
}

static void w_update_menu(w_index *index) {
    w_catalog_state *next=w_new_catalog();
    if (!next) return;
    w_copy_rows(next,index->shell);
    w_copy_rows(next,index->commands);
    w_copy_rows(next,index->tasks);
    if (!next->failed) {
        w_sort_apps(next);
        w_catalog_state *old=w_catalog_acquire();
        bool same=w_same_rows(old,next,true);
        w_catalog_release(old);
        if (!same) { w_publish(next); return; }
    }
    w_catalog_release(next);
}

static void w_scan_shell(w_index *index) {
    w_catalog_state *next=w_new_catalog();
    if (!next) return;
    const KNOWNFOLDERID *folders[]={&FOLDERID_Programs,&FOLDERID_CommonPrograms};
    bool shortcuts=true;
    for (size_t i=0;i<count_of(folders);++i) {
        PWSTR path=NULL;
        if (SUCCEEDED(SHGetKnownFolderPath(folders[i],KF_FLAG_DONT_VERIFY,NULL,&path))) {
            w_watch_folder(index,path,true,index_shell);
            if (!w_walk(next,path,0)) shortcuts=false;
            CoTaskMemFree(path);
        } else shortcuts=false;
    }
    bool packaged=w_shell_items(next,false);
    if (!shortcuts || !packaged) index->retry |= index_shell;
    w_keep_missing_sources(next,index->shell,shortcuts,packaged);
    if (next->failed) { w_catalog_release(next); return; }
    w_sort_apps(next);
    next->persisted=index->shell && index->shell->persisted && w_same_rows(index->shell,next,false);
    if (*index->cache && shortcuts && packaged && !next->persisted)
        next->persisted=w_save_cache(index->cache,next);
    w_catalog_release(index->shell);
    index->shell=next;

    // Task parsing names identify rows, but some cannot be parsed back into actionable
    // Shell items. Keep the original PIDLs in the live snapshot, out of the disk cache.
    next=w_new_catalog();
    if (!next) return;
    if (w_shell_items(next,true) && !next->failed) {
        w_catalog_release(index->tasks);
        index->tasks=next;
    } else {
        w_catalog_release(next);
        index->retry |= index_shell;
    }
}

static int w_compare_paths(const void *left, const void *right) {
    return _wcsicmp(((const w_catalog_row *)left)->target,((const w_catalog_row *)right)->target);
}

static void w_scan_directory(w_index *index,w_catalog_state *next,const wchar_t *dir,
                             unsigned order,const wchar_t *extensions) {
    w_watch_folder(index,dir,false,index_commands);
    wchar_t path[os_path_cap];
    size_t prefix=wcslen(dir);
    if (prefix+2>=count_of(path)) return;
    wcscpy(path,dir);
    if (path[prefix-1]!=L'\\') path[prefix++]=L'\\';
    wcscpy(path+prefix,L"*");
    WIN32_FIND_DATAW file;
    HANDLE list=FindFirstFileExW(path,FindExInfoBasic,&file,FindExSearchNameMatch,NULL,FIND_FIRST_EX_LARGE_FETCH);
    DWORD err=ERROR_SUCCESS;
    if (list!=INVALID_HANDLE_VALUE) {
        do {
            const wchar_t *suffix=wcsrchr(file.cFileName,L'.');
            int ext=suffix ? w_os_extension_order(suffix,extensions) : 0;
            if (!ext || suffix==file.cFileName || (file.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                prefix+wcslen(file.cFileName)>=count_of(path)) continue;
            wcscpy(path+prefix,file.cFileName);
            wchar_t description[catalog_label_cap]=L"", label[catalog_label_cap];
            w_catalog_row row={.target=path,.path_order=order+(unsigned)ext,.modified=file.ftLastWriteTime,
                               .file_size=((uint64_t)file.nFileSizeHigh<<32)|file.nFileSizeLow};
            const w_catalog_row *old=index->commands && index->commands->count ?
                bsearch(&row,index->commands->rows,index->commands->count,sizeof(row),w_compare_paths) : NULL;
            if (old && !CompareFileTime(&old->modified,&row.modified) && old->file_size==row.file_size) {
                row.label=old->label;
            } else if (!(file.dwFileAttributes & (FILE_ATTRIBUTE_OFFLINE|FILE_ATTRIBUTE_REPARSE_POINT))) {
                w_catalog_file_description(path,description,count_of(description));
                if (*description) {
                    w_catalog_caption(label,count_of(label),description,file.cFileName);
                    row.label=label;
                }
            }
            file.cFileName[suffix-file.cFileName]=0;
            row.name=file.cFileName;
            w_append(next,row);
        } while (!next->failed && FindNextFileW(list,&file));
        err=GetLastError();
        FindClose(list);
    } else err=GetLastError();
    if (err==ERROR_NO_MORE_FILES || err==ERROR_FILE_NOT_FOUND || err==ERROR_PATH_NOT_FOUND) return;
    index->retry |= index_commands;
    // Keep entries from an inaccessible share while it remains in PATH.
    if (index->commands) for (int i=0;i<index->commands->count && !next->failed;++i) {
        const w_catalog_row *r=&index->commands->rows[i];
        if (_wcsnicmp(r->target,path,prefix) || wcschr(r->target+prefix,L'\\')) continue;
        const wchar_t *suffix=wcsrchr(r->target,L'.');
        int ext=suffix ? w_os_extension_order(suffix,extensions) : 0;
        if (!ext) continue;
        w_catalog_row row=*r;
        row.path_order=order+(unsigned)ext;
        w_append(next,row);
    }
}

static void w_scan_commands(w_index *index) {
    w_catalog_state *next=w_new_catalog();
    if (!next) return;
    wchar_t choco[os_path_cap], fallback[os_path_cap], full[os_path_cap];
    w_os_path_extensions(index->extensions,count_of(index->extensions));
    DWORD n=GetEnvironmentVariableW(L"ChocolateyInstall",choco,count_of(choco));
    if (n && n<count_of(choco)) n=swprintf(fallback,count_of(fallback),L"%ls\\bin",choco);
    else {
        n=GetEnvironmentVariableW(L"ProgramData",choco,count_of(choco));
        if (n && n<count_of(choco)) n=swprintf(fallback,count_of(fallback),L"%ls\\chocolatey\\bin",choco);
        else n=0;
    }
    wchar_t *path=w_os_path_directories(), *cursor=path, **seen=NULL;
    unsigned dirs=0;
    const wchar_t *extra=n>0 && n<count_of(fallback) ? fallback : NULL;
    while (!next->failed) {
        const wchar_t *dir=w_os_next_directory(&cursor);
        if (!dir) { dir=extra; extra=NULL; }
        if (!dir) break;
        DWORD len=GetFullPathNameW(dir,count_of(full),full,NULL);
        if (!len || len>=count_of(full)) continue;
        while (len>3 && full[len-1]==L'\\') full[--len]=0;
        unsigned i=0;
        while (i<dirs && _wcsicmp(seen[i],full)) ++i;
        if (i<dirs) continue;
        wchar_t **new_dirs=realloc(seen,(dirs+1)*sizeof(*seen));
        if (!new_dirs) { next->failed=true; break; }
        seen=new_dirs;
        seen[dirs]=_wcsdup(full);
        if (!seen[dirs]) { next->failed=true; break; }
        w_scan_directory(index,next,full,dirs++ << 16,index->extensions);
    }
    free(path);
    for (unsigned i=0;i<dirs;++i) free(seen[i]);
    free(seen);
    if (next->failed) { w_catalog_release(next); return; }
    if (next->count>1) qsort(next->rows,next->count,sizeof(*next->rows),w_compare_paths);
    w_catalog_release(index->commands);
    index->commands=next;
}

static void w_index_signal(int sources) {
    AcquireSRWLockShared(&catalog_lock);
    if (changed && !InterlockedCompareExchange(&stopping,0,0) && !InterlockedOr(&dirty,sources))
        PostQueuedCompletionStatus(changed,0,0,NULL);
    ReleaseSRWLockShared(&catalog_lock);
}

void w_catalog_invalidate(void) {
    w_index_signal(index_all);
}

bool w_catalog_shell_event(WPARAM notification, LPARAM process) {
    PIDLIST_ABSOLUTE *items;
    LONG event;
    HANDLE lock=SHChangeNotification_Lock((HANDLE)notification,(DWORD)process,&items,&event);
    if (!lock) return false;
    SHChangeNotification_Unlock(lock);
    w_index_signal(index_shell);
    return true;
}

static ULONG w_shell_watch(void) {
    PIDLIST_ABSOLUTE apps=NULL;
    ULONG id=0;
    if (SUCCEEDED(SHGetKnownFolderIDList(&FOLDERID_AppsFolder,KF_FLAG_DEFAULT,NULL,&apps))) {
        SHChangeNotifyEntry entry={apps,TRUE};
        LONG events=SHCNE_CREATE|SHCNE_DELETE|SHCNE_RENAMEITEM|SHCNE_MKDIR|SHCNE_RMDIR|
                    SHCNE_RENAMEFOLDER|SHCNE_UPDATEDIR|SHCNE_UPDATEITEM|SHCNE_ASSOCCHANGED;
        id=SHChangeNotifyRegister(target_window,SHCNRF_ShellLevel|SHCNRF_NewDelivery,events,ready_message,1,&entry);
        CoTaskMemFree(apps);
    }
    return id;
}

static bool w_command_change(const w_catalog_watch *w,DWORD bytes,const wchar_t *extensions) {
    // A zero-byte result reports overflow. A rescan recovers every missed change.
    if (!bytes || (!*w->child && w->source!=index_commands)) return true;
    const BYTE *data=(const BYTE *)w->data;
    for (size_t at=0;at<bytes;) {
        const FILE_NOTIFY_INFORMATION *info=(const FILE_NOTIFY_INFORMATION *)(data+at);
        size_t n=info->FileNameLength/sizeof(wchar_t);
        if (*w->child) {
            if (n==wcslen(w->child) && !_wcsnicmp(w->child,info->FileName,n)) return true;
            if (!info->NextEntryOffset) break;
            at+=info->NextEntryOffset;
            continue;
        }
        while (n>0 && info->FileName[n-1]!=L'.') --n;
        if (n) {
            wchar_t suffix[MAX_PATH];
            size_t len=info->FileNameLength/sizeof(wchar_t)-n+1;
            if (len<count_of(suffix)) {
                memcpy(suffix,info->FileName+n-1,len*sizeof(wchar_t));
                suffix[len]=0;
                if (w_os_extension_order(suffix,extensions)) return true;
            }
        }
        if (!info->NextEntryOffset) break;
        at+=info->NextEntryOffset;
    }
    return false;
}

static int w_receive_change(w_index *index,DWORD timeout,bool *received) {
    DWORD bytes=0;
    ULONG_PTR key=0;
    OVERLAPPED *request=NULL;
    BOOL ok=GetQueuedCompletionStatus(index->port,&bytes,&key,&request,timeout);
    *received=ok || request!=NULL;
    if (!key) return InterlockedExchange(&dirty,0);
    w_catalog_watch *w=(w_catalog_watch *)key;
    if (w->dir==INVALID_HANDLE_VALUE) {
        free(w);
        --index->outstanding;
        return 0;
    }
    int source=!ok || w_command_change(w,bytes,index->extensions) ? w->source : 0;
    if (source && *w->child) {
        // Reopen the directory by path. An old handle can still follow its renamed inode.
        wchar_t child[os_path_cap];
        size_t n=wcslen(w->path);
        int len=swprintf(child,count_of(child),L"%ls%ls%ls",w->path,
                         n && w->path[n-1]==L'\\' ? L"" : L"\\",w->child);
        w_catalog_watch **link=&index->watches;
        while (len>0 && *link) {
            w_catalog_watch *other=*link;
            if (other!=w && other->source==source && !_wcsnicmp(other->path,child,len) &&
                (!other->path[len] || other->path[len]==L'\\')) {
                *link=other->next;
                w_retire_watch(other);
            } else link=&other->next;
        }
    }
    if (!ok || !w_watch_read(w)) {
        DWORD err=GetLastError();
        source=w->source;
        index->retry |= source;
        w_catalog_watch **link=&index->watches;
        while (*link!=w) link=&(*link)->next;
        *link=w->next;
        CloseHandle(w->dir);
        free(w);
        --index->outstanding;
        if (err==ERROR_INVALID_FUNCTION || err==ERROR_NOT_SUPPORTED) return 0;
    }
    return source;
}

static DWORD WINAPI w_index_worker(void *unused) {
    (void)unused;
    w_index index={.port=changed};
    HRESULT apartment=CoInitializeEx(NULL,COINIT_APARTMENTTHREADED);
    ULONG shell_watch=0;
    if (w_os_data_path(index.cache,count_of(index.cache),L"apps.v3.bin",true)) {
        index.shell=w_read_cache(index.cache);
        if (index.shell) w_update_menu(&index);
    }
    int sources=index_all;
    ULONGLONG reconcile=GetTickCount64()+60000;
    while (!InterlockedCompareExchange(&stopping,0,0)) {
        index.retry &= ~sources;
        for (w_catalog_watch *w=index.watches;w;w=w->next) {
            if (w->source & sources) w->seen=false;
        }
        if (sources & index_commands) {
            w_scan_commands(&index);
            w_update_menu(&index);
        }
        if ((sources & index_shell) && SUCCEEDED(apartment)) {
            if (!shell_watch) shell_watch=w_shell_watch();
            if (!shell_watch) index.retry |= index_shell;
            w_scan_shell(&index);
            w_update_menu(&index);
        }
        w_sweep_watches(&index,sources);
        if (InterlockedCompareExchange(&stopping,0,0)) break;
        ULONGLONG now=GetTickCount64();
        bool received=false;
        DWORD timeout=index.retry ? (now<reconcile ? (DWORD)(reconcile-now) : 0) : INFINITE;
        sources=w_receive_change(&index,timeout,&received);
        // Collect bursty installer changes. Never extend the deadline with each arrival.
        ULONGLONG deadline=GetTickCount64()+50;
        while (received && !InterlockedCompareExchange(&stopping,0,0) && (now=GetTickCount64())<deadline)
            sources |= w_receive_change(&index,(DWORD)(deadline-now),&received);
        now=GetTickCount64();
        if (now>=reconcile) {
            // Some file systems and virtual providers do not emit notifications.
            sources |= index.retry;
            reconcile=now+60000;
        }
    }
    if (shell_watch) SHChangeNotifyDeregister(shell_watch);
    for (w_catalog_watch *w=index.watches;w;w=w->next) w->seen=false;
    w_sweep_watches(&index,index_all);
    while (index.outstanding) {
        bool received;
        w_receive_change(&index,INFINITE,&received);
    }
    w_catalog_release(index.shell);
    w_catalog_release(index.tasks);
    w_catalog_release(index.commands);
    if (SUCCEEDED(apartment)) CoUninitialize();
    AcquireSRWLockExclusive(&catalog_lock);
    CloseHandle(changed);
    changed=NULL;
    w_catalog_state *old=store;
    store=NULL;
    ReleaseSRWLockExclusive(&catalog_lock);
    w_catalog_release(old);
    return 0;
}

void w_catalog_begin(HWND w, UINT msg) {
    AcquireSRWLockExclusive(&catalog_lock);
    if (!changed) {
        changed=CreateIoCompletionPort(INVALID_HANDLE_VALUE,NULL,0,1);
        if (changed) {
            target_window=w;
            ready_message=msg;
            check_after=GetTickCount64()+60000;
            InterlockedExchange(&stopping,0);
            InterlockedExchange(&dirty,0);
            HANDLE worker=CreateThread(NULL,0,w_index_worker,NULL,0,NULL);
            if (worker) CloseHandle(worker);
            else { CloseHandle(changed); changed=NULL; }
        }
    } else if (!stopping && GetTickCount64()>=check_after) {
        check_after=GetTickCount64()+60000;
        if (!InterlockedOr(&dirty,index_all)) PostQueuedCompletionStatus(changed,0,0,NULL);
    }
    ReleaseSRWLockExclusive(&catalog_lock);
}

void w_catalog_end(void) {
    AcquireSRWLockExclusive(&catalog_lock);
    if (changed && !InterlockedExchange(&stopping,1)) PostQueuedCompletionStatus(changed,0,0,NULL);
    ReleaseSRWLockExclusive(&catalog_lock);
}
////////////////
//~ Catalog queries

int w_catalog_count(const w_catalog_state *c) {
    return c ? c->count : 0;
}

uint64_t w_catalog_identity(const w_catalog_state *c, int i) {
    return c->rows[i].identity;
}

HANDLE w_catalog_images(const w_catalog_state *c) {
    return c ? c->images : NULL;
}

w_catalog_info w_catalog_label(const w_catalog_state *c, int i, wchar_t *out, int cap) {
    const w_catalog_row *r = &c->rows[i];
    if (!r->variant) lstrcpynW(out, r->label, cap);
    else {
        const wchar_t *file = wcsrchr(r->target, L'\\');
        swprintf(out, cap, L"%.150ls  ·  %.80ls (%d)", r->label, file ? file+1 : r->target, r->variant);
    }
    return (w_catalog_info){r->name,*r->keywords ? r->keywords : NULL,r->icon,r->task!=NULL};
}

typedef struct w_catalog_launch_job {
    bool admin, packaged;
    PIDLIST_ABSOLUTE task;
    wchar_t target[];
} w_catalog_launch_job;

static DWORD WINAPI w_launch_entry(void *ptr) {
    w_catalog_launch_job *job = ptr;
    bool launched = false;
    DWORD error = ERROR_SUCCESS;
    if (job->task || (job->admin && job->packaged)) {
        HRESULT apartment = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        wchar_t name[os_path_cap + 32];
        PIDLIST_ABSOLUTE item = job->task;
        if (!item) {
            swprintf(name, count_of(name), L"shell:AppsFolder\\%ls", job->target);
            SHParseDisplayName(name, NULL, &item, 0, NULL);
        }
        if (item) {
            SHELLEXECUTEINFOW info = {.cbSize=sizeof(info), .lpIDList=item, .lpVerb=job->admin ? L"runas" : NULL,
                .fMask=SEE_MASK_INVOKEIDLIST | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI, .nShow=SW_SHOWNORMAL};
            launched = ShellExecuteExW(&info) != FALSE;
            if (!launched) error = GetLastError();
            CoTaskMemFree(item);
        }
        if (SUCCEEDED(apartment)) CoUninitialize();
    } else if (job->packaged) {
        if (SUCCEEDED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED))) {
            IApplicationActivationManager *am = NULL;
            if (SUCCEEDED(CoCreateInstance(&CLSID_ApplicationActivationManager, NULL, CLSCTX_LOCAL_SERVER,
                                           &IID_IApplicationActivationManager, (void **)&am))) {
                DWORD pid;
                CoAllowSetForegroundWindow((IUnknown *)am, NULL);
                launched = SUCCEEDED(IApplicationActivationManager_ActivateApplication(am, job->target, NULL, AO_NONE, &pid));
                IApplicationActivationManager_Release(am);
            }
            CoUninitialize();
        }
    } else {
        launched = w_os_shell_execute(job->target, NULL, NULL, job->admin);
        if (!launched) error = GetLastError();
    }
    if (!launched && error != ERROR_CANCELLED) PostMessageW(target_window, ready_message, job->admin ? 2 : 1, 0);
    free(job);
    return 0;
}

bool w_catalog_launch_item(const w_catalog_state *c, int index, bool admin) {
    if (!c || (unsigned)index >= (unsigned)c->count) return false;
    const w_catalog_row *row = &c->rows[index];
    size_t size = (wcslen(row->target)+1)*sizeof(wchar_t);
    w_catalog_launch_job *entry = malloc(sizeof(*entry)+size);
    if (!entry)
        return false;
    entry->packaged = row->packaged;
    entry->task = row->task ? ILCloneFull(row->task) : NULL;
    if (row->task && !entry->task) { free(entry); return false; }
    memcpy(entry->target,row->target,size);
    entry->admin = admin;
    if (QueueUserWorkItem(w_launch_entry, entry, WT_EXECUTEDEFAULT))
        return true;
    CoTaskMemFree(entry->task);
    free(entry);
    return false;
}

bool w_catalog_launch(const char *name, bool admin) {
    wchar_t wide[256];
    if (!w_os_wide(name, wide, count_of(wide))) return false;
    w_catalog_state *c = w_catalog_acquire();
    bool ok = false;
    int best=-1;
    for (int i=0; i<w_catalog_count(c); ++i) {
        if (!_wcsicmp(c->rows[i].name,wide) && (best<0 || w_compare_apps(&c->rows[i],&c->rows[best])<0)) best=i;
    }
    if (best>=0) ok=w_catalog_launch_item(c,best,admin);
    w_catalog_release(c);
    return ok;
}
