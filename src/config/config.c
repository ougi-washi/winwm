#include "config/config.h"
#include "base/json.h"
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
    for (int i = 0; i < c->binding_count; i++)
        if (c->bindings[i].mods == b.mods && c->bindings[i].key == b.key && c->bindings[i].modal == b.modal &&
            c->bindings[i].next_key == b.next_key && c->bindings[i].next_mods == b.next_mods &&
            c->bindings[i].release == b.release) {
            if (remove)
                c->bindings[i] = c->bindings[--c->binding_count];
            else
                c->bindings[i] = b;
            return true;
        }
    if (remove)
        return true;
    if (c->binding_count >= binding_limit)
        return false;
    c->bindings[c->binding_count++] = b;
    return true;
}

////////////////
//~ Default bindings

void w_config_defaults(w_desktop_config *c) {
    memset(c, 0, sizeof(*c));
    c->gap = 6;
    c->outer_gap = 6;
    c->border_size = 2;
    c->resize_on_border = true;
    c->drag_mods = 8; // Super
    c->rounding = 8;
    c->animations = motion_system;
    const int ms[motion_count] = {160, 190, 180, 140, 140, 160, 120, 160, 140};
    for (int i = 0; i < motion_count; i++)
        c->motion[i] = (w_transition){ms[i], {.22, 1, .36, 1}, style_default, -1};
    c->inactive_alpha = 250;
    c->inactive_border = 0x292b33;
    c->follow_mouse = true;
    c->bar_height = 32;
    c->bar_tooltip_delay = 400;
    c->bar_tooltip_duration = 4500;
    c->bar_workspace_count = 5;
    c->background = 0x101116;
    c->foreground = 0xe4e5ea;
    c->accent = 0x91aaf7;
    strcpy(c->edge, "top");
    strcpy(c->bar_left, "menu workspaces mode");
    strcpy(c->bar_center, "clock");
    strcpy(c->bar_right, "cpu bluetooth network audio keyboard battery");
    strcpy(c->file_browser, "FilePilot.exe");
    strcpy(c->task_manager, "TaskSlinger.exe");
    strcpy(c->terminal, "auto");
    strcpy(c->editor, "%SystemRoot%\\System32\\notepad.exe");
    static const char *const pairs[][2] = {{"super+enter", "terminal"},
                                           {"super+d", "menu"},
                                           {"ctrl+alt+space", "menu"},
                                           {"super+p", "open_folder"},
                                           {"super+shift+escape", "open_tasks"},
                                           {"super+tab", "switcher"},
                                           {"super+b", "launch brave"},
                                           {"super+shift+v", "split horizontal"},
                                           {"alt+tab", "switcher"},
                                           {"alt+shift+tab", "switcher"},
                                           {"alt+escape", "switcher"},
                                           {"alt+f4", "close"},
                                           {"ctrl+shift+escape", "open_tasks"},
                                           {"ctrl+escape", "menu"},
                                           {"resize:left", "resize shrink"},
                                           {"resize:right", "resize grow"},
                                           {"super+v", "split vertical"},
                                           {"super+e", "layout tree"},
                                           {"super+w", "layout tabs"},
                                           {"super+s", "layout stack"},
                                           {"super+f", "fullscreen"},
                                           {"super+shift+space", "floating"},
                                           {"resize:escape", "resize_mode"},
                                           {"resize:h", "resize shrink"},
                                           {"resize:l", "resize grow"},
                                           {"super+r", "resize_mode"},
                                           {"super+minus", "scratch_show"},
                                           {"super+shift+minus", "scratch_send"},
                                           {"super+shift+q", "close"},
                                           {"super+shift+c", "reload"},
                                           {"super+shift+p", "settings"},
                                           {"super+shift+i", "clipboard"},
                                           {"super+shift+x", "capture"},
                                           {"super+shift+b", "bar_toggle"},
                                           {"super+shift+d", "output_send"},
                                           {"super+o", "output_next"},
                                           {"super+shift+r", "reload"},
                                           {"super+shift+e", "confirm_sign_out"},
                                           {"super+space", "focus_mode_toggle"},
                                           {"super+e", "layout toggle_split"},
                                           {"resize:enter", "resize_mode"},
                                           {"print", "capture_region"},
                                           {"super+f6", "capture_region"},
                                           {"volume_mute", "mute"},
                                           {"repeat:volume_up", "volume_up"},
                                           {"repeat:volume_down", "volume_down"},
                                           {"resize:repeat:left", "resize left"},
                                           {"resize:repeat:down", "resize down"},
                                           {"resize:repeat:up", "resize up"},
                                           {"resize:repeat:right", "resize right"},
                                           {"resize:repeat:h", "resize left"},
                                           {"resize:repeat:j", "resize down"},
                                           {"resize:repeat:k", "resize up"},
                                           {"resize:repeat:l", "resize right"}};
    for (size_t i = 0; i < count_of(pairs); i++)
        w_config_bind(c, pairs[i][0], pairs[i][1]);
    const char *dirs[] = {"left", "down", "up", "right"};
    for (int i = 0; i < 4; i++) {
        char chord[64], action[64];
        snprintf(action, sizeof(action), "focus %s", dirs[i]);
        snprintf(chord, sizeof(chord), "super+%s", dirs[i]);
        w_config_bind(c, chord, action);
        snprintf(chord, sizeof(chord), "ctrl+alt+%s", dirs[i]);
        w_config_bind(c, chord, action);
        snprintf(chord, sizeof(chord), "super+%c", "hjkl"[i]);
        w_config_bind(c, chord, action);
        snprintf(action, sizeof(action), "move %s", dirs[i]);
        snprintf(chord, sizeof(chord), "repeat:super+shift+%s", dirs[i]);
        w_config_bind(c, chord, action);
        snprintf(chord, sizeof(chord), "repeat:super+shift+%c", "hjkl"[i]);
        w_config_bind(c, chord, action);
    }
    w_config_bind(c, "super+h", "split horizontal");
    for (int n = 1; n <= 10; n++) {
        char chord[64], action[64];
        snprintf(chord, sizeof(chord), "super+%d", n % 10);
        snprintf(action, sizeof(action), "workspace_select %d", n);
        w_config_bind(c, chord, action);
        snprintf(chord, sizeof(chord), "super+shift+%d", n % 10);
        snprintf(action, sizeof(action), "workspace_send %d", n);
        w_config_bind(c, chord, action);
    }
}

static bool w_token_int(w_json_token *t, int lo, int hi, int *out) {
    if (t->type != json_scalar || !t->length || t->length > 6)
        return false;
    int n = 0;
    for (size_t i = 0; i < t->length; i++) {
        if (!isdigit((unsigned char)t->text[i]))
            return false;
        n = n * 10 + t->text[i] - '0';
    }
    if (n < lo || n > hi)
        return false;
    *out = n;
    return true;
}

static bool w_token_bool(w_json_token *t, bool *v) {
    if (t->type != json_scalar)
        return false;
    if (t->length == 4 && !strncmp(t->text, "true", 4)) {
        *v = true;
        return true;
    }
    if (t->length == 5 && !strncmp(t->text, "false", 5)) {
        *v = false;
        return true;
    }
    return false;
}

static bool w_color(w_json_token *t, uint32_t *out) {
    char s[10];
    if (!w_json_read(t, s, sizeof(s)) || strlen(s) != 7 || *s != '#')
        return false;
    for (int i = 1; i <= 6; i++)
        if (!isxdigit((unsigned char)s[i]))
            return false;
    *out = (uint32_t)strtoul(s + 1, NULL, 16);
    return true;
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
    "display", "clipboard", "notifications", "project"};

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
//~ Configuration parsing

static bool w_json_modules(const w_json_token *t, int v, char *out, size_t cap) {
    char spec[sizeof(((w_desktop_config *)0)->bar_left)];
    if (t[v].type == json_string)
        return w_json_read(&t[v],spec,sizeof(spec)) && w_config_set_modules(out,cap,spec);
    if (t[v].type != json_array) return false;
    size_t used = 0;
    for (int i=v+1; i<t[v].after; i=t[i].after) {
        char name[sizeof(((w_module_style *)0)->name)];
        if (!w_json_read(&t[i],name,sizeof(name)) || !w_module_valid(name,strlen(name))) return false;
        size_t n = strlen(name);
        if (used + (used != 0) + n >= sizeof(spec)) return false;
        if (used) spec[used++] = ' ';
        memcpy(spec+used,name,n);
        used += n;
    }
    spec[used] = 0;
    return w_str_copy(out,cap,spec);
}

static bool w_parse_object(w_desktop_config *c, w_json_token *t, int root, char *err, size_t cap) {
    bool version = false;
    if (t[root].type != json_object)
        return false;
    for (int i = root + 1; i < t[root].after;) {
        char k[128];
        int v = i + 1;
        bool ok = w_json_read(&t[i], k, sizeof(k));
        if (!ok)
            return false;
        w_json_token *a = t + v;
        if (!strcmp(k, "version")) {
            int n;
            ok = w_token_int(a, 1, 1, &n);
            version = ok;
        } else if (!strcmp(k, "gap")) {
            ok = w_token_int(a, 0, 128, &c->gap);
            c->outer_gap = c->gap;
        } else if (!strcmp(k, "workspace_names")) {
            ok = a->type == json_array;
            memset(c->workspace_names, 0, sizeof(c->workspace_names));
            int which = 0;
            for (int j = v + 1; ok && j < a->after; j = t[j].after) {
                ok = which < workspace_limit &&
                     w_json_read(t + j, c->workspace_names[which], sizeof(c->workspace_names[0]));
                if (ok && *c->workspace_names[which]) {
                    if (isdigit((unsigned char)c->workspace_names[which][0]))
                        ok = false;
                    for (int prev = 0; prev < which; prev++)
                        if (!strcmp(c->workspace_names[which], c->workspace_names[prev]))
                            ok = false;
                }
                which++;
            }
        } else if (!strcmp(k, "bar_height"))
            ok = w_token_int(a, 16, 128, &c->bar_height);
        else if (!strcmp(k, "bar_tooltip_delay"))
            ok = w_token_int(a, 0, 10000, &c->bar_tooltip_delay);
        else if (!strcmp(k, "bar_tooltip_duration"))
            ok = w_token_int(a, 0, 60000, &c->bar_tooltip_duration);
        else if (!strcmp(k, "bar_workspace_count"))
            ok = w_token_int(a, 0, workspace_limit, &c->bar_workspace_count);
        else if (!strcmp(k, "background"))
            ok = w_color(a, &c->background);
        else if (!strcmp(k, "foreground"))
            ok = w_color(a, &c->foreground);
        else if (!strcmp(k, "accent"))
            ok = w_color(a, &c->accent);
        else if (!strcmp(k, "inactive_border"))
            ok = w_color(a, &c->inactive_border);
        else if (!strcmp(k, "border_size"))
            ok = w_token_int(a, 0, 16, &c->border_size);
        else if (!strcmp(k, "rounding"))
            ok = w_token_int(a, 0, 32, &c->rounding);
        else if (!strcmp(k, "resize_on_border"))
            ok = w_token_bool(a, &c->resize_on_border);
        else if (!strcmp(k, "animations")) {
            bool on;
            ok = w_token_bool(a, &on);
            if (ok)
                c->animations = on ? motion_on : motion_off;
        }
        else if (!strcmp(k, "edge"))
            ok = w_json_read(a, c->edge, sizeof(c->edge)) &&
                 (!strcmp(c->edge, "top") || !strcmp(c->edge, "bottom") || !strcmp(c->edge, "left") ||
                  !strcmp(c->edge, "right"));
        else if (!strcmp(k, "bar_left"))
            ok = w_json_modules(t,v,c->bar_left,sizeof(c->bar_left));
        else if (!strcmp(k, "bar_center"))
            ok = w_json_modules(t,v,c->bar_center,sizeof(c->bar_center));
        else if (!strcmp(k, "bar_right"))
            ok = w_json_modules(t,v,c->bar_right,sizeof(c->bar_right));
        else if (!strcmp(k, "file_browser"))
            ok = w_json_read(a, c->file_browser, sizeof(c->file_browser)) && *c->file_browser;
        else if (!strcmp(k, "task_manager"))
            ok = w_json_read(a, c->task_manager, sizeof(c->task_manager)) && *c->task_manager;
        else if (!strcmp(k, "terminal"))
            ok = w_json_read(a, c->terminal, sizeof(c->terminal)) && *c->terminal;
        else if (!strcmp(k, "editor"))
            ok = w_json_read(a, c->editor, sizeof(c->editor)) && *c->editor;
        else if (!strcmp(k, "bindings") || !strcmp(k, "apps")) {
            ok = a->type == json_object;
            for (int j = v + 1; ok && j < a->after; j = t[j + 1].after) {
                char name[80], value[1024];
                ok = w_json_read(&t[j], name, sizeof(name)) && w_json_read(&t[j + 1], value, sizeof(value)) &&
                     *name && *value;
                if (!ok)
                    break;
                if (!strcmp(k, "bindings"))
                    ok = w_config_bind(c, name, value);
                else {
                    int n = 0;
                    while (n < c->app_count && strcmp(c->apps[n].name, name))
                        n++;
                    ok = n < app_limit;
                    if (ok) {
                        if (n == c->app_count)
                            c->app_count++;
                        strcpy(c->apps[n].name, name);
                        strcpy(c->apps[n].path, value);
                    }
                }
                if (!ok) {
                    snprintf(err, cap, "line %d: invalid %s entry '%s'", t[j].line, k, name);
                    return false;
                }
            }
        } else if (!strcmp(k, "modules")) {
            ok = a->type == json_object;
            for (int j = v + 1; ok && j < a->after; j = t[j + 1].after) {
                char name[sizeof(c->styles[0].name)];
                if (t[j + 1].type != json_object || !w_json_read(&t[j], name, sizeof(name))) {
                    ok = false;
                    break;
                }
                w_module_style *style = w_config_module_style(c,name);
                if (!style) {
                    ok = false;
                    break;
                }
                for (int z = j + 2; ok && z < t[j + 1].after; z = t[z + 1].after) {
                    char field[32];
                    w_json_token *val = &t[z + 1];
                    if (!w_json_read(t + z, field, sizeof(field))) {
                        ok = false;
                        break;
                    }
                    if (!strcmp(field, "color")) {
                        ok = w_color(val, &style->color);
                        style->colored = ok;
                    } else if (!strcmp(field,"tooltip_enabled") || (!strcmp(field,"tooltip") && val->type == json_scalar)) {
                        bool enabled;
                        ok = w_token_bool(val,&enabled);
                        if (ok) style->hide_tooltip = !enabled;
                    } else {
                        char value[sizeof(style->tooltip)];
                        ok = w_json_read(val,value,sizeof(value)) && w_config_style_option(style,field,value);
                    }
                    if (!ok) {
                        snprintf(err,cap,"line %d: invalid module option '%s.%s'",val->line,name,field);
                        return false;
                    }
                }
            }
        } else if (!strcmp(k, "providers")) {
            ok = a->type == json_object;
            for (int j = v + 1; ok && j < a->after; j = t[j + 1].after) {
                char provider[sizeof(c->providers[0].name)];
                if (t[j + 1].type != json_object || !w_json_read(&t[j], provider, sizeof(provider)) || !*provider) {
                    ok = false;
                    break;
                }
                int n = 0;
                while (n < c->provider_count && strcmp(c->providers[n].name, provider))
                    n++;
                if (n >= provider_limit) {
                    ok = false;
                    break;
                }
                w_provider_config *p = &c->providers[n];
                if (n == c->provider_count) {
                    c->provider_count++;
                    p->timeout = 10;
                    strcpy(p->name, provider);
                }
                for (const char *q = p->name; ok && *q; q++)
                    ok = isalnum((unsigned char)*q) || *q == '_' || *q == '-';
                for (int z = j + 2; ok && z < t[j + 1].after; z = t[z + 1].after) {
                    char name[32];
                    w_json_token *val = &t[z + 1];
                    if (!w_json_read(&t[z], name, sizeof(name))) {
                        ok = false;
                        break;
                    }
                    if (!strcmp(name, "executable"))
                        ok = w_json_read(val, p->executable, sizeof(p->executable));
                    else if (!strcmp(name, "interval"))
                        ok = w_token_int(val, 0, 86400, &p->interval);
                    else if (!strcmp(name, "timeout"))
                        ok = w_token_int(val, 1, 86400, &p->timeout);
                    else if (!strcmp(name, "stream"))
                        ok = w_token_bool(val, &p->stream);
                    else if (!strcmp(name, "action")) {
                        w_command action;
                        ok =
                            w_json_read(val, p->action, sizeof(p->action)) && w_command_parse(p->action, &action);
                    } else
                        ok = false;
                }
                ok = ok && *p->executable && !(p->stream && p->interval);
            }
        } else if (!strcmp(k, "rules")) {
            ok = a->type == json_array;
            for (int j = v + 1; ok && j < a->after; j = t[j].after) {
                if (t[j].type != json_object || c->rule_count >= rule_limit) {
                    ok = false;
                    break;
                }
                w_window_rule *r = &c->rules[c->rule_count++];
                r->workspace = r->output = r->layout = -1;
                r->sets_floating = true;
                for (int z = j + 1; ok && z < t[j].after; z = t[z + 1].after) {
                    char name[32];
                    w_json_token *val = &t[z + 1];
                    if (!w_json_read(&t[z], name, sizeof(name))) {
                        ok = false;
                        break;
                    }
                    if (!strcmp(name, "class"))
                        ok = w_json_read(val, r->class_name, sizeof(r->class_name));
                    else if (!strcmp(name, "title"))
                        ok = w_json_read(val, r->title, sizeof(r->title));
                    else if (!strcmp(name, "executable"))
                        ok = w_json_read(val, r->executable, sizeof(r->executable));
                    else if (!strcmp(name, "output")) {
                        int index;
                        ok = w_token_int(val, 1, output_limit, &index);
                        if (ok)
                            r->output = index - 1;
                    } else if (!strcmp(name, "border")) {
                        ok = w_color(val, &r->border);
                        r->colored = ok;
                    } else if (!strcmp(name, "layout")) {
                        char layout[20];
                        const char *names[] = {"tree", "alternate", "master", "tabs", "stack"};
                        ok = w_json_read(val, layout, sizeof(layout));
                        if (ok) {
                            for (int k = 0; k < 5; k++)
                                if (!strcmp(layout, names[k]))
                                    r->layout = k;
                            ok = r->layout >= 0;
                        }
                    } else if (!strcmp(name, "floating"))
                        ok = w_token_bool(val, &r->floating);
                    else if (!strcmp(name, "exclude"))
                        ok = w_token_bool(val, &r->exclude);
                    else if (!strcmp(name, "workspace")) {
                        int ws;
                        ok = w_token_int(val, 1, 10, &ws);
                        if (ok)
                            r->workspace = ws - 1;
                    } else
                        ok = false;
                }
                ok = ok && (*r->class_name || *r->title || *r->executable);
            }
        } else
            ok = false;
        if (!ok) {
            snprintf(err, cap, "line %d: unknown key or invalid value for '%s'", t[i].line, k);
            return false;
        }
        i = a->after;
    }
    if (!version)
        snprintf(err, cap, "configuration requires version 1");
    return version;
}

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

static bool w_parse_json(w_desktop_config *out, const char *s, char *err, size_t cap, bool merge) {
    w_json_token *tokens = calloc(4096, sizeof(*tokens));
    w_desktop_config *next = malloc(sizeof(*next));
    w_json_doc d;
    snprintf(err, cap, "invalid configuration");
    bool ok = false;
    if (tokens && next) {
        if (merge)
            *next = *out;
        else
            w_config_defaults(next);
        if (w_json_parse(&d, s, tokens, 4096))
            ok = w_parse_object(next, tokens, 0, err, cap) && (merge || w_config_validate_providers(next, err, cap));
        else
            snprintf(err, cap, "%s", d.error);
        if (ok) {
            *out = *next;
            if (cap)
                *err = 0;
        }
    }
    free(next);
    free(tokens);
    return ok;
}

bool w_config_parse(w_desktop_config *out, const char *s, char *err, size_t cap) {
    return w_parse_json(out, s, err, cap, false);
}

bool w_config_merge(w_desktop_config *out, const char *s, char *err, size_t cap) {
    return w_parse_json(out, s, err, cap, true);
}
