#include "desktop/desktop.h"
#include "providers/providers.h"
#include <stdio.h>

static wchar_t clip_rows[count_of(desk.clips)][count_of(desk.clips[0])];
static w_catalog_state *snapshot;
static wchar_t aliases[window_limit][128];
static enum w_palette_group group;
static w_palette_entry entries[window_limit];
static w_command actions[window_limit];
static w_window_id targets[window_limit];
static int row_count, configured_start;
static const wchar_t *configured_labels[app_limit];
static int row_output[window_limit], palette_output;
static enum command_kind current_palette;
static HWND menu_target;
static DWORD menu_pid;
static w_window_id menu_owner;
static bool dispatching_row;

typedef struct {
    HWND owner;
    UINT request;
    bool admin;
    wchar_t line[];
} w_run_request;

static DWORD WINAPI w_run_line_worker(void *arg) {
    w_run_request *job = arg;
    SetLastError(ERROR_SUCCESS);
    bool started = w_os_launch_wide_line(job->line, job->admin);
    DWORD error = ERROR_SUCCESS;
    if (!started) {
        error = GetLastError();
        if (!error) error = ERROR_GEN_FAILURE;
    }
    PostMessageW(job->owner, msg_run_result, job->request, error);
    free(job);
    return 0;
}

static void w_desktop_run_line(const wchar_t *line, UINT request, bool admin) {
    size_t bytes = (wcslen(line) + 1) * sizeof(*line);
    w_run_request *job = malloc(sizeof(*job) + bytes);
    if (job) {
        job->owner = desk.root;
        job->request = request;
        job->admin = admin;
        memcpy(job->line, line, bytes);
        if (QueueUserWorkItem(w_run_line_worker, job, WT_EXECUTEDEFAULT)) return;
        free(job);
    }
    w_palette_line_result(request, ERROR_NOT_ENOUGH_MEMORY);
}

////////////////
//~ Command palette

static const wchar_t command_glyphs[cmd_count] = {
    [cmd_terminal]=0xe756, [cmd_program]=0xe756, [cmd_open_folder]=0xe8b7, [cmd_open_place]=0xe8b7,
    [cmd_edit_config]=0xe70f, [cmd_project]=0xe943, [cmd_settings]=0xe713, [cmd_shortcuts]=0xe765,
    [cmd_close]=0xe8bb, [cmd_fullscreen]=0xe740, [cmd_workspace_back]=0xe72b,
    [cmd_clipboard]=0xe77f, [cmd_clipboard_clear]=0xe74d, [cmd_notifications]=0xe7e7,
    [cmd_capture]=0xe722, [cmd_capture_window]=0xe722, [cmd_capture_region]=0xe722, [cmd_record]=0xe7c8,
    [cmd_volume_up]=0xe995, [cmd_volume_down]=0xe993, [cmd_mute]=0xe74f, [cmd_audio]=0xe767,
    [cmd_volume_set]=0xe767, [cmd_volume_mixer]=0xe9e9, [cmd_microphone]=0xe720,
    [cmd_media_previous]=0xe892, [cmd_media_play]=0xe768, [cmd_media_next]=0xe893, [cmd_media_stop]=0xe71a,
    [cmd_wifi]=0xe701, [cmd_bluetooth]=0xe702, [cmd_network_settings]=0xe839, [cmd_display]=0xe7f4,
    [cmd_date_time]=0xe823, [cmd_languages]=0xe774, [cmd_input_methods]=0xe765, [cmd_keyboard_settings]=0xe765,
    [cmd_packages]=0xe896, [cmd_chocolatey]=0xe896, [cmd_google_japanese_input]=0xe896, [cmd_updates]=0xe895, [cmd_windows_update]=0xe895,
    [cmd_reload]=0xe72c, [cmd_provider_refresh]=0xe72c, [cmd_lock]=0xe72e, [cmd_restart]=0xe777,
    [cmd_sleep]=0xe708, [cmd_confirm_sign_out]=0xf3b1, [cmd_power_settings]=0xe7e8, [cmd_quit]=0xe8bb,
    [cmd_windows_security]=0xea18, [cmd_windows_settings]=0xe713,
};

static void w_add_row(const wchar_t *label, const char *action, w_window_id target) {
    if (row_count >= window_limit)
        return;
    w_command command = {0};
    if (action && !w_command_parse(action, &command)) return;
    int n = row_count++;
    row_output[n] = -1;
    entries[n] = (w_palette_entry){.id = n, .group = group, .glyph = command_glyphs[command.kind]};
    entries[n].can_run_admin = w_desktop_can_run_admin(command.kind);
    if (group!=palette_flat && command.kind==cmd_program)
        w_catalog_program_caption(entries[n].label,count_of(entries[n].label),label,command.arg,true);
    else wcsncpy(entries[n].label, label, 255);
    entries[n].label[255] = 0;
    if (IS_HIGH_SURROGATE(entries[n].label[254])) entries[n].label[254] = 0;
    actions[n] = command;
    if (action && group != palette_flat &&
        w_os_wide(action,aliases[n],count_of(aliases[n])))
        entries[n].alias = aliases[n];
    targets[n] = target;
}

static void w_pick(int n, bool admin) {
    if (current_palette == cmd_menu && n >= window_limit) {
        if (!w_catalog_launch_item(snapshot, n - window_limit, admin))
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
        if (!mem) {
            w_desktop_notice("Could not copy the history entry.");
            return;
        }
        void *p = GlobalLock(mem);
        if (!p) {
            GlobalFree(mem);
            w_desktop_notice("Could not copy the history entry.");
            return;
        }
        memcpy(p, clip_rows[n], bytes);
        GlobalUnlock(mem);
        if (OpenClipboard(desk.root)) {
            if (EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, mem))
                mem = NULL;
            CloseClipboard();
            if (mem) w_desktop_notice("Could not copy the history entry.");
        } else {
            w_desktop_notice("Clipboard is busy. Select the history entry again.");
        }
        if (mem)
            GlobalFree(mem);
    } else if (actions[n].kind == cmd_capture_window) {
        w_desktop_capture_window(menu_target,menu_pid);
    } else if (actions[n].kind == cmd_close) {
        if (!w_desktop_close_target(menu_target,menu_pid,menu_owner))
            w_desktop_notice("The selected window is no longer available to close.");
    } else if (actions[n].kind) {
        dispatching_row = true;
        int output = row_output[n] >= 0 ? row_output[n] : palette_output;
        const w_popup_anchor *anchor = w_palette_anchor();
        if (anchor && !admin && output == palette_output)
            w_desktop_run_anchored(&actions[n], output, false, anchor);
        else
            w_desktop_run_on(&actions[n], output, admin);
        dispatching_row = false;
    }
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
    {palette_tools, "Open PowerShell", "program %SystemRoot%\\System32\\WindowsPowerShell\\v1.0\\powershell.exe -NoLogo"},
    {palette_tools, "Open Command Prompt", "program %SystemRoot%\\System32\\cmd.exe"},
    {palette_tools, "Copy display screenshot", "capture"},
    {palette_tools, "Copy window screenshot", "capture_window"},
    {palette_tools, "Copy region screenshot", "capture_region"},
    {palette_tools, "Start or stop monitor recording", "record"},
    {palette_tools, "Pick screen color", "pick_color"},
    {palette_settings, "Windows Security", "windows_security"},
    {palette_settings, "Windows Settings", "windows_settings"},
    {palette_settings, "Control Panel", "program %SystemRoot%\\System32\\control.exe"},
    {palette_settings, "Sound devices", "audio"},
    {palette_settings, "Microphone inputs", "microphone"},
    {palette_settings, "Application volume mixer", "volume_mixer"},
    {palette_settings, "Wi-Fi networks", "wifi"},
    {palette_settings, "Network adapters", "network_settings"},
    {palette_settings, "Bluetooth devices", "bluetooth"},
    {palette_input, "Add or remove languages", "languages"},
    {palette_input, "Manage keyboard layouts", "keyboard_settings"},
    {palette_input, "Choose input method and test typing", "input_methods"},
    {palette_input, "Get Google Japanese Input", "google_japanese_input"},
    {palette_software, "Find and install software with WinGet", "packages"},
    {palette_software, "Find and install software with Chocolatey", "chocolatey"},
    {palette_software, "Review and apply software updates", "updates"},
    {palette_software, "Windows Update settings", "windows_update"},
    {palette_settings, "Power plans and timeouts", "power_settings"},
    {palette_settings, "Change wallpaper", "wallpaper"},
    {palette_settings, "Show or hide toolbar", "bar_toggle"},
    {palette_settings, "Configure toolbar", "bar_settings"},
    {palette_tools, "Clipboard history", "clipboard"},
    {palette_tools, "Notifications", "notifications"},
    {palette_settings, "Date and time", "date_time"},
    {palette_tools, "Play or pause media", "media_play"},
    {palette_tools, "Previous track", "media_previous"},
    {palette_tools, "Next track", "media_next"},
    {palette_tools, "Stop playback", "media_stop"},
    {palette_tools, "Raise volume", "volume_up"},
    {palette_tools, "Lower volume", "volume_down"},
    {palette_tools, "Mute or unmute", "mute"},
    {palette_places, "Open project folder…", "project"},
    {palette_tools, "Activity monitor", "activity"},
    {palette_settings, "Edit winwm configuration", "edit_config"},
    {palette_settings, "Reload configuration", "reload"},
    {palette_tools, "Keyboard shortcuts", "shortcuts"},
    {palette_tools, "Open configured task manager", "open_tasks"},
    {palette_session, "Lock computer", "lock"},
    {palette_session, "Sleep…", "sleep"},
    {palette_session, "Restart computer…", "restart"},
    {palette_session, "Sign out…", "confirm_sign_out"},
    {palette_session, "Exit winwm and return to Windows", "quit"},
};

static const wchar_t *w_menu_keywords(enum command_kind kind) {
    switch (kind) {
    case cmd_languages: return L"add remove language languages japanese english ja jp 日本語 preferred region";
    case cmd_keyboard_settings: return L"add keyboard layout dvorak qwerty input";
    case cmd_input_methods: return L"ime microsoft japanese input method romaji hiragana katakana kanji typing 日本語";
    case cmd_google_japanese_input: return L"google japanese input ime gboard mozc 日本語入力";
    case cmd_packages: return L"winget search install package packages software apps applications";
    case cmd_chocolatey: return L"choco chocolatey search install package packages software apps applications admin";
    case cmd_updates: return L"winget upgrade update packages apps applications";
    case cmd_windows_update: return L"windows system updates check";
    case cmd_windows_security: return L"defender antivirus virus threat protection firewall";
    case cmd_project: return L"developer recent project folder terminal editor";
    default: return NULL;
    }
}

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

static void w_toolbar_action(const char *module, const char *action, int first) {
    if (!*action) return;
    char label[200];
    snprintf(label, sizeof(label), "%s   ·   %s", module, action);
    wchar_t name[200];
    if (!w_os_wide(label, name, count_of(name))) return;
    for (int i = first; i < row_count; ++i)
        if (!wcscmp(entries[i].label, name)) return;
    w_add_row(name, action, (w_window_id){-1, 0});
}

static void w_menu_actions(bool run) {
    for (size_t i = 0; i < count_of(menu_items); ++i) {
        const w_menu_item *item = &menu_items[i];
        if (!run && item->group != palette_settings && item->group != palette_input &&
            strcmp(item->command,"shortcuts") && strcmp(item->command,"activity") && strcmp(item->command,"project")) continue;
        group = run ? item->group : palette_flat;
        int previous = row_count;
        w_action_row(item->label, item->command);
        if (row_count>previous) entries[previous].keywords=w_menu_keywords(actions[previous].kind);
    }
    group = run ? palette_settings : palette_flat;
    if (w_providers_need_manual_refresh())
        w_action_row("Refresh toolbar providers", "provider_refresh");
    w_device_rows();
    group = run ? palette_settings : palette_flat;
    w_action_row(desk.motion_allowed ? "Disable animations" : "Enable animations", "animations_toggle");
    if (desk.config.animations != motion_system)
        w_action_row("Follow Windows animation preference", "animations_system");
    w_action_row(desk.borders_hidden ? "Show window borders" : "Hide window borders", "borders_toggle");
    group = run ? palette_session : palette_flat;
    w_action_row(desk.inhibit ? "Resume automatic sleep" : "Keep awake", "idle_inhibit");
    group = run ? palette_tools : palette_flat;
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
    group = palette_toolbar;
    int toolbar_first = row_count;
    for (int i = 0; i < desk.config.style_count; ++i) {
        const w_module_style *style = &desk.config.styles[i];
        const char *commands[] = {style->action, style->middle_click, style->right_click,
                                  style->scroll_up, style->scroll_down};
        for (size_t j = 0; j < count_of(commands); ++j)
            w_toolbar_action(style->name, commands[j], toolbar_first);
    }
    for (int i = 0; i < desk.config.provider_count; ++i) {
        const w_provider_config *provider = &desk.config.providers[i];
        char module[56];
        snprintf(module, sizeof(module), "custom/%s", provider->name);
        w_toolbar_action(module, provider->action, toolbar_first);
    }
    group = palette_apps;
    configured_start = row_count;
    for (int i = 0; i < desk.config.app_count; ++i) {
        char command[160];
        snprintf(command,sizeof(command),"launch %s",desk.config.apps[i].name);
        int previous=row_count;
        w_action_row(desk.config.apps[i].name,command);
        wchar_t name[80];
        if (row_count>previous && w_os_wide(desk.config.apps[i].name,name,count_of(name)))
            w_catalog_program_caption(entries[previous].label,count_of(entries[previous].label),name,desk.config.apps[i].path,false);
    }
}

static int w_compare_label(const void *a, const void *b) {
    return _wcsicmp(*(const wchar_t *const *)a,*(const wchar_t *const *)b);
}

static void w_installed_row(int index, w_palette_entry *row) {
    row->identity = w_catalog_identity(snapshot,index);
    w_catalog_info info=w_catalog_label(snapshot,index,row->label,count_of(row->label));
    row->system_icon=info.icon;
    row->alias=info.name;
    row->keywords=info.keywords;
    const wchar_t *label = row->label;
    int configured = row_count-configured_start;
    if (configured && bsearch(&label,configured_labels,configured,sizeof(label),w_compare_label) &&
        wcslen(row->label)+15 < count_of(row->label))
        wcscat(row->label,L"  ·  Installed");
    row->id = window_limit+index;
    row->group = info.task ? palette_settings : palette_apps;
    row->can_run_admin = !info.task;
}

static void w_open_palette(enum command_kind kind, int output, const w_popup_anchor *anchor) {
    w_palette_site site = {.monitor=desk.outputs[output].monitor,.follow_theme_motion=true};
    if (anchor) site.anchor=*anchor;
    // A submenu opens after its parent hides. Keep the original application for its actions.
    if (!w_palette_shown() && !dispatching_row) {
        menu_target = GetForegroundWindow();
        menu_pid = 0;
        GetWindowThreadProcessId(menu_target,&menu_pid);
        int slot = w_desktop_find(w_os_app_window(menu_target));
        menu_owner = (w_window_id){slot,slot >= 0 ? desk.model.windows[slot].generation : 0};
    }
    group = palette_flat;
    current_palette = kind;
    palette_output = output;
    row_count = 0;
    const wchar_t *title = L"Menu";
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
    } else if (kind == cmd_clipboard) {
        if (!desk.clipboard_enabled)
            desk.clipboard_enabled = AddClipboardFormatListener(desk.root) != 0;
        w_desktop_copy_clip();
        title = L"Clipboard history";
        memcpy(clip_rows, desk.clips, desk.clip_count * sizeof(clip_rows[0]));
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
        bool run = kind == cmd_menu;
        if (!run) title = L"Settings";
        w_menu_actions(run);
        if (run) {
            w_catalog_begin(desk.root, msg_catalog);
            w_catalog_state *next = w_catalog_acquire();
            int configured = row_count-configured_start;
            for (int i=0; i<configured; ++i) configured_labels[i] = entries[configured_start+i].label;
            qsort(configured_labels,configured,sizeof(*configured_labels),w_compare_label);
            w_catalog_state *previous=snapshot;
            snapshot = next;
            w_palette_apps apps = {.count=w_catalog_count(snapshot),.read=w_installed_row};
            w_palette_open(&site,title,entries,row_count,w_pick,
                           w_catalog_images(snapshot),w_desktop_run_line,&apps);
            w_catalog_release(previous);
            return;
        }
    }
    w_palette_open(&site,title,entries,row_count,w_pick,NULL,NULL,NULL);
    w_catalog_release(snapshot);
    snapshot = NULL;
}

void w_desktop_palette(enum command_kind kind, int output) {
    w_open_palette(kind, output, NULL);
}

void w_desktop_bar_palette(enum command_kind kind, int output, const w_popup_anchor *anchor) {
    const w_popup_anchor *open = w_palette_anchor();
    if (w_palette_shown() && open && current_palette == kind && w_popup_same_source(open,anchor)) {
        w_palette_close();
        return;
    }
    w_open_palette(kind, output, anchor);
}

static void w_refresh_palette(enum command_kind kind) {
    const w_popup_anchor *anchor = w_palette_anchor();
    int output = palette_output;
    if (anchor && (!IsWindowVisible(anchor->bar) || anchor->bar != desk.outputs[output].bar.hwnd)) {
        w_palette_close();
        return;
    }
    if (!desk.outputs[output].live) output = desk.model.output;
    w_open_palette(kind, output, anchor);
}

void w_desktop_catalog_update(void) {
    if (w_palette_visible(L"Menu")) w_refresh_palette(cmd_menu);
}

void w_desktop_clipboard_changed(bool cleared) {
    if (cleared) SecureZeroMemory(clip_rows, sizeof(clip_rows));
    // Keep populated lists stable under the pointer and during keyboard selection.
    if ((cleared || !row_count) && w_palette_visible(L"Clipboard history"))
        w_refresh_palette(cmd_clipboard);
}

void w_desktop_focus_menu(int output) {
    if ((unsigned)output >= output_limit || !desk.outputs[output].live) output = palette_output;
    if (output != palette_output || w_palette_anchor()) {
        palette_output = output;
        w_palette_site site = {.monitor=desk.outputs[output].monitor,.follow_theme_motion=true};
        w_palette_position(&site);
    }
    w_palette_focus();
}
