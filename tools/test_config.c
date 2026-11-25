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

static bool test_bar_config(void) {
    char error[160];
    w_config_defaults(&config);
    if (!parse("bar {\n modules_left =\n modules_right = audio##desk\t cpu audio##headset\n"
               " tooltip_duration = 0\n workspace_count = 0\n audio##desk {\n"
               " display = icon_text\n on_click_right = volume_mixer\n"
               " tooltip_enabled = 0\n color = rgb(aabbcc)\n }\n}\n"))
        return false;
    w_module_style *s = &config.styles[0];
    if (*config.bar_left || strcmp(config.bar_right,"audio#desk cpu audio#headset") ||
        !w_config_has_module(&config,"audio") || config.bar_tooltip_duration || config.bar_workspace_count ||
        config.style_count != 1 || strcmp(s->name,"audio#desk") || s->display != module_icon_text ||
        !s->hide_tooltip || !s->colored || s->color != 0xaabbcc || strcmp(s->right_click,"volume_mixer"))
        return false;
    if (!w_config_merge(&config,"{\"version\":1,\"bar_right\":[\"clock\",\"audio#desk\"],\"bar_center\":[],"
        "\"modules\":{\"audio#desk\":{\"on_click_right\":\"\",\"on_click_middle\":\"mute\",\"tooltip_enabled\":true}}}",
        error,sizeof(error))) return false;
    if (config.style_count != 1 || strcmp(config.bar_right,"clock audio#desk") || *config.bar_center ||
        *s->right_click || strcmp(s->middle_click,"mute") || s->hide_tooltip || s->display != module_icon_text)
        return false;
    return !w_config_merge(&config,"{\"version\":1,\"bar_right\":[\"audio\",\"unknown-module\"]}",error,sizeof(error)) &&
           !strcmp(config.bar_right,"clock audio#desk") &&
           !w_config_modules_valid("audio#desk\nbar") && !w_config_style_option(s,"format","invalid\alabel") &&
           w_config_style_option(s,"tooltip","Volume\n{text}") &&
           !parse("bar:audio:on_click_right = no_such_action") &&
           !parse("bar:modules_right = audio##") && !parse("bar:tooltip_delay = -1");
}

int main(void) {
    w_config_defaults(&config);
    language.config = &config;
    static const char stock[] =
#include "config/default_conf.inc"
    ;
    FILE *asset = fopen("assets/winwm.conf", "r");
    if (!asset)
        return 1;
    char text[sizeof(stock)];
    size_t n = fread(text, 1, sizeof(text), asset);
    bool matches = !ferror(asset) && n == sizeof(stock)-1 && !memcmp(text, stock, n);
    fclose(asset);
    if (!matches || !parse(stock)) {
        fputs("Invalid or mismatched default configuration.\n", stderr);
        return 2;
    }
    if (parse("animations:animation = windowsMove, 1, nan, default") || !test_json_sources() || !test_bar_config())
        return 3;
    puts("Shipped defaults, configuration merging and failed reloads passed.");
    return 0;
}
