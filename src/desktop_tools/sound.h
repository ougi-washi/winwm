#pragma once
#include "win_platform/win_platform.h"

enum w_sound_view { sound_outputs, sound_inputs, sound_apps, sound_view_count };
void w_sound_refresh(HWND list, enum w_sound_view view);
bool w_sound_watch(void);
bool w_sound_inventory_changed(void);
void w_sound_stop(void);
bool w_sound_state(int index, int *percent, BOOL *muted);
bool w_sound_change(int index, int percent);
bool w_sound_can_default(int index, bool calls);
bool w_sound_make_default(int index, bool calls);
