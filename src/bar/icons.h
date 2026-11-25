#pragma once
#include "win_platform/win_platform.h"

////////////////
//~ Icons

typedef enum bar_icon {
    icon_none,
    icon_menu,
    icon_bluetooth,
    icon_network,
    icon_audio,
    icon_muted,
    icon_battery,
    icon_memory,
    icon_play,
    icon_quiet,
    icon_plug,
    icon_ethernet,
    icon_offline,
    icon_cpu,
    icon_keyboard,
    icon_clock,
    icon_traffic,
    icon_display,
    icon_project,
    icon_clipboard,
    icon_bell,
    icon_resize,
    icon_awake,
    icon_left, icon_right, icon_up, icon_down, icon_more,
    icon_count
} bar_icon;

typedef struct w_icon_atlas {
    BYTE *coverage;
    DWORD *pixels;
    HDC source;
    HBITMAP image;
    COLORREF tint[icon_count];
    int size;
} w_icon_atlas;

////////////////
//~ Icons Interface

void w_icons_resize(w_icon_atlas *atlas, int pixels);
void w_icons_free(w_icon_atlas *atlas);
void w_icons_draw(w_icon_atlas *atlas, HDC dc, bar_icon icon, RECT box, COLORREF ink);
