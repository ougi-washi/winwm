#pragma once
#include "config/config.h"

////////////////
//~ Language

typedef struct w_config_language {
    w_desktop_config *config;
    struct { char name[32]; w_curve curve; } curves[16];
    int curve_count;
    bool resize, module;
    void *files, *runtime;
    bool (*source)(struct w_config_language *, const char *, char *, size_t);
} w_config_language;

////////////////
//~ Language Interface

bool w_config_language_set(w_config_language *language, const char *key, char *value);
bool w_config_language_bind(w_config_language *language, const char *chord, const char *action,
                            bool repeating, bool release, enum w_mouse_action mouse);
bool w_config_language_animation(w_config_language *language, const char *leaf, bool enabled,
                                 double speed, const char *curve, const char *style);
bool w_config_literal_filter(char *pattern, char *out, size_t cap);
bool w_config_color(const char *s, uint32_t *out);
