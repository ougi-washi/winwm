#include "os_win32/os_win32.h"
#include "os_win32/environment.h"
#include <dwmapi.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <evntprov.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

////////////////
//~ Os Win32

static const wchar_t *const instance_names[] = {
    [os_host_instance] = L"Local\\winwm.host.v1",
    [os_interactive_instance] = L"Local\\winwm.interactive.v1",
};
////////////////
//~ Os Win32 Functions

void w_os_instance_release(w_os_instance_lock *lock) {
    if (*lock) CloseHandle(*lock);
    *lock = NULL;
}

DWORD w_os_instance_claim(os_instance_kind kind, w_os_instance_lock *lock) {
    *lock = CreateMutexW(NULL,FALSE,instance_names[kind]);
    if (!*lock) return GetLastError();
    if (GetLastError() != ERROR_ALREADY_EXISTS) return ERROR_SUCCESS;
    w_os_instance_release(lock);
    return ERROR_ALREADY_EXISTS;
}

DWORD w_os_instance_status(os_instance_kind kind) {
    HANDLE mutex = OpenMutexW(SYNCHRONIZE,FALSE,instance_names[kind]);
    if (mutex) {
        CloseHandle(mutex);
        return ERROR_ALREADY_EXISTS;
    }
    DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : error;
}
////////////////
//~ Strings and paths

bool w_os_wide(const char *s, wchar_t *out, int cap) {
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, out, cap) > 0;
}

bool w_os_utf8(const wchar_t *s, char *out, int cap) {
    return WideCharToMultiByte(CP_UTF8, 0, s, -1, out, cap, NULL, NULL) > 0;
}

bool w_os_config_path(wchar_t *out, int cap) {
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", out, cap);
    const wchar_t *tail = L"\\.config\\winwm\\winwm.lua";
    if (!n || n + wcslen(tail) >= (size_t)cap)
        return false;
    wcscat(out, tail);
    return true;
}

static bool w_local_path(wchar_t *out, int cap, const wchar_t *directory, const wchar_t *name, bool create) {
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", out, cap);
    if (!n || (size_t)n + wcslen(directory) + 1 + wcslen(name) >= (size_t)cap)
        return false;
    wcscat(out, directory);
    if (create && !CreateDirectoryW(out, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        return false;
    wcscat(out, L"\\");
    wcscat(out, name);
    return true;
}

bool w_os_data_path(wchar_t *out, int cap, const wchar_t *name, bool create) {
    return w_local_path(out, cap, L"\\winwm", name, create);
}

bool w_os_self(wchar_t *out, int cap) {
    DWORD n = GetModuleFileNameW(NULL, out, cap);
    return n && n < (DWORD)cap;
}

////////////////
//~ Process launching

static bool w_launchable(const wchar_t *path) {
    const wchar_t *base = wcsrchr(path, L'\\');
    base = base ? base + 1 : path;
    DWORD flags = GetFileAttributesW(path);
    const wchar_t *ext = wcsrchr(base, L'.');
    return flags != INVALID_FILE_ATTRIBUTES && !(flags & FILE_ATTRIBUTE_DIRECTORY) && ext &&
           !_wcsicmp(ext, L".exe");
}

static bool w_absolute_executable(const wchar_t *path, wchar_t *out, int cap) {
    if (!(path[0] && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')) && wcsncmp(path, L"\\\\", 2))
        return false;
    DWORD n = GetFullPathNameW(path, cap, out, NULL);
    return n && n < (DWORD)cap && w_launchable(out);
}

static bool w_registered_executable(const wchar_t *name, wchar_t *out, int cap) {
    const wchar_t *ext = wcsrchr(name,L'.');
    wchar_t key[320];
    int len = swprintf(key,count_of(key),L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\%ls%ls",
                       name,ext && !_wcsicmp(ext,L".exe") ? L"" : L".exe");
    if (len < 0 || (size_t)len >= count_of(key))
        return false;
    const HKEY roots[] = {HKEY_CURRENT_USER,HKEY_LOCAL_MACHINE};
    for (size_t i=0; i<count_of(roots); i++) {
        wchar_t path[os_path_cap];
        DWORD bytes = sizeof(path);
        if (RegGetValueW(roots[i],key,NULL,RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,NULL,path,&bytes) != ERROR_SUCCESS)
            continue;
        wchar_t *target = path;
        size_t n = wcslen(target);
        if (n >= 2 && target[0] == L'"' && target[n-1] == L'"') {
            target[n-1] = 0;
            target++;
        }
        if (w_absolute_executable(target,out,cap))
            return true;
    }
    return false;
}

wchar_t *w_os_path_directories(void) {
    DWORD capacity = GetEnvironmentVariableW(L"PATH",NULL,0);
    while (capacity) {
        wchar_t *value = malloc((size_t)capacity * sizeof(*value));
        if (!value) break;
        DWORD length = GetEnvironmentVariableW(L"PATH",value,capacity);
        if (length && length < capacity) return value;
        free(value);
        capacity = length;
    }
    return NULL;
}

wchar_t *w_os_next_directory(wchar_t **cursor) {
    while (*cursor && **cursor) {
        wchar_t *start = *cursor;
        start += wcsspn(start,L" \t");
        wchar_t *end = (wchar_t *)w_environment_path_end(start);
        *cursor = *end ? end + 1 : end;
        while (end > start && (end[-1] == L' ' || end[-1] == L'\t')) --end;
        if (end - start >= 2 && *start == L'"' && end[-1] == L'"') {
            ++start;
            --end;
        }
        *end = 0;
        if (*start) return start;
    }
    return NULL;
}

void w_os_path_extensions(wchar_t *out, DWORD cap) {
    DWORD length = GetEnvironmentVariableW(L"PATHEXT",out,cap);
    if (!length || length >= cap) lstrcpynW(out,L".COM;.EXE;.BAT;.CMD",(int)cap);
}

int w_os_extension_order(const wchar_t *extension, const wchar_t *extensions) {
    size_t length = wcslen(extension);
    int order = 1;
    for (const wchar_t *p = extensions; *p; ++order) {
        size_t n = wcscspn(p,L";");
        if (n == length && !_wcsnicmp(p,extension,length)) return order;
        p += n;
        if (*p) ++p;
    }
    return 0;
}

static bool w_in_directory(const wchar_t *directory, const wchar_t *name, const wchar_t *extensions,
                           wchar_t *out, int cap) {
    // An empty list checks the explicitly supplied filename just once.
    for (const wchar_t *p = extensions;;) {
        int n = (int)wcscspn(p,L";");
        if (!*extensions || (n > 1 && *p == L'.' && (size_t)n <= wcscspn(p,L"\\/:*?\""))) {
            wchar_t candidate[os_path_cap];
            if (swprintf(candidate,count_of(candidate),L"%ls\\%ls%.*ls",directory,name,n,p) >= 0) {
                DWORD length = GetFullPathNameW(candidate,cap,out,NULL);
                DWORD flags = length && length < (DWORD)cap ? GetFileAttributesW(out) : INVALID_FILE_ATTRIBUTES;
                if (flags != INVALID_FILE_ATTRIBUTES && !(flags & FILE_ATTRIBUTE_DIRECTORY)) return true;
            }
        }
        p += n;
        if (!*p) return false;
        ++p;
    }
}

static bool w_resolve_program(const wchar_t *raw, const wchar_t *extensions, wchar_t *out, int cap) {
    wchar_t path[os_path_cap];
    DWORD n = ExpandEnvironmentStringsW(raw,path,count_of(path));
    if (!n || n > count_of(path) || !*path) return false;
    if (wcspbrk(path,L"\\/:")) return w_absolute_executable(path,out,cap);
    const wchar_t *ext = wcsrchr(path,L'.');
    if (ext && (!_wcsicmp(ext,L".exe") || w_os_extension_order(ext,extensions))) extensions = L"";
    wchar_t *env = w_os_path_directories(), *cursor = env, *directory;
    bool found = false;
    while (!found && (directory = w_os_next_directory(&cursor)))
        found = w_in_directory(directory,path,extensions,out,cap);
    free(env);
    return found || w_registered_executable(path,out,cap);
}

bool w_os_provider(const char *name, wchar_t *out, int cap) {
    wchar_t raw[os_path_cap];
    w_environment_for_launch();
    return w_os_wide(name,raw,count_of(raw)) && w_resolve_program(raw,L".exe",out,cap);
}

bool w_os_task_manager(const char *name, wchar_t *out, int cap) {
    return w_os_provider(name,out,cap) || (!_stricmp(name,"TaskSlinger.exe") &&
           w_os_provider("%SystemRoot%\\System32\\Taskmgr.exe",out,cap));
}

// Encode a single Windows argv field, including its terminal run of backslashes.
wchar_t *w_os_quote_argument(wchar_t *p, const wchar_t *s) {
    *p++ = L'"';
    for (;;) {
        size_t slashes = 0;
        while (*s == L'\\') {
            ++s;
            ++slashes;
        }
        size_t copies = (*s == 0 || *s == L'"') ? slashes * 2 : slashes;
        while (copies--)
            *p++ = L'\\';
        if (!*s)
            break;
        if (*s == L'"')
            *p++ = L'\\';
        *p++ = *s++;
    }
    *p++ = L'"';
    *p = 0;
    return p;
}

static HANDLE w_launch_at(const wchar_t *exe, const wchar_t *arg, const wchar_t *directory, const POINT *origin) {
    w_environment_for_launch();
    size_t len = 2 * (wcslen(exe) + (arg ? wcslen(arg) : 0)) + 10;
    if (len > 32767)
        return NULL;
    wchar_t *line = calloc(len, sizeof(wchar_t));
    if (!line)
        return NULL;
    wchar_t *p = w_os_quote_argument(line, exe);
    if (arg && *arg) {
        *p++ = L' ';
        w_os_quote_argument(p, arg);
    }
    STARTUPINFOW si = {.cb = sizeof(si)};
    if (origin) {
        si.dwFlags |= STARTF_USEPOSITION;
        si.dwX = (DWORD)origin->x;
        si.dwY = (DWORD)origin->y;
    }
    PROCESS_INFORMATION pi = {0};
    BOOL ok =
        CreateProcessW(exe, line, NULL, NULL, FALSE, CREATE_UNICODE_ENVIRONMENT, NULL, directory, &si, &pi);
    free(line);
    if (!ok)
        return NULL;
    AllowSetForegroundWindow(pi.dwProcessId);
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

HANDLE w_os_launch(const wchar_t *exe, const wchar_t *arg) {
    return w_launch_at(exe, arg, NULL, NULL);
}

HANDLE w_os_launch_positioned(const wchar_t *exe, const wchar_t *arg, POINT origin) {
    return w_launch_at(exe, arg, NULL, &origin);
}

bool w_os_shell_execute(const wchar_t *file, const wchar_t *parameters, const wchar_t *directory, bool admin) {
    w_environment_for_launch();
    HRESULT apartment = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    SHELLEXECUTEINFOW info = {.cbSize = sizeof(info), .fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI,
        .lpVerb = admin ? L"runas" : NULL, .lpFile = file, .lpParameters = parameters, .lpDirectory = directory,
        .nShow = SW_SHOWNORMAL};
    if (wcschr(file, L'%') && !PathIsURLW(file))
        info.fMask |= SEE_MASK_DOENVSUBST;
    bool started = ShellExecuteExW(&info) != FALSE;
    DWORD error = started ? ERROR_SUCCESS : GetLastError();
    if (SUCCEEDED(apartment)) CoUninitialize();
    SetLastError(error);
    return started;
}

bool w_os_run_executable(const wchar_t *exe, const wchar_t *argument, const wchar_t *directory, bool admin) {
    if (admin) {
        wchar_t quoted[2 * os_path_cap + 3];
        if (argument && wcslen(argument) >= os_path_cap) return false;
        if (argument && *argument) w_os_quote_argument(quoted, argument);
        return w_os_shell_execute(exe, argument && *argument ? quoted : NULL, directory, true);
    }
    HANDLE child = w_launch_at(exe, argument, directory, NULL);
    if (!child) return false;
    CloseHandle(child);
    return true;
}

static bool w_launch_command_line(wchar_t *line, bool admin, bool allow_shell_targets) {
    wchar_t path[os_path_cap], home[os_path_cap];
    int argc = 0;
    while (*line == L' ' || *line == L'\t') ++line;
    if (!*line) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
    wchar_t **argv = CommandLineToArgvW(line, &argc);
    if (!argv) return false;
    w_environment_for_launch();
    wchar_t extensions[32768];
    if (allow_shell_targets) w_os_path_extensions(extensions,count_of(extensions));
    bool found = argc && w_resolve_program(argv[0],allow_shell_targets ? extensions : L".exe",path,count_of(path));
    if (!found && !allow_shell_targets) {
        LocalFree(argv);
        SetLastError(ERROR_FILE_NOT_FOUND);
        return false;
    }
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", home, count_of(home));
    const wchar_t *directory = n && n < count_of(home) ? home : NULL;
    const wchar_t *ext = found ? wcsrchr(path,L'.') : NULL;
    if (admin || !ext || (_wcsicmp(ext,L".exe") && _wcsicmp(ext,L".com"))) {
        // Let Windows resolve shell objects, applets and registered URI handlers.
        // Preserve quoting in the argument tail of executable commands.
        const wchar_t *args = *line == L'"' ? wcschr(line + 1, L'"') : line + wcscspn(line, L" \t");
        if (args && *args == L'"') ++args;
        if (args) args += wcsspn(args, L" \t");
        bool started = w_os_shell_execute(found ? path : argv[0], args && *args ? args : NULL, directory, admin);
        DWORD error = GetLastError();
        LocalFree(argv);
        SetLastError(error);
        return started;
    }
    LocalFree(argv);
    STARTUPINFOW si = {.cb = sizeof(si)};
    PROCESS_INFORMATION pi = {0};
    if (!CreateProcessW(path, line, NULL, NULL, FALSE, CREATE_UNICODE_ENVIRONMENT, NULL, directory, &si, &pi))
        return false;
    AllowSetForegroundWindow(pi.dwProcessId);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

bool w_os_launch_wide_line(wchar_t *line, bool admin) {
    return w_launch_command_line(line, admin, true);
}

bool w_os_launch_line(const char *command, bool admin, bool allow_shell_targets) {
    wchar_t line[os_path_cap];
    return w_os_wide(command, line, count_of(line)) && w_launch_command_line(line, admin, allow_shell_targets);
}

bool w_os_launch_terminal_at(const char *name, const wchar_t *directory, bool admin) {
    wchar_t path[os_path_cap];
    if (!strcmp(name, "auto")) {
        // Starting a console application lets Windows select its configured terminal host.
        return w_os_provider("%SystemRoot%\\System32\\cmd.exe", path, count_of(path)) &&
               w_os_shell_execute(path, NULL, directory, admin);
    }
    return w_os_provider(name, path, count_of(path)) && w_os_run_executable(path, NULL, directory, admin);
}

bool w_os_launch_terminal(const char *name, bool admin) {
    wchar_t home[os_path_cap];
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", home, count_of(home));
    return n && n < count_of(home) && w_os_launch_terminal_at(name, home, admin);
}

bool w_os_launch_provider(const char *name, const char *arg, bool admin) {
    wchar_t path[os_path_cap], param[os_path_cap];
    if (!w_os_provider(name, path, count_of(path))) {
        return (!arg || !*arg) && w_os_wide(name, path, count_of(path)) &&
               w_os_shell_execute(path, NULL, NULL, admin);
    }
    if (!w_os_wide(arg ? arg : "", param, count_of(param)))
        return false;
    return w_os_run_executable(path, param, NULL, admin);
}

void w_os_mark(const wchar_t *stage) {
    static REGHANDLE provider;
    static const GUID id = {0x76227e72, 0x6688, 0x4cad, {0xa7, 0x86, 0xaf, 0x11, 0x9d, 0x55, 0xc2, 0x6b}};
    if (!provider)
        EventRegister(&id, NULL, NULL, &provider);
    if (provider)
        EventWriteString(provider, 4, 1, stage);
}

////////////////
//~ Window discovery

bool w_os_keyboard_name(HKEY layouts, const wchar_t *key, wchar_t *name, DWORD count) {
    wchar_t indirect[256];
    DWORD bytes = sizeof(indirect);
    if (RegGetValueW(layouts, key, L"Layout Display Name", RRF_RT_REG_SZ, NULL, indirect, &bytes) == ERROR_SUCCESS &&
        SUCCEEDED(SHLoadIndirectString(indirect, name, count, NULL)) && *name)
        return true;
    bytes = count * sizeof(*name);
    return RegGetValueW(layouts, key, L"Layout Text", RRF_RT_REG_SZ | RRF_ZEROONFAILURE,
                        NULL, name, &bytes) == ERROR_SUCCESS && *name;
}

bool w_os_same_process(HWND w) {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

w_rect w_os_bounds(HWND w) {
    RECT r = {0};
    GetWindowRect(w, &r);
    return (w_rect){r.left, r.top, r.right - r.left, r.bottom - r.top};
}

RECT w_os_to_rect(w_rect r) {
    return (RECT){r.x, r.y, r.x + r.w, r.y + r.h};
}

bool w_os_desktop_surface(HWND window) {
    if (!window || window == GetDesktopWindow() || window == GetShellWindow())
        return true;
    wchar_t name[32];
    return GetClassNameW(window, name, count_of(name)) &&
           (!wcscmp(name, L"WorkerW") || !wcscmp(name, L"Progman") ||
            (!wcscmp(name, L"WinwmEvents") && w_os_same_process(window)));
}

// An explicitly taskbar-visible owned window is an independent app window.
// Its dialogs stop at that window, even if its own owner is a hidden helper.
HWND w_os_app_window(HWND w) {
    w = GetAncestor(w, GA_ROOT);
    while (w && !(GetWindowLongPtrW(w, GWL_EXSTYLE) & WS_EX_APPWINDOW)) {
        HWND owner = GetWindow(w, GW_OWNER);
        if (!owner)
            break;
        w = owner;
    }
    return w;
}

bool w_os_cloaked(HWND w) {
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked;
}

bool w_os_candidate(HWND w, DWORD *pid) {
    if (!w || !IsWindowVisible(w) || GetAncestor(w, GA_ROOT) != w ||
        !GetWindowThreadProcessId(w, pid) || *pid == GetCurrentProcessId() || GetPropW(w, L"winwm.ignore"))
        return false;
    LONG_PTR ex = GetWindowLongPtrW(w, GWL_EXSTYLE), style = GetWindowLongPtrW(w, GWL_STYLE);
    if ((ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) || (style & WS_CHILD) ||
        (!(ex & WS_EX_APPWINDOW) && GetWindow(w, GW_OWNER)))
        return false;
    wchar_t cls[128] = {0};
    GetClassNameW(w, cls, count_of(cls));
    const wchar_t *skip[] = {
        L"Progman",          L"WorkerW",          L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd",
        L"#32768",           L"tooltips_class32", L"SysShadow",     L"Windows.UI.Core.CoreWindow",
        L"WinwmHost"};
    for (size_t i = 0; i < count_of(skip); i++)
        if (!_wcsicmp(cls, skip[i]))
            return false;
    return !w_os_cloaked(w);
}

// Properties are owned by the HWND lifetime. A reused handle never inherits recovery data.
static const wchar_t *const coords[] = {L"winwm.x", L"winwm.y", L"winwm.w",
                                        L"winwm.h"};
static const wchar_t *const placement_fields[] = {L"winwm.normal_lt", L"winwm.normal_rb",
    L"winwm.min_pos", L"winwm.max_pos", L"winwm.show_flags"};

// winwm is x64. Keep the values on the foreign HWND so a separate process
// can recover them after termination, even when the owner reuses an HWND.
static HANDLE w_pair(LONG x, LONG y) {
    return (HANDLE)(uintptr_t)(((uint64_t)(DWORD)y << 32) | (DWORD)x);
}

static POINT w_unpack(HANDLE value) {
    uint64_t bits = (uintptr_t)value;
    return (POINT){(LONG)(DWORD)bits, (LONG)(DWORD)(bits >> 32)};
}

////////////////
//~ Window recovery

static w_rect w_remembered_bounds(HWND w) {
    int v[4];
    for (int i = 0; i < 4; i++)
        v[i] = (int)((uintptr_t)GetPropW(w, coords[i]) - 1);
    return (w_rect){v[0], v[1], v[2], v[3]};
}

bool w_os_remember(HWND w, w_rect *original) {
    if (GetPropW(w, L"winwm.managed")) {
        *original = w_remembered_bounds(w);
        return true;
    }
    w_rect r = w_os_bounds(w);
    WINDOWPLACEMENT placement = {.length = sizeof(placement)};
    if (!GetWindowPlacement(w, &placement))
        return false;
    bool zoomed = placement.showCmd == SW_SHOWMAXIMIZED;
    MONITORINFO mi = {.cbSize = sizeof(mi)};
    if (zoomed && GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi)) {
        RECT normal = placement.rcNormalPosition;
        OffsetRect(&normal, mi.rcWork.left - mi.rcMonitor.left, mi.rcWork.top - mi.rcMonitor.top);
        r = (w_rect){normal.left, normal.top, normal.right - normal.left, normal.bottom - normal.top};
    }
    int v[] = {r.x, r.y, r.w, r.h};
    if (r.w <= 0 || r.h <= 0)
        return false;
    for (int i = 0; i < 4; i++) {
        if (!SetPropW(w, coords[i], (HANDLE)(uintptr_t)((uint64_t)(uint32_t)v[i] + 1))) {
            for (int j = 0; j < i; j++)
                RemovePropW(w, coords[j]);
            return false;
        }
    }
    HANDLE values[] = {w_pair(placement.rcNormalPosition.left, placement.rcNormalPosition.top),
        w_pair(placement.rcNormalPosition.right, placement.rcNormalPosition.bottom),
        w_pair(placement.ptMinPosition.x, placement.ptMinPosition.y),
        w_pair(placement.ptMaxPosition.x, placement.ptMaxPosition.y),
        w_pair(placement.showCmd, placement.flags)};
    for (size_t i = 0; i < count_of(values); i++) {
        if (!SetPropW(w, placement_fields[i], values[i]))
            goto failed;
    }
    if (zoomed && !SetPropW(w, L"winwm.zoomed", (HANDLE)1))
        goto failed;
    // Publish the complete journal before the first layout or appearance change.
    if (!SetPropW(w, L"winwm.managed", (HANDLE)2))
        goto failed;
    *original = r;
    return true;
failed:
    for (int i = 0; i < 4; i++)
        RemovePropW(w, coords[i]);
    for (size_t i = 0; i < count_of(placement_fields); i++)
        RemovePropW(w, placement_fields[i]);
    RemovePropW(w, L"winwm.zoomed");
    return false;
}

// Serialize cross-thread SWP requests. A reversal waits for the previous request to land.
enum { hide_pending = 1, hidden_confirmed, show_pending };

static const wchar_t visibility_prop[] = L"winwm.hidden";
bool w_os_shell_hidden(HWND window) {
    return GetPropW(window, visibility_prop) != NULL;
}

bool w_os_hidden_confirmed(HWND window) {
    return (uintptr_t)GetPropW(window, visibility_prop) == hidden_confirmed;
}

bool w_os_ready_for_focus(HWND window) {
    uintptr_t state = (uintptr_t)GetPropW(window, visibility_prop);
    return (!state || state == show_pending) && IsWindowVisible(window) && !IsIconic(window);
}

////////////////
//~ Visibility and appearance

bool w_os_set_visible(HWND w, bool wanted) {
    int state = (int)(uintptr_t)GetPropW(w, visibility_prop);
    bool visible = IsWindowVisible(w) != 0;
    if (state == hide_pending) {
        if (visible)
            return false;
        state = hidden_confirmed;
        SetPropW(w, visibility_prop, (HANDLE)(uintptr_t)state);
    } else if (state == show_pending) {
        if (!visible)
            return false;
        RemovePropW(w, visibility_prop);
        state = 0;
    }
    if (wanted ? !state : !visible)
        return false;
    if (wanted && visible) {
        RemovePropW(w, visibility_prop);
        return false;
    }
    int next = wanted ? show_pending : hide_pending;
    if (!SetPropW(w, visibility_prop, (HANDLE)(uintptr_t)next))
        return false;
    UINT flags = SWP_ASYNCWINDOWPOS | SWP_NOSIZE | SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE |
                 (wanted ? SWP_SHOWWINDOW : SWP_HIDEWINDOW);
    // SWP_NOACTIVATE can leave an invisible panel as the foreground window.
    // Let Windows transfer activation when hiding the current foreground app.
    HWND foreground = GetForegroundWindow();
    bool hide_active = !wanted && foreground && (foreground == w || w_os_app_window(foreground) == w);
    if (hide_active ? ShowWindowAsync(w, SW_HIDE) : SetWindowPos(w, NULL, 0, 0, 0, 0, flags))
        return true;
    if (state)
        SetPropW(w, visibility_prop, (HANDLE)(uintptr_t)state);
    else
        RemovePropW(w, visibility_prop);
    return false;
}

static void w_release_visibility(HWND w) {
    if (GetPropW(w, visibility_prop) && SetWindowPos(w, NULL, 0, 0, 0, 0,
                     SWP_ASYNCWINDOWPOS | SWP_NOSIZE | SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE |
                         SWP_SHOWWINDOW))
        RemovePropW(w, visibility_prop);
}

static const wchar_t dim_prop[] = L"winwm.dim_added";
#define dim_releasing ((HANDLE)2)
static void w_remove_dim(HWND w) {
    if (GetPropW(w, dim_prop)) {
        LONG_PTR ex = GetWindowLongPtrW(w, GWL_EXSTYLE);
        if (!(ex & WS_EX_LAYERED) || SetWindowLongPtrW(w, GWL_EXSTYLE, ex & ~WS_EX_LAYERED))
            RemovePropW(w, dim_prop);
    }
}

// Changing a foreign extended style sends synchronous WM_STYLECHANGING messages.
// A stalled client must not hold up other clients or the interactive shell.
static SRWLOCK layer_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE layers_done = CONDITION_VARIABLE_INIT;
static unsigned restoring_layers;

static void w_layer_finished(void) {
    AcquireSRWLockExclusive(&layer_lock);
    if (!--restoring_layers) WakeAllConditionVariable(&layers_done);
    ReleaseSRWLockExclusive(&layer_lock);
}

static DWORD WINAPI w_release_layer(void *arg) {
    HWND w = arg;
    wchar_t name[80];
    swprintf(name, count_of(name), L"Local\\winwm.layer.%p", (void *)w);
    HANDLE lock = CreateMutexW(NULL, FALSE, name);
    if (lock) {
        DWORD wait = WaitForSingleObject(lock, 0);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
            if (GetPropW(w, dim_prop) == dim_releasing) {
                SetLayeredWindowAttributes(w, 0, 255, LWA_ALPHA);
                w_remove_dim(w);
            }
            ReleaseMutex(lock);
        }
        CloseHandle(lock);
    }
    w_layer_finished();
    return 0;
}

static BOOL CALLBACK w_queue_layer(HWND w, LPARAM unused) {
    (void)unused;
    if (GetPropW(w, dim_prop) && SetPropW(w, dim_prop, dim_releasing)) {
        AcquireSRWLockExclusive(&layer_lock);
        ++restoring_layers;
        ReleaseSRWLockExclusive(&layer_lock);
        if (!QueueUserWorkItem(w_release_layer, w, WT_EXECUTELONGFUNCTION)) w_layer_finished();
    }
    return TRUE;
}

void w_os_wait_layer_cleanup(void) {
    AcquireSRWLockExclusive(&layer_lock);
    while (restoring_layers)
        SleepConditionVariableSRW(&layers_done, &layer_lock, INFINITE, 0);
    ReleaseSRWLockExclusive(&layer_lock);
}

void w_os_window_appearance(HWND w, DWORD color, DWORD corner) {
    DWORD previous;
    if (GetPropW(w, L"winwm.corner") ||
        (SUCCEEDED(DwmGetWindowAttribute(w, 33, &previous, sizeof(previous))) &&
         SetPropW(w, L"winwm.corner", (HANDLE)(uintptr_t)(previous + 1))))
        DwmSetWindowAttribute(w, 33, &corner, sizeof(corner));
    if (GetPropW(w, L"winwm.border") ||
        (SUCCEEDED(DwmGetWindowAttribute(w, 34, &previous, sizeof(previous))) &&
         SetPropW(w, L"winwm.border", (HANDLE)(uintptr_t)((uint64_t)previous + 1))))
        DwmSetWindowAttribute(w, 34, &color, sizeof(color));
}

bool w_os_direct_surface(HWND w, DWORD pid) {
    if (GetPropW(w, dim_prop) == dim_releasing ||
        (GetWindowLongPtrW(w, GWL_EXSTYLE) & (WS_EX_LAYERED | WS_EX_NOREDIRECTIONBITMAP)) ||
        (GetClassLongPtrW(w, GCL_STYLE) & (CS_OWNDC | CS_CLASSDC)))
        return true;
    // A foreign GetDC can return a common DC with no pixel format even when
    // the application is already presenting with OpenGL. In particular Winit
    // uses a full-redraw window class without CS_OWNDC. Check the rendering
    // runtime once at discovery, never in the animation or input loop.
    HANDLE modules = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (modules == INVALID_HANDLE_VALUE)
        return true;
    MODULEENTRY32W entry = {.dwSize = sizeof(entry)};
    bool direct = !Module32FirstW(modules, &entry);
    if (!direct) {
        do {
            direct = !_wcsicmp(entry.szModule, L"opengl32.dll") || !_wcsicmp(entry.szModule, L"libEGL.dll") ||
                     !_wcsicmp(entry.szModule, L"vulkan-1.dll");
        } while (!direct && Module32NextW(modules, &entry));
    }
    CloseHandle(modules);
    return direct;
}

bool w_os_window_opacity(HWND w, BYTE alpha, bool retain_layer) {
    if (GetPropW(w, dim_prop) == dim_releasing)
        return false;
    if (alpha == 255 && !retain_layer) {
        w_remove_dim(w);
        return true;
    }
    if (GetPropW(w, dim_prop)) {
        // Changing the extended style on every focus transition rebuilds the window's
        // redirection surface. Keep our layer and change only its opacity.
        return SetLayeredWindowAttributes(w, 0, alpha, LWA_ALPHA) != 0;
    }
    if (alpha == 255)
        return true;
    LONG_PTR ex = GetWindowLongPtrW(w, GWL_EXSTYLE);
    if ((ex & (WS_EX_LAYERED | WS_EX_NOREDIRECTIONBITMAP)) ||
        (GetClassLongPtrW(w, GCL_STYLE) & (CS_OWNDC | CS_CLASSDC)))
        return false;
    if (SetPropW(w, dim_prop, (HANDLE)1)) {
        SetWindowLongPtrW(w, GWL_EXSTYLE, ex | WS_EX_LAYERED);
        if (SetLayeredWindowAttributes(w, 0, alpha, LWA_ALPHA))
            return true;
        w_remove_dim(w);
    }
    return false;
}

static bool w_restore_placement(HWND w, bool native) {
    bool visible = IsWindowVisible(w) || w_os_shell_hidden(w);
    bool iconic = IsIconic(w) != 0;
    WINDOWPLACEMENT placement = {.length = sizeof(placement)};
    if (native) {
        POINT lt = w_unpack(GetPropW(w, placement_fields[0])), rb = w_unpack(GetPropW(w, placement_fields[1]));
        POINT show = w_unpack(GetPropW(w, placement_fields[4]));
        placement.ptMinPosition = w_unpack(GetPropW(w, placement_fields[2]));
        bool zoomed = IsZoomed(w) != 0;
        bool offscreen_min = placement.ptMinPosition.x == -32000 && placement.ptMinPosition.y == -32000;
        if (!iconic && ((show.x == SW_SHOWNORMAL && !zoomed) ||
            (visible && offscreen_min && (show.x == SW_SHOWNORMAL || show.x == SW_SHOWMAXIMIZED)))) {
            // A geometry change preserves native minimize coordinates. Passing an
            // offscreen minimize position to SetWindowPlacement resets it to (-1,-1).
            w_rect r = w_remembered_bounds(w);
            if (zoomed && !ShowWindowAsync(w,SW_SHOWNOACTIVATE)) return false;
            if (!SetWindowPos(w, NULL, r.x, r.y, r.w, r.h, SWP_ASYNCWINDOWPOS |
                              SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOZORDER)) return false;
            return show.x != SW_SHOWMAXIMIZED || ShowWindowAsync(w,SW_SHOWMAXIMIZED);
        }
        placement.rcNormalPosition = (RECT){lt.x, lt.y, rb.x, rb.y};
        placement.ptMaxPosition = w_unpack(GetPropW(w, placement_fields[3]));
        placement.flags = (UINT)show.y;
        placement.showCmd = (UINT)show.x;
        if (placement.ptMinPosition.x != -1 || placement.ptMinPosition.y != -1)
            placement.flags |= WPF_SETMINPOSITION;
    } else {
        // Earlier releases stored only the normal screen rectangle and a maximize marker.
        w_rect r = w_remembered_bounds(w);
        RECT normal = w_os_to_rect(r);
        MONITORINFO mon = {.cbSize = sizeof(mon)};
        if (!GetWindowPlacement(w, &placement) ||
            !GetMonitorInfoW(MonitorFromRect(&normal, MONITOR_DEFAULTTONEAREST), &mon))
            return false;
        OffsetRect(&normal, mon.rcMonitor.left-mon.rcWork.left, mon.rcMonitor.top-mon.rcWork.top);
        placement.rcNormalPosition = normal;
        placement.flags = 0;
        placement.showCmd = GetPropW(w, L"winwm.zoomed") ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
    }
    // Respect applications that withdrew themselves and user-minimized windows.
    // A winwm workspace hide, including one still queued, must be reversed.
    if (!visible) placement.showCmd = SW_HIDE;
    else if (iconic) {
        if (placement.showCmd == SW_SHOWMAXIMIZED) placement.flags |= WPF_RESTORETOMAXIMIZED;
        placement.showCmd = SW_SHOWMINNOACTIVE;
    } else if (placement.showCmd == SW_SHOWNORMAL)
        placement.showCmd = SW_SHOWNOACTIVATE;
    placement.flags |= WPF_ASYNCWINDOWPLACEMENT;
    return SetWindowPlacement(w, &placement) != 0;
}

static void w_restore_attribute(HWND w, const wchar_t *name, DWORD attribute) {
    HANDLE saved = GetPropW(w, name);
    if (saved) {
        DWORD value = (DWORD)((uintptr_t)saved - 1);
        if (SUCCEEDED(DwmSetWindowAttribute(w, attribute, &value, sizeof(value))))
            RemovePropW(w, name);
    }
}

static void w_restore_window(HWND w) {
    HANDLE journal = GetPropW(w, L"winwm.managed");
    w_restore_attribute(w, L"winwm.corner", 33);
    w_restore_attribute(w, L"winwm.border", 34);
    if (!journal) {
        w_release_visibility(w);
        return;
    }
    bool restored = w_restore_placement(w, journal == (HANDLE)2);
    w_release_visibility(w);
    if (restored) {
        RemovePropW(w, L"winwm.managed");
        for (int i = 0; i < 4; i++)
            RemovePropW(w, coords[i]);
        for (size_t i = 0; i < count_of(placement_fields); i++)
            RemovePropW(w, placement_fields[i]);
        RemovePropW(w, L"winwm.zoomed");
    }
}

void w_os_restore(HWND w) {
    w_restore_window(w);
    w_queue_layer(w, 0);
}

static const wchar_t stack_prop[] = L"winwm.initial_order", focus_prop[] = L"winwm.initial_foreground";
static const wchar_t anchor_prop[] = L"winwm.initial_above", anchor_pid_prop[] = L"winwm.initial_above_pid";

typedef struct w_stack_capture {
    unsigned rank;
    HWND unrelated[2];
} w_stack_capture;

static BOOL CALLBACK w_save_order(HWND w, LPARAM arg) {
    w_stack_capture *capture = (w_stack_capture *)arg;
    if (w_os_same_process(w)) return TRUE;
    int band = (GetWindowLongPtrW(w, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    if (!GetPropW(w, L"winwm.managed")) {
        capture->unrelated[band] = w;
    } else if (capture->rank < window_limit) {
        HWND anchor = capture->unrelated[band];
        if (SetPropW(w, stack_prop, (HANDLE)(uintptr_t)++capture->rank) && anchor) {
            DWORD pid = 0;
            if (GetWindowThreadProcessId(anchor, &pid) && SetPropW(w, anchor_pid_prop, (HANDLE)(uintptr_t)pid))
                SetPropW(w, anchor_prop, anchor);
        }
        capture->unrelated[band] = NULL;
    }
    return TRUE;
}

void w_os_remember_stack(void) {
    w_stack_capture capture = {0};
    EnumWindows(w_save_order, (LPARAM)&capture);
    HWND foreground = GetForegroundWindow();
    if (foreground && GetPropW(w_os_app_window(foreground), L"winwm.managed"))
        SetPropW(foreground, focus_prop, (HANDLE)1);
}

typedef struct w_recovery_stack {
    HWND windows[window_limit], foreground;
    unsigned count, last_rank[2];
    bool changed[2];
} w_recovery_stack;

static BOOL CALLBACK w_restore_one(HWND w, LPARAM arg) {
    w_recovery_stack *stack = (w_recovery_stack *)arg;
    RemovePropW(w,WINWM_TRAY_WINDOW_PROPERTY);
    uintptr_t rank = (uintptr_t)GetPropW(w, stack_prop);
    if (rank && rank <= window_limit) {
        stack->windows[rank-1] = w;
        if (stack->count < rank) stack->count = (unsigned)rank;
        int band = (GetWindowLongPtrW(w, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
        // Reversing a workspace hide or a maximize can also raise the app later,
        // when its thread handles the asynchronous placement.
        if (rank < stack->last_rank[band] || w_os_shell_hidden(w) ||
            (IsZoomed(w) != 0) != (GetPropW(w,L"winwm.zoomed") != NULL))
            stack->changed[band] = true;
        stack->last_rank[band] = (unsigned)rank;
    }
    if (GetPropW(w, focus_prop)) stack->foreground = w;
    w_restore_window(w);
    return TRUE;
}

void w_os_restore_all(void) {
    w_recovery_stack stack = {0};
    HWND foreground = GetForegroundWindow();
    bool restore_focus = !foreground || w_os_desktop_surface(foreground) || w_os_same_process(foreground) ||
                         GetPropW(w_os_app_window(foreground), L"winwm.managed");
    EnumWindows(w_restore_one, (LPARAM)&stack);
    HWND previous[2] = {0};
    for (unsigned i = 0; i < stack.count; i++) {
        HWND w = stack.windows[i];
        if (!w || (uintptr_t)GetPropW(w, stack_prop) != i+1) continue;
        int band = (GetWindowLongPtrW(w, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
        if (!stack.changed[band]) continue;
        HWND above = GetPropW(w, anchor_prop);
        DWORD pid = 0;
        if (!above || !GetWindowThreadProcessId(above, &pid) || pid != (uintptr_t)GetPropW(w, anchor_pid_prop) ||
            ((GetWindowLongPtrW(above, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) != band)
            above = previous[band];
        if (above)
            SetWindowPos(w, above, 0, 0, 0, 0, SWP_ASYNCWINDOWPOS | SWP_NOSIZE | SWP_NOMOVE |
                         SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        previous[band] = w;
    }
    if (stack.foreground && RemovePropW(stack.foreground, focus_prop) && restore_focus &&
        IsWindowVisible(stack.foreground) && !IsIconic(stack.foreground))
        SetForegroundWindow(stack.foreground);
    for (unsigned i = 0; i < stack.count; i++)
        if (stack.windows[i]) {
            RemovePropW(stack.windows[i], stack_prop);
            RemovePropW(stack.windows[i], anchor_prop);
            RemovePropW(stack.windows[i], anchor_pid_prop);
        }
    EnumWindows(w_queue_layer, 0);
}

int w_os_error(wchar_t *dst, int cap) {
    return (int)FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
                               GetLastError(), 0, dst, cap, NULL);
}
