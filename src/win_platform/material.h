#pragma once
#include "config/config.h"
#include <windows.h>

enum w_material_role { material_unattached, material_panel, material_dock };
void w_material_apply(HWND window, const w_panel_material *material, COLORREF tint, int rounding, enum w_material_role role);
enum w_material_role w_material_role_of(HWND window);
bool w_material_alpha(HWND window, BYTE fade);
