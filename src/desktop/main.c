#include "theme/theme.h"
#include "desktop/entry.h"
#include "native_notifications/publish.h"
#include "native_notifications/listener.h"
#include "shell_host/entry.h"
#include "os_win32/environment.h"
#include "desktop/desktop.h"
#include "desktop/tray.h"
#include "tray/host.h"
#include "providers/audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

////////////////
//~ Desktop

w_desktop_state desk;
static bool event_overflow, logging_off, loaded_programs;
static bool shell_ready, config_ready, startup_config_loaded;
static int pending_events;
static UINT shell_notification, taskbar_created;
enum { discovery_timer = 29 };

////////////////
//~ Desktop Functions

static void w_invalidate_language(void) {
    for (int i = 0; i < output_limit; ++i)
        if (desk.outputs[i].live && !desk.outputs[i].bar_hidden)
            w_bar_invalidate(&desk.outputs[i].bar, "keyboard");
}

static void w_repaint_bars(void) {
    for (int i = 0; i < output_limit; ++i) {
        w_output_state *output = &desk.outputs[i];
        if (output->live && !output->bar_hidden && output->bar.hwnd)
            InvalidateRect(output->bar.hwnd, NULL, FALSE);
    }
}

typedef struct w_config_result {
    w_desktop_config cfg;
    LONG revision;
    char error[256];
    bool ok;
} w_config_result;

static SRWLOCK cache_lock = SRWLOCK_INIT;
static LONG config_revision;
enum reload_scope { reload_none, reload_bar, reload_full };
static enum reload_scope pending_reload;
static DWORD WINAPI w_save_config(void *ptr) {
    w_config_result *r = ptr;
    AcquireSRWLockExclusive(&cache_lock);
    if (r->revision == InterlockedCompareExchange(&config_revision, 0, 0))
        w_os_cache_write(&r->cfg);
    ReleaseSRWLockExclusive(&cache_lock);
    free(r);
    return 0;
}

static DWORD WINAPI w_load_thread(void *scope) {
    w_config_result *r = calloc(1, sizeof(*r));
    if (r)
        r->ok = w_os_config_load(&r->cfg, r->error, sizeof(r->error));
    if (!PostMessageW(desk.root, msg_load, (WPARAM)scope, (LPARAM)r))
        free(r);
    return 0;
}

static void w_reload(enum reload_scope scope) {
    if (desk.loading) {
        if (scope > pending_reload) pending_reload = scope;
        return;
    }
    desk.loading = QueueUserWorkItem(w_load_thread, (void *)(uintptr_t)scope, WT_EXECUTEDEFAULT) != 0;
}

void w_desktop_load(void) {
    w_reload(reload_full);
}

void w_desktop_schedule(void) {
    if (!desk.refresh_pending)
        desk.refresh_pending = PostMessageW(desk.root, msg_refresh, 0, 0) != 0;
}

void w_desktop_bind(void) {
    w_effects_preferences();
    w_theme_configure(&desk.config);
    w_theme_publish(desk.root);
    w_desktop_notifications_configure();
    w_input_bind(desk.root, msg_command, msg_hover, desk.preview ? 0 : msg_desktop_menu, msg_pointer_down,
               &desk.config, desk.registered, w_desktop_drag);
}

static void CALLBACK w_observe(HWINEVENTHOOK hook, DWORD event, HWND w, LONG obj, LONG child, DWORD thread,
                             DWORD time) {
    (void)hook;
    (void)thread;
    if (event == EVENT_SYSTEM_DESKTOPSWITCH) {
        w_input_reset();
        PostMessageW(desk.root, msg_window, event, 0);
        return;
    }
    if (!w || obj != OBJID_WINDOW || child != CHILDID_SELF)
        return;
    if (event >= EVENT_OBJECT_CREATE && event != EVENT_OBJECT_CREATE && event != EVENT_OBJECT_DESTROY &&
        event != EVENT_OBJECT_SHOW && event != EVENT_OBJECT_HIDE && event != EVENT_OBJECT_LOCATIONCHANGE &&
        event != EVENT_OBJECT_NAMECHANGE && event != EVENT_OBJECT_CLOAKED && event != EVENT_OBJECT_UNCLOAKED)
        return;
    if (pending_events >= 512) {
        event_overflow = true;
        return;
    }
    if (event == EVENT_SYSTEM_FOREGROUND && w_os_same_process(w)) {
        w_desktop_schedule();
        return;
    }
    WPARAM record = (WPARAM)event | ((WPARAM)time << 32);
    if (PostMessageW(desk.root, msg_window, record, (LPARAM)w))
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
                        {EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_NAMECHANGE},
                        {EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED}};
    _Static_assert(count_of(range) == count_of(desk.hooks), "window hook storage");
    for (size_t i = 0; i < count_of(range); i++) {
        // A shell activation initiated by Menu can foreground another process's
        // existing HWND without generating a show or location event.
        DWORD flags = WINEVENT_OUTOFCONTEXT;
        if (range[i][0] != EVENT_SYSTEM_FOREGROUND) flags |= WINEVENT_SKIPOWNPROCESS;
        desk.hooks[i] = SetWinEventHook(range[i][0], range[i][1], NULL, w_observe, 0, 0, flags);
        if (!desk.hooks[i]) {
            w_desktop_notice("Window event hook unavailable");
            SetTimer(desk.root, discovery_timer, 2000, NULL);
        }
    }
    w_os_restore_all();
    w_desktop_scan(false);
    w_os_remember_stack();
    desk.ready = true;
    for (int i = 0; i < command_count; i++)
        w_desktop_run_on(&pending_commands[i].action, pending_commands[i].output, false);
    command_count = 0;
}

static void w_finish_startup(void) {
    if (desk.ready || !shell_ready || !config_ready) return;
    // Recovery must own shell restoration before we manage windows or launch
    // any commands from the user's config. Input and config I/O can overlap it.
    w_desktop_status_configure();
    if (!desk.hide_bars) w_providers_configure(desk.root, msg_devices, &desk.config);
    if (startup_config_loaded) {
        w_desktop_start_programs(true);
        loaded_programs = true;
    }
    w_discover_initial();
    w_catalog_begin(desk.root,msg_catalog);
}

static void w_position_desktop(void) {
    if (desk.preview)
        ShowWindow(desk.root, SW_HIDE);
    else
        SetWindowPos(desk.root, HWND_BOTTOM, GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                     GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN),
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static void w_initialize(void) {
    if (!startup_config_loaded)
        w_desktop_load();
    w_position_desktop();
    shell_notification = RegisterWindowMessageW(L"SHELLHOOK");
    if (shell_notification)
        RegisterShellHookWindow(desk.root);
    w_desktop_outputs();
    wchar_t pipe_name[96];
    desk.ipc_live = w_ipc_name(pipe_name, count_of(pipe_name)) && w_ipc_start(desk.channels, pipe_name);
    if (!desk.ipc_live)
        w_desktop_notice("Control pipe unavailable");
    if (!desk.loading) {
        config_ready = true;
        w_finish_startup();
    }
    w_desktop_timer();
}

static bool w_startup_ui_ready(void) {
    for (int i = 0; i < desk.config.binding_count; i++)
        if (!desk.registered[i]) return false;
    for (int i = 0; i < output_limit; i++)
        if (desk.outputs[i].live && !desk.outputs[i].bar.hwnd) return false;
    return desk.outputs[desk.model.output].live;
}

void w_desktop_timer(void) {
    KillTimer(desk.root, 1);
    bool clock = w_config_has_module(&desk.config, "clock");
    if (!desk.hide_bars && clock) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        UINT wait = 60000 - t.wSecond * 1000 - t.wMilliseconds;
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
            "desktop protocol 1\nwindow events: %s\nfile browser: %s\ntask manager: %s\nlast message: %s\n"
            "animations: %s (%s)\nborder width: %d DIP\nnotification capture: %s\nnotification history: %d\n",
            desk.ready ? "initialized" : "starting", desk.config.file_browser, desk.config.task_manager,
            desk.status, desk.motion_allowed ? "on" : "off",
            desk.config.animations == motion_system ? "follow Windows" : "winwm preference",
            desk.borders_hidden ? 0 : desk.config.border_size, w_desktop_notification_status(), w_notice_count());
        DWORD shell_status = w_os_startup_shell();
        HWND host = FindWindowW(L"WinwmHost", NULL);
        HANDLE exit_code = GetPropW(host, L"winwm.child_exit");
        HANDLE key_error = GetPropW(host, L"winwm.recovery_hotkey_error");
        if (key_error && n >= 0 && (size_t)n + 100 < cap)
            n += snprintf(reply + n, cap - n, "Ctrl+Alt+F12 unavailable (Win32 %lu). Use winwm.exe rescue.\n",
                          (DWORD)(uintptr_t)key_error);
        if (n >= 0 && (size_t)n + 200 < cap)
            n += snprintf(reply + n, cap - n, "sign-in shell: %s\n",
                          !shell_status ? "Explorer" : shell_status == ERROR_ALREADY_ASSIGNED ? "custom" : "unknown");
        if (n >= 0 && (size_t)n + 100 < cap)
            n += snprintf(reply + n, cap - n, "Windows shell desktop: %s\n",GetShellWindow() ? "present" : "absent");
        if (n >= 0 && (size_t)n + 100 < cap)
            n += snprintf(reply + n, cap - n, "application tray: %s\nregistered tray icons: %zu\n",
                          w_tray_is_active() ? "winwm" : desk.preview ? "Explorer preview" : "external or unavailable",
                          w_tray_count());
        if (exit_code && n >= 0 && (size_t)n + 120 < cap)
            n += snprintf(reply + n, cap - n, "previous desktop exit: %lu\n",
                          (DWORD)((uintptr_t)exit_code - 1));
        for (int i = 0; i < desk.config.binding_count && n >= 0 && (size_t)n + 100 < cap; i++)
            if (!desk.registered[i])
                n += snprintf(reply + n, cap - (size_t)n, "unavailable binding: %s\n",
                              desk.config.bindings[i].chord);
    } else if (!strcmp(s, "reload-bar")) {
        w_reload(reload_bar);
        snprintf(reply, cap, "toolbar reload scheduled");
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
    if (taskbar_created && msg == taskbar_created) {
        // A replacement tray also broadcasts TaskbarCreated so apps republish their icons.
        if (!desk.preview && !w_os_explorer_tray_present()) return 0;
        PostMessageW(hwnd, msg_shell_started, 0, 0);
        w_position_desktop();
        if (desk.root) w_desktop_outputs();
        return 0;
    }
    if (shell_notification && msg == shell_notification) {
        if (wp == HSHELL_LANGUAGE)
            w_invalidate_language();
        else if (desk.ready && wp == HSHELL_WINDOWCREATED)
            w_desktop_event((HWND)lp, EVENT_OBJECT_SHOW, 0);
        return 0;
    }
    switch (msg) {
    case WM_ACTIVATE:
        if (desk.ready && LOWORD(wp) != WA_INACTIVE)
            w_desktop_focus_observe(hwnd);
        return DefWindowProcW(hwnd, msg, wp, lp);
    case WM_WINDOWPOSCHANGING: {
        WINDOWPOS *pos = (WINDOWPOS *)lp;
        if (!desk.preview && !(pos->flags & SWP_NOZORDER))
            pos->hwndInsertAfter = HWND_BOTTOM;
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(hwnd, &paint);
        if (!PaintDesktop(dc))
            FillRect(dc, &paint.rcPaint, GetSysColorBrush(COLOR_DESKTOP));
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wp & 0xfff0) == SC_CLOSE)
            return 0;
        return DefWindowProcW(hwnd, msg, wp, lp);
    case WM_INPUT:
        w_input_mouse_motion();
        return DefWindowProcW(hwnd, msg, wp, lp);
    case WM_INPUTLANGCHANGE:
        w_invalidate_language();
        return DefWindowProcW(hwnd, msg, wp, lp);
    case msg_desktop_menu: {
        POINT point = {(LONG)(LONG_PTR)wp, (LONG)lp};
        HMONITOR monitor = MonitorFromPoint(point, MONITOR_DEFAULTTONULL);
        for (int i = 0; i < output_limit; i++)
            if (desk.outputs[i].live && desk.outputs[i].monitor == monitor) {
                w_desktop_action(i, "menu");
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
    case msg_pointer_down: {
        HWND hit = WindowFromPoint((POINT){(LONG)(LONG_PTR)wp, (LONG)lp});
        w_notice_outside(hit);
        w_bar_menu_outside(hit);
        w_palette_outside(hit);
        w_desktop_popup_outside(hit);
        return 0;
    }
    case winwm_popup_released: {
        HANDLE popup = w_desktop_popup_process();
        if (popup && GetProcessId(popup) == (DWORD)wp)
            w_desktop_popup_finished();
        return 0;
    }
    case msg_pointer_select: {
        w_window_id id = {(int)wp, (uint32_t)lp};
        if (w_model_valid(&desk.model, id) && w_model_visible(&desk.model, id.slot))
            w_desktop_activate(id.slot);
        return 0;
    }
    case msg_drag_preview:
        w_desktop_drag_preview();
        return 0;
    case WM_CANCELMODE:
        w_input_cancel_pointer();
        return 0;
    case msg_drag_limits:
        w_desktop_drag_limits((w_window_id){(int)wp, (uint32_t)lp});
        return 0;
    case msg_command:
        if ((DWORD)(uintptr_t)GetPropW(hwnd, L"winwm.binding_generation") == (DWORD)lp &&
            wp < (WPARAM)desk.config.binding_count)
            w_desktop_run(&desk.config.bindings[wp].action);
        return 0;
    case msg_init:
        w_initialize();
        return 0;
    case msg_tray_changed:
        w_tray_ack();
        w_desktop_tray_balloons();
        w_desktop_tray_update();
        return 0;
    case msg_tray_refresh:
        w_desktop_tray_update();
        return 0;
    case msg_load: {
        w_config_result *r = (w_config_result *)lp;
        desk.loading = false;
        if (pending_reload) {
            enum reload_scope next = pending_reload;
            pending_reload = reload_none;
            w_reload(next);
        }
        bool layout_only = wp == reload_bar && desk.ready;
        if (layout_only && r && r->ok) {
            // Adopt the modules and their settings without running desktop startup programs again.
            strcpy(desk.config.bar_left, r->cfg.bar_left);
            strcpy(desk.config.bar_center, r->cfg.bar_center);
            strcpy(desk.config.bar_right, r->cfg.bar_right);
            desk.config.style_count = r->cfg.style_count;
            memcpy(desk.config.styles, r->cfg.styles, sizeof(desk.config.styles));
            desk.config.provider_count = r->cfg.provider_count;
            memcpy(desk.config.providers, r->cfg.providers, sizeof(desk.config.providers));
            r->cfg = desk.config;
        }
        bool changed = r && r->ok && memcmp(&desk.config, &r->cfg, sizeof(desk.config)) != 0;
        if (r && r->ok) {
            if (!desk.ready || r->cfg.master_default != desk.config.master_default) {
                for (int o = 0; o < output_limit; o++)
                    for (int j = 0; j < workspace_limit; j++)
                        desk.model.spaces[o][j].layout = r->cfg.master_default ? layout_master : layout_tree;
            }
            desk.config = r->cfg;
            if (!layout_only && (desk.ready || changed || !w_startup_ui_ready())) {
                w_desktop_bind();
                w_desktop_outputs();
            }
            w_desktop_timer();
            if (desk.ready) w_desktop_notice("Configuration loaded");
            else w_str_copy(desk.status,sizeof(desk.status),"Configuration loaded");
            if (desk.ready) {
                w_desktop_status_configure();
                if (!desk.hide_bars) w_providers_configure(desk.root, msg_devices, &desk.config);
                if (!layout_only) {
                    w_desktop_start_programs(!loaded_programs);
                    loaded_programs = true;
                }
            } else startup_config_loaded = true;
            w_repaint_bars();
        } else
            w_desktop_notify(L"Configuration error", r ? r->error : "Configuration allocation failed");
        config_ready = true;
        w_finish_startup();
        if (changed || (layout_only && r && r->ok)) {
            r->revision = InterlockedIncrement(&config_revision);
            if (QueueUserWorkItem(w_save_config, r, WT_EXECUTEDEFAULT))
                return 0;
        }
        free(r);
        return 0;
    }
    case msg_notifications:
        w_desktop_notifications_receive(lp);
        return 0;
    case msg_notification_published:
        w_desktop_notification_published(lp);
        return 0;
    case msg_notification_banner:
        w_desktop_banner_receive(wp,lp);
        return 0;
    case msg_notification_open:
        w_desktop_run(&(w_command){.kind=cmd_notifications});
        return 0;
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
        if (lp) {
            w_catalog_shell_event(wp,lp);
            return 0;
        }
        if (wp) w_desktop_notice(wp == 2 ? "This application could not start as administrator." :
                                           "The selected application could not start.");
        else w_desktop_catalog_update();
        return 0;
    case msg_run_result:
        w_palette_line_result((UINT)wp, (DWORD)lp);
        return 0;
    case msg_devices:
        if (w_providers_tick()) w_repaint_bars();
        return 0;
    case msg_status:
        w_desktop_notice((char *)lp);
        free((void *)lp);
        return 0;
    case msg_window:
        if ((DWORD)wp == EVENT_SYSTEM_DESKTOPSWITCH) {
            w_desktop_scan(false);
            return 0;
        }
        pending_events--;
        w_desktop_event((HWND)lp, (DWORD)wp, (DWORD)(wp >> 32));
        if ((DWORD)wp == EVENT_SYSTEM_FOREGROUND)
            w_invalidate_language();
        if (!pending_events && event_overflow) {
            event_overflow = false;
            w_desktop_scan(true);
        }
        return 0;
    case msg_titles:
        w_desktop_titles();
        return 0;
    case msg_refresh:
        desk.refresh_pending = false;
        w_desktop_apply();
        return 0;
    case WM_DISPLAYCHANGE:
        w_desktop_popup_cancel();
        w_position_desktop();
        if (desk.root)
            w_desktop_outputs();
        return 0;
    case WM_SETTINGCHANGE:
        if (lp && !_wcsicmp((const wchar_t *)lp,L"Environment")) {
            w_environment_refresh();
            return 0;
        }
        if (desk.root && wp == SPI_SETWORKAREA)
            w_desktop_outputs();
        w_notification_refresh();
        InvalidateRect(hwnd, NULL, FALSE);
        w_effects_preferences();
        w_theme_configure(&desk.config);
        w_desktop_notifications_configure();
        w_desktop_schedule();
        return 0;
    case WM_SYSCOLORCHANGE:
    case WM_THEMECHANGED:
        InvalidateRect(hwnd, NULL, FALSE);
        return DefWindowProcW(hwnd, msg, wp, lp);
    case winwm_packages_changed:
        w_environment_refresh();
        w_catalog_invalidate();
        return 0;
    case msg_environment:
        if (wp) w_catalog_invalidate();
        return 0;
    case WM_TIMER:
        if (wp == animation_timer) {
            w_effects_tick();
            return 0;
        }
        if (wp == discovery_timer) {
            w_desktop_scan(true);
            return 0;
        }
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
                }
            w_desktop_timer();
        }
        return 0;
    case WM_TIMECHANGE:
    case WM_POWERBROADCAST:
        if (msg == WM_POWERBROADCAST && (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND))
            w_notification_refresh();
        if (desk.ready) {
            w_desktop_timer();
            w_desktop_schedule();
            w_repaint_bars();
        }
        return msg == WM_POWERBROADCAST ? TRUE : 0;
    case WM_CLIPBOARDUPDATE:
        w_desktop_copy_clip();
        return 0;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (wp) {
            logging_off = true;
            PostQuitMessage(0);
        }
        return 0;
    case WM_CLOSE:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

typedef struct w_shell_handoff {
    HANDLE recovery;
    HWND host;
} w_shell_handoff;

static DWORD WINAPI w_take_shell(void *arg) {
    const w_shell_handoff *handoff = arg;
    return w_recovery_take_desktop(handoff->recovery, handoff->host);
}

int w_session_run(HINSTANCE instance, bool preview) {
    desk.preview = preview;
    DWORD host = w_os_instance_status(os_host_instance);
    if (host != ERROR_SUCCESS)
        return (int)host;
    w_os_instance_lock lock;
    DWORD error = w_os_instance_claim(os_interactive_instance, &lock);
    if (error)
        return (int)error;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    w_config_defaults(&desk.config);
    if (!w_os_cache_read(&desk.config)) {
        char detail[256] = {0};
        if (!w_os_config_load(&desk.config, detail, sizeof(detail))) {
            wchar_t message[256] = L"Could not read winwm.lua.";
            w_os_wide(detail, message, count_of(message));
            w_os_instance_release(&lock);
            if (!preview && !GetShellWindow()) w_os_ensure_shell();
            if (!w_notification_publish(L"Configuration error", message))
                MessageBoxW(NULL, message, L"winwm configuration error", MB_OK | MB_ICONERROR);
            return ERROR_INVALID_DATA;
        }
        startup_config_loaded = true;
    }
    HWND host_window = NULL;
    HANDLE recovery = w_recovery_attach(preview, &host_window);
    if (!recovery) {
        error = GetLastError();
        if (!preview && !GetShellWindow()) w_os_ensure_shell();
        w_os_instance_release(&lock);
        MessageBoxW(NULL,L"Could not start winwm. Place the matching winwm_recovery.exe beside winwm.exe.",
                    L"winwm recovery unavailable",MB_OK|MB_ICONERROR);
        return error ? (int)error : ERROR_PROCESS_ABORTED;
    }
    w_os_mark(L"desktop_entry");
    w_model_init(&desk.model);
    for (int o = 0; o < output_limit; ++o)
        for (int j = 0; j < workspace_limit; ++j)
            desk.model.spaces[o][j].layout = desk.config.master_default ? layout_master : layout_tree;
    if (startup_config_loaded) {
        w_config_result *initial = malloc(sizeof(*initial));
        if (initial) {
            initial->cfg = desk.config;
            initial->revision = InterlockedIncrement(&config_revision);
            if (!QueueUserWorkItem(w_save_config, initial, WT_EXECUTEDEFAULT))
                free(initial);
        }
    }
    WNDCLASSW wc = {
        .hInstance = instance, .lpszClassName = L"WinwmEvents", .lpfnWndProc = w_desktop_proc,
        .hCursor = LoadCursorW(NULL, IDC_ARROW)};
    RegisterClassW(&wc);
    taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    desk.root = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"winwm events", WS_POPUP, 0, 0, 0, 0,
                                NULL, NULL, instance, NULL);
    if (!desk.root) {
        CloseHandle(recovery);
        w_os_instance_release(&lock);
        return 3;
    }
    w_shell_handoff handoff = {recovery,host_window};
    HANDLE shell_thread = NULL;
    bool waiting_for_shell = false, tray_seen = false;
    shell_ready = preview;
    if (!preview) {
        shell_thread = CreateThread(NULL,0,w_take_shell,&handoff,0,NULL);
        if (!shell_thread) {
            error = GetLastError();
            DestroyWindow(desk.root);
            CloseHandle(recovery);
            w_os_instance_release(&lock);
            return (int)error;
        }
    }
    w_environment_start(desk.root,msg_environment);
    w_desktop_bind();
    w_os_mark(L"bindings_registered");
    PostMessageW(desk.root, msg_init, 0, 0);
    int result = 0;
    for (;;) {
        HANDLE handles[2*ipc_slots+4];
        DWORD count = desk.ipc_live ? 2*ipc_slots : 0;
        for (DWORD i = 0; i < count / 2; i++) {
            handles[2*i] = desk.channels[i].peer_event;
            handles[2*i+1] = desk.channels[i].event;
        }
        DWORD total = count;
        handles[total++] = recovery;
        HANDLE popup = w_desktop_popup_process();
        if (popup) handles[total++] = popup;
        DWORD clipboard_index = total;
        if (desk.clipboard_reader) handles[total++] = desk.clipboard_reader;
        if (shell_thread) handles[total++] = shell_thread;
        DWORD wait = MsgWaitForMultipleObjectsEx(total, handles, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (wait == WAIT_OBJECT_0+count) {
            if (shell_thread) result = ERROR_PROCESS_ABORTED;
            break;
        }
        if (popup && wait == WAIT_OBJECT_0+count+1) w_desktop_popup_finished();
        if (desk.clipboard_reader && wait == WAIT_OBJECT_0+clipboard_index)
            w_desktop_clipboard_done();
        if (shell_thread && wait == WAIT_OBJECT_0+total-1) {
            if (!GetExitCodeThread(shell_thread,&error)) error = GetLastError();
            CloseHandle(shell_thread);
            shell_thread = NULL;
            waiting_for_shell = error == ERROR_NOT_FOUND;
            if (error && !waiting_for_shell) {
                result = (int)error;
                break;
            }
            shell_ready = true;
            w_tray_start(desk.root,msg_tray_changed,w_desktop_tray_locate);
            w_finish_startup();
            if (waiting_for_shell && tray_seen) PostMessageW(desk.root,msg_shell_started,0,0);
            tray_seen = false;
        }
        if (wait == WAIT_FAILED) {
            result = 4;
            break;
        }
        if (wait < count) {
            if (wait % 2)
                w_ipc_pump(desk.channels, (int)(wait/2), w_desktop_request);
            else
                w_ipc_disconnect(&desk.channels[wait/2]);
        }
        MSG message;
        for (int i = 0; i < 64 && PeekMessageW(&message, NULL, 0, 0, PM_REMOVE); i++) {
            if (message.hwnd == desk.root && message.message == msg_shell_started) {
                // At sign-in Explorer may create its first taskbar after winwm starts.
                if (shell_thread) tray_seen = true;
                else if (waiting_for_shell && !logging_off) {
                    w_tray_stop();
                    w_desktop_tray_update();
                    shell_thread = CreateThread(NULL,0,w_take_shell,&handoff,0,NULL);
                    if (!shell_thread) {
                        result = (int)GetLastError();
                        goto done;
                    }
                } else if (!preview && !logging_off && w_tray_is_active() && w_os_explorer_tray_present()) {
                    // Explorer was explicitly restored after startup. Give its tray the registrations.
                    w_tray_stop();
                    w_desktop_tray_update();
                    SendNotifyMessageW(HWND_BROADCAST,taskbar_created,0,0);
                }
                continue;
            }
            if (message.message == WM_QUIT) {
                result = (int)message.wParam;
                goto done;
            }
            if (!w_notice_message(&message) && !w_palette_message(&message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
    }
done:
    w_desktop_notifications_stop();
    w_tray_stop();
    w_catalog_end();
    if (desk.clipboard_enabled) RemoveClipboardFormatListener(desk.root);
    if (desk.clipboard_reader) CloseHandle(desk.clipboard_reader);
    if (shell_thread) {
        WaitForSingleObject(shell_thread,INFINITE);
        CloseHandle(shell_thread);
    }
    w_desktop_popup_cancel();
    if (w_desktop_popup_process()) w_desktop_popup_finished();
    if ((DWORD)(uintptr_t)GetPropW(host_window,L"winwm.session_pid") == GetCurrentProcessId())
        PostMessageW(host_window,recovery_session_closing,GetCurrentProcessId(),result);
    KillTimer(desk.root, animation_timer);
    for (int o = 0; o < output_limit; ++o)
        if (desk.outputs[o].tabs)
            DestroyWindow(desk.outputs[o].tabs);
    for (int i = 0; i < window_limit; i++)
        w_effects_hide(i);
    for (size_t i = 0; i < count_of(desk.hooks); i++)
        if (desk.hooks[i])
            UnhookWinEvent(desk.hooks[i]);
    w_desktop_tray_stop();
    w_input_stop();
    DeregisterShellHookWindow(desk.root);
    w_desktop_status_stop();
    w_audio_stop();
    w_providers_stop();
    if (desk.ipc_live)
        w_ipc_close(desk.channels);
    w_os_restore_all();
    if (!preview && !logging_off && WaitForSingleObject(recovery, 0) == WAIT_OBJECT_0)
        w_os_ensure_shell();
    CloseHandle(recovery);
    w_os_instance_release(&lock);
    w_environment_stop();
    return result;
}
