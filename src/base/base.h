#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define count_of(a) (sizeof(a) / sizeof((a)[0]))

enum { window_limit = 512, output_limit = 16, workspace_limit = 10 };

////////////////
//~ Base

typedef struct w_rect {
    int x, y, w, h;
} w_rect;

////////////////
//~ Base Interface

static inline bool w_rect_equal(w_rect a, w_rect b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

static inline int w_clamp_int(int n, int lo, int hi) {
    return n < lo ? lo : n > hi ? hi : n;
}

static inline bool w_str_copy(char *dst, size_t cap, const char *src) {
    size_t n = strlen(src);
    if (n >= cap)
        return false;
    memcpy(dst, src, n + 1);
    return true;
}
