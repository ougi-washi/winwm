#pragma once
#include "win_platform/win_platform.h"

////////////////
//~ Audio

typedef struct w_audio_state {
    bool available, muted;
    int percent;
    wchar_t device[128];
} w_audio_state;

////////////////
//~ Audio Interface

bool w_audio_start(HWND owner, UINT changed);
w_audio_state w_audio_read(void);
bool w_audio_adjust(int delta); /* Zero toggles mute. */
bool w_audio_set_percent(int percent);
void w_audio_stop(void);
