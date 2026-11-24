#pragma once
#include "win_platform/win_platform.h"

////////////////
//~ Providers

typedef struct w_provider_value {
    wchar_t text[192], tooltip[256];
    char class_name[48];
    int percent;
} w_provider_value;

////////////////
//~ Providers Interface

void w_providers_configure(HWND owner, UINT message, const w_desktop_config *config);
void w_providers_tick(void);
bool w_providers_value(const char *name, w_provider_value *out, char *action, size_t cap);
void w_providers_refresh(void);
void w_providers_stop(void);
enum { provider_timer = 27 };
