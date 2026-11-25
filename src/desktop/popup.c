#include "desktop/desktop.h"
#include "theme/metrics.h"
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
static wchar_t placement_key[popup_event_chars];
static RECT published_area;
static int published_gap;

static bool w_same_request(const w_popup_request *request, const w_popup_anchor *anchor, enum command_kind kind) {
    return request->kind == kind && w_popup_same_source(&request->anchor,anchor);
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
    if (palette && palette->bar == bar) w_palette_reanchor(NULL);
    const w_popup_anchor *notice=w_notice_anchor();
    if (notice && notice->bar==bar) w_notice_reanchor(NULL);
}

void w_desktop_popup_layout(w_bar_window *bar) {
    w_popup_anchor resolved;
    const w_popup_anchor *palette=w_palette_shown() ? w_palette_anchor() : NULL;
    if (palette && palette->bar==bar->hwnd)
        w_palette_reanchor(w_bar_resolve_anchor(bar,palette,&resolved) ? &resolved : NULL);
    const w_popup_anchor *notice=w_notice_anchor();
    if (notice && notice->bar==bar->hwnd)
        w_notice_reanchor(w_bar_resolve_anchor(bar,notice,&resolved) ? &resolved : NULL);
    if (next.kind && next.anchor.bar==bar->hwnd) {
        if (w_bar_resolve_anchor(bar,&next.anchor,&resolved)) next.anchor=resolved;
        else next.kind=cmd_invalid;
    }
    if (!child || current.anchor.bar!=bar->hwnd || WaitForSingleObject(cancel,0)!=WAIT_TIMEOUT) return;
    if (!w_bar_resolve_anchor(bar,&current.anchor,&resolved)) { SetEvent(cancel);return; }
    RECT area=w_popup_area(bar->hwnd);
    int gap=w_theme_spacing(bar->dpi);
    bool changed=!EqualRect(&current.anchor.button,&resolved.button) || strcmp(current.anchor.edge,resolved.edge) ||
                 !EqualRect(&published_area,&area) || published_gap!=gap;
    current.anchor=resolved;
    if (!changed) return;
    if (!w_popup_publish(&resolved,placement_key)) { SetEvent(cancel);return; }
    published_area=area;published_gap=gap;
    HWND window=GetPropW(bar->hwnd,w_popup_child_property);
    DWORD pid=0;
    if (window && GetWindowThreadProcessId(window,&pid) && pid==GetProcessId(child))
        PostMessageW(window,winwm_popup_moved,0,0);
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
    if (!w_popup_publish(&request.anchor,name)) { CloseHandle(event);return false; }
    swprintf(command, count_of(command), L"\"%ls\" --tool-%hs --bar-popup %llu %lu",
             exe, w_command_name(request.kind), (unsigned long long)(uintptr_t)request.anchor.bar,
             serial);
    STARTUPINFOW si = {.cb = sizeof(si)};
    PROCESS_INFORMATION pi = {0};
    if (!CreateProcessW(exe, command, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        RemovePropW(request.anchor.bar,name);
        CloseHandle(event);
        return false;
    }
    CloseHandle(pi.hThread);
    AllowSetForegroundWindow(pi.dwProcessId);
    current = request;
    wcscpy(placement_key,name);
    published_area=w_popup_area(request.anchor.bar);
    published_gap=w_theme_spacing(GetDpiForWindow(request.anchor.bar));
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
    RemovePropW(current.anchor.bar,placement_key);
    CloseHandle(cancel);
    CloseHandle(child);
    child = cancel = NULL;
    w_popup_request request = next;
    next.kind = cmd_invalid;
    if (request.kind) {
        for (int i=0;i<output_limit;++i) {
            w_bar_window *bar=&desk.outputs[i].bar;
            if (!desk.outputs[i].live || bar->hwnd!=request.anchor.bar) continue;
            UpdateWindow(bar->hwnd);
            w_popup_anchor latest;
            if (w_bar_resolve_anchor(bar,&request.anchor,&latest)) {
                request.anchor=latest;
                if (!w_start_popup(request)) w_desktop_notice("Could not open the toolbar panel.");
            }
            break;
        }
    }
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
