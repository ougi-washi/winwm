#include "desktop/entry.h"
#include "desktop_ctl/entry.h"
#include "desktop_tools/entry.h"
#include "win_platform/popup.h"
#include <shellapi.h>
#include <stdio.h>
#ifndef _WIN64
#error Build with an x64 Windows compiler.
#endif

////////////////
//~ App

static int w_run_bar_tool(wchar_t **argv) {
    unsigned long long handle;
    DWORD serial, pid = 0;
    wchar_t extra;
    RECT r;
    if (swscanf(argv[3], L"%llu%lc", &handle, &extra) != 1 ||
        swscanf(argv[4], L"%ld%lc", &r.left, &extra) != 1 ||
        swscanf(argv[5], L"%ld%lc", &r.top, &extra) != 1 ||
        swscanf(argv[6], L"%ld%lc", &r.right, &extra) != 1 ||
        swscanf(argv[7], L"%ld%lc", &r.bottom, &extra) != 1 ||
        swscanf(argv[9], L"%lu%lc", &serial, &extra) != 1) return ERROR_INVALID_PARAMETER;
    HWND bar = (HWND)(uintptr_t)handle;
    if (!GetWindowThreadProcessId(bar, &pid)) return ERROR_INVALID_WINDOW_HANDLE;
    wchar_t name[popup_event_chars];
    w_popup_event_name(name, pid, serial);
    HANDLE cancel = OpenEventW(SYNCHRONIZE, FALSE, name);
    if (!cancel) return ERROR_CANCELLED;
    if (!w_tools_set_popup(bar, r, argv[8], cancel)) {
        CloseHandle(cancel);
        return ERROR_CANCELLED;
    }
    return w_tools_run(argv[1] + 7);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR line, int show) {
    (void)previous;
    (void)line;
    (void)show;
    int argc = 0, result = 2;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv)
        return 1;
    if (argc == 1 || (argc == 2 && !wcscmp(argv[1], L"--preview")))
        result = w_session_run(instance, argc == 2);
    else if (argc == 2 && !wcsncmp(argv[1], L"--tool-", 7))
        result = w_tools_run(argv[1] + 7);
    else if (argc == 10 && !wcsncmp(argv[1], L"--tool-", 7) && !wcscmp(argv[2], L"--bar-popup"))
        result = w_run_bar_tool(argv);
    else if (!wcscmp(argv[1], L"ctl")) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            if (_fileno(stdout) < 0)
                freopen("CONOUT$", "w", stdout);
            if (_fileno(stderr) < 0)
                freopen("CONOUT$", "w", stderr);
        }
        result = w_ctl_run(argc - 1, argv + 1);
    }
    LocalFree(argv);
    return result;
}
