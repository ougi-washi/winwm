#pragma once
#include "command/command.h"
enum { binding_limit = 160, rule_limit = 64, app_limit = 64 };

////////////////
//~ Config

typedef struct w_key_binding {
    unsigned mods, key, next_mods, next_key;
    bool release, repeat, modal;
    w_command action;
    char chord[96];
} w_key_binding;

typedef struct w_window_rule {
    char class_name[128], title[128], executable[256];
    int workspace, output, layout;
    uint32_t border;
    bool colored;
    bool floating, exclude, exact_match, sets_floating;
} w_window_rule;

typedef struct w_app_entry {
    char name[80], path[1024];
} w_app_entry;
enum { provider_limit = 16 };

typedef struct w_provider_config {
    char name[40], executable[1024], action[128];
    int interval, timeout;
    bool stream;
} w_provider_config;

typedef struct w_module_style {
    char name[56], format[128], tooltip[192], action[128], scroll_up[128], scroll_down[128];
    uint32_t color;
    bool colored;
} w_module_style;

typedef struct w_startup_entry {
    char command_line[1024];
    bool once;
} w_startup_entry;

typedef struct w_desktop_config {
    w_startup_entry startup[32];
    int startup_count;
    w_module_style styles[48];
    int style_count;
    w_provider_config providers[provider_limit];
    int provider_count;
    w_key_binding bindings[binding_limit];
    int binding_count;
    w_window_rule rules[rule_limit];
    int rule_count;
    w_app_entry apps[app_limit];
    int app_count;
    char file_browser[1024], task_manager[1024], terminal[1024], editor[1024];
    char bar_left[128], bar_center[128], bar_right[128], edge[12];
    char workspace_names[workspace_limit][40];
    int gap, outer_gap, bar_height;
    unsigned char inactive_alpha;
    uint32_t inactive_border;
    bool follow_mouse, master_default;
    uint32_t background, foreground, accent;
} w_desktop_config;

////////////////
//~ Config Interface

bool w_config_has_module(const w_desktop_config *config, const char *module);
bool w_config_modules_valid(const char *text);
bool w_config_bind(w_desktop_config *cfg, const char *chord, const char *action);
void w_config_defaults(w_desktop_config *cfg);
bool w_config_chord(const char *text, unsigned *mods, unsigned *key);
bool w_config_parse(w_desktop_config *out, const char *source, char *error, size_t cap);
