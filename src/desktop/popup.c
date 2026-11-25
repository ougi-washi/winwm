#include "desktop/desktop.h"
#include "win_platform/popup.h"

typedef struct w_popup_request {
    w_popup_anchor anchor;
    enum command_kind kind;
} w_popup_request;

// Only the session thread changes these. A switch coalesces to the last requested
// panel while the previous process closes, including during its startup.
static w_popup_request current, next;
static HANDLE child, cancel;
static DWORD serial;

static bool w_same_request(const w_popup_request *request, const w_popup_anchor *anchor, enum command_kind kind) {
    return request->kind == kind && request->anchor.bar == anchor->bar &&
           request->anchor.source == anchor->source;
}

HANDLE w_desktop_popup_process(void) {
    return child;
}

void w_desktop_popup_cancel(void) {
    next.kind = cmd_invalid;
    if (cancel) SetEvent(cancel);
}

void w_desktop_popup_bar_hidden(HWND bar) {
    if (next.anchor.bar == bar) next.kind = cmd_invalid;
    if (cancel && current.anchor.bar == bar) SetEvent(cancel);
    const w_popup_anchor *palette = w_palette_anchor();
    if (palette && palette->bar == bar) w_palette_outside(NULL);
}

static bool w_start_popup(w_popup_request request) {
    if (!IsWindowVisible(request.anchor.bar)) return false;
    wchar_t exe[os_path_cap], command[os_path_cap + 256], name[popup_event_chars];
    if (!w_os_self(exe, count_of(exe))) return false;
    w_popup_event_name(name, GetCurrentProcessId(), ++serial);
    HANDLE event = CreateEventW(NULL, TRUE, FALSE, name);
    if (!event) return false;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(event);
        return false;
    }
    RECT r = request.anchor.button;
    swprintf(command, count_of(command), L"\"%ls\" --tool-%hs --bar-popup %llu %ld %ld %ld %ld %hs %lu",
             exe, w_command_name(request.kind), (unsigned long long)(uintptr_t)request.anchor.bar,
             r.left, r.top, r.right, r.bottom, request.anchor.edge, serial);
    STARTUPINFOW si = {.cb = sizeof(si)};
    PROCESS_INFORMATION pi = {0};
    if (!CreateProcessW(exe, command, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(event);
        return false;
    }
    CloseHandle(pi.hThread);
    AllowSetForegroundWindow(pi.dwProcessId);
    current = request;
    cancel = event;
    child = pi.hProcess;
    return true;
}

bool w_desktop_popup_toggle(const w_popup_anchor *anchor, enum command_kind kind) {
    bool same = w_same_request(&next, anchor, kind) ||
                (child && WaitForSingleObject(child, 0) == WAIT_TIMEOUT &&
                 WaitForSingleObject(cancel, 0) == WAIT_TIMEOUT && w_same_request(&current, anchor, kind));
    w_desktop_popup_cancel();
    if (same) return true;
    w_popup_request request = {.anchor = *anchor, .kind = kind};
    if (!child) return w_start_popup(request);
    next = request;
    return true;
}

void w_desktop_popup_finished(void) {
    CloseHandle(cancel);
    CloseHandle(child);
    child = cancel = NULL;
    w_popup_request request = next;
    next.kind = cmd_invalid;
    if (request.kind && !w_start_popup(request))
        w_desktop_notice("Could not open the toolbar panel.");
    if (w_desktop_focus_pending()) w_desktop_schedule();
}

void w_desktop_popup_outside(HWND hit) {
    if (!child) return;
    HWND root = GetAncestor(hit, GA_ROOT);
    for (int i = 0; i < output_limit; ++i)
        if (root && desk.outputs[i].bar.hwnd == root) return;
    DWORD pid = GetProcessId(child);
    for (HWND window = root; window; window = GetWindow(window, GW_OWNER)) {
        DWORD owner = 0;
        GetWindowThreadProcessId(window, &owner);
        if (owner == pid) return;
    }
    w_desktop_popup_cancel();
}
