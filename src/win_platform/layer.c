#include "win_platform/layer.h"
#include "win_platform/material.h"

bool w_layer_fade_tick(HWND hwnd, w_layer_fade *fade, bool enabled) {
    uint64_t elapsed = GetTickCount64()-fade->start;
    bool running = enabled && fade->start && elapsed < (unsigned)fade->timing.ms;
    double t = running ? w_motion_progress(fade->timing, elapsed) : 1;
    fade->value = (BYTE)w_clamp_int((int)(fade->from+(fade->target-fade->from)*t+.5), 0, 255);
    if (w_material_alpha(hwnd,fade->value) && running)
        return true;
    KillTimer(hwnd, layer_fade_timer);
    fade->start = 0;
    fade->value = fade->target;
    return false;
}

bool w_layer_fade_to(HWND hwnd, w_layer_fade *fade, BYTE target, w_transition timing, bool enabled) {
    fade->target = target;
    fade->from = fade->value;
    fade->timing = timing;
    fade->start = enabled && timing.ms && fade->value != target && SetTimer(hwnd, layer_fade_timer, 16, NULL)
                  ? GetTickCount64() : 0;
    return w_layer_fade_tick(hwnd, fade, enabled);
}
