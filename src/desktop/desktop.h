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
    msg_shell_exit
};

typedef struct w_output_state {
    HMONITOR monitor;
    wchar_t device[32];
    bool live, bar_hidden;
    w_rect screen, work;
    int dpi;
    w_bar_window bar;
} w_output_state;
enum float_update { float_move = 1, float_size = 2 };

typedef struct w_window_state {
    HWND hwnd;
    DWORD pid;
    int displaced_from;
    w_rect issued, float_before_request;
    unsigned floating_update;
    bool float_request_sent;
    enum { move_idle, move_dragging, move_released } native_move;
    bool has_issued, restoring, wants_visible, decorated, active, custom_border;
    BYTE alpha;
    DWORD border, applied_border;
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
    HWINEVENTHOOK hooks[6];
    char status[512];
    wchar_t notices[32][256], clips[24][512];
    int notice_count, clip_count;
    DWORD clipboard_seq;
    bool clipboard_enabled;
} w_desktop_state;

extern w_desktop_state desk;

////////////////
//~ Desktop Interface

void w_desktop_schedule(void);
void w_desktop_scan(void);
void w_desktop_event(HWND hwnd, DWORD event);
void w_desktop_outputs(void);
void w_desktop_select(int slot);
void w_desktop_move_window(int slot, int output, int workspace);
void w_desktop_toggle_fullscreen(int slot);
void w_desktop_cycle_output(bool carry);
void w_desktop_navigate(enum direction dir, bool move);
void w_desktop_resize_axis(enum split_axis axis, int delta);
void w_desktop_resize_direction(int direction);
void w_desktop_place_floating(int slot, w_rect bounds);
void w_desktop_apply(void);
void w_desktop_bind(void);
bool w_desktop_defer(const w_command *cmd, int output);
bool w_desktop_run_on(const w_command *cmd, int output);
bool w_desktop_run(const w_command *cmd);
void w_desktop_action(int output, const char *text);
void w_desktop_palette(enum command_kind kind, int output);
void w_desktop_notice(const char *s);
void w_desktop_load(void);
void w_desktop_start_programs(bool first_load);
void w_desktop_copy_clip(void);
void w_desktop_capture(void);
int w_desktop_find(HWND w);
void w_desktop_activate(int slot);
void w_desktop_hover(POINT p);
void w_desktop_focus_cancel(void);
void w_desktop_focus_empty(void);
bool w_desktop_close_foreground(void);
void w_desktop_focus_apply(void);
void w_desktop_focus_observe(HWND window);
void w_desktop_request(const char *s, char *reply, size_t cap);
void w_desktop_timer(void);
void w_desktop_status_sample(void);
void w_desktop_network_changed(void);
void w_desktop_status_stop(void);
void w_desktop_status_done(void *result);
void w_desktop_status_configure(void);
bool w_desktop_status_text(const char *key, wchar_t *s, int cap, char *action, size_t acap);
bool w_desktop_drag(enum input_drag_event event, POINT point);
