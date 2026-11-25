#include "command/command.h"
#include <stdlib.h>
#define name_string(name) #name,

////////////////
//~ Command

static const char *const names[] = {"invalid", command_list(name_string)};

////////////////
//~ Command Functions

const char *w_command_name(enum command_kind k) {
    return k > cmd_invalid && k < cmd_count ? names[k] : names[0];
}

bool w_command_delta(const char *args, int *dx, int *dy) {
    char *end;
    long x = strtol(args, &end, 10);
    if (end == args || (*end != ' ' && *end != '\t') || x < -10000 || x > 10000)
        return false;
    args = end;
    long y = strtol(args, &end, 10);
    if (end == args || *end || y < -10000 || y > 10000)
        return false;
    *dx = (int)x;
    *dy = (int)y;
    return true;
}

int w_command_direction(const char *s) {
    const char *const dirs[] = {"left", "down", "up", "right"};
    for (int i = 0; i < 4; i++)
        if (strcmp(dirs[i], s) == 0)
            return i;
    return -1;
}

bool w_command_parse(const char *s, w_command *out) {
    size_t n = strcspn(s, " ");
    for (int i = 1; i < cmd_count; i++) {
        if (strlen(names[i]) != n || strncmp(s, names[i], n))
            continue;
        w_command c = {.kind = (enum command_kind)i};
        s += n;
        while (*s == ' ')
            ++s;
        if (!w_str_copy(c.arg, sizeof(c.arg), s))
            return false;
        switch (i) {
        case cmd_volume_set:
            if (!*s || strlen(s) > 3 || s[strspn(s, "0123456789")] || atoi(s) > 100)
                return false;
            break;
        case cmd_move:
        case cmd_focus:
            if (w_command_direction(s) < 0)
                return false;
            break;
        case cmd_resize_by: {
            int dx, dy;
            if (!w_command_delta(s, &dx, &dy))
                return false;
            break;
        }
        case cmd_set_mode:
            if (strcmp(s, "resize") && strcmp(s, "reset"))
                return false;
            break;
        case cmd_workspace_follow:
        case cmd_workspace_select:
        case cmd_workspace_send:
            if (!(s[0] >= '1' && s[0] <= '9' && s[1] == 0) && strcmp(s, "10") &&
                !(*s && (*s < '0' || *s > '9') && strlen(s) < 40))
                return false;
            break;
        case cmd_layout:
            if (strcmp(s, "tree") && strcmp(s, "alternate") && strcmp(s, "master") && strcmp(s, "tabs") &&
                strcmp(s, "stack") && strcmp(s, "toggle_split"))
                return false;
            break;
        case cmd_split:
            if (strcmp(s, "horizontal") && strcmp(s, "vertical"))
                return false;
            break;
        case cmd_resize:
            if (strcmp(s, "grow") && strcmp(s, "shrink") && w_command_direction(s) < 0)
                return false;
            break;
        case cmd_program:
        case cmd_launch:
        case cmd_reveal_file:
        case cmd_notify:
            if (!*s)
                return false;
            break;
        case cmd_open_folder:
            break;
        default:
            if (*s)
                return false;
        }
        *out = c;
        return true;
    }
    return false;
}
