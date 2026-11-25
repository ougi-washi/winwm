#include "config/language.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

////////////////
//~ Language

static char *w_trim(char *s) {
    while (isspace((unsigned char)*s))
        s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        --end;
    *end = 0;
    return s;
}

static bool w_identifier(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

static char *w_comma_field(char **rest) {
    char *field = *rest, *comma = strchr(field, ',');
    if (comma) {
        *comma = 0;
        *rest = comma + 1;
    } else
        *rest += strlen(*rest);
    return w_trim(field);
}

static int w_variable(w_config_language *lang, const char *name, size_t length) {
    for (int i = 0; i < lang->variable_count; ++i)
        if (strlen(lang->variables[i].name) == length && !strncmp(lang->variables[i].name, name, length))
            return i;
    return -1;
}

static bool w_expand(w_config_language *lang, const char *in, char *out, size_t cap) {
    size_t n = 0;
    while (*in) {
        if (*in != '$' || !w_identifier(in[1])) {
            if (n + 1 >= cap)
                return false;
            out[n++] = *in++;
            continue;
        }
        const char *start = ++in;
        while (w_identifier(*in))
            in++;
        int i = w_variable(lang, start, (size_t)(in - start));
        if (i < 0)
            return false;
        const char *v = lang->variables[i].value;
        size_t len = strlen(v);
        if (len >= cap - n)
            return false;
        memcpy(out + n, v, len);
        n += len;
    }
    out[n] = 0;
    return true;
}

static bool w_set_variable(w_config_language *lang, const char *name, const char *value) {
    if (!*name || strlen(name) >= sizeof(lang->variables[0].name))
        return false;
    for (const char *p = name; *p; p++)
        if (!w_identifier(*p))
            return false;
    int i = w_variable(lang, name, strlen(name));
    if (i < 0) {
        if ((size_t)lang->variable_count == count_of(lang->variables))
            return false;
        i = lang->variable_count++;
        strcpy(lang->variables[i].name, name);
    }
    return w_str_copy(lang->variables[i].value, sizeof(lang->variables[i].value), value);
}

static bool w_number(const char *s, int minimum, int maximum, int *out) {
    char *end;
    long value = strtol(s, &end, 10);
    if (s == end || *end || value < minimum || value > maximum)
        return false;
    *out = (int)value;
    return true;
}

static bool w_color(const char *s, uint32_t *out) {
    bool alpha = !strncmp(s, "rgba(", 5);
    if (!alpha && strncmp(s, "rgb(", 4))
        return false;
    s += alpha ? 5 : 4;
    size_t n = alpha ? 8 : 6;
    if (strlen(s) != n + 1 || s[n] != ')')
        return false;
    for (size_t i = 0; i < n; i++)
        if (!isxdigit((unsigned char)s[i]))
            return false;
    *out = (uint32_t)strtoul(s, NULL, 16) >> (alpha ? 8 : 0);
    return true;
}

static bool w_decimal(const char *s, double lo, double hi, double *out) {
    char *end;
    double v = strtod(s, &end);
    if (end == s || *end || !isfinite(v) || v < lo || v > hi)
        return false;
    *out = v;
    return true;
}

static bool w_gradient(char *value, uint32_t *start, w_border_gradient *gradient) {
    char *next = value + strcspn(value, " \t");
    if (!*next) {
        *gradient = (w_border_gradient){0};
        return w_color(value, start);
    }
    *next++ = 0;
    next = w_trim(next);
    char *direction = next + strcspn(next, " \t");
    int angle = 0;
    if (*direction) {
        *direction++ = 0;
        direction = w_trim(direction);
        size_t n = strlen(direction);
        if (n < 4 || strcmp(direction+n-3, "deg"))
            return false;
        direction[n-3] = 0;
        if (!w_number(direction, 0, 360, &angle))
            return false;
    }
    gradient->enabled = true;
    gradient->angle = angle;
    return w_color(value, start) && w_color(next, &gradient->end);
}

static bool w_bezier(w_config_language *l, char *value) {
    char *name = w_comma_field(&value);
    double points[4];
    if (!*name || strlen(name) >= sizeof(l->curves[0].name) ||
        !strcmp(name, "linear") || !strcmp(name, "default"))
        return false;
    for (int i = 0; i < 4; i++) {
        if ((i == 3 && strchr(value, ',')) || !w_decimal(w_comma_field(&value), i%2 ? -4 : 0, i%2 ? 4 : 1, &points[i]))
            return false;
    }
    int i = 0;
    while (i < l->curve_count && strcmp(l->curves[i].name, name))
        i++;
    if (i == (int)count_of(l->curves))
        return false;
    if (i == l->curve_count)
        l->curve_count++;
    strcpy(l->curves[i].name, name);
    l->curves[i].curve = (w_curve){points[0], points[1], points[2], points[3]};
    return true;
}

static bool w_animation_style(int kind, char *value, w_transition *t) {
    if (!*value)
        return true;
    char *arg = value + strcspn(value, " \t");
    if (*arg) {
        *arg++ = 0;
        arg = w_trim(arg);
    }
    if (kind == motion_open && !strcmp(value,"popin"))
        t->style = style_popin;
    else if ((kind == motion_open || kind == motion_workspace) && !strcmp(value,"slide"))
        t->style = style_slide;
    else if (kind == motion_workspace && !strcmp(value,"slidevert"))
        t->style = style_slidevert;
    else
        return (kind == motion_layers_in || kind == motion_layers_out) && !strcmp(value,"fade") && !*arg;
    if (!*arg)
        return true;
    if (kind == motion_open && t->style == style_slide) {
        const char *edges[] = {"top","bottom","left","right"};
        for (int i = 0; i < 4; i++)
            if (!strcmp(arg,edges[i])) {
                t->style = style_top + i;
                return true;
            }
        return false;
    }
    size_t len = strlen(arg);
    if (arg[len-1] != '%')
        return false;
    arg[len-1] = 0;
    return w_number(arg, 0, 100, &t->percent);
}

static bool w_animation(w_config_language *l, char *value) {
    const char *const leaves[motion_count] = {"windowsIn", "windowsMove", "workspaces", "border", "barModules",
                                             "layersIn", "layersOut", "fadeIn", "fadeSwitch"};
    char *name = w_comma_field(&value);
    int kind = 0, on;
    while (kind < motion_count && strcmp(name, leaves[kind]))
        kind++;
    if (kind == motion_count || !w_number(w_comma_field(&value), 0, 1, &on))
        return false;
    w_transition *transition = &l->config->motion[kind];
    if (!on && !*value) {
        transition->ms = 0;
        return true;
    }
    double speed;
    if (!w_decimal(w_comma_field(&value), .01, 30, &speed))
        return false;
    char *curve = w_comma_field(&value);
    w_curve easing = {.22, 1, .36, 1};
    if (!strcmp(curve, "linear"))
        easing = (w_curve){0, 0, 1, 1};
    else if (strcmp(curve, "default")) {
        int i = 0;
        while (i < l->curve_count && strcmp(curve, l->curves[i].name))
            i++;
        if (i == l->curve_count)
            return false;
        easing = l->curves[i].curve;
    }
    value = w_trim(value);
    w_transition t = {on ? (int)(speed*100 + .5) : 0, easing, style_default, -1};
    if (!w_animation_style(kind, value, &t))
        return false;
    *transition = t;
    return true;
}

static bool w_make_chord(char *mods, char *key, char *out, size_t cap) {
    for (char *p = mods; *p; p++)
        *p = (char)tolower((unsigned char)*p);
    for (char *p = key; *p; p++)
        *p = (char)tolower((unsigned char)*p);
    const struct w_config_alias {
        const char *from, *to;
    } aliases[] = {{"return", "enter"},
                   {"xf86audioraisevolume", "volume_up"},
                   {"xf86audiolowervolume", "volume_down"},
                   {"xf86audiomute", "volume_mute"},
                   {"escape", "escape"}};
    for (size_t i = 0; i < count_of(aliases); i++)
        if (!strcmp(key, aliases[i].from)) {
            key = (char *)aliases[i].to;
            break;
        }
    char result[80] = {0};
    while (*mods) {
        size_t n = strcspn(mods, " +\t"), skip = strspn(mods + n, " +\t");
        char *next = mods + n + skip;
        mods[n] = 0;
        const char *m = !strcmp(mods, "mod4") ? "super" : !strcmp(mods, "control") ? "ctrl" : mods;
        if (strcmp(m, "super") && strcmp(m, "alt") && strcmp(m, "shift") && strcmp(m, "ctrl"))
            return false;
        if (strlen(result) + strlen(m) + 2 >= sizeof(result))
            return false;
        strcat(result, m);
        strcat(result, "+");
        mods = next;
    }
    int n = snprintf(out, cap, "%s%s", result, key);
    return n >= 0 && (size_t)n < cap;
}

////////////////
//~ Command bindings

static bool w_dispatcher(w_desktop_config *c, const char *name, const char *arg, char *out, size_t cap) {
    const char *action = NULL, *param = arg;
    if (!strcmp(name, "exec")) {
        if (!strcmp(arg, c->terminal)) {
            action = "terminal";
            param = "";
        } else if (!strcmp(arg, "Menu") || !strcmp(arg, "Run")) {
            action = "menu";
            param = "";
        } else
            action = "program";
    } else if (!strcmp(name, "winwm"))
        return w_str_copy(out, cap, arg);
    else if (!strcmp(name, "submap"))
        action = "set_mode";
    else if (!strcmp(name, "resizeactive"))
        action = "resize_by";
    else if (!strcmp(name, "workspace"))
        action = "workspace_select";
    else if (!strcmp(name, "movetoworkspacesilent"))
        action = "workspace_send";
    else if (!strcmp(name, "movetoworkspace"))
        action = "workspace_follow";
    else if (!strcmp(name, "movefocus") || !strcmp(name, "movewindow")) {
        action = !strcmp(name, "movefocus") ? "focus" : "move";
        const char *dirs = "ldur", *names[] = {"left", "down", "up", "right"};
        const char *d = *arg ? strchr(dirs, *arg) : NULL;
        if (!d || arg[1])
            return false;
        param = names[d - dirs];
    } else {
        const struct w_config_alias {
            const char *source, *action;
        } aliases[] = {{"killactive", "close"},
                       {"togglefloating", "floating"},
                       {"fullscreen", "fullscreen"},
                       {"togglesplit", "layout toggle_split"}};
        for (size_t i = 0; i < count_of(aliases); i++)
            if (!strcmp(name, aliases[i].source))
                action = aliases[i].action;
        if (!action || (*arg && (strcmp(name, "fullscreen") || strcmp(arg, "0"))))
            return false;
        param = "";
    }
    int len = snprintf(out, cap, "%s%s%s", action, *param ? " " : "", param);
    return len >= 0 && (size_t)len < cap;
}

static bool w_binding(w_config_language *l, const char *keyword, char *value) {
    bool repeat = false, release = false, remove = !strcmp(keyword, "unbind");
    if (!remove)
        for (const char *p = keyword + 4; *p; p++) {
            if (*p == 'e')
                repeat = true;
            else if (*p == 'r')
                release = true;
            else
                return false;
        }
    char *mods = w_comma_field(&value), *key = w_comma_field(&value);
    char chord[80], scoped[96], action[1100];
    if (!w_make_chord(mods, key, chord, sizeof(chord)))
        return false;
    snprintf(scoped, sizeof(scoped), "%s%s%s", l->resize ? "resize:" : "",
             repeat    ? "repeat:"
             : release ? "release:"
                       : "",
             chord);
    if (remove)
        return !*w_trim(value) && w_config_bind(l->config, scoped, "unbound");
    char *name = w_comma_field(&value);
    return !(repeat && release) && w_dispatcher(l->config, name, w_trim(value), action, sizeof(action)) &&
           w_config_bind(l->config, scoped, action);
}

////////////////
//~ Window rules

static bool w_literal_filter(char *s, char *out, size_t cap) {
    if (*s == '^')
        s++;
    size_t len = strlen(s);
    if (len && s[len - 1] == '$')
        s[--len] = 0;
    if (len >= 2 && *s == '(' && s[len - 1] == ')') {
        s[len - 1] = 0;
        s++;
    }
    size_t n = 0;
    for (; *s; s++) {
        if (*s == '\\' && s[1])
            s++;
        else if (strchr(".^$*+?|[]{}()", *s))
            return false;
        if (n + 1 >= cap)
            return false;
        out[n++] = *s;
    }
    out[n] = 0;
    return n != 0;
}

static bool w_parse_window_rule(w_desktop_config *c, char *value) {
    if (c->rule_count >= rule_limit)
        return false;
    w_window_rule rule = {.workspace = -1, .output = -1, .layout = -1, .exact_match = true};
    char *action = w_comma_field(&value);
    if (!strcmp(action, "float") || !strcmp(action, "tile")) {
        rule.sets_floating = true;
        rule.floating = !strcmp(action, "float");
    } else if (!strncmp(action, "workspace ", 10)) {
        int ws;
        if (!w_number(w_trim(action + 10), 1, workspace_limit, &ws))
            return false;
        rule.workspace = ws - 1;
    } else
        return false;
    while (*value) {
        char *filter = w_comma_field(&value);
        if (!strncmp(filter, "class:", 6) && !*rule.class_name) {
            if (!w_literal_filter(filter + 6, rule.class_name, sizeof(rule.class_name)))
                return false;
        } else if (!strncmp(filter, "title:", 6) && !*rule.title) {
            if (!w_literal_filter(filter + 6, rule.title, sizeof(rule.title)))
                return false;
        } else
            return false;
    }
    if (!*rule.class_name && !*rule.title)
        return false;
    c->rules[c->rule_count++] = rule;
    return true;
}

////////////////
//~ Configuration options

static bool w_option(w_config_language *l, const char *key, char *v) {
    w_desktop_config *c = l->config;
    if (!strcmp(key, "input:floating_modifier") || !strcmp(key, "floating_modifier")) {
        char *mode = v + strcspn(v, " \t");
        if (*mode)
            *mode++ = 0;
        mode = w_trim(mode);
        if (*mode && strcmp(mode,"normal") && strcmp(mode,"inverse"))
            return false;
        char chord[64];
        unsigned keycode, mods;
        int n = snprintf(chord, sizeof(chord), "%s+a", v);
        if (n < 0 || (size_t)n >= sizeof(chord) || !w_config_chord(chord, &mods, &keycode) || !mods)
            return false;
        c->drag_mods = mods;
        c->inverse_drag = !strcmp(mode,"inverse");
        return true;
    }
    if (!strcmp(key, "exec") || !strcmp(key, "exec-once")) {
        if (!*v || c->startup_count >= (int)count_of(c->startup))
            return false;
        w_startup_entry *entry = &c->startup[c->startup_count];
        if (!w_str_copy(entry->command_line, sizeof(entry->command_line), v))
            return false;
        entry->once = !strcmp(key, "exec-once");
        c->startup_count++;
        return true;
    }
    if (!strcmp(key, "windowrulev2"))
        return w_parse_window_rule(c, v);
    if (!strcmp(key, "general:layout")) {
        if (strcmp(v, "dwindle") && strcmp(v, "master"))
            return false;
        c->master_default = !strcmp(v, "master");
        return true;
    }
    if (!strcmp(key, "general:gaps_in"))
        return w_number(v, 0, 128, &c->gap);
    if (!strcmp(key, "general:gaps_out"))
        return w_number(v, 0, 128, &c->outer_gap);
    if (!strcmp(key, "general:col.active_border"))
        return w_gradient(v, &c->accent, &c->active_gradient);
    if (!strcmp(key, "general:col.inactive_border"))
        return w_gradient(v, &c->inactive_border, &c->inactive_gradient);
    if (!strcmp(key, "general:border_size"))
        return w_number(v, 0, 16, &c->border_size);
    if (!strcmp(key, "decoration:rounding"))
        return w_number(v, 0, 32, &c->rounding);
    if (!strcmp(key, "animations:enabled")) {
        if (!strcmp(v, "auto")) {
            c->animations = motion_system;
            return true;
        }
        int on;
        if (!w_number(v, 0, 1, &on))
            return false;
        c->animations = on ? motion_on : motion_off;
        return true;
    }
    if (!strcmp(key, "animations:bezier"))
        return w_bezier(l, v);
    if (!strcmp(key, "animations:animation"))
        return w_animation(l, v);
    if (!strcmp(key, "decoration:inactive_opacity")) {
        char *end;
        double f = strtod(v, &end);
        if (end == v || *end || !isfinite(f) || f < 0 || f > 1)
            return false;
        c->inactive_alpha = (unsigned char)(255 * f + .5);
        return true;
    }
    if (!strcmp(key, "input:follow_mouse") || !strcmp(key, "general:resize_on_border")) {
        int on;
        if (!w_number(v, 0, 1, &on))
            return false;
        if (!strcmp(key, "input:follow_mouse"))
            c->follow_mouse = on != 0;
        else
            c->resize_on_border = on != 0;
        return true;
    }
    if (!strcmp(key, "bar:height"))
        return w_number(v, 16, 128, &c->bar_height);
    if (!strcmp(key, "bar:tooltip_delay"))
        return w_number(v, 0, 10000, &c->bar_tooltip_delay);
    if (!strcmp(key, "bar:tooltip_duration"))
        return w_number(v, 0, 60000, &c->bar_tooltip_duration);
    if (!strcmp(key, "bar:workspace_count"))
        return w_number(v, 0, workspace_limit, &c->bar_workspace_count);
    if (!strcmp(key, "bar:background"))
        return w_color(v, &c->background);
    if (!strcmp(key, "bar:foreground"))
        return w_color(v, &c->foreground);
    if (!strcmp(key, "bar:position"))
        return (!strcmp(v, "left") || !strcmp(v, "right") || !strcmp(v, "top") || !strcmp(v, "bottom")) &&
               w_str_copy(c->edge, sizeof(c->edge), v);
    char *group = !strcmp(key, "bar:modules_left")     ? c->bar_left
                  : !strcmp(key, "bar:modules_center") ? c->bar_center
                  : !strcmp(key, "bar:modules_right")  ? c->bar_right
                                                       : NULL;
    if (group)
        return w_config_set_modules(group, sizeof(c->bar_left), v);
    if (!strncmp(key,"bar:",4)) {
        const char *name = key+4, *field = strchr(name,':');
        char module[sizeof(c->styles[0].name)];
        if (!field || (size_t)(field-name) >= sizeof(module)) return false;
        memcpy(module,name,field-name);
        module[field-name] = 0;
        w_module_style *style = w_config_module_style(c,module);
        if (!style) return false;
        if (!strcmp(field+1,"color")) {
            style->colored = w_color(v,&style->color);
            return style->colored;
        }
        return w_config_style_option(style,field+1,v);
    }
    char *exe = !strcmp(key, "programs:terminal")       ? c->terminal
                : !strcmp(key, "programs:file_browser") ? c->file_browser
                : !strcmp(key, "programs:task_manager") ? c->task_manager
                : !strcmp(key, "programs:editor")       ? c->editor
                                                        : NULL;
    if (exe)
        return *v && w_str_copy(exe, sizeof(c->terminal), v);
    return false;
}

bool w_config_language_parse(w_config_language *l, char *text, char *error, size_t cap) {
    char section[128] = {0}, expanded[4096];
    int line = 0;
    while (*text) {
        line++;
        char *next = strchr(text, '\n');
        if (next)
            *next++ = 0;
        else
            next = text + strlen(text);
        char *s = w_trim(text);
        text = next;
        // A doubled hash is a literal hash in the configuration language.
        char *dst = s;
        for (char *p = s; *p; p++) {
            if (*p == '#') {
                if (p[1] != '#')
                    break;
                p++;
            }
            *dst++ = *p;
        }
        *dst = 0;
        s = w_trim(s);
        size_t n = strlen(s);
        if (!n)
            continue;
        if (!strcmp(s, "}")) {
            if (!*section)
                goto bad;
            char *last = strrchr(section, ':');
            if (last)
                *last = 0;
            else
                *section = 0;
            continue;
        }
        if (s[n - 1] == '{' && !strchr(s, '=')) {
            s[n - 1] = 0;
            s = w_trim(s);
            if (!*s || strlen(section) + strlen(s) + 2 > sizeof(section))
                goto bad;
            if (*section)
                strcat(section, ":");
            strcat(section, s);
            continue;
        }
        char *equals = strchr(s, '=');
        if (!equals)
            goto bad;
        *equals = 0;
        char *key = w_trim(s), *value = w_trim(equals + 1);
        if (!w_expand(l, value, expanded, sizeof(expanded)))
            goto bad;
        bool ok = false;
        if (*key == '$')
            ok = w_set_variable(l, key + 1, expanded);
        else if (!*section && !strcmp(key, "source")) {
            if (!l->source || !*expanded)
                goto bad;
            if (!l->source(l, expanded, error, cap))
                return false;
            continue;
        } else if (!*section && !strcmp(key, "submap")) {
            ok = !strcmp(expanded, "reset") || !strcmp(expanded, "resize");
            if (ok)
                l->resize = !strcmp(expanded, "resize");
        } else if (!*section && (!strncmp(key, "bind", 4) || !strcmp(key, "unbind")))
            ok = w_binding(l, key, expanded);
        else {
            char path[192];
            int k = snprintf(path, sizeof(path), "%s%s%s", section, *section ? ":" : "", key);
            ok = k >= 0 && (size_t)k < sizeof(path) && w_option(l, path, expanded);
        }
        if (ok)
            continue;
    bad:
        snprintf(error, cap, "line %d: unsupported directive or invalid value near '%.64s'", line, s);
        return false;
    }
    if (*section) {
        snprintf(error, cap, "line %d: unclosed section %s", line, section);
        return false;
    }
    return true;
}
