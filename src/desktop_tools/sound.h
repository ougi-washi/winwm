#pragma once
#include "win_platform/win_platform.h"

void w_sound_refresh(HWND list, bool sessions);
void w_sound_clear(void);
bool w_sound_state(int index, int *percent, BOOL *muted);
bool w_sound_change(int index, int percent);
bool w_sound_can_default(int index);
bool w_sound_make_default(int index);
