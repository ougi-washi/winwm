#define COBJMACROS
#include "desktop_tools/tools.h"
#include "theme/theme.h"
#include "theme/icon_button.h"
#include <shobjidl.h>
#include <shlwapi.h>
#include <stdio.h>

enum { project_limit = 16, launch_result = WM_APP + 12 };
typedef struct { int count; wchar_t paths[project_limit][os_path_cap]; } w_recent;
enum project_action { open_terminal, open_files, choose_document, open_editor };
typedef struct {
    enum project_action action;
    wchar_t folder[os_path_cap], file[os_path_cap];
    const char *provider;
    bool launched, saved;
    const wchar_t *error;
} w_project_job;

static const wchar_t projects_key[] = L"Software\\winwm\\Projects";
static HWND panel, label, folder, browse, recent_label, recent, forget, status, terminal, files, editor;
static HFONT font;
static const w_desktop_config *programs;
static HANDLE worker;
static LONG closing;
static w_project_job job;

static void w_load_recent(w_recent *history) {
    history->count = 0;
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, projects_key, L"Recent", RRF_RT_REG_MULTI_SZ, NULL, NULL, &bytes) ||
        !bytes || bytes > project_limit*os_path_cap*sizeof(wchar_t)+sizeof(wchar_t)) return;
    wchar_t *text = calloc(1, bytes + 2*sizeof(*text));
    if (!text) return;
    if (!RegGetValueW(HKEY_CURRENT_USER,projects_key,L"Recent",RRF_RT_REG_MULTI_SZ,NULL,text,&bytes)) {
        for (const wchar_t *p = text; *p && history->count < project_limit; p += wcslen(p)+1) {
            if (wcslen(p) < os_path_cap) wcscpy(history->paths[history->count++],p);
        }
    }
    free(text);
}

static bool w_update_recent(const wchar_t *path, bool remove) {
    // More than one panel can be open. Merge with the latest completed launch.
    HANDLE mutex = CreateMutexW(NULL,FALSE,L"Local\\winwm.project-history");
    if (!mutex) return false;
    DWORD wait = WaitForSingleObject(mutex,500);
    bool saved = false;
    if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
        w_recent history;
        w_load_recent(&history);
        wchar_t *values = calloc(project_limit*os_path_cap+2,sizeof(*values));
        if (values) {
            wchar_t *next = values;
            int count = 0;
            if (!remove) {
                wcscpy(next,path);
                next += wcslen(path)+1;
                count++;
            }
            for (int i=0; i<history.count && count < project_limit; i++) {
                if (!_wcsicmp(history.paths[i],path)) continue;
                wcscpy(next,history.paths[i]);
                next += wcslen(next)+1;
                count++;
            }
            HKEY key;
            if (RegCreateKeyExW(HKEY_CURRENT_USER,projects_key,0,NULL,0,KEY_SET_VALUE,NULL,&key,NULL) == ERROR_SUCCESS) {
                DWORD bytes = (DWORD)(max(2,next-values+1)*sizeof(*values));
                saved = RegSetValueExW(key,L"Recent",0,REG_MULTI_SZ,(BYTE *)values,bytes) == ERROR_SUCCESS;
                RegCloseKey(key);
            }
            free(values);
        }
        ReleaseMutex(mutex);
    }
    CloseHandle(mutex);
    return saved;
}

static void w_recent_list(void) {
    w_recent history;
    w_load_recent(&history);
    SendMessageW(recent,LB_RESETCONTENT,0,0);
    for (int i=0; i<history.count; i++) SendMessageW(recent,LB_ADDSTRING,0,(LPARAM)history.paths[i]);
    EnableWindow(forget,FALSE);
}

// Resolve user input without changing the process working directory or interpreting shell commands.
static bool w_project_path(const wchar_t *input, wchar_t out[os_path_cap]) {
    wchar_t text[os_path_cap], expanded[os_path_cap], combined[os_path_cap];
    if (wcslen(input) >= count_of(text)) return false;
    wcscpy(text,input);
    StrTrimW(text,L" \t");
    PathUnquoteSpacesW(text);
    if (!text[0]) return false;
    DWORD len = ExpandEnvironmentStringsW(text,expanded,count_of(expanded));
    if (!len || len > count_of(expanded)) return false;
    const wchar_t *p = expanded;
    if ((p[0] == L'~' && (!p[1] || p[1] == L'\\' || p[1] == L'/')) || PathIsRelativeW(p)) {
        DWORD n = GetEnvironmentVariableW(L"USERPROFILE",combined,count_of(combined));
        if (!n || n >= count_of(combined)) return false;
        if (p[0] == L'~' && (!p[1] || p[1] == L'\\' || p[1] == L'/')) { p++; if (*p) p++; }
        if (n+1+wcslen(p) >= count_of(combined)) return false;
        combined[n++] = L'\\';
        wcscpy(combined+n,p);
        p = combined;
    }
    len = GetFullPathNameW(p,os_path_cap,out,NULL);
    if (!len || len >= os_path_cap) return false;
    while (len && out[len-1] == L'\\' && !PathIsRootW(out)) out[--len] = 0;
    return len > 0;
}

static HRESULT w_pick_path(bool directory, const wchar_t *initial, wchar_t out[os_path_cap]) {
    IFileOpenDialog *dialog = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_FileOpenDialog,NULL,CLSCTX_INPROC_SERVER,&IID_IFileOpenDialog,(void **)&dialog);
    if (FAILED(hr)) return hr;
    FILEOPENDIALOGOPTIONS options;
    hr = IFileOpenDialog_GetOptions(dialog,&options);
    if (SUCCEEDED(hr)) hr = IFileOpenDialog_SetOptions(dialog,options | FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR |
                          FOS_DONTADDTORECENT | FOS_PATHMUSTEXIST | (directory ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
    if (SUCCEEDED(hr) && initial && *initial) {
        IShellItem *item = NULL;
        if (SUCCEEDED(SHCreateItemFromParsingName(initial,NULL,&IID_IShellItem,(void **)&item))) {
            IFileOpenDialog_SetFolder(dialog,item);
            IShellItem_Release(item);
        }
    }
    IFileOpenDialog_SetTitle(dialog,directory ? L"Choose project folder" : L"Open a project file");
    if (SUCCEEDED(hr)) hr = IFileOpenDialog_Show(dialog,panel);
    IShellItem *chosen = NULL;
    if (SUCCEEDED(hr)) hr = IFileOpenDialog_GetResult(dialog,&chosen);
    if (chosen) {
        PWSTR path = NULL;
        hr = IShellItem_GetDisplayName(chosen,SIGDN_FILESYSPATH,&path);
        if (SUCCEEDED(hr)) {
            if (wcslen(path) < os_path_cap) wcscpy(out,path);
            else hr = HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
            CoTaskMemFree(path);
        }
        IShellItem_Release(chosen);
    }
    IFileOpenDialog_Release(dialog);
    return hr;
}

static DWORD WINAPI w_project_launch(void *arg) {
    HWND receiver = arg;
    DWORD attr = GetFileAttributesW(job.folder);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
        job.error = L"Folder unavailable. Choose or enter an existing project folder.";
    else if (!InterlockedCompareExchange(&closing,0,0)) {
        if (job.action == open_terminal)
            job.launched = w_os_launch_terminal_at(job.provider,job.folder,false);
        else if (job.action != choose_document) {
            wchar_t exe[os_path_cap];
            HANDLE child = w_os_provider(job.provider,exe,count_of(exe)) ?
                           w_os_launch(exe,job.action == open_files ? job.folder : job.file) : NULL;
            if (child) { job.launched = true; CloseHandle(child); }
        }
        if (job.launched) job.saved = w_update_recent(job.folder,false);
        else if (job.action != choose_document)
            job.error = L"Could not open the configured program. Check its executable in winwm.conf.";
    }
    PostMessageW(receiver,launch_result,0,0);
    return 0;
}

static void w_buttons(bool busy) {
    bool has_path = GetWindowTextLengthW(folder) > 0;
    EnableWindow(folder,!busy);
    EnableWindow(browse,!busy);
    EnableWindow(recent,!busy);
    EnableWindow(forget,!busy && SendMessageW(recent,LB_GETCURSEL,0,0) != LB_ERR);
    EnableWindow(terminal,!busy && has_path);
    EnableWindow(files,!busy && has_path);
    EnableWindow(editor,!busy && has_path);
}

static void w_start_job(enum project_action action) {
    if (worker) return;
    job.action = action;
    job.error = NULL;
    job.launched = job.saved = false;
    job.provider = action == open_terminal ? programs->terminal : action == open_files ? programs->file_browser : programs->editor;
    wchar_t text[os_path_cap];
    GetWindowTextW(folder,text,count_of(text));
    if (!w_project_path(text,job.folder)) {
        SetWindowTextW(status,L"Enter a valid project folder.");
        SetFocus(folder);
        return;
    }
    worker = CreateThread(NULL,0,w_project_launch,panel,0,NULL);
    if (!worker) { SetWindowTextW(status,L"Could not start the project action."); return; }
    w_buttons(true);
    SetWindowTextW(status,L"Opening project…");
}

static bool w_selected_folder(wchar_t out[os_path_cap]) {
    LRESULT index = SendMessageW(recent,LB_GETCURSEL,0,0);
    return index != LB_ERR && SendMessageW(recent,LB_GETTEXTLEN,index,0) < os_path_cap &&
           SendMessageW(recent,LB_GETTEXT,index,(LPARAM)out) != LB_ERR;
}

static void w_layout_project(bool resize_font) {
    if (!editor) return;
    UINT dpi = GetDpiForWindow(panel);
    if (resize_font) {
        HFONT next = CreateFontW(-MulDiv(15,dpi,96),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,
                                 DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        HWND controls[] = {label,folder,browse,recent_label,recent,forget,status,terminal,files,editor};
        for (size_t i=0; i<count_of(controls); i++) SendMessageW(controls[i],WM_SETFONT,(WPARAM)next,TRUE);
        SendMessageW(recent,LB_SETITEMHEIGHT,0,MulDiv(32,dpi,96));
        DeleteObject(font);
        font = next;
    }
    RECT client;
    GetClientRect(panel,&client);
    int pad = MulDiv(16,dpi,96), gap = MulDiv(8,dpi,96), line = MulDiv(32,dpi,96);
    int width = max(1,(int)client.right-2*pad), side = min(width/3,line), y = pad;
    MoveWindow(label,pad,y,width,line,TRUE);
    y += line;
    MoveWindow(folder,pad,y,max(1,width-side-gap),line,TRUE);
    MoveWindow(browse,pad+width-side,y,side,line,TRUE);
    y += line+pad;
    MoveWindow(recent_label,pad,y,max(1,width-side-gap),line,TRUE);
    MoveWindow(forget,pad+width-side,y,side,line,TRUE);
    y += line+gap;
    int last = client.bottom-pad-line;
    MoveWindow(recent,pad,y,width,max(1,last-line-gap-y),TRUE);
    MoveWindow(status,pad,last-line,width,line,TRUE);
    HWND actions[] = {terminal,files,editor};
    for (int i = 0; i < 3; i++) MoveWindow(actions[i],pad+i*(side+gap),last,side,line,TRUE);
}

static LRESULT CALLBACK w_project_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    LRESULT result;
    if (w_theme_message(w,msg,wp,lp,&result)) return result;
    if (msg == WM_COMMAND) {
        HWND from = (HWND)lp;
        if (!from || !editor || worker) return 0;
        wchar_t text[os_path_cap], path[os_path_cap];
        if (from == folder && HIWORD(wp) == EN_CHANGE) {
            if (w_selected_folder(path)) {
                GetWindowTextW(folder,text,count_of(text));
                if (_wcsicmp(path,text)) SendMessageW(recent,LB_SETCURSEL,-1,0);
            }
            w_buttons(false);
        }
        else if (from == recent && HIWORD(wp) == LBN_SELCHANGE && w_selected_folder(path)) {
            SetWindowTextW(folder,path);
            w_buttons(false);
        } else if (from == browse && HIWORD(wp) == BN_CLICKED) {
            GetWindowTextW(folder,text,count_of(text));
            if (!w_project_path(text,path)) path[0] = 0;
            HRESULT hr = w_pick_path(true,path,text);
            if (SUCCEEDED(hr)) SetWindowTextW(folder,text);
            else if (hr != HRESULT_FROM_WIN32(ERROR_CANCELLED)) SetWindowTextW(status,L"Folder picker unavailable. Enter a folder path.");
        } else if (from == forget && HIWORD(wp) == BN_CLICKED && w_selected_folder(path)) {
            if (w_update_recent(path,true)) {
                w_recent_list();
                SetWindowTextW(status,L"Removed from recent folders.");
            } else SetWindowTextW(status,L"Recent folders could not be saved.");
        } else if ((from == terminal && HIWORD(wp) == BN_CLICKED) || (from == recent && HIWORD(wp) == LBN_DBLCLK)) w_start_job(open_terminal);
        else if (from == files && HIWORD(wp) == BN_CLICKED) w_start_job(open_files);
        else if (from == editor && HIWORD(wp) == BN_CLICKED) w_start_job(choose_document);
        return 0;
    }
    if (msg == launch_result && worker) {
        WaitForSingleObject(worker,INFINITE);
        CloseHandle(worker);
        worker = NULL;
        w_buttons(false);
        if (job.error) SetWindowTextW(status,job.error);
        else if (job.action == choose_document) {
            HRESULT hr = w_pick_path(false,job.folder,job.file);
            if (SUCCEEDED(hr)) w_start_job(open_editor);
            else SetWindowTextW(status,hr == HRESULT_FROM_WIN32(ERROR_CANCELLED) ? L"" : L"File picker unavailable.");
        } else if (job.launched && job.saved) DestroyWindow(w);
        else if (job.launched) SetWindowTextW(status,L"Project opened. Recent folders could not be saved.");
        return 0;
    }
    if (msg == WM_GETMINMAXINFO) {
        UINT dpi = GetDpiForWindow(w);
        w_tool_minimum(w,(MINMAXINFO *)lp,MulDiv(390,dpi,96),MulDiv(330,dpi,96));
        return 0;
    }
    if (msg == WM_SIZE) { if (wp != SIZE_MINIMIZED) w_layout_project(false); return 0; }
    if (msg == WM_DPICHANGED) {
        w_tool_resize_for_dpi(w,(RECT *)lp);
        w_layout_project(true);
        return 0;
    }
    if (msg == WM_DESTROY) {
        InterlockedExchange(&closing,1);
        if (worker) CancelSynchronousIo(worker);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w,msg,wp,lp);
}

static HWND w_project_control(const wchar_t *kind, const wchar_t *text, DWORD style, int id) {
    return CreateWindowExW(0,kind,text,WS_CHILD|WS_VISIBLE|style,0,0,1,1,panel,(HMENU)(INT_PTR)id,GetModuleHandleW(NULL),NULL);
}

int w_project_run(const w_desktop_config *config) {
    programs = config;
    HRESULT apartment = CoInitializeEx(NULL,COINIT_APARTMENTTHREADED|COINIT_DISABLE_OLE1DDE);
    HINSTANCE instance = GetModuleHandleW(NULL);
    WNDCLASSW wc = {.lpfnWndProc = w_project_proc,.hInstance = instance,.lpszClassName = L"WinwmProjects",
                    .hCursor = LoadCursorW(NULL,IDC_ARROW)};
    RegisterClassW(&wc);
    MONITORINFO monitor = {.cbSize = sizeof(monitor)};
    UINT dpi = w_tool_work_area(&monitor);
    RECT area=monitor.rcWork;
    int width = min(MulDiv(600,dpi,96),area.right-area.left);
    int height = min(MulDiv(430,dpi,96),area.bottom-area.top);
    panel = CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_CONTROLPARENT,wc.lpszClassName,L"Projects",
                            WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,(area.left+area.right-width)/2,(area.top+area.bottom-height)/2,
                            width,height,NULL,NULL,instance,NULL);
    if (!panel) { if (SUCCEEDED(apartment)) CoUninitialize(); return 1; }
    DWORD button = BS_OWNERDRAW | WS_TABSTOP;
    label = w_project_control(L"STATIC",L"Project &folder",SS_CENTERIMAGE,100);
    folder = w_project_control(L"EDIT",L"",WS_TABSTOP|WS_BORDER|ES_AUTOHSCROLL,101);
    SendMessageW(folder,EM_SETLIMITTEXT,os_path_cap-1,0);
    browse = w_project_control(L"BUTTON",L"Browse…",button,102);
    recent_label = w_project_control(L"STATIC",L"&Recent folders",SS_CENTERIMAGE,103);
    recent = w_project_control(L"LISTBOX",L"Recent folders",WS_TABSTOP|WS_VSCROLL|LBS_NOINTEGRALHEIGHT|LBS_OWNERDRAWFIXED|LBS_HASSTRINGS|LBS_NOTIFY,104);
    forget = w_project_control(L"BUTTON",L"Forget",button,105);
    status = w_project_control(L"STATIC",L"",SS_CENTERIMAGE|SS_ENDELLIPSIS,106);
    terminal = w_project_control(L"BUTTON",L"Open terminal",button,107);
    files = w_project_control(L"BUTTON",L"Open files",button,108);
    editor = w_project_control(L"BUTTON",L"Edit file…",button,109);
    if (!label || !folder || !recent_label || !recent || !forget || !status || !browse || !terminal || !files || !editor) {
        DestroyWindow(panel);
        if (SUCCEEDED(apartment)) CoUninitialize();
        return 1;
    }
    w_icon_button(browse,L"\uE838",L"Browse for a project folder");
    w_icon_button(forget,L"\uE738",L"Remove folder from recent projects");
    w_icon_button(terminal,L"\uE756",L"Open terminal in project folder");
    w_icon_button(files,L"\uE8B7",L"Open project folder in file manager");
    w_icon_button(editor,L"\uE70F",L"Choose a project file to edit");
    w_recent_list();
    wchar_t initial[os_path_cap] = {0};
    if (SendMessageW(recent,LB_SETCURSEL,0,0) != LB_ERR && w_selected_folder(initial)) SetWindowTextW(folder,initial);
    else {
        DWORD n = GetEnvironmentVariableW(L"USERPROFILE",initial,count_of(initial));
        if (n && n < count_of(initial)) SetWindowTextW(folder,initial);
    }
    w_buttons(false);
    w_layout_project(true);
    ShowWindow(panel,SW_SHOW);
    SetFocus(folder);
    SendMessageW(folder,EM_SETSEL,0,-1);
    MSG msg;
    while (GetMessageW(&msg,NULL,0,0)>0) {
        if (msg.message==WM_KEYDOWN && msg.wParam==VK_ESCAPE) { DestroyWindow(panel); continue; }
        if (msg.message==WM_KEYDOWN && msg.wParam==VK_RETURN && (msg.hwnd==recent || msg.hwnd==folder)) {
            w_start_job(open_terminal); continue;
        }
        if (!IsDialogMessageW(panel,&msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    if (worker) { WaitForSingleObject(worker,INFINITE); CloseHandle(worker); }
    DeleteObject(font);
    if (SUCCEEDED(apartment)) CoUninitialize();
    return 0;
}
