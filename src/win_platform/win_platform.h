#pragma once
#include "os_win32/os_win32.h"
#include "config/config.h"

////////////////
//~ Win Platform

bool w_os_cache_read(w_desktop_config *cfg);
void w_os_cache_write(w_desktop_config *cfg);
void w_os_cache_discard(void);
bool w_os_config_seed(bool migrate);
bool w_os_config_load(w_desktop_config *c, char *error, size_t cap);
