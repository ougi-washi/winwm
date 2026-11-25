#pragma once
#include "bar/bar.h"

enum { bar_access_activate = WM_APP + 190, bar_access_event };
LRESULT w_bar_access_object(w_bar_window *bar, WPARAM request);
void w_bar_access_layout(w_bar_window *bar);
void w_bar_access_activate(w_bar_window *bar, LONG child);
void w_bar_access_notify(w_bar_window *bar, DWORD event, LONG child);
void w_bar_access_close(w_bar_window *bar);
