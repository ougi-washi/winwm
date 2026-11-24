#define WIN32_LEAN_AND_MEAN
#include <windows.h>
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
static const wchar_t *const previous_values[slot_count] = {L"PreviousA", L"PreviousB", L"PreviousC"};
static const wchar_t install_key[] = L"Software\\winwm\\Install";
typedef wchar_t path_string[path_cap];

static const wchar_t policy_key[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System";
typedef struct w_install_paths {
    path_string root, source_shell, source_setup;
    path_string slot[slot_count], shell[slot_count], command[slot_count], setup[slot_count];
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
        if (!error && swprintf(p->command[i], path_cap, L"\"%ls\"", p->shell[i]) < 0)
            error = ERROR_FILENAME_EXCED_RANGE;
    }
    return error;
}

static bool w_absent(LSTATUS error) {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

// Only the exact commands this installer publishes are owned by it.
static DWORD w_active_slot(const w_install_paths *p, int *slot) {
    wchar_t command[path_cap];
    DWORD bytes = sizeof(command);
    *slot = -1;
    LSTATUS error =
        RegGetValueW(HKEY_CURRENT_USER, policy_key, L"Shell", RRF_RT_REG_SZ, NULL, command, &bytes);
    if (w_absent(error))
        return ERROR_SUCCESS;
    if (error)
        return error;
    for (int i = 0; i < slot_count; i++) {
        if (!_wcsicmp(command, p->command[i])) {
            *slot = i;
            return ERROR_SUCCESS;
        }
    }
    return ERROR_ALREADY_ASSIGNED;
}

static DWORD w_check_platform(void) {
    const wchar_t machine_key[] = L"Software\\Microsoft\\Windows NT\\CurrentVersion";
    wchar_t edition[96];
    DWORD size = sizeof(edition);
    LSTATUS error =
        RegGetValueW(HKEY_LOCAL_MACHINE, machine_key, L"EditionID", RRF_RT_REG_SZ, NULL, edition, &size);
    if (error)
        return error;
    const wchar_t *editions[] = {L"Professional", L"Education", L"Enterprise", L"IoTEnterprise"};
    bool supported = false;
    for (size_t i = 0; i < length_of(editions); i++)
        supported |= !_wcsnicmp(edition, editions[i], wcslen(editions[i]));
    if (!supported)
        return ERROR_NOT_SUPPORTED;
    DWORD lightweight = 0;
    size = sizeof(lightweight);
    error =
        RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\PolicyManager\\current\\device\\WindowsLogon",
                     L"OverrideShellProgram", RRF_RT_REG_DWORD, NULL, &lightweight, &size);
    if (error && !w_absent(error))
        return error;
    return lightweight ? ERROR_ACCESS_DISABLED_BY_POLICY : ERROR_SUCCESS;
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
    swprintf(command, length_of(command), L"\"%ls\" ctl validate", exe);
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
static DWORD w_previous_slot(int current, int *previous) {
    *previous = -1;
    if (current < 0)
        return ERROR_SUCCESS;
    DWORD value, bytes = sizeof(value);
    LSTATUS error = RegGetValueW(HKEY_CURRENT_USER, install_key, previous_values[current], RRF_RT_REG_DWORD,
                                 NULL, &value, &bytes);
    if (w_absent(error))
        return ERROR_SUCCESS;
    if (error)
        return error;
    if (value >= slot_count || (int)value == current)
        return ERROR_INVALID_DATA;
    *previous = (int)value;
    return ERROR_SUCCESS;
}

static DWORD w_activate(const w_install_paths *p, int current, int slot) {
    int observed;
    DWORD error = w_active_slot(p, &observed);
    if (error || observed != current)
        return error ? error : ERROR_REVISION_MISMATCH;
    if (current == slot)
        return ERROR_SUCCESS;
    if (current >= 0) {
        DWORD previous = (DWORD)current;
        error = RegSetKeyValueW(HKEY_CURRENT_USER, install_key, previous_values[slot], REG_DWORD, &previous,
                                sizeof(previous));
    } else {
        error = RegDeleteKeyValueW(HKEY_CURRENT_USER, install_key, previous_values[slot]);
        if (w_absent(error))
            error = ERROR_SUCCESS;
    }
    if (error)
        return error;
    return RegSetKeyValueW(HKEY_CURRENT_USER, policy_key, L"Shell", REG_SZ, p->command[slot],
                           (DWORD)((wcslen(p->command[slot]) + 1) * sizeof(wchar_t)));
}

static DWORD w_select_release(const w_install_paths *p, int current, bool rollback) {
    DWORD error = w_check_platform();
    int previous = -1;
    if (!error)
        error = w_previous_slot(current, &previous);
    if (error)
        return error;
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
            while (slot == current || slot == previous)
                ++slot;
        }
        error = w_create_directory(p->slot[slot]);
        if (!error)
            error = w_stage_file(p->source_shell, p->shell[slot]);
        if (!error)
            error = w_stage_file(p->source_setup, p->setup[slot]);
    }
    if (!error)
        error = w_validate(p->shell[slot], p->slot[slot]);
    return error ? error : w_activate(p, current, slot);
}

static DWORD w_uninstall(const w_install_paths *p, int current, bool *files_busy) {
    if (current >= 0) {
        LSTATUS error = RegDeleteKeyValueW(HKEY_CURRENT_USER, policy_key, L"Shell");
        if (error && !w_absent(error))
            return error;
    }
    for (int i = 0; i < slot_count; i++) {
        const wchar_t *owned[] = {p->shell[i], p->setup[i]};
        for (size_t j = 0; j < length_of(owned); j++) {
            if (!DeleteFileW(owned[j]) && !w_absent(GetLastError()))
                *files_busy = true;
        }
        RemoveDirectoryW(p->slot[i]);
        RegDeleteKeyValueW(HKEY_CURRENT_USER, install_key, previous_values[i]);
    }
    return ERROR_SUCCESS;
}

static DWORD w_change_shell(enum operation op, bool *files_busy) {
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
    error = w_active_slot(&p, &slot);
    if (!error)
        error = op == op_uninstall ? w_uninstall(&p, slot, files_busy)
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
        detail = op == op_uninstall  ? L"Your normal Windows shell will start at the next sign-in."
                 : op == op_rollback ? L"The previous winwm release will start at the next sign-in."
                                     : L"winwm will start as your shell at the next sign-in.";
        break;
    case ERROR_NOT_FOUND:
        detail = L"This installation has no previous release to restore.";
        break;
    case ERROR_ACCESS_DENIED:
        detail = L"Windows denied access to the install directory or this account's sign-in shell policy. "
                 L"Check the policy permissions for the account being configured.";
        break;
    case ERROR_ALREADY_ASSIGNED:
        detail = L"Another custom sign-in shell already owns this user policy.";
        break;
    case ERROR_NOT_SUPPORTED:
        detail =
            L"Custom User Interface requires a supported Pro, Education or Enterprise edition of Windows.";
        break;
    case ERROR_ACCESS_DISABLED_BY_POLICY:
        detail = L"An active Windows policy prevents installation.";
        break;
    case ERROR_FILE_NOT_FOUND:
        detail = L"Place winwm.exe beside winwm_install.exe and retry.";
        break;
    case ERROR_INVALID_DATA:
        detail = L"The staged shell failed configuration validation. Run winwm.exe ctl validate for details.";
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
        MessageBoxW(NULL, message, L"winwm setup", MB_OK | (error ? MB_ICONERROR : MB_ICONINFORMATION));
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
            remove ? L"Restore the normal Windows shell for this account at the next sign-in?"
            : rollback
                ? L"Use the previous installed winwm release at the next sign-in?"
                : L"Install winwm as this account's sign-in shell? The next sign-in will start winwm in "
                  L"place of the Explorer desktop.";
        if (MessageBoxW(NULL, question, L"winwm setup", MB_YESNO | MB_ICONQUESTION) != IDYES)
            return ERROR_CANCELLED;
    }
    bool files_busy = false;
    DWORD error = w_change_shell(op, &files_busy);
    w_report(interactive, op, files_busy, error);
    return (int)error;
}
