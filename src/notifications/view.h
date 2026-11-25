#pragma once
#include "config/config.h"
#include "win_platform/popup.h"

typedef struct w_notice_item {
    DWORD native_id;
    FILETIME time;
    const wchar_t *app, *title, *body;
    bool has_native_id, await_echo, own_native, confirmed_receipt;
    uint64_t balloon;
    void (*balloon_event)(uint64_t,UINT);
} w_notice_item;

// Call on the desktop thread. The view copies incoming text.
void w_notice_configure(HWND owner, const w_notification_config *config, bool motion_allowed);
void w_notice_work_area(bool (*read)(HMONITOR, RECT *));
void w_notice_access(const wchar_t *status, void (*request)(HWND button));
void w_notice_add(const w_notice_item *item, bool toast);
void w_notice_cancel_balloon(uint64_t token);
void w_notice_batch(bool begin);
void w_notice_open(HMONITOR monitor, const w_popup_anchor *anchor);
bool w_notice_close(void);
bool w_notice_visible(void);
bool w_notice_contains(HWND hit);
bool w_notice_message(MSG *msg);
void w_notice_outside(HWND hit);
void w_notice_reposition(void);
int w_notice_count(void);
void w_notice_shutdown(void);
const w_popup_anchor *w_notice_anchor(void);
void w_notice_reanchor(const w_popup_anchor *anchor);
