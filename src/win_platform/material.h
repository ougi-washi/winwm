#pragma once
#include "config/config.h"
#include <windows.h>

void w_material_apply(HWND window, const w_panel_material *material, COLORREF tint);
bool w_material_attached(HWND window);
bool w_material_alpha(HWND window, BYTE fade);
