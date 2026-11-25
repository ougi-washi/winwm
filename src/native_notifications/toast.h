#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>

#define WINWM_NOTIFICATION_AUMID L"winwm.Portable.Desktop"

// Only the serial publication worker calls submit/done. Windows may report
// successful submission before the listener receives (or suppresses) the toast.
bool w_notification_submit(const wchar_t *title, const wchar_t *body);
void w_notification_submit_done(void);
void w_notification_submit_stop(void);
void w_notification_submit_target(HWND window, UINT message);
void w_notification_submit_recover(DWORD pid, uint64_t birth);
