#pragma once
#include <stddef.h>
#include <stdbool.h>

typedef struct {
    wchar_t label[192];
    wchar_t detail[256];
} w_adapter_problem;

size_t w_network_problems(w_adapter_problem *out, size_t capacity, bool wireless);
