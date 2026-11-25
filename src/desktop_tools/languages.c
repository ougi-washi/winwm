#include "desktop_tools/tools.h"
#include "theme/theme.h"
#include <commctrl.h>
#include <stdio.h>
#include <wchar.h>

typedef struct w_input_choice {
    wchar_t id[LOCALE_NAME_MAX_LENGTH], name[192], native[128];
} w_input_choice;

static w_input_choice *choices;
static size_t used, capacity;
static HWND panel, category, search, results, add, make_default, status;
static HFONT font;
static HANDLE applying;
static bool keyboards;
static int dpi;

static int px(int n) { return MulDiv(n, dpi, 96); }

static bool w_add_choice(const w_input_choice *choice) {
    if (used == capacity) {
        size_t next = capacity ? capacity * 2 : 256;
        w_input_choice *more = realloc(choices, next * sizeof(*choices));
        if (!more)
            return false;
        choices = more;
        capacity = next;
    }
    choices[used++] = *choice;
    return true;
}

static BOOL CALLBACK w_locale(LPWSTR tag, DWORD flags, LPARAM unused) {
    (void)unused;
    if (!*tag || (flags & (LOCALE_NEUTRALDATA | LOCALE_ALTERNATE_SORTS)) ||
        wcslen(tag) >= LOCALE_NAME_MAX_LENGTH ||
        tag[wcsspn(tag, L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-")])
        return TRUE;
    w_input_choice choice = {0};
    if (GetLocaleInfoEx(tag, LOCALE_SLOCALIZEDDISPLAYNAME, choice.name, count_of(choice.name))) {
        wcscpy(choice.id, tag);
        GetLocaleInfoEx(tag, LOCALE_SNATIVEDISPLAYNAME, choice.native, count_of(choice.native));
        return w_add_choice(&choice);
    }
    return TRUE;
}

static void w_keyboards(void) {
    HKEY layouts;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Keyboard Layouts",
                      0, KEY_READ, &layouts) != ERROR_SUCCESS)
        return;
    for (DWORD i = 0; ; i++) {
        w_input_choice choice = {0};
        DWORD length = count_of(choice.id);
        LSTATUS ret = RegEnumKeyExW(layouts, i, choice.id, &length, NULL, NULL, NULL, NULL);
        if (ret == ERROR_MORE_DATA)
            continue;
        if (ret != ERROR_SUCCESS)
            break;
        if (length == 8 && wcsspn(choice.id, L"0123456789abcdefABCDEF") == 8 &&
            w_os_keyboard_name(layouts, choice.id, choice.name, count_of(choice.name))) {
            LANGID lang = LOWORD(wcstoul(choice.id, NULL, 16));
            GetLocaleInfoW(lang, LOCALE_SLOCALIZEDDISPLAYNAME, choice.native, count_of(choice.native));
            if (!w_add_choice(&choice))
                break;
        }
    }
    RegCloseKey(layouts);
}

static bool w_contains(const wchar_t *text, const wchar_t *query) {
    return !*query || FindNLSStringEx(LOCALE_NAME_USER_DEFAULT, FIND_FROMSTART | NORM_IGNORECASE |
           NORM_IGNORENONSPACE, text, -1, query, -1, NULL, NULL, NULL, 0) >= 0;
}

static void w_filter_choices(void) {
    wchar_t query[128];
    GetWindowTextW(search, query, count_of(query));
    SendMessageW(results, WM_SETREDRAW, FALSE, 0);
    SendMessageW(results, LB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < used; i++) {
        const w_input_choice *c = &choices[i];
        if (!w_contains(c->name, query) && !w_contains(c->id, query) && !w_contains(c->native, query))
            continue;
        wchar_t row[300];
        swprintf(row, count_of(row), L"%ls   \u00b7   %ls", c->name, keyboards && *c->native ? c->native : c->id);
        LRESULT n = SendMessageW(results, LB_ADDSTRING, 0, (LPARAM)row);
        if (n >= 0)
            SendMessageW(results, LB_SETITEMDATA, n, i);
    }
    bool any = SendMessageW(results, LB_SETCURSEL, 0, 0) != LB_ERR;
    SendMessageW(results, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(results, NULL, TRUE);
    EnableWindow(add, any && !applying);
    EnableWindow(make_default, any && !applying);
    if (!applying)
        SetWindowTextW(status, any ? L"" : L"No matches");
}

static void w_load_choices(void) {
    used = 0;
    keyboards = SendMessageW(category, CB_GETCURSEL, 0, 0) == 1;
    if (keyboards)
        w_keyboards();
    else
        EnumSystemLocalesEx(w_locale, LOCALE_ALL, 0, NULL);
    SendMessageW(search, EM_SETCUEBANNER, TRUE, (LPARAM)(keyboards ? L"Search keyboard layouts" : L"Search languages or region codes"));
    SetWindowTextW(add, keyboards ? L"Add keyboard" : L"Add language");
    ShowWindow(make_default, keyboards ? SW_SHOW : SW_HIDE);
    SetWindowTextW(search, L"");
    w_filter_choices();
    SetFocus(search);
}

// Append to the user's existing International language objects. Recreating the
// list from tags would discard custom keyboard tips and the preference order.
static HANDLE w_append_language(const wchar_t *tag) {
    wchar_t system[MAX_PATH], exe[MAX_PATH], line[2048];
    UINT n = GetSystemDirectoryW(system, count_of(system));
    if (!n || n >= count_of(system) ||
        swprintf(exe, count_of(exe), L"%ls\\WindowsPowerShell\\v1.0\\powershell.exe", system) < 0)
        return NULL;
    // tag comes exclusively from the validated Windows locale enumeration.
    int len = swprintf(line, count_of(line),
        L"\"%ls\" -NoLogo -NoProfile -NonInteractive -Command \""
        L"$ErrorActionPreference='Stop'; $lock=[Threading.Mutex]::new($false,'Local\\winwm.InputLanguages'); "
        L"$owned=$false; try { try { $owned=$lock.WaitOne(15000) } "
        L"catch [Threading.AbandonedMutexException] { $owned=$true }; if (!$owned) {exit 1}; "
        L"$list=Get-WinUserLanguageList; "
        L"$new=(New-WinUserLanguageList -Language '%ls')[0]; "
        L"if ($list.LanguageTag -contains $new.LanguageTag) {exit 2}; "
        L"$list.Add($new); Set-WinUserLanguageList -LanguageList $list -Force; "
        L"if ((Get-WinUserLanguageList).LanguageTag -contains $new.LanguageTag) {exit 0}; exit 1 "
        L"} catch {exit 1} finally { if ($owned) {$lock.ReleaseMutex()}; $lock.Dispose() }\"", exe, tag);
    if (len < 0 || (size_t)len >= count_of(line))
        return NULL;
    STARTUPINFOW si = {.cb = sizeof(si)};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(exe, line, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        return NULL;
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

static void w_apply_choice(bool set_default) {
    LRESULT row = SendMessageW(results, LB_GETCURSEL, 0, 0);
    if (row < 0 || applying)
        return;
    size_t i = (size_t)SendMessageW(results, LB_GETITEMDATA, row, 0);
    if (i >= used)
        return;
    if (keyboards) {
        wchar_t tip[32];
        swprintf(tip, count_of(tip), L"0x%04x:0x%ls", LOWORD(wcstoul(choices[i].id,NULL,16)), choices[i].id);
        HMODULE input = LoadLibraryExW(L"input.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        typedef BOOL (WINAPI *update_fn)(LPCWSTR, DWORD);
        update_fn install = input ? (update_fn)GetProcAddress(input, "InstallLayoutOrTip") : NULL;
        update_fn set = input ? (update_fn)GetProcAddress(input, "SetDefaultLayoutOrTip") : NULL;
        bool enabled = install && install(tip, 0);
        const wchar_t *message = enabled ? L"Keyboard enabled. Select it from the toolbar." : L"Windows could not enable this keyboard.";
        if (set_default && enabled)
            message = set && set(tip, 0) ? L"Default keyboard updated." : L"Keyboard enabled. Windows could not set the default.";
        if (input)
            FreeLibrary(input);
        SetWindowTextW(status, message);
    } else {
        applying = w_append_language(choices[i].id);
        if (!applying) {
            SetWindowTextW(status, L"Windows language setup could not start.");
            return;
        }
        EnableWindow(category, FALSE);
        EnableWindow(search, FALSE);
        EnableWindow(results, FALSE);
        EnableWindow(add, FALSE);
        SetWindowTextW(status, L"Adding language\u2026");
    }
}

static LRESULT CALLBACK w_setup_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    LRESULT themed;
    if (w_theme_message(w, msg, wp, lp, &themed))
        return themed;
    if (msg == WM_COMMAND) {
        HWND source = (HWND)lp;
        if (source == category && HIWORD(wp) == CBN_SELCHANGE)
            w_load_choices();
        else if (source == search && HIWORD(wp) == EN_CHANGE)
            w_filter_choices();
        else if ((source == add || source == make_default) && HIWORD(wp) == BN_CLICKED)
            w_apply_choice(source == make_default);
        return 0;
    }
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

static HWND w_control(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h) {
    DWORD tab = !wcscmp(cls, L"STATIC") ? 0 : WS_TABSTOP;
    HWND control = CreateWindowW(cls, text, WS_CHILD | WS_VISIBLE | tab | style, px(x), px(y),
                                 px(w), px(h), panel, NULL, GetModuleHandleW(NULL), NULL);
    SendMessageW(control, WM_SETFONT, (WPARAM)font, TRUE);
    return control;
}

int w_language_setup(bool start_keyboards) {
    HINSTANCE instance = GetModuleHandleW(NULL);
    WNDCLASSW wc = {.lpfnWndProc = w_setup_proc, .hInstance = instance,
                    .hCursor = LoadCursorW(NULL, IDC_ARROW), .lpszClassName = L"WinwmInputSetup"};
    RegisterClassW(&wc);
    HWND foreground = GetForegroundWindow();
    dpi = foreground ? (int)GetDpiForWindow(foreground) : 96;
    if (!dpi) dpi = 96;
    MONITORINFO mi = {.cbSize = sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(foreground, MONITOR_DEFAULTTOPRIMARY), &mi);
    RECT r = {0, 0, px(500), px(374)};
    DWORD style = WS_CAPTION | WS_SYSMENU;
    AdjustWindowRectExForDpi(&r, style, FALSE, WS_EX_TOOLWINDOW, dpi);
    int width = r.right - r.left, height = r.bottom - r.top;
    panel = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"Input setup", style,
                            (mi.rcWork.left + mi.rcWork.right - width)/2, (mi.rcWork.top + mi.rcWork.bottom - height)/2,
                            width, height, NULL, NULL, instance, NULL);
    if (!panel)
        return 1;
    font = CreateFontW(-px(15),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
    category = w_control(L"COMBOBOX", L"Category", CBS_DROPDOWNLIST, 16, 15, 145, 120);
    search = w_control(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 171, 15, 312, 28);
    results = w_control(L"LISTBOX", L"Available choices", WS_VSCROLL | LBS_SORT | LBS_NOINTEGRALHEIGHT |
                        LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
                        16, 56, 467, 230);
    status = w_control(L"STATIC", L"", SS_LEFT, 16, 293, 467, 31);
    add = w_control(L"BUTTON", L"Add language", BS_OWNERDRAW, 16, 329, 150, 32);
    make_default = w_control(L"BUTTON", L"Set as default", BS_OWNERDRAW, 174, 329, 150, 32);
    if (!category || !search || !results || !status || !add || !make_default)
        return 1;
    SendMessageW(category, CB_ADDSTRING, 0, (LPARAM)L"Languages");
    SendMessageW(category, CB_ADDSTRING, 0, (LPARAM)L"Keyboard layouts");
    SendMessageW(category, CB_SETCURSEL, start_keyboards, 0);
    w_load_choices();
    ShowWindow(panel, SW_SHOW);
    SetForegroundWindow(panel);
    SetFocus(search);
    MSG msg;
    bool done = false;
    while (!done) {
        DWORD wait = MsgWaitForMultipleObjectsEx(applying ? 1 : 0, &applying, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (wait == WAIT_FAILED) break;
        if (applying && wait == WAIT_OBJECT_0) {
            DWORD code = 1;
            GetExitCodeProcess(applying, &code);
            CloseHandle(applying);
            applying = NULL;
            EnableWindow(category, TRUE);
            EnableWindow(search, TRUE);
            EnableWindow(results, TRUE);
            EnableWindow(add, TRUE);
            SetWindowTextW(status, code == 0 ? L"Language added. Select its input layout from the toolbar."
                                 : code == 2 ? L"This language is already in your preferred languages."
                                             : L"Windows could not add this language.");
            SetFocus(search);
        }
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { done = true; break; }
            if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE && !SendMessageW(category, CB_GETDROPPEDSTATE,0,0)) {
                DestroyWindow(panel);
            } else if (msg.message == WM_KEYDOWN && msg.hwnd == search && msg.wParam == VK_DOWN) {
                SetFocus(results);
            } else if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN && (msg.hwnd == results || msg.hwnd == search)) {
                w_apply_choice(false);
            } else if (!IsDialogMessageW(panel, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }
    if (applying) CloseHandle(applying);
    DeleteObject(font);
    free(choices);
    return 0;
}
