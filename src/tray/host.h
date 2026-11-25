#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint64_t token;
    HWND owner;
    DWORD pid;
    UINT id;
    GUID guid;
    HICON icon;
    wchar_t tip[128];
    bool hidden;
} w_tray_icon;

typedef struct {
    uint64_t token;
    HWND owner;
    DWORD pid;
    wchar_t tip[128], title[64], body[256];
    UINT flags;
} w_tray_balloon;

enum w_tray_action { w_tray_primary, w_tray_context, w_tray_keyboard, w_tray_double, w_tray_middle };

// Desktop UI thread only. Snapshots borrow their icon until the next message dispatch.
bool w_tray_start(HWND owner, UINT changed_message, bool (*locate)(uint64_t,RECT *));
bool w_tray_is_active(void);
void w_tray_stop(void);
void w_tray_ack(void);
size_t w_tray_count(void);
bool w_tray_get(size_t index, w_tray_icon *out);
bool w_tray_action(uint64_t token, enum w_tray_action action, POINT screen);
void w_tray_prune(HWND owner);
bool w_tray_next_balloon(w_tray_balloon *out);
void w_tray_balloon_event(uint64_t token, UINT event);
