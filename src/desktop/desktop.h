#pragma once
#include "layout/layout.h"
#include "bar/bar.h"
#include "palette/palette.h"
#include "ipc/ipc.h"
#include "providers/providers.h"
#include "input/input.h"
#include "catalog/catalog.h"

////////////////
//~ Desktop

enum {
    msg_refresh = WM_APP + 1,
    msg_command,
    msg_load,
    msg_window,
    msg_devices,
    msg_status,
    msg_init,
    msg_sample,
    msg_network,
    msg_audio,
    msg_hover,
    msg_desktop_menu,
    msg_catalog,
    msg_pointer_select, msg_drag_preview, msg_drag_limits, msg_run_result
};

typedef struct w_output_state {
    HMONITOR monitor;
    wchar_t device[32];
    bool live, bar_hidden;
    w_rect screen, system_work, work;
    int dpi;
    w_bar_window bar;
    HWND tabs;
    w_tab_layout tab_layout;
} w_output_state;
enum float_update { float_move = 1, float_size = 2 };

typedef struct w_window_state {
    HWND hwnd;
    DWORD pid;
    wchar_t tab_title[256];
    unsigned tab_title_revision;
    bool tab_title_dirty;
    ULONGLONG workspace_hide_tick;
    int displaced_from;
    w_rect issued, issued_native, rejected_bounds, float_before_request, drag_start;
    w_rect saved_float_work;
    int saved_float_dpi;
    RECT frame_margins;
    LONG_PTR frame_style, frame_exstyle;
    UINT frame_dpi;
    unsigned floating_update;
    bool float_request_sent;
    enum { move_idle, move_dragging, move_released } native_move;
    bool has_issued, restoring, wants_visible, externally_shown, decorated, active, custom_border;
    BYTE alpha, corner;
    struct {
        ULONGLONG start;
        BYTE from, value, target;
        w_transition timing;
    } opacity;
    DWORD border, applied_border;
    struct {
        ULONGLONG start;
        w_transition timing;
        w_rect from, target, last;
    } motion;
    struct {
        HWND hwnd;
        w_rect bounds;
        int width, radius, angle;
        uint32_t color[2], from[2], target[2];
        w_transition timing;
        ULONGLONG start;
        bool initialized;
    } outline;
    bool presented, direct_surface;
} w_window_state;

typedef struct w_desktop_state {
    HWND root;
    w_desktop_model model;
    w_desktop_config config;
    w_output_state outputs[output_limit];
    w_window_state windows[window_limit];
    w_ipc_channel channels[ipc_slots];
    bool ipc_live, resize, inhibit, hide_bars, refresh_pending, ready, first_input, loading;
    bool registered[binding_limit];
    uint64_t revision;
    HWINEVENTHOOK hooks[7];
    char status[512];
    wchar_t notices[32][256], clips[24][4096];
    int notice_count, clip_count;
    DWORD clipboard_seq;
    bool clipboard_enabled;
    bool motion_allowed, high_contrast, borders_hidden;
} w_desktop_state;

extern w_desktop_state desk;

////////////////
//~ Desktop Interface

void w_desktop_schedule(void);
void w_desktop_scan(bool focus_new);
void w_desktop_event(HWND hwnd, DWORD event, DWORD event_tick);
void w_desktop_outputs(void);
void w_desktop_measure_output(int output);
HWND w_desktop_tabs_update(int output, const int *slots, int count, int selected, w_rect bounds);
int w_desktop_output_for_bounds(int current, w_rect r);
void w_desktop_select(int slot);
void w_desktop_move_window(int slot, int output, int workspace);
void w_desktop_drop_tile(int slot, POINT point);
bool w_desktop_tile_drop(int slot, POINT point, w_rect *preview, bool commit);
void w_desktop_toggle_fullscreen(int slot);
void w_desktop_cycle_output(bool carry);
void w_desktop_navigate(enum direction dir, bool move);
void w_desktop_resize_axis(enum split_axis axis, int delta);
void w_desktop_resize_direction(int direction);
void w_desktop_place_floating(int slot, w_rect bounds);
void w_desktop_set_floating(int slot, bool floating);
void w_desktop_apply(void);
enum { animation_timer = 30 };
void w_effects_preferences(void);
void w_effects_tick(void);
void w_effects_opacity(int slot, bool focused, bool entering);
bool w_effects_move(int slot, w_rect target, enum motion_kind kind, int direction);
bool w_outline_sync(int slot, bool focused);
void w_outline_position(int slot);
void w_outline_hide(int slot);
void w_effects_hide(int slot);
void w_desktop_bind(void);
bool w_desktop_defer(const w_command *cmd, int output);
bool w_desktop_run_on(const w_command *cmd, int output, bool admin);
bool w_desktop_can_run_admin(enum command_kind kind);
bool w_desktop_run(const w_command *cmd);
void w_desktop_action(int output, const char *text);
void w_desktop_palette(enum command_kind kind, int output);
void w_desktop_focus_menu(int output);
void w_desktop_catalog_update(void);
void w_desktop_clear_clips(void);
void w_desktop_notice(const char *s);
void w_desktop_load(void);
void w_desktop_start_programs(bool first_load);
void w_desktop_copy_clip(void);
void w_desktop_capture(int output);
bool w_desktop_capture_window(HWND target, DWORD pid);
int w_desktop_find(HWND w);
void w_desktop_activate(int slot);
void w_desktop_hover(POINT p);
void w_desktop_focus_cancel(void);
bool w_desktop_focus_new(w_window_id id);
void w_desktop_focus_empty(void);
bool w_desktop_close_foreground(void);
bool w_desktop_close_target(HWND window, DWORD pid, w_window_id owner);
void w_desktop_focus_apply(void);
void w_desktop_focus_observe(HWND window);
void w_desktop_stack_floats(int slot, bool fullscreen_changed);
void w_desktop_request(const char *s, char *reply, size_t cap);
void w_desktop_timer(void);
void w_desktop_status_sample(void);
void w_desktop_network_changed(void);
void w_desktop_status_stop(void);
void w_desktop_status_done(void *result);
void w_desktop_status_configure(void);
bool w_desktop_status_text(const char *key, wchar_t *s, int cap, char *action, size_t acap);
bool w_desktop_status_detail(const char *key, wchar_t *s, int cap);
bool w_desktop_drag(enum input_drag_event event, POINT point);
bool w_desktop_resizing(void);
bool w_desktop_dragging_float(int slot);
void w_desktop_drag_client_resized(int slot);
void w_desktop_drag_limits(w_window_id id);
void w_desktop_cancel_drag(void);
void w_desktop_drag_preview(void);
