#include "config/config.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

////////////////
//~ Config

static double w_cubic(double t, double a, double b) {
    double u = 1-t;
    return 3*u*u*t*a + 3*u*t*t*b + t*t*t;
}

double w_motion_progress(w_transition timing, uint64_t elapsed) {
    if (timing.ms <= 0 || elapsed >= (unsigned)timing.ms)
        return 1;
    if (!elapsed)
        return 0;
    double time = (double)elapsed / timing.ms;
    double lo = 0, hi = 1;
    for (int i = 0; i < 14; i++) {
        double t = (lo+hi)/2;
        if (w_cubic(t, timing.curve.x1, timing.curve.x2) < time)
            lo = t;
        else
            hi = t;
    }
    return w_cubic((lo+hi)/2, timing.curve.y1, timing.curve.y2);
}

bool w_config_chord(const char *text, unsigned *mods, unsigned *key) {
    char chord[64];
    if (!w_str_copy(chord, sizeof(chord), text))
        return false;
    for (char *p = chord; *p; p++)
        *p = (char)tolower((unsigned char)*p);
    unsigned m = 0, k = 0;
    char *p = chord, *next;
    while ((next = strchr(p, '+')) != NULL) {
        *next = 0;
        unsigned bit = !strcmp(p, "super")   ? 8
                       : !strcmp(p, "shift") ? 4
                       : !strcmp(p, "ctrl")  ? 2
                       : !strcmp(p, "alt")   ? 1
                                             : 0;
        if (!bit || (m & bit))
            return false;
        m |= bit;
        p = next + 1;
    }
    static const struct w_key_name {
        const char *name;
        unsigned key;
    } keys[] = {{"wheel_up", 0x201},
                {"wheel_down", 0x202},
                {"mouse4", 0x203},
                {"mouse5", 0x204},
                {"enter", 13},
                {"escape", 27},
                {"space", 32},
                {"left", 37},
                {"up", 38},
                {"right", 39},
                {"down", 40},
                {"minus", 0xbd},
                {"plus", 0xbb},
                {"tab", 9},
                {"print", 0x2c},
                {"volume_up", 0xaf},
                {"volume_down", 0xae},
                {"volume_mute", 0xad},
                {"delete", 0x2e},
                {"backspace", 8}};
    if (!strncmp(p, "sc_", 3) && p[3]) {
        char *end;
        long code = strtol(p + 3, &end, 16);
        if (!*end && code > 0 && code <= 0x1ff)
            k = (unsigned)code + 0x400;
    } else if (p[0] && !p[1] && isalnum((unsigned char)*p))
        k = (unsigned)toupper((unsigned char)*p);
    else if (*p == 'f' && isdigit((unsigned char)p[1])) {
        char *end;
        long f = strtol(p + 1, &end, 10);
        if (!*end && f >= 1 && f <= 24)
            k = (unsigned)(0x70 + f - 1);
    } else
        for (size_t i = 0; i < count_of(keys); i++)
            if (!strcmp(p, keys[i].name))
                k = keys[i].key;
    if (!k || (m == 3 && (k == 0x7b || k == 0x2e)))
        return false;
    *mods = m;
    *key = k;
    return true;
}

bool w_config_bind(w_desktop_config *c, const char *chord, const char *action) {
    w_key_binding b = {0};
    char text[96];
    if (!w_str_copy(text, sizeof(text), chord) || !w_str_copy(b.chord, sizeof(b.chord), chord))
        return false;
    char *first = text;
    bool prefix = true;
    while (prefix) {
        if (!strncmp(first, "resize:", 7)) {
            b.modal = true;
            first += 7;
        } else if (!strncmp(first, "release:", 8)) {
            b.release = true;
            first += 8;
        } else if (!strncmp(first, "repeat:", 7)) {
            b.repeat = true;
            first += 7;
        } else
            prefix = false;
    }
    char *second = strchr(first, ',');
    if (second) {
        *second++ = 0;
        while (*second == ' ')
            second++;
        if (!w_config_chord(second, &b.next_mods, &b.next_key) || b.release || b.repeat)
            return false;
    }
    if (!w_config_chord(first, &b.mods, &b.key) ||
        (!b.mods && !b.modal && b.key != 0x2c && !(b.key >= 0xad && b.key <= 0xaf)) ||
        (b.release && b.repeat))
        return false;
    bool remove = !strcmp(action, "unbound");
    if (!remove && !w_command_parse(action, &b.action))
        return false;
    for (int i = 0; i < c->binding_count;) {
        w_key_binding *existing = &c->bindings[i];
        if (existing->mods == b.mods && existing->key == b.key && existing->modal == b.modal &&
            existing->next_key == b.next_key && existing->next_mods == b.next_mods) {
            if (remove) {
                *existing = c->bindings[--c->binding_count];
                continue;
            }
            if (existing->release == b.release) {
                *existing = b;
                return true;
            }
        }
        ++i;
    }
    if (remove)
        return true;
    if (c->binding_count >= binding_limit)
        return false;
    c->bindings[c->binding_count++] = b;
    return true;
}

////////////////
//~ Default options

void w_config_defaults(w_desktop_config *c) {
    memset(c, 0, sizeof(*c));
    c->gap = 2;
    c->outer_gap = 2;
    c->border_size = 1;
    c->resize_on_border = true;
    c->rounding = 2;
    c->panel_material = (w_panel_material){.opacity = 240, .blur = true, .shadow = true};
    c->animations = motion_system;
    const int ms[motion_count] = {160, 190, 180, 140, 140, 160, 120, 160, 140};
    for (int i = 0; i < motion_count; i++)
        c->motion[i] = (w_transition){ms[i], {.22, 1, .36, 1}, style_default, -1};
    c->inactive_alpha = 242;
    c->inactive_border = 0x292b33;
    c->follow_mouse = true;
    c->bar_height = 32;
    c->notifications = (w_notification_config){
        .enabled = true, .capture = notice_capture_windows, .corner = notice_top_right, .duration_ms = 5000,
        .fade_ms = 160, .history_limit = 128, .visible_limit = 3, .margin = 12, .width = 360
    };
    c->bar_tooltip_delay = 400;
    c->bar_tooltip_duration = 4500;
    c->bar_workspace_count = 5;
    c->background = 0x101116;
    c->foreground = 0xe4e5ea;
    c->accent = 0x91aaf7;
    strcpy(c->edge, "top");
    strcpy(c->bar_left, "menu workspaces mode");
    strcpy(c->bar_center, "clock");
    strcpy(c->bar_right, "tray cpu bluetooth network audio keyboard battery notifications");
    strcpy(c->file_browser, "FilePilot.exe");
    strcpy(c->task_manager, "TaskSlinger.exe");
    strcpy(c->terminal, "auto");
    strcpy(c->editor, "%SystemRoot%\\System32\\notepad.exe");
}

bool w_config_notifications_valid(const w_notification_config *c) {
    return (unsigned)c->capture <= notice_capture_visible && (unsigned)c->corner <= notice_bottom_left &&
           (unsigned)c->duration_ms <= 3600000 &&
           (unsigned)c->fade_ms <= 5000 && c->history_limit >= 1 && c->history_limit <= notice_history_max &&
           c->visible_limit >= 1 && c->visible_limit <= notice_visible_max &&
           (unsigned)c->margin <= 256 && c->width >= 160 && c->width <= 1200;
}

bool w_config_has_module(const w_desktop_config *c, const char *module) {
    const char *groups[] = {c->bar_left, c->bar_center, c->bar_right};
    size_t length = strlen(module);
    for (size_t i = 0; i < count_of(groups); ++i) {
        const char *p = groups[i];
        while (*p) {
            p += strspn(p, " ");
            if (!*p)
                break;
            size_t token = strcspn(p, " ");
            if (token >= length && (token == length || p[length] == '#') &&
                !memcmp(p, module, length))
                return true;
            p += token;
        }
    }
    return false;
}

static const char *const builtins[] = {"menu", "workspaces", "mode", "title", "clock", "bluetooth",
    "network", "throughput", "audio", "keyboard", "battery", "cpu", "memory", "media",
    "display", "clipboard", "notifications", "project", "tray"};

const char *w_config_module_name(int index) {
    return (unsigned)index < count_of(builtins) ? builtins[index] : NULL;
}

static bool w_module_valid(const char *p, size_t length) {
    if (!length || length >= sizeof(((w_module_style *)0)->name)) return false;
    for (size_t i=0; i<length; ++i)
        if ((unsigned char)p[i] <= ' ' || p[i] == 0x7f) return false;
    const char *tag = memchr(p,'#',length);
    size_t base = tag ? (size_t)(tag-p) : length;
    if (tag && (base+1 == length || memchr(tag+1,'#',length-base-1))) return false;
    for (size_t i=0; i<count_of(builtins); ++i)
        if (strlen(builtins[i]) == base && !strncmp(p,builtins[i],base)) return true;
    return base > 7 && base-7 < sizeof(((w_provider_config *)0)->name) && !strncmp(p,"custom/",7);
}

bool w_config_set_modules(char *out, size_t cap, const char *spec) {
    char normalized[sizeof(((w_desktop_config *)0)->bar_left)];
    size_t used = 0;
    while (*(spec += strspn(spec," \t"))) {
        size_t n = strcspn(spec," \t");
        if (!w_module_valid(spec,n) || used + (used != 0) + n >= sizeof(normalized)) return false;
        if (used) normalized[used++] = ' ';
        memcpy(normalized+used,spec,n);
        used += n;
        spec += n;
    }
    normalized[used] = 0;
    return !out || w_str_copy(out,cap,normalized);
}

bool w_config_modules_valid(const char *spec) {
    return w_config_set_modules(NULL,0,spec);
}

w_module_style *w_config_module_style(w_desktop_config *c, const char *name) {
    if (!w_module_valid(name,strlen(name))) return NULL;
    for (int i=0; i<c->style_count; ++i)
        if (!strcmp(c->styles[i].name,name)) return &c->styles[i];
    if (c->style_count >= (int)count_of(c->styles)) return NULL;
    w_module_style *style = &c->styles[c->style_count++];
    strcpy(style->name,name);
    return style;
}

bool w_config_style_option(w_module_style *style, const char *key, const char *value) {
    bool tooltip = !strcmp(key,"tooltip");
    for (const unsigned char *p=(const unsigned char *)value; *p; ++p)
        if ((*p < ' ' || *p == 0x7f) && !(tooltip && (*p == '\n' || *p == '\r' || *p == '\t'))) return false;
    if (!strcmp(key,"format")) return w_str_copy(style->format,sizeof(style->format),value);
    if (tooltip) return w_str_copy(style->tooltip,sizeof(style->tooltip),value);
    if (!strcmp(key,"tooltip_enabled")) {
        if (!strcmp(value,"true") || !strcmp(value,"1")) style->hide_tooltip = false;
        else if (!strcmp(value,"false") || !strcmp(value,"0")) style->hide_tooltip = true;
        else return false;
        return true;
    }
    if (!strcmp(key,"display")) {
        const char *const modes[] = {"auto", "icon", "text", "icon_text"};
        for (size_t i=0; i<count_of(modes); ++i)
            if (!strcmp(modes[i],value)) { style->display = (enum w_module_display)i; return true; }
        return false;
    }
    char *action = !strcmp(key,"on_click") ? style->action :
                   !strcmp(key,"on_click_right") ? style->right_click :
                   !strcmp(key,"on_click_middle") ? style->middle_click :
                   !strcmp(key,"scroll_up") ? style->scroll_up :
                   !strcmp(key,"scroll_down") ? style->scroll_down : NULL;
    w_command command;
    return action && (!*value || w_command_parse(value,&command)) && w_str_copy(action,sizeof(style->action),value);
}

////////////////
//~ Provider references

bool w_config_validate_providers(const w_desktop_config *c, char *err, size_t cap) {
    const char *groups[] = {c->bar_left, c->bar_center, c->bar_right};
    for (size_t g = 0; g < count_of(groups); g++) {
        const char *p = groups[g];
        while (*p) {
            while (*p == ' ')
                p++;
            if (!*p)
                break;
            size_t n = strcspn(p, " ");
            if (n > 7 && !strncmp(p, "custom/", 7)) {
                bool found = false;
                for (int i = 0; i < c->provider_count; i++)
                    if (strlen(c->providers[i].name) == strcspn(p + 7, "# ") &&
                        !strncmp(c->providers[i].name, p + 7, strlen(c->providers[i].name)))
                        found = true;
                if (!found) {
                    snprintf(err, cap, "Unknown bar provider '%.*s'", (int)n, p);
                    return false;
                }
            }
            p += n;
        }
    }
    return true;
}
