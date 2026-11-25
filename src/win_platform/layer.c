#include "win_platform/layer.h"
#include "win_platform/material.h"
#include <stdlib.h>

bool w_layer_fade_tick(HWND hwnd, w_layer_fade *fade, bool enabled) {
    uint64_t elapsed = GetTickCount64()-fade->start;
    bool running = enabled && fade->start && elapsed < (unsigned)fade->timing.ms;
    double t = running ? w_motion_progress(fade->timing, elapsed) : 1;
    fade->value = (BYTE)w_clamp_int((int)(fade->from+(fade->target-fade->from)*t+.5), 0, 255);
    bool applied=w_material_alpha(hwnd,fade->value);
    if (applied && running) return true;
    if (!applied && fade->value!=fade->target) w_material_alpha(hwnd,fade->target);
    KillTimer(hwnd, layer_fade_timer);
    fade->start = 0;
    fade->value = fade->target;
    return false;
}

bool w_layer_fade_to(HWND hwnd, w_layer_fade *fade, BYTE target, w_transition timing, bool enabled) {
    if (enabled && fade->start && fade->target==target &&
        fade->full_ms==timing.ms && !memcmp(&fade->timing.curve,&timing.curve,sizeof(timing.curve)))
        return w_layer_fade_tick(hwnd,fade,true);
    fade->target = target;
    fade->from = fade->value;
    fade->full_ms = timing.ms;
    timing.ms = MulDiv(timing.ms,abs((int)target-fade->value),255);
    fade->timing = timing;
    fade->start = enabled && timing.ms && (fade->start || SetTimer(hwnd, layer_fade_timer, 16, NULL))
                  ? GetTickCount64() : 0;
    return w_layer_fade_tick(hwnd, fade, enabled);
}
