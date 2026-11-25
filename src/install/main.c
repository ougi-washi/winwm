#define WIN32_LEAN_AND_MEAN
#include "theme/dialog.h"
#include "os_win32/os_win32.h"
#include <shlobj.h>
#include <shellapi.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define length_of(a) (sizeof(a) / sizeof((a)[0]))
enum { path_cap = 4096, slot_count = 3 };
enum operation { op_install, op_uninstall, op_rollback };

////////////////
//~ Install

static const wchar_t *const slot_dirs[slot_count] = {L"shell-a", L"shell-b", L"shell-c"};
static const wchar_t *const previous_values[slot_count] = {L"StartupPreviousA", L"StartupPreviousB", L"StartupPreviousC"};
static const wchar_t install_key[] = L"Software\\winwm\\Install";
static const wchar_t run_key[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t run_name[] = L"winwm";
typedef wchar_t path_string[path_cap];

typedef struct w_install_paths {
    path_string root, source_shell, source_setup;
    path_string slot[slot_count], shell[slot_count], command[slot_count], setup[slot_count], recovery[slot_count];
} w_install_paths;

////////////////
//~ Install Functions

static DWORD w_join(path_string out, const wchar_t *directory, const wchar_t *leaf) {
    if (wcslen(directory) + wcslen(leaf) + 2 > path_cap)
        return ERROR_FILENAME_EXCED_RANGE;
    swprintf(out, path_cap, L"%ls\\%ls", directory, leaf);
    return ERROR_SUCCESS;
}

static DWORD w_get_paths(w_install_paths *p) {
    PWSTR local = NULL;
    HRESULT hr = SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &local);
    if (FAILED(hr))
        return HRESULT_FACILITY(hr) == FACILITY_WIN32 ? HRESULT_CODE(hr) : ERROR_PATH_NOT_FOUND;
    DWORD error = w_join(p->root, local, L"winwm");
    CoTaskMemFree(local);
    if (error)
        return error;
    DWORD n = GetModuleFileNameW(NULL, p->source_setup, path_cap);
    if (!n || n >= path_cap)
        return n ? ERROR_FILENAME_EXCED_RANGE : GetLastError();
    path_string source_directory;
    wcscpy(source_directory, p->source_setup);
    wchar_t *separator = wcsrchr(source_directory, L'\\');
    if (!separator)
        return ERROR_BAD_PATHNAME;
    *separator = 0;
    error = w_join(p->source_shell, source_directory, L"winwm.exe");
    for (int i = 0; !error && i < slot_count; i++) {
        error = w_join(p->slot[i], p->root, slot_dirs[i]);
        if (!error)
            error = w_join(p->shell[i], p->slot[i], L"winwm.exe");
        if (!error)
            error = w_join(p->setup[i], p->slot[i], L"winwm_install.exe");
        if (!error)
            error = w_join(p->recovery[i], p->slot[i], L"winwm_recovery.exe");
        if (!error && swprintf(p->command[i], path_cap, L"\"%ls\"", p->shell[i]) < 0)
            error = ERROR_FILENAME_EXCED_RANGE;
    }
    return error;
}

static bool w_absent(LSTATUS error) {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

enum { foreign_slot = -2, absent_slot = -1 };

// Only the exact REG_SZ commands this installer publishes are owned by it.
static DWORD w_registered_slot(const w_install_paths *p, const wchar_t *key, const wchar_t *name, int *slot) {
    wchar_t command[path_cap];
    DWORD bytes = sizeof(command);
    *slot = absent_slot;
    LSTATUS error = RegGetValueW(HKEY_CURRENT_USER,key,name,RRF_RT_REG_SZ|RRF_NOEXPAND,NULL,command,&bytes);
    if (w_absent(error))
        return ERROR_SUCCESS;
    *slot = foreign_slot;
    if (error == ERROR_UNSUPPORTED_TYPE || error == ERROR_MORE_DATA) return ERROR_SUCCESS;
    if (error)
        return error;
    for (int i = 0; i < slot_count; i++) {
        if (bytes == (wcslen(p->command[i])+1)*sizeof(wchar_t) && !_wcsicmp(command, p->command[i])) {
            *slot = i;
            return ERROR_SUCCESS;
        }
    }
    return ERROR_SUCCESS;
}

static DWORD w_remove_owned(const w_install_paths *p, const wchar_t *key, const wchar_t *name) {
    int slot;
    DWORD error = w_registered_slot(p,key,name,&slot);
    if (error || slot < 0) return error;
    error = RegDeleteKeyValueW(HKEY_CURRENT_USER,key,name);
    return w_absent(error) ? ERROR_SUCCESS : error;
}

static DWORD w_create_directory(const wchar_t *path) {
    if (CreateDirectoryW(path, NULL))
        return ERROR_SUCCESS;
    DWORD error = GetLastError();
    if (error == ERROR_ALREADY_EXISTS) {
        DWORD attrs = GetFileAttributesW(path);
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY))
            return ERROR_SUCCESS;
    }
    return error;
}

////////////////
//~ Release staging

// Comparison also covers retries from a running installed copy with either slot active.
static DWORD w_files_equal(const wchar_t *a, const wchar_t *b, bool *equal) {
    *equal = false;
    HANDLE first = CreateFileW(a, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (first == INVALID_HANDLE_VALUE)
        return GetLastError();
    HANDLE second = CreateFileW(b, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    DWORD error = ERROR_SUCCESS;
    if (second == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        if (w_absent(error))
            error = ERROR_SUCCESS;
    } else {
        BYTE left[16384], right[16384];
        for (;;) {
            DWORD n, m;
            if (!ReadFile(first, left, sizeof(left), &n, NULL) ||
                !ReadFile(second, right, sizeof(right), &m, NULL)) {
                error = GetLastError();
                break;
            }
            if (n != m || memcmp(left, right, n) != 0)
                break;
            if (!n) {
                *equal = true;
                break;
            }
        }
        CloseHandle(second);
    }
    CloseHandle(first);
    return error;
}

static DWORD w_stage_file(const wchar_t *source, const wchar_t *destination) {
    bool equal;
    DWORD error = w_files_equal(source, destination, &equal);
    if (error || equal)
        return error;
    path_string temp;
    if (swprintf(temp, path_cap, L"%ls.new", destination) < 0)
        return ERROR_FILENAME_EXCED_RANGE;
    if (!DeleteFileW(temp) && !w_absent(GetLastError()))
        return GetLastError();
    if (!CopyFileW(source, temp, TRUE)) {
        error = GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS)
            DeleteFileW(temp);
        return error;
    }
    if (!MoveFileExW(temp, destination, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = GetLastError();
        DeleteFileW(temp);
    }
    return error;
}

static DWORD w_validate(const wchar_t *exe, const wchar_t *directory) {
    wchar_t command[path_cap + 32];
    swprintf(command, length_of(command), L"\"%ls\" validate-startup", exe);
    STARTUPINFOW startup = {.cb = sizeof(startup)};
    PROCESS_INFORMATION process;
    if (!CreateProcessW(exe, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, directory, &startup,
                        &process))
        return GetLastError();
    CloseHandle(process.hThread);
    DWORD result = ERROR_SUCCESS, code;
    if (WaitForSingleObject(process.hProcess, INFINITE) != WAIT_OBJECT_0 ||
        !GetExitCodeProcess(process.hProcess, &code))
        result = GetLastError();
    else if (code)
        result = ERROR_INVALID_DATA;
    CloseHandle(process.hProcess);
    return result;
}

////////////////
//~ Release activation and rollback

// The live slot and its predecessor remain readable throughout an interrupted update.
static DWORD w_release_history(int current, bool owned[slot_count], int *previous) {
    *previous = absent_slot;
    if (current >= 0) owned[current] = true;
    for (int i = 0; i < slot_count; ++i) {
        DWORD value, bytes = sizeof(value);
        LSTATUS error = RegGetValueW(HKEY_CURRENT_USER,install_key,previous_values[i],RRF_RT_REG_DWORD,
                                     NULL,&value,&bytes);
        if (w_absent(error)) continue;
        if (error) return error;
        // slot_count identifies an installed release with no predecessor.
        if (value > slot_count || (int)value == i) return ERROR_INVALID_DATA;
        owned[i] = true;
        if (value < slot_count) {
            owned[value] = true;
            if (i == current) *previous = (int)value;
        }
    }
    return ERROR_SUCCESS;
}

static bool w_slot_empty(const w_install_paths *p, int slot) {
    const wchar_t *files[] = {p->shell[slot],p->setup[slot],p->recovery[slot]};
    for (size_t i = 0; i < length_of(files); ++i) {
        if (GetFileAttributesW(files[i]) != INVALID_FILE_ATTRIBUTES || !w_absent(GetLastError())) return false;
    }
    return true;
}

static DWORD w_activate(const w_install_paths *p, int current, int slot) {
    DWORD error = w_os_startup_shell();
    if (error) return error;
    int observed;
    error = w_registered_slot(p,run_key,run_name,&observed);
    if (error || observed != current) return error ? error : ERROR_REVISION_MISMATCH;
    if (current == slot) return ERROR_SUCCESS;
    if (wcslen(p->command[slot]) > 260) return ERROR_FILENAME_EXCED_RANGE;
    DWORD previous = current < 0 ? slot_count : (DWORD)current;
    error = RegSetKeyValueW(HKEY_CURRENT_USER,install_key,previous_values[slot],REG_DWORD,&previous,sizeof(previous));
    if (error) return error;
    return RegSetKeyValueW(HKEY_CURRENT_USER,run_key,run_name,REG_SZ,p->command[slot],
                           (DWORD)((wcslen(p->command[slot])+1)*sizeof(wchar_t)));
}
static DWORD w_select_release(const w_install_paths *p, int current, bool rollback) {
    if (current == foreign_slot) return ERROR_ALREADY_ASSIGNED;
    DWORD error = w_os_startup_shell();
    if (error) return error;
    bool owned[slot_count] = {false};
    int previous;
    error = w_release_history(current,owned,&previous);
    if (error) return error;
    int slot = previous;
    if (rollback) {
        if (previous < 0)
            return ERROR_NOT_FOUND;
    } else {
        bool identical = false;
        if (current >= 0) {
            error = w_files_equal(p->source_shell, p->shell[current], &identical);
            if (error)
                return error;
        }
        slot = current;
        if (!identical) {
            slot = 0;
            while (slot < slot_count && (slot == current || slot == previous ||
                                         (!owned[slot] && !w_slot_empty(p,slot)))) ++slot;
            if (slot == slot_count) return ERROR_BUSY;
        }
        if (!owned[slot]) {
            // Record the empty slot before copying. A partial stage remains retryable.
            DWORD none = slot_count;
            error = RegSetKeyValueW(HKEY_CURRENT_USER,install_key,previous_values[slot],
                                    REG_DWORD,&none,sizeof(none));
            if (error) return error;
        }
        error = w_create_directory(p->slot[slot]);
        if (!error)
            error = w_stage_file(p->source_shell, p->shell[slot]);
        if (!error)
            error = w_stage_file(p->source_setup, p->setup[slot]);
    }
    if (!error)
        error = w_validate(p->shell[slot], p->slot[slot]);
    return error ? error : w_activate(p,current,slot);
}

static DWORD w_uninstall(const w_install_paths *p, int current, bool *files_busy) {
    bool owned[slot_count] = {false};
    int previous;
    DWORD error = w_release_history(current,owned,&previous);
    if (error) return error;
    error = w_remove_owned(p,run_key,run_name);
    if (error) return error;
    for (int i = 0; i < slot_count; ++i) {
        if (!owned[i]) continue;
        const wchar_t *files[] = {p->shell[i],p->setup[i],p->recovery[i]};
        bool busy = false;
        for (size_t j = 0; j < length_of(files); ++j) {
            if (!DeleteFileW(files[j]) && !w_absent(GetLastError())) busy = true;
        }
        if (busy) {
            // Keep ownership after removing Run so a later uninstall can finish.
            DWORD none = slot_count;
            error = RegSetKeyValueW(HKEY_CURRENT_USER,install_key,previous_values[i],REG_DWORD,&none,sizeof(none));
            if (error) return error;
            *files_busy = true;
        } else {
            RemoveDirectoryW(p->slot[i]);
            error = RegDeleteKeyValueW(HKEY_CURRENT_USER,install_key,previous_values[i]);
            if (error && !w_absent(error)) return error;
        }
    }
    return ERROR_SUCCESS;
}
static DWORD w_change_installation(enum operation op, bool *files_busy) {
    w_install_paths p;
    DWORD error = w_get_paths(&p);
    if (error)
        return error;
    path_string lock_path;
    error = w_join(lock_path, p.root, L"shell-install.lock");
    if (!error)
        error = w_create_directory(p.root);
    if (error)
        return error;
    HANDLE lock = CreateFileW(lock_path, GENERIC_WRITE | DELETE, 0, NULL, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (lock == INVALID_HANDLE_VALUE)
        return GetLastError();
    int slot;
    error = w_registered_slot(&p,run_key,run_name,&slot);
    if (!error)
        error = op == op_uninstall ? w_uninstall(&p,slot,files_busy)
                                   : w_select_release(&p, slot, op == op_rollback);
    CloseHandle(lock);
    return error;
}

////////////////
//~ Installer interface

static void w_report(bool interactive, enum operation op, bool files_busy, DWORD error) {
    wchar_t system_error[512] = L"";
    wchar_t message[1024];
    const wchar_t *detail;
    switch (error) {
    case ERROR_SUCCESS:
        detail = op == op_uninstall  ? L"Removed winwm startup for this account."
                 : op == op_rollback ? L"The previous winwm release will start at the next sign-in."
                                     : L"winwm will start alongside Windows at the next sign-in.";
        break;
    case ERROR_NOT_FOUND:
        detail = L"This installation has no previous release to restore.";
        break;
    case ERROR_BUSY:
        detail = L"No release slot is free. The current release, its rollback copy, and files outside this installation occupy the available slots.";
        break;
    case ERROR_ACCESS_DENIED:
        detail = L"Windows denied access to the install directory or this account's startup settings.";
        break;
    case ERROR_ALREADY_ASSIGNED:
        detail = L"Another installation controls the startup entry or Windows logon shell.";
        break;
    case ERROR_FILE_NOT_FOUND:
        detail = L"Place winwm.exe beside winwm_install.exe and retry.";
        break;
    case ERROR_INVALID_DATA:
        detail = L"The staged release failed startup validation. Run winwm.exe validate-startup for details.";
        break;
    default:
        detail = L"Windows could not complete this operation.";
        break;
    }
    if (error) {
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, error, 0,
                       system_error, length_of(system_error), NULL);
        swprintf(message, length_of(message), L"%ls\nWindows error %lu. %ls", detail, error, system_error);
    } else {
        swprintf(message, length_of(message), L"%ls%ls", detail,
                 files_busy ? L" Files still in use can be removed by running /uninstall from the downloaded "
                              L"copy after signing in."
                            : L"");
    }
    if (interactive)
        w_dialog(NULL, message, L"winwm setup", MB_OK | (error ? MB_ICONERROR : MB_ICONINFORMATION));
    else if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE *stream = _wfopen(L"CONOUT$", L"w");
        if (stream) {
            fwprintf(stream, L"%ls\n", message);
            fclose(stream);
        }
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR args, int show) {
    (void)instance;
    (void)previous;
    (void)args;
    (void)show;
    int count = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!argv)
        return (int)GetLastError();
    const wchar_t *arg = count == 2 ? argv[1] : L"";
    bool interactive = count == 1 || !_wcsicmp(arg, L"/restore") || !_wcsicmp(arg, L"/previous");
    bool remove = !_wcsicmp(arg, L"/uninstall") || !_wcsicmp(arg, L"/restore");
    bool rollback = !_wcsicmp(arg, L"/rollback") || !_wcsicmp(arg, L"/previous");
    bool valid = interactive || remove || rollback || !_wcsicmp(arg, L"/install");
    enum operation op = remove ? op_uninstall : rollback ? op_rollback : op_install;
    LocalFree(argv);
    if (!valid)
        return ERROR_INVALID_PARAMETER;
    if (interactive) {
        const wchar_t *question =
            remove ? L"Remove winwm and its startup entry from this account?"
            : rollback
                ? L"Use the previous installed winwm release at the next sign-in?"
                : L"Install winwm and start it alongside Windows each time this account signs in?";
        if (w_dialog(NULL, question, L"winwm setup", MB_YESNO | MB_ICONQUESTION) != IDYES)
            return ERROR_CANCELLED;
    }
    bool files_busy = false;
    DWORD error = w_change_installation(op, &files_busy);
    w_report(interactive, op, files_busy, error);
    return (int)error;
}
