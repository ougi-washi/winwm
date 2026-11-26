#include "desktop_tools/tools.h"
#include "install/releases.h"
#include "theme/theme.h"
#include "theme/icon_button.h"
#include <winhttp.h>
#include <stdio.h>

enum { release_done = WM_APP+1 };
static HWND panel, label, versions, refresh, status, apply;
static HFONT font;
static UINT font_dpi;
static w_tool_feedback feedback;
static w_tool_choice choice = {.row=-1};
static w_release_catalog catalog;
static w_release selected;
static HANDLE worker, cancel;
static bool applying;

static void w_layout(void) {
    UINT dpi = GetDpiForWindow(panel);
    if (dpi != font_dpi) {
        HFONT next = w_theme_message_font(dpi);
        if (next) {
            HWND controls[] = {label,versions,refresh,status,apply};
            for (size_t i=0; i<count_of(controls); ++i)
                SendMessageW(controls[i],WM_SETFONT,(WPARAM)next,FALSE);
            DeleteObject(font);
            font = next;
            font_dpi = dpi;
        }
    }
    RECT body;
    w_theme_content_rect(panel,&body);
    int gap = w_theme_spacing(dpi), pad = 2*gap, side = w_theme_button_size(dpi);
    int width = max(1,body.right-2*pad), line = w_theme_role_line_height(dpi,text_body);
    int y = pad;
    w_tool_place(label,pad,y,width,line);
    y += line+gap;
    w_tool_place_combo(versions,pad,y,width-side-gap,side,7);
    w_tool_place(refresh,pad+width-side,y,side,side);
    y += side+2*gap;
    int text_width = max(1,width-side-gap);
    int height = max(side,w_tool_feedback_height(status,&feedback,text_width));
    w_tool_place(status,pad,y,text_width,height);
    w_tool_place(apply,pad+width-side,y,side,side);
    w_tool_fit_height(panel,y+height+pad-body.bottom,false);
}

static bool w_selected(void) {
    return choice.row >= 0 && (size_t)choice.row < catalog.count;
}

static void w_selection(void) {
    bool installed = w_selected() && !strcmp(catalog.items[choice.row].digest,catalog.installed);
    EnableWindow(versions,!worker && catalog.count);
    EnableWindow(refresh,!worker);
    EnableWindow(apply,!worker && w_selected() && !installed);
    w_icon_button(apply,installed ? L"\uE73E" : L"\uE896",
                  installed ? L"Installed" : L"Install selected release at sign-in");
    if (installed) w_tool_status(status,L"This release is already set to start at sign-in.");
    else if (!catalog.count) w_tool_status(status,L"No compatible releases are available.");
    else if (!w_selected()) w_tool_status(status,L"Choose a release to install.");
    else {
        const wchar_t *active = NULL;
        for (size_t i=0; i<catalog.count; ++i)
            if (!strcmp(catalog.items[i].digest,catalog.installed)) active = catalog.items[i].tag;
        wchar_t text[220];
        if (active) swprintf(text,count_of(text),L"Switch from %ls at your next sign-in.",active);
        else wcscpy(text,catalog.startup ? L"Apply this release to use it at your next sign-in."
                                         : L"Install the selected release to start at sign-in.");
        w_tool_status(status,text);
    }
}

static void w_fill_versions(void) {
    SendMessageW(versions,CB_RESETCONTENT,0,0);
    choice = (w_tool_choice){.row=-1};
    for (size_t i=0; i<catalog.count; ++i) {
        const w_release *r = catalog.items+i;
        wchar_t text[180];
        swprintf(text,count_of(text),L"%ls%ls%ls",r->tag,r->latest ? L"   ·   Latest" : r->prerelease ? L"   ·   Prerelease" : L"",
                 !strcmp(r->digest,catalog.installed) ? L"   ·   Installed" : L"");
        if (SendMessageW(versions,CB_ADDSTRING,0,(LPARAM)text)<0) break;
        if ((selected.asset && selected.asset == r->asset) || (!selected.asset && r->latest)) choice.row=(int)i;
    }
    SendMessageW(versions,CB_SETCURSEL,choice.row,0);
    w_selection();
}

static DWORD WINAPI w_worker(void *unused) {
    (void)unused;
    DWORD error = applying ? w_release_apply(&selected,cancel) : w_release_list(&catalog,cancel);
    PostMessageW(panel,release_done,error,0);
    return error;
}

static void w_error(DWORD error) {
    const wchar_t *detail;
    switch (error) {
    case ERROR_NOT_FOUND: detail=L"The release is no longer available. Refresh the list."; break;
    case ERROR_RETRY: detail=L"GitHub is limiting requests. Try again later."; break;
    case ERROR_WINHTTP_NAME_NOT_RESOLVED: case ERROR_WINHTTP_CANNOT_CONNECT: case ERROR_WINHTTP_TIMEOUT:
        detail=L"Could not reach GitHub. Check your connection and retry."; break;
    case ERROR_CRC: case ERROR_FILE_TOO_LARGE: detail=L"The download did not match the published release."; break;
    case ERROR_ALREADY_ASSIGNED: detail=L"Another installation controls startup for this account."; break;
    case ERROR_INVALID_DATA: detail=applying ? L"This release could not start with your configuration. Check winwm.lua before retrying." : L"Could not read the release information."; break;
    case ERROR_SHARING_VIOLATION: case ERROR_BUSY: detail=L"Installation files are busy. Retry after the other operation finishes."; break;
    default: {
        wchar_t text[128];
        swprintf(text,count_of(text),L"%ls failed (Windows error %lu).",
                 applying ? L"Installation" : L"Loading releases",error);
        w_tool_status(status,text);
        return;
    }
    }
    w_tool_status(status,detail);
}

static void w_start(bool install) {
    if (worker || (install && !w_selected())) return;
    applying = install;
    if (w_selected()) selected = catalog.items[choice.row];
    if (!install) {
        SendMessageW(versions,CB_RESETCONTENT,0,0);
        choice.row = -1;
        free(catalog.items);
        catalog = (w_release_catalog){0};
    }
    EnableWindow(refresh,FALSE);
    EnableWindow(versions,FALSE);
    EnableWindow(apply,FALSE);
    w_tool_status(status,install ? L"Downloading and verifying the selected release…" : L"Checking GitHub releases…");
    worker = CreateThread(NULL,0,w_worker,NULL,0,NULL);
    if (!worker) {
        DWORD error = GetLastError();
        w_selection();
        w_error(error);
    }
}

static LRESULT CALLBACK w_release_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    LRESULT result;
    if (w_theme_message(w,msg,wp,lp,&result)) return result;
    if (msg == release_done) {
        WaitForSingleObject(worker,INFINITE);
        CloseHandle(worker);
        worker = NULL;
        if (!wp && applying) {
            catalog.startup = true;
            strcpy(catalog.installed,selected.digest);
        }
        w_fill_versions();
        if (wp) w_error((DWORD)wp);
        else if (applying) {
            wchar_t text[200];
            swprintf(text,count_of(text),L"%ls will start at your next sign-in.",selected.tag);
            w_tool_status(status,text);
        }
        if (!GetFocus() || GetFocus()==w) SetFocus(IsWindowEnabled(versions) ? versions : refresh);
        return 0;
    }
    if (msg == WM_COMMAND && apply) {
        HWND from = (HWND)lp;
        if (from == versions && !worker && w_tool_choice_changed(&choice,versions,HIWORD(wp))) w_selection();
        else if (HIWORD(wp)==BN_CLICKED && (from==apply || from==refresh)) w_start(from==apply);
        else if (!from && LOWORD(wp)==IDCANCEL) SendMessageW(w,WM_CLOSE,0,0);
        return 0;
    }
    if (w_tool_font_change(w,msg,wp,lp) || msg==winwm_theme_updated || msg==WM_FONTCHANGE) {
        font_dpi = 0;
        if (apply) w_layout();
        return 0;
    }
    if ((msg==WM_SIZE || msg==w_tool_feedback_changed) && apply && wp!=SIZE_MINIMIZED) { w_layout(); return 0; }
    if (msg==WM_DESTROY) {
        SetEvent(cancel);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w,msg,wp,lp);
}

int w_releases_run(void) {
    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSW wc = {.lpfnWndProc=w_release_proc,.hInstance=inst,.hCursor=LoadCursorW(NULL,IDC_ARROW),
                    .lpszClassName=L"WinwmReleases"};
    RegisterClassW(&wc);
    MONITORINFO monitor = {.cbSize=sizeof(monitor)};
    UINT dpi = w_tool_work_area(&monitor);
    RECT area = monitor.rcWork;
    int width = min(MulDiv(440,dpi,96),area.right-area.left), height = MulDiv(170,dpi,96);
    panel = CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_CONTROLPARENT,wc.lpszClassName,L"Update winwm",
        WS_CAPTION|WS_SYSMENU|WS_CLIPCHILDREN,(area.left+area.right-width)/2,(area.top+area.bottom-height)/2,
        width,height,NULL,NULL,inst,NULL);
    if (!panel) return 1;
    DWORD child = WS_CHILD|WS_VISIBLE, tab = child|WS_TABSTOP;
    label = CreateWindowW(L"STATIC",L"Release",child,0,0,0,0,panel,NULL,inst,NULL);
    versions = CreateWindowW(L"COMBOBOX",L"",tab|w_tool_dropdown_list|WS_VSCROLL,0,0,0,0,panel,(HMENU)101,inst,NULL);
    refresh = CreateWindowW(L"BUTTON",L"Refresh releases",tab|BS_OWNERDRAW,0,0,0,0,panel,(HMENU)102,inst,NULL);
    status = CreateWindowW(L"STATIC",L"",child|SS_EDITCONTROL|SS_NOPREFIX,0,0,0,0,panel,NULL,inst,NULL);
    apply = CreateWindowW(L"BUTTON",L"Install at sign-in",tab|BS_OWNERDRAW,0,0,0,0,panel,(HMENU)103,inst,NULL);
    cancel = CreateEventW(NULL,TRUE,FALSE,NULL);
    if (!label || !versions || !refresh || !status || !apply || !cancel) { DestroyWindow(panel); return 1; }
    w_icon_button(refresh,L"\uE72C",L"Refresh releases");
    w_icon_button(apply,L"\uE896",L"Install selected release at sign-in");
    w_tool_track_feedback(status,&feedback);
    w_layout();
    if (w_tool_show(panel)) w_start(false);
    MSG m;
    while (GetMessageW(&m,NULL,0,0)>0) {
        if (w_tool_repeated_dialog_key(&m)) continue;
        if (m.message==WM_KEYDOWN && m.wParam==VK_RETURN && m.hwnd==versions &&
            !SendMessageW(versions,CB_GETDROPPEDSTATE,0,0) && IsWindowEnabled(apply)) {
            w_start(true);
            continue;
        }
        if (!IsDialogMessageW(panel,&m)) { TranslateMessage(&m); DispatchMessageW(&m); }
    }
    if (worker) { WaitForSingleObject(worker,INFINITE); CloseHandle(worker); }
    free(catalog.items);
    CloseHandle(cancel);
    DeleteObject(font);
    return 0;
}
