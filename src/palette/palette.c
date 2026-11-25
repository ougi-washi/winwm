#include "theme/theme.h"
#include "palette/palette.h"
#include <richedit.h>
#include <commctrl.h>
#include <windowsx.h>
#include <usp10.h>
#include <shellscalingapi.h>
#include <uxtheme.h>
#include <wctype.h>
#include <stdlib.h>
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
static bool manual_selection;
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

static void w_search_text(wchar_t *out, const wchar_t *text) {
    int n = 0;
    bool space = false;
    for (; *text; text++) {
        if (iswspace(*text) || *text == L'_' || *text == L'-') {
            space = n != 0;
            continue;
        }
        if (space)
            out[n++] = L' ';
        space = false;
        out[n++] = *text;
    }
    out[n] = 0;
    CharLowerBuffW(out, n);
}

typedef struct w_query_word {
    const wchar_t *start;
    size_t length;
} w_query_word;

// Match the longest prefixes first so that "man manage" can use two distinct words.
static int w_query_words(const wchar_t *query, w_query_word *words) {
    int count = 0;
    while (*query) {
        size_t length = wcscspn(query, L" ");
        int i = count++;
        while (i > 0 && words[i - 1].length < length) {
            words[i] = words[i - 1];
            i--;
        }
        words[i] = (w_query_word){query, length};
        query += length;
        if (*query)
            query++;
    }
    return count;
}

static bool w_word_start(const wchar_t *text, const wchar_t *position) {
    return position == text || !IsCharAlphaNumericW(position[-1]);
}

static int w_match_tier(const wchar_t *text, const wchar_t *query, const w_query_word *words,
                        int count, int *cost) {
    if (!wcscmp(text, query))
        return 0;
    *cost = (int)wcslen(text);
    const wchar_t *hit = wcsstr(text, query);
    if (hit == text)
        return 1;
    for (const wchar_t *p = hit; p; p = wcsstr(p + 1, query)) {
        if (w_word_start(text, p)) {
            *cost += (int)(p - text);
            return 2;
        }
    }
    if (count > 1) {
        bool used[palette_text_capacity] = {0};
        int i;
        for (i = 0; i < count; i++) {
            const wchar_t *p = text;
            while (*p && (used[p - text] || !w_word_start(text, p) ||
                          wcsncmp(p, words[i].start, words[i].length)))
                p++;
            if (!*p)
                break;
            used[p - text] = true;
        }
        if (i == count)
            return 3;
    }
    if (hit) {
        *cost += (int)(hit - text);
        return 4;
    }
    const wchar_t *initials = query, *fuzzy = query;
    int gaps = 0;
    for (const wchar_t *p = text; *p; p++) {
        if (*initials && w_word_start(text, p) && *p == *initials)
            initials++;
        if (*fuzzy) {
            if (*p == *fuzzy) {
                fuzzy++;
                if (*fuzzy == L' ')
                    fuzzy++;
            } else {
                gaps++;
            }
        }
    }
    if (!*initials)
        return 5;
    *cost += gaps;
    return *fuzzy ? -1 : 6;
}

typedef struct w_result {
    int row, tier, cost;
} w_result;

static int w_compare_result(const void *left, const void *right) {
    const w_result *a = left, *b = right;
    if (a->tier != b->tier)
        return a->tier - b->tier;
    if (a->cost != b->cost)
        return a->cost - b->cost;
    int order = CompareStringOrdinal(rows[a->row].label, -1, rows[b->row].label, -1, TRUE);
    return order == CSTR_EQUAL ? a->row - b->row : order - CSTR_EQUAL;
}

static void w_filter(void) {
    pressed_result = -1;
    manual_selection = false;
    wchar_t input[palette_text_capacity], query[palette_text_capacity];
    GetWindowTextW(edit, input, count_of(input));
    w_search_text(query, input);
    w_query_word words[palette_text_capacity / 2];
    int word_count = w_query_words(query, words);
    w_result results[palette_limit];
    int matches = 0;
    for (int i = 0; i < row_count; i++) {
        int tier = 0, cost = 0;
        if (word_count) {
            wchar_t text[palette_text_capacity];
            w_search_text(text, rows[i].label);
            tier = w_match_tier(text, query, words, word_count, &cost);
        }
        if (tier >= 0)
            results[matches++] = (w_result){i, tier, cost};
    }
    if (word_count)
        qsort(results, matches, sizeof(*results), w_compare_result);
    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    for (int j = 0; j < matches; j++) {
        const w_palette_entry *entry = &rows[results[j].row];
        LRESULT n = SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)entry->label);
        if (n >= 0)
            SendMessageW(list, LB_SETITEMDATA, n, entry->id);
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
        if ((HWND)lp == list && HIWORD(wp) == LBN_SELCHANGE) {
            manual_selection = true;
            SetFocus(edit);
        }
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
    LRESULT selected = preserve && manual_selection ? SendMessageW(list, LB_GETCURSEL, 0, 0) : LB_ERR;
    wchar_t label[palette_text_capacity] = {0};
    if (selected != LB_ERR)
        SendMessageW(list, LB_GETTEXT, selected, (LPARAM)label);
    w_filter();
    if (preserve) {
        if (*label) {
            LRESULT found = SendMessageW(list, LB_FINDSTRINGEXACT, -1, (LPARAM)label);
            if (found != LB_ERR) {
                SendMessageW(list, LB_SETCURSEL, found, 0);
                manual_selection = true;
            }
        }
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
        if (len) {
            SendMessageW(list, LB_SETCURSEL, w_clamp_int(n + (msg->wParam == VK_UP ? -1 : 1), 0, len - 1), 0);
            manual_selection = true;
        }
        return true;
    }
    }
    return false;
}
