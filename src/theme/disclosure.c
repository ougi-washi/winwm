#include "theme/disclosure.h"
#include <stdlib.h>

void w_disclosure_clip(HWND child,const RECT *visible) {
    RECT previous;
    int kind=GetWindowRgnBox(child,&previous);
    if (!visible) {
        if (kind!=ERROR) SetWindowRgn(child,NULL,TRUE);
    } else if (kind==ERROR || (kind==NULLREGION ? !IsRectEmpty(visible) : !EqualRect(&previous,visible))) {
        HRGN clip=CreateRectRgnIndirect(visible);
        if (clip && !SetWindowRgn(child,clip,TRUE)) DeleteObject(clip);
    }
}

void w_disclosure_stop(HWND owner,w_disclosure *section,UINT_PTR timer,bool open) {
    if (section->running) KillTimer(owner,timer);
    section->value=section->target=open ? 255 : 0;
    section->running=false;
}

bool w_disclosure_tick(HWND owner,w_disclosure *section,UINT_PTR timer) {
    if (!section->running) return false;
    ULONGLONG elapsed=GetTickCount64()-section->start;
    if (elapsed>=(unsigned)section->timing.ms || !IsWindowVisible(owner)) {
        w_disclosure_stop(owner,section,timer,section->target!=0);
        return false;
    }
    double t=w_motion_progress(section->timing,elapsed);
    section->value=w_clamp_int((int)(section->from+(section->target-section->from)*t+.5),0,255);
    return true;
}

bool w_disclosure_to(HWND owner,w_disclosure *section,UINT_PTR timer,bool open) {
    int target=open ? 255 : 0;
    if (!section->running && section->value==target) return false;
    w_transition timing=w_theme_layer_timing(open);
    if (!timing.ms || !IsWindowVisible(owner) || !w_theme_motion_enabled()) {
        w_disclosure_stop(owner,section,timer,open);
        return false;
    }
    if (section->running && target==section->target && section->full_ms==timing.ms &&
        !memcmp(&timing.curve,&section->timing.curve,sizeof(timing.curve)))
        return true;
    section->target=target;
    section->from=section->value;
    section->full_ms=timing.ms;
    timing.ms=MulDiv(timing.ms,abs(target-section->from),255);
    if (!timing.ms || (!section->running && !SetTimer(owner,timer,16,NULL))) {
        w_disclosure_stop(owner,section,timer,open);
        return false;
    }
    section->timing=timing;
    section->start=GetTickCount64();
    section->running=true;
    return true;
}
