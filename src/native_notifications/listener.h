#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>

enum w_notification_access {
    notification_unspecified, notification_allowed, notification_denied, notification_unavailable
};

typedef struct {
    DWORD id;
    INT64 created; // UTC, 100 ns since 1601.
    wchar_t app[128], title[256], body[4096];
    bool toast, from_winwm;
} w_native_notification;

typedef struct {
    enum w_notification_access access;
    HRESULT error;
    size_t count;
    w_native_notification entries[]; // Oldest first. Initial history is silent.
} w_notification_snapshot;

// Owner UI thread. The receiver frees each snapshot delivered in LPARAM.
bool w_notification_listen(HWND target, UINT message);
HRESULT w_notification_request(HWND owner);
void w_notification_refresh(void);
void w_notification_stop(void);
