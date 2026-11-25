#pragma once
#include "os_win32/os_win32.h"

HFONT w_theme_font(const wchar_t *family, int pixel_height, int weight);
enum w_text_role { text_body, text_secondary, text_emphasis, text_heading, text_title, text_mono };
HFONT w_theme_role_font(UINT dpi,enum w_text_role role);
HFONT w_theme_message_font(UINT dpi);
int w_theme_line_height(HFONT font);
int w_theme_role_line_height(UINT dpi,enum w_text_role role);
int w_theme_text_width(HFONT font,const wchar_t *text,UINT flags);
int w_theme_wrapped_height(HFONT font,const wchar_t *text,int width);
void w_theme_fonts_changed(void);
const wchar_t *w_theme_text_family(const wchar_t *text, int length);
// The caller owns a returned font. NULL means the base font should stay selected.
HFONT w_theme_font_for_family(HFONT base, const wchar_t *family);
// Select a cached variant for drawing. Restore the returned font after painting.
HFONT w_theme_select_family(HDC dc, const wchar_t *family);
int w_theme_draw_text(HDC dc, const wchar_t *text, int length, RECT *bounds, UINT format);
BOOL w_theme_text_extent(HDC dc, const wchar_t *text, int length, SIZE *size);
// Select a smaller variant only when the current font exceeds a fixed row.
// Restore the returned previous font when non-NULL.
HFONT w_theme_select_fitting_font(HDC dc,int max_height);
// Native controls retain their selected font. The caller owns this variant.
HFONT w_theme_create_fitting_font(HFONT base,int height,int max_height);
int w_theme_draw_fitted_line(HDC dc,const wchar_t *text,int length,RECT *bounds,UINT flags);
int w_theme_draw_control_line(HWND control,HDC dc,const wchar_t *text,RECT *bounds,UINT flags);
