#define COBJMACROS
#include "desktop_tools/tools.h"
#include "theme/theme.h"
#include "theme/icon_button.h"
#include <commctrl.h>
#include <imm.h>
#include <msctf.h>
#include <oleauto.h>
#include <shellapi.h>
#include <stdio.h>
#include <wchar.h>

typedef struct w_input_choice {
    wchar_t id[LOCALE_NAME_MAX_LENGTH], name[192], native[128];
    CLSID service;
    GUID profile;
    LANGID lang;
    bool enabled;
} w_input_choice;

static w_input_choice *choices;
static size_t used, capacity;
static HWND panel, category, search, results, add, make_default, status, remove_lang, typing, test_label, methods, basic;
static HFONT font;
static HANDLE applying;
static ITfInputProcessorProfiles *profiles;
static ITfThreadMgr *thread_manager;
static wchar_t *preferred;
static size_t preferred_count;
typedef enum w_input_operation { language_add, language_remove, basic_typing } w_input_operation;
static w_input_operation operation;
static w_input_page page;
static int dpi;

static int px(int n) { return MulDiv(n, dpi, 96); }

static void w_read_preferred(void) {
    free(preferred);
    preferred = NULL;
    preferred_count = 0;
    const wchar_t *key = L"Control Panel\\International\\User Profile";
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, key, L"Languages", RRF_RT_REG_MULTI_SZ, NULL, NULL, &size) != ERROR_SUCCESS)
        return;
    preferred = calloc((size / sizeof(wchar_t)) + 2, sizeof(wchar_t));
    if (!preferred) return;
    if (RegGetValueW(HKEY_CURRENT_USER, key, L"Languages", RRF_RT_REG_MULTI_SZ, NULL, preferred, &size) != ERROR_SUCCESS)
        *preferred = 0;
    for (const wchar_t *tag = preferred; *tag; tag += wcslen(tag) + 1)
        preferred_count++;
}

// International may persist a neutral tag (ja) for its default region (ja-JP).
// Resolve only neutral tags; keep every explicitly selected region distinct.
static const wchar_t *w_specific_tag(const wchar_t *tag, wchar_t resolved[LOCALE_NAME_MAX_LENGTH]) {
    DWORD neutral = 0;
    if (GetLocaleInfoEx(tag, LOCALE_INEUTRAL | LOCALE_RETURN_NUMBER, (wchar_t *)&neutral,
                        sizeof(neutral)/sizeof(wchar_t)) && neutral &&
        ResolveLocaleName(tag, resolved, LOCALE_NAME_MAX_LENGTH)) return resolved;
    return tag;
}

static bool w_same_language(const wchar_t *a, const wchar_t *b) {
    if (!_wcsicmp(a, b)) return true;
    wchar_t first[LOCALE_NAME_MAX_LENGTH], second[LOCALE_NAME_MAX_LENGTH];
    return !_wcsicmp(w_specific_tag(a, first), w_specific_tag(b, second));
}

static bool w_preferred(const wchar_t *id) {
    for (const wchar_t *tag = preferred; tag && *tag; tag += wcslen(tag) + 1)
        if (w_same_language(tag, id)) return true;
    return false;
}

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
        choice.enabled = w_preferred(tag);
        GetLocaleInfoEx(tag, LOCALE_SNATIVEDISPLAYNAME, choice.native, count_of(choice.native));
        return w_add_choice(&choice);
    }
    return TRUE;
}

static void w_input_methods(void) {
    ITfInputProcessorProfileMgr *manager = NULL;
    if (!profiles || FAILED(ITfInputProcessorProfiles_QueryInterface(profiles, &IID_ITfInputProcessorProfileMgr,
                                                                     (void **)&manager))) return;
    IEnumTfInputProcessorProfiles *list = NULL;
    HRESULT hr = ITfInputProcessorProfileMgr_EnumProfiles(manager, 0, &list);
    ITfInputProcessorProfileMgr_Release(manager);
    if (FAILED(hr) || !list) return;
    TF_INPUTPROCESSORPROFILE p;
    ULONG fetched;
    while (IEnumTfInputProcessorProfiles_Next(list, 1, &p, &fetched) == S_OK) {
        if (p.dwProfileType != TF_PROFILETYPE_INPUTPROCESSOR || !IsEqualGUID(&p.catid, &GUID_TFCAT_TIP_KEYBOARD))
            continue;
        BSTR label = NULL;
        if (FAILED(ITfInputProcessorProfiles_GetLanguageProfileDescription(profiles, &p.clsid,
                    p.langid, &p.guidProfile, &label)) || !label) continue;
        w_input_choice c = {.service = p.clsid, .profile = p.guidProfile, .lang = p.langid};
        lstrcpynW(c.name, label, count_of(c.name));
        SysFreeString(label);
        LCIDToLocaleName(MAKELCID(p.langid, SORT_DEFAULT), c.id, count_of(c.id), 0);
        GetLocaleInfoW(p.langid, LOCALE_SLOCALIZEDDISPLAYNAME, c.native, count_of(c.native));
        BOOL enabled = FALSE;
        ITfInputProcessorProfiles_IsEnabledLanguageProfile(profiles, &p.clsid, p.langid, &p.guidProfile, &enabled);
        c.enabled = enabled != FALSE;
        if (!w_add_choice(&c)) break;
    }
    IEnumTfInputProcessorProfiles_Release(list);
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

static w_input_choice *w_selected(void) {
    LRESULT row = SendMessageW(results, LB_GETCURSEL, 0, 0);
    if (row == LB_ERR) return NULL;
    size_t i = (size_t)SendMessageW(results, LB_GETITEMDATA, row, 0);
    return i < used ? &choices[i] : NULL;
}

static void w_buttons(void) {
    w_input_choice *c = w_selected();
    bool ready = c && !applying;
    EnableWindow(add, ready && !(page == input_languages && c->enabled));
    EnableWindow(remove_lang, ready && c->enabled && preferred_count > 1);
    EnableWindow(make_default, ready);
    EnableWindow(basic, ready && *c->id);
    EnableWindow(methods, !applying);
    w_icon_button(add, page == input_methods ? L"\uE92E" : L"\uE710",
                  page == input_languages ? L"Add selected language" :
                  page == input_keyboards ? L"Add selected keyboard layout" :
                  c && !c->enabled ? L"Enable and test input method" : L"Test input method");
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
        wchar_t row[384];
        swprintf(row, count_of(row), L"%ls   \u00b7   %ls%ls", c->name,
                 page != input_languages && *c->native ? c->native : c->id,
                 c->enabled ? (page == input_languages ? L"   \u00b7   Added" : L"   \u00b7   Enabled") : L"");
        LRESULT n = SendMessageW(results, LB_ADDSTRING, 0, (LPARAM)row);
        if (n >= 0) SendMessageW(results, LB_SETITEMDATA, n, i);
    }
    bool any = SendMessageW(results, LB_SETCURSEL, 0, 0) != LB_ERR;
    SendMessageW(results, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(results, NULL, TRUE);
    w_buttons();
    if (!applying)
        SetWindowTextW(status, !any ? (page == input_methods && !used ? L"No keyboard input methods are registered for your languages." :
                          L"No matches") : page == input_methods ? L"Select an input method to try it here." :
                          page == input_languages ? L"Added languages appear first." : L"Select a keyboard layout.");
}

static int w_order(const void *left, const void *right) {
    const w_input_choice *a = left, *b = right;
    if (a->enabled != b->enabled) return a->enabled ? -1 : 1;
    return _wcsicmp(a->name, b->name);
}

static void w_layout(void);

static void w_load_choices(bool keep_query) {
    w_input_choice saved = {0};
    w_input_choice *selected = keep_query ? w_selected() : NULL;
    bool restore = selected != NULL;
    if (restore) saved = *selected;
    used = 0;
    page = (w_input_page)SendMessageW(category, CB_GETCURSEL, 0, 0);
    if (page == input_keyboards) w_keyboards();
    else if (page == input_methods) w_input_methods();
    else {
        w_read_preferred();
        EnumSystemLocalesEx(w_locale, LOCALE_ALL, 0, NULL);
        // Keep preferred neutral or custom tags accessible as well.
        for (const wchar_t *tag = preferred; tag && *tag; tag += wcslen(tag) + 1) {
            size_t i = 0;
            while (i < used && !w_same_language(choices[i].id, tag)) i++;
            if (i == used) w_locale((LPWSTR)tag, 0, 0);
        }
    }
    if (used > 1) qsort(choices, used, sizeof(*choices), w_order);
    SendMessageW(search, EM_SETCUEBANNER, TRUE, (LPARAM)(page == input_keyboards ? L"Search keyboard layouts" :
                  page == input_methods ? L"Search input methods or languages" : L"Search languages or region codes"));
    ShowWindow(remove_lang, page == input_languages ? SW_SHOW : SW_HIDE);
    ShowWindow(make_default, page != input_languages ? SW_SHOW : SW_HIDE);
    ShowWindow(methods, page != input_methods ? SW_SHOW : SW_HIDE);
    ShowWindow(typing, page == input_methods ? SW_SHOW : SW_HIDE);
    ShowWindow(test_label, page == input_methods ? SW_SHOW : SW_HIDE);
    ShowWindow(basic, page == input_methods ? SW_SHOW : SW_HIDE);
    if (!keep_query) SetWindowTextW(search, L"");
    w_filter_choices();
    if (restore) {
        LRESULT rows = SendMessageW(results, LB_GETCOUNT, 0, 0);
        for (LRESULT row = 0; row < rows; row++) {
            size_t i = (size_t)SendMessageW(results, LB_GETITEMDATA, row, 0);
            if (i < used && !_wcsicmp(choices[i].id, saved.id) &&
                IsEqualGUID(&choices[i].service, &saved.service) && IsEqualGUID(&choices[i].profile, &saved.profile)) {
                SendMessageW(results, LB_SETCURSEL, row, 0);
                w_buttons();
                break;
            }
        }
    }
    w_layout();
    SetFocus(search);
}

// Keep the original International objects and their input tips, order and options.
static HANDLE w_change_language(const wchar_t *tag, bool remove) {
    wchar_t system[MAX_PATH], exe[MAX_PATH], line[2048];
    UINT n = GetSystemDirectoryW(system, count_of(system));
    if (!*tag || tag[wcsspn(tag, L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-")] ||
        !n || n >= count_of(system) ||
        swprintf(exe, count_of(exe), L"%ls\\WindowsPowerShell\\v1.0\\powershell.exe", system) < 0)
        return NULL;
    int len = swprintf(line, count_of(line),
        L"\"%ls\" -NoLogo -NoProfile -NonInteractive -Command \""
        L"$ErrorActionPreference='Stop'; $lock=[Threading.Mutex]::new($false,'Local\\winwm.InputLanguages'); "
        L"$owned=$false; try { try { $owned=$lock.WaitOne(15000) } "
        L"catch [Threading.AbandonedMutexException] { $owned=$true }; if (!$owned) {exit 1}; "
        L"function Resolve-Tag($t) { try { $c=[cultureinfo]::GetCultureInfo($t); "
        L"if ($c.IsNeutralCulture) {return [cultureinfo]::CreateSpecificCulture($t).Name} } catch {}; return $t }; "
        L"$list=Get-WinUserLanguageList; $tag='%ls'; $key=Resolve-Tag $tag; "
        L"$item=$list | Where-Object {(Resolve-Tag $_.LanguageTag) -eq $key} | Select-Object -First 1; "
        L"%ls Set-WinUserLanguageList -LanguageList $list -Force; "
        L"$keys=@((Get-WinUserLanguageList) | ForEach-Object {Resolve-Tag $_.LanguageTag}); "
        L"if ($keys -%ls $key) {exit 0}; exit 1 "
        L"} catch {exit 1} finally { if ($owned) {$lock.ReleaseMutex()}; $lock.Dispose() }\"", exe, tag,
        remove ? L"if ($null -eq $item) {exit 2}; if ($list.Count -le 1) {exit 3}; [void]$list.Remove($item);"
               : L"if ($null -ne $item) {exit 2}; $list.Add((New-WinUserLanguageList -Language $tag)[0]);",
        remove ? L"notcontains" : L"contains");
    if (len < 0 || (size_t)len >= count_of(line)) return NULL;
    STARTUPINFOW si = {.cb = sizeof(si)};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(exe, line, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return NULL;
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

static void w_try_method(w_input_choice *c) {
    if (!profiles || !thread_manager) {
        SetWindowTextW(status, L"Windows Text Services could not start.");
        return;
    }
    if (!c->enabled) {
        if (ITfInputProcessorProfiles_EnableLanguageProfile(profiles, &c->service, c->lang, &c->profile, TRUE) != S_OK) {
            SetWindowTextW(status, L"Windows could not enable this input method.");
            return;
        }
        w_load_choices(true);
        c = w_selected();
        if (!c || !c->enabled) {
            SetWindowTextW(status, L"Windows did not enable this input method.");
            return;
        }
    }
    SetFocus(typing);
    HRESULT hr = ITfInputProcessorProfiles_ChangeCurrentLanguage(profiles, c->lang);
    if (hr == S_OK)
        hr = ITfInputProcessorProfiles_ActivateLanguageProfile(profiles, &c->service, c->lang, &c->profile);
    GUID active = {0};
    LANGID lang = 0;
    if (hr != S_OK || ITfInputProcessorProfiles_GetActiveLanguageProfile(profiles, &c->service, &lang, &active) != S_OK ||
            lang != c->lang || !IsEqualGUID(&active, &c->profile)) {
        SetWindowTextW(status, L"Windows could not activate this method. Check that its language is added.");
        return;
    }
    if (PRIMARYLANGID(c->lang) == LANG_JAPANESE) {
        HIMC ctx = ImmGetContext(typing);
        if (ctx) {
            DWORD conversion, sentence;
            ImmSetOpenStatus(ctx, TRUE);
            if (ImmGetConversionStatus(ctx, &conversion, &sentence))
                ImmSetConversionStatus(ctx, (conversion & ~IME_CMODE_KATAKANA) | IME_CMODE_NATIVE | IME_CMODE_FULLSHAPE, sentence);
            ImmReleaseContext(typing, ctx);
        }
        SetWindowTextW(status, L"Type kana, then Enter. If it stays Latin, select Hiragana or install basic typing.");
    } else {
        SetWindowTextW(status, L"Input method active. Type in the field below.");
    }
}

static void w_busy(const wchar_t *message) {
    EnableWindow(category, FALSE);
    EnableWindow(search, FALSE);
    EnableWindow(results, FALSE);
    w_buttons();
    SetWindowTextW(status, message);
}

static void w_install_typing(const w_input_choice *c) {
    wchar_t system[MAX_PATH], exe[MAX_PATH], args[1536];
    UINT n = GetSystemDirectoryW(system, count_of(system));
    if (!*c->id || c->id[wcsspn(c->id, L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-")] ||
        !n || n >= count_of(system) ||
        swprintf(exe, count_of(exe), L"%ls\\WindowsPowerShell\\v1.0\\powershell.exe", system) < 0) {
        SetWindowTextW(status, L"Windows language servicing could not start.");
        return;
    }
    int len = swprintf(args, count_of(args),
        L"-NoLogo -NoProfile -NonInteractive -Command \""
        L"$ErrorActionPreference='Stop'; $name='Language.Basic~~~%ls~0.0.1.0'; try { "
        L"$env:PSModulePath=$PSHOME+'\\Modules'; Import-Module ($PSHOME+'\\Modules\\Dism\\Dism.psd1'); "
        L"$cap=Get-WindowsCapability -Online -Name $name; if ($cap.Name -ne $name) {exit 4}; "
        L"if ($cap.State -eq 'Installed') {exit 2}; if ($cap.State -eq 'InstallPending') {exit 5}; "
        L"if ($cap.State -eq 'UninstallPending') {exit 6}; "
        L"$result=Add-WindowsCapability -Online -Name $name; if ($result.RestartNeeded) {exit 5}; "
        L"if ((Get-WindowsCapability -Online -Name $name).State -eq 'Installed') {exit 0}; exit 1 "
        L"} catch {exit $_.Exception.HResult}\"", c->id);
    if (len < 0 || (size_t)len >= count_of(args)) return;
    SHELLEXECUTEINFOW launch = {.cbSize = sizeof(launch), .hwnd = panel, .lpVerb = L"runas",
        .lpFile = exe, .lpParameters = args, .lpDirectory = system, .nShow = SW_HIDE,
        .fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI};
    if (!ShellExecuteExW(&launch) || !launch.hProcess) {
        SetWindowTextW(status, GetLastError() == ERROR_CANCELLED ? L"Basic typing installation canceled." :
                                                               L"Windows could not start language servicing.");
        return;
    }
    applying = launch.hProcess;
    operation = basic_typing;
    w_busy(L"Checking and installing basic typing. Windows may download files.");
}

static void w_apply_choice(HWND action) {
    w_input_choice *c = w_selected();
    if (!c || applying) return;
    if (page == input_methods && action == basic) {
        w_install_typing(c);
        return;
    }
    if (page == input_methods && action == add) {
        w_try_method(c);
        return;
    }
    if (page != input_languages) {
        wchar_t tip[128];
        if (page == input_keyboards)
            swprintf(tip, count_of(tip), L"0x%04x:0x%ls", LOWORD(wcstoul(c->id, NULL, 16)), c->id);
        else {
            wchar_t service[40], profile[40];
            if (!StringFromGUID2(&c->service, service, count_of(service)) ||
                !StringFromGUID2(&c->profile, profile, count_of(profile))) return;
            swprintf(tip, count_of(tip), L"0x%04x:%ls%ls", c->lang, service, profile);
        }
        HMODULE input = LoadLibraryExW(L"input.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        typedef BOOL (WINAPI *update_fn)(LPCWSTR, DWORD);
        update_fn install = input ? (update_fn)GetProcAddress(input, "InstallLayoutOrTip") : NULL;
        update_fn set = input ? (update_fn)GetProcAddress(input, "SetDefaultLayoutOrTip") : NULL;
        bool enabled = install && install(tip, 0);
        const wchar_t *message = enabled ? L"Keyboard enabled. Select it from the toolbar." : L"Windows could not enable this keyboard.";
        if (action == make_default && enabled)
            message = set && set(tip, 0) ? L"Default keyboard updated." : L"Keyboard enabled. Windows could not set the default.";
        if (input) FreeLibrary(input);
        if (enabled) w_load_choices(true);
        SetWindowTextW(status, message);
        return;
    }
    bool removing = action == remove_lang;
    if (removing) {
        if (!c->enabled || preferred_count <= 1) return;
        wchar_t prompt[384];
        swprintf(prompt, count_of(prompt), L"Remove %ls and its input methods from your preferred languages?", c->name);
        if (MessageBoxW(panel, prompt, L"Remove language", MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION) != IDYES) return;
    } else if (c->enabled) {
        SetWindowTextW(status, L"This language is already in your preferred languages.");
        return;
    }
    applying = w_change_language(c->id, removing);
    if (!applying) {
        SetWindowTextW(status, L"Windows language setup could not start.");
        return;
    }
    operation = removing ? language_remove : language_add;
    w_busy(removing ? L"Removing language\u2026" : L"Adding language\u2026");
}

static void w_place(HWND child, int x, int y, int width, int height) {
    MoveWindow(child, x, y, max(1, width), max(1, height), TRUE);
}

static void w_layout(void) {
    if (!panel || !methods) return;
    RECT r;
    GetClientRect(panel, &r);
    int pad = px(16), gap = px(8), width = r.right - 2*pad;
    int category_width = min(px(150), width/2);
    int actions_y = r.bottom-pad-px(34);
    int test_y = actions_y-gap-px(34);
    int status_y = (page == input_methods ? test_y : actions_y)-gap-px(44);
    int results_y = pad+px(32)+gap;
    int icon = px(34);
    w_place(category, pad, pad, category_width, px(180));
    w_place(search, pad+category_width+gap, pad, width-category_width-gap, px(30));
    w_place(results, pad, results_y, width, status_y-gap-results_y);
    w_place(status, pad, status_y, width, px(44));
    int label_width = px(78);
    w_place(test_label, pad, test_y, label_width, px(34));
    w_place(typing, pad+label_width+gap, test_y, width-label_width-gap, px(34));
    w_place(add, pad, actions_y, icon, icon);
    w_place(remove_lang, pad+icon+gap, actions_y, icon, icon);
    w_place(make_default, pad+icon+gap, actions_y, icon, icon);
    int more = min(px(172),width-2*(icon+gap));
    w_place(methods, pad+width-more, actions_y, more, icon);
    w_place(basic, pad+width-more, actions_y, more, icon);
}

static void w_font(void) {
    HFONT next = CreateFontW(-px(15),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
    if (!next) return;
    for (HWND child = GetWindow(panel, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
        SendMessageW(child, WM_SETFONT, (WPARAM)next, TRUE);
    if (font) DeleteObject(font);
    font = next;
    SendMessageW(results, LB_SETITEMHEIGHT, 0, px(30));
}

static LRESULT CALLBACK w_setup_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    LRESULT themed;
    if (w_theme_message(w, msg, wp, lp, &themed)) return themed;
    switch (msg) {
    case WM_COMMAND: {
        HWND source = (HWND)lp;
        if (source == category && HIWORD(wp) == CBN_SELCHANGE)
            w_load_choices(false);
        else if (source == search && HIWORD(wp) == EN_CHANGE)
            w_filter_choices();
        else if (source == results && HIWORD(wp) == LBN_SELCHANGE)
            w_buttons();
        else if (source == results && HIWORD(wp) == LBN_DBLCLK)
            w_apply_choice(add);
        else if ((source == add || source == make_default || source == remove_lang || source == basic) && HIWORD(wp) == BN_CLICKED)
            w_apply_choice(source);
        else if (source == methods && HIWORD(wp) == BN_CLICKED) {
            w_input_choice *c = w_selected();
            wchar_t tag[LOCALE_NAME_MAX_LENGTH] = L"";
            if (c && page == input_languages) wcscpy(tag, c->id);
            SendMessageW(category, CB_SETCURSEL, input_methods, 0);
            w_load_choices(false);
            if (*tag) SetWindowTextW(search, tag);
        }
        return 0;
    }
    case WM_SIZE:
        w_layout();
        return 0;
    case WM_DPICHANGED:
        dpi = HIWORD(wp);
        w_font();
        w_tool_resize_for_dpi(w, (const RECT *)lp);
        w_layout();
        return 0;
    case WM_GETMINMAXINFO:
        w_tool_minimum(w, (MINMAXINFO *)lp, px(435), px(345));
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

static HWND w_control(int id, const wchar_t *cls, const wchar_t *text, DWORD style) {
    DWORD tab = !wcscmp(cls, L"STATIC") ? 0 : WS_TABSTOP;
    return CreateWindowW(cls, text, WS_CHILD | WS_VISIBLE | tab | style, 0, 0, 1, 1,
                         panel, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
}

static bool w_ime_composing(HWND edit) {
    HIMC ctx = ImmGetContext(edit);
    if (!ctx) return false;
    DWORD lists = 0;
    bool active = ImmGetCompositionStringW(ctx, GCS_COMPSTR, NULL, 0) > 0 ||
                  (ImmGetCandidateListCountW(ctx, &lists) > 0 && lists);
    ImmReleaseContext(edit, ctx);
    return active;
}

int w_language_setup(w_input_page start_page) {
    HINSTANCE instance = GetModuleHandleW(NULL);
    WNDCLASSW wc = {.lpfnWndProc = w_setup_proc, .hInstance = instance,
                    .hCursor = LoadCursorW(NULL, IDC_ARROW), .lpszClassName = L"WinwmInputSetup"};
    RegisterClassW(&wc);
    MONITORINFO mi = {.cbSize = sizeof(mi)};
    dpi = (int)w_tool_work_area(&mi);
    RECT r = {0, 0, px(540), px(430)};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MAXIMIZEBOX;
    AdjustWindowRectExForDpi(&r, style, FALSE, WS_EX_TOOLWINDOW, dpi);
    int width = min(r.right-r.left, mi.rcWork.right-mi.rcWork.left);
    int height = min(r.bottom-r.top, mi.rcWork.bottom-mi.rcWork.top);
    panel = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"Input setup", style,
                            (mi.rcWork.left + mi.rcWork.right - width)/2, (mi.rcWork.top + mi.rcWork.bottom - height)/2,
                            width, height, NULL, NULL, instance, NULL);
    if (!panel) return 1;
    category = w_control(101, L"COMBOBOX", L"Category", CBS_DROPDOWNLIST);
    search = w_control(102, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL);
    results = w_control(103, L"LISTBOX", L"Available choices", WS_VSCROLL | LBS_NOINTEGRALHEIGHT |
                        LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS);
    status = w_control(107, L"STATIC", L"", SS_LEFT | SS_NOPREFIX);
    test_label = w_control(109, L"STATIC", L"Test input", SS_CENTERIMAGE);
    typing = w_control(108, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL);
    add = w_control(104, L"BUTTON", L"Add language", BS_OWNERDRAW);
    remove_lang = w_control(105, L"BUTTON", L"Remove language", BS_OWNERDRAW);
    make_default = w_control(106, L"BUTTON", L"Set as default", BS_OWNERDRAW);
    methods = w_control(110, L"BUTTON", L"Input methods\u2026", BS_OWNERDRAW);
    basic = w_control(111, L"BUTTON", L"Install basic typing\u2026", BS_OWNERDRAW);
    if (!category || !search || !results || !status || !test_label || !typing || !add || !remove_lang || !make_default || !methods || !basic) {
        DestroyWindow(panel);
        return 1;
    }
    w_icon_button(remove_lang,L"\uE738",L"Remove selected language");
    w_icon_button(make_default,L"\uE734",L"Set selected keyboard or input method as default");
    HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    TfClientId client = 0;
    if (SUCCEEDED(com)) {
        if (SUCCEEDED(CoCreateInstance(&CLSID_TF_ThreadMgr, NULL, CLSCTX_INPROC_SERVER,
                                       &IID_ITfThreadMgr, (void **)&thread_manager)) &&
            FAILED(ITfThreadMgr_Activate(thread_manager, &client))) {
            ITfThreadMgr_Release(thread_manager);
            thread_manager = NULL;
        }
        CoCreateInstance(&CLSID_TF_InputProcessorProfiles, NULL, CLSCTX_INPROC_SERVER,
                         &IID_ITfInputProcessorProfiles, (void **)&profiles);
    }
    w_font();
    SendMessageW(typing, EM_SETCUEBANNER, TRUE, (LPARAM)L"Type here to test your input method");
    SendMessageW(category, CB_ADDSTRING, 0, (LPARAM)L"Languages");
    SendMessageW(category, CB_ADDSTRING, 0, (LPARAM)L"Keyboard layouts");
    SendMessageW(category, CB_ADDSTRING, 0, (LPARAM)L"Input methods");
    SendMessageW(category, CB_SETCURSEL, start_page, 0);
    w_load_choices(false);
    if (w_tool_show(panel)) {
        SetForegroundWindow(panel);
        SetFocus(search);
    }
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
            w_load_choices(true);
            if (operation == basic_typing) {
                wchar_t error[160];
                swprintf(error, count_of(error), L"Basic typing could not be installed (0x%08lx). Check Windows Update or your servicing policy.", code);
                SetWindowTextW(status, code == 0 ? L"Basic typing installed. Select the input method and try typing." :
                               code == 2 ? L"Basic typing is already installed." :
                               code == 4 ? L"Windows does not offer basic typing for this language." :
                               code == 5 ? L"Restart Windows to finish installing basic typing." :
                               code == 6 ? L"Restart Windows to finish the pending removal, then try again." : error);
            } else {
                bool removing = operation == language_remove;
                SetWindowTextW(status, code == 0 ? (removing ? L"Language removed." : L"Language added. Choose Input methods to try it.") :
                               code == 2 ? (removing ? L"This language was already removed." : L"This language is already added.") :
                               code == 3 ? L"Keep at least one preferred language." : L"Windows could not update your preferred languages.");
            }
        }
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { done = true; break; }
            bool navigation = msg.message == WM_KEYDOWN && (msg.wParam == VK_TAB || msg.wParam == VK_RETURN ||
                              msg.wParam == VK_ESCAPE || msg.wParam == VK_DOWN);
            // Let native edit/IME processing commit, cancel or navigate candidates first.
            if ((msg.hwnd == typing && !(navigation && msg.wParam == VK_TAB)) ||
                (navigation && (msg.hwnd == search || msg.hwnd == typing) && w_ime_composing(msg.hwnd))) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            } else if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE &&
                !SendMessageW(category, CB_GETDROPPEDSTATE,0,0)) {
                SendMessageW(panel, WM_CLOSE, 0, 0);
            } else if (msg.message == WM_KEYDOWN && msg.hwnd == search && msg.wParam == VK_DOWN) {
                SetFocus(results);
            } else if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN && (msg.hwnd == results || msg.hwnd == search)) {
                w_apply_choice(add);
            } else if (!IsDialogMessageW(panel, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }
    if (applying) CloseHandle(applying);
    if (profiles) ITfInputProcessorProfiles_Release(profiles);
    if (thread_manager) {
        ITfThreadMgr_Deactivate(thread_manager);
        ITfThreadMgr_Release(thread_manager);
    }
    if (SUCCEEDED(com)) CoUninitialize();
    DeleteObject(font);
    free(preferred);
    free(choices);
    return 0;
}
