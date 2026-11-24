#include "theme/theme.h"
#include "palette/palette.h"
#include <richedit.h>
#include <usp10.h>
#include <wctype.h>
enum { check_deactivation = WM_APP + 1, restore_foreground };

////////////////
//~ Palette

static HWND panel, edit, list, return_to, pending_restore;
static HMODULE edit_library;
static w_palette_entry rows[palette_limit];
static int row_count;
static palette_callback select_entry;

////////////////
//~ Window matching

static bool w_matches(const wchar_t *s, const wchar_t *q) {
    while (*s && *q) {
        if (towlower(*s) == towlower(*q))
            ++q;
        ++s;
    }
    return !*q;
}

static void w_filter(void) {
    wchar_t q[palette_text_capacity];
    GetWindowTextW(edit, q, count_of(q));
    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < row_count; i++)
        if (w_matches(rows[i].label, q)) {
            LRESULT n = SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)rows[i].label);
            if (n >= 0)
                SendMessageW(list, LB_SETITEMDATA, n, rows[i].id);
        }
    SendMessageW(list, LB_SETCURSEL, 0, 0);
    SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list, NULL, TRUE);
}

bool w_palette_shown(void) {
    return panel && IsWindowVisible(panel);
}

void w_palette_focus(void) {
    if (w_palette_shown()) {
        if (GetForegroundWindow() != panel)
            SetForegroundWindow(panel);
        if (GetForegroundWindow() == panel)
            SetFocus(edit);
    }
}

static void w_dismiss(bool restore_focus) {
    pending_restore = restore_focus && GetForegroundWindow() == panel ? return_to : NULL;
    return_to = NULL;
    ShowWindow(panel, SW_HIDE);
    if (pending_restore)
        PostMessageW(panel, restore_foreground, 0, 0);
}

static void w_restore_window(HWND previous) {
    if (w_palette_shown() || previous == GetDesktopWindow() || !IsWindowVisible(previous) || IsIconic(previous))
        return;
    HWND foreground = GetForegroundWindow();
    if (foreground && foreground != GetDesktopWindow() && IsWindowVisible(foreground) &&
        !(GetWindowLongPtrW(foreground, GWL_EXSTYLE) & WS_EX_NOACTIVATE))
        return;
    HWND popup = GetLastActivePopup(previous);
    if (IsWindowVisible(popup) && IsWindowEnabled(popup))
        previous = popup;
    if (IsWindowEnabled(previous) && !(GetWindowLongPtrW(previous, GWL_EXSTYLE) & WS_EX_NOACTIVATE))
        SetForegroundWindow(previous);
}

static void w_accept(void) {
    LRESULT n = SendMessageW(list, LB_GETCURSEL, 0, 0);
    if (n == LB_ERR)
        return;
    int id = (int)SendMessageW(list, LB_GETITEMDATA, n, 0);
    palette_callback cb = select_entry;
    w_dismiss(false);
    if (cb)
        cb(id);
}

////////////////
//~ Palette window

static LRESULT CALLBACK w_palette_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    LRESULT color_result;
    if (w_theme_message(w, msg, wp, lp, &color_result))
        return color_result;
    if (msg == WM_SETFOCUS) {
        if (edit)
            SetFocus(edit);
        return 0;
    }
    if (msg == WM_COMMAND) {
        if ((HWND)lp == edit && HIWORD(wp) == EN_CHANGE)
            w_filter();
        if ((HWND)lp == list && HIWORD(wp) == LBN_SELCHANGE)
            SetFocus(edit);
        if ((HWND)lp == list && HIWORD(wp) == LBN_DBLCLK)
            w_accept();
        return 0;
    }
    if (msg == WM_SIZE) {
        RECT r;
        GetClientRect(w, &r);
        MoveWindow(edit, 8, 8, r.right - 16, 26, TRUE);
        MoveWindow(list, 8, 42, r.right - 16, r.bottom - 50, TRUE);
        return 0;
    }
    if (msg == restore_foreground) {
        HWND previous = pending_restore;
        pending_restore = NULL;
        if (previous)
            w_restore_window(previous);
        return 0;
    }
    if (msg == WM_CLOSE) {
        w_dismiss(true);
        return 0;
    }
    if (msg == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE)
        PostMessageW(w, check_deactivation, 0, 0);
    if (msg == check_deactivation) {
        // Activation transfers across input queues asynchronously. Check the
        // settled foreground before dismissing an open or just-reinvoked Run.
        if (IsWindowVisible(w) && GetForegroundWindow() != w) {
            return_to = NULL;
            ShowWindow(w, SW_HIDE);
        }
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

void w_palette_open(HMONITOR monitor, const wchar_t *title, const w_palette_entry *entries, int count,
                  palette_callback cb) {
    pending_restore = NULL;
    bool preserve = w_palette_visible(title);
    if (!panel) {
        if (!edit_library)
            edit_library = LoadLibraryExW(L"msftedit.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!edit_library)
            return;
        HINSTANCE instance = GetModuleHandleW(NULL);
        WNDCLASSW wc = {.hInstance = instance,
                        .lpszClassName = L"WinwmPalette",
                        .lpfnWndProc = w_palette_proc,
                        .hCursor = LoadCursorW(NULL, IDC_ARROW),
                        .hbrBackground = NULL};
        RegisterClassW(&wc);
        panel = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, wc.lpszClassName, title,
                                WS_CAPTION | WS_SYSMENU | WS_THICKFRAME, CW_USEDEFAULT, 0, 520, 380, NULL,
                                NULL, instance, NULL);
        if (!panel)
            return;
        edit = CreateWindowExW(WS_EX_CLIENTEDGE, MSFTEDIT_CLASS, L"",
                               WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_NOOLEDRAGDROP, 8, 8, 480, 26,
                               panel, NULL, instance, NULL);
        list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                               WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL, 8, 42, 480, 280, panel, NULL,
                               instance, NULL);
        if (!edit || !list) {
            DestroyWindow(panel);
            panel = edit = list = NULL;
            return;
        }
        SendMessageW(edit, EM_SETTEXTMODE, TM_PLAINTEXT, 0);
        SendMessageW(edit, EM_EXLIMITTEXT, 0, palette_text_capacity - 1);
        SendMessageW(edit, EM_SETEVENTMASK, 0, SendMessageW(edit, EM_GETEVENTMASK, 0, 0) | ENM_CHANGE);
        SendMessageW(edit, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
        SendMessageW(list, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    }
    w_theme_rich_edit(edit);
    row_count = w_clamp_int(count, 0, palette_limit);
    memcpy(rows, entries, row_count * sizeof(*rows));
    select_entry = cb;
    MONITORINFO mi = {.cbSize = sizeof(mi)};
    if (!monitor || !GetMonitorInfoW(monitor, &mi))
        GetMonitorInfoW(MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY), &mi);
    SetWindowTextW(panel, title);
    if (!preserve)
        SetWindowTextW(edit, L"");
    LRESULT selected = preserve ? SendMessageW(list, LB_GETCURSEL, 0, 0) : LB_ERR;
    wchar_t label[palette_text_capacity] = {0};
    if (selected != LB_ERR)
        SendMessageW(list, LB_GETTEXT, selected, (LPARAM)label);
    w_filter();
    if (preserve) {
        if (*label)
            SendMessageW(list, LB_SELECTSTRING, -1, (LPARAM)label);
        return;
    }
    if (!w_palette_shown())
        return_to = GetForegroundWindow();
    SetWindowPos(panel, HWND_TOPMOST, (mi.rcWork.left + mi.rcWork.right - 520) / 2,
                 (mi.rcWork.top + mi.rcWork.bottom - 380) / 2, 520, 380, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    w_palette_focus();
}

bool w_palette_visible(const wchar_t *title) {
    wchar_t s[64];
    return panel && IsWindowVisible(panel) && GetWindowTextW(panel, s, count_of(s)) && !wcscmp(s, title);
}

static void w_delete_word(bool backwards) {
    CHARRANGE selection;
    SendMessageW(edit, EM_EXGETSEL, 0, (LPARAM)&selection);
    if (selection.cpMin == selection.cpMax) {
        LONG caret = selection.cpMin;
        wchar_t query[palette_text_capacity];
        int len = GetWindowTextW(edit, query, count_of(query));
        if (len == 0 || (backwards && caret <= 0) || (!backwards && caret >= len))
            return;
        SCRIPT_STRING_ANALYSIS analysis;
        if (FAILED(ScriptStringAnalyse(NULL, query, len, 0, -1, SSA_BREAK, 0, NULL, NULL, NULL,
                                       NULL, NULL, &analysis)))
            return;
        const SCRIPT_LOGATTR *attr = ScriptString_pLogAttr(analysis);
        LONG boundary = w_clamp_int(caret, 0, len);
        if (attr) {
            for (;;) {
                boundary += backwards ? -1 : 1;
                if (boundary == 0 || boundary == len)
                    break;
                bool starts_word = attr[boundary].fWordStop || attr[boundary - 1].fWhiteSpace;
                if (starts_word && !attr[boundary].fWhiteSpace)
                    break;
            }
        }
        ScriptStringFree(&analysis);
        if (boundary == caret)
            return;
        if (backwards)
            selection.cpMin = boundary;
        else
            selection.cpMax = boundary;
        SendMessageW(edit, EM_EXSETSEL, 0, (LPARAM)&selection);
    }
    SendMessageW(edit, EM_REPLACESEL, TRUE, (LPARAM)L"");
}

bool w_palette_message(MSG *msg) {
    if (!panel || !IsWindowVisible(panel) || !(msg->hwnd == panel || IsChild(panel, msg->hwnd)) ||
        msg->message != WM_KEYDOWN)
        return false;
    bool ctrl = GetKeyState(VK_CONTROL) < 0;
    if (GetKeyState(VK_SHIFT) < 0 || GetKeyState(VK_MENU) < 0)
        return false;
    if (ctrl) {
        if (msg->hwnd != edit || (msg->wParam != VK_BACK && msg->wParam != VK_DELETE))
            return false;
        w_delete_word(msg->wParam == VK_BACK);
        return true;
    }
    switch (msg->wParam) {
    case VK_ESCAPE:
        w_dismiss(true);
        return true;
    case VK_RETURN:
        w_accept();
        return true;
    case VK_UP:
    case VK_DOWN: {
        int n = (int)SendMessageW(list, LB_GETCURSEL, 0, 0), len = (int)SendMessageW(list, LB_GETCOUNT, 0, 0);
        if (len)
            SendMessageW(list, LB_SETCURSEL, w_clamp_int(n + (msg->wParam == VK_UP ? -1 : 1), 0, len - 1), 0);
        return true;
    }
    }
    return false;
}
