#pragma once
#include "os_win32/os_win32.h"
#include <shobjidl.h>

enum { catalog_label_cap = 256, catalog_keywords_cap = 1024 };

typedef struct {
    wchar_t process[MAX_PATH], keywords[catalog_keywords_cap];
} w_catalog_metadata;

void w_catalog_file_description(const wchar_t *path, wchar_t *out, int cap);
void w_catalog_shell_metadata(IShellItem2 *item, const wchar_t *app_id, bool task, w_catalog_metadata *meta);
void w_catalog_caption(wchar_t *out, int cap, const wchar_t *name, const wchar_t *process);
