#include "desktop/desktop.h"
#include "providers/audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

////////////////
//~ Actions

enum launch_kind { launch_executable, launch_terminal, launch_command, launch_task_manager, launch_panel };
typedef struct w_launch_job {
    char exe[1024], arg[4096];
    enum launch_kind kind;
    POINT origin;
} w_launch_job;

////////////////
//~ Actions Functions

static DWORD WINAPI w_launch_thread(void *ptr) {
    w_launch_job *j = ptr;
    bool launched;
    switch (j->kind) {
    case launch_panel: {
        wchar_t self[os_path_cap], mode[96];
        HANDLE child = w_os_self(self, count_of(self)) && w_os_wide(j->exe, mode, count_of(mode)) ?
                       w_os_launch_positioned(self, mode, j->origin) : NULL;
        launched = child != NULL;
        if (child) CloseHandle(child);
        break;
    }
    case launch_command:
        launched = w_os_launch_line(j->exe);
        break;
    case launch_terminal:
        launched = w_os_launch_terminal(j->exe);
        break;
    case launch_task_manager: {
        wchar_t path[os_path_cap];
        HANDLE child = w_os_task_manager(j->exe,path,count_of(path)) ? w_os_launch(path,NULL) : NULL;
        launched = child != NULL;
        if (child)
            CloseHandle(child);
        break;
    }
    default:
        launched = w_os_launch_provider(j->exe,j->arg);
        break;
    }
    if (!launched) {
        char *why = malloc(512);
        if (why) {
            snprintf(why, 512, "Could not start %.380s. Check its configured executable path.", j->exe);
            if (!PostMessageW(desk.root, msg_status, 0, (LPARAM)why))
                free(why);
        }
    }
    free(j);
    return 0;
}

static bool w_queue_launch(const char *exe, const char *arg, enum launch_kind kind, const POINT *origin) {
    w_launch_job *j = calloc(1, sizeof(*j));
    if (!j)
        return false;
    j->kind = kind;
    if (origin) j->origin = *origin;
    if (!w_str_copy(j->exe, sizeof(j->exe), exe) || !w_str_copy(j->arg, sizeof(j->arg), arg ? arg : "") ||
        !QueueUserWorkItem(w_launch_thread, j, WT_EXECUTEDEFAULT)) {
        free(j);
        return false;
    }
    return true;
}

static DWORD WINAPI w_start_configured_programs(void *ptr) {
    w_startup_entry *entries = ptr;
    for (int i = 0; entries[i].command_line[0]; i++) {
        if (w_os_launch_line(entries[i].command_line))
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

static bool w_launch(const char *exe, const char *arg) {
    return w_queue_launch(exe, arg, launch_executable, NULL);
}

////////////////
//~ Desktop commands

void w_desktop_notice(const char *s) {
    w_str_copy(desk.status, sizeof(desk.status), s);
    if (desk.notice_count == 32) {
        memmove(desk.notices, desk.notices + 1, 31 * sizeof(desk.notices[0]));
        desk.notice_count--;
    }
    wchar_t *entry = desk.notices[desk.notice_count++];
    if (!w_os_wide(s, entry, 256))
        wcscpy(entry, L"See winwm.exe ctl doctor for details");
}

void w_desktop_action(int output, const char *s) {
    w_command c;
    if (w_command_parse(s, &c))
        w_desktop_run_on(&c, output);
}

static bool w_open_files(const w_command *c) {
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
           w_launch(desk.config.file_browser, utf8);
}

static wchar_t clip_rows[24][512];
bool w_desktop_run(const w_command *c) {
    return w_desktop_run_on(c, -1);
}

bool w_desktop_run_on(const w_command *c, int output) {
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
    if ((unsigned)output >= output_limit || !desk.outputs[output].live)
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
    switch (c->kind) {
    case cmd_terminal:
        ok = w_queue_launch(desk.config.terminal, NULL, launch_terminal, NULL);
        break;
    case cmd_open_tasks:
        ok = w_queue_launch(desk.config.task_manager, NULL, launch_task_manager, NULL);
        break;
    case cmd_open_folder:
    case cmd_reveal_file:
        ok = w_open_files(c);
        break;
    case cmd_launch: {
        const char *configured = NULL;
        for (int j = 0; j < desk.config.app_count; j++)
            if (!strcmp(c->arg, desk.config.apps[j].name)) {
                configured = desk.config.apps[j].path;
                break;
            }
        ok = configured ? w_launch(configured,"") : w_catalog_launch(c->arg) || w_launch(c->arg,"");
        break;
    }
    case cmd_audio:
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
    case cmd_capture_window:
    case cmd_record: {
        char mode[64];
        snprintf(mode, sizeof(mode), "--tool-%s", w_command_name(c->kind));
        w_rect work = desk.outputs[output].work;
        POINT origin = {work.x + work.w/2, work.y + work.h/2};
        ok = w_queue_launch(mode, NULL, launch_panel, &origin);
        break;
    }
    case cmd_edit_config: {
        wchar_t path[os_path_cap];
        char s[4096];
        ok = w_os_config_path(path, count_of(path)) && w_os_utf8(path, s, sizeof(s)) &&
             w_launch(desk.config.editor, s);
        break;
    }
    case cmd_launcher:
    case cmd_switcher:
    case cmd_shortcuts:
    case cmd_settings:
    case cmd_clipboard:
    case cmd_notifications:
        w_desktop_focus_cancel();
        if (desk.resize) {
            desk.resize = false;
            w_input_mode(false);
        }
        if (c->kind == cmd_launcher && w_palette_visible(L"Run"))
            w_palette_focus();
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
        ok = w_queue_launch(c->arg, "", launch_command, NULL);
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
        w_desktop_select(m->focus);
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
    case cmd_confirm_sign_out:
        if (MessageBoxW(NULL, L"Sign out and close your applications?", L"Sign out",
                        MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION) == IDYES)
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
        SecureZeroMemory(desk.clips, sizeof(desk.clips));
        SecureZeroMemory(clip_rows, sizeof(clip_rows));
        desk.clip_count = 0;
        break;
    case cmd_capture:
        w_desktop_capture();
        break;
    case cmd_notify:
        w_desktop_notice(c->arg);
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
    default:
        ok = false;
    }
    desk.revision++;
    w_desktop_schedule();
    if (desk.ipc_live)
        w_ipc_publish(desk.channels, w_command_name(c->kind));
    return ok;
}

static w_palette_entry entries[palette_limit];
static w_command actions[palette_limit];
static w_window_id targets[palette_limit];
static int row_count;
static int row_output[palette_limit], palette_output;
static enum command_kind current_palette;

////////////////
//~ Command palette

static void w_add_row(const wchar_t *label, const char *action, w_window_id target) {
    if (row_count >= palette_limit)
        return;
    int n = row_count++;
    row_output[n] = palette_output;
    entries[n].id = n;
    wcsncpy(entries[n].label, label, 255);
    entries[n].label[255] = 0;
    actions[n] = (w_command){0};
    if (action)
        w_command_parse(action, &actions[n]);
    targets[n] = target;
}

static void w_pick(int n) {
    if ((unsigned)n >= (unsigned)row_count)
        return;
    if (current_palette == cmd_switcher) {
        if (w_model_valid(&desk.model, targets[n]))
            w_desktop_activate(targets[n].slot);
    } else if (current_palette == cmd_clipboard) {
        if (n >= desk.clip_count)
            return;
        size_t bytes = (wcslen(clip_rows[n]) + 1) * sizeof(wchar_t);
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (!mem)
            return;
        void *p = GlobalLock(mem);
        if (!p) {
            GlobalFree(mem);
            return;
        }
        memcpy(p, clip_rows[n], bytes);
        GlobalUnlock(mem);
        if (OpenClipboard(desk.root)) {
            EmptyClipboard();
            if (SetClipboardData(CF_UNICODETEXT, mem))
                mem = NULL;
            CloseClipboard();
        }
        if (mem)
            GlobalFree(mem);
    } else if (actions[n].kind)
        w_desktop_run_on(&actions[n], row_output[n]);
}

static void w_action_row(const char *label, const char *action) {
    wchar_t s[256];
    if (w_os_wide(label, s, count_of(s)))
        w_add_row(s, action, (w_window_id){-1, 0});
}

void w_desktop_palette(enum command_kind kind, int output) {
    current_palette = kind;
    palette_output = output;
    row_count = 0;
    const wchar_t *title = L"Run";
    if (kind == cmd_switcher) {
        title = L"Switch window";
        for (int j = 0; j < window_limit; j++)
            if (desk.model.windows[j].live) {
                wchar_t s[256] = {0};
                GetWindowTextW(desk.windows[j].hwnd, s, count_of(s));
                w_add_row(*s ? s : L"Untitled window", NULL, (w_window_id){j, desk.model.windows[j].generation});
            }
    } else if (kind == cmd_shortcuts) {
        title = L"Active bindings";
        for (int j = 0; j < desk.config.binding_count; j++) {
            w_key_binding *b = &desk.config.bindings[j];
            char action[1200], label[1400];
            snprintf(action, sizeof(action), "%s%s%s", w_command_name(b->action.kind),
                     *b->action.arg ? " " : "", b->action.arg);
            snprintf(label, sizeof(label), "%s   %s%s", b->chord, action,
                     desk.registered[j] ? "" : "   [unavailable]");
            w_action_row(label, action);
        }
    } else if (kind == cmd_notifications) {
        title = L"Notifications";
        for (int j = desk.notice_count - 1; j >= 0; j--)
            w_add_row(desk.notices[j], NULL, (w_window_id){-1, 0});
    } else if (kind == cmd_clipboard) {
        if (!desk.clipboard_enabled) {
            desk.clipboard_enabled = AddClipboardFormatListener(desk.root) != 0;
            w_desktop_copy_clip();
        }
        title = L"Clipboard history";
        memcpy(clip_rows, desk.clips, sizeof(clip_rows));
        for (int j = 0; j < desk.clip_count; j++)
            w_add_row(desk.clips[j], NULL, (w_window_id){-1, 0});
    } else if (kind == cmd_settings) {
        title = L"Settings";
        for (int o = 0; o < output_limit; o++)
            if (desk.outputs[o].live) {
                w_output_state *d = &desk.outputs[o];
                wchar_t s[160];
                const wchar_t *number = !wcsncmp(d->device, L"\\\\.\\DISPLAY", 11) ? d->device + 11 : d->device;
                swprintf(s, count_of(s), L"Display %ls   ·   %d × %d   ·   %d%%", number, d->screen.w, d->screen.h,
                         MulDiv(d->dpi, 100, 96));
                w_add_row(s, "display", (w_window_id){-1, 0});
                row_output[row_count-1] = o;
            }
        static const struct { const char *label, *action; } items[] = {
            {"Sound devices", "audio"}, {"Volume mixer", "volume_mixer"}, {"Wi-Fi and networks", "wifi"},
            {"Bluetooth devices", "bluetooth"}, {"Add languages and keyboards", "languages"},
            {"Set up keyboard layouts", "keyboard_settings"}, {"Power and sleep", "power_settings"},
            {"Date and time", "date_time"}, {"Change wallpaper", "wallpaper"}, {"Edit configuration", "edit_config"},
            {"Reload configuration", "reload"}, {"Keyboard shortcuts", "shortcuts"}, {"Volume up", "volume_up"},
            {"Volume down", "volume_down"}, {"Mute or unmute", "mute"}, {"Toggle toolbar", "bar_toggle"},
            {"Clear clipboard history", "clipboard_clear"}
        };
        for (size_t i = 0; i < count_of(items); i++)
            w_action_row(items[i].label, items[i].action);
        w_action_row(desk.inhibit ? "Resume automatic sleep" : "Keep awake", "idle_inhibit");
        w_action_row(desk.motion_allowed ? "Disable animations" : "Enable animations", "animations_toggle");
        if (desk.config.animations != motion_system)
            w_action_row("Follow Windows animation preference", "animations_system");
        w_action_row(desk.borders_hidden ? "Show window borders" : "Hide window borders", "borders_toggle");
    } else {
        for (int j = 0; j < desk.config.app_count; j++) {
            char a[160];
            snprintf(a, sizeof(a), "launch %s", desk.config.apps[j].name);
            w_action_row(desk.config.apps[j].name, a);
        }
        w_catalog_begin(desk.root, msg_catalog);
        for (int j = 0; j < w_catalog_count(); j++) {
            char name[768], action[800];
            if (!w_catalog_label(j, name, sizeof(name)))
                continue;
            snprintf(action, sizeof(action), "launch %s", name);
            w_action_row(name, action);
        }
        for (int j = 1; j < cmd_count; j++) {
            w_command c;
            if (w_command_parse(w_command_name((enum command_kind)j), &c) && c.kind != cmd_launcher)
                w_action_row(w_command_name(c.kind), w_command_name(c.kind));
        }
        const char *ops[] = {"focus left",  "focus right",      "focus up",         "focus down",
                             "resize grow", "resize shrink",    "split horizontal", "split vertical",
                             "layout tree", "layout alternate", "layout master",    "layout tabs",
                             "layout stack"};
        for (size_t j = 0; j < count_of(ops); j++)
            w_action_row(ops[j], ops[j]);
        for (int j = 1; j <= 10; j++) {
            char s[48];
            snprintf(s, sizeof(s), "workspace_select %d", j);
            w_action_row(s, s);
        }
    }
    w_palette_open(desk.outputs[output].monitor, title, entries, row_count, w_pick);
}

////////////////
//~ Clipboard and capture

void w_desktop_copy_clip(void) {
    if (!desk.clipboard_enabled || !OpenClipboard(desk.root))
        return;

    UINT exclude = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    UINT history = RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");
    HANDLE h = NULL;
    if (!IsClipboardFormatAvailable(exclude) && !IsClipboardFormatAvailable(history))
        h = GetClipboardData(CF_UNICODETEXT);
    if (h && GlobalSize(h) <= 1024) {
        const wchar_t *s = GlobalLock(h);
        size_t bound = GlobalSize(h) / sizeof(wchar_t);
        if (s && bound && *s && wmemchr(s, 0, bound) && wcslen(s) < count_of(desk.clips[0]) &&
            (!desk.clip_count || wcscmp(s, desk.clips[0]))) {
            int keep = w_clamp_int(desk.clip_count, 0, 23);
            memmove(desk.clips + 1, desk.clips, keep * sizeof(desk.clips[0]));
            wcscpy(desk.clips[0], s);
            desk.clip_count = keep + 1;
        }
        if (s)
            GlobalUnlock(h);
    }
    CloseClipboard();
}

void w_desktop_capture(void) {
    w_rect r = desk.outputs[desk.model.output].screen;
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
