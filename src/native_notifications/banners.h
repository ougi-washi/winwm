#pragma once
#include <windows.h>
#include <stdbool.h>

typedef struct {
    FILETIME time;
    wchar_t app[256], title[1024], body[4096];
} w_banner;

// Desktop thread. LPARAM owns a malloc-allocated banner, or NULL for startup
// status with HRESULT in WPARAM. Stop discards deliveries still in the queue.
bool w_banners_start(HWND receiver, UINT message);
void w_banners_stop(void);
