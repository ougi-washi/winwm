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
        } else if (!strcmp(arg, "Run")) {
            action = "launcher";
            param = "";
        } else if (!strcmp(arg, "brave") || !strcmp(arg, "brave.exe")) {
            action = "launch";
            param = "brave";
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
        return w_color(v, &c->accent);
    if (!strcmp(key, "general:col.inactive_border"))
        return w_color(v, &c->inactive_border);
    if (!strcmp(key, "decoration:inactive_opacity")) {
        char *end;
        double f = strtod(v, &end);
        if (end == v || *end || !isfinite(f) || f < 0 || f > 1)
            return false;
        c->inactive_alpha = (unsigned char)(255 * f + .5);
        return true;
    }
    if (!strcmp(key, "input:follow_mouse")) {
        int on;
        if (!w_number(v, 0, 1, &on))
            return false;
        c->follow_mouse = on != 0;
        return true;
    }
    if (!strcmp(key, "bar:height"))
        return w_number(v, 16, 128, &c->bar_height);
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
        return w_config_modules_valid(v) && w_str_copy(group, sizeof(c->bar_left), v);
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
