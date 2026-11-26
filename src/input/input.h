#pragma once
#include "config/config.h"
#include "os_win32/os_win32.h"
enum input_drag_event { input_drag_edge, input_drag_inside, input_drag_reposition, input_drag_move,
                        input_drag_end, input_drag_cancel, input_drag_commit };

////////////////
//~ Input

typedef bool (*input_drag_callback)(enum input_drag_event event, POINT point);

////////////////
//~ Input Interface

DWORD w_input_bind(HWND target, UINT message, UINT hover_message, UINT press_message,
                UINT gesture_message, const w_desktop_config *config, bool *status, input_drag_callback drag);
enum { input_health_timer = 32 };
void w_input_check(bool *binding_status);
void w_input_diagnostics(char *output, size_t cap);
HANDLE w_input_thread(void);
void w_input_pointer_message(WPARAM request, LPARAM position);
uint64_t w_input_pointer_press_count(void);
uint64_t w_input_press_count(void);
void w_input_hover_point(POINT *out);
void w_input_cancel_pointer(void);
bool w_input_send_media_key(WORD key);
void w_input_mode(bool resize);
void w_input_stop(void);
void w_input_reset(void);
