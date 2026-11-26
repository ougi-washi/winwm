// Exercise the production shutdown policy on a private desktop. Every process
// that can be stopped here is a child fixture, including the hung tray owner.
#include "os_win32/os_win32.h"
#include <restartmanager.h>
#include <tlhelp32.h>
#include <stdio.h>

enum { surrender_window = WM_APP + 1 };
enum response { accept_shutdown, refuse_shutdown, lose_ownership, cancel_shutdown, refuse_responsive };
static enum response response;
static HWND test_shell, fixture_shell_window, hung_window;
static DWORD target_pid;
static HANDLE cancel, job;
static RM_UNIQUE_PROCESS registered;
static unsigned graceful_calls, forced_calls, posted_closes;
static bool wrong_target, older_fixture;
static int fixture_threads, thread_index;

static BOOL fixture_thread_first(HANDLE snapshot, THREADENTRY32 *entry) {
    if (!fixture_threads) return Thread32First(snapshot, entry);
    thread_index = 0;
    entry->th32OwnerProcessID = target_pid;
    entry->th32ThreadID = GetWindowThreadProcessId(fixture_shell_window, NULL);
    return TRUE;
}

static BOOL fixture_thread_next(HANDLE snapshot, THREADENTRY32 *entry) {
    if (!fixture_threads) return Thread32Next(snapshot, entry);
    if (++thread_index < fixture_threads) {
        ++entry->th32ThreadID;
        return TRUE;
    }
    SetLastError(ERROR_NO_MORE_FILES);
    return FALSE;
}

static void test_clock(FILETIME *now) {
    GetSystemTimeAsFileTime(now);
    if (older_fixture) {
        ULARGE_INTEGER value = {.LowPart = now->dwLowDateTime, .HighPart = now->dwHighDateTime};
        value.QuadPart += 600000000ULL;
        now->dwLowDateTime = value.LowPart;
        now->dwHighDateTime = value.HighPart;
    }
}

static DWORD rm_register(UINT count, const RM_UNIQUE_PROCESS *apps) {
    if (count != 1 || apps[0].dwProcessId != target_pid) {
        wrong_target = true;
        return ERROR_INVALID_OWNER;
    }
    registered = apps[0];
    return ERROR_SUCCESS;
}

static DWORD rm_list(UINT *needed, UINT *count, RM_PROCESS_INFO *apps, DWORD *reason) {
    if (*count != 1) return ERROR_BAD_ARGUMENTS;
    *needed = *count = 1;
    apps[0].Process = registered;
    *reason = RmRebootReasonNone;
    return ERROR_SUCCESS;
}

static DWORD rm_shutdown(void) {
    ++graceful_calls;
    HWND w = fixture_shell_window;
    DWORD pid = 0;
    if (!GetWindowThreadProcessId(w, &pid) || pid != target_pid) return ERROR_NOT_FOUND;
    if (response == accept_shutdown)
        return PostMessageW(w, WM_CLOSE, 0, 0) ? ERROR_SUCCESS : GetLastError();
    if (response == lose_ownership && !SendMessageTimeoutW(w, surrender_window, 0, 0, SMTO_ABORTIFHUNG, 1000, NULL))
        return ERROR_TIMEOUT;
    if (response == cancel_shutdown) SetEvent(cancel);
    return ERROR_FAIL_SHUTDOWN;
}

static BOOL checked_terminate(HANDLE process, UINT code) {
    BOOL contained = FALSE;
    if (GetProcessId(process) != target_pid || !IsProcessInJob(process, job, &contained) || !contained) {
        wrong_target = true;
        SetLastError(ERROR_INVALID_OWNER);
        return FALSE;
    }
    ++forced_calls;
    return TerminateProcess(process, code);
}

static BOOL fixture_post(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CLOSE) { ++graceful_calls; ++posted_closes; }
    return PostMessageW(window, msg, wp, lp);
}

#define GetShellWindow() test_shell
#define GetSystemTimeAsFileTime(time) test_clock(time)
#define IsHungAppWindow(window) ((window) == hung_window)
#define PostMessageW(window, msg, wp, lp) fixture_post(window, msg, wp, lp)
#define Thread32First(snapshot, entry) fixture_thread_first(snapshot, entry)
#define Thread32Next(snapshot, entry) fixture_thread_next(snapshot, entry)
#define RmStartSession(session, flags, key) (*(session) = 1, (void)(key), ERROR_SUCCESS)
#define RmRegisterResources(s, nf, files, na, apps, ns, services) ((void)(s), rm_register(na, apps))
#define RmGetList(s, needed, count, apps, reason) rm_list(needed, count, apps, reason)
#define RmShutdown(s, flags, callback) rm_shutdown()
#define RmEndSession(s) ((void)(s))
#define TerminateProcess(process, code) checked_terminate(process, code)
#include "../src/os_win32/explorer.c"
#undef TerminateProcess
#undef PostMessageW

void w_environment_for_launch(void) {}

static LRESULT CALLBACK fixture_proc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == surrender_window) {
        wchar_t cls[64];
        if (GetClassNameW(window, cls, count_of(cls)) && !wcscmp(cls, L"SystemUserAdapterWindowClass")) {
            CreateWindowW(L"CabinetWClass", L"Folder opened", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                          0, 0, 100, 100, NULL, NULL, GetModuleHandleW(NULL), NULL);
            return 0;
        }
        DestroyWindow(window);
        return 0;
    }
    if (msg == WM_CLOSE) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, msg, wp, lp);
}

static DWORD WINAPI stalled_tray(void *unused) {
    (void)unused;
    WNDCLASSW wc = {.lpfnWndProc = fixture_proc, .hInstance = GetModuleHandleW(NULL), .lpszClassName = L"Shell_TrayWnd"};
    if (!RegisterClassW(&wc) || !CreateWindowW(wc.lpszClassName, L"Stalled tray", WS_POPUP | WS_VISIBLE,
                                               0, 0, 100, 20, NULL, NULL, wc.hInstance, NULL)) return 1;
    Sleep(INFINITE);
    return 0;
}

static int fixture_main(const wchar_t *class_name, bool hung) {
    WNDCLASSW wc = {.lpfnWndProc = fixture_proc, .hInstance = GetModuleHandleW(NULL), .lpszClassName = class_name};
    bool background = !wcscmp(class_name, L"SystemUserAdapterWindowClass");
    if (background) {
        wc.lpszClassName = L"CabinetWClass";
        if (!RegisterClassW(&wc)) return 1;
        wc.lpszClassName = L"WorkerW";
        if (!RegisterClassW(&wc) || !CreateWindowW(wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                                                    NULL, wc.hInstance, NULL)) return 1;
        wc.lpszClassName = class_name;
    }
    if (!RegisterClassW(&wc) || !CreateWindowW(wc.lpszClassName, L"Shell test",
                       background ? 0 : WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 100, 100,
                       background ? HWND_MESSAGE : NULL, NULL, wc.hInstance, NULL)) return 1;
    if (!wcscmp(class_name, L"WinwmFixtureSplitShell")) {
        HANDLE thread = CreateThread(NULL, 0, stalled_tray, NULL, 0, NULL);
        if (!thread) return 1;
        CloseHandle(thread);
        hung = false;
    }
    if (hung) Sleep(INFINITE);
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) DispatchMessageW(&m);
    return 0;
}

static wchar_t desktop_name[80];
static HANDLE fixture(const wchar_t *exe, const wchar_t *class_name, bool hung, HWND *window) {
    wchar_t line[MAX_PATH + 128];
    swprintf(line, count_of(line), L"\"%ls\" %ls %ls", exe, class_name, hung ? L"hung" : L"normal");
    STARTUPINFOW si = {.cb = sizeof(si), .lpDesktop = desktop_name};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(exe, line, NULL, NULL, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return NULL;
    if (!AssignProcessToJobObject(job, pi.hProcess) || ResumeThread(pi.hThread) == (DWORD)-1) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return NULL;
    }
    CloseHandle(pi.hThread);
    ULONGLONG deadline = GetTickCount64() + 5000;
    while (GetTickCount64() < deadline && WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT) {
        HWND w = NULL;
        HWND parent = !wcscmp(class_name, L"SystemUserAdapterWindowClass") ? HWND_MESSAGE : GetDesktopWindow();
        while ((w = FindWindowExW(parent, w, class_name, NULL))) {
            if (w_window_pid(w) != pi.dwProcessId) continue;
            *window = w;
            return pi.hProcess;
        }
        Sleep(10);
    }
    CloseHandle(pi.hProcess);
    return NULL;
}

enum fixture_role { tray_role, desktop_role, remnant_role, split_role };

static bool exercise(const wchar_t *exe, enum fixture_role role, enum response reply) {
    HWND window = NULL;
    const wchar_t *classes[] = {L"Shell_TrayWnd", L"WinwmFixtureShellDesktop", L"SystemUserAdapterWindowClass",
                                L"WinwmFixtureSplitShell"};
    HANDLE child = fixture(exe, classes[role], reply == refuse_shutdown, &window);
    if (!child) return false;
    target_pid = GetProcessId(child);
    fixture_shell_window = window;
    test_shell = role == desktop_role || role == split_role ? window : NULL;
    response = reply;
    graceful_calls = forced_calls = posted_closes = 0;
    hung_window = role != remnant_role && reply == refuse_shutdown ? window : NULL;
    if (role == split_role) {
        ULONGLONG deadline = GetTickCount64() + 3000;
        while (!(hung_window = w_shell_tray(target_pid)) && GetTickCount64() < deadline) Sleep(10);
        if (!hung_window || !SendMessageTimeoutW(window, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 500, NULL)) return false;
    }
    ResetEvent(cancel);
    older_fixture = false;
    if (role == remnant_role) {
        if (w_exit_explorer(exe, cancel) != ERROR_NOT_FOUND || graceful_calls || forced_calls) return false;
        older_fixture = true;
        // Loader worker threads in a newly started test process can outlive
        // this whole test. Model both snapshots of an otherwise identical
        // aged process using the actual PID and GUI thread ID.
        fixture_threads = 2;
        if (w_exit_explorer(exe, cancel) != ERROR_NOT_FOUND || graceful_calls || forced_calls) return false;
        fixture_threads = 1;
    }
    ULONGLONG before = GetTickCount64();
    DWORD result = w_exit_explorer(exe, cancel);
    ULONGLONG elapsed = GetTickCount64() - before;
    bool ended = WaitForSingleObject(child, 0) == WAIT_OBJECT_0;
    bool keep_running = reply == lose_ownership || reply == cancel_shutdown;
    DWORD expected = reply == lose_ownership ? ERROR_NOT_FOUND :
                     reply == cancel_shutdown ? ERROR_CANCELLED : ERROR_SUCCESS;
    bool force = reply == refuse_shutdown || reply == refuse_responsive;
    bool ok = result == expected && ended != keep_running && !wrong_target &&
              graceful_calls == 1 && forced_calls == (force ? 1u : 0u) &&
              posted_closes == (hung_window ? 1u : 0u) && (!hung_window || elapsed < 3000);
    printf("%ls, response %d: result %lu, graceful %u, forced %u, exited %d, %llu ms (%s)\n",
           classes[role], (int)reply, result, graceful_calls, forced_calls, ended, elapsed, ok ? "passed" : "FAILED");
    if (!ok && role == remnant_role) {
        HWND w = NULL;
        while ((w = FindWindowExW(GetDesktopWindow(), w, NULL, NULL))) {
            if (w_window_pid(w) != target_pid) continue;
            wchar_t cls[80];
            GetClassNameW(w, cls, 80);
            printf(" top class %ls\n", cls);
        }
        printf(" sole thread %d\n", w_only_thread(target_pid, GetWindowThreadProcessId(window, NULL)));
    }
    // These two cases intentionally leave our own now-unmanaged process alive.
    if (keep_running && !ended) TerminateProcess(child, 0);
    WaitForSingleObject(child, 3000);
    CloseHandle(child);
    test_shell = NULL;
    fixture_threads = 0;
    return ok;
}

int wmain(int argc, wchar_t **argv) {
    if (argc == 3) return fixture_main(argv[1], !wcscmp(argv[2], L"hung"));
    wchar_t self[MAX_PATH], temp[MAX_PATH], copy[MAX_PATH];
    DWORD n = GetTempPathW(count_of(temp), temp);
    swprintf(desktop_name, count_of(desktop_name), L"WinwmShellScope-%lu-%llu", GetCurrentProcessId(), GetTickCount64());
    if (!n || n + wcslen(desktop_name) + 5 >= count_of(copy) || !GetModuleFileNameW(NULL, self, count_of(self)))
        return 2;
    swprintf(copy, count_of(copy), L"%ls%ls.exe", temp, desktop_name);
    HDESK desktop = CreateDesktopW(desktop_name, NULL, NULL, 0, GENERIC_ALL, NULL);
    if (!desktop || !SetThreadDesktop(desktop)) return 2;
    job = CreateJobObjectW(NULL, NULL);
    cancel = CreateEventW(NULL, TRUE, FALSE, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !cancel || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
        !CopyFileW(self, copy, TRUE)) return 2;
    HWND unused;
    HANDLE folder = fixture(self, L"CabinetWClass", false, &unused);
    HANDLE impostor = fixture(copy, L"Shell_TrayWnd", false, &unused);
    bool ok = folder && impostor && w_exit_explorer(self, cancel) == ERROR_NOT_FOUND &&
              exercise(self, tray_role, accept_shutdown) && exercise(self, tray_role, refuse_responsive) &&
              exercise(self, tray_role, refuse_shutdown) &&
              exercise(self, desktop_role, refuse_shutdown) && exercise(self, split_role, refuse_shutdown) &&
              exercise(self, tray_role, lose_ownership) &&
              exercise(self, tray_role, cancel_shutdown) && exercise(self, remnant_role, refuse_shutdown) &&
              exercise(self, remnant_role, lose_ownership) &&
              WaitForSingleObject(folder, 0) == WAIT_TIMEOUT && WaitForSingleObject(impostor, 0) == WAIT_TIMEOUT;
    printf("Separate folder process and unrelated tray survived: %s\n", ok ? "passed" : "FAILED");
    CloseHandle(job);
    if (folder) { WaitForSingleObject(folder, 3000); CloseHandle(folder); }
    if (impostor) { WaitForSingleObject(impostor, 3000); CloseHandle(impostor); }
    DeleteFileW(copy);
    CloseHandle(cancel);
    return ok ? 0 : 1;
}
