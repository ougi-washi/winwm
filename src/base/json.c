#include "base/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

////////////////
//~ Json

static bool w_fail(w_json_doc *d, const char *why) {
    snprintf(d->error, sizeof(d->error), "line %d: %s", d->line, why);
    return false;
}

static bool w_skip(w_json_doc *d) {
    for (;;) {
        while (isspace((unsigned char)*d->cursor))
            if (*d->cursor++ == '\n')
                ++d->line;
        if (strncmp(d->cursor, "//", 2) == 0) {
            while (*d->cursor && *d->cursor != '\n')
                ++d->cursor;
        } else if (strncmp(d->cursor, "/*", 2) == 0) {
            d->cursor += 2;
            while (*d->cursor && strncmp(d->cursor, "*/", 2) != 0)
                if (*d->cursor++ == '\n')
                    ++d->line;
            if (!*d->cursor)
                return w_fail(d, "unterminated comment");
            d->cursor += 2;
        } else
            return true;
    }
}

static int w_hex4(const char *s) {
    int n = 0;
    for (int i = 0; i < 4; i++) {
        unsigned c = (unsigned char)s[i];
        int v;
        if (c >= '0' && c <= '9')
            v = c - '0';
        else if ((c | 32) >= 'a' && (c | 32) <= 'f')
            v = (c | 32) - 'a' + 10;
        else
            return -1;
        n = n * 16 + v;
    }
    return n;
}

bool w_json_read(const w_json_token *t, char *out, size_t cap) {
    if (t->type != json_string || !cap)
        return false;
    size_t used = 0;
    for (size_t i = 0; i < t->length; i++) {
        unsigned char c = t->text[i];
        char bytes[4];
        size_t n = 1;
        bytes[0] = (char)c;
        if (c == '\\') {
            if (++i >= t->length)
                return false;
            c = t->text[i];
            if (c == 'u') {
                if (i + 4 >= t->length)
                    return false;
                int v = w_hex4(t->text + i + 1);
                i += 4;
                if (v >= 0xd800 && v <= 0xdbff) {
                    if (i + 6 >= t->length || t->text[i + 1] != '\\' || t->text[i + 2] != 'u')
                        return false;
                    int low = w_hex4(t->text + i + 3);
                    if (low < 0xdc00 || low > 0xdfff)
                        return false;
                    v = 0x10000 + ((v - 0xd800) << 10) + low - 0xdc00;
                    i += 6;
                }
                if (v <= 0 || (v >= 0xdc00 && v <= 0xdfff))
                    return false;
                if (v < 128)
                    bytes[0] = (char)v;
                else if (v < 2048) {
                    n = 2;
                    bytes[0] = 0xc0 | (v >> 6);
                    bytes[1] = 0x80 | (v & 63);
                } else if (v < 65536) {
                    n = 3;
                    bytes[0] = 0xe0 | (v >> 12);
                    bytes[1] = 0x80 | ((v >> 6) & 63);
                    bytes[2] = 0x80 | (v & 63);
                } else {
                    n = 4;
                    bytes[0] = 0xf0 | (v >> 18);
                    bytes[1] = 0x80 | ((v >> 12) & 63);
                    bytes[2] = 0x80 | ((v >> 6) & 63);
                    bytes[3] = 0x80 | (v & 63);
                }
            } else {
                const char *allowed = "\"\\/bfnrt", *match = strchr(allowed, c);
                if (!match)
                    return false;
                bytes[0] = "\"\\/\b\f\n\r\t"[match - allowed];
            }
        }
        if (used + n >= cap)
            return false;
        memcpy(out + used, bytes, n);
        used += n;
    }
    out[used] = 0;
    return true;
}

static bool w_value(w_json_doc *d, int depth) {
    if (depth > 32 || d->count >= d->capacity)
        return w_fail(d, "configuration too complex");
    if (!w_skip(d))
        return false;
    w_json_token *t = &d->tokens[d->count++];
    *t = (w_json_token){.line = d->line, .text = d->cursor};
    char start = *d->cursor;
    if (start == '[' || start == '{') {
        t->type = start == '{' ? json_object : json_array;
        char end = start == '{' ? '}' : ']';
        d->cursor++;
        if (!w_skip(d))
            return false;
        while (*d->cursor != end) {
            if (start == '{') {
                if (*d->cursor != '"' || !w_value(d, depth + 1))
                    return w_fail(d, "expected object key");
                if (!w_skip(d) || *d->cursor != ':')
                    return w_fail(d, "expected colon");
                d->cursor++;
            }
            if (!w_value(d, depth + 1) || !w_skip(d))
                return false;
            if (*d->cursor == end)
                break;
            if (*d->cursor != ',')
                return w_fail(d, "expected comma");
            d->cursor++;
            if (!w_skip(d))
                return false;
        }
        d->cursor++;
    } else if (start == '"') {
        t->type = json_string;
        t->text = ++d->cursor;
        while (*d->cursor != '"') {
            if ((unsigned char)*d->cursor < 32)
                return w_fail(d, "invalid string");
            if (*d->cursor == '\\') {
                d->cursor++;
                if (!*d->cursor || !strchr("\"\\/bfnrtu", *d->cursor))
                    return w_fail(d, "invalid escape");
                if (*d->cursor == 'u') {
                    if (w_hex4(d->cursor + 1) < 0)
                        return w_fail(d, "invalid unicode escape");
                    d->cursor += 4;
                }
            }
            d->cursor++;
        }
        t->length = d->cursor++ - t->text;
    } else {
        t->type = json_scalar;
        while (*d->cursor && !strchr(",]}/", *d->cursor) && !isspace((unsigned char)*d->cursor))
            d->cursor++;
        t->length = d->cursor - t->text;
        if (!t->length)
            return w_fail(d, "expected value");
    }
    t->after = d->count;
    return true;
}

////////////////
//~ Document parsing

bool w_json_parse(w_json_doc *d, const char *s, w_json_token *tokens, int cap) {
    *d = (w_json_doc){.tokens = tokens, .capacity = cap, .line = 1, .cursor = s};
    if (!w_value(d, 0) || !w_skip(d))
        return false;
    return !*d->cursor || w_fail(d, "unexpected trailing input");
}
