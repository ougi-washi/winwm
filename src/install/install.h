#pragma once
#include "os_win32/os_win32.h"

enum w_install_operation { op_install, op_uninstall, op_rollback };
typedef struct {
    const BYTE *bytes;
    DWORD size;
} w_install_image;

DWORD w_install_release(w_install_image image);
DWORD w_install_run(enum w_install_operation op, bool interactive);
DWORD w_install_current(wchar_t path[os_path_cap]);
// The caller releases the read-only view with UnmapViewOfFile.
DWORD w_install_read(const wchar_t *path, w_install_image *image);
