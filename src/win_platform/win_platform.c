#include "win_platform/win_platform.h"
#include "config/language.h"
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

////////////////
//~ Win Platform

static const char starter[] =
#include "config/default_conf.inc"
    ;
enum { file_limit = 256 * 1024, include_limit = 16, total_limit = 1024 * 1024 };

typedef struct w_file_stack {
    wchar_t paths[include_limit][os_path_cap];
    const wchar_t *candidate;
    unsigned depth, bytes, count;
} w_file_stack;

////////////////
//~ Win Platform Functions

static bool w_present(const wchar_t *path) {
    DWORD attr = GetFileAttributesW(path);
    if (attr != INVALID_FILE_ATTRIBUTES)
        return true;
    DWORD e = GetLastError();
    return e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND;
}

static bool w_make_parent(const wchar_t *path) {
    wchar_t dir[os_path_cap];
    wcscpy(dir, path);
    wchar_t *end = wcsrchr(dir, L'\\');
    if (!end)
        return false;
    *end = 0;
    wchar_t *parent = wcsrchr(dir, L'\\');
    if (!parent)
        return false;
    *parent = 0;
    if (!CreateDirectoryW(dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        return false;
    *parent = L'\\';
    return CreateDirectoryW(dir, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

// Publish the complete file with a non-replacing rename so concurrent starts preserve user edits.
static bool w_publish(const wchar_t *path, const char *bytes, const wchar_t *copy) {
    wchar_t temp[os_path_cap + 72];
    static LONG serial;
    swprintf(temp, count_of(temp), L"%ls.%lu.%ld.tmp", path, GetCurrentProcessId(),
             InterlockedIncrement(&serial));
    bool ok = false;
    if (copy)
        ok = CopyFileW(copy, temp, TRUE) != 0;
    else {
        HANDLE file = CreateFileW(temp, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE)
            return false;
        DWORD len = (DWORD)strlen(bytes), written = 0;
        ok = WriteFile(file, bytes, len, &written, NULL) && written == len;
        CloseHandle(file);
    }
    if (ok) {
        ok = MoveFileExW(temp, path, MOVEFILE_WRITE_THROUGH) != 0;
        if (!ok) {
            DWORD e = GetLastError();
            ok = e == ERROR_ALREADY_EXISTS || e == ERROR_FILE_EXISTS;
        }
    }
    DeleteFileW(temp);
    return ok;
}

////////////////
//~ Configuration setup

bool w_os_config_seed(bool migrate) {
    wchar_t path[os_path_cap], old[os_path_cap], local[os_path_cap];
    if (!w_os_config_path(path, count_of(path)))
        return false;
    if (w_present(path))
        return true;
    if (!w_make_parent(path))
        return false;
    if (migrate) {
        wcscpy(local, path);
        wchar_t *name = wcsrchr(local, L'\\') + 1;
        if ((size_t)(name - local) + wcslen(L"legacy.jsonc") >= count_of(local))
            return false;
        wcscpy(name, L"legacy.jsonc");
        if (!w_present(local) && w_os_legacy_config_path(old, count_of(old)) && w_present(old) &&
            !w_publish(local, NULL, old))
            return false;
        if (w_present(local))
            return w_publish(path, "source = legacy.jsonc\n", NULL);
    }
    return w_publish(path, starter, NULL);
}

static bool w_parse_file(w_config_language *lang, const wchar_t *path, char *err, size_t cap);

////////////////
//~ Configuration includes

static int w_filename_order(const void *a, const void *b) {
    return _wcsicmp((const wchar_t *)a, (const wchar_t *)b);
}

static bool w_glob_files(w_config_language *lang, wchar_t *pattern, char *err, size_t cap) {
    WIN32_FIND_DATAW entry;
    HANDLE search = FindFirstFileW(pattern, &entry);
    if (search == INVALID_HANDLE_VALUE) {
        snprintf(err, cap, "Source pattern matched no files");
        return false;
    }
    wchar_t(*names)[MAX_PATH] = calloc(256, sizeof(*names));
    int count = 0;
    bool ok = names != NULL;
    while (ok) {
        if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            if (count >= 256) {
                ok = false;
                break;
            }
            wcscpy(names[count++], entry.cFileName);
        }
        if (!FindNextFileW(search, &entry)) {
            ok = GetLastError() == ERROR_NO_MORE_FILES;
            break;
        }
    }
    FindClose(search);
    if (!ok || !count) {
        free(names);
        snprintf(err, cap, "Failed to enumerate source pattern");
        return false;
    }
    qsort(names, count, sizeof(*names), w_filename_order);
    wchar_t *name = wcsrchr(pattern, L'\\') + 1;
    for (int i = 0; ok && i < count; i++) {
        if ((size_t)(name - pattern) + wcslen(names[i]) >= os_path_cap) {
            snprintf(err, cap, "Source path too long");
            ok = false;
            break;
        }
        wcscpy(name, names[i]);
        ok = w_parse_file(lang, pattern, err, cap);
    }
    free(names);
    return ok;
}

static bool w_include_file(w_config_language *lang, const char *source, char *err, size_t cap) {
    w_file_stack *stack = lang->files;
    wchar_t value[os_path_cap], base[os_path_cap], canonical[os_path_cap];
    if (!w_os_wide(source, value, count_of(value)))
        goto invalid;
    for (wchar_t *p = value; *p; p++)
        if (*p == '/')
            *p = L'\\';
    const wchar_t *tail = value;
    if (value[0] == L'~' && value[1] == L'\\') {
        DWORD n = GetEnvironmentVariableW(L"USERPROFILE", base, count_of(base));
        if (!n || n >= count_of(base))
            goto invalid;
        tail = value + 1;
    } else if ((value[0] && value[1] == L':' && value[2] == L'\\') ||
               (value[0] == L'\\' && value[1] == L'\\'))
        *base = 0;
    else {
        wcscpy(base, stack->paths[stack->depth - 1]);
        wcsrchr(base, L'\\')[1] = 0;
    }
    if (wcslen(base) + wcslen(tail) >= count_of(base))
        goto invalid;
    wcscat(base, tail);
    DWORD n = GetFullPathNameW(base, count_of(canonical), canonical, NULL);
    if (!n || n >= count_of(canonical))
        goto invalid;
    if (wcspbrk(canonical, L"*?"))
        return w_glob_files(lang, canonical, err, cap);
    return w_parse_file(lang, canonical, err, cap);
invalid:
    snprintf(err, cap, "Invalid source path: %.120s", source);
    return false;
}

static bool w_parse_file(w_config_language *lang, const wchar_t *path, char *err, size_t cap) {
    w_file_stack *stack = lang->files;
    if (++stack->count > 256) {
        snprintf(err, cap, "Too many sourced files");
        return false;
    }
    if (stack->depth >= include_limit) {
        snprintf(err, cap, "Source nesting exceeds %d", include_limit);
        return false;
    }
    for (unsigned i = 0; i < stack->depth; i++)
        if (!_wcsicmp(path, stack->paths[i])) {
            snprintf(err, cap, "Source cycle");
            return false;
        }
    const wchar_t *name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    char label[192] = "configuration";
    w_os_utf8(name, label, sizeof(label));
    FILE *f = _wfopen(!stack->depth && stack->candidate ? stack->candidate : path, L"rb");
    if (!f) {
        snprintf(err, cap, "Cannot read %.150s", label);
        return false;
    }
    char *text = malloc(file_limit + 2);
    bool ok = false;
    if (!text) {
        snprintf(err, cap, "Out of memory");
        fclose(f);
        return false;
    }
    size_t n = fread(text, 1, file_limit + 1, f);
    if (n <= file_limit && stack->bytes + n <= total_limit && !ferror(f) && !memchr(text, 0, n)) {
        text[n] = 0;
        char *body = n >= 3 && !memcmp(text, "\xef\xbb\xbf", 3) ? text + 3 : text;
        stack->bytes += (unsigned)n;
        wcscpy(stack->paths[stack->depth++], path);
        const wchar_t *extension = wcsrchr(name, L'.');
        if (extension && (!_wcsicmp(extension, L".jsonc") || !_wcsicmp(extension, L".json")))
            ok = w_config_merge(lang->config, body, err, cap);
        else
            ok = w_config_language_parse(lang, body, err, cap);
        stack->depth--;
        if (!ok && !strstr(err, " -> ")) {
            char reason[192];
            snprintf(reason, sizeof(reason), "%s", err);
            snprintf(err, cap, "%.66s -> %.174s", label, reason);
        }
    } else
        snprintf(err, cap, "Invalid or oversized configuration: %.120s", label);
    fclose(f);
    free(text);
    return ok;
}

bool w_os_config_load(w_desktop_config *out, char *err, size_t cap) {
    wchar_t path[os_path_cap];
    if (!w_os_config_path(path, count_of(path)) || !w_os_config_seed(true)) {
        snprintf(err, cap, "Cannot create the user profile's .config/winwm/winwm.conf");
        return false;
    }
    return w_os_config_read_path(path, out, err, cap);
}

bool w_os_config_read_path(const wchar_t *path, w_desktop_config *out, char *err, size_t cap) {
    return w_os_config_read_candidate(path,NULL,out,err,cap);
}

bool w_os_config_read_candidate(const wchar_t *path, const wchar_t *candidate, w_desktop_config *out,
                                char *err, size_t cap) {
    w_file_stack *stack = calloc(1, sizeof(*stack));
    w_config_language *lang = calloc(1, sizeof(*lang));
    w_desktop_config *next = malloc(sizeof(*next));
    bool ok = false;
    if (lang && stack && next) {
        stack->candidate = candidate;
        w_config_defaults(next);
        lang->config = next;
        lang->source = w_include_file;
        lang->files = stack;
        ok = w_parse_file(lang, path, err, cap) && w_config_validate_providers(next, err, cap);
        if (ok) {
            *out = *next;
            if (cap)
                *err = 0;
        }
    } else
        snprintf(err, cap, "Out of memory");
    free(next);
    free(lang);
    free(stack);
    return ok;
}
