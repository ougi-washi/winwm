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
DWORD w_os_ensure_shell(void);
bool w_os_provider(const char *name, wchar_t *out, int cap);
bool w_os_task_manager(const char *name, wchar_t *out, int cap);
HANDLE w_os_launch(const wchar_t *exe, const wchar_t *arg);
HANDLE w_os_launch_positioned(const wchar_t *exe, const wchar_t *arg, POINT origin);
bool w_os_shell_execute(const wchar_t *file, const wchar_t *parameters, const wchar_t *directory, bool admin);
bool w_os_run_executable(const wchar_t *exe, const wchar_t *argument, const wchar_t *directory, bool admin);
bool w_os_launch_line(const char *command, bool admin, bool allow_shell_targets);
bool w_os_launch_wide_line(wchar_t *line, bool admin);
bool w_os_launch_terminal(const char *name, bool admin);
bool w_os_launch_terminal_at(const char *name, const wchar_t *directory, bool admin);
bool w_os_launch_provider(const char *name, const char *arg, bool admin);
void w_os_mark(const wchar_t *stage);
void w_os_restore_all(void);
void w_os_wait_layer_cleanup(void);
void w_os_remember_stack(void);
bool w_os_remember(HWND w, w_rect *original);
void w_os_restore(HWND w);
bool w_os_shell_hidden(HWND window);
bool w_os_hidden_confirmed(HWND window);
bool w_os_set_visible(HWND w, bool wanted);
void w_os_window_appearance(HWND w, DWORD border, DWORD corner);
bool w_os_window_opacity(HWND w, BYTE alpha, bool retain_layer);
bool w_os_direct_surface(HWND w);
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
