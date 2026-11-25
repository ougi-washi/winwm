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
    if (parse("animations:animation = windowsMove, 1, nan, default") || !test_json_sources())
        return 3;
    puts("Shipped defaults, configuration merging and failed reloads passed.");
    return 0;
}
