#define COBJMACROS
#include "desktop_tools/tools.h"
#include <shellapi.h>
#include <stdio.h>

// Exercise the real command construction and status handling with a local
// stand-in for Defender. The test never elevates or runs a scan/update.
static const wchar_t fixture_dir[] = L"C:\\Test platform with spaces";
static const wchar_t fixture_exe[] = L"C:\\Test platform with spaces\\MpCmdRun.exe";
static const wchar_t *parameters;
static DWORD process_result;
static int launches, failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "Security test line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

static LSTATUS WINAPI fixture_location(HKEY key, LPCWSTR subkey, LPCWSTR name, DWORD flags, LPDWORD type,
                                       PVOID data, LPDWORD bytes) {
    (void)type;
    if (key != HKEY_LOCAL_MACHINE || wcscmp(subkey, L"SOFTWARE\\Microsoft\\Windows Defender") ||
        wcscmp(name, L"InstallLocation") || flags != RRF_RT_REG_SZ || *bytes < sizeof(fixture_dir)) return ERROR_INVALID_DATA;
    memcpy(data, fixture_dir, sizeof(fixture_dir));
    *bytes = sizeof(fixture_dir);
    return ERROR_SUCCESS;
}

static DWORD WINAPI fixture_attributes(LPCWSTR file) {
    return wcscmp(file, fixture_exe) ? INVALID_FILE_ATTRIBUTES : FILE_ATTRIBUTE_NORMAL;
}

static BOOL WINAPI fixture_launch(SHELLEXECUTEINFOW *launch) {
    ++launches;
    CHECK(!wcscmp(launch->lpFile, fixture_exe) && !wcscmp(launch->lpDirectory, fixture_dir));
    CHECK(!wcscmp(launch->lpVerb, L"runas") && !wcscmp(launch->lpParameters, parameters));
    CHECK(launch->nShow == SW_HIDE && (launch->fMask & SEE_MASK_NOCLOSEPROCESS));
    if (process_result == ERROR_CANCELLED) { SetLastError(ERROR_CANCELLED); return FALSE; }
    wchar_t self[MAX_PATH], line[64];
    if (!GetModuleFileNameW(NULL, self, count_of(self))) return FALSE;
    swprintf(line, count_of(line), L"test_security_actions.exe --return %lu", process_result);
    STARTUPINFOW startup = {.cb=sizeof(startup)};
    PROCESS_INFORMATION child = {0};
    if (!CreateProcessW(self, line, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &startup, &child)) return FALSE;
    CloseHandle(child.hThread);
    launch->hProcess = child.hProcess;
    return TRUE;
}

#define RegGetValueW fixture_location
#define GetFileAttributesW fixture_attributes
#define ShellExecuteExW fixture_launch
#include "../src/desktop_tools/windows_panels.c"

bool w_os_run_executable(const wchar_t *exe, const wchar_t *argument, const wchar_t *directory, bool admin) {
    wchar_t system[MAX_PATH], mmc[MAX_PATH], console[MAX_PATH];
    if (!GetSystemDirectoryW(system, count_of(system))) return false;
    swprintf(mmc, count_of(mmc), L"%ls\\mmc.exe", system);
    swprintf(console, count_of(console), L"%ls\\wf.msc", system);
    return !directory && !admin && !_wcsicmp(exe, mmc) && !_wcsicmp(argument, console);
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--return")) return atoi(argv[2]);
    result.kind = quick_scan;
    parameters = L"-Scan -ScanType 1";
    w_defender_action();
    CHECK(wcsstr(result.summary, L"quick scan finished"));
    process_result = 2;
    w_defender_action();
    CHECK(wcsstr(result.summary, L"unresolved threat"));
    result.kind = signature_update;
    parameters = L"-SignatureUpdate";
    process_result = ERROR_CANCELLED;
    w_defender_action();
    CHECK(wcsstr(result.summary, L"canceled"));
    process_result = 0;
    w_defender_action();
    CHECK(wcsstr(result.summary, L"up to date") && launches == 4);
    CHECK(w_open_firewall());
    return failures != 0;
}
