#pragma once
#include "base/base.h"
enum json_type { json_object, json_array, json_string, json_scalar };

////////////////
//~ Json

typedef struct w_json_token {
    enum json_type type;
    int after, line;
    const char *text;
    size_t length;
} w_json_token;

typedef struct w_json_doc {
    w_json_token *tokens;
    int count, capacity, line;
    const char *cursor;
    char error[160];
} w_json_doc;

////////////////
//~ Json Interface

bool w_json_parse(w_json_doc *doc, const char *source, w_json_token *tokens, int capacity);
bool w_json_read(const w_json_token *t, char *out, size_t cap);
