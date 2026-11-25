#include "theme/dialog.h"
#include "desktop_tools/tools.h"
#include "desktop_tools/rows.h"
#include "theme/theme.h"
#include "theme/icon_button.h"
#include "theme/disclosure.h"
#include "win_platform/appearance.h"
#include <commctrl.h>
#include <richedit.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wchar.h>
#include <wctype.h>

typedef enum w_package_action { pkg_search, pkg_updates, pkg_show, pkg_install, pkg_upgrade } w_package_action;
enum { page_winget, page_updates, page_choco };
enum { poll_timer=1, catalog_timer, output_timer, resize_timer, debounce_ms=350, updates_ttl=60000, search_ttl=300000, retry_ms=30000 };
typedef struct { int page; wchar_t text[512]; } w_catalog_key;
typedef struct {
    w_catalog_key key;
    wchar_t *text;
    DWORD code;
    ULONGLONG expires,accessed;
} w_snapshot;
typedef struct { wchar_t id[256],version[128]; } w_package;
typedef struct {
    w_tool_rows rows;
    int selection,anchor;
    w_package prior,first_visible;
} w_render;

static HWND window, pages, query, results, status, output, apply;
static w_tool_feedback package_feedback;
static HFONT font, output_font;
static HMODULE rich_edit;
static HANDLE process, stream;
static w_package_action action;
static bool updates, chocolatey, closing, truncated, reviewed, source_terms, accept_once;
static char bytes[256 * 1024];
static size_t used;
static wchar_t selected[256];
static wchar_t selected_version[128];
static wchar_t last_value[512];
static ULONGLONG next_progress;
static wchar_t *progress_frame;
static size_t progress_mark;
static w_catalog_key wanted;
static w_snapshot snapshots[6], *shown;
static w_package *items;
static size_t capacity;
static ULONGLONG catalog_due;
static w_disclosure transcript;
static bool show_results,show_review,show_transcript;
static w_disclosure dock_resize;
static bool docked;
static bool log_scrollbar;
static int dock_rows, dock_lines, output_line_height, resize_from, resize_to;
static void w_layout_body(void);
static const wchar_t *const sources[]={L"WinGet",L"WinGet updates",L"Chocolatey"};
static int source_ink;
static void w_update_dock(void);
static bool focus_results, detail_open, confirming, composing, suspended, refresh_failed, receipt;

static bool w_mutating(void) { return action == pkg_install || action == pkg_upgrade; }
static bool w_catalog(w_package_action op) { return op==pkg_search || op==pkg_updates; }
static bool w_same_key(const w_catalog_key *a,const w_catalog_key *b) {
    return a->page==b->page && !wcscmp(a->text,b->text);
}
static bool w_current(void) { return shown && w_same_key(&shown->key,&wanted); }
static void w_cancel_read(void);

static void w_schedule(UINT delay) {
    KillTimer(window,catalog_timer);
    catalog_due=0;
    if (delay && !closing && !composing && !confirming && !detail_open && !source_terms &&
        IsWindowVisible(window) && !IsIconic(window) && (updates || *wanted.text)) {
        catalog_due=GetTickCount64()+delay;
        SetTimer(window,catalog_timer,delay,NULL);
    }
}

static void w_forget_snapshots(void) {
    shown=NULL;
    for (size_t i=0;i<count_of(snapshots);++i) {
        free(snapshots[i].text);
        snapshots[i]=(w_snapshot){0};
    }
}

static bool w_package_id(const wchar_t *id) {
    size_t n = wcslen(id);
    if (!n || n > 128 || wcspbrk(id,L"\\/:*?\"<>|…")) return false;
    int parts = 1, length = 0;
    for (const wchar_t *s = id; *s; ++s) {
        if (*s == L'.') {
            if (!length || ++parts > 8) return false;
            length = 0;
        } else if (*s < 0x20 || iswspace(*s) || ++length > 32) return false;
    }
    return parts >= 2 && length > 0;
}

static bool w_choco_token(const wchar_t *s, bool version) {
    const wchar_t *alnum = L"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    if (!*s || !wcschr(alnum,*s) || wcslen(s) >= (version ? count_of(selected_version) : count_of(selected)))
        return false;
    for (const wchar_t *p = s+1; *p; ++p)
        if (!wcschr(alnum,*p) && !wcschr(version ? L".-+" : L".-_",*p)) return false;
    // Chocolatey interprets a .config argument as a local package manifest.
    const wchar_t *ext = wcsrchr(s,L'.');
    return version || !ext || (_wcsicmp(ext,L".config") && _wcsicmp(ext,L".nupkg"));
}

static bool w_choco_args(w_package_action op, const wchar_t *value, wchar_t *line, size_t cap) {
    const wchar_t *source = L"--source=https://community.chocolatey.org/api/v2/";
    if (op == pkg_search) {
        if (!*value || *value == L'-' || *value == L'/' || iswspace(*value) || wcspbrk(value,L"\"\\\r\n\t")) return false;
        return swprintf(line,cap,L"choco.exe search \"%ls\" --limit-output --page=0 --page-size=75 %ls",value,source) > 0;
    }
    if (!w_choco_token(value,false) || !w_choco_token(selected_version,true)) return false;
    if (op != pkg_show && op != pkg_install) return false;
    return swprintf(line,cap,L"choco.exe %ls %ls --version=%ls %ls %ls",
                    op == pkg_show ? L"info" : L"install",value,selected_version,source,
                    op == pkg_install ? L"--yes --no-progress" : L"--no-color") > 0;
}

/* Quote one argument using CommandLineToArgvW rules. The executable never goes through a shell. */
static bool w_args(w_package_action op, const wchar_t *value, wchar_t *line, size_t cap) {
    if (chocolatey) return w_choco_args(op,value,line,cap);
    const wchar_t *verb = op == pkg_search ? L"search --count 75 --query=" :
                          op == pkg_updates ? L"list --upgrade-available " :
                          op == pkg_show ? L"show --exact --id=" :
                          op == pkg_install ? L"install --exact --id=" : L"upgrade --exact --id=";
    if (op != pkg_updates && op != pkg_search && !w_package_id(value)) return false;
    const wchar_t *flags = op == pkg_install ? L" --source winget --disable-interactivity --no-upgrade --accept-package-agreements" :
                           op == pkg_upgrade ? L" --source winget --disable-interactivity --accept-package-agreements" :
                           L" --source winget --disable-interactivity";
    if (wcslen(verb) + 2*wcslen(value) + wcslen(flags) + 32 >= cap) return false;
    wcscpy(line,L"winget.exe ");
    wcscat(line,verb);
    wchar_t *p = line + wcslen(line);
    if (op != pkg_updates) {
        *p++ = L'"';
        size_t slashes = 0;
        for (const wchar_t *s = value;; ++s) {
            if (*s == L'\\') { ++slashes; continue; }
            if (*s == L'"' || !*s) slashes *= 2;
            while (slashes) { *p++ = L'\\'; --slashes; }
            if (!*s) break;
            if (*s == L'"') *p++ = L'\\';
            *p++ = *s;
        }
        *p++ = L'"';
    }
    wcscpy(p,flags);
    return true;
}

static void w_controls(void) {
    bool locked=process && w_mutating();
    bool reading_details=process && !w_catalog(action);
    int count=(int)SendMessageW(results,LB_GETCOUNT,0,0);
    bool matches=count>0;
    EnableWindow(pages,!locked);
    EnableWindow(query,!locked && !updates);
    if (!matches && GetFocus()==results) SetFocus(locked ? output : updates ? pages : query);
    EnableWindow(results,!locked && matches && w_current());
    bool can_review=!reading_details && ((w_current() && selected[0]) || source_terms);
    if (GetFocus()==apply && !can_review) {
        HWND next=IsWindowEnabled(results) && SendMessageW(results,LB_GETCOUNT,0,0)>0 ? results :
                  locked ? output : updates ? pages : query;
        SetFocus(next);
    }
    EnableWindow(apply,can_review);
    bool review=source_terms || (w_current() && selected[0]) || reading_details;
    bool log=detail_open || locked || (output && GetWindowTextLengthW(output)>0);
    bool layout=show_results!=matches || show_review!=review || show_transcript!=log;
    if (layout) {
        show_results=matches;
        show_review=review;
        show_transcript=log;
        if (!log && GetFocus()==output) SetFocus(IsWindowEnabled(results) ? results : updates ? pages : query);
        if (!docked) w_disclosure_to(window,&transcript,output_timer,log);
    }
    if (docked) {
        // A narrower match set keeps the browsing viewport while typing. An empty result starts fresh.
        int rows=matches ? max(dock_rows,min(6,count)) : 0;
        int lines=log ? (reading_details ? 8 : min(10,max(1,(int)SendMessageW(output,EM_GETLINECOUNT,0,0)))) : 0;
        if (rows!=dock_rows || lines!=dock_lines) layout=true;
        dock_rows=rows;
        dock_lines=lines;
        if (layout) w_update_dock();
    }
    if (layout) w_layout_body();
    w_icon_button(apply,source_terms ? L"\uE8FB" : reviewed ? L"\uE896" : L"\uE890",
                  source_terms ? L"Review source terms…" : reviewed ? (updates ? L"Update selected package…" : L"Install selected package…") :
                  L"Review selected package");
}

static void w_clear_rows(void) {
    if (SendMessageW(results,LB_GETCOUNT,0,0)>0) SendMessageW(results,LB_RESETCONTENT,0,0);
    selected[0] = 0;
    selected_version[0] = 0;
    reviewed = false;
}

static void w_select_package(void) {
    LRESULT row = SendMessageW(results,LB_GETCURSEL,0,0);
    if (row<0 || (size_t)row>=capacity) {
        selected[0]=selected_version[0]=0;
        reviewed=detail_open=false;
        w_controls();
        return;
    }
    if (detail_open && !wcscmp(selected,items[row].id) && !wcscmp(selected_version,items[row].version)) return;
    if (process && action==pkg_show) w_cancel_read();
    receipt=false;
    wcscpy(selected,items[row].id);
    wcscpy(selected_version,items[row].version);
    reviewed = false;
    detail_open = false;
    source_terms = false;
    if (selected[0]) {
        wchar_t label[420];
        if (chocolatey) swprintf(label,count_of(label),L"%ls %ls   ·   Chocolatey",selected,selected_version);
        else swprintf(label,count_of(label),L"%ls   ·   WinGet",selected);
        w_tool_status(status,label);
        w_tool_status(output,L"");
    }
    if (!catalog_due && !process) w_schedule(updates ? updates_ttl : search_ttl);
    w_controls();
}

static bool w_put(w_render *view,const wchar_t *id,const wchar_t *version,const wchar_t *label) {
    size_t i=(size_t)view->rows.written;
    if (i==capacity) {
        size_t next=capacity ? 2*capacity : 32;
        w_package *more=realloc(items,next*sizeof(*items));
        if (!more) return false;
        items=more;capacity=next;
    }
    if (w_rows_put(&view->rows,label,0)<0) return false;
    wcscpy(items[i].id,id);
    wcscpy(items[i].version,version);
    if (!wcscmp(view->prior.id,id) && !wcscmp(view->prior.version,version)) view->selection=(int)i;
    if (!wcscmp(view->first_visible.id,id) && !wcscmp(view->first_visible.version,version)) view->anchor=(int)i;
    return true;
}

static void w_choco_rows(wchar_t *text,w_render *view) {
    wchar_t *ctx = NULL;
    for (wchar_t *id = wcstok_s(text,L"\r\n",&ctx); id; id = wcstok_s(NULL,L"\r\n",&ctx)) {
        wchar_t *version = wcschr(id,L'|');
        if (!version) continue;
        *version++ = 0;
        if (!w_choco_token(id,false) || !w_choco_token(version,true)) continue;
        wchar_t label[420];
        swprintf(label,count_of(label),L"%ls   ·   %ls",id,version);
        if (!w_put(view,id,version,label)) break;
    }
}

static bool w_choco_details(const wchar_t *text) {
    wchar_t header[400];
    int n = swprintf(header,count_of(header),L"%ls %ls",selected,selected_version);
    for (const wchar_t *p = text; *p; p += wcsspn(p,L"\r\n")) {
        if (!_wcsnicmp(p,header,n) && (!p[n] || iswspace(p[n]))) return true;
        p += wcscspn(p,L"\r\n");
    }
    return false;
}

/* WinGet pads columns by terminal cell width. Only accept complete manifest IDs at column boundaries. */
static wchar_t *w_column(wchar_t *s, int column) {
    int pos = 0;
    while (*s && pos < column) {
        unsigned c = *s++;
        WORD info = 0;
        GetStringTypeW(CT_CTYPE3,s-1,1,&info);
        if (c >= 0xd800 && c <= 0xdbff && *s >= 0xdc00 && *s <= 0xdfff) { ++s; pos += 2; }
        else if (info & C3_NONSPACING) continue;
        else pos += ((info & (C3_FULLWIDTH | C3_IDEOGRAPH | C3_HIRAGANA | C3_KATAKANA)) && !(info & C3_HALFWIDTH)) ? 2 : 1;
    }
    return pos == column ? s : NULL;
}

static void w_rows(wchar_t *text,w_render *view) {
    int id_col = -1, version_col = -1;
    wchar_t *ctx = NULL;
    for (wchar_t *s = wcstok_s(text,L"\r\n",&ctx); s; s = wcstok_s(NULL,L"\r\n",&ctx)) {
        if (id_col < 0) {
            /* The table's separator follows its header. Column labels may be localized. */
            wchar_t *next = ctx;
            while (next && (*next == L'\r' || *next == L'\n')) ++next;
            if (!next || wcsspn(next,L"-") < 8) continue;
            int starts[3] = {0}, columns = 0, pos = 0;
            for (wchar_t *p = s; *p && columns < 3;) {
                while (*p == L' ') { ++pos; ++p; }
                if (!*p) break;
                starts[columns++] = pos;
                while (*p && *p != L' ') {
                    WORD info = 0;
                    GetStringTypeW(CT_CTYPE3,p,1,&info);
                    pos += ((info & (C3_FULLWIDTH | C3_IDEOGRAPH | C3_HIRAGANA | C3_KATAKANA)) && !(info & C3_HALFWIDTH)) ? 2 : 1;
                    ++p;
                }
            }
            if (columns == 3) { id_col = starts[1]; version_col = starts[2]; }
            continue;
        }
        wchar_t *id = w_column(s,id_col), *version = w_column(s,version_col);
        if (!id || id == s || id[-1] != L' ' || !version || version <= id || !*version || version[-1] != L' ') continue;
        wchar_t *end = version;
        while (end > id && end[-1] == L' ') --end;
        wchar_t saved = *end;
        *end = 0;
        bool valid = w_package_id(id);
        if (!valid) { *end=saved; continue; }
        wchar_t *name_end = id;
        while (name_end > s && name_end[-1] == L' ') --name_end;
        wchar_t label[768];
        int n = swprintf(label,count_of(label),L"%.*ls   ·   %ls   ·   %ls",(int)(name_end-s),s,id,version);
        bool ok=n<0 || w_put(view,id,L"",label);
        *end=saved;
        if (!ok) break;
    }
}

/* Remove terminal spinner frames and normalize lines for the native read-only edit. */
static wchar_t *w_decode(void) {
    int n = MultiByteToWideChar(CP_UTF8,0,bytes,(int)used,NULL,0);
    wchar_t *raw = calloc((size_t)n+1,sizeof(*raw)), *clean = calloc(2*(size_t)n+80,sizeof(*clean));
    if (!raw || !clean) { free(raw); free(clean); return NULL; }
    MultiByteToWideChar(CP_UTF8,0,bytes,(int)used,raw,n);
    wchar_t *out = clean, *line = out;
    for (int i = 0; i < n; ++i) {
        wchar_t c = raw[i];
        if (c == 27 && raw[i+1] == L'[') {
            i += 2;
            while (i < n && (raw[i] < 0x40 || raw[i] > 0x7e)) ++i;
        } else if (c == L'\r' && raw[i+1] != L'\n') out = line;
        else if (c == L'\n') { *out++ = L'\r'; *out++ = L'\n'; line = out; }
        else if (c == L'\b') { if (out > line) --out; }
        else if (c == L'\t' || c >= L' ') *out++ = c;
    }
    *out = 0;
    if (truncated) wcscat(clean,L"\r\n[Output limit reached.]");
    free(raw);
    return clean;
}

static void w_start(w_package_action op, const wchar_t *value);

static w_snapshot *w_cached(void) {
    for (size_t i=0;i<count_of(snapshots);++i)
        if (snapshots[i].text && w_same_key(&snapshots[i].key,&wanted)) return &snapshots[i];
    return NULL;
}

static void w_focus_match(void) {
    if (!focus_results) return;
    focus_results=false;
    if (GetFocus()!=query || !w_current()) return;
    if (SendMessageW(results,LB_GETCOUNT,0,0)>0) {
        if (SendMessageW(results,LB_GETCURSEL,0,0)==LB_ERR) {
            SendMessageW(results,LB_SETCURSEL,0,0);
            w_select_package();
        }
        SetFocus(results);
    } else if (*selected) {
        detail_open=true;
        w_start(pkg_show,selected);
    }
}

static void w_display(w_snapshot *snapshot) {
    bool same_view=w_current() && shown==snapshot;
    w_render view={.rows=w_rows_begin(results),.selection=-1,.anchor=-1};
    if (same_view) {
        wcscpy(view.prior.id,selected);
        wcscpy(view.prior.version,selected_version);
        if (view.rows.top>=0 && view.rows.top<view.rows.original) view.first_visible=items[view.rows.top];
    }
    if (!snapshot->code) {
        wchar_t *copy=_wcsdup(snapshot->text);
        if (!copy) return;
        if (chocolatey) w_choco_rows(copy,&view); else w_rows(copy,&view);
        free(copy);
    }
    bool retained=view.selection>=0;
    if (view.selection<0 && view.rows.written==1 && !receipt) view.selection=0;
    w_rows_end(&view.rows,view.selection,view.anchor>=0 ? view.anchor : same_view ? view.rows.top : 0);
    shown=snapshot;
    if ((!retained || refresh_failed) && !receipt) {
        selected[0]=selected_version[0]=0;
        reviewed=source_terms=false;
        if (view.selection>=0) w_select_package();
        else {
            wchar_t message[180];
            if (view.rows.written) swprintf(message,count_of(message),L"%d packages. Select one to review.",view.rows.written);
            else if (updates && (snapshot->code || !*snapshot->text)) wcscpy(message,L"No available updates.");
            else if (!updates && !snapshot->code && !chocolatey && w_package_id(wanted.text)) {
                // A narrow or localized WinGet table can abbreviate IDs.
                wcscpy(selected,wanted.text);
                wcscpy(message,L"Press Enter to review this package ID.");
            } else if (!chocolatey && !snapshot->code && *snapshot->text) wcscpy(message,L"See WinGet output below.");
            else wcscpy(message,L"No matching packages. Try another search.");
            w_tool_status(status,message);
            bool raw=!view.rows.written && !snapshot->code && !chocolatey;
            SetWindowTextW(output,raw ? snapshot->text : L"");
        }
    }
    w_controls();
}

static void w_catalog_result(wchar_t *text,DWORD code) {
    w_snapshot *entry=w_cached();
    if (!entry) {
        entry=&snapshots[0];
        for (size_t i=1;i<count_of(snapshots);++i)
            if (snapshots[i].accessed<entry->accessed) entry=&snapshots[i];
        if (shown==entry) shown=NULL;
    }
    bool identical=entry==shown && w_current() && code==entry->code && !wcscmp(entry->text,text);
    free(entry->text);
    entry->key=wanted;
    entry->text=text;
    entry->code=code;
    entry->accessed=GetTickCount64();
    UINT ttl=updates ? updates_ttl : search_ttl;
    entry->expires=entry->accessed+ttl;
    if (!identical || refresh_failed) w_display(entry);
    refresh_failed=false;
    w_schedule(ttl);
    w_controls();
    w_focus_match();
}

static void w_release_process(void) {
    KillTimer(window,poll_timer);
    if (stream) CloseHandle(stream);
    if (process) CloseHandle(process);
    stream=process=NULL;
}

static void w_cancel_read(void) {
    if (process && !w_mutating()) {
        TerminateProcess(process,ERROR_CANCELLED);
        w_release_process();
    }
}

// Rich Edit reports scroll positions in pixels and normalizes CRLF in selection offsets.
// Restore the native offsets directly so appended lines never disturb the reader.
static void w_progress_text(const wchar_t *previous,const wchar_t *next) {
    size_t prefix=previous ? wcslen(previous) : 0;
    bool append=previous && !wcsncmp(previous,next,prefix);
    if ((append && !next[prefix]) || (!*next && !GetWindowTextLengthW(output))) return;

    DWORD from=0,to=0;
    SendMessageW(output,EM_GETSEL,(WPARAM)&from,(LPARAM)&to);
    GETTEXTLENGTHEX length={GTL_NUMCHARS|GTL_PRECISE,1200};
    DWORD end=rich_edit ? (DWORD)SendMessageW(output,EM_GETTEXTLENGTHEX,(WPARAM)&length,0) :
                          (DWORD)GetWindowTextLengthW(output);
    SCROLLINFO bar={.cbSize=sizeof(bar),.fMask=SIF_ALL};
    // Rich Edit stops one pixel short of a standard scrollbar's inclusive maximum.
    bool follow=from==to && to==end && (!end || !GetScrollInfo(output,SB_VERT,&bar) ||
                bar.nPos>=bar.nMax-max(0,(int)bar.nPage-(rich_edit ? 0 : 1)));
    POINT scroll={0};
    int first=(int)SendMessageW(output,EM_GETFIRSTVISIBLELINE,0,0);
    int left=GetScrollPos(output,SB_HORZ);
    if (rich_edit) SendMessageW(output,EM_GETSCROLLPOS,0,(LPARAM)&scroll);

    if (append) {
        SendMessageW(output,EM_SETSEL,INT_MAX,INT_MAX);
        SendMessageW(output,EM_REPLACESEL,FALSE,(LPARAM)(next+prefix));
    } else SetWindowTextW(output,next);
    if (follow) {
        SendMessageW(output,EM_SETSEL,INT_MAX,INT_MAX);
        SendMessageW(output,WM_VSCROLL,SB_BOTTOM,0);
        // Following new lines should leave horizontally scrolled columns in view.
        if (rich_edit) {
            POINT tail={0};
            SendMessageW(output,EM_GETSCROLLPOS,0,(LPARAM)&tail);
            scroll.y=tail.y;
        }
    } else SendMessageW(output,EM_SETSEL,from,to);
    if (rich_edit) SendMessageW(output,EM_SETSCROLLPOS,0,(LPARAM)&scroll);
    else {
        if (!follow) SendMessageW(output,EM_LINESCROLL,0,first-SendMessageW(output,EM_GETFIRSTVISIBLELINE,0,0));
        if (GetScrollPos(output,SB_HORZ)!=left)
            SendMessageW(output,WM_HSCROLL,MAKEWPARAM(SB_THUMBPOSITION,left),0);
    }
}

static const wchar_t choco_running[]=L"Chocolatey is running in its own window.";

static void w_finish(DWORD code) {
    w_release_process();
    bool success=!code || (w_mutating() && (code==ERROR_SUCCESS_REBOOT_REQUIRED || code==ERROR_SUCCESS_REBOOT_INITIATED));
    const wchar_t *restart=code==ERROR_SUCCESS_REBOOT_REQUIRED ? L"Restart required  ·  " :
                           code==ERROR_SUCCESS_REBOOT_INITIATED ? L"Restart initiated  ·  " : L"";
    if (w_mutating() && success) {
        HWND manager = FindWindowW(L"WinwmEvents",NULL);
        if (manager) PostMessageW(manager,winwm_packages_changed,0,0);
        w_forget_snapshots();
        // Re-read the catalog while keeping the operation's receipt available to read or copy.
        receipt=true;
        detail_open=reviewed=false;
        w_schedule(debounce_ms);
    }
    if (closing) { DestroyWindow(window); return; }
    if (chocolatey && action == pkg_install) {
        wchar_t message[512];
        if (success) {
            swprintf(message,count_of(message),L"%lsInstall finished  ·  %ls %ls",restart,selected,selected_version);
            selected[0]=selected_version[0]=0;
        } else swprintf(message,count_of(message),L"Chocolatey install failed (exit code %ld).",(long)code);
        SetWindowTextW(status,message);
        w_progress_text(choco_running,L"Installation details are in the Chocolatey log (logs\\chocolatey.log under the Chocolatey installation folder).");
        w_controls();
        return;
    }
    wchar_t *transcript=w_decode();
    if (w_catalog(action) && transcript && (!code || (chocolatey ? code==2 : code==0x8a150014u))) {
        w_catalog_result(transcript,code);
        return;
    }
    bool stale=w_catalog(action) && w_current();
    refresh_failed=stale;
    if (w_catalog(action) && !stale) { shown=NULL; w_clear_rows(); }
    if (w_mutating()) {
        w_progress_text(progress_frame,transcript ? transcript : L"Could not read package manager output.");
        free(progress_frame); progress_frame=NULL;
    } else if ((!stale && !(w_catalog(action) && receipt)) || code==0x8a150046u)
        SetWindowTextW(output,transcript ? transcript : L"Could not read package manager output.");
    wchar_t message[380];
    if (chocolatey && code == 2) wcscpy(message,L"No matching package version.");
    else if (chocolatey && code) swprintf(message,count_of(message),L"Chocolatey returned %ld. See the output below.",(long)code);
    else if (code == 0x8a150046u && transcript && transcript[0] && !truncated) {
        source_terms = true;
        receipt=false;
        wcscpy(message,L"Review the WinGet source terms below.");
    }
    else if (code == 0x8a150014u) wcscpy(message,action == pkg_updates ? L"No available updates." : L"No matching packages.");
    else if (code == 0x8a150061u || code == 0x8a15010du || code == 0x8a15010eu)
        wcscpy(message,L"Already installed. Check Updates for a newer version.");
    else if (code == 0x8a15002bu) wcscpy(message,L"No applicable update is available.");
    else if (!success) swprintf(message,count_of(message),L"WinGet returned 0x%08lX. See the output below.",code);
    else if (action == pkg_show) {
        reviewed = transcript && transcript[0] && !truncated && (!chocolatey || w_choco_details(transcript));
        swprintf(message,count_of(message),reviewed ? L"Review package details  ·  %ls" : L"Incomplete package details  ·  %ls",selected);
    } else if (w_mutating()) {
        swprintf(message,count_of(message),L"%ls%ls finished  ·  %ls",restart,action==pkg_upgrade ? L"Update" : L"Install",selected);
        selected[0]=selected_version[0]=0;
    } else wcscpy(message,L"Could not read package manager output.");
    if (stale && !source_terms) wcscpy(message,L"Could not refresh the catalog. Showing the last results.");
    if (!w_catalog(action) || !receipt || source_terms) SetWindowTextW(status,message);
    free(transcript);
    if (w_catalog(action)) { focus_results=false; w_schedule(retry_ms); }
    w_controls();
}

static void w_poll(void) {
    DWORD available = 0, read = 0;
    char block[8192];
    for (int batch = 0; stream && batch < 16; ++batch) {
        if (!PeekNamedPipe(stream,NULL,0,NULL,&available,NULL) || !available) break;
        if (!ReadFile(stream,block,min(available,(DWORD)sizeof(block)),&read,NULL) || !read) break;
        size_t keep = min(sizeof(bytes)-used,(size_t)read);
        memcpy(bytes+used,block,keep); used += keep;
        if (keep < read) truncated = true;
    }
    if (WaitForSingleObject(process,0) == WAIT_OBJECT_0 &&
        (!stream || !PeekNamedPipe(stream,NULL,0,NULL,&available,NULL) || !available)) {
        DWORD code = ERROR_GEN_FAILURE;
        GetExitCodeProcess(process,&code);
        w_finish(code);
    } else if (w_mutating() && stream && used+(size_t)truncated!=progress_mark &&
               !closing && GetTickCount64()>=next_progress) {
        wchar_t *progress = w_decode();
        if (progress) {
            w_progress_text(progress_frame,progress);
            free(progress_frame);
            progress_frame=progress;
            progress_mark=used+(size_t)truncated;
        }
        next_progress = GetTickCount64()+500;
    }
}

static bool w_choco_path(wchar_t *path, DWORD cap) {
    DWORD bytes = cap*sizeof(*path);
    LSTATUS rc = RegGetValueW(HKEY_LOCAL_MACHINE,L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment",
                              L"ChocolateyInstall",RRF_RT_REG_SZ,NULL,path,&bytes);
    if (rc == ERROR_FILE_NOT_FOUND) {
        if (cap < MAX_PATH || FAILED(SHGetFolderPathW(NULL,CSIDL_COMMON_APPDATA,NULL,SHGFP_TYPE_CURRENT,path))) return false;
        if (wcslen(path)+count_of(L"\\chocolatey") > cap) return false;
        wcscat(path,L"\\chocolatey");
    } else if (rc != ERROR_SUCCESS) return false;
    if (wcslen(path) < 3 || path[1] != L':' || path[2] != L'\\' || wcslen(path)+count_of(L"\\bin\\choco.exe") > cap)
        return false;
    wcscat(path,L"\\bin\\choco.exe");
    DWORD flags = GetFileAttributesW(path);
    return flags != INVALID_FILE_ATTRIBUTES && !(flags & FILE_ATTRIBUTE_DIRECTORY);
}

static const wchar_t choco_missing[] = L"Chocolatey CLI was not found. Install Chocolatey CLI for this machine, then retry.";

static void w_launch_error(w_package_action op,const wchar_t *label,const wchar_t *detail) {
    if (w_catalog(op) && receipt) {
        shown=NULL;
        w_clear_rows();
    } else if (w_catalog(op) && w_current()) {
        refresh_failed=true;
        w_tool_status(status,L"Could not refresh the catalog. Showing the last results.");
    } else {
        if (w_catalog(op)) { shown=NULL; w_clear_rows(); }
        w_tool_status(status,label);
        w_tool_status(output,detail);
    }
    focus_results=false;
    if (w_catalog(op)) w_schedule(retry_ms);
    w_controls();
}

static void w_start(w_package_action op, const wchar_t *value) {
    if (process) return;
    w_schedule(0);
    wchar_t path[os_path_cap], line[1500];
    if (!w_args(op,value,line,count_of(line))) {
        if (w_catalog(op)) { shown=NULL; w_clear_rows(); }
        w_tool_status(status,L"Enter an app name or package ID without command-line options.");
        w_controls();
        return;
    }
    if (accept_once) wcscat(line,L" --accept-source-agreements");
    accept_once = false;
    source_terms = false;
    if (value != last_value) wcscpy(last_value,value);
    if (chocolatey) {
        if (!w_choco_path(path,count_of(path))) {
            w_launch_error(op,L"Chocolatey CLI unavailable.",choco_missing);
            return;
        }
        if (op == pkg_install) {
            SHELLEXECUTEINFOW run = {.cbSize=sizeof(run), .hwnd=window, .lpVerb=L"runas", .lpFile=path,
                .lpParameters=wcschr(line,L' ')+1, .nShow=SW_SHOWNORMAL,
                .fMask=SEE_MASK_NOCLOSEPROCESS|SEE_MASK_NOASYNC|SEE_MASK_FLAG_NO_UI};
            if (!ShellExecuteExW(&run) || !run.hProcess) {
                DWORD err = GetLastError();
                wchar_t message[180];
                if (err == ERROR_CANCELLED) wcscpy(message,L"Administrator approval canceled. You can retry the install.");
                else swprintf(message,count_of(message),L"Could not start Chocolatey as administrator (Windows error %lu).",err);
                SetWindowTextW(status,message);
                return;
            }
            action = op;
            process = run.hProcess;
            SetWindowTextW(status,L"Installing as administrator…");
            SetWindowTextW(output,choco_running);
            w_controls();
            SetTimer(window,poll_timer,100,NULL);
            return;
        }
    } else {
        DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA",path,count_of(path));
        if (!n || n + 34 >= count_of(path)) {
            w_launch_error(op,L"Could not locate App Installer.",L"The local App Installer directory could not be located.");
            return;
        }
        wcscat(path,L"\\Microsoft\\WindowsApps\\winget.exe");
    }
    SECURITY_ATTRIBUTES sec = {.nLength = sizeof(sec), .bInheritHandle = TRUE};
    HANDLE reader = NULL, writer = NULL;
    HANDLE input = CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&sec,OPEN_EXISTING,0,NULL);
    PROCESS_INFORMATION pi = {0};
    STARTUPINFOW si = {.cb = sizeof(si),.dwFlags = STARTF_USESTDHANDLES,.hStdInput = input};
    DWORD error = 0;
    if (input == INVALID_HANDLE_VALUE || !CreatePipe(&reader,&writer,&sec,0) ||
        !SetHandleInformation(reader,HANDLE_FLAG_INHERIT,0)) error = GetLastError();
    else {
        si.hStdOutput = si.hStdError = writer;
        if (!CreateProcessW(path,line,NULL,NULL,TRUE,CREATE_NO_WINDOW,NULL,NULL,&si,&pi)) error = GetLastError();
    }
    if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
    if (writer) CloseHandle(writer);
    if (error) {
        if (reader) CloseHandle(reader);
        wchar_t detail[512];
        swprintf(detail,count_of(detail),chocolatey ? L"Check the Chocolatey CLI installation. Windows error %lu." :
                 L"Check App Installer and its WinGet execution alias. Windows error %lu.",error);
        w_launch_error(op,chocolatey ? L"Chocolatey could not start." : L"WinGet could not start.",detail);
        return;
    }
    CloseHandle(pi.hThread);
    action = op; process = pi.hProcess; stream = reader; used = 0; truncated = false; next_progress = 0;
    progress_mark=0;
    if (!w_catalog(op) || (!w_current() && !receipt)) {
        if (!w_catalog(op)) SetWindowTextW(output,L"");
        w_tool_status(status,op == pkg_search ? (chocolatey ? L"Searching Chocolatey Community Repository…" : L"Searching the WinGet catalog…") : op == pkg_updates ? L"Checking installed packages…" :
                      op == pkg_show ? L"Loading publisher and license details…" :
                      op == pkg_install ? L"Installing package…" : L"Updating package…");
    }
    w_controls();
    SetTimer(window,poll_timer,80,NULL);
}

static void w_search(void) {
    if (process || detail_open || source_terms || confirming || composing || !IsWindowVisible(window) || IsIconic(window) ||
        (!updates && !*wanted.text)) return;
    w_snapshot *cached=w_cached();
    ULONGLONG now=GetTickCount64();
    if (cached) {
        cached->accessed=now;
        if (shown!=cached) w_display(cached);
        if (cached->expires>now) {
            w_schedule((UINT)(cached->expires-now));
            w_controls();
            w_focus_match();
            return;
        }
    }
    w_start(updates ? pkg_updates : pkg_search,wanted.text);
}

static void w_request(bool immediate) {
    w_catalog_key next={.page=updates ? page_updates : chocolatey ? page_choco : page_winget};
    if (!updates) {
        GetWindowTextW(query,next.text,count_of(next.text));
        size_t start=wcsspn(next.text,L" \t"),end=wcslen(next.text);
        while (end>start && iswspace(next.text[end-1])) --end;
        next.text[end]=0;
        if (start) memmove(next.text,next.text+start,(end-start+1)*sizeof(*next.text));
    }
    if (!w_same_key(&wanted,&next)) {
        w_cancel_read();
        wanted=next;
        source_terms=reviewed=detail_open=focus_results=refresh_failed=receipt=false;
        selected[0]=selected_version[0]=0;
        // Keep the previous rows on screen until their replacement is ready.
        // Disabling an obsolete view prevents actions on another query or source.
        if (!w_current()) {
            SendMessageW(results,LB_SETCURSEL,(WPARAM)-1,0);
            w_tool_status(status,updates ? L"Checking installed packages…" : L"Search by app name or package ID.");
            w_tool_status(output,L"");
        } else w_display(shown);
    }
    if (!updates && !*wanted.text) {
        w_schedule(0);
        shown=NULL; w_clear_rows();
        w_tool_status(status,L"Search by app name or package ID.");
        w_tool_status(output,L"");
    } else if (!composing) {
        if (immediate || w_cached()) w_search();
        else w_schedule(debounce_ms);
    }
    w_controls();
}

static void w_review(void) {
    if (confirming || (process && !w_catalog(action)) ||
        (!source_terms && (!w_current() || (chocolatey ? !w_choco_token(selected,false) : !w_package_id(selected))))) return;
    w_cancel_read();
    w_schedule(0);
    if (!process && source_terms) {
        confirming=true;
        int choice=w_dialog(window,L"Accept the displayed source terms for the winget catalog and retry?",
                            L"WinGet source terms",MB_OKCANCEL|MB_DEFBUTTON2);
        confirming=false;
        if (choice==IDOK) {
            accept_once = true;
            w_start(action,last_value);
        }
        return;
    }
    detail_open=true;
    if (!reviewed) { w_start(pkg_show,selected); return; }
    wchar_t prompt[560];
    if (chocolatey) swprintf(prompt,count_of(prompt),
        L"Install %ls %ls from the Chocolatey Community Repository as administrator and accept the package license terms?",selected,selected_version);
    else swprintf(prompt,count_of(prompt),L"%ls %ls from the WinGet catalog and accept its package license terms?",
             updates ? L"Update" : L"Install",selected);
    confirming=true;
    int choice=w_dialog(window,prompt,updates ? L"Review update" : L"Review install",MB_OKCANCEL|MB_DEFBUTTON2);
    confirming=false;
    if (choice==IDOK)
        w_start(updates ? pkg_upgrade : pkg_install,selected);
}

static void w_output_colors(void) {
    if (!rich_edit || !output) return;
    CHARFORMATW color = {.cbSize=sizeof(color),.dwMask=CFM_COLOR,.crTextColor=w_theme_foreground()};
    SendMessageW(output,EM_SETBKGNDCOLOR,0,w_theme_background());
    SendMessageW(output,EM_SETCHARFORMAT,SCF_ALL,(LPARAM)&color);
}

static int w_status_band(UINT dpi,int width) {
    int side=w_theme_button_size(dpi);
    int text_width=width-(show_review ? side+w_theme_spacing(dpi) : 0);
    int measured=w_tool_feedback_height(status,&package_feedback,text_width);
    return max(side,measured);
}

static int w_search_height(UINT dpi,int width) {
    return w_status_band(dpi,width)+w_theme_button_size(dpi)+5*w_theme_spacing(dpi);
}

static int w_log_height(UINT dpi,int lines) {
    if (!lines) return 0;
    int inset=2*GetSystemMetricsForDpi(SM_CYBORDER,dpi);
    if (log_scrollbar) inset+=GetSystemMetricsForDpi(SM_CYHSCROLL,dpi);
    return max(w_theme_button_size(dpi),lines*output_line_height+inset);
}

static void w_fit_dock(void) {
    RECT client,outer;
    w_theme_content_rect(window,&client);
    GetWindowRect(window,&outer);
    int fraction=resize_to>=resize_from ? dock_resize.value : 255-dock_resize.value;
    int height=resize_from+MulDiv(resize_to-resize_from,fraction,255);
    if (client.bottom!=height)
        SetWindowPos(window,NULL,0,0,outer.right-outer.left,outer.bottom-outer.top+height-client.bottom,
                     SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
}

static void w_update_dock(void) {
    if (!docked || !output) return;
    UINT dpi=GetDpiForWindow(window);
    int gap=w_theme_spacing(dpi);
    RECT client;
    w_theme_content_rect(window,&client);
    int target=w_search_height(dpi,client.right-4*gap)+(dock_rows ? dock_rows*w_theme_button_size(dpi)+gap : 0)+
               (dock_lines ? w_log_height(dpi,dock_lines)+gap : 0);
    if (target==resize_to) return;
    resize_from=client.bottom;
    resize_to=target;
    bool grow=target>=resize_from;
    w_disclosure_stop(window,&dock_resize,resize_timer,!grow);
    if (target!=resize_from) w_disclosure_to(window,&dock_resize,resize_timer,grow);
    w_fit_dock();
}

static void w_layout_body(void) {
    if (!apply || !output) return;
    RECT area;
    w_theme_content_rect(window,&area);
    UINT dpi=GetDpiForWindow(window);
    int gap=w_theme_spacing(dpi),pad=2*gap,h=w_theme_button_size(dpi);
    int width=max(1,(int)area.right-2*pad),top=pad+h+gap;
    int band=w_status_band(dpi,width);
    if (!docked) {
        int required=top+band+pad+(show_results ? h+gap : 0)+(show_transcript ? h+gap : 0);
        if (required>area.bottom) {
            w_tool_fit_height(window,required-area.bottom,false);
            w_theme_content_rect(window,&area);
        }
    }
    int space=max(2,(int)area.bottom-pad-top-band-gap);
    int full=show_results ? max(1,space-gap-(space-gap)*3/5) : space;
    int extent=MulDiv(full+gap,transcript.value,255);
    int list_height=max(1,space-extent);
    if (docked) {
        int available=max(0,(int)area.bottom-(top+band+pad));
        int log_band=show_transcript ? w_log_height(dpi,dock_lines)+gap : 0;
        int list_room=available;
        if (dock_rows && log_band) {
            // A short work area still needs a legible receipt beside a scrollable list.
            int one_line=w_log_height(dpi,1)+gap;
            int two_lines=w_log_height(dpi,min(2,dock_lines))+gap;
            int reserved=min(two_lines,max(one_line,available-h-gap));
            list_room=max(0,available-reserved);
        }
        int list_band=min(list_room,dock_rows ? dock_rows*h+gap : 0);
        if (log_band && list_band) {
            int rows=max(0,list_band-gap)/h;
            list_band=rows ? rows*h+gap : 0;
        }
        list_height=max(1,list_band-gap);
        extent=min(log_band,available-list_band);
        if (show_results && !list_band && extent) extent=max(0,extent-1-gap);
    }
    w_tool_place(results,pad,top,width,list_height);
    w_tool_visible(results,show_results);
    if (show_results) top+=list_height+gap;
    LONG_PTR style=GetWindowLongPtrW(status,GWL_STYLE);
    bool centered=package_feedback.height<=w_theme_role_line_height(dpi,text_body);
    if (!!(style&SS_CENTERIMAGE)!=centered)
        SetWindowLongPtrW(status,GWL_STYLE,centered ? style|SS_CENTERIMAGE : style&~(LONG_PTR)SS_CENTERIMAGE);
    w_tool_place(status,pad,top,show_review ? max(1,width-h-gap) : width,band);
    w_tool_place(apply,pad+width-h,top+(band-h)/2,h,h);
    w_tool_visible(apply,show_review);
    bool visible=extent>gap;
    if (visible) w_tool_place(output,pad,top+band+gap,width,extent-gap);
    w_tool_visible(output,visible);
}

static void w_layout(bool fonts) {
    if (!apply) return;
    RECT area;
    w_theme_content_rect(window,&area);
    UINT dpi = GetDpiForWindow(window);
    int gap = w_theme_spacing(dpi), pad = 2*gap, h = w_theme_button_size(dpi);
    int width = max(1,(int)area.right-2*pad);
    int top = pad;
    if (fonts) {
        HFONT next = w_theme_message_font(dpi);
        if (next) {
            for (HWND c = GetWindow(window,GW_CHILD); c; c = GetWindow(c,GW_HWNDNEXT))
                if (c != output) SendMessageW(c,WM_SETFONT,(WPARAM)next,TRUE);
            DeleteObject(font); font = next;
            source_ink=w_tool_widest_option(font,sources,count_of(sources));
        }
        HFONT mono = w_theme_role_font(dpi,text_mono);
        if (mono) {
            SendMessageW(output,WM_SETFONT,(WPARAM)mono,TRUE);
            DeleteObject(output_font); output_font = mono;
        }
        if (docked) output_line_height=w_theme_line_height((HFONT)SendMessageW(output,WM_GETFONT,0,0));
        w_output_colors();
    }
    if (SendMessageW(results,LB_GETITEMHEIGHT,0,0) != h) SendMessageW(results,LB_SETITEMHEIGHT,0,h);
    int mode=updates ? width : min(w_tool_selector_width(pages,source_ink,dpi),width/2);
    w_tool_place_combo(pages,pad,top,mode,h,4);
    if (!updates) w_tool_place(query,pad+mode+gap,top,max(1,width-mode-gap),h);
    w_update_dock();
    w_layout_body();
}

static w_tool_choice page_choice;
static void w_page(int page) {
    page_choice.row = page;
    updates = page == page_updates;
    chocolatey = page == page_choco;
    if (docked && wanted.page>=0 && wanted.page!=page) { shown=NULL; w_clear_rows(); dock_rows=0; }
    SendMessageW(pages,CB_SETCURSEL,page,0);
    ShowWindow(query,updates ? SW_HIDE : SW_SHOWNA);
    w_layout(false);
    w_request(true);
    SetFocus(updates ? pages : query);
}

static LRESULT CALLBACK w_query_proc(HWND w,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR data) {
    (void)id; (void)data;
    if (msg==WM_IME_STARTCOMPOSITION) { composing=true; w_schedule(0); }
    LRESULT result=DefSubclassProc(w,msg,wp,lp);
    if (composing && (msg==WM_IME_ENDCOMPOSITION || msg==WM_KILLFOCUS)) {
        composing=false;
        w_request(false);
    }
    return result;
}

enum { catalog_wake=WM_APP+73, dock_reflow };

static LRESULT CALLBACK w_output_metrics(HWND w,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR data) {
    (void)data;
    if (msg==WM_NCDESTROY) RemoveWindowSubclass(w,w_output_metrics,id);
    LRESULT result=DefSubclassProc(w,msg,wp,lp);
    if (msg==WM_WINDOWPOSCHANGED || msg==WM_STYLECHANGED || msg==WM_SETTEXT) {
        // Rich Edit hides its horizontal bar for short receipts. The plain Edit fallback keeps its bar.
        bool visible=(GetWindowLongPtrW(w,GWL_STYLE)&WS_HSCROLL)!=0;
        if (visible!=log_scrollbar) {
            log_scrollbar=visible;
            PostMessageW(GetParent(w),dock_reflow,0,0);
        }
    }
    return result;
}

static LRESULT CALLBACK w_pkg_proc(HWND w,UINT msg,WPARAM wp,LPARAM lp) {
    LRESULT themed;
    if (w_theme_message(w,msg,wp,lp,&themed)) return themed;
    if (msg==w_tool_feedback_changed) { w_update_dock(); w_layout_body(); return 0; }
    if (transcript.running && (msg==winwm_motion_updated ||
        (msg==WM_SETTINGCHANGE && (wp==SPI_SETCLIENTAREAANIMATION || wp==SPI_SETHIGHCONTRAST)))) {
        w_disclosure_to(w,&transcript,output_timer,show_transcript);
        w_layout_body();
    }
    if (dock_resize.running && (msg==winwm_motion_updated ||
        (msg==WM_SETTINGCHANGE && (wp==SPI_SETCLIENTAREAANIMATION || wp==SPI_SETHIGHCONTRAST)))) {
        w_disclosure_to(w,&dock_resize,resize_timer,resize_to>=resize_from);
        w_fit_dock();
    }
    if (w_tool_font_change(w,msg,wp,lp)) { w_layout(true); return 0; }
    // The manager commits its new nonclient heading after publishing the theme.
    if (msg==winwm_theme_updated && docked) PostMessageW(w,dock_reflow,0,0);
    if (msg == winwm_theme_updated || msg == WM_THEMECHANGED || msg == WM_SYSCOLORCHANGE ||
        (msg == WM_SETTINGCHANGE && w_appearance_setting(wp,lp))) w_output_colors();
    switch (msg) {
    case WM_TIMER:
        if (wp==resize_timer && dock_resize.running) {
            w_disclosure_tick(w,&dock_resize,resize_timer);
            w_fit_dock();
        } else if (wp==output_timer && transcript.running) {
            int old=transcript.value;
            w_disclosure_tick(w,&transcript,output_timer);
            if (transcript.value!=old) w_layout_body();
        } else if (wp==poll_timer && process) w_poll();
        else if (wp==catalog_timer && catalog_due && GetTickCount64()>=catalog_due) {
            w_schedule(0);
            w_search();
        }
        return 0;
    case WM_SHOWWINDOW:
        if (wp) PostMessageW(w,catalog_wake,0,0);
        else if (window==w) {
            w_schedule(0);
            if (dock_resize.running) {
                w_disclosure_stop(w,&dock_resize,resize_timer,resize_to>=resize_from);
                w_fit_dock();
            }
            w_disclosure_stop(w,&transcript,output_timer,show_transcript);
            w_layout_body();
        }
        break;
    case WM_SIZE:
        if (wp==SIZE_MINIMIZED) { suspended=true; w_schedule(0); }
        else {
            w_layout(false);
            if (suspended) { suspended=false; PostMessageW(w,catalog_wake,0,0); }
        }
        return 0;
    case catalog_wake:
        if (apply && wanted.page>=0 && !catalog_due) w_search();
        return 0;
    case dock_reflow:
        w_update_dock();
        w_fit_dock();
        return 0;
    case WM_GETMINMAXINFO: {
        UINT dpi = GetDpiForWindow(w);
        RECT client;
        w_theme_content_rect(w,&client);
        int minimum=w_search_height(dpi,client.right-4*w_theme_spacing(dpi));
        if (!docked) minimum+=3*w_theme_button_size(dpi)+2*w_theme_spacing(dpi);
        w_tool_minimum(w,(MINMAXINFO *)lp,MulDiv(430,dpi,96),minimum); return 0;
    }
    case WM_COMMAND:
        if (!confirming && !(process && w_mutating())) {
            HWND c = (HWND)lp;
            if (c == pages && w_tool_choice_changed(&page_choice,pages,HIWORD(wp))) w_page(page_choice.row);
            else if (c == query && HIWORD(wp) == EN_CHANGE) w_request(false);
            else if (c == results && HIWORD(wp) == LBN_SELCHANGE && w_current()) w_select_package();
            else if ((c == apply && HIWORD(wp) == BN_CLICKED) || (c == results && HIWORD(wp) == LBN_DBLCLK)) w_review();
        }
        return 0;
    case WM_CLOSE:
        if (process && w_mutating()) { closing = true; ShowWindow(w,SW_HIDE); return 0; }
        w_cancel_read();
        DestroyWindow(w); return 0;
    case WM_DESTROY:
        w_disclosure_stop(w,&dock_resize,resize_timer,false);
        w_disclosure_stop(w,&transcript,output_timer,false);
        w_schedule(0); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(w,msg,wp,lp);
}

static HWND w_child(const wchar_t *kind,const wchar_t *name,DWORD style,int id) {
    return CreateWindowExW(0,kind,name,WS_CHILD|WS_VISIBLE|style,0,0,1,1,window,(HMENU)(INT_PTR)id,GetModuleHandleW(NULL),NULL);
}

static HWND w_output_create(void) {
    DWORD style = WS_TABSTOP|WS_BORDER|WS_VSCROLL|WS_HSCROLL|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|ES_AUTOHSCROLL;
    // Rich Edit binds each script independently while preserving the console's fixed-width Latin font.
    rich_edit = LoadLibraryExW(L"msftedit.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (rich_edit) {
        HWND edit = w_child(MSFTEDIT_CLASS,L"",style,106);
        if (edit && !SendMessageW(edit,EM_SETTEXTMODE,TM_PLAINTEXT,0)) {
            // Rich Edit adds a sunken client edge on top of the requested native border.
            SetWindowLongPtrW(edit,GWL_EXSTYLE,GetWindowLongPtrW(edit,GWL_EXSTYLE)&~(LONG_PTR)WS_EX_CLIENTEDGE);
            SetWindowPos(edit,NULL,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
            SendMessageW(edit,EM_EXLIMITTEXT,0,2*sizeof(bytes)+80);
            return edit;
        }
        if (edit) DestroyWindow(edit);
        FreeLibrary(rich_edit);
        rich_edit = NULL;
    }
    HWND edit=w_child(L"EDIT",L"",style,106);
    if (edit) SendMessageW(edit,EM_SETLIMITTEXT,2*sizeof(bytes)+80,0);
    return edit;
}

int w_packages_run(const wchar_t *mode) {
    HINSTANCE instance = GetModuleHandleW(NULL);
    WNDCLASSW wc = {.lpfnWndProc=w_pkg_proc,.hInstance=instance,.hCursor=LoadCursorW(NULL,IDC_ARROW),.lpszClassName=L"WinwmSoftware"};
    RegisterClassW(&wc);
    MONITORINFO mi = {.cbSize=sizeof(mi)};
    UINT dpi = w_tool_work_area(&mi);
    RECT a = mi.rcWork;
    int width = min(MulDiv(700,dpi,96),a.right-a.left), height = min(MulDiv(510,dpi,96),a.bottom-a.top);
    docked=w_tool_popup_monitor()!=NULL;
    window = CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_CONTROLPARENT,wc.lpszClassName,L"Software",
        WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,(a.left+a.right-width)/2,(a.top+a.bottom-height)/2,width,height,NULL,NULL,instance,NULL);
    if (!window) return 1;
    pages = w_child(L"COMBOBOX",L"",WS_TABSTOP|w_tool_dropdown_list,100);
    query = w_native_field(window,101,WS_VISIBLE);
    results = w_child(L"LISTBOX",L"Packages",WS_TABSTOP|WS_VSCROLL|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT|LBS_OWNERDRAWFIXED|LBS_HASSTRINGS,103);
    status = w_child(L"STATIC",L"",SS_EDITCONTROL|SS_NOPREFIX,104);
    w_tool_track_feedback(status,&package_feedback);
    apply = w_child(L"BUTTON",L"Install…",WS_TABSTOP|BS_OWNERDRAW,105);
    if (!pages || !query || !results || !status || !apply || !(output = w_output_create())) {
        DestroyWindow(window); return 1;
    }
    SetWindowSubclass(query,w_query_proc,1,0);
    if (docked) {
        log_scrollbar=(GetWindowLongPtrW(output,GWL_STYLE)&WS_HSCROLL)!=0;
        SetWindowSubclass(output,w_output_metrics,1,0);
    }
    w_native_edit_frame(output,NULL);
    SendMessageW(query,EM_SETLIMITTEXT,510,0);
    SendMessageW(query,EM_SETCUEBANNER,TRUE,(LPARAM)L"App name or package ID");
    for (size_t i=0;i<count_of(sources);i++) SendMessageW(pages,CB_ADDSTRING,0,(LPARAM)sources[i]);
    HRESULT apartment = CoInitializeEx(NULL,COINIT_APARTMENTTHREADED|COINIT_DISABLE_OLE1DDE);
    wanted.page=-1;
    w_layout(true);
    if (w_tool_show(window)) {
        if (docked) { w_update_dock(); w_fit_dock(); }
        w_page(!wcscmp(mode,L"chocolatey") ? page_choco : !wcscmp(mode,L"updates") ? page_updates : page_winget);
        if (!_wcsicmp(mode,L"google_japanese_input")) { SetWindowTextW(query,L"Google.JapaneseIME"); w_request(true); }
    }
    MSG m;
    while (GetMessageW(&m,NULL,0,0) > 0) {
        if (w_native_ime_key(&m)) { TranslateMessage(&m); DispatchMessageW(&m); continue; }
        if (w_tool_repeated_dialog_key(&m)) continue;
        if (m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE && !SendMessageW(pages,CB_GETDROPPEDSTATE,0,0)) {
            SendMessageW(window,WM_CLOSE,0,0); continue;
        }
        if (!(process && w_mutating()) && m.message == WM_KEYDOWN && m.wParam == VK_RETURN) {
            if (m.hwnd == query) {
                if (source_terms) w_review();
                else if (detail_open && w_current()) SetFocus(IsWindowEnabled(apply) ? apply : results);
                else {
                    focus_results=true;
                    if (w_current() && SendMessageW(results,LB_GETCOUNT,0,0)>0) w_focus_match();
                    w_request(true);
                }
                continue;
            }
            if (m.hwnd == results) { w_review(); continue; }
        }
        if (!IsDialogMessageW(window,&m)) { TranslateMessage(&m); DispatchMessageW(&m); }
    }
    DeleteObject(font); font = NULL;
    DeleteObject(output_font); output_font = NULL;
    w_forget_snapshots();
    free(progress_frame); progress_frame=NULL;
    free(items); items=NULL; capacity=0;
    if (SUCCEEDED(apartment)) CoUninitialize();
    if (rich_edit) { FreeLibrary(rich_edit); rich_edit = NULL; }
    return 0;
}
