#include "desktop/desktop.h"
#include <stdio.h>

static wchar_t clip_rows[count_of(desk.clips)][count_of(desk.clips[0])];
static w_catalog_state *snapshot;
static char aliases[window_limit][128];
static enum w_palette_group group;
static w_palette_entry entries[window_limit];
static w_command actions[window_limit];
static w_window_id targets[window_limit];
static int row_count, configured_start;
static int row_output[window_limit], palette_output;
static enum command_kind current_palette;
static HWND capture_target;
static DWORD capture_pid;

typedef struct {
    HWND owner;
    UINT request;
    wchar_t line[];
} w_run_request;

static DWORD WINAPI w_run_line_worker(void *arg) {
    w_run_request *job = arg;
    bool started = w_os_launch_wide_line(job->line);
    PostMessageW(job->owner, msg_run_result, job->request, started);
    free(job);
    return 0;
}

static void w_desktop_run_line(const wchar_t *line, UINT request) {
    size_t bytes = (wcslen(line) + 1) * sizeof(*line);
    w_run_request *job = malloc(sizeof(*job) + bytes);
    if (job) {
        job->owner = desk.root;
        job->request = request;
        memcpy(job->line, line, bytes);
        if (QueueUserWorkItem(w_run_line_worker, job, WT_EXECUTEDEFAULT)) return;
        free(job);
    }
    w_palette_line_result(request, false);
}

////////////////
//~ Command palette

static void w_add_row(const wchar_t *label, const char *action, w_window_id target) {
    if (row_count >= window_limit)
        return;
    w_command command = {0};
    if (action && !w_command_parse(action, &command)) return;
    int n = row_count++;
    row_output[n] = palette_output;
    entries[n] = (w_palette_entry){.id = n, .group = group};
    wcsncpy(entries[n].label, label, 255);
    entries[n].label[255] = 0;
    if (IS_HIGH_SURROGATE(entries[n].label[254])) entries[n].label[254] = 0;
    actions[n] = command;
    if (action && group != palette_flat &&
        w_str_copy(aliases[n], sizeof(aliases[n]), action))
        entries[n].command_name = aliases[n];
    targets[n] = target;
}

static void w_pick(int n) {
    if (current_palette == cmd_launcher && n >= window_limit) {
        if (!w_catalog_launch_item(snapshot, n - window_limit))
            w_desktop_notice("Could not start the selected application.");
        return;
    }
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
    } else if (actions[n].kind == cmd_capture_window) {
        w_desktop_capture_window(capture_target,capture_pid);
    } else if (actions[n].kind)
        w_desktop_run_on(&actions[n], row_output[n]);
}

static void w_action_row(const char *label, const char *action) {
    wchar_t s[1400]; // Shortcut descriptions may contain a complete command line.
    if (w_os_wide(label, s, count_of(s)))
        w_add_row(s, action, (w_window_id){-1, 0});
}

typedef struct {
    enum w_palette_group group;
    const char *label, *command;
} w_menu_item;

static const w_menu_item menu_items[] = {
    {palette_apps, "Terminal", "terminal"},
    {palette_places, "Home folder", "open_folder"},
    {palette_places, "Downloads", "open_place downloads"},
    {palette_places, "Documents", "open_place documents"},
    {palette_places, "Pictures", "open_place pictures"},
    {palette_places, "Desktop folder", "open_place desktop"},
    {palette_places, "Videos", "open_place videos"},
    {palette_places, "Configuration folder", "open_place config"},
    {palette_windows, "Switch window", "switcher"},
    {palette_windows, "Float or tile window", "floating"},
    {palette_windows, "Toggle fullscreen", "fullscreen"},
    {palette_windows, "Switch between floating and tiled windows", "focus_mode_toggle"},
    {palette_windows, "Show scratchpad window", "scratch_show"},
    {palette_windows, "Send window to scratchpad", "scratch_send"},
    {palette_windows, "Previous workspace", "workspace_back"},
    {palette_windows, "Focus next monitor", "output_next"},
    {palette_windows, "Move window to next monitor", "output_send"},
    {palette_windows, "Split horizontally", "split horizontal"},
    {palette_windows, "Split vertically", "split vertical"},
    {palette_windows, "Tree layout", "layout tree"},
    {palette_windows, "Alternating layout", "layout alternate"},
    {palette_windows, "Master layout", "layout master"},
    {palette_windows, "Tabbed layout", "layout tabs"},
    {palette_windows, "Stacked layout", "layout stack"},
    {palette_windows, "Resize mode", "resize_mode"},
    {palette_windows, "Grow focused split", "resize grow"},
    {palette_windows, "Shrink focused split", "resize shrink"},
    {palette_windows, "Close focused window", "close"},
    {palette_capture, "Copy display screenshot", "capture"},
    {palette_capture, "Copy window screenshot", "capture_window"},
    {palette_capture, "Copy region screenshot", "capture_region"},
    {palette_capture, "Record screen region", "record"},
    {palette_capture, "Pick screen color", "pick_color"},
    {palette_devices, "Sound devices", "audio"},
    {palette_devices, "Application volume mixer", "volume_mixer"},
    {palette_devices, "Wi-Fi networks", "wifi"},
    {palette_devices, "Network adapters", "network_settings"},
    {palette_devices, "Bluetooth devices", "bluetooth"},
    {palette_devices, "Languages", "languages"},
    {palette_devices, "Keyboard layouts", "keyboard_settings"},
    {palette_devices, "Power plans and timeouts", "power_settings"},
    {palette_appearance, "Change wallpaper", "wallpaper"},
    {palette_appearance, "Show or hide toolbar", "bar_toggle"},
    {palette_tools, "Clipboard history", "clipboard"},
    {palette_tools, "Notifications", "notifications"},
    {palette_tools, "Date and time", "date_time"},
    {palette_tools, "Play or pause media", "media_play"},
    {palette_tools, "Previous track", "media_previous"},
    {palette_tools, "Next track", "media_next"},
    {palette_tools, "Stop playback", "media_stop"},
    {palette_tools, "Raise volume", "volume_up"},
    {palette_tools, "Lower volume", "volume_down"},
    {palette_tools, "Mute or unmute", "mute"},
    {palette_developer, "Open project folder…", "project"},
    {palette_developer, "Activity monitor", "activity"},
    {palette_developer, "Edit winwm configuration", "edit_config"},
    {palette_developer, "Reload configuration", "reload"},
    {palette_developer, "Refresh toolbar providers", "provider_refresh"},
    {palette_developer, "Keyboard shortcuts", "shortcuts"},
    {palette_system, "Desktop settings", "settings"},
    {palette_system, "Open configured task manager", "open_tasks"},
    {palette_system, "Lock computer", "lock"},
    {palette_system, "Sleep…", "sleep"},
    {palette_system, "Restart computer…", "restart"},
    {palette_system, "Sign out…", "confirm_sign_out"},
};

static void w_device_rows(void) {
    for (int o = 0; o < output_limit; ++o) {
        w_output_state *d = &desk.outputs[o];
        if (!d->live) continue;
        wchar_t label[160];
        const wchar_t *number = wcsrchr(d->device, L'\\');
        number = number ? number+1 : d->device;
        if (!_wcsnicmp(number, L"DISPLAY", 7)) number += 7;
        swprintf(label, count_of(label), L"Display %ls   ·   %d × %d   ·   %d%%", number, d->screen.w,
                 d->screen.h, MulDiv(d->dpi,100,96));
        int before = row_count;
        w_add_row(label, "display", (w_window_id){-1, 0});
        if (row_count > before) row_output[before] = o;
    }
}

static void w_menu_actions(bool run) {
    for (size_t i = 0; i < count_of(menu_items); ++i) {
        const w_menu_item *item = &menu_items[i];
        if (!run && item->group != palette_devices && item->group != palette_appearance &&
            item->group != palette_developer && strcmp(item->command,"date_time")) continue;
        group = run ? item->group : palette_flat;
        w_action_row(item->label, item->command);
    }
    group = run ? palette_devices : palette_flat;
    w_device_rows();
    group = run ? palette_appearance : palette_flat;
    w_action_row(desk.motion_allowed ? "Disable animations" : "Enable animations", "animations_toggle");
    if (desk.config.animations != motion_system)
        w_action_row("Follow Windows animation preference", "animations_system");
    w_action_row(desk.borders_hidden ? "Show window borders" : "Hide window borders", "borders_toggle");
    group = run ? palette_system : palette_flat;
    w_action_row(desk.inhibit ? "Resume automatic sleep" : "Keep awake", "idle_inhibit");
    w_action_row("Clear clipboard history", "clipboard_clear");
    if (!run) return;
    group = palette_windows;
    const char *directions[] = {"left", "right", "up", "down"};
    for (size_t i = 0; i < count_of(directions); ++i) {
        char label[96], command[32];
        snprintf(label,sizeof(label),"Move window %s",directions[i]);
        snprintf(command,sizeof(command),"move %s",directions[i]);
        w_action_row(label,command);
        snprintf(label,sizeof(label),"Focus window %s",directions[i]);
        snprintf(command,sizeof(command),"focus %s",directions[i]);
        w_action_row(label,command);
    }
    for (int i = 0; i < workspace_limit; ++i) {
        char label[128], command[48];
        const char *name = desk.config.workspace_names[i];
        snprintf(label,sizeof(label),"Switch to workspace %d%s%s",i+1,*name ? " · " : "",name);
        snprintf(command,sizeof(command),"workspace_select %d",i+1);
        w_action_row(label,command);
        snprintf(label,sizeof(label),"Send window to workspace %d%s%s",i+1,*name ? " · " : "",name);
        snprintf(command,sizeof(command),"workspace_send %d",i+1);
        w_action_row(label,command);
    }
    group = palette_apps;
    configured_start = row_count;
    for (int i = 0; i < desk.config.app_count; ++i) {
        char command[160];
        snprintf(command,sizeof(command),"launch %s",desk.config.apps[i].name);
        w_action_row(desk.config.apps[i].name,command);
    }
}

void w_desktop_clear_clips(void) {
    SecureZeroMemory(desk.clips, sizeof(desk.clips));
    SecureZeroMemory(clip_rows, sizeof(clip_rows));
    desk.clip_count = 0;
}

void w_desktop_catalog_update(void) {
    if (!w_palette_visible(L"Run")) return;
    int output = palette_output;
    if (!desk.outputs[output].live) output = desk.model.output;
    w_desktop_palette(cmd_launcher, output);
}

void w_desktop_palette(enum command_kind kind, int output) {
    if (!w_palette_shown()) {
        capture_target = GetForegroundWindow();
        capture_pid = 0;
        GetWindowThreadProcessId(capture_target,&capture_pid);
    }
    group = palette_flat;
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
        for (int j = 0; j < desk.clip_count; j++) {
            wchar_t preview[palette_text_capacity];
            size_t n = 0;
            bool space = false;
            for (const wchar_t *p = clip_rows[j]; *p && n+1 < count_of(preview); ++p) {
                if (*p == L'\r' || *p == L'\n' || *p == L'\t') { space = n > 0; continue; }
                if (space && n+2 < count_of(preview)) preview[n++] = L' ';
                space = false;
                preview[n++] = *p;
            }
            preview[n] = 0;
            w_add_row(n ? preview : L"Whitespace", NULL, (w_window_id){-1, 0});
        }
    } else {
        bool run = kind == cmd_launcher;
        if (!run) title = L"Settings";
        w_menu_actions(run);
        if (run) {
            w_catalog_begin(desk.root, msg_catalog);
            w_catalog_state *next = w_catalog_acquire();
            int apps = w_catalog_count(next);
            w_palette_entry *all = apps ? calloc((size_t)row_count + apps, sizeof(*all)) : NULL;
            int total = row_count;
            if (all) {
                memcpy(all, entries, (size_t)row_count * sizeof(*entries));
                for (int i = 0; i < apps; ++i) {
                    w_palette_entry *row = &all[total++];
                    row->identity = w_catalog_identity(next,i);
                    w_catalog_label(next,i,row->label,count_of(row->label),&row->system_icon);
                    // An explicitly configured app owns its plain display name.
                    // Keep the Start menu shortcut independently selectable.
                    for (int j = configured_start; j < row_count; ++j) {
                        if (!_wcsicmp(all[j].label, row->label) &&
                            wcslen(row->label) + 15 < count_of(row->label)) {
                            wcscat(row->label, L"  ·  Installed");
                            break;
                        }
                    }
                    row->id = window_limit+i;
                    row->group = palette_apps;
                }
            }
            w_catalog_release(snapshot);
            snapshot = next;
            w_palette_open(desk.outputs[output].monitor,title,all ? all : entries,total,w_pick,
                           w_catalog_images(snapshot),w_desktop_run_line);
            free(all);
            return;
        }
    }
    w_catalog_release(snapshot);
    snapshot = NULL;
    w_palette_open(desk.outputs[output].monitor,title,entries,row_count,w_pick,NULL,NULL);
}
