#include "desktop_ctl/entry.h"
#include "ipc/ipc.h"
#include <stdio.h>
#include <wchar.h>

////////////////
//~ Desktop Ctl

int w_ctl_run(int argc, wchar_t **argv) {
    if (argc == 2 && (!wcscmp(argv[1],L"--help") || !wcscmp(argv[1],L"help"))) {
        puts("winwm.exe [--preview]\nwinwm.exe ACTION [ARG...]\n"
             "winwm.exe query [provider NAME] | subscribe | doctor | validate | reload\n"
             "winwm.exe physical-modifier install|restore");
        for (int i=1; i<cmd_count; ++i)
            printf("%s%s",w_command_name((enum command_kind)i),i==cmd_count-1 ? "\n" : " ");
        return 0;
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
    bool activate=argc==2 && !wcscmp(argv[1],L"validate-activation");
    if (argc == 2 && (activate || !wcscmp(argv[1], L"validate") || !wcscmp(argv[1], L"validate-startup") || !wcscmp(argv[1], L"prepare"))) {
        if (activate && w_os_startup_shell()) {
            fputs("Windows shell configuration prevents automatic winwm startup.\n",stderr);
            return 1;
        }
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
    bool control = !wcscmp(argv[1],L"query") || !wcscmp(argv[1],L"subscribe") ||
                   !wcscmp(argv[1],L"doctor") || !wcscmp(argv[1],L"reload") || !wcscmp(argv[1],L"reload-bar");
    char request[2048] = "run ";
    size_t used = control ? 0 : 4;
    bool program = !wcscmp(argv[1],L"program");
    w_command action;
    for (int i = 1; i < argc; i++) {
        if (i > 1) {
            if (used + 1 >= sizeof(request))
                return 2;
            request[used++] = ' ';
        }
        const wchar_t *arg = argv[i];
        wchar_t quoted[2 * sizeof(action.arg) + 3];
        if (i > 1 && program && (!*arg || wcspbrk(arg,L" \t\""))) {
            if (2 * wcslen(arg) + 3 > count_of(quoted)) return 2;
            w_os_quote_argument(quoted,arg);
            arg = quoted;
        }
        if (!w_os_utf8(arg, request + used, (int)(sizeof(request) - used)))
            return 2;
        used += strlen(request + used);
    }
    if (!control && !w_command_parse(request+4,&action)) {
        fputs("Invalid command or arguments. See winwm.exe --help.\n",stderr);
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
