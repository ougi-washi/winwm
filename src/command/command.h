#pragma once
#include "base/base.h"
#define command_list(x) \
    x(terminal) x(menu) x(switcher) x(shortcuts) x(open_folder) x(reveal_file) x(open_tasks) x(launch) \
    x(focus) x(move) x(workspace_select) x(workspace_send) x(workspace_back) x(output_next) \
    x(output_send) x(split) x(layout) x(resize_mode) x(resize) x(floating) x(fullscreen) x(scratch_send) \
    x(scratch_show) x(close) x(reload) x(settings) x(lock) x(sign_out) x(idle_inhibit) x(capture) \
    x(clipboard) x(clipboard_clear) x(notify) x(notifications) x(volume_up) x(volume_down) x(mute) \
    x(media_play) x(provider_refresh) x(bar_toggle) x(rescue) x(edit_config) x(audio) x(wifi) \
    x(bluetooth) x(display) x(wallpaper) x(sleep) x(restart) x(capture_window) x(capture_region) \
    x(record) x(confirm_sign_out) x(focus_mode_toggle) x(set_mode) x(resize_by) x(program) \
    x(workspace_follow) x(volume_set) x(volume_mixer) x(languages) x(keyboard_settings) \
    x(power_settings) x(date_time) x(network_settings) x(animations_toggle) x(borders_toggle) \
    x(animations_system) x(media_previous) x(media_next) x(media_stop) x(activity) x(open_place) \
    x(project) x(pick_color) x(bar_settings) x(microphone) x(input_methods) x(packages) x(updates) \
    x(google_japanese_input) x(windows_update) x(chocolatey) x(quit) x(windows_security) x(windows_settings) \
    x(update_winwm) x(install_winwm) x(uninstall_winwm)
#define command_enum(name) cmd_##name,
enum command_kind { cmd_invalid, command_list(command_enum) cmd_count };

////////////////
//~ Command

typedef struct w_command {
    enum command_kind kind;
    char arg[1024];
} w_command;

////////////////
//~ Command Interface

const char *w_command_name(enum command_kind kind);
bool w_command_navigates(enum command_kind kind);
bool w_command_parse(const char *text, w_command *out);
bool w_command_delta(const char *args, int *dx, int *dy);
int w_command_direction(const char *name);
