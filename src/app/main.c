#include "desktop/entry.h"
#include "desktop_ctl/entry.h"
#include "desktop_tools/entry.h"
#include "shell_host/entry.h"
#include <shellapi.h>
#include <stdio.h>
#ifndef _WIN64
#error Build with an x64 Windows compiler.
#endif

////////////////
//~ App

static int w_shell_result(int result) {
    if (result == ERROR_REVISION_MISMATCH)
        MessageBoxW(NULL, L"An earlier desktop manager is running in this session. Close it or sign out "
                          L"before starting winwm.", L"winwm", MB_OK | MB_ICONINFORMATION);
    return result;
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
        result = w_shell_result(w_host_run(instance, argc == 2));
    else if (argc == 2 && (!wcscmp(argv[1], L"--session") || !wcscmp(argv[1], L"--preview-session")))
        result = w_shell_result(w_session_run(instance, !wcscmp(argv[1], L"--preview-session")));
    else if (argc == 2 && !wcsncmp(argv[1], L"--tool-", 7))
        result = w_tools_run(argv[1] + 7);
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
