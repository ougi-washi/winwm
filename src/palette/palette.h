#pragma once
#include "win_platform/win_platform.h"
#include "win_platform/popup.h"
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
    wchar_t glyph;
    const char *command_name;
    const wchar_t *keywords; // Normalized search terms for built-in actions; static lifetime.
    int system_icon; // One-based index in the shell's small icon image list, or zero.
    uint64_t identity;
    bool can_run_admin;
} w_palette_entry;

typedef void (*palette_callback)(int id, bool admin);
typedef void (*palette_line_callback)(const wchar_t *line, UINT request, bool admin);
typedef struct {
    int count;
    void (*read)(int index, w_palette_entry *entry);
} w_palette_apps;
typedef struct w_palette_site {
    HMONITOR monitor;
    w_popup_anchor anchor;
    w_transition enter, leave;
} w_palette_site;

////////////////
//~ Palette Interface

void w_palette_open(const w_palette_site *site, const wchar_t *title, const w_palette_entry *entries, int count,
                    palette_callback callback, HANDLE app_images, palette_line_callback run_line,
                    const w_palette_apps *apps);
void w_palette_line_result(UINT request, DWORD error);
void w_palette_focus(void);
void w_palette_position(const w_palette_site *site);
const w_popup_anchor *w_palette_anchor(void);
void w_palette_outside(HWND hit);
bool w_palette_shown(void);
bool w_palette_close(void);
bool w_palette_visible(const wchar_t *title);
bool w_palette_message(MSG *msg);
