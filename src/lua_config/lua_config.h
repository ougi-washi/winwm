#pragma once
#include "config/language.h"

bool w_lua_config_parse(w_config_language *language, const char *text, const char *label,
                        char *error, size_t capacity);
