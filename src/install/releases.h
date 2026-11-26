#pragma once
#include "install/install.h"

typedef struct {
    wchar_t tag[128];
    unsigned long long asset;
    DWORD size;
    char digest[65];
    bool latest, prerelease;
} w_release;

typedef struct {
    w_release *items;
    size_t count;
    char installed[65];
    bool startup;
} w_release_catalog;

DWORD w_release_list(w_release_catalog *catalog, HANDLE cancel);
DWORD w_release_apply(const w_release *release, HANDLE cancel);
