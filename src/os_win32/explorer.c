#include "os_win32/os_win32.h"
#include <wchar.h>

static bool w_named_user_object(HANDLE object, const wchar_t *expected) {
    wchar_t name[64];
    DWORD bytes;
    return GetUserObjectInformationW(object,UOI_NAME,name,sizeof(name),&bytes) && !_wcsicmp(name,expected);
}

DWORD w_os_ensure_shell(void) {
    if (GetShellWindow()) return ERROR_SUCCESS;
    if (GetSystemMetrics(SM_SHUTTINGDOWN)) return ERROR_SHUTDOWN_IN_PROGRESS;
    if (!w_named_user_object(GetProcessWindowStation(),L"WinSta0") ||
        !w_named_user_object(GetThreadDesktop(GetCurrentThreadId()),L"Default"))
        return ERROR_REQUIRES_INTERACTIVE_WINDOWSTATION;

    wchar_t directory[MAX_PATH], exe[MAX_PATH];
    UINT n = GetSystemWindowsDirectoryW(directory,count_of(directory));
    if (!n) return GetLastError();
    if (n+count_of(L"\\explorer.exe") > count_of(exe)) return ERROR_FILENAME_EXCED_RANGE;
    wcscpy(exe,directory);
    wcscat(exe,L"\\explorer.exe");
    STARTUPINFOW startup = {.cb=sizeof(startup)};
    PROCESS_INFORMATION child = {0};
    if (!CreateProcessW(exe,NULL,NULL,NULL,FALSE,0,NULL,directory,&startup,&child))
        return GetLastError();
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    // Explorer may delegate shell startup to another process. The shell window
    // identifies a usable desktop even when the initial executable has exited.
    ULONGLONG deadline = GetTickCount64()+10000;
    while (!GetShellWindow()) {
        if (GetSystemMetrics(SM_SHUTTINGDOWN)) return ERROR_SHUTDOWN_IN_PROGRESS;
        if (GetTickCount64() >= deadline) return ERROR_TIMEOUT;
        Sleep(50);
    }
    return ERROR_SUCCESS;
}
