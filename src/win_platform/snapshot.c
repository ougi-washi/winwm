#include "win_platform/win_platform.h"
#include "model/model.h"
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
enum { cache_limit = 512 * 1024, cache_format = 14 };

////////////////
//~ Snapshot

typedef struct w_cache_stream {
    unsigned char *data;
    size_t pos, end;
    bool writing, ok;
} w_cache_stream;

typedef struct w_cache_header {
    uint32_t magic, format, bytes, checksum;
} w_cache_header;

////////////////
//~ Snapshot Functions

static uint32_t w_checksum(const unsigned char *s, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++)
        h = (h ^ s[i]) * 16777619u;
    return h;
}

static void w_field(w_cache_stream *s, void *p, size_t n) {
    if (!s->ok || n > s->end - s->pos) {
        s->ok = false;
        return;
    }
    if (s->writing)
        memcpy(s->data + s->pos, p, n);
    else
        memcpy(p, s->data + s->pos, n);
    s->pos += n;
}

static void w_string_field(w_cache_stream *s, char *p, size_t cap) {
    uint32_t n = s->writing ? (uint32_t)strlen(p) : 0;
    w_field(s, &n, sizeof(n));
    if (n >= cap) {
        s->ok = false;
        return;
    }
    w_field(s, p, n);
    if (!s->writing && s->ok) {
        if (memchr(p, 0, n))
            s->ok = false;
        p[n] = 0;
    }
}

static void w_contents(w_cache_stream *s, w_desktop_config *c) {
#define string_member(member) w_string_field(s, member, sizeof(member))
    string_member(c->file_browser);
    string_member(c->terminal);
    string_member(c->task_manager);
    string_member(c->editor);
    string_member(c->bar_left);
    string_member(c->bar_center);
    string_member(c->bar_right);
    string_member(c->edge);
    w_field(s, &c->bar_height, sizeof(int));
    w_field(s, &c->gap, sizeof(int));
    w_field(s, &c->outer_gap, sizeof(int));
    w_field(s, &c->inactive_alpha, 1);
    w_field(s, &c->inactive_border, 4);
    unsigned char follow = s->writing && c->follow_mouse;
    w_field(s, &follow, 1);
    c->follow_mouse = follow != 0;
    w_field(s, &c->background, 4);
    w_field(s, &c->foreground, 4);
    w_field(s, &c->accent, 4);
    w_field(s, &c->border_size, sizeof(int));
    w_field(s, &c->rounding, sizeof(int));
    w_field(s,&c->panel_material.opacity,sizeof(int));
    unsigned char panel_flags = s->writing ? c->panel_material.blur | (c->panel_material.shadow << 1) : 0;
    w_field(s,&panel_flags,sizeof(panel_flags));
    if ((unsigned)c->panel_material.opacity > 255 || panel_flags > 3) s->ok = false;
    c->panel_material.blur = (panel_flags & 1) != 0;
    c->panel_material.shadow = (panel_flags & 2) != 0;
    unsigned char resize = c->resize_on_border;
    w_field(s, &resize, 1);
    c->resize_on_border = resize != 0;
    unsigned char animations = c->animations;
    w_field(s, &animations, 1);
    if (animations > motion_system)
        s->ok = false;
    c->animations = (enum motion_preference)animations;
    w_border_gradient *gradients[] = {&c->active_gradient, &c->inactive_gradient};
    for (size_t i = 0; i < count_of(gradients); i++) {
        w_border_gradient *g = gradients[i];
        unsigned char enabled = g->enabled;
        w_field(s, &enabled, 1);
        g->enabled = enabled != 0;
        w_field(s, &g->end, 4);
        w_field(s, &g->angle, sizeof(int));
        if (g->angle < 0 || g->angle > 360)
            s->ok = false;
    }
    for (int i = 0; i < motion_count; i++) {
        w_transition *m = &c->motion[i];
        w_field(s, &m->ms, sizeof(int));
        w_field(s, &m->curve, sizeof(w_curve));
        w_field(s, &m->style, sizeof(m->style));
        w_field(s, &m->percent, sizeof(int));
        if ((unsigned)m->style > style_right || m->percent < -1 || m->percent > 100)
            s->ok = false;
        double points[] = {m->curve.x1, m->curve.y1, m->curve.x2, m->curve.y2};
        for (size_t j = 0; j < count_of(points); j++)
            if (!isfinite(points[j]) || points[j] < (j%2 ? -4 : 0) || points[j] > (j%2 ? 4 : 1))
                s->ok = false;
        if (m->ms < 0 || m->ms > 3000)
            s->ok = false;
    }
    if (c->bar_height < 16 || c->bar_height > 128 || c->gap < 0 || c->gap > 128 || c->outer_gap < 0 ||
        c->outer_gap > 128 || c->border_size < 0 || c->border_size > 16 || c->rounding < 0 || c->rounding > 32) {
        s->ok = false;
        return;
    }
    int n = c->binding_count;
    w_field(s, &n, sizeof(n));
    if (n < 0 || n > binding_limit) {
        s->ok = false;
        return;
    }
    if (!s->writing)
        c->binding_count = 0;
    for (int i = 0; s->ok && i < n; i++) {
        w_key_binding *b = &c->bindings[i];
        char chord[96] = {0}, arg[1024] = {0};
        int kind = 0;
        if (s->writing) {
            strcpy(chord, b->chord);
            strcpy(arg, b->action.arg);
            kind = b->action.kind;
        }
        string_member(chord);
        w_field(s, &kind, sizeof(kind));
        string_member(arg);
        if (!s->writing && s->ok) {
            char action[1100];
            snprintf(action, sizeof(action), "%s%s%s", w_command_name((enum command_kind)kind), *arg ? " " : "",
                     arg);
            if (!w_config_bind(c, chord, action))
                s->ok = false;
        }
    }
    w_field(s, &c->app_count, sizeof(int));
    if (c->app_count < 0 || c->app_count > app_limit) {
        s->ok = false;
        return;
    }
    for (int i = 0; i < c->app_count; i++) {
        string_member(c->apps[i].name);
        string_member(c->apps[i].path);
    }
#undef string_member
}

static void w_bar_preferences(w_cache_stream *s, w_desktop_config *c) {
    w_field(s,&c->bar_tooltip_delay,sizeof(int));
    w_field(s,&c->bar_tooltip_duration,sizeof(int));
    w_field(s,&c->bar_workspace_count,sizeof(int));
    w_field(s,&c->style_count,sizeof(int));
    if (c->bar_tooltip_delay < 0 || c->bar_tooltip_delay > 10000 || c->bar_tooltip_duration < 0 ||
        c->bar_tooltip_duration > 60000 || (unsigned)c->bar_workspace_count > workspace_limit ||
        (unsigned)c->style_count > count_of(c->styles)) {
        s->ok = false;
        return;
    }
    for (int i=0; s->ok && i<workspace_limit; ++i)
        w_string_field(s,c->workspace_names[i],sizeof(c->workspace_names[i]));
    for (int i=0; s->ok && i<c->style_count; ++i) {
        w_module_style *style = &c->styles[i];
        unsigned char flags = (style->colored ? 1 : 0) | (style->hide_tooltip ? 2 : 0);
        w_field(s,&flags,1);
        w_field(s,&style->color,sizeof(style->color));
        w_field(s,&style->display,sizeof(style->display));
        style->colored = (flags & 1) != 0;
        style->hide_tooltip = (flags & 2) != 0;
        w_string_field(s,style->name,sizeof(style->name));
        w_string_field(s,style->format,sizeof(style->format));
        w_string_field(s,style->tooltip,sizeof(style->tooltip));
        if (flags > 3 || (unsigned)style->display > module_icon_text ||
            !*style->name || strpbrk(style->name," \t") || !w_config_modules_valid(style->name)) s->ok = false;
        char *actions[] = {style->action, style->right_click, style->middle_click, style->scroll_up, style->scroll_down};
        for (size_t j=0; s->ok && j<count_of(actions); ++j) {
            w_string_field(s,actions[j],sizeof(style->action));
            w_command cmd;
            if (s->ok && *actions[j] && !w_command_parse(actions[j],&cmd)) s->ok = false;
        }
    }
}

// Desktop startup commands always come from a successful parse of the active profile.
static void w_session_settings(w_cache_stream *s, w_desktop_config *c) {
    w_notification_config *nc = &c->notifications;
    unsigned char prefs = nc->enabled | (nc->capture << 1);
    w_field(s,&prefs,sizeof(prefs));
    nc->enabled = (prefs & 1) != 0;
    nc->capture = (enum w_capture_source)(prefs >> 1);
    int *values[] = {&nc->corner,&nc->duration_ms,&nc->fade_ms,&nc->history_limit,&nc->visible_limit,&nc->margin,&nc->width};
    for (size_t i = 0; i < count_of(values); ++i) w_field(s,values[i],sizeof(int));
    if (!w_config_notifications_valid(nc)) s->ok = false;
    unsigned char master = c->master_default;
    w_field(s,&master,sizeof(master));
    w_field(s,c->mouse_actions,sizeof(c->mouse_actions));
    w_field(s,&c->rule_count,sizeof(c->rule_count));
    if (master > 1 || (unsigned)c->rule_count > count_of(c->rules)) s->ok = false;
    c->master_default = master != 0;
    for (int b=0; b<2; ++b)
        for (int m=0; m<16; ++m)
            if (c->mouse_actions[b][m] > mouse_resize) s->ok = false;
    for (int i=0; s->ok && i<c->rule_count; ++i) {
        w_window_rule *r = &c->rules[i];
        w_string_field(s,r->class_name,sizeof(r->class_name));
        w_string_field(s,r->title,sizeof(r->title));
        w_string_field(s,r->executable,sizeof(r->executable));
        w_field(s,&r->workspace,sizeof(r->workspace));
        w_field(s,&r->output,sizeof(r->output));
        w_field(s,&r->layout,sizeof(r->layout));
        w_field(s,&r->border,sizeof(r->border));
        unsigned char flags = r->colored | (r->floating << 1) | (r->exclude << 2) | (r->sets_floating << 3);
        w_field(s,&flags,sizeof(flags));
        r->colored = flags & 1;
        r->floating = flags & 2;
        r->exclude = flags & 4;
        r->sets_floating = flags & 8;
        if (flags > 15 || r->workspace < -1 || r->workspace >= workspace_limit ||
            r->output < -1 || r->output >= output_limit || r->layout < -1 || r->layout > layout_stack)
            s->ok = false;
    }
    w_field(s,&c->provider_count,sizeof(c->provider_count));
    if ((unsigned)c->provider_count > count_of(c->providers)) s->ok = false;
    for (int i=0; s->ok && i<c->provider_count; ++i) {
        w_provider_config *p = &c->providers[i];
        w_string_field(s,p->name,sizeof(p->name));
        w_string_field(s,p->executable,sizeof(p->executable));
        w_string_field(s,p->action,sizeof(p->action));
        w_field(s,&p->interval,sizeof(p->interval));
        w_field(s,&p->timeout,sizeof(p->timeout));
        unsigned char stream = p->stream;
        w_field(s,&stream,sizeof(stream));
        p->stream = stream != 0;
        w_command action;
        if (stream > 1 || !*p->name || !*p->executable || (unsigned)p->interval > 86400 ||
            p->timeout < 1 || p->timeout > 86400 || (p->stream && p->interval) ||
            (*p->action && !w_command_parse(p->action,&action))) s->ok = false;
    }
    if (s->ok && !s->writing) {
        c->startup_count = 0;
        s->ok = w_config_modules_valid(c->bar_left) && w_config_modules_valid(c->bar_center) &&
                w_config_modules_valid(c->bar_right) && w_config_validate_providers(c,NULL,0);
    }
}

static bool w_cache_path(wchar_t *out, bool create) {
    if (!w_os_data_path(out, os_path_cap, L"startup.bin", create) || out[1] != L':')
        return false;
    wchar_t drive[] = {out[0], L':', L'\\', 0};
    if (GetDriveTypeW(drive) != DRIVE_FIXED)
        return false;
    return true;
}

static bool w_cache_matches(const wchar_t *path, const w_cache_header *header, const unsigned char *data) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return false;
    w_cache_header previous;
    DWORD n;
    LARGE_INTEGER size;
    bool equal = GetFileSizeEx(file,&size) && size.QuadPart == (LONGLONG)sizeof(previous) + header->bytes &&
                 ReadFile(file,&previous,sizeof(previous),&n,NULL) && n == sizeof(previous) &&
                 !memcmp(header,&previous,sizeof(previous));
    unsigned char block[8192];
    for (DWORD offset = 0; equal && offset < header->bytes; offset += n) {
        DWORD length = min(header->bytes - offset, sizeof(block));
        equal = ReadFile(file,block,length,&n,NULL) && n == length && !memcmp(block,data + offset,length);
    }
    CloseHandle(file);
    return equal;
}

void w_os_cache_write(w_desktop_config *cfg) {
    wchar_t path[os_path_cap], tmp[os_path_cap + 48];
    if (!w_cache_path(path, true))
        return;
    w_cache_stream s = {.data = malloc(cache_limit), .end = cache_limit, .writing = true, .ok = true};
    if (!s.data)
        return;
    w_contents(&s, cfg);
    if (s.ok) w_bar_preferences(&s,cfg);
    if (s.ok) w_session_settings(&s,cfg);
    if (s.ok) {
        w_cache_header h = {0x4e444348, cache_format, (uint32_t)s.pos, w_checksum(s.data, s.pos)};
        if (w_cache_matches(path,&h,s.data)) {
            free(s.data);
            return;
        }
        swprintf(tmp, count_of(tmp), L"%ls.%lu.%lu.tmp", path, GetCurrentProcessId(), GetCurrentThreadId());
        HANDLE file = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD n;
            bool ok = WriteFile(file, &h, sizeof(h), &n, NULL) && n == sizeof(h) &&
                      WriteFile(file, s.data, h.bytes, &n, NULL) && n == h.bytes;
            CloseHandle(file);
            if (!ok || !MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                DeleteFileW(tmp);
        }
    }
    free(s.data);
}

bool w_os_cache_read(w_desktop_config *cfg) {
    wchar_t path[os_path_cap];
    if (!w_cache_path(path, false))
        return false;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    DWORD n;
    w_cache_header h;
    bool ok = false;
    if (ReadFile(f, &h, sizeof(h), &n, NULL) && n == sizeof(h) && h.magic == 0x4e444348 &&
        h.format == cache_format &&
        h.bytes > 0 && h.bytes <= cache_limit) {
        w_cache_stream s = {.data = malloc(h.bytes), .end = h.bytes, .ok = true};
        if (s.data && ReadFile(f, s.data, h.bytes, &n, NULL) && n == h.bytes &&
            w_checksum(s.data, n) == h.checksum) {
            w_contents(&s, cfg);
            if (s.ok) w_bar_preferences(&s,cfg);
            if (s.ok) w_session_settings(&s,cfg);
            ok = s.ok && s.pos == s.end;
        }
        free(s.data);
    }
    CloseHandle(f);
    if (!ok)
        w_config_defaults(cfg);
    return ok;
}

void w_os_cache_discard(void) {
    wchar_t path[os_path_cap];
    if (w_cache_path(path, false))
        DeleteFileW(path);
}
