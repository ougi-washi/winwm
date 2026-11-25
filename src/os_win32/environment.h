#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>

// Start/stop from the owner thread. A nonzero message reports the initial scan
// and subsequent process-environment changes on the supplied window.
bool w_environment_start(HWND window, UINT message);
void w_environment_refresh(void);
void w_environment_stop(void);
// Call on launch workers before resolving executable paths or spawning a child.
void w_environment_for_launch(void);
const wchar_t *w_environment_path_end(const wchar_t *start);
