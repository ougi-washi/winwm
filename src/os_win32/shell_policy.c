#include "os_win32/os_win32.h"
#include <wchar.h>

// Read the configured logon shell in precedence order. Never write shell policy.
DWORD w_os_startup_shell(void) {
    static const wchar_t logon[] = L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon";
    static const struct { HKEY hive; const wchar_t *key; } locations[] = {
        {HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System"},
        {HKEY_CURRENT_USER,logon}, {HKEY_LOCAL_MACHINE,logon}
    };
    for (size_t i=0;i<count_of(locations);++i) {
        wchar_t raw[os_path_cap], value[os_path_cap];
        DWORD type, bytes=sizeof(raw);
        LSTATUS error=RegGetValueW(locations[i].hive,locations[i].key,L"Shell",RRF_RT_ANY|RRF_NOEXPAND,
                                   &type,raw,&bytes);
        if (error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND) continue;
        if (error==ERROR_MORE_DATA) return ERROR_ALREADY_ASSIGNED;
        if (error) return error;
        if ((type!=REG_SZ && type!=REG_EXPAND_SZ) || bytes<sizeof(wchar_t) ||
            bytes!=(wcslen(raw)+1)*sizeof(wchar_t)) return ERROR_ALREADY_ASSIGNED;
        DWORD n=ExpandEnvironmentStringsW(raw,value,count_of(value));
        if (!n) return GetLastError();
        if (n>count_of(value)) return ERROR_ALREADY_ASSIGNED;
        wchar_t *start=value+wcsspn(value,L" \t"), *end=start+wcslen(start);
        while (end>start && (end[-1]==L' ' || end[-1]==L'\t')) --end;
        if (end-start>=2 && *start==L'"' && end[-1]==L'"') { ++start; --end; }
        *end=0;
        if (!_wcsicmp(start,L"explorer.exe")) return ERROR_SUCCESS;
        wchar_t explorer[MAX_PATH];
        UINT len=GetSystemWindowsDirectoryW(explorer,count_of(explorer));
        if (!len) return GetLastError();
        if (len+count_of(L"\\explorer.exe")>count_of(explorer)) return ERROR_FILENAME_EXCED_RANGE;
        wcscat(explorer,L"\\explorer.exe");
        return _wcsicmp(start,explorer) ? ERROR_ALREADY_ASSIGNED : ERROR_SUCCESS;
    }
    return ERROR_SUCCESS;
}
