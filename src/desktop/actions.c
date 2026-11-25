#include "desktop/desktop.h"
#include "providers/audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <shlobj.h>

////////////////
//~ Actions

enum launch_kind { launch_executable, launch_terminal, launch_command, launch_task_manager, launch_panel, launch_place, launch_window_capture };
typedef struct w_launch_job {
    char exe[1024], arg[4096];
    enum launch_kind kind;
    bool admin;
    POINT origin;
} w_launch_job;

////////////////
//~ Actions Functions

static bool w_open_place(const char *browser, const char *place, bool admin) {
    wchar_t config[os_path_cap];
    PWSTR known = NULL;
    const wchar_t *path;
    if (!strcmp(place, "config")) {
        if (!w_os_config_path(config, count_of(config))) return false;
        wchar_t *end = wcsrchr(config, L'\\');
        if (!end) return false;
        *end = 0;
        path = config;
    } else {
        const KNOWNFOLDERID *id = !strcmp(place, "desktop") ? &FOLDERID_Desktop :
            !strcmp(place, "downloads") ? &FOLDERID_Downloads : !strcmp(place, "documents") ? &FOLDERID_Documents :
            !strcmp(place, "pictures") ? &FOLDERID_Pictures : &FOLDERID_Videos;
        if (FAILED(SHGetKnownFolderPath(id, 0, NULL, &known))) return false;
        path = known;
    }
    wchar_t exe[os_path_cap];
    bool launched = w_os_provider(browser, exe, count_of(exe)) && w_os_run_executable(exe, path, NULL, admin);
    CoTaskMemFree(known);
    return launched;
}

static DWORD WINAPI w_launch_thread(void *ptr) {
    w_launch_job *j = ptr;
    bool launched;
    SetLastError(ERROR_SUCCESS);
    switch (j->kind) {
    case launch_place:
        launched = w_open_place(j->exe, j->arg, j->admin);
        break;
    case launch_window_capture:
    case launch_panel: {
        wchar_t self[os_path_cap], mode[96];
        if (!w_os_self(self, count_of(self)) || !w_os_wide(j->exe, mode, count_of(mode))) {
            launched = false;
            break;
        }
        if (j->admin) {
            launched = w_os_shell_execute(self, mode, NULL, true);
            break;
        }
        HANDLE child = w_os_launch_positioned(self, mode, j->origin);
        launched = child != NULL;
        if (child && j->kind == launch_window_capture) {
            DWORD code;
            if (WaitForSingleObject(child,10000) != WAIT_OBJECT_0) {
                // A hung application can block PrintWindow inside the isolated helper.
                TerminateProcess(child,1);
                launched = false;
            } else launched = GetExitCodeProcess(child,&code) && !code;
        }
        if (child) CloseHandle(child);
        break;
    }
    case launch_command:
        launched = w_os_launch_line(j->exe, j->admin, true);
        break;
    case launch_terminal:
        launched = w_os_launch_terminal(j->exe, j->admin);
        break;
    case launch_task_manager: {
        wchar_t path[os_path_cap];
        launched = w_os_task_manager(j->exe,path,count_of(path)) && w_os_run_executable(path,NULL,NULL,j->admin);
        break;
    }
    default:
        launched = w_os_launch_provider(j->exe,j->arg,j->admin);
        break;
    }
    if ((!launched && !(j->admin && GetLastError() == ERROR_CANCELLED)) || j->kind == launch_window_capture) {
        char *why = malloc(512);
        if (why) {
            if (j->kind == launch_window_capture)
                w_str_copy(why,512,launched ? "Window capture copied to clipboard." : "Could not capture the selected window.");
            else if (j->kind == launch_command && !GetShellWindow() &&
                     (!_strnicmp(j->exe, "ms-settings:", 12) || !_strnicmp(j->exe, "windowsdefender:", 16)))
                w_str_copy(why, 512, "Windows could not open this panel. Exit winwm and run winwm.exe --preview to use it with Explorer.");
            else snprintf(why, 512, "Could not open %.380s.", j->exe);
            if (!PostMessageW(desk.root, msg_status, 0, (LPARAM)why))
                free(why);
        }
    }
    free(j);
    return 0;
}

static bool w_queue_launch(const char *exe, const char *arg, enum launch_kind kind, const POINT *origin, bool admin) {
    w_launch_job *j = calloc(1, sizeof(*j));
    if (!j)
        return false;
    j->kind = kind;
    j->admin = admin;
    if (origin) j->origin = *origin;
    if (!w_str_copy(j->exe, sizeof(j->exe), exe) || !w_str_copy(j->arg, sizeof(j->arg), arg ? arg : "") ||
        !QueueUserWorkItem(w_launch_thread, j, WT_EXECUTEDEFAULT)) {
        free(j);
        return false;
    }
    return true;
}

bool w_desktop_capture_window(HWND target, DWORD pid) {
    DWORD current = 0;
    GetWindowThreadProcessId(target,&current);
    if (!pid || current != pid || target == GetDesktopWindow() || target == GetShellWindow() ||
        !IsWindowVisible(target) || IsIconic(target)) {
        w_desktop_notice("No visible application window to capture.");
        return false;
    }
    char mode[96];
    snprintf(mode,sizeof(mode),"--tool-capture_window-%llx-%lu",(unsigned long long)(uintptr_t)target,pid);
    return w_queue_launch(mode,NULL,launch_window_capture,NULL,false);
}

static DWORD WINAPI w_start_configured_programs(void *ptr) {
    w_startup_entry *entries = ptr;
    for (int i = 0; entries[i].command_line[0]; i++) {
        if (w_os_launch_line(entries[i].command_line, false, false))
            continue;
        char *message = malloc(512);
        if (message) {
            snprintf(message, 512, "Could not start %.380s", entries[i].command_line);
            if (!PostMessageW(desk.root, msg_status, 0, (LPARAM)message))
                free(message);
        }
    }
    free(entries);
    return 0;
}

void w_desktop_start_programs(bool first_load) {
    if (!desk.config.startup_count)
        return;
    w_startup_entry *entries = calloc(desk.config.startup_count + 1, sizeof(*entries));
    if (!entries)
        return;
    int count = 0;
    for (int i = 0; i < desk.config.startup_count; i++) {
        const w_startup_entry *entry = &desk.config.startup[i];
        if (first_load || !entry->once)
            entries[count++] = *entry;
    }
    if (!count || !QueueUserWorkItem(w_start_configured_programs, entries, WT_EXECUTEDEFAULT))
        free(entries);
}

static bool w_launch(const char *exe, const char *arg, bool admin) {
    return w_queue_launch(exe, arg, launch_executable, NULL, admin);
}

////////////////
//~ Desktop commands

void w_desktop_action(int output, const char *s) {
    w_command c;
    if (w_command_parse(s, &c))
        w_desktop_run_on(&c, output, false);
}

void w_desktop_bar_action(int output, const char *action, const w_popup_anchor *anchor) {
    w_command command;
    if (action && w_command_parse(action, &command)) {
        switch (command.kind) {
        case cmd_menu: case cmd_switcher: case cmd_shortcuts: case cmd_settings:
        case cmd_clipboard: case cmd_notifications:
            break; // The palette handles same-button toggling and submenu transitions.
        default:
            w_palette_outside(NULL);
            break;
        }
        w_desktop_run_anchored(&command, output, false, anchor);
    } else {
        w_desktop_popup_cancel();
        w_palette_outside(NULL);
    }
}

static bool w_open_files(const w_command *c, bool admin) {
    char utf8[4096];
    wchar_t path[os_path_cap];
    if (*c->arg) {
        if (!w_os_wide(c->arg, path, count_of(path)))
            return false;
    } else {
        DWORD n = GetEnvironmentVariableW(L"USERPROFILE", path, count_of(path));
        if (!n || n >= count_of(path))
            return false;
    }
    if (c->kind == cmd_reveal_file) {
        wchar_t full[os_path_cap];
        DWORD n = GetFullPathNameW(path, count_of(full), full, NULL);
        if (!n || n >= count_of(full))
            return false;
        wchar_t *slash = wcsrchr(full, L'\\');
        if (!slash)
            return false;
        if (slash == full + 2 && full[1] == L':')
            slash[1] = 0;
        else
            *slash = 0;
        wcscpy(path, full);
        w_desktop_notice("Opened containing directory; exact selection is unavailable.");
    }
    wchar_t canonical[os_path_cap];
    DWORD n = GetFullPathNameW(path, count_of(canonical), canonical, NULL);
    return n && n < count_of(canonical) && w_os_utf8(canonical, utf8, sizeof(utf8)) &&
           w_launch(desk.config.file_browser, utf8, admin);
}

static bool w_panel_command(enum command_kind kind) {
    switch (kind) {
    case cmd_activity:
    case cmd_packages:
    case cmd_chocolatey:
    case cmd_updates:
    case cmd_google_japanese_input:
    case cmd_input_methods:
    case cmd_bar_settings:
    case cmd_microphone:
    case cmd_audio:
    case cmd_project:
    case cmd_pick_color:
    case cmd_languages:
    case cmd_keyboard_settings:
    case cmd_volume_mixer:
    case cmd_power_settings:
    case cmd_date_time:
    case cmd_network_settings:
    case cmd_wifi:
    case cmd_bluetooth:
    case cmd_display:
    case cmd_wallpaper:
    case cmd_sleep:
    case cmd_restart:
    case cmd_capture_region:
    case cmd_record:
        return true;
    default: return false;
    }
}

bool w_desktop_can_run_admin(enum command_kind kind) {
    switch (kind) {
    case cmd_launch: case cmd_program: case cmd_terminal: case cmd_open_tasks:
    case cmd_open_folder: case cmd_reveal_file: case cmd_open_place: case cmd_edit_config:
        return true;
    default:
        return w_panel_command(kind);
    }
}

bool w_desktop_run(const w_command *c) {
    return w_desktop_run_on(c, -1, false);
}

bool w_desktop_run_on(const w_command *c, int output, bool admin) {
    return w_desktop_run_anchored(c, output, admin, NULL);
}

bool w_desktop_run_anchored(const w_command *c, int output, bool admin, const w_popup_anchor *anchor) {
    if (admin && !w_desktop_can_run_admin(c->kind))
        return false;
    w_desktop_model *m = &desk.model;
    int i = m->focus, o = m->output, ws = m->selected[o];
    w_model_window *w = i >= 0 ? &m->windows[i] : NULL;
    w_workspace *space = &m->spaces[o][ws];
    bool ok = true;
    if (!desk.first_input) {
        desk.first_input = true;
        w_os_mark(L"first_command_dispatch");
    }
    if (!desk.ready && c->kind >= cmd_focus && c->kind <= cmd_close)
        return w_desktop_defer(c, output);
    bool explicit_output = (unsigned)output < output_limit && desk.outputs[output].live;
    if (!explicit_output)
        output = o;
    int destination = -1;
    if (c->kind == cmd_workspace_select || c->kind == cmd_workspace_send || c->kind == cmd_workspace_follow) {
        if (c->arg[0] >= '0' && c->arg[0] <= '9')
            destination = atoi(c->arg) - 1;
        else
            for (int k = 0; k < workspace_limit; k++)
                if (!strcmp(desk.config.workspace_names[k], c->arg))
                    destination = k;
        if (destination < 0 || destination >= workspace_limit)
            return false;
    }
    bool popup = anchor && !admin && w_panel_command(c->kind) &&
                 c->kind != cmd_capture_region && c->kind != cmd_record && c->kind != cmd_pick_color;
    if (c->kind != cmd_close && c->kind != cmd_notifications && c->kind != cmd_notify)
        w_notice_close();
    if (!popup && (anchor || c->kind == cmd_workspace_select || c->kind == cmd_workspace_back ||
                   c->kind == cmd_output_next || c->kind == cmd_menu || c->kind == cmd_notifications))
        w_desktop_popup_cancel();
    if (popup) {
        w_palette_close();
        w_desktop_focus_cancel();
        ok = w_desktop_popup_toggle(anchor, c->kind);
    } else if (w_panel_command(c->kind)) {
        char mode[64];
        snprintf(mode, sizeof(mode), "--tool-%s", w_command_name(c->kind));
        w_rect work = desk.outputs[output].work;
        POINT origin = {work.x + work.w/2, work.y + work.h/2};
        ok = w_queue_launch(mode, NULL, launch_panel, &origin, admin);
    } else switch (c->kind) {
    case cmd_open_place:
        ok = w_queue_launch(desk.config.file_browser, c->arg, launch_place, NULL, admin);
        break;
    case cmd_terminal:
        ok = w_queue_launch(desk.config.terminal, NULL, launch_terminal, NULL, admin);
        break;
    case cmd_open_tasks:
        ok = w_queue_launch(desk.config.task_manager, NULL, launch_task_manager, NULL, admin);
        break;
    case cmd_open_folder:
    case cmd_reveal_file:
        ok = w_open_files(c, admin);
        break;
    case cmd_launch: {
        const char *configured = NULL;
        for (int j = 0; j < desk.config.app_count; j++)
            if (!strcmp(c->arg, desk.config.apps[j].name)) {
                configured = desk.config.apps[j].path;
                break;
            }
        ok = configured ? w_launch(configured,"",admin) : w_catalog_launch(c->arg,admin) || w_launch(c->arg,"",admin);
        break;
    }
    case cmd_capture_window: {
        HWND target = GetForegroundWindow();
        DWORD pid = 0;
        GetWindowThreadProcessId(target,&pid);
        ok = w_desktop_capture_window(target,pid);
        break;
    }
    case cmd_edit_config: {
        wchar_t path[os_path_cap];
        char s[4096];
        ok = w_os_config_path(path, count_of(path)) && w_os_utf8(path, s, sizeof(s)) &&
             w_launch(desk.config.editor, s, admin);
        break;
    }
    case cmd_notifications:
        w_desktop_focus_cancel();
        w_bar_menu_close(NULL,true);
        w_palette_close();
        if (desk.resize) { desk.resize = false; w_input_mode(false); }
        w_desktop_notifications_open(output,anchor);
        break;
    case cmd_menu:
    case cmd_switcher:
    case cmd_shortcuts:
    case cmd_settings:
    case cmd_clipboard:
        w_desktop_focus_cancel();
        if (desk.resize) {
            desk.resize = false;
            w_input_mode(false);
        }
        if (anchor)
            w_desktop_bar_palette(c->kind, output, anchor);
        else if (c->kind == cmd_menu && w_palette_visible(L"Menu"))
            w_desktop_focus_menu(explicit_output ? output : -1);
        else
            w_desktop_palette(c->kind, output);
        break;
    case cmd_set_mode:
        desk.resize = !strcmp(c->arg, "resize");
        w_input_mode(desk.resize);
        break;
    case cmd_resize_by: {
        int dx, dy;
        if (w_command_delta(c->arg, &dx, &dy)) {
            w_desktop_resize_axis(split_x, dx);
            w_desktop_resize_axis(split_y, dy);
        }
        break;
    }
    case cmd_program:
        ok = w_queue_launch(c->arg, "", launch_command, NULL, admin);
        break;
    case cmd_resize_mode:
        desk.resize = !desk.resize;
        w_input_mode(desk.resize);
        break;
    case cmd_focus:
    case cmd_move: {
        int d = w_command_direction(c->arg);
        if (desk.resize) {
            w_desktop_resize_direction(d);
            break;
        }
        w_desktop_navigate((enum direction)d, c->kind == cmd_move);
        break;
    }
    case cmd_workspace_select:
    case cmd_workspace_back:
        w_os_mark(L"workspace_requested");
        if (m->output != output) {
            m->output = output;
            m->focus = w_model_recent(m, output, m->selected[output]);
        }
        w_model_workspace(m, c->kind == cmd_workspace_back ? m->previous[m->output] : destination);
        w_desktop_select(w_model_recent(m, output, m->selected[output]));
        break;
    case cmd_workspace_follow:
        if (w) {
            w_desktop_move_window(i, o, destination);
            w_model_workspace(m, destination);
            w_desktop_select(i);
        }
        break;
    case cmd_workspace_send:
        if (w) {
            w_desktop_move_window(i, o, destination);
            w_desktop_select(w_model_recent(m, o, ws));
        }
        break;
    case cmd_output_next:
    case cmd_output_send:
        w_desktop_cycle_output(c->kind == cmd_output_send);
        break;
    case cmd_split:
        space->next_axis = !strcmp(c->arg, "vertical") ? split_y : split_x;
        space->layout = layout_tree;
        break;
    case cmd_focus_mode_toggle: {
        int best = -1;
        if (!w)
            break;
        for (int k = 0; k < window_limit; k++) {
            w_model_window *candidate = &m->windows[k];
            if (w_model_visible(m, k) && candidate->output == o && candidate->floating != w->floating &&
                (best < 0 || candidate->focus_order > m->windows[best].focus_order))
                best = k;
        }
        if (best >= 0)
            w_desktop_select(best);
        break;
    }
    case cmd_layout: {
        if (!strcmp(c->arg, "toggle_split")) {
            int parent = w ? m->nodes[w->leaf].parent : 0;
            enum split_axis axis = parent ? m->nodes[parent].axis : space->next_axis;
            space->next_axis = axis == split_x ? split_y : split_x;
            if (parent)
                m->nodes[parent].axis = space->next_axis;
            space->layout = layout_tree;
            break;
        }
        const char *names[] = {"tree", "alternate", "master", "tabs", "stack"};
        for (int j = 0; j < 5; j++)
            if (!strcmp(names[j], c->arg))
                space->layout = (enum layout_kind)j;
        break;
    }
    case cmd_resize: {
        int d = w_command_direction(c->arg);
        if (d >= 0)
            w_desktop_resize_direction(d);
        else
            w_model_resize(m, !strcmp(c->arg, "grow") ? 50 : -50);
        break;
    }
    case cmd_floating:
        if (w) {
            bool floating = w->fullscreen || !w->floating;
            if (w->fullscreen)
                w_desktop_toggle_fullscreen(i);
            w_desktop_set_floating(i, floating);
        }
        break;
    case cmd_fullscreen:
        if (w)
            w_desktop_toggle_fullscreen(i);
        break;
    case cmd_scratch_send:
        if (w) {
            if (w->fullscreen)
                w_desktop_toggle_fullscreen(i);
            w_desktop_set_floating(i, true);
            w->scratch = true;
            w->scratch_visible = false;
            w_desktop_select(w_model_recent(m, o, ws));
        }
        break;
    case cmd_scratch_show: {
        int best = -1;
        for (int j = 0; j < window_limit; j++)
            if (m->windows[j].live && m->windows[j].scratch &&
                (best < 0 || m->windows[j].focus_order > m->windows[best].focus_order))
                best = j;
        if (best >= 0) {
            w_model_window *s = &m->windows[best];
            bool show = !(s->scratch_visible && s->output == o && s->workspace == ws);
            w_desktop_move_window(best, o, ws);
            s->scratch_visible = show;
            w_desktop_select(show ? best : w_model_recent(m, o, ws));
        }
        break;
    }
    case cmd_close:
        ok = w_desktop_close_foreground();
        break;
    case cmd_reload:
        w_desktop_load();
        break;
    case cmd_lock:
        ok = LockWorkStation() != 0;
        break;
    case cmd_windows_update:
        ok = w_queue_launch("ms-settings:windowsupdate", NULL, launch_command, NULL, false);
        break;
    case cmd_windows_security:
        ok = w_queue_launch("windowsdefender:", NULL, launch_command, NULL, false);
        break;
    case cmd_windows_settings:
        ok = w_queue_launch("ms-settings:", NULL, launch_command, NULL, false);
        break;
    case cmd_confirm_sign_out:
        if (MessageBoxW(NULL, L"Sign out and close your applications?", L"Sign out",
                        MB_OKCANCEL | MB_DEFBUTTON2 | MB_ICONQUESTION) == IDOK)
            ok = ExitWindowsEx(EWX_LOGOFF, 0) != 0;
        break;
    case cmd_sign_out:
        ok = ExitWindowsEx(EWX_LOGOFF, 0) != 0;
        break;
    case cmd_idle_inhibit:
        ok = SetThreadExecutionState(ES_CONTINUOUS |
                                     (desk.inhibit ? 0 : ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED)) != 0;
        if (ok)
            desk.inhibit = !desk.inhibit;
        break;
    case cmd_clipboard_clear:
        w_desktop_clear_clips();
        break;
    case cmd_capture:
        w_desktop_capture(output);
        break;
    case cmd_notify:
        w_desktop_notify(L"winwm",c->arg);
        break;
    case cmd_volume_up:
    case cmd_volume_down:
    case cmd_mute:
        ok = w_audio_start(desk.root, msg_audio) && w_audio_adjust(c->kind == cmd_mute ? 0 : c->kind == cmd_volume_up ? 5 : -5);
        break;
    case cmd_volume_set:
        ok = w_audio_start(desk.root, msg_audio) && w_audio_set_percent(atoi(c->arg));
        break;
    case cmd_media_play:
    case cmd_media_previous:
    case cmd_media_next:
    case cmd_media_stop: {
        UINT key = c->kind == cmd_media_next ? VK_MEDIA_NEXT_TRACK : c->kind == cmd_media_previous ?
                   VK_MEDIA_PREV_TRACK : c->kind == cmd_media_stop ? VK_MEDIA_STOP : VK_MEDIA_PLAY_PAUSE;
        INPUT keys[2] = {0};
        keys[0].type = keys[1].type = INPUT_KEYBOARD;
        keys[0].ki.wVk = keys[1].ki.wVk = (WORD)key;
        keys[1].ki.dwFlags = KEYEVENTF_KEYUP;
        ok = SendInput(2, keys, sizeof(INPUT)) == 2;
        break;
    }
    case cmd_provider_refresh:
        w_providers_refresh();
        break;
    case cmd_bar_toggle:
        desk.hide_bars = !desk.hide_bars;
        if (desk.hide_bars) w_providers_stop();
        else w_providers_configure(desk.root,msg_devices,&desk.config);
        w_desktop_status_configure();
        w_desktop_timer();
        break;
    case cmd_animations_toggle:
        desk.config.animations = desk.motion_allowed ? motion_off : motion_on;
        w_effects_preferences();
        break;
    case cmd_animations_system:
        desk.config.animations = motion_system;
        w_effects_preferences();
        break;
    case cmd_borders_toggle:
        desk.borders_hidden = !desk.borders_hidden;
        for (int i = 0; i < window_limit; i++)
            desk.windows[i].has_issued = false;
        break;
    case cmd_rescue:
        PostQuitMessage(42);
        break;
    case cmd_quit:
        PostQuitMessage(0);
        break;
    default:
        ok = false;
    }
    desk.revision++;
    w_desktop_schedule();
    if (desk.ipc_live)
        w_ipc_publish(desk.channels, w_command_name(c->kind));
    return ok;
}


////////////////
//~ Clipboard and capture

static SRWLOCK clipboard_lock = SRWLOCK_INIT;
static DWORD clipboard_generation, clipboard_sequence;
static bool clipboard_again;
static struct {
    DWORD generation;
    wchar_t text[count_of(desk.clips[0])];
} clipboard_read;

static bool w_history_permitted(void) {
    UINT exclude = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    UINT history = RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");
    if (!exclude || !history || IsClipboardFormatAvailable(exclude)) return false;
    if (!IsClipboardFormatAvailable(history)) return true;
    HANDLE h = GetClipboardData(history);
    const DWORD *value = h && GlobalSize(h) >= sizeof(DWORD) ? GlobalLock(h) : NULL;
    bool permitted = value && *value == 1;
    if (value) GlobalUnlock(h);
    return permitted;
}

static DWORD WINAPI w_read_clipboard(void *unused) {
    (void)unused;
    if (!OpenClipboard(NULL)) return 0;
    DWORD sequence = GetClipboardSequenceNumber();
    AcquireSRWLockShared(&clipboard_lock);
    clipboard_read.generation = clipboard_generation;
    bool fresh = !sequence || sequence != clipboard_sequence;
    ReleaseSRWLockShared(&clipboard_lock);
    if (fresh && w_history_permitted()) {
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        const wchar_t *s = h ? GlobalLock(h) : NULL;
        if (s) {
            size_t bound = min(GlobalSize(h) / sizeof(*s), count_of(clipboard_read.text));
            const wchar_t *end = wmemchr(s, 0, bound);
            if (end) {
                size_t length = (size_t)(end - s);
                memcpy(clipboard_read.text, s, length * sizeof(*s));
                clipboard_read.text[length] = L'\0';
            }
            GlobalUnlock(h);
        }
    }
    // Delayed rendering can advance the sequence while we hold the clipboard.
    AcquireSRWLockExclusive(&clipboard_lock);
    clipboard_sequence = GetClipboardSequenceNumber();
    ReleaseSRWLockExclusive(&clipboard_lock);
    CloseClipboard();
    return 0;
}

void w_desktop_copy_clip(void) {
    if (!desk.clipboard_enabled) return;
    if (desk.clipboard_reader) {
        clipboard_again = true;
        return;
    }
    DWORD sequence = GetClipboardSequenceNumber();
    if (sequence && sequence == clipboard_sequence) return;
    desk.clipboard_reader = CreateThread(NULL, 0, w_read_clipboard, NULL, 0, NULL);
}

void w_desktop_clipboard_done(void) {
    CloseHandle(desk.clipboard_reader);
    desk.clipboard_reader = NULL;
    bool append = clipboard_read.generation == clipboard_generation && clipboard_read.text[0] &&
                  (!desk.clip_count || wcscmp(clipboard_read.text, desk.clips[0]));
    if (append) {
        int keep = min(desk.clip_count, (int)count_of(desk.clips) - 1);
        memmove(desk.clips + 1, desk.clips, keep * sizeof(desk.clips[0]));
        wcscpy(desk.clips[0], clipboard_read.text);
        desk.clip_count = keep + 1;
    }
    SecureZeroMemory(&clipboard_read, sizeof(clipboard_read));
    if (clipboard_again) {
        clipboard_again = false;
        w_desktop_copy_clip();
    }
    if (append) w_desktop_clipboard_changed(false);
}

void w_desktop_clear_clips(void) {
    AcquireSRWLockExclusive(&clipboard_lock);
    ++clipboard_generation;
    clipboard_sequence = GetClipboardSequenceNumber();
    ReleaseSRWLockExclusive(&clipboard_lock);
    SecureZeroMemory(desk.clips, sizeof(desk.clips));
    desk.clip_count = 0;
    w_desktop_clipboard_changed(true);
}

void w_desktop_capture(int output) {
    w_rect r = desk.outputs[output].screen;
    HDC screen = GetDC(NULL), dc = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, r.w, r.h);
    bool ok = false;
    if (dc && bmp) {
        HGDIOBJ old = SelectObject(dc, bmp);
        ok = BitBlt(dc, 0, 0, r.w, r.h, screen, r.x, r.y, SRCCOPY | CAPTUREBLT) != 0;
        SelectObject(dc, old);
        if (ok && OpenClipboard(desk.root)) {
            EmptyClipboard();
            if (SetClipboardData(CF_BITMAP, bmp))
                bmp = NULL;
            CloseClipboard();
        }
    }
    w_desktop_notice(bmp || !ok ? "Capture failed" : "Display capture copied to clipboard");
    if (bmp)
        DeleteObject(bmp);
    if (dc)
        DeleteDC(dc);
    ReleaseDC(NULL, screen);
}
