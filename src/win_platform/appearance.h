#pragma once
#include <windows.h>
#include <stdbool.h>

// A zero-action WM_SETTINGCHANGE can also announce a locale or an environment edit.
// Keep unknown areas and policy broadcasts eligible for a preference refresh.
static inline bool w_appearance_setting(WPARAM action, LPARAM area) {
    if (action==SPI_SETHIGHCONTRAST || action==SPI_SETDROPSHADOW) return true;
    const wchar_t *name=(const wchar_t *)area;
    if (action) return action==1 && name && !lstrcmpiW(name,L"Policy");
    return !name || (lstrcmpiW(name,L"Environment") && lstrcmpiW(name,L"intl"));
}
