#pragma once
#include "base/base.h"
#include <windows.h>

typedef struct {
    uint64_t idle, total;
    DWORD processors;
} w_cpu_times;

w_cpu_times w_cpu_read(void);
double w_cpu_percent(w_cpu_times before, w_cpu_times after);
void w_cpu_format(wchar_t *text, size_t cap, double percent);
