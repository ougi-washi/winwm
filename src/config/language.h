#pragma once
#include "config/config.h"

////////////////
//~ Language

typedef struct w_config_language {
    w_desktop_config *config;
    struct w_config_variable {
        char name[64], value[1024];
    } variables[96];
    int variable_count;
    struct { char name[32]; w_curve curve; } curves[16];
    int curve_count;
    bool resize;
    void *files;
    bool (*source)(struct w_config_language *, const char *, char *, size_t);
} w_config_language;

////////////////
//~ Language Interface

bool w_config_language_parse(w_config_language *language, char *text, char *error, size_t capacity);
