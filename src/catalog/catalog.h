#pragma once
#include "win_platform/win_platform.h"

////////////////
//~ Catalog

void w_catalog_begin(HWND target, UINT updated);
typedef struct w_catalog_state w_catalog_state;
w_catalog_state *w_catalog_acquire(void);
void w_catalog_release(w_catalog_state *snapshot);
int w_catalog_count(const w_catalog_state *snapshot);
uint64_t w_catalog_identity(const w_catalog_state *snapshot, int index);
HANDLE w_catalog_images(const w_catalog_state *snapshot);
void w_catalog_label(const w_catalog_state *snapshot, int index, wchar_t *label, int cap, int *icon);
bool w_catalog_launch_item(const w_catalog_state *snapshot, int index);
bool w_catalog_launch(const char *name);
