#pragma once
#include "win_platform/win_platform.h"

////////////////
//~ Icons

typedef enum bar_icon {
    icon_none,
    icon_run,
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
    icon_count
} bar_icon;

typedef struct w_icon_atlas {
    BYTE *coverage;
    DWORD *pixels;
    int size;
} w_icon_atlas;

////////////////
//~ Icons Interface

void w_icons_resize(w_icon_atlas *atlas, int dpi);
void w_icons_free(w_icon_atlas *atlas);
void w_icons_draw(const w_icon_atlas *atlas, HDC dc, bar_icon icon, RECT box, COLORREF ink, COLORREF paper);
