#pragma once
#include "win_platform/win_platform.h"
enum input_drag_event { input_drag_edge, input_drag_inside, input_drag_reposition, input_drag_move,
                        input_drag_end, input_drag_cancel };

////////////////
//~ Input

typedef bool (*input_drag_callback)(enum input_drag_event event, POINT point);

////////////////
//~ Input Interface

void w_input_bind(HWND target, UINT message, UINT hover_message, UINT context_message,
                const w_desktop_config *config, bool *status, input_drag_callback drag);
uint64_t w_input_pointer_press_count(void);
void w_input_hover_point(POINT *out);
void w_input_mouse_motion(void);
void w_input_cancel_pointer(void);
void w_input_mode(bool resize);
void w_input_stop(void);
void w_input_reset(void);
