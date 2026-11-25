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
    bool floating, exclude, sets_floating;
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

enum w_module_display { module_auto, module_icon, module_text, module_icon_text };

typedef struct w_module_style {
    char name[56], format[128], tooltip[192], action[128], scroll_up[128], scroll_down[128];
    char right_click[128], middle_click[128];
    enum w_module_display display;
    uint32_t color;
    bool colored, hide_tooltip;
} w_module_style;

typedef struct w_startup_entry {
    char command_line[1024];
    bool once;
} w_startup_entry;

enum motion_kind {
    motion_open, motion_move, motion_workspace, motion_border, motion_modules, motion_layers_in,
    motion_layers_out, motion_fade_in, motion_fade_switch, motion_count
};
enum motion_preference { motion_off, motion_on, motion_system };
typedef struct w_curve { double x1, y1, x2, y2; } w_curve;
enum motion_style { style_default, style_popin, style_slide, style_slidevert, style_top, style_bottom, style_left, style_right };
typedef struct w_transition { int ms; w_curve curve; enum motion_style style; int percent; } w_transition;
double w_motion_progress(w_transition timing, uint64_t elapsed);
typedef struct w_border_gradient { uint32_t end; int angle; bool enabled; } w_border_gradient;

enum w_mouse_action { mouse_unbound, mouse_move, mouse_resize, mouse_inherit };

enum w_notice_corner { notice_top_right, notice_top_left, notice_bottom_right, notice_bottom_left };
enum w_capture_source { notice_capture_off, notice_capture_windows, notice_capture_visible };
enum { notice_history_max = 512, notice_visible_max = 8 };
typedef struct w_notification_config {
    bool enabled;
    enum w_capture_source capture;
    int corner, duration_ms, fade_ms, history_limit, visible_limit, margin, width;
} w_notification_config;

typedef struct w_panel_material {
    int opacity;
    bool blur, shadow;
} w_panel_material;

enum { button_size_min = 24, button_size_default = 32, button_size_max = 48,
       panel_spacing_min = 2, panel_spacing_default = 6, panel_spacing_max = 12 };
typedef struct w_panel_metrics {
    int button_size, spacing;
} w_panel_metrics;

typedef struct w_desktop_config {
    w_notification_config notifications;
    w_panel_material panel_material;
    w_panel_metrics panel_metrics;
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
    int bar_tooltip_delay, bar_tooltip_duration, bar_workspace_count;
    int border_size, rounding;
    enum motion_preference animations;
    w_transition motion[motion_count];
    w_border_gradient active_gradient, inactive_gradient;
    unsigned char inactive_alpha;
    uint32_t inactive_border;
    bool follow_mouse, master_default, resize_on_border;
    unsigned char mouse_actions[2][16]; // Left/right button, then modifier mask.
    uint32_t background, foreground, accent;
} w_desktop_config;

////////////////
//~ Config Interface

bool w_config_has_module(const w_desktop_config *config, const char *module);
bool w_config_modules_valid(const char *text);
const char *w_config_module_name(int index);
bool w_config_set_modules(char *out, size_t cap, const char *spec);
w_module_style *w_config_module_style(w_desktop_config *config, const char *name);
bool w_config_style_option(w_module_style *style, const char *key, const char *value);
bool w_config_bind(w_desktop_config *cfg, const char *chord, const char *action);
void w_config_defaults(w_desktop_config *cfg);
bool w_config_chord(const char *text, unsigned *mods, unsigned *key);
bool w_config_validate_providers(const w_desktop_config *c, char *error, size_t cap);
bool w_config_notifications_valid(const w_notification_config *config);
