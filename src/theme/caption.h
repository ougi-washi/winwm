#pragma once
#include <windows.h>

void w_theme_caption_install(HWND window);
int w_theme_caption_height(HWND window);
int w_theme_caption_width(HWND window);
void w_theme_content_rect(HWND window, RECT *content);
void w_theme_adjust_rect(RECT *content, DWORD style, DWORD exstyle, UINT dpi);
