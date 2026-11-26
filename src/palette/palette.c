#include "theme/theme.h"
#include "theme/icon_button.h"
#include "theme/hover.h"
#include "theme/control_hover.h"
#include "theme/native_controls.h"
#include "palette/palette.h"
#include "win_platform/layer.h"
#include "win_platform/appearance.h"
#include <commctrl.h>
#include <imm.h>
#include <windowsx.h>
#include <usp10.h>
#include <shellscalingapi.h>
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
static HIMAGELIST app_icons;
static UINT palette_dpi, font_dpi;
static int row_height, glyph_size;
static int title_line, body_line, footer_line;
static struct { int label, hint, context; } group_width[palette_group_count];
static const wchar_t *const admin_keys[]={L"Shift+Enter  Admin",L"Shift+↵"};
static int admin_width[count_of(admin_keys)];
static const wchar_t *const nav_hint[]={L"↑ ↓  Navigate       Enter  Open       Esc  Close",L"↑ ↓  Navigate       Enter  Open       Esc  Back"};
static int nav_width[count_of(nav_hint)];
static RECT search_box;
static w_palette_site placement;
static w_layer_fade fade;
static bool closing, motion_enabled;
static int pressed_index = -1;
static bool pressed_inside;
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

static w_transition w_palette_timing(bool entering) {
    if (placement.follow_theme_motion) return w_theme_layer_timing(entering);
    return entering ? placement.enter : placement.leave;
}

// Choose the viewport once on opening. Filtering, inventory updates and section
// navigation keep the window stationary underneath the pointer.
static int opening_rows;
static int w_viewport_chrome(UINT dpi,int *row) {
    int button=w_theme_button_size(dpi),gap=w_theme_spacing(dpi);
    int body=font_dpi==dpi ? body_line : w_theme_role_line_height(dpi,text_body);
    int title=font_dpi==dpi ? title_line : w_theme_role_line_height(dpi,text_title);
    int footer_height=font_dpi==dpi ? footer_line : w_theme_role_line_height(dpi,text_secondary);
    *row=max(button,body+1);
    return max(button,title)+max(button,body+2)+footer_height+5*gap;
}

static int w_initial_height(UINT dpi,int available) {
    int row,chrome=w_viewport_chrome(dpi,&row);
    int budget=min(available,MulDiv(grouped ? 512 : 404,dpi,96));
    int limit=max(1,(budget-chrome)/row);
    return min(available,chrome+w_clamp_int(opening_rows,1,limit)*row);
}

static RECT w_button_bounds(const w_popup_anchor *anchor, UINT dpi) {
    RECT area=w_popup_area(anchor->bar);
    int gap=w_theme_spacing(dpi),width=MulDiv(432,dpi,96);
    RECT space=w_popup_bounds(anchor,area,width,area.bottom-area.top,gap);
    return w_popup_bounds(anchor,area,width,w_initial_height(dpi,space.bottom-space.top),gap);
}

static void w_navigation_hint(void) {
    RECT area;
    GetClientRect(panel,&area);
    const wchar_t *hint = launch_status;
    if (!hint) hint=nav_hint[!!group];
    if (!launch_status && nav_width[!!group]>area.right-4*w_theme_spacing(palette_dpi))
        hint = group ? L"↑ ↓  Move    ↵  Open    Esc  Back" : L"↑ ↓  Move    ↵  Open    Esc  Close";
    wchar_t current[256];
    GetWindowTextW(footer,current,count_of(current));
    if (wcscmp(current,hint)) SetWindowTextW(footer,hint);
}

static void w_font(HFONT *current, HFONT next, const HWND *controls, size_t count) {
    if (next) {
        for (size_t i=0;i<count;++i) SendMessageW(controls[i],WM_SETFONT,(WPARAM)next,FALSE);
        if (*current) DeleteObject(*current);
        *current = next;
    }
}

static void w_palette_fonts(UINT dpi) {
    if (font_dpi==dpi || !footer) return;
    HFONT text=w_theme_role_font(dpi,text_body), title=w_theme_role_font(dpi,text_title);
    HFONT small=w_theme_role_font(dpi,text_secondary);
    if (!text || !title || !small) {
        DeleteObject(text); DeleteObject(title); DeleteObject(small);
        return;
    }
    HWND controls[]={edit,list,empty_text,back,close_button};
    w_font(&text_font,text,controls,count_of(controls));
    w_font(&title_font,title,&heading,1);
    w_font(&small_font,small,&footer,1);
    title_line=w_theme_line_height(title);
    body_line=w_theme_line_height(text);
    footer_line=w_theme_line_height(small);
    for (int i=1;i<palette_group_count;++i) {
        group_width[i].label=w_theme_text_width(text,groups[i].label,DT_NOPREFIX);
        group_width[i].context=w_theme_text_width(small,groups[i].label,DT_NOPREFIX);
        group_width[i].hint=w_theme_text_width(small,groups[i].hint,DT_NOPREFIX);
    }
    for (size_t i=0;i<count_of(admin_keys);++i) admin_width[i]=w_theme_text_width(small,admin_keys[i],DT_NOPREFIX);
    for (size_t i=0;i<count_of(nav_hint);++i) nav_width[i]=w_theme_text_width(small,nav_hint[i],DT_NOPREFIX);
    font_dpi=dpi;
    palette_dpi=0;
}

static void w_palette_size(void) {
    if (!list || !footer) return;
    UINT dpi = GetDpiForWindow(panel);
    if (!dpi) dpi = 96;
    int button = w_theme_button_size(dpi), gap = w_theme_spacing(dpi);
    RECT r;
    GetClientRect(panel,&r);
    static RECT previous;
    static int last_gap;
    static bool had_back;
    bool show_back = group != palette_flat;
    w_palette_fonts(dpi);
    int row=max(button,body_line+1);
    int icon_size=w_theme_icon_size(dpi);
    if (palette_dpi==dpi && glyph_font && row_height == row && glyph_size == icon_size &&
        last_gap == gap && had_back == show_back && EqualRect(&r,&previous)) return;
    palette_dpi = dpi;
    previous = r;
    last_gap = gap;
    had_back = show_back;
    if (glyph_size != icon_size) {
        HFONT glyph=w_theme_font(L"Segoe MDL2 Assets",icon_size,FW_NORMAL);
        if (glyph) {
            w_font(&glyph_font,glyph,NULL,0);
            glyph_size=icon_size;
        }
    }
    if (row_height!=row && w_native_list_row_height(list,row)) row_height=row;
    int pad = 2 * gap, width = max(1, r.right - 2*pad);
    int left = group ? pad + button + gap : pad;
    int header=max(button,title_line),field=max(button,body_line+2);
    int action_top=gap+(header-button)/2;
    search_box = (RECT){pad,header+2*gap,r.right-pad,header+2*gap+field};
    int results_top = search_box.bottom + gap, footer_top = max(results_top,r.bottom-footer_line-gap);
    UINT place = SWP_NOZORDER | SWP_NOACTIVATE;
    SetWindowPos(back, NULL, pad, action_top, button, button, place);
    SetWindowPos(heading, NULL, left, gap, max(1,r.right-left-pad-button-gap), header, place);
    SetWindowPos(close_button, NULL, r.right-pad-button, action_top, button, button, place);
    int edit_height=body_line;
    SetWindowPos(edit, NULL, pad+button, search_box.top+(field-edit_height)/2,
                 max(1,width-button-gap), edit_height, place);
    w_native_edit_frame(edit,&search_box);
    SetWindowPos(list, NULL, pad, results_top, width, max(1,footer_top-results_top-gap), place);
    RECT viewport;
    GetClientRect(list, &viewport);
    if (ListView_GetColumnWidth(list,0) != max(1,(int)viewport.right))
        ListView_SetColumnWidth(list,0,max(1,(int)viewport.right));
    SetWindowPos(empty_text, NULL, pad, results_top, width, button, place);
    SetWindowPos(footer, NULL, pad, footer_top, width, footer_line, place);
    w_navigation_hint();
    RedrawWindow(panel,NULL,NULL,RDW_INVALIDATE|RDW_ALLCHILDREN);
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
    if (position == text) return true;
    wchar_t ch = position[-1];
    if (ch < 128)
        return !(ch >= L'a' && ch <= L'z') && !(ch >= L'A' && ch <= L'Z') && !(ch >= L'0' && ch <= L'9');
    return !IsCharAlphaNumericW(ch);
}

static int w_match_tier(const wchar_t *text, const wchar_t *query, const w_query_word *words,
                        int count, int *cost) {
    if (!wcscmp(text, query))
        return 0;
    const wchar_t *hit = wcsstr(text, query);
    if (!hit && (!query[1] || !wcschr(text,*query))) return -1;
    *cost = (int)wcslen(text);
    if (hit == text)
        return 1;
    for (const wchar_t *p = hit; p; p = wcsstr(p + 1, query)) {
        if (w_word_start(text, p)) {
            *cost += (int)(p - text);
            return 2;
        }
    }
    if (count > 1) {
        const wchar_t *used[(palette_text_capacity+1)/2];
        int i;
        for (i = 0; i < count; i++) {
            const wchar_t *p = wcschr(text,*words[i].start);
            while (p) {
                int j=0;
                while (j<i && used[j]!=p) ++j;
                if (j==i && w_word_start(text,p) && !wcsncmp(p,words[i].start,words[i].length)) break;
                p = wcschr(p+1,*words[i].start);
            }
            if (!p)
                break;
            used[i] = p;
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
        if (*p == *initials && w_word_start(text,p))
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

static int w_keyword_match(const wchar_t *text, const wchar_t *query, const w_query_word *words,
                           int count, int *cost) {
    // Exact keywords outrank partial labels. Windows supplies keywords in no relevance order.
    size_t len=wcslen(query);
    bool prefix=false;
    for (const wchar_t *p=wcsstr(text,query); p; p=wcsstr(p+1,query)) {
        if (!w_word_start(text,p)) continue;
        if (!p[len] || !IsCharAlphaNumericW(p[len])) return 0;
        prefix=true;
    }
    if (prefix) return 1;
    return w_match_tier(text,query,words,count,cost);
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

// Source indices can change when the catalog inserts or reorders applications.
typedef struct {
    int source;
    enum w_palette_group group;
    uint64_t identity;
    wchar_t label[palette_text_capacity], alias[palette_text_capacity];
} w_result_key;

static w_result_key w_result_at(int index) {
    w_result_key key={.source=w_source(index)};
    if (key.source>=0) {
        const w_cached_row *row=&rows[key.source];
        key.group=row->entry.group;
        key.identity=row->entry.identity;
        if (!key.identity) {
            wcscpy(key.label,row->entry.label);
            wcscpy(key.alias,row->alias);
        }
    }
    return key;
}

static int w_locate_result(const w_result_key *key) {
    if (key->source==no_result) return -1;
    for (int i=0;i<result_count;++i) {
        int source=results[i].row;
        if (source<0 || key->source<0) {
            if (source==key->source) return i;
        } else {
            const w_cached_row *row=&rows[source];
            if (row->entry.group==key->group &&
                (key->identity ? row->entry.identity==key->identity :
                 !wcscmp(row->entry.label,key->label) && !wcscmp(row->alias,key->alias))) return i;
        }
    }
    return -1;
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
                    int extra=0, match=w_keyword_match(r->entry.keywords,query,words,word_count,&extra);
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

static void w_cancel_press(void) {
    if (pressed_index<0) return;
    int previous=pressed_index;
    pressed_index=-1;
    pressed_inside=false;
    ListView_RedrawItems(list,previous,previous);
    if (GetCapture()==list) ReleaseCapture();
}

static void w_filter(bool reset_view) {
    if (filtering || !list) return;
    filtering = true;
    manual_selection = false;
    w_cancel_press();
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
    if (ListView_GetItemCount(list) != result_count)
        ListView_SetItemCountEx(list, result_count, LVSICF_NOSCROLL | LVSICF_NOINVALIDATEALL);
    if (reset_view) w_select(0);
    if (!result_count) SetWindowTextW(empty_text, message);
    if (!!(GetWindowLongPtrW(list,GWL_STYLE) & WS_VISIBLE) != !!result_count)
        ShowWindow(list,result_count ? SW_SHOWNA : SW_HIDE);
    if (!!(GetWindowLongPtrW(empty_text,GWL_STYLE) & WS_VISIBLE) == !!result_count)
        ShowWindow(empty_text,result_count ? SW_HIDE : SW_SHOWNA);
    RECT viewport;
    GetClientRect(list,&viewport);
    int width = max(1,(int)viewport.right);
    if (ListView_GetColumnWidth(list,0) != width) ListView_SetColumnWidth(list,0,width);
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
    InvalidateRect(edit,NULL,FALSE);
    filtering = false;
    w_filter(true);
    if (previous && !next) {
        for (int i = 0; i < result_count; ++i)
            if (results[i].row == -(int)previous) { w_select(i); break; }
    }
    w_palette_size();
}

static void w_glyph(HDC dc, wchar_t glyph, RECT r, COLORREF color) {
    HGDIOBJ old=SelectObject(dc,glyph_font);
    if (!old) return;
    SetTextColor(dc, color);
    DrawTextW(dc, &glyph, 1, &r, DT_SINGLELINE|DT_CENTER|DT_VCENTER|DT_NOPREFIX);
    SelectObject(dc,old);
}

static bool w_app_icon(HDC dc, int index, RECT box) {
    if (!app_icons || index <= 0) return false;
    int size = glyph_size;
    IMAGELISTDRAWPARAMS image = {.cbSize=sizeof(image), .himl=app_icons, .i=index-1, .hdcDst=dc,
        .x=(box.left+box.right-size)/2, .y=(box.top+box.bottom-size)/2, .cx=size, .cy=size,
        .rgbBk=CLR_NONE, .rgbFg=CLR_NONE, .fStyle=ILD_TRANSPARENT|ILD_SCALE};
    return ImageList_DrawIndirect(&image) != FALSE;
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
    UINT state=ListView_GetItemState(list,index,LVIS_SELECTED|LVIS_FOCUSED);
    bool selected=(state&LVIS_SELECTED)!=0;
    int hover=w_hover_amount(list,index);
    bool highlighted=selected || (hover && w_theme_high_contrast());
    COLORREF ink=w_hover_row(dc,&r,selected,hover,index==pressed_index && pressed_inside);
    if (selected) {
        RECT mark=r;
        mark.right=mark.left+max(1,px(2));
        SetDCBrushColor(dc,w_theme_high_contrast() ? ink : w_theme_accent());
        FillRect(dc,&mark,GetStockObject(DC_BRUSH));
    }
    SetBkMode(dc, TRANSPARENT);
    RECT label = r;
    int gap = w_theme_spacing(palette_dpi);
    label.left += 2*gap;
    label.right -= 2*gap;
    RECT icon = r;
    icon.left += gap;
    icon.right = icon.left + w_theme_button_size(palette_dpi);
    if (glyph) label.left = icon.right + gap;
    if (category) {
        RECT arrow = label;
        arrow.left = arrow.right-glyph_size;
        w_glyph(dc, 0xe76c, arrow, highlighted ? ink : w_theme_accent());
        label.right = arrow.left-gap;
        if (label.right-label.left>=group_width[kind].label+2*gap+group_width[kind].hint) {
            RECT hint=label;
            hint.left=hint.right-group_width[kind].hint;
            SelectObject(dc,small_font);
            SetTextColor(dc,highlighted ? ink : w_theme_hint());
            w_theme_draw_text(dc,groups[kind].hint,-1,&hint,DT_RIGHT|DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
            label.right=hint.left-gap;
        }
    } else if (selected && w_can_run_admin(source)) {
        int available=(label.right-label.left)/2-glyph_size-2*gap;
        int variant=admin_width[0]<=available ? 0 : 1;
        if (admin_width[variant]<=available) {
            SelectObject(dc,small_font);
            RECT key=label; key.left=key.right-admin_width[variant];
            RECT shield={key.left-gap-glyph_size,key.top,key.left-gap,key.bottom};
            SetTextColor(dc,ink);
            w_theme_draw_text(dc,admin_keys[variant],-1,&key,DT_RIGHT|DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX);
            w_glyph(dc,0xea18,shield,ink);
            label.right=shield.left-gap;
        }
    } else if (kind && !group && group_width[kind].context+gap<=(label.right-label.left)/2) {
        SelectObject(dc,small_font);
        RECT context=label;
        context.left=context.right-group_width[kind].context;
        SetTextColor(dc,highlighted ? ink : w_theme_accent());
        w_theme_draw_text(dc,groups[kind].label,-1,&context,DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX);
        label.right=context.left-gap;
    }
    SelectObject(dc, text_font);
    SetTextColor(dc, ink);
    w_theme_draw_text(dc, w_label(source), -1, &label, DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
    if (glyph && (!entry || !w_app_icon(dc,entry->system_icon,icon)))
        w_glyph(dc, glyph, icon, highlighted ? ink : w_theme_accent());
    if ((state&LVIS_FOCUSED) && GetFocus()==list && !(SendMessageW(list,WM_QUERYUISTATE,0,0)&UISF_HIDEFOCUS)) {
        InflateRect(&r,-px(2),-px(2));
        DrawFocusRect(dc,&r);
    }
    RestoreDC(dc, saved);
}

bool w_palette_shown(void) {
    return panel && !closing && IsWindowVisible(panel);
}

const w_popup_anchor *w_palette_anchor(void) {
    return placement.anchor.bar ? &placement.anchor : NULL;
}

void w_palette_focus(void) {
    if (w_palette_shown()) {
        if (GetForegroundWindow() != panel)
            SetForegroundWindow(panel);
        if (GetForegroundWindow() == panel)
            SetFocus(edit);
    }
}

static void w_restore_window(HWND previous);

static void w_dismiss(bool restore_focus) {
    if (closing) return;
    w_cancel_press();
    pending_ticket = 0;
    pending_restore = restore_focus && GetForegroundWindow() == panel ? return_to : NULL;
    return_to = NULL;
    HWND capture = GetCapture();
    if (capture == panel || IsChild(panel, capture))
        ReleaseCapture();
    closing = true;
    if (IsWindowVisible(panel)) {
        SetWindowLongPtrW(panel, GWL_EXSTYLE, GetWindowLongPtrW(panel,GWL_EXSTYLE) | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
        if (pending_restore) w_restore_window(pending_restore);
        if (GetForegroundWindow()!=panel) {
            pending_restore=NULL;
            if (w_layer_fade_to(panel,&fade,0,w_palette_timing(false),motion_enabled)) return;
        }
    }
    KillTimer(panel, layer_fade_timer);
    fade.start = 0;
    ShowWindow(panel, SW_HIDE);
    if (pending_restore)
        PostMessageW(panel, restore_foreground, 0, 0);
}

static bool w_palette_contains(HWND hit) {
    if (!hit) return false;
    HWND root = GetAncestor(hit, GA_ROOT);
    if (root == panel || GetAncestor(hit, GA_ROOTOWNER) == panel) return true;
    // Some input methods show a separate, non-activating candidate window.
    return (root == ImmGetDefaultIMEWnd(edit) || (GetClassLongPtrW(root,GCL_STYLE) & CS_IME)) &&
           (composing || w_native_edit_ime_active(edit));
}

void w_palette_outside(HWND hit) {
    if (!placement.anchor.bar || !w_palette_shown() || w_palette_contains(hit)) return;
    HWND root = GetAncestor(hit, GA_ROOT);
    wchar_t name[32];
    if (root && w_os_same_process(root) && GetClassNameW(root, name, count_of(name)) && !wcscmp(name,L"WinwmBar"))
        return;
    w_dismiss(false);
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
    if (foreground && foreground!=panel && foreground != GetDesktopWindow() && IsWindowVisible(foreground) &&
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
    if (msg==WM_LBUTTONDOWN || msg==WM_LBUTTONUP || (msg==WM_MOUSEMOVE && pressed_index>=0)) {
        LVHITTESTINFO hit = {.pt={GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}};
        int index = ListView_HitTest(w, &hit);
        if (msg == WM_LBUTTONDOWN) {
            w_cancel_press();
            if (w_source(index)!=no_result) {
                w_select(index); manual_selection=true;
                SetCapture(w);
                if (GetCapture()!=w) return 0;
                pressed_index=index; pressed_inside=true;
                ListView_RedrawItems(w,index,index);
            }
            return 0;
        } else if (msg==WM_LBUTTONUP) {
            bool pick=index>=0 && index==pressed_index;
            w_cancel_press();
            if (pick && w_palette_shown()) { w_select(index); w_accept(false); }
            return 0;
        } else if (pressed_inside!=(index==pressed_index)) {
            pressed_inside=index==pressed_index;
            ListView_RedrawItems(w,pressed_index,pressed_index);
        }
    } else if (msg == WM_CANCELMODE || msg == WM_CAPTURECHANGED || msg == WM_LBUTTONDBLCLK) {
        w_cancel_press();
    } else if (msg == WM_CHAR && wp >= L' ' && GetKeyState(VK_CONTROL) >= 0) {
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
            w_theme_draw_text(dc,search_hint,-1,&box,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
            RestoreDC(dc,saved);
            if (msg == WM_PAINT) ReleaseDC(w,dc);
        }
        return result;
    }
    return DefSubclassProc(w, msg, wp, lp);
}

static LRESULT CALLBACK w_palette_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg==WM_FONTCHANGE || (msg==WM_SETTINGCHANGE && wp==SPI_SETNONCLIENTMETRICS)) {
        font_dpi=0;
        w_palette_size();
        if (placement.anchor.bar && w_palette_shown()) w_palette_reanchor(&placement.anchor);
    }
    if (msg==winwm_motion_updated || msg==WM_SYSCOLORCHANGE ||
        (msg==WM_SETTINGCHANGE && (wp==SPI_SETCLIENTAREAANIMATION || w_appearance_setting(wp,lp)))) {
        motion_enabled=!placement.follow_theme_motion || w_theme_motion_enabled();
        if (fade.start && !w_layer_fade_to(w,&fade,fade.target,w_palette_timing(!closing),motion_enabled) && closing)
            ShowWindow(w,SW_HIDE);
    }
    // Keep WS_THICKFRAME/WS_SYSMENU for native mouse and keyboard resizing.
    // Both palette modes paint their own border into the full window rectangle.
    if (msg == WM_NCCALCSIZE || msg == WM_NCPAINT) return 0;
    if (msg == WM_NCACTIVATE) return TRUE;
    if (msg == WM_ERASEBKGND) return 1;
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
    if (msg==winwm_theme_updated && list) {
        ListView_SetBkColor(list,w_theme_background());
        ListView_SetTextBkColor(list,w_theme_background());
        return 0;
    }
    if (msg == WM_PAINT || msg == WM_PRINTCLIENT) {
        PAINTSTRUCT ps;
        HDC dc = msg == WM_PAINT ? BeginPaint(w, &ps) : (HDC)wp;
        RECT area;
        GetClientRect(w, &area);
        FillRect(dc,&area,w_theme_brush());
        w_theme_frame(dc,&area,palette_dpi);
        int saved = SaveDC(dc);
        w_theme_input_frame(dc,&search_box,w_hover_amount(edit,0),GetFocus()==edit,IsWindowEnabled(edit));
        RECT icon = search_box;
        icon.right = icon.left + w_theme_button_size(palette_dpi);
        SetBkMode(dc, TRANSPARENT);
        w_glyph(dc, 0xe721, icon, w_theme_accent());
        RestoreDC(dc,saved);
        if (msg == WM_PAINT) EndPaint(w, &ps);
        return 0;
    }
    if (msg == WM_NCHITTEST) {
        if (placement.anchor.bar) return HTCLIENT;
        POINT p = {GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
        ScreenToClient(w,&p);
        HWND child = ChildWindowFromPointEx(w,p,CWP_SKIPINVISIBLE);
        if (child && child!=w && child!=heading && child!=footer) return HTCLIENT;
        LRESULT hit=w_popup_resize_hit(w,p);
        return hit==HTCLIENT && p.y<search_box.top ? HTCAPTION : hit;
    }
    if (msg == WM_GETMINMAXINFO) {
        UINT dpi = GetDpiForWindow(w);
        int button=w_theme_button_size(dpi), gap=w_theme_spacing(dpi);
        int row,chrome=w_viewport_chrome(dpi,&row);
        int minimum=chrome+row;
        ((MINMAXINFO *)lp)->ptMinTrackSize = (POINT){max(MulDiv(240,dpi,96),2*button+7*gap),minimum};
        return 0;
    }
    if (msg == WM_LBUTTONDOWN && PtInRect(&search_box,(POINT){GET_X_LPARAM(lp),GET_Y_LPARAM(lp)})) {
        SetFocus(edit);
        return 0;
    }
    if (msg == WM_SETFOCUS) { if (edit) SetFocus(edit); return 0; }
    if (msg == WM_COMMAND && lp) {
        if ((HWND)lp == edit && HIWORD(wp) == EN_CHANGE && !filtering) {
            pending_ticket = 0;
            launch_status = NULL;
            w_navigation_hint();
            w_filter(true);
        }
        if ((HWND)lp == back && HIWORD(wp) == BN_CLICKED) { w_route(palette_flat); SetFocus(edit); }
        if ((HWND)lp == close_button && HIWORD(wp) == BN_CLICKED) w_dismiss(true);
        return 0;
    }
    if (msg == WM_SIZE) { w_palette_size(); return 0; }
    if (msg == WM_DPICHANGED) {
        RECT r = placement.anchor.bar ? w_button_bounds(&placement.anchor, HIWORD(wp)) : *(RECT *)lp;
        SetWindowPos(w, NULL, r.left,r.top,r.right-r.left,r.bottom-r.top,SWP_NOACTIVATE|SWP_NOZORDER);
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
    if (msg == WM_TIMER && wp == layer_fade_timer) {
        if (fade.start && !w_layer_fade_tick(w,&fade,motion_enabled) && closing) ShowWindow(w,SW_HIDE);
        return 0;
    }
    if (msg == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE) PostMessageW(w,check_deactivation,0,0);
    if (msg == check_deactivation) {
        if (w_palette_shown() && !w_palette_contains(GetForegroundWindow())) w_dismiss(false);
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
    if (close_button) w_icon_button(close_button,L"\xe711",L"Close");
    // The static label precedes the edit in the child order for its accessible name.
    HWND search_name = CreateWindowW(L"STATIC",L"Search",visible,0,0,0,0,panel,NULL,inst,NULL);
    edit = CreateWindowExW(0,L"EDIT",L"",visible|WS_TABSTOP|ES_AUTOHSCROLL,0,0,0,0,panel,NULL,inst,NULL);
    list = CreateWindowExW(0,WC_LISTVIEWW,L"Results",visible|WS_TABSTOP|LVS_REPORT|LVS_NOCOLUMNHEADER|
                           LVS_OWNERDATA|LVS_SINGLESEL|LVS_SHOWSELALWAYS,
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
    w_theme_native_controls(panel);
    w_palette_size();
    return true;
}


static bool w_cache(const w_palette_entry *entries, int count, const w_palette_apps *apps) {
    int app_count = apps ? apps->count : 0;
    if (count < 0 || app_count < 0 || (!entries && count) ||
        (int64_t)count+app_count > INT_MAX-palette_group_count-1) return false;
    w_cancel_press();
    int total = count+app_count;
    if (!results || total != row_count) {
        w_result *slots = realloc(results, ((size_t)total+palette_group_count+1)*sizeof(*slots));
        if (!slots) return false;
        results = slots;
        result_count = 0;
        if (total) {
            w_cached_row *next = realloc(rows,(size_t)total*sizeof(*next));
            if (!next) { row_count = 0; return false; }
            rows = next;
        } else { free(rows); rows = NULL; }
    }
    memset(in_group,0,sizeof(in_group));
    grouped = false;
    for (int i = 0; i < total; ++i) {
        w_cached_row *r = &rows[i];
        w_palette_entry app = {0};
        const w_palette_entry *source;
        if (i < count) source = &entries[i];
        else {
            apps->read(i-count,&app);
            source = &app;
        }
        bool same_label = i < row_count && !wcsncmp(r->entry.label,source->label,palette_text_capacity-1);
        r->entry = *source;
        r->entry.label[palette_text_capacity-1] = 0;
        if ((unsigned)r->entry.group >= palette_group_count) r->entry.group = palette_flat;
        ++in_group[r->entry.group];
        grouped |= r->entry.group != palette_flat;
        if (!same_label) w_search_text(r->normalized, r->entry.label);
        r->alias[0] = 0;
        if (r->entry.alias) {
            wchar_t alias[palette_text_capacity];
            lstrcpynW(alias,r->entry.alias,count_of(alias));
            w_search_text(r->alias,alias);
        }
    }
    row_count = total;
    result_count = 0;
    return true;
}

void w_palette_open(const w_palette_site *site, const wchar_t *title, const w_palette_entry *entries, int count,
                    palette_callback cb, HANDLE app_images, palette_line_callback line_callback, const w_palette_apps *apps) {
    pending_restore = NULL;
    bool preserve = w_palette_visible(title);
    if (!panel && !w_create_palette()) return;
    int old_index = preserve && manual_selection ? w_selected_index() : -1;
    int top = preserve ? ListView_GetTopIndex(list) : 0;
    bool selection_visible = old_index >= top && old_index < top + ListView_GetCountPerPage(list);
    RECT old_bounds;
    w_result_key selected=w_result_at(old_index);
    w_result_key first=w_result_at(top>0 && ListView_GetItemRect(list,top,&old_bounds,LVIR_BOUNDS) ? top : -1);
    if (!w_cache(entries,count,apps) && (!apps || !apps->count || !w_cache(entries,count,NULL))) {
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
    app_icons = (HIMAGELIST)app_images;
    lstrcpynW(root_title,title,count_of(root_title));
    if (ListView_GetBkColor(list) != w_theme_background()) {
        ListView_SetBkColor(list,w_theme_background());
        ListView_SetTextBkColor(list,w_theme_background());
    }
    if (!preserve) SetWindowTextW(panel,title);
    if (preserve && (site->monitor != placement.monitor || !w_popup_same_source(&site->anchor,&placement.anchor) ||
        !EqualRect(&site->anchor.button, &placement.anchor.button) || strcmp(site->anchor.edge, placement.anchor.edge))) {
        w_palette_position(site);
        w_palette_focus();
    }
    if (preserve && (!group || in_group[group])) {
        w_filter(false);
        int restored=w_locate_result(&selected), target=max(0,restored);
        if (result_count && target != w_selected_index())
            ListView_SetItemState(list,target,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
        int first_index=w_locate_result(&first);
        RECT bounds;
        if (first_index>=0 && ListView_GetItemRect(list,first_index,&bounds,LVIR_BOUNDS) && bounds.top!=old_bounds.top)
            ListView_Scroll(list,0,bounds.top-old_bounds.top);
        if (restored>=0 && selection_visible && (target!=old_index || (first_index>=0 && first_index!=top)))
            ListView_EnsureVisible(list,target,FALSE);
        manual_selection = restored>=0;
        return;
    }
    group = palette_flat;
    w_route(palette_flat);
    if (preserve) return;
    opening_rows=result_count;
    if (!w_palette_shown()) return_to = GetForegroundWindow();
    w_palette_position(site);
    w_palette_focus();
}

void w_palette_reanchor(const w_popup_anchor *anchor) {
    if (!w_palette_shown() || !placement.anchor.bar) return;
    if (!anchor) { w_dismiss(false); return; }
    placement.anchor=*anchor;
    placement.monitor=MonitorFromWindow(anchor->bar,MONITOR_DEFAULTTONEAREST);
    RECT old, next=w_button_bounds(anchor,GetDpiForWindow(anchor->bar));
    if (!GetWindowRect(panel,&old) || !EqualRect(&old,&next))
        SetWindowPos(panel,NULL,next.left,next.top,next.right-next.left,next.bottom-next.top,SWP_NOACTIVATE|SWP_NOZORDER);
}

void w_palette_position(const w_palette_site *site) {
    MONITORINFO mi = {.cbSize = sizeof(mi)};
    HMONITOR monitor = site->monitor;
    if (!monitor || !GetMonitorInfoW(monitor,&mi)) {
        monitor = MonitorFromWindow(GetForegroundWindow(),MONITOR_DEFAULTTOPRIMARY);
        GetMonitorInfoW(monitor,&mi);
    }
    UINT dpi=96, vertical=96;
    GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi, &vertical);
    bool attached = site->anchor.bar != NULL;
    RECT bounds;
    if (attached) {
        bounds = w_button_bounds(&site->anchor, dpi);
    } else {
        int width = MulDiv(520,dpi,96), height = w_initial_height(dpi,mi.rcWork.bottom-mi.rcWork.top);
        UINT old_dpi = GetDpiForWindow(panel);
        RECT current;
        if (!placement.anchor.bar && w_palette_shown() && old_dpi && GetWindowRect(panel,&current)) {
            width = MulDiv(current.right-current.left,dpi,old_dpi);
            height = MulDiv(current.bottom-current.top,dpi,old_dpi);
        }
        width = min(mi.rcWork.right-mi.rcWork.left,width);
        height = min(mi.rcWork.bottom-mi.rcWork.top,height);
        bounds.left = mi.rcWork.left + (mi.rcWork.right-mi.rcWork.left-width)/2;
        bounds.top = mi.rcWork.top + (mi.rcWork.bottom-mi.rcWork.top-height)/2;
        bounds.right = bounds.left + width;
        bounds.bottom = bounds.top + height;
    }
    bool hidden = !IsWindowVisible(panel);
    bool opening = hidden || closing;
    bool was_attached=placement.anchor.bar!=NULL;
    closing = false;
    placement = *site;
    LONG_PTR style = GetWindowLongPtrW(panel,GWL_STYLE);
    LONG_PTR next_style=attached ? style & ~(LONG_PTR)WS_THICKFRAME : style | WS_THICKFRAME;
    LONG_PTR ex=GetWindowLongPtrW(panel,GWL_EXSTYLE);
    LONG_PTR next_ex=ex & ~(LONG_PTR)(WS_EX_TRANSPARENT|WS_EX_NOACTIVATE);
    if (next_style!=style) SetWindowLongPtrW(panel,GWL_STYLE,next_style);
    if (next_ex!=ex) SetWindowLongPtrW(panel,GWL_EXSTYLE,next_ex);
    if (attached!=was_attached) w_theme_material(panel);
    if (opening || attached != was_attached) {
        if (hidden) fade.value=0;
        motion_enabled=!placement.follow_theme_motion || w_theme_motion_enabled();
        w_layer_fade_to(panel,&fade,255,w_palette_timing(true),opening && motion_enabled);
    }
    SetWindowPos(panel,HWND_TOPMOST,bounds.left,bounds.top,bounds.right-bounds.left,bounds.bottom-bounds.top,
                 SWP_SHOWWINDOW|SWP_NOACTIVATE|(next_style != style || next_ex != ex ? SWP_FRAMECHANGED : 0));
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
        (msg->message != WM_KEYDOWN && msg->message != WM_SYSKEYDOWN) || composing ||
        (msg->hwnd==edit && w_native_ime_key(msg))) return false;
    bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    if ((msg->lParam & (1L<<30)) && (msg->wParam==VK_RETURN || msg->wParam==VK_ESCAPE) &&
        !ctrl && msg->message==WM_KEYDOWN) return true;
    if (GetKeyState(VK_MENU) < 0) {
        if (group && msg->wParam == VK_LEFT) { w_route(palette_flat); SetFocus(edit); return true; }
        return false;
    }
    if (msg->message == WM_SYSKEYDOWN) return false;
    if (msg->wParam == VK_RETURN && shift && !ctrl && run_line && (msg->hwnd == edit || msg->hwnd == list)) {
        if (!(msg->lParam & (1L << 30))) w_accept(true);
        return true;
    }
    if (ctrl && !shift) {
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
