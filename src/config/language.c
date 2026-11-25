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

static bool w_number(const char *s, int minimum, int maximum, int *out) {
    char *end;
    long value = strtol(s, &end, 10);
    if (s == end || *end || value < minimum || value > maximum)
        return false;
    *out = (int)value;
    return true;
}

static bool w_switch(const char *s, bool *out) {
    if (!strcmp(s,"true") || !strcmp(s,"yes") || !strcmp(s,"on") || !strcmp(s,"1")) *out = true;
    else if (!strcmp(s,"false") || !strcmp(s,"no") || !strcmp(s,"off") || !strcmp(s,"0")) *out = false;
    else return false;
    return true;
}

bool w_config_color(const char *s, uint32_t *out) {
    if (!strncmp(s,"0x",2) && strlen(s) == 10) {
        for (int i = 2; i < 10; ++i)
            if (!isxdigit((unsigned char)s[i])) return false;
        *out = (uint32_t)strtoul(s+2,NULL,16) & 0xffffff;
        return true;
    }
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

static bool w_gradient(char *value, uint32_t *start, w_border_gradient *gradient) {
    char *next = value + strcspn(value, " \t");
    if (!*next) {
        *gradient = (w_border_gradient){0};
        return w_config_color(value, start);
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
    return w_config_color(value, start) && w_config_color(next, &gradient->end);
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

bool w_config_language_animation(w_config_language *l, const char *name, bool enabled,
                                 double speed, const char *curve, const char *style) {
    const char *const leaves[motion_count] = {"windowsIn", "windowsMove", "workspaces", "border", "barModules",
                                             "layersIn", "layersOut", "fadeIn", "fadeSwitch"};
    int kind = 0;
    while (kind < motion_count && strcmp(name, leaves[kind]))
        kind++;
    if (kind == motion_count)
        return false;
    w_transition *transition = &l->config->motion[kind];
    if (!enabled) {
        transition->ms = 0;
        return true;
    }
    if (!isfinite(speed) || speed < .01 || speed > 30 || !curve)
        return false;
    w_curve easing = {.22, 1, .36, 1};
    int i = 0;
    while (i < l->curve_count && strcmp(curve, l->curves[i].name))
        ++i;
    if (i < l->curve_count)
        easing = l->curves[i].curve;
    else if (!strcmp(curve, "linear"))
        easing = (w_curve){0, 0, 1, 1};
    else if (strcmp(curve, "default"))
        return false;
    char value[64];
    w_transition t = {(int)(speed*100 + .5), easing, style_default, -1};
    if (!w_str_copy(value,sizeof(value),style ? style : "") || !w_animation_style(kind, value, &t))
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
                   {"mouse_up", "wheel_up"}, {"mouse_down", "wheel_down"},
                   {"mouse:275", "mouse4"}, {"mouse:276", "mouse5"}};
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
        const char *m = !strcmp(mods, "mod4") || !strcmp(mods, "win") || !strcmp(mods, "logo") ? "super" :
                        !strcmp(mods, "mod1") ? "alt" : !strcmp(mods, "control") ? "ctrl" : mods;
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

static bool w_chord_parts(char *spec, char **mods, char **key) {
    spec = w_trim(spec);
    char *last = strrchr(spec, '+');
    *mods = "";
    if (last) {
        *last = 0;
        *mods = w_trim(spec);
        if (!**mods) return false;
        spec = last + 1;
    }
    *key = w_trim(spec);
    return **key != 0;
}

bool w_config_language_bind(w_config_language *l, const char *spec, const char *action,
                            bool repeating, bool release, enum w_mouse_action mouse) {
    char buffer[160], first[80], second[80] = "", scoped[96];
    if (!w_str_copy(buffer, sizeof(buffer), spec) || (release && repeating)) return false;
    char *mods, *key, *next = strchr(buffer, ',');
    if (next) *next++ = 0;
    if (!w_chord_parts(buffer, &mods, &key)) return false;
    if (mouse != mouse_inherit) {
        int button = !strcmp(key,"mouse:272") ? 0 : !strcmp(key,"mouse:273") ? 1 : -1;
        unsigned mask, vk;
        char dummy[] = "a";
        if (button < 0 || l->resize || release || repeating || next ||
            !w_make_chord(mods,dummy,first,sizeof(first)) || !w_config_chord(first,&mask,&vk))
            return false;
        l->config->mouse_actions[button][mask] = (unsigned char)mouse;
        return true;
    }
    if (!w_make_chord(mods,key,first,sizeof(first))) return false;
    if (next && (!w_chord_parts(next,&mods,&key) ||
                 !w_make_chord(mods,key,second,sizeof(second)))) return false;
    int n = snprintf(scoped,sizeof(scoped),"%s%s%s%s%s",l->resize ? "resize:" : "",
                     repeating ? "repeat:" : release ? "release:" : "", first, next ? "," : "", second);
    return n >= 0 && (size_t)n < sizeof(scoped) && w_config_bind(l->config,scoped,action);
}

////////////////
//~ Window rules

bool w_config_literal_filter(char *s, char *out, size_t cap) {
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
        if (*s == '\\' && s[1] && !isalnum((unsigned char)s[1]))
            s++;
        else if (strchr(".^$*+?|[]{}()\\", *s))
            return false;
        if (n + 1 >= cap)
            return false;
        out[n++] = *s;
    }
    out[n] = 0;
    return n != 0;
}

bool w_config_language_set(w_config_language *l, const char *key, char *v) {
    w_desktop_config *c = l->config;
    if (!strncmp(key,"notifications:",14)) {
        w_notification_config *nc = &c->notifications;
        key += 14;
        if (!strcmp(key,"enabled")) return w_switch(v,&nc->enabled);
        if (!strcmp(key,"capture")) {
            if (!strcmp(v,"windows")) nc->capture = notice_capture_windows;
            else if (!strcmp(v,"visible")) nc->capture = notice_capture_visible;
            else if (!strcmp(v,"off")) nc->capture = notice_capture_off;
            else return false;
            return true;
        }
        if (!strcmp(key,"corner")) {
            const char *const names[] = {"top_right","top_left","bottom_right","bottom_left"};
            for (size_t i = 0; i < count_of(names); ++i)
                if (!strcmp(v,names[i])) { nc->corner = (int)i; return true; }
            return false;
        }
        int *setting = !strcmp(key,"duration_ms") ? &nc->duration_ms :
                       !strcmp(key,"fade_ms") ? &nc->fade_ms :
                       !strcmp(key,"history_limit") ? &nc->history_limit :
                       !strcmp(key,"visible_limit") ? &nc->visible_limit :
                       !strcmp(key,"margin") ? &nc->margin :
                       !strcmp(key,"width") ? &nc->width : NULL;
        if (!setting) return false;
        int previous = *setting;
        if (!w_number(v,0,3600000,setting)) return false;
        if (w_config_notifications_valid(nc)) return true;
        *setting = previous;
        return false;
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
        bool on;
        if (!w_switch(v,&on))
            return false;
        c->animations = on ? motion_on : motion_off;
        return true;
    }
    if (!strcmp(key,"decoration:panels.blur"))
        return w_switch(v,&c->panel_material.blur);
    if (!strcmp(key,"decoration:panels.shadow"))
        return w_switch(v,&c->panel_material.shadow);
    if (!strcmp(key,"decoration:inactive_opacity") || !strcmp(key,"decoration:panels.opacity")) {
        char *end;
        double f = strtod(v, &end);
        if (end == v || *end || !isfinite(f) || f < 0 || f > 1)
            return false;
        int alpha = (int)(255 * f + .5);
        if (!strcmp(key,"decoration:inactive_opacity")) c->inactive_alpha = (unsigned char)alpha;
        else c->panel_material.opacity = alpha;
        return true;
    }
    if (!strcmp(key, "input:follow_mouse") || !strcmp(key, "general:resize_on_border")) {
        return w_switch(v,!strcmp(key,"input:follow_mouse") ? &c->follow_mouse : &c->resize_on_border);
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
        return w_config_color(v, &c->background);
    if (!strcmp(key, "bar:foreground"))
        return w_config_color(v, &c->foreground);
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
            style->colored = w_config_color(v,&style->color);
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
