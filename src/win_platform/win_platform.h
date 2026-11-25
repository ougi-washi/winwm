#pragma once
#include "os_win32/os_win32.h"
#include "config/config.h"

////////////////
//~ Win Platform

enum { winwm_packages_changed = WM_APP + 0x435 };

bool w_os_cache_read(w_desktop_config *cfg);
void w_os_cache_write(w_desktop_config *cfg);
void w_os_cache_discard(void);
bool w_os_config_seed(void);
bool w_os_config_load(w_desktop_config *c, char *error, size_t cap);
bool w_os_toolbar_path(wchar_t *path, int cap);
bool w_os_config_load_toolbar(const wchar_t *candidate, w_desktop_config *c, char *error, size_t cap);
