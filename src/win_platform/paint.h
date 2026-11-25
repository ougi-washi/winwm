#pragma once
#include <windows.h>
#include <uxtheme.h>

typedef struct {
    PAINTSTRUCT paint;
    HPAINTBUFFER buffer;
} w_paint_frame;

// Draw a complete animation frame into the invalid region, with the native
// client coordinates preserved. Printing uses the caller's DC directly.
static inline HDC w_begin_frame(HWND window,w_paint_frame *frame) {
    HDC target=BeginPaint(window,&frame->paint),dc=NULL;
    frame->buffer=target ? BeginBufferedPaint(target,&frame->paint.rcPaint,BPBF_COMPATIBLEBITMAP,NULL,&dc) : NULL;
    return frame->buffer ? dc : target;
}

static inline void w_end_frame(HWND window,w_paint_frame *frame) {
    if (frame->buffer) EndBufferedPaint(frame->buffer,TRUE);
    EndPaint(window,&frame->paint);
}
