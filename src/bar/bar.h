#pragma once
#include "bar/icons.h"

////////////////
//~ Bar

typedef struct w_bar_view {
    int selected, occupied;
    bool resize_mode, inhibit;
    wchar_t title[256];
} w_bar_view;

typedef void (*bar_action)(int output, const char *action);
typedef bool (*bar_custom)(const char *name, wchar_t *text, int cap, char *action, size_t action_cap);
typedef struct w_bar_window {
    HWND hwnd;
    int output, dpi;
    HFONT font;
    w_icon_atlas icons;
    HDC buffer_dc;
    HBITMAP buffer, previous_bitmap;
    SIZE buffer_size;
    int hover, wheel_remainder, audio_percent;
    bool calendar_date, audio_muted;
    POINT press;
    w_rect screen;
    const char *docked_edge;
    bool pressed, dragging;
    void (*dock)(int output);
    const w_desktop_config *config;
    w_bar_view view;
    struct w_bar_hit {
        RECT box;
        char action[128], module[56];
        wchar_t tooltip[256];
        const w_module_style *style;
    } hits[48];
    int hit_count;
    bar_action invoke;
    bar_custom custom;
} w_bar_window;

////////////////
//~ Bar Interface

bool w_bar_create(w_bar_window *b, int output, const w_desktop_config *config, bar_action invoke);
w_rect w_bar_position(w_bar_window *b, w_rect screen, int dpi, bool hidden);
void w_bar_invalidate(w_bar_window *b, const char *module);
void w_bar_set_audio(w_bar_window *b, int percent, bool muted);
void w_bar_update(w_bar_window *b, const w_bar_view *v);

bool w_bar_menu_active(void);
void w_bar_menu_close(const w_bar_window *bar, bool restore);
void w_bar_menu_audio_error(void);
