// gcc -Isrc tools/test_appearance_config.c src/config/{language,config}.c src/command/command.c src/base/json.c
#include "config/language.h"
#include <stdio.h>
#include <stdlib.h>

static w_desktop_config config;
static w_config_language language;

static bool parse(const char *text) {
    char error[256];
    char *copy = malloc(strlen(text) + 1);
    if (!copy)
        return false;
    strcpy(copy, text);
    bool ok = w_config_language_parse(&language, copy, error, sizeof(error));
    free(copy);
    return ok;
}

static bool test_json_sources(void) {
    static w_desktop_config previous;
    char error[256];
    const char *first =
        "{\"version\":1,\"apps\":{\"Editor\":\"old.exe\"},\"bar_left\":\"custom/status\","
        "\"modules\":{\"audio\":{\"format\":\"Volume\"}}}";
    const char *second =
        "{\"version\":1,\"providers\":{\"status\":{\"executable\":\"provider.exe\"}},"
        "\"modules\":{\"audio\":{\"tooltip\":\"Sound\"}},\"apps\":{\"Editor\":\"new.exe\"}}";
    w_config_defaults(&config);
    if (!parse("general:border_size = 9") ||
        !w_config_merge(&config, first, error, sizeof(error)) ||
        !w_config_merge(&config, second, error, sizeof(error)) ||
        !w_config_validate_providers(&config, error, sizeof(error)) ||
        config.border_size != 9 || config.app_count != 1 || strcmp(config.apps[0].path, "new.exe") ||
        config.style_count != 1 || strcmp(config.styles[0].format, "Volume") ||
        strcmp(config.styles[0].tooltip, "Sound"))
        return false;
    previous = config;
    if (w_config_merge(&config, "{\"version\":1,\"border_size\":3,\"gap\":-1}", error, sizeof(error)) ||
        memcmp(&config, &previous, sizeof(config)))
        return false;
    if (w_config_parse(&config, first, error, sizeof(error)) || !strstr(error, "Unknown bar provider") ||
        memcmp(&config, &previous, sizeof(config)))
        return false;
    return w_config_parse(&config, "{\"version\":1}", error, sizeof(error)) &&
           config.border_size == 2 && !config.app_count && !config.style_count && !config.provider_count;
}

int main(void) {
    w_config_defaults(&config);
    language.config = &config;
    const char *stock =
#include "config/default_conf.inc"
    ;
    if (!parse(stock) || config.motion[motion_open].ms != 160 || config.active_gradient.end != 0x76daca ||
        config.active_gradient.angle != 45 || config.border_size != 2)
        return 1;
    if (!parse("animations:animation = windowsMove, 0\nanimations:animation = border, 1, 0.8, linear\n"
               "general:col.active_border = rgb(000001)\ngeneral:border_size = 0\n") ||
        config.motion[motion_move].ms != 0 || config.motion[motion_border].ms != 80 || config.active_gradient.enabled)
        return 2;
    const char *invalid[] = {"general:border_size = -1", "decoration:rounding = 33",
        "input:floating_modifier = invalid normal", "input:floating_modifier = super side",
        "general:col.active_border = rgb(ffffff) rgb(000000) invalid",
        "animations:animation = windowsMove, 1, nan, default",
        "animations:animation = workspaces, 1, 2, missing",
        "animations:animation = windowsIn, 1, 2, default, popin 101%",
        "animations:animation = layersOut, 1, 2, default, popin",
        "animations:animation = workspaces, 1, 2, default, slidevert 12",
        "animations:bezier = bad, 2, 0, 0, 1", "animations:animation = border, 1, 2, linear, slide"};
    for (size_t i = 0; i < count_of(invalid); i++)
        if (parse(invalid[i])) {
            fprintf(stderr, "Accepted invalid config: %s\n", invalid[i]);
            return 3;
        }
    if (!parse("input:floating_modifier = ALT+SHIFT inverse") || config.drag_mods != 5 || !config.inverse_drag)
        return 4;
    if (!parse("animations:bezier = spring, 0.2, 1.5, 0.5, 1.2\n"
               "animations:animation = windowsIn, 1, 2, spring, popin 80%\n"
               "animations:animation = workspaces, 1, 2, default, slidevert 20%\n"
               "animations:animation = barModules, 0\n") ||
        config.motion[motion_open].percent != 80 || config.motion[motion_workspace].style != style_slidevert ||
        config.motion[motion_workspace].percent != 20 || config.motion[motion_modules].ms ||
        config.motion[motion_layers_in].ms != 160 || config.motion[motion_layers_out].ms != 120 ||
        w_motion_progress(config.motion[motion_open],0) != 0 || w_motion_progress(config.motion[motion_open],130) <= 1 ||
        w_motion_progress(config.motion[motion_open],200) != 1)
        return 5;
    if (!parse("animations:animation = windowsIn, 1, 1.3, linear, slide left") || config.motion[motion_open].style != style_left)
        return 6;
    if (!test_json_sources())
        return 7;
    puts("Appearance settings, animation curves, ordered JSON sources and atomic parse errors passed.");
    return 0;
}
