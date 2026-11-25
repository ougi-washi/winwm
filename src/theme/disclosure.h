#pragma once
#include "theme/surface.h"

// A section keeps its native controls while the owner lays them out at
// value/255 of their full extent. The owner supplies one timer ID per section.
typedef struct {
    int value, from, target, full_ms;
    ULONGLONG start;
    w_transition timing;
    bool running;
} w_disclosure;

bool w_disclosure_to(HWND owner,w_disclosure *section,UINT_PTR timer,bool open);
bool w_disclosure_tick(HWND owner,w_disclosure *section,UINT_PTR timer);
void w_disclosure_stop(HWND owner,w_disclosure *section,UINT_PTR timer,bool open);
// Clip a native child in window coordinates. NULL restores its full paint and hit area.
void w_disclosure_clip(HWND child,const RECT *visible);
