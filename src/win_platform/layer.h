#pragma once
#include <windows.h>
#include "config/config.h"

enum { layer_fade_timer = 0x574d };
typedef struct w_layer_fade {
    uint64_t start;
    BYTE from, value, target;
    int full_ms;
    w_transition timing;
} w_layer_fade;

void w_layer_present(HWND hwnd, w_layer_fade *fade);
bool w_layer_fade_to(HWND hwnd, w_layer_fade *fade, BYTE target, w_transition timing, bool enabled);
bool w_layer_fade_tick(HWND hwnd, w_layer_fade *fade, bool enabled);
