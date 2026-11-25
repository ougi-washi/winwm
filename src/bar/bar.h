#pragma once
#include "bar/icons.h"
#include "win_platform/layer.h"
#include "win_platform/popup.h"

////////////////
//~ Bar

typedef struct w_bar_view {
    int selected, occupied;
    bool resize_mode, inhibit;
    wchar_t title[256];
} w_bar_view;

typedef void (*bar_action)(int output, const char *action, const w_popup_anchor *anchor);
typedef bool (*bar_custom)(const char *name, wchar_t *text, int cap, char *action, size_t action_cap);
typedef struct w_bar_tray_item {
    uint64_t key;
    HICON icon;
    wchar_t tooltip[256];
} w_bar_tray_item;
enum { bar_modules_timer = layer_fade_timer + 1, bar_tip_show_timer, bar_tip_hide_timer, bar_tray_timer };
enum { bar_tray_slots = 8 };
typedef struct w_bar_window {
    HWND hwnd;
    struct w_bar_access *access;
    int output, dpi;
    HFONT font, heading_font;
    w_icon_atlas icons;
    bar_icon network_icon;
    HDC buffer_dc;
    HBITMAP buffer, previous_bitmap;
    SIZE buffer_size;
    int hover, wheel_hit, wheel_remainder, audio_percent;
    bool calendar_date, audio_muted;
    bool motion_allowed, modules_ticking, pointer_inside, high_contrast;
    POINT pointer;
    struct w_bar_tip {
        HWND window;
        int index, workspace;
        char instance[56];
        wchar_t heading[256], body[512], help[320];
        HKL keyboard_layout;
        wchar_t keyboard_name[208];
        RECT parts[3];
        POINT anchor;
        ULONGLONG deadline;
        bool pending, showing;
    } tip;
    w_layer_fade fade;
    POINT press;
    char press_instance[56];
    int press_workspace;
    w_rect screen;
    const char *docked_edge;
    bool pressed, dragging;
    void (*dock)(int output);
    const w_desktop_config *config;
    w_bar_view view;
    struct w_bar_hit {
        RECT box;
        char action[128], module[56], instance[56];
        wchar_t tooltip[256], text[256];
        const w_module_style *style;
        int workspace, source;
        bar_icon icon;
        uint64_t tray_key;
        int tray_index;
        bool tray_more, tray_hide_tip;
        bool active, empty, label, painted;
        uint64_t start;
        COLORREF paper, ink, from_paper, from_ink, target_paper, target_ink;
    } *hits;
    struct w_bar_cell *cells;
    int hit_count, cell_capacity;
    bar_action invoke;
    bar_custom custom;
    bool (*detail)(const char *module, wchar_t *text, int cap);
    int (*tray_count)(void);
    bool (*tray_read)(int index, w_bar_tray_item *item);
    void (*tray_activate)(uint64_t key, UINT event, POINT screen_point);
    struct {
        int source, suppressed, first, page, total;
        bool open;
        double position, from;
        ULONGLONG start;
        uint64_t context_key;
        UINT context_button;
        RECT strip;
    } tray;
} w_bar_window;

////////////////
//~ Bar Interface

bool w_bar_create(w_bar_window *b, int output, const w_desktop_config *config, bar_action invoke);
w_rect w_bar_position(w_bar_window *b, w_rect screen, w_rect available, int dpi, bool hidden);
void w_bar_invalidate(w_bar_window *b, const char *module);
void w_bar_set_audio(w_bar_window *b, int percent, bool muted);
void w_bar_update(w_bar_window *b, const w_bar_view *v);
bool w_bar_activate_hit(w_bar_window *bar, int hit);
bool w_bar_anchor(const w_bar_window *bar, int hit, w_popup_anchor *anchor);
bool w_bar_tray_bounds(const w_bar_window *bar, uint64_t key, RECT *screen);
void w_bar_invoke(w_bar_window *bar, int hit, const char *action);

bool w_bar_menu_active(void);
bool w_bar_menu_close(const w_bar_window *bar, bool restore);
void w_bar_menu_outside(HWND hit);
void w_bar_menu_audio_error(void);
