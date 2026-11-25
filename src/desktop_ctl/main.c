#include "desktop_ctl/entry.h"
#include "ipc/ipc.h"
#include <stdio.h>
#include <wchar.h>

////////////////
//~ Desktop Ctl

int w_ctl_run(int argc, wchar_t **argv) {
    if (argc < 2) {
        fwprintf(
            stderr,
            L"Usage: winwm.exe ctl run ACTION [ARG] | query | subscribe | reload | validate | doctor\n");
        return 2;
    }
    if (argc == 3 && !wcscmp(argv[1], L"physical-modifier") &&
        (!wcscmp(argv[2], L"install") || !wcscmp(argv[2], L"restore"))) {
        DWORD error = w_os_modifier_map(!wcscmp(argv[2], L"install"));
        if (error == ERROR_ACCESS_DENIED)
            fputs("Open an elevated terminal to change the machine keyboard map.\n", stderr);
        else if (error == ERROR_ALREADY_ASSIGNED || error == ERROR_REVISION_MISMATCH)
            fputs("The existing keyboard map conflicts or changed externally. Review it before retrying.\n",
                  stderr);
        else if (error)
            fprintf(stderr, "Keyboard mapping failed (Windows error %lu).\n", error);
        else
            puts("Restart Windows to apply the physical modifier mapping to all accounts.");
        return error ? 1 : 0;
    }
    if (argc == 2 && (!wcscmp(argv[1], L"validate") || !wcscmp(argv[1], L"prepare"))) {
        static w_desktop_config cfg;
        char error[256];
        if (!w_os_config_load(&cfg, error, sizeof(error))) {
            fprintf(stderr, "%s\n", error);
            return 1;
        }
        if (!wcscmp(argv[1], L"prepare")) {
            wchar_t path[os_path_cap];
            if (!w_os_provider(cfg.file_browser, path, count_of(path)) ||
                !w_os_task_manager(cfg.task_manager, path, count_of(path))) {
                fputs("File browser or task manager executable unavailable.\n", stderr);
                return 1;
            }
            w_os_cache_write(&cfg);
        }
        printf("Valid configuration: %d bindings, %d rules, %d apps\n", cfg.binding_count, cfg.rule_count,
               cfg.app_count);
        return 0;
    }
    char request[2048] = {0};
    for (int i = 1; i < argc; i++) {
        size_t n = strlen(request);
        if (i > 1) {
            if (n + 2 >= sizeof(request))
                return 2;
            request[n++] = ' ';
        }
        if (!w_os_utf8(argv[i], request + n, (int)(sizeof(request) - n)))
            return 2;
    }
    wchar_t name[96];
    if (!w_ipc_name(name, count_of(name)))
        return 1;
    HANDLE pipe = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                              SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, NULL);
    if (pipe == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "winwm unavailable or busy (Windows error %lu)\n", GetLastError());
        return 1;
    }
    ULONG pid;
    if (GetNamedPipeServerProcessId(pipe, &pid))
        AllowSetForegroundWindow(pid);
    DWORD mode = PIPE_READMODE_MESSAGE, n = 0;
    SetNamedPipeHandleState(pipe, &mode, NULL, NULL);
    bool stream = !strcmp(request, "subscribe"), ok = false;
    if (!WriteFile(pipe, request, (DWORD)strlen(request) + 1, &n, NULL)) {
        CloseHandle(pipe);
        return 1;
    }
    do {
        char reply[ipc_capacity];
        if (!ReadFile(pipe, reply, sizeof(reply), &n, NULL) || !n || reply[n - 1])
            break;
        puts(reply);
        fflush(stdout);
        ok = strncmp(reply, "error:", 6) != 0;
    } while (stream && ok);
    CloseHandle(pipe);
    return ok ? 0 : 1;
}
