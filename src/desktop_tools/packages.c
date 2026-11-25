#include "desktop_tools/tools.h"
#include "theme/theme.h"
#include "theme/icon_button.h"
#include <commctrl.h>
#include <wchar.h>
#include <wctype.h>

typedef enum w_package_action { pkg_search, pkg_updates, pkg_show, pkg_install, pkg_upgrade } w_package_action;

static HWND window, pages, query, search, results, status, output, apply;
static HFONT font, output_font;
static HANDLE process, stream;
static w_package_action action;
static bool updates, closing, truncated, reviewed, source_terms, accept_once;
static char bytes[256 * 1024];
static size_t used;
static wchar_t selected[256];
static wchar_t last_value[512];
static wchar_t *transcript;
static ULONGLONG next_progress;

static bool w_mutating(void) { return action == pkg_install || action == pkg_upgrade; }

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

/* Quote one argument using CommandLineToArgvW rules. The executable never goes through a shell. */
static bool w_args(w_package_action op, const wchar_t *value, wchar_t *line, size_t cap) {
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
    EnableWindow(pages,!process);
    EnableWindow(query,!process && !updates);
    EnableWindow(results,!process);
    EnableWindow(search,!process && (updates || GetWindowTextLengthW(query) > 0));
    EnableWindow(apply,!process && (selected[0] || source_terms));
    w_icon_button(search,updates ? L"\uE72C" : L"\uE721",
                  updates ? L"Refresh available updates" : L"Search WinGet");
    SetWindowTextW(apply,source_terms ? L"Accept terms…" : reviewed ? (updates ? L"Update…" : L"Install…") : L"Review");
}

static void w_clear_rows(void) {
    int count = (int)SendMessageW(results,LB_GETCOUNT,0,0);
    for (int i = 0; i < count; ++i) {
        LRESULT ptr = SendMessageW(results,LB_GETITEMDATA,i,0);
        if (ptr && ptr != LB_ERR) free((void *)ptr);
    }
    SendMessageW(results,LB_RESETCONTENT,0,0);
    selected[0] = 0;
    reviewed = false;
}

static void w_selection(void) {
    LRESULT row = SendMessageW(results,LB_GETCURSEL,0,0);
    LRESULT ptr = row == LB_ERR ? 0 : SendMessageW(results,LB_GETITEMDATA,row,0);
    selected[0] = 0;
    reviewed = false;
    source_terms = false;
    if (ptr && ptr != LB_ERR) wcscpy(selected,(wchar_t *)ptr);
    if (selected[0]) {
        wchar_t label[320];
        swprintf(label,count_of(label),L"%ls   ·   winget",selected);
        SetWindowTextW(status,label);
        SetWindowTextW(output,L"Review the publisher, version, and license before installing or updating.");
    }
    w_controls();
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

static int w_rows(wchar_t *text) {
    int id_col = -1, version_col = -1, count = 0;
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
        wchar_t *copy = valid ? _wcsdup(id) : NULL;
        *end = saved;
        if (!copy) continue;
        wchar_t *name_end = id;
        while (name_end > s && name_end[-1] == L' ') --name_end;
        wchar_t label[768];
        int n = swprintf(label,count_of(label),L"%.*ls   ·   %ls   ·   %ls",(int)(name_end-s),s,copy,version);
        LRESULT row = n < 0 ? LB_ERR : SendMessageW(results,LB_ADDSTRING,0,(LPARAM)label);
        if (row < 0) free(copy);
        else { SendMessageW(results,LB_SETITEMDATA,row,(LPARAM)copy); ++count; }
    }
    return count;
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

static void w_finish(DWORD code) {
    KillTimer(window,1);
    CloseHandle(stream); stream = NULL;
    CloseHandle(process); process = NULL;
    if (closing) { DestroyWindow(window); return; }
    free(transcript); transcript = w_decode();
    SetWindowTextW(output,transcript ? transcript : L"Could not read WinGet output.");
    int count = 0;
    if (!code && transcript && (action == pkg_search || action == pkg_updates)) {
        wchar_t *copy = _wcsdup(transcript);
        if (copy) { count = w_rows(copy); free(copy); }
    }
    wchar_t message[380];
    if (code == 0x8a150046u && transcript && transcript[0] && !truncated) {
        source_terms = true;
        wcscpy(message,L"Review the WinGet source terms below.");
    }
    else if (code == 0x8a150014u) wcscpy(message,action == pkg_updates ? L"No available updates." : L"No matching packages.");
    else if (code == 0x8a150061u || code == 0x8a15010du || code == 0x8a15010eu)
        wcscpy(message,L"Already installed. Check Updates for a newer version.");
    else if (code == 0x8a15002bu) wcscpy(message,L"No applicable update is available.");
    else if (code) swprintf(message,count_of(message),L"WinGet returned 0x%08lX. See the output below.",code);
    else if (action == pkg_show) {
        reviewed = transcript && transcript[0] && !truncated;
        swprintf(message,count_of(message),reviewed ? L"Review package details  ·  %ls" : L"Incomplete package details  ·  %ls",selected);
    } else if (w_mutating()) {
        swprintf(message,count_of(message),L"%ls finished. %ls",action == pkg_upgrade ? L"Update" : L"Install",selected);
        w_clear_rows();
    } else if (count) swprintf(message,count_of(message),L"%d package%ls. Select one to review.",count,count == 1 ? L"" : L"s");
    else {
        wchar_t value[512];
        GetWindowTextW(query,value,count_of(value));
        /* A narrow or localized table can abbreviate IDs. Resolve an explicitly typed ID against the source. */
        if (action == pkg_search && w_package_id(value)) {
            wcscpy(selected,value);
            w_start(pkg_show,selected);
            return;
        }
        wcscpy(message,L"See WinGet output below. Enter a full package ID to review it.");
    }
    SetWindowTextW(status,message);
    if (count == 1) { SendMessageW(results,LB_SETCURSEL,0,0); w_selection(); }
    w_controls();
}

static void w_poll(void) {
    DWORD available = 0, read = 0;
    char block[8192];
    for (int batch = 0; batch < 16; ++batch) {
        if (!PeekNamedPipe(stream,NULL,0,NULL,&available,NULL) || !available) break;
        if (!ReadFile(stream,block,min(available,(DWORD)sizeof(block)),&read,NULL) || !read) break;
        size_t keep = min(sizeof(bytes)-used,(size_t)read);
        memcpy(bytes+used,block,keep); used += keep;
        if (keep < read) truncated = true;
    }
    if (WaitForSingleObject(process,0) == WAIT_OBJECT_0 &&
        (!PeekNamedPipe(stream,NULL,0,NULL,&available,NULL) || !available)) {
        DWORD code = ERROR_GEN_FAILURE;
        GetExitCodeProcess(process,&code);
        w_finish(code);
    } else if (w_mutating() && read && !closing && GetTickCount64() >= next_progress) {
        wchar_t *progress = w_decode();
        if (progress) {
            SetWindowTextW(output,progress);
            SendMessageW(output,EM_SETSEL,(WPARAM)-1,-1);
            SendMessageW(output,EM_SCROLLCARET,0,0);
            free(progress);
        }
        next_progress = GetTickCount64()+500;
    }
}

static void w_start(w_package_action op, const wchar_t *value) {
    if (process) return;
    wchar_t path[os_path_cap], line[1500];
    if (!w_args(op,value,line,count_of(line))) return;
    if (accept_once) wcscat(line,L" --accept-source-agreements");
    accept_once = false;
    source_terms = false;
    if (value != last_value) wcscpy(last_value,value);
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA",path,count_of(path));
    if (!n || n + 34 >= count_of(path)) { SetWindowTextW(status,L"Could not locate App Installer."); return; }
    wcscat(path,L"\\Microsoft\\WindowsApps\\winget.exe");
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
        swprintf(detail,count_of(detail),L"Could not start WinGet (Windows error %lu). Check App Installer and its WinGet execution alias.",error);
        SetWindowTextW(status,detail);
        SetWindowTextW(output,detail);
        w_controls();
        return;
    }
    CloseHandle(pi.hThread);
    action = op; process = pi.hProcess; stream = reader; used = 0; truncated = false; next_progress = 0;
    if (op == pkg_search || op == pkg_updates) w_clear_rows();
    SetWindowTextW(output,L"");
    SetWindowTextW(status,op == pkg_search ? L"Searching the WinGet catalog…" : op == pkg_updates ? L"Checking installed packages…" :
                   op == pkg_show ? L"Loading publisher and license details…" :
                   op == pkg_install ? L"Installing package…" : L"Updating package…");
    w_controls();
    SetTimer(window,1,80,NULL);
}

static void w_search(void) {
    wchar_t value[512];
    GetWindowTextW(query,value,count_of(value));
    if (!updates && !value[0]) {
        SetWindowTextW(status,L"Enter an app name or package ID."); return;
    }
    w_start(updates ? pkg_updates : pkg_search,value);
}

static void w_review(void) {
    if (!process && source_terms) {
        if (MessageBoxW(window,L"Accept the displayed source terms for the winget catalog and retry?",
                        L"WinGet source terms",MB_OKCANCEL|MB_DEFBUTTON2) == IDOK) {
            accept_once = true;
            w_start(action,last_value);
        }
        return;
    }
    if (process || !w_package_id(selected)) return;
    if (!reviewed) { w_start(pkg_show,selected); return; }
    wchar_t prompt[460];
    swprintf(prompt,count_of(prompt),L"%ls %ls from the WinGet catalog and accept its package license terms?",
             updates ? L"Update" : L"Install",selected);
    if (MessageBoxW(window,prompt,updates ? L"Review update" : L"Review install",MB_OKCANCEL|MB_DEFBUTTON2) == IDOK)
        w_start(updates ? pkg_upgrade : pkg_install,selected);
}

static void w_layout(bool fonts) {
    if (!apply) return;
    RECT area;
    GetClientRect(window,&area);
    UINT dpi = GetDpiForWindow(window);
    int pad = MulDiv(14,dpi,96), gap = MulDiv(8,dpi,96), h = MulDiv(32,dpi,96);
    int width = max(1,(int)area.right-2*pad), button = min(width/3,MulDiv(104,dpi,96));
    int top = pad, mode = updates ? width-h-gap : min(width/3,MulDiv(132,dpi,96));
    if (fonts) {
        HFONT next = CreateFontW(-MulDiv(14,dpi,96),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        if (next) {
            for (HWND c = GetWindow(window,GW_CHILD); c; c = GetWindow(c,GW_HWNDNEXT)) SendMessageW(c,WM_SETFONT,(WPARAM)next,TRUE);
            DeleteObject(font); font = next;
        }
        HFONT mono = CreateFontW(-MulDiv(13,dpi,96),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Consolas");
        if (mono) {
            SendMessageW(output,WM_SETFONT,(WPARAM)mono,TRUE);
            DeleteObject(output_font); output_font = mono;
        }
        SendMessageW(results,LB_SETITEMHEIGHT,0,h);
    }
    MoveWindow(pages,pad,top,mode,h*5,TRUE);
    MoveWindow(query,pad+mode+gap,top,max(1,width-mode-h-2*gap),h,TRUE);
    MoveWindow(search,pad+width-h,top,h,h,TRUE);
    top += h+gap;
    int space = max(2,(int)area.bottom-pad-top-h-2*gap);
    int list_height = space*3/5;
    MoveWindow(results,pad,top,width,list_height,TRUE);
    top += list_height+gap;
    MoveWindow(status,pad,top,max(1,width-button-gap),h,TRUE);
    MoveWindow(apply,pad+width-button,top,button,h,TRUE);
    MoveWindow(output,pad,top+h+gap,width,space-list_height,TRUE);
}

static void w_page(bool update) {
    updates = update;
    source_terms = false;
    SendMessageW(pages,CB_SETCURSEL,updates ? 1 : 0,0);
    ShowWindow(query,updates ? SW_HIDE : SW_SHOWNA);
    w_layout(false);
    w_clear_rows(); SetWindowTextW(output,L"");
    SetWindowTextW(status,L"Search by app name or package ID.");
    w_controls();
    if (updates) w_search();
    else SetFocus(query);
}

static LRESULT CALLBACK w_pkg_proc(HWND w,UINT msg,WPARAM wp,LPARAM lp) {
    LRESULT themed;
    if (w_theme_message(w,msg,wp,lp,&themed)) return themed;
    switch (msg) {
    case WM_TIMER: if (wp == 1 && process) w_poll(); return 0;
    case WM_SIZE: if (wp != SIZE_MINIMIZED) w_layout(false); return 0;
    case WM_DPICHANGED: w_tool_resize_for_dpi(w,(RECT *)lp); w_layout(true); return 0;
    case WM_GETMINMAXINFO: {
        UINT dpi = GetDpiForWindow(w);
        w_tool_minimum(w,(MINMAXINFO *)lp,MulDiv(430,dpi,96),MulDiv(325,dpi,96)); return 0;
    }
    case WM_COMMAND:
        if (!process) {
            HWND c = (HWND)lp;
            if (c == pages && HIWORD(wp) == CBN_SELCHANGE) w_page(SendMessageW(pages,CB_GETCURSEL,0,0) == 1);
            else if (c == search && HIWORD(wp) == BN_CLICKED) w_search();
            else if (c == results && HIWORD(wp) == LBN_SELCHANGE) w_selection();
            else if ((c == apply && HIWORD(wp) == BN_CLICKED) || (c == results && HIWORD(wp) == LBN_DBLCLK)) w_review();
            else if (c == query && HIWORD(wp) == EN_CHANGE) {
                source_terms = false; w_clear_rows();
                SetWindowTextW(status,L"Search by app name or package ID.");
                SetWindowTextW(output,L"");
                w_controls();
            }
        }
        return 0;
    case WM_CLOSE:
        if (process && w_mutating()) { closing = true; ShowWindow(w,SW_HIDE); return 0; }
        if (process) { TerminateProcess(process,ERROR_CANCELLED); CloseHandle(stream); CloseHandle(process); process = NULL; }
        DestroyWindow(w); return 0;
    case WM_DESTROY: w_clear_rows(); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(w,msg,wp,lp);
}

static HWND w_child(const wchar_t *kind,const wchar_t *name,DWORD style,int id) {
    return CreateWindowExW(0,kind,name,WS_CHILD|WS_VISIBLE|style,0,0,1,1,window,(HMENU)(INT_PTR)id,GetModuleHandleW(NULL),NULL);
}

int w_packages_run(const wchar_t *mode) {
    HINSTANCE instance = GetModuleHandleW(NULL);
    WNDCLASSW wc = {.lpfnWndProc=w_pkg_proc,.hInstance=instance,.hCursor=LoadCursorW(NULL,IDC_ARROW),.lpszClassName=L"WinwmSoftware"};
    RegisterClassW(&wc);
    MONITORINFO mi = {.cbSize=sizeof(mi)};
    UINT dpi = w_tool_work_area(&mi);
    RECT a = mi.rcWork;
    int width = min(MulDiv(700,dpi,96),a.right-a.left), height = min(MulDiv(510,dpi,96),a.bottom-a.top);
    window = CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_CONTROLPARENT,wc.lpszClassName,L"Software",
        WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,(a.left+a.right-width)/2,(a.top+a.bottom-height)/2,width,height,NULL,NULL,instance,NULL);
    if (!window) return 1;
    pages = w_child(L"COMBOBOX",L"",WS_TABSTOP|CBS_DROPDOWNLIST,100);
    query = w_child(L"EDIT",L"",WS_TABSTOP|WS_BORDER|ES_AUTOHSCROLL,101);
    search = w_child(L"BUTTON",L"Search",WS_TABSTOP|BS_OWNERDRAW,102);
    results = w_child(L"LISTBOX",L"Packages",WS_TABSTOP|WS_VSCROLL|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT|LBS_OWNERDRAWFIXED|LBS_HASSTRINGS,103);
    status = w_child(L"STATIC",L"",SS_CENTERIMAGE|SS_ENDELLIPSIS|SS_NOPREFIX,104);
    apply = w_child(L"BUTTON",L"Install…",WS_TABSTOP|BS_OWNERDRAW,105);
    output = w_child(L"EDIT",L"",WS_TABSTOP|WS_BORDER|WS_VSCROLL|WS_HSCROLL|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|ES_AUTOHSCROLL,106);
    if (!pages || !query || !search || !results || !status || !apply || !output) { DestroyWindow(window); return 1; }
    SendMessageW(query,EM_SETLIMITTEXT,510,0);
    SendMessageW(query,EM_SETCUEBANNER,TRUE,(LPARAM)L"App name or package ID");
    SendMessageW(pages,CB_ADDSTRING,0,(LPARAM)L"Find software");
    SendMessageW(pages,CB_ADDSTRING,0,(LPARAM)L"Updates");
    w_layout(true);
    ShowWindow(window,SW_SHOW);
    w_page(!_wcsicmp(mode,L"updates"));
    if (!_wcsicmp(mode,L"google_japanese_input")) { SetWindowTextW(query,L"Google.JapaneseIME"); w_search(); }
    MSG m;
    while (GetMessageW(&m,NULL,0,0) > 0) {
        if (m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE && !SendMessageW(pages,CB_GETDROPPEDSTATE,0,0)) {
            SendMessageW(window,WM_CLOSE,0,0); continue;
        }
        if (!process && m.message == WM_KEYDOWN && m.wParam == VK_RETURN) {
            if (m.hwnd == query) { w_search(); continue; }
            if (m.hwnd == results) { w_review(); continue; }
        }
        if (!IsDialogMessageW(window,&m)) { TranslateMessage(&m); DispatchMessageW(&m); }
    }
    DeleteObject(font); font = NULL;
    DeleteObject(output_font); output_font = NULL;
    free(transcript); transcript = NULL;
    return 0;
}
