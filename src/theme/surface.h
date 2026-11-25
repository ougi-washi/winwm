#pragma once
#include "os_win32/os_win32.h"
#include "config/config.h"
#include "theme/metrics.h"

////////////////
//~ Theme

HBRUSH w_theme_brush(void);
COLORREF w_theme_background(void);
COLORREF w_theme_foreground(void);
COLORREF w_theme_separator(void);
COLORREF w_theme_accent(void);
COLORREF w_theme_hint(void);
COLORREF w_theme_selection(bool foreground);
int w_theme_radius(UINT dpi);
int w_theme_control_radius(UINT dpi);
void w_theme_frame(HDC dc,const RECT *bounds,UINT dpi);
void w_theme_control_frame(HDC dc,const RECT *bounds,UINT dpi);
void w_theme_input_frame(HDC dc,const RECT *bounds,int hover,bool focused,bool enabled);
void w_theme_configure(const w_desktop_config *config);
bool w_theme_configured(void);
void w_theme_material(HWND window);
void w_theme_dock_material(HWND window);
void w_theme_native_controls(HWND window);
void w_theme_publish(HWND manager);
void w_theme_follow(HWND tool);
void w_theme_follow_overlay(HWND tool);
w_transition w_theme_layer_timing(bool entering);
bool w_theme_motion_enabled(void);
enum { winwm_theme_updated = WM_APP + 0x436, winwm_motion_updated = WM_APP + 0x43a };
bool w_theme_high_contrast(void);
bool w_theme_button_shadows(void);
COLORREF w_theme_mix(COLORREF a, COLORREF b, int percent);
bool w_theme_surface_message(HWND window, UINT message, WPARAM wp, LPARAM lp);
