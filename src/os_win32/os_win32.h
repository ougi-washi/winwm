#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "base/base.h"
enum { os_path_cap = 4096 };
typedef enum { os_host_instance, os_interactive_instance } os_instance_kind;

////////////////
//~ Os Win32

typedef struct w_os_instance_lock {
    HANDLE current, legacy;
} w_os_instance_lock;

////////////////
//~ Os Win32 Interface

DWORD w_os_instance_claim(os_instance_kind kind, w_os_instance_lock *lock);
DWORD w_os_instance_status(os_instance_kind kind);
void w_os_instance_release(w_os_instance_lock *lock);
bool w_os_wide(const char *s, wchar_t *out, int cap);
bool w_os_utf8(const wchar_t *s, char *out, int cap);
bool w_os_data_path(wchar_t *out, int cap, const wchar_t *name, bool create);
bool w_os_legacy_config_path(wchar_t *out, int cap);
bool w_os_config_path(wchar_t *out, int cap);
bool w_os_self(wchar_t *out, int cap);
DWORD w_os_exit_explorer(void);
bool w_os_provider(const char *name, wchar_t *out, int cap);
bool w_os_task_manager(const char *name, wchar_t *out, int cap);
HANDLE w_os_launch(const wchar_t *exe, const wchar_t *arg);
bool w_os_launch_line(const char *command);
bool w_os_launch_terminal(const char *name);
bool w_os_launch_provider(const char *name, const char *arg);
void w_os_mark(const wchar_t *stage);
void w_os_area_open(bool host);
void w_os_area_apply(HMONITOR monitor, w_rect usable);
void w_os_area_restore(void);
void w_os_area_close(void);
void w_os_restore_all(void);
bool w_os_remember(HWND w);
void w_os_restore(HWND w);
bool w_os_shell_hidden(HWND window);
bool w_os_set_visible(HWND w, bool wanted);
void w_os_window_appearance(HWND w, DWORD border, DWORD corner);
bool w_os_window_opacity(HWND w, BYTE alpha, bool retain_layer);
w_rect w_os_bounds(HWND w);
RECT w_os_to_rect(w_rect r);
bool w_os_same_process(HWND w);
bool w_os_desktop_surface(HWND window);
bool w_os_candidate(HWND w);
HWND w_os_app_window(HWND w);
bool w_os_cloaked(HWND w);
bool w_os_keyboard_name(HKEY layouts, const wchar_t *key, wchar_t *name, DWORD count);
int w_os_error(wchar_t *dst, int cap);
void w_os_modifier_keys(bool mapped[2]);
DWORD w_os_modifier_map(bool install);
