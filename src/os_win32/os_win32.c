#include "os_win32/os_win32.h"
#include <dwmapi.h>
#include <shellapi.h>
#include <evntprov.h>
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

bool w_os_provider(const char *name, wchar_t *out, int cap) {
    wchar_t raw[os_path_cap], path[os_path_cap];
    if (!w_os_wide(name, raw, count_of(raw)))
        return false;
    DWORD n = ExpandEnvironmentStringsW(raw, path, count_of(path));
    if (!n || n > count_of(path))
        return false;
    if (!_wcsicmp(path, L"brave.exe")) {
        const HKEY roots[] = {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE};
        for (size_t i = 0; i < count_of(roots); ++i) {
            DWORD bytes = cap * sizeof(*out);
            if (RegGetValueW(roots[i], L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\brave.exe",
                             NULL, RRF_RT_REG_SZ, NULL, out, &bytes) == ERROR_SUCCESS &&
                wcsrchr(out, L'\\') && !_wcsicmp(wcsrchr(out, L'\\') + 1, L"brave.exe") && w_launchable(out))
                return true;
        }
        const wchar_t *locations[] = {L"%LOCALAPPDATA%", L"%ProgramFiles%", L"%ProgramFiles(x86)%"};
        for (size_t i = 0; i < count_of(locations); ++i) {
            const wchar_t *suffix = L"\\BraveSoftware\\Brave-Browser\\Application\\brave.exe";
            DWORD length = ExpandEnvironmentStringsW(locations[i], out, cap);
            if (length && length + wcslen(suffix) <= (size_t)cap && w_launchable(wcscat(out, suffix)))
                return true;
        }
    }
    if (wcschr(path, L'\\') || wcschr(path, L'/')) {
        if (!(path[0] && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')) &&
            wcsncmp(path, L"\\\\", 2))
            return false;
        n = GetFullPathNameW(path, cap, out, NULL);
    } else {
        wchar_t *env = calloc(32768, sizeof(wchar_t));
        if (!env)
            return false;
        DWORD len = GetEnvironmentVariableW(L"PATH", env, 32768);
        n = len && len < 32768 ? SearchPathW(env, path, L".exe", cap, out, NULL) : 0;
        free(env);
    }
    if (!n || n >= (DWORD)cap)
        return false;
    return w_launchable(out);
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

static HANDLE w_launch_at(const wchar_t *exe, const wchar_t *arg, const wchar_t *directory) {
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
    PROCESS_INFORMATION pi = {0};
    BOOL ok =
        CreateProcessW(exe, line, NULL, NULL, FALSE, CREATE_UNICODE_ENVIRONMENT, NULL, directory, &si, &pi);
    free(line);
    if (!ok)
        return NULL;
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

HANDLE w_os_launch(const wchar_t *exe, const wchar_t *arg) {
    return w_launch_at(exe, arg, NULL);
}

bool w_os_launch_line(const char *command) {
    wchar_t line[os_path_cap], path[os_path_cap], home[os_path_cap];
    int argc = 0;
    if (!w_os_wide(command, line, count_of(line)))
        return false;
    wchar_t **argv = CommandLineToArgvW(line, &argc);
    char name[4096];
    bool found =
        argv && argc && w_os_utf8(argv[0], name, sizeof(name)) && w_os_provider(name, path, count_of(path));
    if (argv)
        LocalFree(argv);
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", home, count_of(home));
    if (!found || !n || n >= count_of(home))
        return false;
    STARTUPINFOW si = {.cb = sizeof(si)};
    PROCESS_INFORMATION pi = {0};
    if (!CreateProcessW(path, line, NULL, NULL, FALSE, CREATE_UNICODE_ENVIRONMENT, NULL, home, &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

bool w_os_launch_terminal(const char *name) {
    wchar_t home[os_path_cap], path[os_path_cap];
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", home, count_of(home));
    if (!n || n >= count_of(home))
        return false;
    if (!strcmp(name, "auto")) {
        if (!w_os_provider("alacritty.exe", path, count_of(path)) &&
            !w_os_provider("%ProgramFiles%\\Alacritty\\alacritty.exe", path, count_of(path)) &&
            !w_os_provider("%SystemRoot%\\System32\\cmd.exe", path, count_of(path)))
            return false;
    } else if (!w_os_provider(name, path, count_of(path)))
        return false;
    HANDLE p = w_launch_at(path, NULL, home);
    if (!p)
        return false;
    CloseHandle(p);
    return true;
}

bool w_os_launch_provider(const char *name, const char *arg) {
    wchar_t path[os_path_cap], param[os_path_cap];
    if (!w_os_provider(name, path, count_of(path)) || !w_os_wide(arg ? arg : "", param, count_of(param)))
        return false;
    HANDLE p = w_os_launch(path, param);
    if (!p)
        return false;
    CloseHandle(p);
    return true;
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
           (!wcscmp(name, L"WorkerW") || !wcscmp(name, L"Progman"));
}

bool w_os_candidate(HWND w) {
    if (!w || !IsWindowVisible(w) || w_os_same_process(w) || GetAncestor(w, GA_ROOT) != w ||
        GetWindow(w, GW_OWNER) || GetPropW(w, L"winwm.ignore"))
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
    DWORD cloaked = 0;
    DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    return !cloaked;
}

// Properties are owned by the HWND lifetime. A reused handle never inherits recovery data.
static const wchar_t *const coords[] = {L"winwm.x", L"winwm.y", L"winwm.w",
                                        L"winwm.h"};

////////////////
//~ Window recovery

bool w_os_remember(HWND w) {
    if (GetPropW(w, L"winwm.managed"))
        return true;
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

void w_os_window_appearance(HWND w, bool focused, DWORD color, BYTE alpha) {
    DWORD previous;
    if (GetPropW(w, L"winwm.border") ||
        (SUCCEEDED(DwmGetWindowAttribute(w, 34, &previous, sizeof(previous))) &&
         SetPropW(w, L"winwm.border", (HANDLE)(uintptr_t)((uint64_t)previous + 1))))
        DwmSetWindowAttribute(w, 34, &color, sizeof(color));
    if (alpha == 255) {
        w_remove_dim(w);
        return;
    }
    if (GetPropW(w, dim_prop)) {
        // Changing the extended style on every focus transition rebuilds the window's
        // redirection surface. Keep our layer and change only its opacity.
        SetLayeredWindowAttributes(w, 0, focused ? 255 : alpha, LWA_ALPHA);
        return;
    }
    if (focused)
        return;
    LONG_PTR ex = GetWindowLongPtrW(w, GWL_EXSTYLE);
    if ((ex & (WS_EX_LAYERED | WS_EX_NOREDIRECTIONBITMAP)) ||
        (GetClassLongPtrW(w, GCL_STYLE) & (CS_OWNDC | CS_CLASSDC)))
        return;
    if (SetPropW(w, dim_prop, (HANDLE)1)) {
        SetWindowLongPtrW(w, GWL_EXSTYLE, ex | WS_EX_LAYERED);
        if (!SetLayeredWindowAttributes(w, 0, alpha, LWA_ALPHA))
            w_remove_dim(w);
    }
}

void w_os_restore(HWND w) {
    if (!RemovePropW(w, L"winwm.managed")) {
        w_release_visibility(w);
        return;
    }
    w_remove_dim(w);
    HANDLE border = RemovePropW(w, L"winwm.border");
    if (border) {
        DWORD color = (DWORD)((uintptr_t)border - 1);
        DwmSetWindowAttribute(w, 34, &color, sizeof(color));
    }
    int v[4];
    for (int i = 0; i < 4; i++)
        v[i] = (int)((uintptr_t)RemovePropW(w, coords[i]) - 1);
    if (!IsIconic(w) && !IsZoomed(w))
        SetWindowPos(w, NULL, v[0], v[1], v[2], v[3], SWP_ASYNCWINDOWPOS | SWP_NOZORDER | SWP_NOACTIVATE);
    w_release_visibility(w);
    if (RemovePropW(w, L"winwm.zoomed"))
        ShowWindowAsync(w, SW_SHOWMAXIMIZED);
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
