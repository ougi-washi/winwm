#include "install/install.h"
#include "theme/dialog.h"
#include <shlobj.h>
#include <shellapi.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define length_of(a) (sizeof(a) / sizeof((a)[0]))
enum { path_cap = 4096, slot_count = 3 };

////////////////
//~ Install

static const wchar_t *const slot_dirs[slot_count] = {L"shell-a", L"shell-b", L"shell-c"};
static const wchar_t *const previous_values[slot_count] = {L"StartupPreviousA", L"StartupPreviousB", L"StartupPreviousC"};
static const wchar_t install_key[] = L"Software\\winwm\\Install";
static const wchar_t run_key[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t run_name[] = L"winwm";
typedef wchar_t path_string[path_cap];

typedef struct w_install_paths {
    path_string root;
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
DWORD w_install_read(const wchar_t *path, w_install_image *image) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) return GetLastError();
    LARGE_INTEGER size;
    DWORD error = GetFileSizeEx(file,&size) ? ERROR_SUCCESS : GetLastError();
    if (!error && (size.QuadPart <= 0 || size.HighPart)) error = ERROR_FILE_INVALID;
    if (!error) {
        HANDLE mapping = CreateFileMappingW(file,NULL,PAGE_READONLY,0,0,NULL);
        const void *bytes = mapping ? MapViewOfFile(mapping,FILE_MAP_READ,0,0,0) : NULL;
        if (!bytes) error = GetLastError();
        else *image = (w_install_image){bytes,size.LowPart};
        if (mapping) CloseHandle(mapping);
    }
    CloseHandle(file);
    return error;
}

static DWORD w_files_equal(w_install_image image, const wchar_t *path, bool *equal) {
    w_install_image existing;
    DWORD error = w_install_read(path,&existing);
    *equal = false;
    if (w_absent(error) || error == ERROR_FILE_INVALID) return ERROR_SUCCESS;
    if (!error) {
        *equal = existing.size == image.size && !memcmp(existing.bytes,image.bytes,image.size);
        UnmapViewOfFile(existing.bytes);
    }
    return error;
}

static DWORD w_write_new(const wchar_t *path, w_install_image image) {
    HANDLE file = CreateFileW(path,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
    if (file == INVALID_HANDLE_VALUE) return GetLastError();
    DWORD written;
    DWORD error = WriteFile(file,image.bytes,image.size,&written,NULL) ? ERROR_SUCCESS : GetLastError();
    if (!error && written != image.size) error = ERROR_WRITE_FAULT;
    if (!error && !FlushFileBuffers(file)) error = GetLastError();
    CloseHandle(file);
    if (error) DeleteFileW(path);
    return error;
}

static DWORD w_stage_file(w_install_image image, const wchar_t *destination) {
    path_string temp;
    if (swprintf(temp, path_cap, L"%ls.new", destination) < 0)
        return ERROR_FILENAME_EXCED_RANGE;
    if (!DeleteFileW(temp) && !w_absent(GetLastError()))
        return GetLastError();
    DWORD error = w_write_new(temp,image);
    if (error) return error;
    if (!MoveFileExW(temp,destination,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
        error=GetLastError();
    if (error) DeleteFileW(temp);
    return error;
}

static w_install_image w_default_config(HMODULE module) {
    HRSRC res = FindResourceW(module,L"WINWM_DEFAULT_CONFIG",RT_RCDATA);
    if (!res) return (w_install_image){0};
    return (w_install_image){LockResource(LoadResource(module,res)),SizeofResource(module,res)};
}

static bool w_config_matches(const wchar_t *path,w_install_image stock) {
    bool equal;
    return stock.bytes && stock.size && !w_files_equal(stock,path,&equal) && equal;
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
    DWORD attrs = GetFileAttributesW(p->slot[slot]);
    if (attrs == INVALID_FILE_ATTRIBUTES) return w_absent(GetLastError());
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY) || (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    path_string pattern;
    if (w_join(pattern,p->slot[slot],L"*")) return false;
    WIN32_FIND_DATAW item;
    HANDLE search = FindFirstFileW(pattern,&item);
    if (search == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    bool empty = false;
    do {
        if (wcscmp(item.cFileName,L".") && wcscmp(item.cFileName,L"..")) break;
        if (!FindNextFileW(search,&item)) {
            empty = GetLastError() == ERROR_NO_MORE_FILES;
            break;
        }
    } while (true);
    FindClose(search);
    return empty;
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

// Older releases may reject settings in a newer shipped default. Switch only a
// byte-identical stock config and keep the original next to it as a backup.
static DWORD w_validate_release(const w_install_paths *p,int current,int slot) {
    DWORD error = w_validate(p->shell[slot],p->slot[slot]);
    if (!error) return w_activate(p,current,slot);
    path_string config, next, backup;
    if (error != ERROR_INVALID_DATA || !w_os_config_path(config,count_of(config))) return error;
    DWORD flags = LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE|LOAD_LIBRARY_AS_IMAGE_RESOURCE;
    HMODULE old = current >= 0 ? LoadLibraryExW(p->shell[current],NULL,flags) : NULL;
    HMODULE target = LoadLibraryExW(p->shell[slot],NULL,flags);
    w_install_image local = w_default_config(NULL), prior = w_default_config(old), defaults = w_default_config(target);
    ULONGLONG stamp = GetTickCount64();
    if (target && defaults.bytes && defaults.size && !w_config_matches(config,defaults) &&
        (w_config_matches(config,local) || w_config_matches(config,prior)) &&
        swprintf(next,path_cap,L"%ls.%lu-%llu.tmp",config,GetCurrentProcessId(),stamp)>0 &&
        swprintf(backup,path_cap,L"%ls.before-release-%lu-%llu",config,GetCurrentProcessId(),stamp)>0) {
        error = w_write_new(next,defaults);
        if (!error && !ReplaceFileW(config,next,backup,0,NULL,NULL)) {
            error = GetLastError();
            DeleteFileW(next);
        } else if (!error) {
            // Recheck the actual displaced file in case an editor saved during staging.
            error = w_config_matches(backup,local) || w_config_matches(backup,prior) ?
                    w_validate(p->shell[slot],p->slot[slot]) : ERROR_REVISION_MISMATCH;
            if (!error) error = w_activate(p,current,slot);
            if (error && w_config_matches(config,defaults) && !ReplaceFileW(config,backup,NULL,0,NULL,NULL))
                error = GetLastError();
        }
    }
    if (target) FreeLibrary(target);
    if (old) FreeLibrary(old);
    return error;
}
static DWORD w_select_release(const w_install_paths *p, int current, w_install_image image, bool rollback) {
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
            error = w_files_equal(image, p->shell[current], &identical);
            if (error)
                return error;
        }
        slot = current;
        if (!identical && previous >= 0) {
            error = w_files_equal(image,p->shell[previous],&identical);
            if (error) return error;
            if (identical) slot = previous;
        }
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
        if (!error && !identical)
            error = w_stage_file(image, p->shell[slot]);
        // An inactive slot may still contain the old split release's helpers.
        if (!error && !identical) {
            const wchar_t *legacy[] = {p->setup[slot],p->recovery[slot]};
            for (size_t i=0; i<count_of(legacy); ++i)
                if (!DeleteFileW(legacy[i]) && !w_absent(GetLastError())) return GetLastError();
        }
    }
    return error ? error : w_validate_release(p,current,slot);
}

static bool w_remove_installed_file(const wchar_t *path) {
    if (DeleteFileW(path)) return true;
    DWORD error = GetLastError();
    if (w_absent(error)) return true;
    if (error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION) return false;
    // Windows allows a mapped image to be renamed. Retire it to the user's
    // temporary files so uninstall works from the installed executable itself.
    DWORD attrs = GetFileAttributesW(path);
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & (FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)))
        return false;
    wchar_t temp[MAX_PATH+1], retired[MAX_PATH+1];
    DWORD n = GetTempPathW(count_of(temp),temp);
    if (!n || n >= count_of(temp) || !GetTempFileNameW(temp,L"wwm",0,retired)) return false;
    bool moved = MoveFileExW(path,retired,MOVEFILE_REPLACE_EXISTING) != FALSE;
    if (moved) SetFileAttributesW(retired,FILE_ATTRIBUTE_TEMPORARY);
    DeleteFileW(retired);
    return moved;
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
            if (!w_remove_installed_file(files[j])) busy = true;
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
DWORD w_install_current(wchar_t path[os_path_cap]) {
    w_install_paths p;
    int slot;
    path[0] = 0;
    DWORD error = w_get_paths(&p);
    if (!error) error = w_registered_slot(&p,run_key,run_name,&slot);
    if (!error && slot == foreign_slot) return ERROR_ALREADY_ASSIGNED;
    if (!error && slot >= 0) wcscpy(path,p.shell[slot]);
    return error;
}

static DWORD w_change_installation(enum w_install_operation op, w_install_image image, bool *files_busy) {
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
                                   : w_select_release(&p, slot, image, op == op_rollback);
    CloseHandle(lock);
    return error;
}

////////////////
//~ Installer interface

static void w_report(bool interactive, enum w_install_operation op, bool files_busy, DWORD error) {
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
        detail = L"The selected executable could not be found.";
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
                 files_busy ? L" To remove files still in use, run winwm.exe uninstall from your downloaded copy after signing out and back in."
                            : L"");
    }
    if (interactive)
        w_dialog(NULL, message, L"winwm setup", MB_OK | (error ? MB_ICONERROR : MB_ICONINFORMATION));
    else
        fwprintf(error ? stderr : stdout,L"%ls\n",message);
}

DWORD w_install_release(w_install_image image) {
    bool busy = false;
    return w_change_installation(op_install,image,&busy);
}

static DWORD w_install_self(void) {
    path_string self;
    if (!w_os_self(self,count_of(self))) return GetLastError();
    w_install_image image;
    DWORD error = w_install_read(self,&image);
    if (!error) {
        error = w_install_release(image);
        UnmapViewOfFile(image.bytes);
    }
    return error;
}

DWORD w_install_run(enum w_install_operation op, bool interactive) {
    if (interactive) {
        const wchar_t *question =
            op == op_uninstall ? L"Remove installed winwm releases and stop starting winwm at sign-in?"
            : op == op_rollback
                ? L"Use the previous installed winwm release at the next sign-in?"
                : L"Install winwm and start it alongside Windows each time this account signs in?";
        if (w_dialog(NULL, question, L"winwm setup", MB_YESNO | MB_ICONQUESTION) != IDYES)
            return ERROR_CANCELLED;
    }
    bool files_busy = false;
    DWORD error = op == op_install ? w_install_self() : w_change_installation(op,(w_install_image){0},&files_busy);
    w_report(interactive, op, files_busy, error);
    return (int)error;
}
