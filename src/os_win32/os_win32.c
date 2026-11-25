#include "os_win32/os_win32.h"
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

// The second name is also claimed by older builds. Holding both prevents two
// generations of the window manager from applying incompatible HWND state.
static const struct w_instance_names {
    const wchar_t *current, *legacy;
} instance_names[] = {
    [os_host_instance] = {L"Local\\winwm.host.v1", L"Local\\native_desktop.host.v1"},
    [os_interactive_instance] = {L"Local\\winwm.interactive.v1", L"Local\\native_desktop.interactive.v1"},
};

////////////////
//~ Os Win32 Functions

void w_os_instance_release(w_os_instance_lock *lock) {
    if (lock->legacy)
        CloseHandle(lock->legacy);
    if (lock->current)
        CloseHandle(lock->current);
    *lock = (w_os_instance_lock){0};
}

static DWORD w_claim_mutex(const wchar_t *name, HANDLE *out, DWORD occupied) {
    *out = CreateMutexW(NULL, FALSE, name);
    if (!*out)
        return GetLastError();
    return GetLastError() == ERROR_ALREADY_EXISTS ? occupied : ERROR_SUCCESS;
}

DWORD w_os_instance_claim(os_instance_kind kind, w_os_instance_lock *lock) {
    *lock = (w_os_instance_lock){0};
    DWORD error = w_claim_mutex(instance_names[kind].current, &lock->current, ERROR_ALREADY_EXISTS);
    if (!error)
        error = w_claim_mutex(instance_names[kind].legacy, &lock->legacy, ERROR_REVISION_MISMATCH);
    if (error)
        w_os_instance_release(lock);
    return error;
}

static DWORD w_mutex_status(const wchar_t *name, DWORD occupied) {
    HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, name);
    if (mutex) {
        CloseHandle(mutex);
        return occupied;
    }
    DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : error;
}

DWORD w_os_instance_status(os_instance_kind kind) {
    DWORD result = w_mutex_status(instance_names[kind].current, ERROR_ALREADY_EXISTS);
    return result ? result : w_mutex_status(instance_names[kind].legacy, ERROR_REVISION_MISMATCH);
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
    const wchar_t *tail = L"\\.config\\winwm\\winwm.conf";
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

bool w_os_legacy_config_path(wchar_t *out, int cap) {
    return w_local_path(out, cap, L"\\native_desktop", L"config.jsonc", false);
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
           !_wcsicmp(ext, L".exe") && _wcsicmp(base, L"explorer.exe");
}

static bool w_absolute_executable(const wchar_t *path, wchar_t *out, int cap) {
    if (!(path[0] && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')) && wcsncmp(path, L"\\\\", 2))
        return false;
    DWORD n = GetFullPathNameW(path, cap, out, NULL);
    return n && n < (DWORD)cap && w_launchable(out);
}

static bool w_registered_executable(const wchar_t *name, wchar_t *out, int cap) {
    const wchar_t *ext = wcsrchr(name,L'.');
    if (ext && _wcsicmp(ext,L".exe"))
        return false;
    wchar_t key[320];
    int len = swprintf(key,count_of(key),L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\%ls%ls",
                       name,ext ? L"" : L".exe");
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

bool w_os_provider(const char *name, wchar_t *out, int cap) {
    wchar_t raw[os_path_cap], path[os_path_cap];
    if (!w_os_wide(name, raw, count_of(raw)))
        return false;
    DWORD n = ExpandEnvironmentStringsW(raw, path, count_of(path));
    if (!n || n > count_of(path))
        return false;
    if (wcspbrk(path,L"\\/:"))
        return w_absolute_executable(path,out,cap);
    if (!*path || !_wcsicmp(path,L"explorer") || !_wcsicmp(path,L"explorer.exe"))
        return false;
    wchar_t *env = calloc(32768, sizeof(wchar_t));
    if (env) {
        DWORD len = GetEnvironmentVariableW(L"PATH", env, 32768);
        n = len && len < 32768 ? SearchPathW(env, path, L".exe", cap, out, NULL) : 0;
        free(env);
        if (n && n < (DWORD)cap && w_launchable(out))
            return true;
    }
    return w_registered_executable(path,out,cap);
}

bool w_os_task_manager(const char *name, wchar_t *out, int cap) {
    return w_os_provider(name,out,cap) || (!_stricmp(name,"TaskSlinger.exe") &&
           w_os_provider("%SystemRoot%\\System32\\Taskmgr.exe",out,cap));
}

// Encode a single Windows argv field, including its terminal run of backslashes.
static wchar_t *w_quote(wchar_t *p, const wchar_t *s) {
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
    size_t len = 2 * (wcslen(exe) + (arg ? wcslen(arg) : 0)) + 10;
    if (len > 32767)
        return NULL;
    wchar_t *line = calloc(len, sizeof(wchar_t));
    if (!line)
        return NULL;
    wchar_t *p = w_quote(line, exe);
    if (arg && *arg) {
        *p++ = L' ';
        w_quote(p, arg);
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

bool w_os_run_as_admin(const wchar_t *file, const wchar_t *parameters, const wchar_t *directory) {
    HRESULT apartment = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    SHELLEXECUTEINFOW info = {.cbSize = sizeof(info), .fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI,
        .lpVerb = L"runas", .lpFile = file, .lpParameters = parameters, .lpDirectory = directory,
        .nShow = SW_SHOWNORMAL};
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
        if (argument && *argument) w_quote(quoted, argument);
        return w_os_run_as_admin(exe, argument && *argument ? quoted : NULL, directory);
    }
    HANDLE child = w_launch_at(exe, argument, directory, NULL);
    if (!child) return false;
    CloseHandle(child);
    return true;
}

bool w_os_launch_wide_line(wchar_t *line, bool admin) {
    wchar_t path[os_path_cap], home[os_path_cap];
    int argc = 0;
    while (*line == L' ' || *line == L'\t') ++line;
    if (!*line) return false;
    wchar_t **argv = CommandLineToArgvW(line, &argc);
    char name[4096];
    bool found =
        argv && argc && w_os_utf8(argv[0], name, sizeof(name)) && w_os_provider(name, path, count_of(path));
    if (argv)
        LocalFree(argv);
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", home, count_of(home));
    if (!found || !n || n >= count_of(home))
        return false;
    if (admin) {
        // argv[0] is the executable. Give its original argument tail to ShellExecuteEx.
        const wchar_t *args = *line == L'"' ? wcschr(line + 1, L'"') : line + wcscspn(line, L" \t");
        if (args && *args == L'"') ++args;
        if (args) args += wcsspn(args, L" \t");
        return w_os_run_as_admin(path, args && *args ? args : NULL, home);
    }
    STARTUPINFOW si = {.cb = sizeof(si)};
    PROCESS_INFORMATION pi = {0};
    if (!CreateProcessW(path, line, NULL, NULL, FALSE, CREATE_UNICODE_ENVIRONMENT, NULL, home, &si, &pi))
        return false;
    AllowSetForegroundWindow(pi.dwProcessId);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

bool w_os_launch_line(const char *command, bool admin) {
    wchar_t line[os_path_cap];
    return w_os_wide(command, line, count_of(line)) && w_os_launch_wide_line(line, admin);
}

bool w_os_launch_terminal_at(const char *name, const wchar_t *directory, bool admin) {
    wchar_t path[os_path_cap];
    if (!strcmp(name, "auto")) {
        if (!w_os_provider("alacritty.exe", path, count_of(path)) &&
            !w_os_provider("%ProgramFiles%\\Alacritty\\alacritty.exe", path, count_of(path)) &&
            !w_os_provider("%SystemRoot%\\System32\\cmd.exe", path, count_of(path)))
            return false;
    } else if (!w_os_provider(name, path, count_of(path)))
        return false;
    return w_os_run_executable(path, NULL, directory, admin);
}

bool w_os_launch_terminal(const char *name, bool admin) {
    wchar_t home[os_path_cap];
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", home, count_of(home));
    return n && n < count_of(home) && w_os_launch_terminal_at(name, home, admin);
}

bool w_os_launch_provider(const char *name, const char *arg, bool admin) {
    wchar_t path[os_path_cap], param[os_path_cap];
    if (!w_os_provider(name, path, count_of(path)) || !w_os_wide(arg ? arg : "", param, count_of(param)))
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

bool w_os_candidate(HWND w) {
    if (!w || !IsWindowVisible(w) || w_os_same_process(w) || GetAncestor(w, GA_ROOT) != w ||
        w_os_app_window(w) != w || GetPropW(w, L"winwm.ignore"))
        return false;
    LONG_PTR ex = GetWindowLongPtrW(w, GWL_EXSTYLE), style = GetWindowLongPtrW(w, GWL_STYLE);
    if ((ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) || (style & WS_CHILD))
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
    bool zoomed = IsZoomed(w) != 0;
    WINDOWPLACEMENT placement = {.length = sizeof(placement)};
    MONITORINFO mi = {.cbSize = sizeof(mi)};
    if (zoomed && GetWindowPlacement(w, &placement) &&
        GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi)) {
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
    if (SetPropW(w, L"winwm.managed", (HANDLE)1)) {
        *original = r;
        if (zoomed)
            SetPropW(w, L"winwm.zoomed", (HANDLE)1);
        return true;
    }
    for (int i = 0; i < 4; i++)
        RemovePropW(w, coords[i]);
    return false;
}

// Serialize cross-thread SWP requests. A reversal waits for the previous request to land.
enum { hide_pending = 1, hidden_confirmed, show_pending };

static const wchar_t visibility_prop[] = L"winwm.hidden";
bool w_os_shell_hidden(HWND window) {
    return GetPropW(window, visibility_prop) != NULL;
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
    if (SetWindowPos(w, NULL, 0, 0, 0, 0, flags))
        return true;
    if (state)
        SetPropW(w, visibility_prop, (HANDLE)(uintptr_t)state);
    else
        RemovePropW(w, visibility_prop);
    return false;
}

static void w_release_visibility(HWND w) {
    if (RemovePropW(w, visibility_prop))
        SetWindowPos(w, NULL, 0, 0, 0, 0,
                     SWP_ASYNCWINDOWPOS | SWP_NOSIZE | SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE |
                         SWP_SHOWWINDOW);
}

static const wchar_t dim_prop[] = L"winwm.dim_added";
static void w_remove_dim(HWND w) {
    if (RemovePropW(w, dim_prop))
        SetWindowLongPtrW(w, GWL_EXSTYLE, GetWindowLongPtrW(w, GWL_EXSTYLE) & ~WS_EX_LAYERED);
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

bool w_os_direct_surface(HWND w) {
    if ((GetWindowLongPtrW(w, GWL_EXSTYLE) & (WS_EX_LAYERED | WS_EX_NOREDIRECTIONBITMAP)) ||
        (GetClassLongPtrW(w, GCL_STYLE) & (CS_OWNDC | CS_CLASSDC)))
        return true;
    // A foreign GetDC can return a common DC with no pixel format even when
    // the application is already presenting with OpenGL. In particular Winit
    // uses a full-redraw window class without CS_OWNDC. Check the rendering
    // runtime once at discovery, never in the animation or input loop.
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
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

void w_os_restore(HWND w) {
    if (!GetPropW(w, L"winwm.managed")) {
        w_release_visibility(w);
        return;
    }
    w_remove_dim(w);
    HANDLE corner = RemovePropW(w, L"winwm.corner");
    if (corner) {
        DWORD preference = (DWORD)((uintptr_t)corner - 1);
        DwmSetWindowAttribute(w, 33, &preference, sizeof(preference));
    }
    HANDLE border = RemovePropW(w, L"winwm.border");
    if (border) {
        DWORD color = (DWORD)((uintptr_t)border - 1);
        DwmSetWindowAttribute(w, 34, &color, sizeof(color));
    }
    w_rect r = w_remembered_bounds(w);
    bool zoomed = GetPropW(w, L"winwm.zoomed") != NULL;
    bool iconic = IsIconic(w) != 0;
    bool visible = IsWindowVisible(w) || w_os_shell_hidden(w);
    bool restored;
    if (iconic || IsZoomed(w) || (visible && zoomed)) {
        WINDOWPLACEMENT placement = {.length = sizeof(placement)};
        RECT normal = w_os_to_rect(r);
        MONITORINFO mon = {.cbSize = sizeof(mon)};
        restored = GetWindowPlacement(w, &placement) &&
                   GetMonitorInfoW(MonitorFromRect(&normal, MONITOR_DEFAULTTONEAREST), &mon);
        if (restored) {
            // WINDOWPLACEMENT uses workspace coordinates for managed top-level windows.
            OffsetRect(&normal, mon.rcMonitor.left-mon.rcWork.left, mon.rcMonitor.top-mon.rcWork.top);
            placement.rcNormalPosition = normal;
            placement.flags |= WPF_ASYNCWINDOWPLACEMENT;
            placement.showCmd = !visible ? SW_HIDE : iconic ? SW_SHOWMINNOACTIVE : SW_SHOWMAXIMIZED;
            if (iconic && zoomed)
                placement.flags |= WPF_RESTORETOMAXIMIZED;
            restored = SetWindowPlacement(w, &placement) != 0;
        }
    } else
        restored = SetWindowPos(w, NULL, r.x, r.y, r.w, r.h,
                                SWP_ASYNCWINDOWPOS | SWP_NOZORDER | SWP_NOACTIVATE) != 0;
    w_release_visibility(w);
    if (restored) {
        for (int i = 0; i < 4; i++)
            RemovePropW(w, coords[i]);
        RemovePropW(w, L"winwm.zoomed");
        RemovePropW(w, L"winwm.managed");
    }
}

static BOOL CALLBACK w_restore_one(HWND w, LPARAM arg) {
    (void)arg;
    w_os_restore(w);
    return TRUE;
}

void w_os_restore_all(void) {
    EnumWindows(w_restore_one, 0);
}

int w_os_error(wchar_t *dst, int cap) {
    return (int)FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
                               GetLastError(), 0, dst, cap, NULL);
}
