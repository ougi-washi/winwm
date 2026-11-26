// Launch the real no-argument entry point on a private desktop. Windows refuses
// to replace Explorer there. A second binary supplies a controllable handoff.
#include <windows.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>

static wchar_t desktop_name[80];
static HWND fixture;
static HANDLE job, entered, release, restored, input_unavailable, explorer_present, refuse_once;
static wchar_t pipe_name[128];

static LRESULT CALLBACK fixture_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_SYSCOMMAND && (wp & 0xfff0) == SC_HOTKEY &&
        (HWND)lp == (HWND)GetPropW(hwnd,L"winwm.test_desktop_target")) {
        SetPropW(fixture,L"winwm.test_foreground",(HANDLE)lp);
        return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}

static void pump(void) {
    MSG m;
    while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    Sleep(10);
}

static bool signaled(HANDLE object) {
    return WaitForSingleObject(object, 0) == WAIT_OBJECT_0;
}

static bool wait_for(HANDLE object) {
    ULONGLONG until = GetTickCount64() + 10000;
    while (!signaled(object) && GetTickCount64() < until) pump();
    return signaled(object);
}

static HANDLE launch(const wchar_t *exe, const wchar_t *args, DWORD *pid) {
    STARTUPINFOW si = {.cb = sizeof(si), .lpDesktop = desktop_name};
    PROCESS_INFORMATION pi;
    wchar_t line[MAX_PATH + 64];
    if (swprintf(line, MAX_PATH + 64, L"\"%ls\" %ls", exe, args) < 0 ||
        !CreateProcessW(exe, line, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &si, &pi))
        return NULL;
    if (!AssignProcessToJobObject(job, pi.hProcess) || ResumeThread(pi.hThread) == (DWORD)-1) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        pi.hProcess = NULL;
    }
    if (pid) *pid = pi.dwProcessId;
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

static HANDLE create_event(const wchar_t *variable) {
    wchar_t name[128];
    swprintf(name, 128, L"Local\\%ls-%ls", desktop_name, variable);
    if (!SetEnvironmentVariableW(variable, name)) return NULL;
    return CreateEventW(NULL, TRUE, FALSE, name);
}

static HWND owned_window(const wchar_t *class_name, DWORD pid) {
    HWND w = NULL;
    DWORD owner;
    while ((w = FindWindowExW(NULL, w, class_name, NULL))) {
        if (GetWindowThreadProcessId(w, &owner) && owner == pid) return w;
    }
    return NULL;
}

enum scenario { native, take_over, quit_during_handoff, repair_input, explorer_returns, transient_refusal, preview };

struct control_request { DWORD pid; const char *command, *expected; };

static DWORD WINAPI write_control(void *arg) {
    const struct control_request *request = arg;
    DWORD owner, bytes;
    HANDLE pipe = CreateFileW(pipe_name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                              SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, NULL);
    if (pipe == INVALID_HANDLE_VALUE) return 1;
    char reply[256] = {0};
    bool ok = GetNamedPipeServerProcessId(pipe, &owner) && owner == request->pid &&
              WriteFile(pipe, request->command, (DWORD)strlen(request->command) + 1, &bytes, NULL) &&
              ReadFile(pipe, reply, sizeof(reply) - 1, &bytes, NULL) && strstr(reply, request->expected);
    CloseHandle(pipe);
    return ok ? 0 : 1;
}

static bool send_control(DWORD pid, const char *command, const char *expected) {
    static struct control_request request;
    request = (struct control_request){pid, command, expected};
    HANDLE thread = CreateThread(NULL, 0, write_control, &request, 0, NULL);
    DWORD code = 1;
    if (!thread) return false;
    bool ok = wait_for(thread) && GetExitCodeThread(thread, &code) && !code;
    CloseHandle(thread);
    return ok;
}

static bool workspace_chords(DWORD pid) {
    HWND keyboard = FindWindowExW(HWND_MESSAGE, NULL, L"WinwmFixtureKeyboard", NULL);
    DWORD owner = 0;
    if (!keyboard || !GetWindowThreadProcessId(keyboard, &owner) || owner != pid) return false;
    DWORD_PTR registered = 0;
    if (!SendMessageTimeoutW(keyboard, WM_APP + 2, 0, 0, SMTO_ABORTIFHUNG, 1000, &registered) || !registered) {
        fputs("Could not query the running hook owner's registered raw input devices.\n", stderr);
        return false;
    }
    if (registered != 1) {
        fprintf(stderr, "Hook owner also registered raw mouse %u, raw keyboard %u.\n",
                (unsigned)((registered - 1) & 1), (unsigned)(((registered - 1) >> 1) & 1));
        return false;
    }
    // Start on the empty second workspace. Switch again, return, send the
    // only window away, follow it back and return from another empty space.
    static const struct { UINT key; unsigned workspace; bool visible; } steps[] = {
        {'3',3,false}, {'1',1,true}, {'S',1,false}, {'2',2,true}, {'F',1,true},
        {'2',2,false}, {'B',1,true}
    };
    for (unsigned i = 0; i < sizeof(steps) / sizeof(steps[0]); ++i) {
        HWND menu = NULL;
        if (i < 2) {
            if (!send_control(pid, "run menu", "accepted")) return false;
            ULONGLONG until = GetTickCount64() + 2000;
            do {
                menu = owned_window(L"WinwmPalette", pid);
                if (IsWindowVisible(menu)) break;
                pump();
            } while (GetTickCount64() < until);
            if (!IsWindowVisible(menu)) {
                fputs("Menu did not open before the workspace shortcut.\n", stderr);
                return false;
            }
        }
        DWORD_PTR matched = 0;
        if (!SendMessageTimeoutW(keyboard, WM_APP + 1, steps[i].key, 0, SMTO_ABORTIFHUNG, 1000, &matched) ||
            !matched) {
            fprintf(stderr, "Win+%c did not reach the workspace binding.\n", steps[i].key);
            return false;
        }
        char expected[32];
        snprintf(expected, sizeof(expected), "\"workspace\":%u,", steps[i].workspace);
        ULONGLONG until = GetTickCount64() + 3000;
        do {
            pump();
            if (send_control(pid, "query", expected) && !!IsWindowVisible(fixture) == steps[i].visible &&
                !IsWindowVisible(menu)) break;
        } while (GetTickCount64() < until);
        if (IsWindowVisible(menu)) {
            fputs("Menu kept keyboard focus after the workspace shortcut.\n", stderr);
            return false;
        }
        if (!send_control(pid, "query", expected) || !!IsWindowVisible(fixture) != steps[i].visible) {
            fprintf(stderr, "Win+%c failed to settle on workspace %u, fixture visible %d.\n",
                    steps[i].key, steps[i].workspace, steps[i].visible);
            return false;
        }
    }
    return true;
}

static bool delayed_activation(DWORD pid) {
    // A foreground notification for workspace 1 is delayed while the user
    // switches from workspace 2 to workspace 3. Its window stays shell-hidden
    // through both selections, so its original hide time alone is insufficient.
    DWORD old = GetTickCount();
    ULONGLONG until = GetTickCount64() + 60;
    while (GetTickCount64() < until) pump();
    if (!send_control(pid, "run workspace_select 3", "accepted") ||
        !send_control(pid, "query", "\"workspace\":3,") ||
        !SetPropW(fixture, L"winwm.test_foreground", fixture) ||
        !SetPropW(fixture, L"winwm.test_event_tick", (HANDLE)(uintptr_t)old)) return false;
    static const DWORD events[] = {EVENT_SYSTEM_FOREGROUND, EVENT_OBJECT_SHOW};
    bool ok = true;
    for (unsigned i = 0; ok && i < sizeof(events)/sizeof(events[0]); ++i) {
        RemovePropW(fixture, L"winwm.test_delivered");
        SetPropW(fixture, L"winwm.test_wait_event", (HANDLE)(uintptr_t)events[i]);
        if (events[i] == EVENT_OBJECT_SHOW) ShowWindow(fixture, SW_SHOWNOACTIVATE);
        else NotifyWinEvent(events[i], fixture, OBJID_WINDOW, CHILDID_SELF);
        until = GetTickCount64() + 2500;
        while (!GetPropW(fixture,L"winwm.test_delivered") && GetTickCount64() < until) pump();
        HWND root = owned_window(L"WinwmEvents", pid);
        // Trigger the same reconciliation used for lost WinEvents.
        if (!GetPropW(fixture,L"winwm.test_delivered") || !root ||
            !SendMessageTimeoutW(root, WM_TIMER, 29, 0, SMTO_ABORTIFHUNG, 1000, NULL)) ok = false;
        while (IsWindowVisible(fixture) && GetTickCount64() < until) pump();
        if (IsWindowVisible(fixture) || !send_control(pid, "query", "\"workspace\":3,")) ok = false;
        if (!ok) fprintf(stderr, "Delayed native event %lu reopened an earlier workspace.\n", events[i]);
    }
    RemovePropW(fixture, L"winwm.test_wait_event");
    RemovePropW(fixture, L"winwm.test_delivered");
    RemovePropW(fixture, L"winwm.test_event_tick");
    return ok;
}

static bool workspace_bursts(DWORD pid) {
    HWND keyboard = FindWindowExW(HWND_MESSAGE, NULL, L"WinwmFixtureKeyboard", NULL);
    DWORD owner = 0;
    if (!keyboard || !GetWindowThreadProcessId(keyboard, &owner) || owner != pid) return false;
    const char *const commands[] = {"213", "321", "132"};
    for (unsigned i = 0; i < 12; ++i) {
        const char *keys = commands[i % 3];
        for (int j = 0; j < 3; ++j) {
            DWORD_PTR matched = 0;
            if (!SendMessageTimeoutW(keyboard, WM_APP + 1, keys[j], 0, SMTO_ABORTIFHUNG, 1000, &matched) ||
                !matched) return false;
        }
        char expected[32];
        snprintf(expected, sizeof(expected), "\"workspace\":%c,", keys[2]);
        bool visible = keys[2] == '1';
        ULONGLONG until = GetTickCount64() + 2500;
        do {
            pump();
            if (send_control(pid, "query", expected) && !!IsWindowVisible(fixture) == visible) break;
        } while (GetTickCount64() < until);
        // Let native hide/show notifications generated by the burst drain.
        until = GetTickCount64() + 90;
        while (GetTickCount64() < until) pump();
        if (!send_control(pid, "query", expected) || !!IsWindowVisible(fixture) != visible) {
            fprintf(stderr, "Workspace burst %s lost its last selection.\n", keys);
            return false;
        }
    }
    return true;
}

static bool restored_activation(DWORD pid) {
    // A natively minimized app is deliberately left iconic across a workspace
    // switch. Reopening it has no manager-issued hide to acknowledge.
    SetPropW(fixture, L"winwm.test_foreground", GetDesktopWindow());
    ShowWindow(fixture, SW_MINIMIZE);
    ULONGLONG until = GetTickCount64() + 2500;
    while ((!IsIconic(fixture) || !send_control(pid, "query", "\"focus\":-1,")) && GetTickCount64() < until)
        pump();
    if (!IsIconic(fixture) || !send_control(pid, "query", "\"focus\":-1,") ||
        !send_control(pid, "run workspace_select 3", "accepted") ||
        !send_control(pid, "query", "\"workspace\":3,") || GetPropW(fixture, L"winwm.hidden")) return false;
    SetPropW(fixture, L"winwm.test_foreground", fixture);
    ShowWindow(fixture, SW_RESTORE);
    NotifyWinEvent(EVENT_SYSTEM_FOREGROUND, fixture, OBJID_WINDOW, CHILDID_SELF);
    until = GetTickCount64() + 2500;
    do {
        pump();
        if (send_control(pid, "query", "\"workspace\":1,") &&
            IsWindowVisible(fixture) && !IsIconic(fixture)) return true;
    } while (GetTickCount64() < until);
    fputs("A restored foreground application stayed on a hidden workspace.\n", stderr);
    return false;
}

static bool outgoing_activation(DWORD pid) {
    HWND root = owned_window(L"WinwmEvents",pid);
    if (!root || !SetPropW(fixture,L"winwm.test_desktop_target",root) ||
        !SetPropW(fixture,L"winwm.test_foreground",fixture) ||
        !send_control(pid,"run workspace_select 3","accepted")) return false;
    ULONGLONG until = GetTickCount64()+2000;
    while ((GetPropW(fixture,L"winwm.test_foreground") != root ||
            (uintptr_t)GetPropW(fixture,L"winwm.hidden") != 2) && GetTickCount64() < until) pump();
    if (GetPropW(fixture,L"winwm.test_foreground") != root || IsWindowVisible(fixture)) return false;
    // USER32 may give the outgoing app foreground again after acknowledging
    // its hide and the desktop's activation. The event itself is genuinely new.
    until = GetTickCount64()+35;
    while (GetTickCount64()<until) pump();
    SetPropW(fixture,L"winwm.test_foreground",fixture);
    NotifyWinEvent(EVENT_SYSTEM_FOREGROUND,fixture,OBJID_WINDOW,CHILDID_SELF);
    until = GetTickCount64()+1500;
    while (GetPropW(fixture,L"winwm.test_foreground") != root && GetTickCount64()<until) pump();
    bool ok = GetPropW(fixture,L"winwm.test_foreground") == root && !IsWindowVisible(fixture) &&
              send_control(pid,"query","\"workspace\":3,");
    if (!ok) fputs("The withdrawn window reclaimed its workspace or kept native keyboard foreground.\n",stderr);
    if (ok) {
        WNDCLASSW wc = {.lpfnWndProc = fixture_proc, .hInstance = GetModuleHandleW(NULL),
                        .lpszClassName = L"WinwmUnmanagedForegroundFixture"};
        RegisterClassW(&wc);
        HWND helper = CreateWindowExW(WS_EX_TOOLWINDOW,wc.lpszClassName,L"",WS_POPUP,10,10,48,32,
                                      NULL,NULL,wc.hInstance,NULL);
        ok = helper && SetPropW(helper,L"winwm.test_desktop_target",root) &&
             SetPropW(fixture,L"winwm.test_foreground",helper);
        if (ok) {
            NotifyWinEvent(EVENT_SYSTEM_FOREGROUND,helper,OBJID_WINDOW,CHILDID_SELF);
            until = GetTickCount64()+1500;
            while (GetPropW(fixture,L"winwm.test_foreground") != root && GetTickCount64()<until) pump();
            ok = GetPropW(fixture,L"winwm.test_foreground") == root;
            if (!ok) fputs("An invisible unmanaged helper kept foreground on the empty workspace.\n",stderr);
        }
        if (ok) {
            ShowWindow(helper,SW_SHOWNOACTIVATE);
            SetPropW(fixture,L"winwm.test_foreground",helper);
            NotifyWinEvent(EVENT_SYSTEM_FOREGROUND,helper,OBJID_WINDOW,CHILDID_SELF);
            until = GetTickCount64()+150;
            while (GetTickCount64()<until) pump();
            ok = GetPropW(fixture,L"winwm.test_foreground") == helper;
            if (!ok) fputs("The empty workspace displaced a visible unmanaged dialog.\n",stderr);
        }
        if (helper) DestroyWindow(helper);
    }
    if (ok) {
        HWND keyboard = FindWindowExW(HWND_MESSAGE,NULL,L"WinwmFixtureKeyboard",NULL);
        DWORD owner = 0;
        DWORD_PTR dispatched = 0;
        ok = keyboard && GetWindowThreadProcessId(keyboard,&owner) && owner == pid &&
             SendMessageTimeoutW(keyboard,WM_APP+3,0,0,SMTO_ABORTIFHUNG,1000,&dispatched) && dispatched;
        SetPropW(fixture,L"winwm.test_foreground",fixture);
        NotifyWinEvent(EVENT_SYSTEM_FOREGROUND,fixture,OBJID_WINDOW,CHILDID_SELF);
        until = GetTickCount64()+1500;
        while (!IsWindowVisible(fixture) && GetTickCount64()<until) pump();
        ok = ok && IsWindowVisible(fixture) && send_control(pid,"query","\"workspace\":1,");
        if (!ok) fputs("A new user input failed to permit an intentional singleton reactivation.\n",stderr);
    }
    RemovePropW(fixture,L"winwm.test_desktop_target");
    return ok;
}

static bool hidden_activation(DWORD pid) {
    ULONGLONG until = GetTickCount64() + 3000;
    while ((uintptr_t)GetPropW(fixture, L"winwm.hidden") != 2 && GetTickCount64() < until) pump();
    if ((uintptr_t)GetPropW(fixture, L"winwm.hidden") != 2 ||
        !SetPropW(fixture, L"winwm.test_foreground", GetDesktopWindow()) ||
        !send_control(pid, "run split vertical", "accepted"))
        return false;
    until = GetTickCount64() + 250;
    while (GetTickCount64() < until) pump();
    if (!SetPropW(fixture, L"winwm.test_foreground", fixture)) return false;
    // The foreground query can return a hidden outgoing app without an
    // activation after the switch. An ordinary layout must stay on workspace 2.
    bool ok = send_control(pid, "run split horizontal", "accepted");
    until = GetTickCount64() + 250;
    while (GetTickCount64() < until) pump();
    ok = ok && !IsWindowVisible(fixture) && send_control(pid, "query", "\"workspace\":2,");
    if (!ok) fputs("A layout reopened the old workspace from a sampled foreground HWND.\n", stderr);
    if (ok) {
        SetPropW(fixture, L"winwm.test_foreground", GetDesktopWindow());
        ok = delayed_activation(pid);
    }
    // A new real foreground notification from a shell-hidden single-instance
    // application, such as Windows Security, should still reveal its workspace.
    if (ok) {
        // Exercise Windows' coarse time resolution. The foreground changed
        // from the desktop to the app despite both actions sharing one tick.
        SetPropW(fixture, L"winwm.test_event_tick", (HANDLE)(INT_PTR)-1);
        NotifyWinEvent(EVENT_SYSTEM_FOREGROUND, fixture, OBJID_WINDOW, CHILDID_SELF);
        until = GetTickCount64() + 3000;
        while (!IsWindowVisible(fixture) && GetTickCount64() < until) pump();
        ok = IsWindowVisible(fixture) && send_control(pid, "query", "\"workspace\":1,");
        RemovePropW(fixture, L"winwm.test_event_tick");
        if (!ok) fputs("A fresh activation failed to reopen a hidden application.\n", stderr);
    }
    if (ok) ok = restored_activation(pid);
    if (ok) ok = outgoing_activation(pid);
    RemovePropW(fixture, L"winwm.test_foreground");
    return ok && send_control(pid, "run workspace_select 2", "accepted");
}

static bool run(const wchar_t *exe, enum scenario scenario) {
    ResetEvent(entered);
    ResetEvent(release);
    ResetEvent(restored);
    SetEvent(explorer_present);
    if (scenario == transient_refusal) SetEvent(refuse_once);
    else ResetEvent(refuse_once);
    if (scenario == native) {
        DWORD sid;
        if (!ProcessIdToSessionId(GetCurrentProcessId(), &sid)) return false;
        swprintf(pipe_name, 128, L"\\\\.\\pipe\\winwm.v1.%lu", sid);
    } else swprintf(pipe_name, 128, L"\\\\.\\pipe\\%ls.0", desktop_name);
    if (scenario == repair_input) SetEvent(input_unavailable);
    else ResetEvent(input_unavailable);
    DWORD pid = 0;
    HANDLE manager = launch(exe, scenario == preview ? L"--preview" : L"", &pid);
    if (!manager) return false;
    HWND root = NULL, bar = NULL, host = NULL;
    ULONGLONG deadline = GetTickCount64() + 15000;
    while (!signaled(manager) && GetTickCount64() < deadline) {
        pump();
        root = owned_window(L"WinwmEvents", pid);
        bar = owned_window(L"WinwmBar", pid);
        host = FindWindowW(L"WinwmHost", NULL);
        if (root && bar && (DWORD)(uintptr_t)GetPropW(host, L"winwm.session_pid") == pid &&
            GetPropW(fixture, L"winwm.managed") &&
            (scenario == native || scenario == preview || scenario == repair_input || signaled(entered))) break;
    }
    DWORD exit_code = 0;
    GetExitCodeProcess(manager, &exit_code);
    if (!root || !bar || !host || signaled(manager) || !GetPropW(fixture, L"winwm.managed") ||
        !!IsWindowVisible(root) != (scenario != preview) || !!GetPropW(bar, L"winwm.preview") != (scenario == preview)) {
        fprintf(stderr, "Manager failed to initialize in the requested mode (exit %lu).\n", exit_code);
        return false;
    }
    MONITORINFO mi = {.cbSize = sizeof(mi)};
    RECT dock;
    if (!GetMonitorInfoW(MonitorFromWindow(bar, MONITOR_DEFAULTTONEAREST), &mi) || !GetWindowRect(bar, &dock) ||
        dock.bottom != (scenario == preview ? mi.rcWork.bottom : mi.rcMonitor.bottom) ||
        !SendMessageTimeoutW(bar, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 1000, NULL)) {
        fputs("The bottom bar is unresponsive or reserved an extra taskbar strip.\n", stderr);
        return false;
    }
    DWORD host_pid = 0;
    GetWindowThreadProcessId(host, &host_pid);
    HANDLE monitor = OpenProcess(SYNCHRONIZE, FALSE, host_pid);
    if (!monitor || !SendMessageTimeoutW(host, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 1000, NULL)) {
        fputs("Recovery monitor is unresponsive during shell handoff.\n", stderr);
        return false;
    }
    // The control pipe is session-wide. Check the PID on the connected handle
    // before sending anything, even if the test process exits unexpectedly.
    if (!send_control(pid, "run workspace_select 2", "accepted")) {
        fputs("Workspace command failed.\n", stderr);
        return false;
    }
    deadline = GetTickCount64() + 5000;
    while (IsWindowVisible(fixture) && GetTickCount64() < deadline) pump();
    if (IsWindowVisible(fixture)) return false;
    if (scenario == repair_input) {
        if (WaitForSingleObject(entered, 250) != WAIT_TIMEOUT) {
            fputs("Manager requested shell takeover with unavailable input.\n", stderr);
            return false;
        }
        ResetEvent(input_unavailable);
        if (!wait_for(entered)) { fputs("Input did not recover automatically.\n", stderr); return false; }
    }
    if (scenario == transient_refusal) {
        ResetEvent(entered);
        if (!wait_for(entered)) { fputs("No retry after a refused shell handoff.\n", stderr); return false; }
    }
    if (scenario != native && scenario != preview && scenario != quit_during_handoff) {
        SetEvent(release);
        deadline = GetTickCount64() + 5000;
        while (!owned_window(L"Shell_TrayWnd", pid) && GetTickCount64() < deadline) pump();
        if (!owned_window(L"Shell_TrayWnd", pid) || !IsWindowVisible(root) || GetPropW(bar, L"winwm.preview")) {
            fputs("Manager failed to take ownership after shell handoff.\n", stderr);
            return false;
        }
        if (scenario == explorer_returns) {
            ResetEvent(entered);
            ResetEvent(release);
            SetEvent(explorer_present); // A new shell appears without broadcasting TaskbarCreated.
            if (!wait_for(entered) || !owned_window(L"Shell_TrayWnd", pid) || GetPropW(bar, L"winwm.preview"))
                return false;
            SetEvent(release);
            deadline = GetTickCount64() + 5000;
            while (signaled(explorer_present) && GetTickCount64() < deadline) pump();
            if (signaled(explorer_present)) return false;
        }
    }
    if (scenario == preview && (signaled(entered) || owned_window(L"Shell_TrayWnd", pid))) return false;
    if (scenario == take_over && (!hidden_activation(pid) || !workspace_bursts(pid))) return false;
    if ((scenario == take_over || scenario == repair_input || scenario == preview) && !workspace_chords(pid))
        return false;
    if (!PostMessageW(root, WM_CLOSE, 0, 0) || !wait_for(manager) ||
        !GetExitCodeProcess(manager, &exit_code) || exit_code) {
        fputs("Manager failed to exit while closing its desktop.\n", stderr);
        return false;
    }
    if (scenario == quit_during_handoff) {
        if (signaled(monitor) || signaled(restored)) return false;
        SetEvent(release);
    }
    bool ok = wait_for(monitor) && IsWindowVisible(fixture) && !GetPropW(fixture, L"winwm.managed") &&
              (scenario == native || (signaled(restored) == (scenario != preview)));
    CloseHandle(monitor);
    CloseHandle(manager);
    return ok;
}

int wmain(int argc, wchar_t **argv) {
    wchar_t exe[MAX_PATH], delayed[MAX_PATH], temp[MAX_PATH], profile[MAX_PATH];
    if (argc != 3) return 2;
    HANDLE existing = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\winwm.interactive.v1");
    bool occupied = existing != NULL;
    if (existing) CloseHandle(existing);
    DWORD n = GetTempPathW(MAX_PATH, temp);
    DWORD first = GetFullPathNameW(argv[1], MAX_PATH, exe, NULL);
    DWORD second = GetFullPathNameW(argv[2], MAX_PATH, delayed, NULL);
    if (!n || n >= MAX_PATH || !first || first >= MAX_PATH || !second || second >= MAX_PATH) return 2;
    swprintf(desktop_name, 80, L"WinwmStartupTest-%lu-%llu", GetCurrentProcessId(), GetTickCount64());
    if (n + wcslen(desktop_name) >= MAX_PATH) return 2;
    swprintf(profile, MAX_PATH, L"%ls%ls", temp, desktop_name);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HDESK desktop = CreateDesktopW(desktop_name, NULL, NULL, 0, GENERIC_ALL, NULL);
    if (!desktop || !SetThreadDesktop(desktop) || !CreateDirectoryW(profile, NULL) || !SetCurrentDirectoryW(profile) ||
        !SetEnvironmentVariableW(L"LOCALAPPDATA", profile) || !SetEnvironmentVariableW(L"USERPROFILE", profile) ||
        !SetEnvironmentVariableW(L"WINWM_TEST_NAMESPACE", desktop_name) ||
        !CreateDirectoryW(L".config", NULL) || !CreateDirectoryW(L".config\\winwm", NULL))
        return 2;
    FILE *config = _wfopen(L".config\\winwm\\winwm.lua", L"wb");
    if (!config) return 2;
    fputs("hl.config({bar={position='bottom',height=32}})\nhl.bind('SUPER + D',hl.dsp.winwm('menu'))\n"
          "for i=1,3 do hl.bind('SUPER + '..i,hl.dsp.focus({workspace=i})) end\n"
          "hl.bind('SUPER + S',hl.dsp.window.move({workspace=2,follow=false}))\n"
          "hl.bind('SUPER + F',hl.dsp.window.move({workspace=1,follow=true}))\n"
          "hl.bind('SUPER + B',hl.dsp.focus({workspace='previous'}))\n", config);
    if (fclose(config)) return 2;
    WNDCLASSW wc = {.lpfnWndProc = fixture_proc, .hInstance = GetModuleHandleW(NULL),
                    .lpszClassName = L"WinwmStartupFixture"};
    if (!RegisterClassW(&wc)) return 2;
    fixture = CreateWindowW(wc.lpszClassName, L"Workspace fixture", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                            120, 130, 430, 300, NULL, NULL, wc.hInstance, NULL);
    entered = create_event(L"WINWM_TEST_HANDOFF_ENTERED");
    release = create_event(L"WINWM_TEST_HANDOFF_RELEASE");
    restored = create_event(L"WINWM_TEST_HANDOFF_RESTORED");
    input_unavailable = create_event(L"WINWM_TEST_INPUT_UNAVAILABLE");
    explorer_present = create_event(L"WINWM_TEST_EXPLORER_PRESENT");
    refuse_once = create_event(L"WINWM_TEST_REFUSE_ONCE");
    job = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!fixture || !entered || !release || !restored || !input_unavailable || !explorer_present || !refuse_once || !job ||
        !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) return 2;
    const char *names[] = {"production startup", "pending then successful takeover", "quit during pending takeover",
                           "input failure repaired automatically", "Explorer reappears without a broadcast",
                           "retry after a refused handoff", "explicit Explorer preview"};
    bool ok = true;
    if (occupied) puts("An existing winwm session is active. Running the private-namespace scenarios.");
    for (int i = occupied ? take_over : native; i <= preview; ++i) {
        ok = run(i == native ? exe : delayed, (enum scenario)i);
        printf("%s: %s\n", names[i], ok ? "passed" : "failed");
        if (!ok) break;
    }
    CloseHandle(job);
    CloseHandle(entered);
    CloseHandle(release);
    CloseHandle(restored);
    CloseHandle(input_unavailable);
    CloseHandle(explorer_present);
    CloseHandle(refuse_once);
    DestroyWindow(fixture);
    DeleteFileW(L".config\\winwm\\winwm.lua");
    DeleteFileW(L"winwm\\apps.v3.bin");
    DeleteFileW(L"winwm\\startup.bin");
    RemoveDirectoryW(L".config\\winwm");
    RemoveDirectoryW(L".config");
    RemoveDirectoryW(L"winwm");
    SetCurrentDirectoryW(temp);
    RemoveDirectoryW(profile);
    return ok ? 0 : 1;
}
