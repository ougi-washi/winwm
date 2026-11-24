#pragma once
#include "win_platform/win_platform.h"

////////////////
//~ Catalog

void w_catalog_begin(HWND target, UINT updated);
int w_catalog_count(void);
bool w_catalog_label(int index, char *out, int cap);
bool w_catalog_launch(const char *name);
