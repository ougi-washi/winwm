#include "theme/theme.h"
#include "desktop/entry.h"
#include "desktop/desktop.h"
#include "providers/audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

////////////////
//~ Desktop

w_desktop_state desk;
static bool event_overflow, preview_mode, loaded_programs;
static int pending_events;
static UINT shell_notification;

////////////////
//~ Desktop Functions

static void w_invalidate_language(void) {
    for (int i = 0; i < output_limit; ++i)
        if (desk.outputs[i].live && !desk.outputs[i].bar_hidden)
            w_bar_invalidate(&desk.outputs[i].bar, "keyboard");
}

static DWORD explorer_status = ERROR_IO_PENDING;
typedef struct w_config_result {
    w_desktop_config cfg;
    LONG revision;
    char error[256];
    bool ok;
} w_config_result;

static SRWLOCK cache_lock = SRWLOCK_INIT;
static LONG config_revision;
static DWORD WINAPI w_save_config(void *ptr) {
    w_config_result *r = ptr;
    AcquireSRWLockExclusive(&cache_lock);
    if (r->revision == InterlockedCompareExchange(&config_revision, 0, 0))
        w_os_cache_write(&r->cfg);
    ReleaseSRWLockExclusive(&cache_lock);
    free(r);
    return 0;
}

static DWORD WINAPI w_load_thread(void *unused) {
    (void)unused;
    w_config_result *r = calloc(1, sizeof(*r));
    if (r)
        r->ok = w_os_config_load(&r->cfg, r->error, sizeof(r->error));
    if (!PostMessageW(desk.root, msg_load, 0, (LPARAM)r))
        free(r);
    return 0;
}

void w_desktop_load(void) {
    if (desk.loading)
        return;
    desk.loading = QueueUserWorkItem(w_load_thread, NULL, WT_EXECUTEDEFAULT) != 0;
}

void w_desktop_schedule(void) {
    if (!desk.refresh_pending)
        desk.refresh_pending = PostMessageW(desk.root, msg_refresh, 0, 0) != 0;
}

void w_desktop_bind(void) {
    w_theme_colors(desk.config.background, desk.config.foreground, desk.config.accent);
    w_input_bind(desk.root, msg_command, msg_hover, preview_mode ? 0 : msg_desktop_menu,
               &desk.config, desk.registered, w_desktop_drag);
}

static void CALLBACK w_observe(HWINEVENTHOOK hook, DWORD event, HWND w, LONG obj, LONG child, DWORD thread,
                             DWORD time) {
    (void)hook;
    (void)thread;
    (void)time;
    if (event == EVENT_SYSTEM_DESKTOPSWITCH) {
        w_input_reset();
        return;
    }
    if (!w || obj != OBJID_WINDOW || child != CHILDID_SELF)
        return;
    if (event >= EVENT_OBJECT_CREATE && event != EVENT_OBJECT_CREATE && event != EVENT_OBJECT_DESTROY &&
        event != EVENT_OBJECT_SHOW && event != EVENT_OBJECT_HIDE && event != EVENT_OBJECT_LOCATIONCHANGE &&
        event != EVENT_OBJECT_NAMECHANGE)
        return;
    if (pending_events >= 512) {
        event_overflow = true;
        return;
    }
    if (PostMessageW(desk.root, msg_window, event, (LPARAM)w))
        pending_events++;
}

static struct w_queued_command {
    w_command action;
    int output;
} pending_commands[32];
static int command_count;
bool w_desktop_defer(const w_command *c, int output) {
    if (command_count == (int)count_of(pending_commands))
        return false;
    pending_commands[command_count++] = (struct w_queued_command){*c, output};
    return true;
}

////////////////
//~ Session startup

static void w_discover_initial(void) {
    DWORD range[][2] = {{EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND},
                        {EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MOVESIZEEND},
                        {EVENT_SYSTEM_DESKTOPSWITCH, EVENT_SYSTEM_DESKTOPSWITCH},
                        {EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND},
                        {EVENT_OBJECT_CREATE, EVENT_OBJECT_HIDE},
                        {EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_NAMECHANGE}};
    for (size_t i = 0; i < count_of(range); i++) {
        desk.hooks[i] = SetWinEventHook(range[i][0], range[i][1], NULL, w_observe, 0, 0,
                                        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        if (!desk.hooks[i])
            w_desktop_notice("Window event hook unavailable");
    }
    w_os_restore_all();
    w_desktop_scan();
    desk.ready = true;
    for (int i = 0; i < command_count; i++)
        w_desktop_run_on(&pending_commands[i].action, pending_commands[i].output);
    command_count = 0;
}

static DWORD WINAPI w_handoff_explorer(void *root) {
    DWORD error = w_os_exit_explorer();
    PostMessageW((HWND)root, msg_shell_exit, error, 0);
    return 0;
}

static void w_initialize(void) {
    shell_notification = RegisterWindowMessageW(L"SHELLHOOK");
    if (shell_notification)
        RegisterShellHookWindow(desk.root);
    w_os_area_open(false);
    w_desktop_outputs();
    if (!preview_mode && !QueueUserWorkItem(w_handoff_explorer, desk.root, WT_EXECUTEDEFAULT))
        explorer_status = GetLastError();
    desk.ipc_live = w_ipc_start(desk.channels);
    if (!desk.ipc_live)
        w_desktop_notice("Control pipe unavailable");
    w_desktop_load();
    if (!desk.loading)
        w_discover_initial();
    w_desktop_timer();
}

void w_desktop_timer(void) {
    KillTimer(desk.root, 1);
    bool clock = w_config_has_module(&desk.config, "clock");
    bool sample = w_config_has_module(&desk.config, "memory");
    if (!desk.hide_bars && (clock || sample)) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        UINT wait = sample ? 5000 : 60000 - t.wSecond * 1000 - t.wMilliseconds;
        SetTimer(desk.root, 1, wait, NULL);
    }
}

////////////////
//~ Session messages

void w_desktop_request(const char *s, char *reply, size_t cap) {
    if (!strcmp(s, "query")) {
        int live = 0;
        for (int i = 0; i < window_limit; i++)
            live += desk.model.windows[i].live;
        snprintf(
            reply, cap,
            "{\"version\":1,\"output\":%d,\"workspace\":%d,\"windows\":%d,\"focus\":%d,\"revision\":%llu}",
            desk.model.output, desk.model.selected[desk.model.output] + 1, live, desk.model.focus,
            (unsigned long long)desk.revision);
    } else if (!strncmp(s, "query provider ", 15)) {
        w_provider_value v;
        char action[128];
        if (!w_providers_value(s + 15, &v, action, sizeof(action)) || !w_os_utf8(v.text, reply, (int)cap))
            snprintf(reply, cap, "error: unknown provider");
    } else if (!strcmp(s, "doctor")) {
        int n = snprintf(
            reply, cap,
            "desktop protocol 1\nwindow events: %s\nfile browser: %s\ntask manager: %s\nlast message: %s\n",
            desk.ready ? "initialized" : "starting", desk.config.file_browser, desk.config.task_manager,
            desk.status);
        wchar_t policy[os_path_cap];
        DWORD bytes = sizeof(policy);
        LSTATUS policy_status =
            RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
                         L"Shell", RRF_RT_REG_SZ, NULL, policy, &bytes);
        HWND host = FindWindowW(L"WinwmHost", NULL);
        HANDLE exit_code = GetPropW(host, L"winwm.child_exit");
        HANDLE key_error = GetPropW(host, L"winwm.recovery_hotkey_error");
        if (key_error && n >= 0 && (size_t)n + 100 < cap)
            n += snprintf(reply + n, cap - n, "Ctrl+Alt+F12 unavailable (Win32 %lu). Use ctl run rescue.\n",
                          (DWORD)(uintptr_t)key_error);
        if (n >= 0 && (size_t)n + 200 < cap)
            n += snprintf(reply + n, cap - n, "user shell policy: %s\n",
                          policy_status == ERROR_SUCCESS ? "configured" : "absent or managed elsewhere");
        if (n >= 0 && (size_t)n + 120 < cap) {
            const char *state = preview_mode                      ? "preview"
                                : explorer_status == ERROR_IO_PENDING ? "pending"
                                : explorer_status == ERROR_NOT_FOUND  ? "no existing shell"
                                : explorer_status == ERROR_SUCCESS    ? "exited"
                                                                      : "shutdown declined or unavailable";
            n += snprintf(reply + n, cap - n, "Explorer handoff: %s (Win32 %lu)\n", state, explorer_status);
        }
        if (exit_code && n >= 0 && (size_t)n + 120 < cap)
            n += snprintf(reply + n, cap - n, "previous desktop exit: %lu\n",
                          (DWORD)((uintptr_t)exit_code - 1));
        for (int i = 0; i < desk.config.binding_count && n >= 0 && (size_t)n + 100 < cap; i++)
            if (!desk.registered[i])
                n += snprintf(reply + n, cap - (size_t)n, "unavailable binding: %s\n",
                              desk.config.bindings[i].chord);
    } else if (!strcmp(s, "reload")) {
        w_desktop_load();
        snprintf(reply, cap, "reload scheduled");
    } else if (!strncmp(s, "run ", 4)) {
        w_command c;
        bool ok = w_command_parse(s + 4, &c) && w_desktop_run(&c);
        snprintf(reply, cap, "%s", ok ? "accepted" : "error: invalid or unavailable command");
    } else
        snprintf(reply, cap, "error: unknown request");
}

static LRESULT CALLBACK w_desktop_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (shell_notification && msg == shell_notification) {
        if (wp == HSHELL_LANGUAGE)
            w_invalidate_language();
        return 0;
    }
    switch (msg) {
    case WM_INPUTLANGCHANGE:
        w_invalidate_language();
        return DefWindowProcW(hwnd, msg, wp, lp);
    case msg_desktop_menu: {
        POINT point = {(LONG)(LONG_PTR)wp, (LONG)lp};
        HMONITOR monitor = MonitorFromPoint(point, MONITOR_DEFAULTTONULL);
        for (int i = 0; i < output_limit; i++)
            if (desk.outputs[i].live && desk.outputs[i].monitor == monitor) {
                w_desktop_action(i, "launcher");
                break;
            }
        return 0;
    }
    case msg_hover: {
        POINT p;
        w_input_hover_point(&p);
        w_desktop_hover(p);
        return 0;
    }
    case msg_command:
        if ((DWORD)(uintptr_t)GetPropW(hwnd, L"winwm.binding_generation") == (DWORD)lp &&
            wp < (WPARAM)desk.config.binding_count)
            w_desktop_run(&desk.config.bindings[wp].action);
        return 0;
    case msg_shell_exit:
        explorer_status = (DWORD)wp;
        if (explorer_status == ERROR_SUCCESS)
            w_desktop_outputs();
        return 0;
    case msg_init:
        w_initialize();
        return 0;
    case msg_load: {
        w_config_result *r = (w_config_result *)lp;
        desk.loading = false;
        bool changed = r && r->ok && memcmp(&desk.config, &r->cfg, sizeof(desk.config)) != 0;
        if (r && r->ok) {
            if (!desk.ready || r->cfg.master_default != desk.config.master_default) {
                for (int o = 0; o < output_limit; o++)
                    for (int j = 0; j < workspace_limit; j++)
                        desk.model.spaces[o][j].layout = r->cfg.master_default ? layout_master : layout_tree;
            }
            desk.config = r->cfg;
            w_desktop_bind();
            w_desktop_outputs();
            w_desktop_timer();
            w_desktop_status_configure();
            w_providers_configure(desk.root, msg_devices, &desk.config);
            w_desktop_notice("Configuration loaded");
            w_desktop_start_programs(!loaded_programs);
            loaded_programs = true;
            for (int i = 0; i < output_limit; i++)
                if (desk.outputs[i].live)
                    InvalidateRect(desk.outputs[i].bar.hwnd, NULL, FALSE);
        } else
            w_desktop_notice(r ? r->error : "Configuration allocation failed");
        if (!desk.ready)
            w_discover_initial();
        if (changed) {
            r->revision = InterlockedIncrement(&config_revision);
            if (QueueUserWorkItem(w_save_config, r, WT_EXECUTEDEFAULT))
                return 0;
        }
        free(r);
        return 0;
    }
    case msg_audio: {
        if (wp) {
            w_bar_menu_audio_error();
            w_desktop_notice("Output device rejected the volume change");
            return 0;
        }
        w_audio_state state = w_audio_read();
        for (int i = 0; i < output_limit; ++i)
            if (desk.outputs[i].live)
                w_bar_set_audio(&desk.outputs[i].bar, state.available ? state.percent : -1, state.muted);
        return 0;
    }
    case msg_network:
        w_desktop_network_changed();
        return 0;
    case msg_sample:
        w_desktop_status_done((void *)lp);
        return 0;
    case msg_catalog:
        if (w_palette_visible(L"Run"))
            w_desktop_palette(cmd_launcher, desk.model.output);
        return 0;
    case msg_devices:
        w_providers_tick();
        for (int i = 0; i < output_limit; i++)
            if (desk.outputs[i].live)
                InvalidateRect(desk.outputs[i].bar.hwnd, NULL, FALSE);
        return 0;
    case msg_status:
        w_desktop_notice((char *)lp);
        free((void *)lp);
        return 0;
    case msg_window:
        pending_events--;
        w_desktop_event((HWND)lp, (DWORD)wp);
        if (wp == EVENT_SYSTEM_FOREGROUND)
            w_invalidate_language();
        if (!pending_events && event_overflow) {
            event_overflow = false;
            w_desktop_scan();
        }
        return 0;
    case msg_refresh:
        desk.refresh_pending = false;
        w_desktop_apply();
        return 0;
    case WM_DISPLAYCHANGE:
        if (desk.ready)
            w_desktop_outputs();
        return 0;
    case WM_TIMER:
        if (wp == 28) {
            w_desktop_status_sample();
            return 0;
        }
        if (wp == provider_timer) {
            PostMessageW(desk.root, msg_devices, 0, 0);
            return 0;
        }
        if (wp == 1) {
            for (int i = 0; i < output_limit; i++)
                if (desk.outputs[i].live && !desk.outputs[i].bar_hidden) {
                    w_bar_invalidate(&desk.outputs[i].bar, "clock");
                    w_bar_invalidate(&desk.outputs[i].bar, "memory");
                }
            w_desktop_timer();
        }
        return 0;
    case WM_TIMECHANGE:
    case WM_POWERBROADCAST:
        if (desk.ready) {
            w_desktop_timer();
            w_desktop_schedule();
            for (int i = 0; i < output_limit; i++)
                if (desk.outputs[i].live)
                    InvalidateRect(desk.outputs[i].bar.hwnd, NULL, FALSE);
        }
        return msg == WM_POWERBROADCAST ? TRUE : 0;
    case WM_CLIPBOARDUPDATE:
        w_desktop_copy_clip();
        return 0;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (wp)
            PostQuitMessage(0);
        return 0;
    case WM_CLOSE:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

int w_session_run(HINSTANCE instance, bool preview) {
    preview_mode = preview;
    DWORD host = w_os_instance_status(os_host_instance);
    if (host != ERROR_SUCCESS && host != ERROR_ALREADY_EXISTS)
        return (int)host;
    w_os_instance_lock lock;
    DWORD error = w_os_instance_claim(os_interactive_instance, &lock);
    if (error)
        return (int)error;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    w_os_mark(L"desktop_entry");
    w_model_init(&desk.model);
    w_config_defaults(&desk.config);
    w_os_cache_read(&desk.config);
    WNDCLASSW wc = {
        .hInstance = instance, .lpszClassName = L"WinwmEvents", .lpfnWndProc = w_desktop_proc};
    RegisterClassW(&wc);
    desk.root = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"winwm events", WS_POPUP, 0, 0, 0, 0,
                                NULL, NULL, instance, NULL);
    if (!desk.root) {
        w_os_instance_release(&lock);
        return 3;
    }
    w_desktop_bind();
    w_os_mark(L"bindings_registered");
    PostMessageW(desk.root, msg_init, 0, 0);
    int result = 0;
    for (;;) {
        HANDLE handles[ipc_slots];
        DWORD count = desk.ipc_live ? ipc_slots : 0;
        for (DWORD i = 0; i < count; i++)
            handles[i] = desk.channels[i].event;
        DWORD wait = MsgWaitForMultipleObjectsEx(count, handles, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (wait == WAIT_FAILED) {
            result = 4;
            break;
        }
        if (wait < count)
            w_ipc_pump(&desk.channels[wait], w_desktop_request);
        MSG message;
        for (int i = 0; i < 64 && PeekMessageW(&message, NULL, 0, 0, PM_REMOVE); i++) {
            if (message.message == WM_QUIT) {
                result = (int)message.wParam;
                goto done;
            }
            if (!w_palette_message(&message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
    }
done:
    for (size_t i = 0; i < count_of(desk.hooks); i++)
        if (desk.hooks[i])
            UnhookWinEvent(desk.hooks[i]);
    w_input_stop();
    DeregisterShellHookWindow(desk.root);
    w_desktop_status_stop();
    w_audio_stop();
    w_providers_stop();
    if (desk.ipc_live)
        w_ipc_close(desk.channels);
    w_os_area_restore();
    w_os_area_close();
    w_os_restore_all();
    w_os_instance_release(&lock);
    return result;
}
