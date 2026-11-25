#pragma once
#include "win_platform/win_platform.h"
enum { palette_text_capacity = 256, palette_command_limit = 32766 };

enum w_palette_group {
    palette_flat, palette_apps, palette_places, palette_windows, palette_tools,
    palette_settings, palette_input, palette_software, palette_session, palette_toolbar,
    palette_group_count
};

////////////////
//~ Palette

typedef struct w_palette_entry {
    wchar_t label[palette_text_capacity];
    int id;
    enum w_palette_group group;
    const char *command_name;
    const wchar_t *keywords; // Normalized search terms for built-in actions; static lifetime.
    int system_icon; // One-based index in the shell's small icon image list, or zero.
    uint64_t identity;
} w_palette_entry;

typedef void (*palette_callback)(int id);
typedef void (*palette_line_callback)(const wchar_t *line, UINT request);

////////////////
//~ Palette Interface

void w_palette_open(HMONITOR monitor, const wchar_t *title, const w_palette_entry *entries, int count,
                  palette_callback callback, HANDLE app_images, palette_line_callback run_line);
void w_palette_line_result(UINT request, bool started);
void w_palette_focus(void);
void w_palette_position(HMONITOR monitor);
bool w_palette_shown(void);
bool w_palette_visible(const wchar_t *title);
bool w_palette_message(MSG *msg);
