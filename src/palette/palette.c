#include "theme/theme.h"
#include "theme/icon_button.h"
#include "palette/palette.h"
#include <commctrl.h>
#include <windowsx.h>
#include <usp10.h>
#include <shellscalingapi.h>
#include <uxtheme.h>
#include <wctype.h>
#include <stdlib.h>
#include <stdio.h>

enum { check_deactivation = WM_APP + 1, restore_foreground, no_result = INT_MIN };
enum { command_prompt = -palette_group_count, command_submit = command_prompt - 1 };

static const struct {
    const wchar_t *label;
    wchar_t glyph;
    const wchar_t *hint;
} groups[palette_group_count] = {
    [palette_apps] = {L"Applications", 0xe71d, L"Installed apps"},
    [palette_places] = {L"Files & projects", 0xe8b7, L"Folders and terminals"},
    [palette_windows] = {L"Windows & workspaces", 0xe737, L"Tiling and monitors"},
    [palette_tools] = {L"Tools", 0xe756, L"Commands, capture, clipboard"},
    [palette_settings] = {L"Settings", 0xe713, L"Devices and appearance"},
    [palette_input] = {L"Languages & input", 0xe8c1, L"Keyboards and IMEs"},
    [palette_software] = {L"Software", 0xe896, L"Install and update"},
    [palette_session] = {L"Session", 0xe7e8, L"Lock, sleep, sign out"},
    [palette_toolbar] = {L"Toolbar actions", 0xe713, L"Configured commands"},
};

typedef struct {
    w_palette_entry entry;
    wchar_t normalized[palette_text_capacity], alias[palette_text_capacity];
} w_cached_row;

typedef struct { int row, tier, cost; } w_result;
static w_cached_row *rows;
static w_result *results;
static int row_count, result_count, in_group[palette_group_count];
static enum w_palette_group group;
static HWND panel, heading, back, close_button, edit, list, empty_text, footer, return_to, pending_restore;
static HFONT text_font, title_font, small_font, glyph_font;
static HIMAGELIST app_icons, row_height_icons;
static struct {
    int index;
    HICON handle;
} icon_cache[64];
static UINT palette_dpi;
static int pressed_result = no_result;
static bool grouped, manual_selection, filtering, composing;
static palette_callback select_entry;
static palette_line_callback run_line;
static UINT ticket, pending_ticket;
static wchar_t input_text[palette_command_limit+4], command_label[palette_text_capacity];
static const wchar_t *command_line;
static wchar_t root_title[128];
static const wchar_t *search_hint;
static const wchar_t *launch_status;

static int px(int units) { return MulDiv(units, palette_dpi, 96); }

static void w_navigation_hint(void) {
    RECT area;
    GetClientRect(panel,&area);
    const wchar_t *hint = launch_status;
    if (!hint && area.right < px(360))
        hint = group ? L"↑ ↓  Move    ↵  Open    Esc  Back" : L"↑ ↓  Move    ↵  Open    Esc  Close";
    if (!hint) hint = group ? L"↑ ↓  Navigate       Enter  Open       Esc  Back" :
                             L"↑ ↓  Navigate       Enter  Open       Esc  Close";
    SetWindowTextW(footer,hint);
}

static void w_font(HFONT *current, int size, int weight, const wchar_t *family) {
    HFONT next = CreateFontW(-px(size),0,0,0,weight,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
                             0,0,CLEARTYPE_QUALITY,0,family);
    if (next) {
        if (*current) DeleteObject(*current);
        *current = next;
    }
}

static void w_palette_size(void) {
    if (!list || !footer) return;
    UINT dpi = GetDpiForWindow(panel);
    if (!dpi) dpi = 96;
    if (dpi != palette_dpi) {
        palette_dpi = dpi;
        w_font(&text_font, 15, FW_NORMAL, L"Segoe UI");
        w_font(&title_font, 21, FW_SEMIBOLD, L"Segoe UI");
        w_font(&small_font, 13, FW_NORMAL, L"Segoe UI");
        w_font(&glyph_font, 17, FW_NORMAL, L"Segoe MDL2 Assets");
        SendMessageW(heading, WM_SETFONT, (WPARAM)title_font, TRUE);
        SendMessageW(edit, WM_SETFONT, (WPARAM)text_font, TRUE);
        SendMessageW(list, WM_SETFONT, (WPARAM)text_font, TRUE);
        SendMessageW(empty_text, WM_SETFONT, (WPARAM)text_font, TRUE);
        SendMessageW(footer, WM_SETFONT, (WPARAM)small_font, TRUE);
        HIMAGELIST size = ImageList_Create(1, max(1,px(38)), ILC_COLOR32, 1, 1);
        if (size) {
            ListView_SetImageList(list, size, LVSIL_SMALL);
            if (row_height_icons) ImageList_Destroy(row_height_icons);
            row_height_icons = size;
        }
    }
    RECT r;
    GetClientRect(panel, &r);
    int pad = px(16), field_top = px(50), results_top = px(98);
    int width = max(1, r.right - 2*pad), footer_top = max(results_top, r.bottom - px(32));
    int left = group ? pad + px(36) : pad;
    MoveWindow(back, pad, px(9), px(30), px(31), FALSE);
    MoveWindow(heading, left, px(8), max(1,r.right-left-pad-px(38)), px(34), TRUE);
    MoveWindow(close_button, r.right-pad-px(30), px(9), px(30), px(31), TRUE);
    MoveWindow(edit, pad+px(38), field_top+px(9), max(1,width-px(50)), px(21), TRUE);
    MoveWindow(list, pad, results_top, width, max(1,footer_top-results_top-px(4)), TRUE);
    RECT viewport;
    GetClientRect(list, &viewport);
    ListView_SetColumnWidth(list, 0, max(1, (int)viewport.right));
    MoveWindow(empty_text, pad, results_top, width, px(62), TRUE);
    MoveWindow(footer, pad, footer_top, width, px(27), TRUE);
    w_navigation_hint();
    InvalidateRect(panel, NULL, TRUE);
}

static void w_search_text(wchar_t *out, const wchar_t *text) {
    int n = 0;
    bool space = false;
    for (; *text; text++) {
        if (iswspace(*text) || *text == L'_' || *text == L'-') {
            space = n != 0;
            continue;
        }
        if (space)
            out[n++] = L' ';
        space = false;
        out[n++] = *text;
    }
    out[n] = 0;
    CharLowerBuffW(out, n);
}

typedef struct w_query_word {
    const wchar_t *start;
    size_t length;
} w_query_word;

// Match the longest prefixes first so that "man manage" can use two distinct words.
static int w_query_words(const wchar_t *query, w_query_word *words) {
    int count = 0;
    while (*query) {
        size_t length = wcscspn(query, L" ");
        int i = count++;
        while (i > 0 && words[i - 1].length < length) {
            words[i] = words[i - 1];
            i--;
        }
        words[i] = (w_query_word){query, length};
        query += length;
        if (*query)
            query++;
    }
    return count;
}

static bool w_word_start(const wchar_t *text, const wchar_t *position) {
    return position == text || !IsCharAlphaNumericW(position[-1]);
}

static int w_match_tier(const wchar_t *text, const wchar_t *query, const w_query_word *words,
                        int count, int *cost) {
    if (!wcscmp(text, query))
        return 0;
    *cost = (int)wcslen(text);
    const wchar_t *hit = wcsstr(text, query);
    if (hit == text)
        return 1;
    if (!hit && !query[1]) return -1;
    for (const wchar_t *p = hit; p; p = wcsstr(p + 1, query)) {
        if (w_word_start(text, p)) {
            *cost += (int)(p - text);
            return 2;
        }
    }
    if (count > 1) {
        bool used[palette_text_capacity] = {0};
        int i;
        for (i = 0; i < count; i++) {
            const wchar_t *p = text;
            while (*p && (used[p - text] || !w_word_start(text, p) ||
                          wcsncmp(p, words[i].start, words[i].length)))
                p++;
            if (!*p)
                break;
            used[p - text] = true;
        }
        if (i == count)
            return 3;
    }
    if (hit) {
        *cost += (int)(hit - text);
        return 4;
    }
    const wchar_t *initials = query, *fuzzy = query;
    int gaps = 0;
    for (const wchar_t *p = text; *p; p++) {
        if (*initials && w_word_start(text, p) && *p == *initials)
            initials++;
        if (*fuzzy) {
            if (*p == *fuzzy) {
                fuzzy++;
                if (*fuzzy == L' ')
                    fuzzy++;
            } else {
                gaps++;
            }
        }
    }
    if (!*initials)
        return 5;
    *cost += gaps;
    return *fuzzy ? -1 : 6;
}


static const wchar_t *w_label(int source) {
    if (source == command_prompt) return L"Run a command…";
    if (source == command_submit) return command_label;
    return source < 0 ? groups[-source].label : rows[source].entry.label;
}

static int w_compare_result(const void *left, const void *right) {
    const w_result *a = left, *b = right;
    if (a->tier != b->tier) return a->tier - b->tier;
    if (a->cost != b->cost) return a->cost - b->cost;
    int order = CompareStringOrdinal(w_label(a->row), -1, w_label(b->row), -1, TRUE);
    if (order != CSTR_EQUAL) return order - CSTR_EQUAL;
    return (a->row > b->row) - (a->row < b->row);
}

static int w_selected_index(void) {
    return ListView_GetNextItem(list, -1, LVNI_SELECTED);
}

static int w_source(int index) {
    return index >= 0 && index < result_count ? results[index].row : no_result;
}

static void w_select(int index) {
    if (index < 0 || index >= result_count) return;
    ListView_SetItemState(list, index, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(list, index, FALSE);
}

static void w_search_results(void) {
    wchar_t query[palette_text_capacity];
    w_search_text(query, input_text);
    w_query_word words[(palette_text_capacity + 1) / 2];
    int word_count = w_query_words(query, words);
    if (grouped && !group) {
        for (int g = 1; g < palette_group_count; ++g) {
            if (!in_group[g]) continue;
            int cost = 0, tier = 0;
            if (word_count) {
                wchar_t name[80];
                w_search_text(name, groups[g].label);
                tier = w_match_tier(name, query, words, word_count, &cost);
            }
            if (tier >= 0) results[result_count++] = (w_result){-g,tier,cost};
        }
    }
    if (!grouped || group || word_count) {
        if (run_line && (group == palette_tools || (!group && word_count))) {
            int cost=0, tier=word_count ? w_match_tier(L"run a command",query,words,word_count,&cost) : 0;
            if (word_count && tier != 0) {
                int alt_cost=0, alt=w_match_tier(L"execute command shell cmd powershell",query,words,word_count,&alt_cost);
                if (alt>=0 && (tier<0 || alt<tier)) { tier=alt; cost=alt_cost+palette_text_capacity; }
            }
            if (tier >= 0) results[result_count++] = (w_result){command_prompt,tier,cost};
        }
        for (int i = 0; i < row_count; ++i) {
            const w_cached_row *r = &rows[i];
            if (group && group != r->entry.group) continue;
            int tier = 0, cost = 0;
            if (word_count) {
                tier = w_match_tier(r->normalized, query, words, word_count, &cost);
                if (tier != 0 && *r->alias) {
                    int alias_cost = 0, alias_tier = w_match_tier(r->alias, query, words, word_count, &alias_cost);
                    // Prefer a visible label to an equally good command alias.
                    if (alias_tier >= 0 && (tier < 0 || alias_tier < tier)) {
                        tier = alias_tier;
                        cost = alias_cost + palette_text_capacity;
                    }
                }
                if (tier != 0 && r->entry.keywords) {
                    int extra=0, match=w_match_tier(r->entry.keywords,query,words,word_count,&extra);
                    if (match>=0 && (tier<0 || match<tier)) { tier=match; cost=extra+2*palette_text_capacity; }
                }
                if (!tier && !*r->alias && r->entry.group==palette_apps) --cost;
            }
            if (tier >= 0) results[result_count++] = (w_result){i,tier,cost};
        }
    }
    if (word_count && result_count > 1)
        qsort(results, (size_t)result_count, sizeof(*results), w_compare_result);
}

static void w_filter(void) {
    if (filtering || !list) return;
    filtering = true;
    manual_selection = false;
    pressed_result = no_result;
    result_count = 0;
    command_line = NULL;
    int length = GetWindowTextLengthW(edit);
    GetWindowTextW(edit,input_text,count_of(input_text));
    const wchar_t *message = length ? L"No matches" : L"No entries";
    if (run_line && input_text[0] == L'>') {
        const wchar_t *line = input_text + 1;
        while (*line == L' ' || *line == L'\t') ++line;
        if (length >= (int)count_of(input_text) || wcslen(line) > palette_command_limit)
            message = L"The command exceeds the Windows command-line limit.";
        else if (!*line) message = L"Enter a command, file, or Windows settings URI";
        else {
            command_line = line;
            swprintf(command_label,count_of(command_label),L"Run   %.235ls%ls",line,wcslen(line)>235 ? L"…" : L"");
            results[result_count++] = (w_result){command_submit,0,0};
        }
    } else if (length < palette_text_capacity) w_search_results();
    if (ListView_GetItemCount(list)) ListView_SetItemState(list,-1,0,LVIS_SELECTED|LVIS_FOCUSED);
    ListView_SetItemCountEx(list, result_count, LVSICF_NOSCROLL);
    w_select(0);
    SetWindowTextW(empty_text, message);
    ShowWindow(list,result_count ? SW_SHOWNA : SW_HIDE);
    ShowWindow(empty_text, result_count ? SW_HIDE : SW_SHOWNA);
    RECT viewport;
    GetClientRect(list,&viewport);
    ListView_SetColumnWidth(list,0,max(1,(int)viewport.right));
    InvalidateRect(list, NULL, FALSE);
    filtering = false;
}

static void w_route(enum w_palette_group next) {
    enum w_palette_group previous = group;
    group = next;
    SetWindowTextW(heading, group ? groups[group].label : root_title);
    ShowWindow(back, group ? SW_SHOWNA : SW_HIDE);
    pending_ticket = 0;
    launch_status = NULL;
    filtering = true;
    SetWindowTextW(edit, L"");
    search_hint = group ? L"Search this category" : run_line ? L"Search apps and actions, or type > for a command" :
                  grouped ? L"Search apps and actions" : L"Filter entries";
    InvalidateRect(edit,NULL,TRUE);
    filtering = false;
    w_filter();
    if (previous && !next) {
        for (int i = 0; i < result_count; ++i)
            if (results[i].row == -(int)previous) { w_select(i); break; }
    }
    w_palette_size();
}

static void w_glyph(HDC dc, wchar_t glyph, RECT r, COLORREF color) {
    SelectObject(dc, glyph_font);
    SetTextColor(dc, color);
    DrawTextW(dc, &glyph, 1, &r, DT_SINGLELINE|DT_CENTER|DT_VCENTER|DT_NOPREFIX);
}

static void w_round_rect(HDC dc, RECT r, COLORREF fill, COLORREF border, int radius) {
    SelectObject(dc, GetStockObject(DC_PEN));
    SelectObject(dc, GetStockObject(DC_BRUSH));
    SetDCPenColor(dc, border);
    SetDCBrushColor(dc, fill);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
}

static bool w_app_icon(HDC dc, int index, RECT box) {
    if (!app_icons || index <= 0) return false;
    size_t slot = (size_t)index % count_of(icon_cache);
    if (icon_cache[slot].index != index) {
        if (icon_cache[slot].handle) DestroyIcon(icon_cache[slot].handle);
        icon_cache[slot].handle = ImageList_GetIcon(app_icons,index-1,ILD_NORMAL);
        icon_cache[slot].index = index;
    }
    int size = px(18);
    return icon_cache[slot].handle && DrawIconEx(dc, (box.left+box.right-size)/2, (box.top+box.bottom-size)/2,
                                                 icon_cache[slot].handle, size,size,0,NULL,DI_NORMAL);
}

static bool w_can_run_admin(int source) {
    return run_line && (source == command_submit || (source >= 0 && rows[source].entry.can_run_admin));
}

static void w_draw_result(NMLVCUSTOMDRAW *draw) {
    const NMCUSTOMDRAW *cd = &draw->nmcd;
    int index = (int)cd->dwItemSpec, source = w_source(index);
    if (source == no_result) return;
    const w_palette_entry *entry = source >= 0 ? &rows[source].entry : NULL;
    bool category = source < 0 && source > command_prompt;
    enum w_palette_group kind = entry ? entry->group : category ? (enum w_palette_group)-source : palette_tools;
    wchar_t glyph = entry && entry->glyph ? entry->glyph : groups[kind].glyph;
    RECT r = {0};
    if (!ListView_GetItemRect(list,index,&r,LVIR_BOUNDS)) return;
    RECT viewport;
    GetClientRect(list, &viewport);
    r.left = 0;
    r.right = viewport.right;
    HDC dc = cd->hdc;
    int saved = SaveDC(dc);
    bool selected = (ListView_GetItemState(list,index,LVIS_SELECTED) & LVIS_SELECTED) != 0;
    COLORREF ink = selected ? w_theme_selection(true) : w_theme_foreground();
    FillRect(dc, &r, w_theme_brush());
    if (selected) {
        RECT box = r;
        InflateRect(&box, -px(1), -px(2));
        w_round_rect(dc, box, w_theme_selection(false), w_theme_selection(false), px(10));
    }
    SetBkMode(dc, TRANSPARENT);
    RECT label = r;
    label.left += px(12);
    label.right -= px(12);
    RECT icon = r;
    icon.left += px(8);
    icon.right = icon.left + px(29);
    if (glyph) label.left = icon.right + px(8);
    if (category) {
        RECT arrow = label;
        arrow.left = arrow.right-px(21);
        w_glyph(dc, 0xe76c, arrow, selected ? ink : w_theme_accent());
        label.right = arrow.left-px(6);
        if (viewport.right >= px(435)) {
            RECT hint=label;
            hint.left+=px(183);
            SelectObject(dc,small_font);
            SetTextColor(dc,selected ? ink : w_theme_hint());
            DrawTextW(dc,groups[kind].hint,-1,&hint,DT_RIGHT|DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
            label.right=hint.left-px(6);
        }
    } else if (selected && w_can_run_admin(source)) {
        const wchar_t *hint = viewport.right >= px(365) ? L"Ctrl+Enter  Admin" : L"Ctrl+↵";
        SelectObject(dc, small_font);
        SIZE size;
        GetTextExtentPoint32W(dc, hint, (int)wcslen(hint), &size);
        RECT key = label;
        key.left = key.right - size.cx;
        RECT shield = {key.left-px(22), key.top, key.left-px(3), key.bottom};
        SetTextColor(dc, ink);
        DrawTextW(dc, hint, -1, &key, DT_RIGHT|DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX);
        w_glyph(dc, 0xea18, shield, ink);
        label.right = shield.left-px(6);
    } else if (kind && !group && viewport.right >= px(350)) {
        SelectObject(dc, small_font);
        SIZE size;
        GetTextExtentPoint32W(dc, groups[kind].label, (int)wcslen(groups[kind].label), &size);
        RECT context = label;
        context.left = context.right - size.cx;
        SetTextColor(dc, selected ? ink : w_theme_accent());
        DrawTextW(dc, groups[kind].label, -1, &context, DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX);
        label.right = context.left - px(16);
    }
    SelectObject(dc, text_font);
    SetTextColor(dc, ink);
    DrawTextW(dc, w_label(source), -1, &label, DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
    if (glyph && (!entry || !w_app_icon(dc,entry->system_icon,icon)))
        w_glyph(dc, glyph, icon, selected ? ink : w_theme_accent());
    RestoreDC(dc, saved);
}

bool w_palette_shown(void) {
    return panel && IsWindowVisible(panel);
}

void w_palette_focus(void) {
    if (w_palette_shown()) {
        if (GetForegroundWindow() != panel)
            SetForegroundWindow(panel);
        if (GetForegroundWindow() == panel)
            SetFocus(edit);
    }
}

static void w_dismiss(bool restore_focus) {
    pending_ticket = 0;
    pending_restore = restore_focus && GetForegroundWindow() == panel ? return_to : NULL;
    return_to = NULL;
    HWND capture = GetCapture();
    if (capture == panel || IsChild(panel, capture))
        ReleaseCapture();
    ShowWindow(panel, SW_HIDE);
    if (pending_restore)
        PostMessageW(panel, restore_foreground, 0, 0);
}

bool w_palette_close(void) {
    if (!w_palette_shown())
        return false;
    w_dismiss(true);
    return true;
}

static void w_restore_window(HWND previous) {
    if (w_palette_shown() || previous == GetDesktopWindow() || !IsWindowVisible(previous) || IsIconic(previous))
        return;
    HWND foreground = GetForegroundWindow();
    if (foreground && foreground != GetDesktopWindow() && IsWindowVisible(foreground) &&
        !(GetWindowLongPtrW(foreground, GWL_EXSTYLE) & WS_EX_NOACTIVATE))
        return;
    HWND popup = GetLastActivePopup(previous);
    if (IsWindowVisible(popup) && IsWindowEnabled(popup))
        previous = popup;
    if (IsWindowEnabled(previous) && !(GetWindowLongPtrW(previous, GWL_EXSTYLE) & WS_EX_NOACTIVATE))
        SetForegroundWindow(previous);
}


static void w_accept(bool admin) {
    int source = w_source(w_selected_index());
    if (source == no_result || (admin && !w_can_run_admin(source))) return;
    if (source == command_submit) {
        if (run_line && command_line && !pending_ticket) {
            if (!++ticket) ++ticket;
            pending_ticket = ticket;
            launch_status = admin ? L"Requesting administrator access…" : L"Starting command…";
            w_navigation_hint();
            run_line(command_line,ticket,admin);
        }
        return;
    }
    if (source == command_prompt) {
        SetWindowTextW(edit,L"> ");
        SetFocus(edit);
        SendMessageW(edit,EM_SETSEL,-1,-1);
        return;
    }
    if (source < 0) {
        w_route((enum w_palette_group)-source);
        SetFocus(edit);
        return;
    }
    int id = rows[source].entry.id;
    palette_callback cb = select_entry;
    w_dismiss(false);
    if (cb) cb(id,admin);
}

void w_palette_line_result(UINT request, DWORD error) {
    if (!request || request != pending_ticket) return;
    pending_ticket = 0;
    if (!w_palette_shown()) return;
    if (!error) w_dismiss(false);
    else {
        static wchar_t message[256];
        DWORD length = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                      NULL, error, 0, message, count_of(message), NULL);
        while (length && iswspace(message[length-1])) message[--length] = 0;
        launch_status = length ? message : L"Windows could not open this command or location.";
        w_navigation_hint();
        SetFocus(edit);
    }
}

static LRESULT CALLBACK w_results_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR subclass, DWORD_PTR data) {
    (void)subclass;
    (void)data;
    if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP) {
        LVHITTESTINFO hit = {.pt={GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}};
        int index = ListView_HitTest(w, &hit);
        int source = w_source(index);
        if (msg == WM_LBUTTONDOWN) {
            pressed_result = source;
            if (source != no_result) { w_select(index); manual_selection = true; SetCapture(w); }
            return 0;
        }
        else {
            bool pick = source != no_result && pressed_result == source;
            pressed_result = no_result;
            if (GetCapture() == w) ReleaseCapture();
            if (pick && w_palette_shown()) { w_select(index); w_accept(false); }
            return 0;
        }
    } else if (msg == WM_CANCELMODE || msg == WM_CAPTURECHANGED || msg == WM_LBUTTONDBLCLK)
        pressed_result = no_result;
    else if (msg == WM_CHAR && wp >= L' ' && GetKeyState(VK_CONTROL) >= 0) {
        SetFocus(edit);
        return SendMessageW(edit, msg, wp, lp);
    }
    return DefSubclassProc(w, msg, wp, lp);
}

static LRESULT CALLBACK w_edit_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR subclass, DWORD_PTR data) {
    (void)subclass;
    (void)data;
    if (msg == WM_IME_STARTCOMPOSITION) composing = true;
    if (msg == WM_IME_ENDCOMPOSITION || msg == WM_KILLFOCUS) composing = false;
    // Classic EDIT controls only support their system cue banner with v6 common controls.
    // Draw the empty-state hint for both themed and classic desktop sessions.
    if ((msg == WM_PAINT || msg == WM_PRINTCLIENT || msg == WM_PRINT) && search_hint &&
        !composing && !GetWindowTextLengthW(w)) {
        LRESULT result = DefSubclassProc(w,msg,wp,lp);
        HDC dc = msg == WM_PAINT ? GetDC(w) : (HDC)wp;
        if (dc) {
            int saved = SaveDC(dc);
            RECT box;
            GetClientRect(w,&box);
            box.left += px(3);
            SetBkMode(dc,TRANSPARENT);
            SelectObject(dc,text_font);
            SetTextColor(dc,w_theme_hint());
            DrawTextW(dc,search_hint,-1,&box,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
            RestoreDC(dc,saved);
            if (msg == WM_PAINT) ReleaseDC(w,dc);
        }
        return result;
    }
    return DefSubclassProc(w, msg, wp, lp);
}

static LRESULT CALLBACK w_palette_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NOTIFY && ((NMHDR *)lp)->hwndFrom == list) {
        NMHDR *note = (NMHDR *)lp;
        if (note->code == LVN_GETDISPINFOW) {
            LVITEMW *item = &((NMLVDISPINFOW *)lp)->item;
            int source = w_source(item->iItem);
            if ((item->mask & LVIF_TEXT) && item->pszText && item->cchTextMax > 0)
                lstrcpynW(item->pszText, source == no_result ? L"" : w_label(source), item->cchTextMax);
        } else if (note->code == NM_CUSTOMDRAW) {
            NMLVCUSTOMDRAW *draw = (NMLVCUSTOMDRAW *)lp;
            if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                w_draw_result(draw);
                return CDRF_SKIPDEFAULT;
            }
        } else if (note->code == LVN_ITEMCHANGED && !filtering) {
            NMLISTVIEW *change = (NMLISTVIEW *)lp;
            if ((change->uChanged & LVIF_STATE) && (change->uNewState & LVIS_SELECTED))
                manual_selection = true;
        }
    }
    LRESULT color_result;
    if (w_theme_message(w, msg, wp, lp, &color_result)) return color_result;
    if (msg == WM_PAINT || msg == WM_PRINTCLIENT) {
        PAINTSTRUCT ps;
        HDC dc = msg == WM_PAINT ? BeginPaint(w, &ps) : (HDC)wp;
        RECT area;
        GetClientRect(w, &area);
        RECT field = {px(16),px(50),area.right-px(16),px(89)};
        int saved = SaveDC(dc);
        w_round_rect(dc, field, w_theme_background(), w_theme_separator(), px(10));
        RECT icon = {field.left+px(6),field.top,field.left+px(34),field.bottom};
        SetBkMode(dc, TRANSPARENT);
        w_glyph(dc, 0xe721, icon, w_theme_accent());
        RestoreDC(dc,saved);
        if (msg == WM_PAINT) EndPaint(w, &ps);
        return 0;
    }
    if (msg == WM_NCHITTEST) {
        LRESULT hit = DefWindowProcW(w,msg,wp,lp);
        POINT p = {GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
        ScreenToClient(w,&p);
        if (hit == HTCLIENT && p.y < px(48)) {
            HWND child = ChildWindowFromPointEx(w,p,CWP_SKIPINVISIBLE|CWP_SKIPDISABLED);
            if (child != back && child != close_button) return HTCAPTION;
        }
        return hit;
    }
    if (msg == WM_GETMINMAXINFO) {
        UINT dpi = GetDpiForWindow(w);
        ((MINMAXINFO *)lp)->ptMinTrackSize = (POINT){MulDiv(240,dpi,96), MulDiv(195,dpi,96)};
        return 0;
    }
    if (msg == WM_LBUTTONDOWN && GET_Y_LPARAM(lp) >= px(50) && GET_Y_LPARAM(lp) <= px(90)) {
        SetFocus(edit);
        return 0;
    }
    if (msg == WM_SETFOCUS) { if (edit) SetFocus(edit); return 0; }
    if (msg == WM_COMMAND && lp) {
        if ((HWND)lp == edit && HIWORD(wp) == EN_CHANGE && !filtering) {
            pending_ticket = 0;
            launch_status = NULL;
            w_navigation_hint();
            w_filter();
        }
        if ((HWND)lp == back && HIWORD(wp) == BN_CLICKED) { w_route(palette_flat); SetFocus(edit); }
        if ((HWND)lp == close_button && HIWORD(wp) == BN_CLICKED) w_dismiss(true);
        return 0;
    }
    if (msg == WM_SIZE) { w_palette_size(); return 0; }
    if (msg == WM_DPICHANGED) {
        RECT *r = (RECT *)lp;
        SetWindowPos(w, NULL, r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOACTIVATE|SWP_NOZORDER);
        w_palette_size();
        return 0;
    }
    if (msg == restore_foreground) {
        HWND previous = pending_restore;
        pending_restore = NULL;
        if (previous) w_restore_window(previous);
        return 0;
    }
    if (msg == WM_CLOSE) { w_dismiss(true); return 0; }
    if (msg == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE) PostMessageW(w,check_deactivation,0,0);
    if (msg == check_deactivation) {
        if (IsWindowVisible(w) && GetForegroundWindow() != w) w_dismiss(false);
        return 0;
    }
    return DefWindowProcW(w,msg,wp,lp);
}

static bool w_create_palette(void) {
    INITCOMMONCONTROLSEX init = {sizeof(init), ICC_LISTVIEW_CLASSES};
    if (!InitCommonControlsEx(&init)) return false;
    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSW wc = {.hInstance = inst, .lpszClassName = L"WinwmPalette", .lpfnWndProc = w_palette_proc,
                    .hCursor = LoadCursorW(NULL,IDC_ARROW)};
    RegisterClassW(&wc);
    panel = CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_TOPMOST, wc.lpszClassName,L"Menu",
                            WS_POPUP|WS_THICKFRAME|WS_SYSMENU|WS_CLIPCHILDREN,CW_USEDEFAULT,0,540,480,NULL,NULL,inst,NULL);
    if (!panel) return false;
    DWORD visible = WS_CHILD|WS_VISIBLE;
    back = CreateWindowW(L"BUTTON",L"Back",WS_CHILD|WS_TABSTOP|BS_OWNERDRAW,0,0,0,0,panel,NULL,inst,NULL);
    heading = CreateWindowW(L"STATIC",L"",visible|SS_CENTERIMAGE|SS_NOPREFIX|SS_ENDELLIPSIS,0,0,0,0,panel,NULL,inst,NULL);
    close_button = CreateWindowW(L"BUTTON",L"Close",visible|WS_TABSTOP|BS_OWNERDRAW,0,0,0,0,panel,NULL,inst,NULL);
    if (back) w_icon_button(back,L"\xe72b",L"Back to categories");
    if (close_button) w_icon_button(close_button,L"\xe8bb",L"Close");
    // The static label precedes the edit in the child order for its accessible name.
    HWND search_name = CreateWindowW(L"STATIC",L"Search",visible,0,0,0,0,panel,NULL,inst,NULL);
    edit = CreateWindowExW(0,L"EDIT",L"",visible|WS_TABSTOP|ES_AUTOHSCROLL,0,0,0,0,panel,NULL,inst,NULL);
    list = CreateWindowExW(0,WC_LISTVIEWW,L"Results",visible|WS_TABSTOP|LVS_REPORT|LVS_NOCOLUMNHEADER|
                           LVS_OWNERDATA|LVS_SINGLESEL|LVS_SHOWSELALWAYS|LVS_SHAREIMAGELISTS,
                           0,0,0,0,panel,NULL,inst,NULL);
    empty_text = CreateWindowW(L"STATIC",L"No matches",WS_CHILD|SS_CENTER|SS_CENTERIMAGE,
                               0,0,0,0,panel,NULL,inst,NULL);
    footer = CreateWindowW(L"STATIC",L"",visible|SS_CENTERIMAGE|SS_ENDELLIPSIS,0,0,0,0,panel,NULL,inst,NULL);
    if (!heading || !back || !close_button || !search_name || !edit || !list || !empty_text || !footer ||
        !SetWindowSubclass(list,w_results_proc,1,0) || !SetWindowSubclass(edit,w_edit_proc,1,0)) {
        DestroyWindow(panel);
        panel=heading=back=close_button=edit=list=empty_text=footer=NULL;
        return false;
    }
    LVCOLUMNW column = {.mask=LVCF_TEXT|LVCF_WIDTH,.cx=1,.pszText=L"Result"};
    ListView_InsertColumn(list,0,&column);
    ListView_SetExtendedListViewStyle(list,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER);
    SetWindowTheme(list,L"DarkMode_Explorer",NULL);
    w_palette_size();
    return true;
}


static bool w_cache(const w_palette_entry *entries, int count) {
    if (count < 0 || count > INT_MAX-palette_group_count-1 || (!entries && count)) return false;
    w_cached_row *next = count ? calloc((size_t)count, sizeof(*next)) : NULL;
    w_result *slots = calloc((size_t)count+palette_group_count+1, sizeof(*slots));
    if ((count && !next) || !slots) { free(next); free(slots); return false; }
    memset(in_group,0,sizeof(in_group));
    grouped = false;
    for (int i = 0; i < count; ++i) {
        w_cached_row *r = &next[i];
        r->entry = entries[i];
        r->entry.label[palette_text_capacity-1] = 0;
        if ((unsigned)r->entry.group >= palette_group_count) r->entry.group = palette_flat;
        ++in_group[r->entry.group];
        grouped |= r->entry.group != palette_flat;
        w_search_text(r->normalized, r->entry.label);
        if (r->entry.command_name) {
            wchar_t alias[palette_text_capacity];
            if (w_os_wide(r->entry.command_name,alias,count_of(alias))) w_search_text(r->alias,alias);
        }
    }
    free(rows);
    free(results);
    rows = next;
    results = slots;
    row_count = count;
    result_count = 0;
    return true;
}

void w_palette_open(HMONITOR monitor, const wchar_t *title, const w_palette_entry *entries, int count,
                    palette_callback cb, HANDLE app_images, palette_line_callback line_callback) {
    pending_restore = NULL;
    bool preserve = w_palette_visible(title);
    if (!panel && !w_create_palette()) return;
    int selected = preserve && manual_selection ? w_source(w_selected_index()) : no_result;
    enum w_palette_group old_group = palette_flat;
    uint64_t old_identity = 0;
    wchar_t old_label[palette_text_capacity] = {0}, old_alias[palette_text_capacity] = {0};
    if (selected >= 0) {
        old_group = rows[selected].entry.group;
        old_identity = rows[selected].entry.identity;
        wcscpy(old_label,rows[selected].entry.label);
        wcscpy(old_alias,rows[selected].alias);
    }
    if (!w_cache(entries,count)) {
        select_entry = NULL;
        run_line = NULL;
        w_dismiss(true);
        return;
    }
    select_entry = cb;
    run_line = line_callback;
    if (run_line) {
        grouped = true;
        ++in_group[palette_tools];
    }
    // Leave room beyond the dispatch limit so an oversized paste stays invalid.
    SendMessageW(edit,EM_SETLIMITTEXT,run_line ? UINT16_MAX : palette_text_capacity-1,0);
    for (size_t i=0; i < count_of(icon_cache); ++i) {
        if (icon_cache[i].handle) DestroyIcon(icon_cache[i].handle);
    }
    memset(icon_cache,0,sizeof(icon_cache));
    app_icons = (HIMAGELIST)app_images;
    lstrcpynW(root_title,title,count_of(root_title));
    ListView_SetBkColor(list,w_theme_background());
    ListView_SetTextBkColor(list,w_theme_background());
    SetWindowTextW(panel,title);
    if (preserve && (!group || in_group[group])) {
        w_filter();
        if (selected != no_result) {
            for (int i=0; i < result_count; ++i) {
                int src = results[i].row;
                if (src < 0 ? src == selected : selected >= 0 && rows[src].entry.group == old_group &&
                    (old_identity ? rows[src].entry.identity == old_identity :
                     !wcscmp(rows[src].entry.label,old_label) && !wcscmp(rows[src].alias,old_alias))) {
                    w_select(i);
                    manual_selection = true;
                    break;
                }
            }
        }
        return;
    }
    group = palette_flat;
    w_route(palette_flat);
    if (preserve) return;
    if (!w_palette_shown()) return_to = GetForegroundWindow();
    w_palette_position(monitor);
    w_palette_focus();
}

void w_palette_position(HMONITOR monitor) {
    MONITORINFO mi = {.cbSize = sizeof(mi)};
    if (!monitor || !GetMonitorInfoW(monitor,&mi)) {
        monitor = MonitorFromWindow(GetForegroundWindow(),MONITOR_DEFAULTTOPRIMARY);
        GetMonitorInfoW(monitor,&mi);
    }
    UINT dpi=96, vertical=96;
    GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi, &vertical);
    int width = MulDiv(520,dpi,96), height = MulDiv(grouped ? 512 : 404,dpi,96);
    RECT current;
    UINT old_dpi = GetDpiForWindow(panel);
    if (w_palette_shown() && old_dpi && GetWindowRect(panel,&current)) {
        width = MulDiv(current.right-current.left,dpi,old_dpi);
        height = MulDiv(current.bottom-current.top,dpi,old_dpi);
    }
    width = min(mi.rcWork.right-mi.rcWork.left,width);
    height = min(mi.rcWork.bottom-mi.rcWork.top,height);
    SetWindowPos(panel,HWND_TOPMOST,(mi.rcWork.left+mi.rcWork.right-width)/2,
                 (mi.rcWork.top+mi.rcWork.bottom-height)/2,width,height,SWP_SHOWWINDOW|SWP_NOACTIVATE);
}

bool w_palette_visible(const wchar_t *title) {
    return w_palette_shown() && !wcscmp(root_title,title);
}


static void w_delete_word(bool backwards) {
    DWORD start=0, end=0;
    SendMessageW(edit, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
    if (start == end) {
        LONG caret = start;
        wchar_t *query = input_text;
        int len = GetWindowTextW(edit, query, count_of(input_text));
        if (len < GetWindowTextLengthW(edit)) return;
        if (len == 0 || (backwards && caret <= 0) || (!backwards && caret >= len))
            return;
        SCRIPT_STRING_ANALYSIS analysis;
        if (FAILED(ScriptStringAnalyse(NULL, query, len, 0, -1, SSA_BREAK, 0, NULL, NULL, NULL,
                                       NULL, NULL, &analysis)))
            return;
        const SCRIPT_LOGATTR *attr = ScriptString_pLogAttr(analysis);
        LONG boundary = w_clamp_int(caret, 0, len);
        if (attr) {
            for (;;) {
                boundary += backwards ? -1 : 1;
                if (boundary == 0 || boundary == len)
                    break;
                bool starts_word = attr[boundary].fWordStop || attr[boundary - 1].fWhiteSpace;
                if (starts_word && !attr[boundary].fWhiteSpace)
                    break;
            }
        }
        ScriptStringFree(&analysis);
        if (boundary == caret)
            return;
        if (backwards)
            start = boundary;
        else
            end = boundary;
        SendMessageW(edit, EM_SETSEL, start, end);
    }
    SendMessageW(edit, EM_REPLACESEL, TRUE, (LPARAM)L"");
}


bool w_palette_message(MSG *msg) {
    if (!w_palette_shown() || !(msg->hwnd==panel || IsChild(panel,msg->hwnd)) ||
        (msg->message != WM_KEYDOWN && msg->message != WM_SYSKEYDOWN) || composing) return false;
    bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    if (GetKeyState(VK_MENU) < 0) {
        if (group && msg->wParam == VK_LEFT) { w_route(palette_flat); SetFocus(edit); return true; }
        return false;
    }
    if (msg->message == WM_SYSKEYDOWN) return false;
    if (ctrl && !shift) {
        if (msg->wParam == VK_RETURN && run_line && (msg->hwnd == edit || msg->hwnd == list)) {
            if (!(msg->lParam & (1L << 30))) w_accept(true);
            return true;
        }
        if (msg->wParam=='A' && msg->hwnd==edit) { SendMessageW(edit,EM_SETSEL,0,-1); return true; }
        if (msg->hwnd==edit && (msg->wParam==VK_BACK || msg->wParam==VK_DELETE)) {
            w_delete_word(msg->wParam==VK_BACK); return true;
        }
        return false;
    }
    if (msg->wParam == VK_TAB && !ctrl) {
        HWND stops[] = {edit,list,back,close_button};
        int i=0;
        while (i<4 && msg->hwnd!=stops[i]) ++i;
        do { i=(i+(shift ? 3 : 1))%4; }
        while ((stops[i]==back && !group) || (stops[i]==list && !result_count));
        SetFocus(stops[i]);
        return true;
    }
    if (ctrl || shift) return false;
    switch (msg->wParam) {
    case VK_ESCAPE:
        if ((group && GetWindowTextLengthW(edit)) || (run_line && input_text[0]==L'>')) SetWindowTextW(edit,L"");
        else if (group) w_route(palette_flat);
        else { w_dismiss(true); return true; }
        SetFocus(edit);
        return true;
    case VK_RETURN:
        if (msg->hwnd == back || msg->hwnd == close_button) SendMessageW(msg->hwnd,BM_CLICK,0,0);
        else w_accept(false);
        return true;
    case VK_LEFT: case VK_BACK:
        if (group && !GetWindowTextLengthW(edit)) { w_route(palette_flat); SetFocus(edit); return true; }
        return false;
    case VK_RIGHT:
        if (!GetWindowTextLengthW(edit) && w_source(w_selected_index()) < 0 && result_count) {
            w_accept(false); return true;
        }
        return false;
    case VK_UP: case VK_DOWN: case VK_PRIOR: case VK_NEXT: {
        int step = (msg->wParam == VK_PRIOR || msg->wParam == VK_NEXT) ? max(1,ListView_GetCountPerPage(list)) : 1;
        if (msg->wParam == VK_UP || msg->wParam == VK_PRIOR) step = -step;
        if (result_count) {
            w_select(w_clamp_int(w_selected_index()+step,0,result_count-1));
            manual_selection = true;
        }
        return true;
    }
    }
    return false;
}
