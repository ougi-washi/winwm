#include "os_win32/os_win32.h"

////////////////
//~ Modifier Map

static const wchar_t layout_path[] = L"SYSTEM\\CurrentControlSet\\Control\\Keyboard Layout";
static const wchar_t journal_path[] = L"SOFTWARE\\Winwm\\PhysicalModifier";
enum { map_capacity = 1024 };

static const DWORD left_modifier = 0xe05b0064u, right_modifier = 0xe05c0065u;

typedef struct w_scan_map {
    DWORD words[map_capacity];
    DWORD bytes;
} w_scan_map;

////////////////
//~ Modifier Map Functions

static LSTATUS w_read_map(HKEY key, const wchar_t *name, w_scan_map *map) {
    map->bytes = sizeof(map->words);
    LSTATUS error = RegGetValueW(key, NULL, name, RRF_RT_REG_BINARY, NULL, map->words, &map->bytes);
    if (error != ERROR_SUCCESS)
        map->bytes = 0;
    return error;
}

static bool w_valid_map(const w_scan_map *map) {
    return map->bytes >= 16 && map->bytes % 4 == 0 && map->words[0] == 0 && map->words[1] == 0 &&
           map->words[2] == map->bytes / 4 - 3 && map->words[map->bytes / 4 - 1] == 0;
}

static bool w_same_map(const w_scan_map *a, const w_scan_map *b) {
    return a->bytes == b->bytes && memcmp(a->words, b->words, a->bytes) == 0;
}

static LSTATUS w_write_map(HKEY key, const wchar_t *name, const w_scan_map *map) {
    return RegSetValueExW(key, name, 0, REG_BINARY, (const BYTE *)map->words, map->bytes);
}

void w_os_modifier_keys(bool mapped[2]) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, layout_path, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return;
    w_scan_map current;
    if (w_read_map(key, L"Scancode Map", &current) == ERROR_SUCCESS && w_valid_map(&current)) {
        for (DWORD i = 3; i + 1 < current.bytes / 4; ++i) {
            mapped[0] |= current.words[i] == left_modifier;
            mapped[1] |= current.words[i] == right_modifier;
        }
    }
    RegCloseKey(key);
}

static bool w_uses_modifier(WORD scan) {
    return scan == 0xe05b || scan == 0xe05c || scan == 0x0064 || scan == 0x0065;
}

static LSTATUS w_edit_map(HKEY layout, HKEY journal, bool install) {
    w_scan_map current = {0}, original = {0}, installed = {0};
    LSTATUS error = w_read_map(layout, L"Scancode Map", &current);
    if (error == ERROR_SUCCESS && !w_valid_map(&current))
        return ERROR_INVALID_DATA;
    if (error != ERROR_SUCCESS && error != ERROR_FILE_NOT_FOUND)
        return error;
    error = w_read_map(journal, L"Installed", &installed);
    if (error == ERROR_SUCCESS) {
        if (!w_valid_map(&installed) || w_read_map(journal, L"Original", &original) != ERROR_SUCCESS ||
            (original.bytes && !w_valid_map(&original)))
            return ERROR_INVALID_DATA;
        if (!w_same_map(&current, &original) && !w_same_map(&current, &installed))
            return ERROR_REVISION_MISMATCH;
        if (install)
            return w_same_map(&current, &installed) ? ERROR_SUCCESS
                                                  : w_write_map(layout, L"Scancode Map", &installed);
        if (original.bytes)
            error = w_write_map(layout, L"Scancode Map", &original);
        else {
            error = RegDeleteValueW(layout, L"Scancode Map");
            if (error == ERROR_FILE_NOT_FOUND)
                error = ERROR_SUCCESS;
        }
        if (!error)
            error = RegDeleteValueW(journal, L"Installed");
        if (!error)
            error = RegDeleteValueW(journal, L"Original");
        return error;
    }
    if (error != ERROR_FILE_NOT_FOUND || !install)
        return error == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : error;
    if (current.bytes + 8 > sizeof(installed.words))
        return ERROR_BUFFER_OVERFLOW;
    installed = current;
    DWORD end = current.bytes ? current.bytes / 4 - 1 : 3;
    for (DWORD i = 3; i < end; ++i)
        if (w_uses_modifier(HIWORD(current.words[i])) || w_uses_modifier(LOWORD(current.words[i])))
            return ERROR_ALREADY_ASSIGNED;
    installed.words[end++] = left_modifier;
    installed.words[end++] = right_modifier;
    installed.words[end++] = 0;
    installed.words[2] = end - 3;
    installed.bytes = end * sizeof(DWORD);
    error = w_write_map(journal, L"Original", &current);
    if (!error)
        error = w_write_map(journal, L"Installed", &installed);
    if (!error)
        error = w_write_map(layout, L"Scancode Map", &installed);
    return error;
}

DWORD w_os_modifier_map(bool install) {
    HANDLE mutex = CreateMutexW(NULL, FALSE, L"Global\\winwm.physical_modifier");
    if (!mutex)
        return GetLastError();
    DWORD wait = WaitForSingleObject(mutex, 5000);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
        CloseHandle(mutex);
        return ERROR_BUSY;
    }
    HKEY layout = NULL, journal = NULL;
    LSTATUS error =
        RegOpenKeyExW(HKEY_LOCAL_MACHINE, layout_path, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &layout);
    if (!error) {
        REGSAM access = KEY_QUERY_VALUE | KEY_SET_VALUE;
        if (install)
            error =
                RegCreateKeyExW(HKEY_LOCAL_MACHINE, journal_path, 0, NULL, 0, access, NULL, &journal, NULL);
        else
            error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, journal_path, 0, access, &journal);
        if (!error)
            error = w_edit_map(layout, journal, install);
        else if (!install && error == ERROR_FILE_NOT_FOUND)
            error = ERROR_SUCCESS;
    }
    if (layout)
        RegCloseKey(layout);
    if (journal)
        RegCloseKey(journal);
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return error;
}
