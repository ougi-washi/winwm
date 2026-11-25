#include "theme/theme.h"
#include "palette/palette.h"
#include <richedit.h>
#include <commctrl.h>
#include <windowsx.h>
#include <usp10.h>
#include <shellscalingapi.h>
#include <uxtheme.h>
#include <wctype.h>
enum { check_deactivation = WM_APP + 1, restore_foreground };

////////////////
//~ Palette

static HWND panel, search_label, edit, list, return_to, pending_restore;
static HMODULE edit_library;
static HFONT text_font;
static UINT palette_dpi;
static w_palette_entry rows[palette_limit];
static int row_count;
static int pressed_result = -1;
static palette_callback select_entry;

static void w_palette_size(void) {
    if (!edit || !list)
        return;
    UINT dpi = GetDpiForWindow(panel);
    if (!dpi)
        dpi = 96;
    if (dpi != palette_dpi) {
        HFONT font = CreateFontW(-MulDiv(15, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        if (font) {
            SendMessageW(edit, WM_SETFONT, (WPARAM)font, TRUE);
            SendMessageW(list, WM_SETFONT, (WPARAM)font, TRUE);
            SendMessageW(search_label, WM_SETFONT, (WPARAM)font, TRUE);
            DeleteObject(text_font);
            text_font = font;
            palette_dpi = dpi;
        }
        SendMessageW(list, LB_SETITEMHEIGHT, 0, MulDiv(30, dpi, 96));
    }
    RECT r;
    GetClientRect(panel, &r);
    int pad = MulDiv(10, dpi, 96), field = MulDiv(28, dpi, 96), top = 2*pad + field;
    int width = max(1, r.right - 2*pad);
    int label_width = MulDiv(56, dpi, 96);
    MoveWindow(search_label, 2*pad, pad, label_width, field, TRUE);
    MoveWindow(edit, 2*pad + label_width, pad + MulDiv(5, dpi, 96), max(1, width - label_width - 2*pad),
               field - MulDiv(6, dpi, 96), TRUE);
    MoveWindow(list, pad, top, width, max(1, r.bottom - top - pad), TRUE);
    InvalidateRect(panel, NULL, TRUE);
}

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
    pressed_result = -1;
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

static LRESULT CALLBACK w_results_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR subclass, DWORD_PTR data) {
    (void)subclass;
    (void)data;
    if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP) {
        POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        LRESULT hit = SendMessageW(w, LB_ITEMFROMPOINT, 0, lp);
        int index = LOWORD(hit), id = -1;
        RECT item;
        if (!HIWORD(hit) && SendMessageW(w, LB_GETITEMRECT, index, (LPARAM)&item) != LB_ERR && PtInRect(&item, p))
            id = (int)SendMessageW(w, LB_GETITEMDATA, index, 0);
        if (msg == WM_LBUTTONDOWN)
            pressed_result = id;
        else {
            bool pick = id >= 0 && pressed_result == id;
            LRESULT result = DefSubclassProc(w, msg, wp, lp);
            pressed_result = -1;
            if (pick && w_palette_shown()) {
                SendMessageW(w, LB_SETCURSEL, index, 0);
                w_accept();
            }
            return result;
        }
    } else if (msg == WM_CANCELMODE || msg == WM_CAPTURECHANGED)
        pressed_result = -1;
    return DefSubclassProc(w, msg, wp, lp);
}

////////////////
//~ Palette window

static LRESULT CALLBACK w_palette_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    LRESULT color_result;
    if (w_theme_message(w, msg, wp, lp, &color_result))
        return color_result;
    if (msg == WM_PAINT && list) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        RECT r;
        GetWindowRect(list, &r);
        MapWindowPoints(HWND_DESKTOP, w, (POINT *)&r, 2);
        r.top -= MulDiv(5, palette_dpi, 96);
        r.bottom = r.top + 1;
        SetDCBrushColor(dc, w_theme_separator());
        FillRect(dc, &r, GetStockObject(DC_BRUSH));
        EndPaint(w, &ps);
        return 0;
    }
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
        return 0;
    }
    if (msg == WM_SIZE) {
        w_palette_size();
        return 0;
    }
    if (msg == WM_DPICHANGED) {
        RECT *r = (RECT *)lp;
        SetWindowPos(w, NULL, r->left, r->top, r->right-r->left, r->bottom-r->top, SWP_NOACTIVATE | SWP_NOZORDER);
        w_palette_size();
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
                                WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_CLIPCHILDREN, CW_USEDEFAULT, 0, 520, 380, NULL,
                                NULL, instance, NULL);
        if (!panel)
            return;
        search_label = CreateWindowW(L"STATIC", L"Search", WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                                     0, 0, 0, 0, panel, NULL, instance, NULL);
        edit = CreateWindowExW(0, MSFTEDIT_CLASS, L"",
                               WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_NOOLEDRAGDROP, 8, 8, 480, 26,
                               panel, NULL, instance, NULL);
        list = CreateWindowExW(0, L"LISTBOX", L"",
                               WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
                               8, 42, 480, 280, panel, NULL,
                               instance, NULL);
        if (!edit || !list || !search_label || !SetWindowSubclass(list, w_results_proc, 1, 0)) {
            DestroyWindow(panel);
            panel = edit = list = search_label = NULL;
            return;
        }
        SetWindowTheme(list, L"DarkMode_Explorer", NULL);
        SendMessageW(edit, EM_SETTEXTMODE, TM_PLAINTEXT, 0);
        SendMessageW(edit, EM_EXLIMITTEXT, 0, palette_text_capacity - 1);
        SendMessageW(edit, EM_SETEVENTMASK, 0, SendMessageW(edit, EM_GETEVENTMASK, 0, 0) | ENM_CHANGE);
        w_palette_size();
    }
    w_theme_rich_edit(edit);
    row_count = w_clamp_int(count, 0, palette_limit);
    memcpy(rows, entries, row_count * sizeof(*rows));
    select_entry = cb;
    MONITORINFO mi = {.cbSize = sizeof(mi)};
    if (!monitor || !GetMonitorInfoW(monitor, &mi)) {
        monitor = MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY);
        GetMonitorInfoW(monitor, &mi);
    }
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
    UINT dpi = 96, vertical = 96;
    GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi, &vertical);
    int width = min(mi.rcWork.right-mi.rcWork.left, MulDiv(520, dpi, 96));
    int height = min(mi.rcWork.bottom-mi.rcWork.top, MulDiv(380, dpi, 96));
    SetWindowPos(panel, HWND_TOPMOST, (mi.rcWork.left + mi.rcWork.right - width) / 2,
                 (mi.rcWork.top + mi.rcWork.bottom - height) / 2, width, height, SWP_SHOWWINDOW | SWP_NOACTIVATE);
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
