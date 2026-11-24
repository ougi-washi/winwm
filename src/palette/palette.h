#pragma once
#include "win_platform/win_platform.h"
enum { palette_limit = 768, palette_text_capacity = 256 };

////////////////
//~ Palette

typedef struct w_palette_entry {
    wchar_t label[palette_text_capacity];
    int id;
} w_palette_entry;

typedef void (*palette_callback)(int id);

////////////////
//~ Palette Interface

void w_palette_open(HMONITOR monitor, const wchar_t *title, const w_palette_entry *entries, int count,
                  palette_callback callback);
void w_palette_focus(void);
bool w_palette_shown(void);
bool w_palette_visible(const wchar_t *title);
bool w_palette_message(MSG *msg);
