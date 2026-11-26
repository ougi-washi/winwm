#include "desktop_tools/network.h"
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <devguid.h>
#include <shlwapi.h>
#include <stdio.h>
#include <wchar.h>

// Unconfigured PCI devices have no Net setup class. Their compatible IDs
// still advertise the PCI network class (02) and subclass.
static bool w_network_class(const wchar_t *ids, bool wireless) {
    const wchar_t *prefix = wireless ? L"PCI\\CC_0280" : L"PCI\\CC_02";
    for (; *ids; ids += wcslen(ids) + 1)
        if (!_wcsnicmp(ids, prefix, wcslen(prefix)))
            return true;
    return false;
}

size_t w_network_problems(w_adapter_problem *out, size_t capacity, bool wireless) {
    HDEVINFO devices = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (devices == INVALID_HANDLE_VALUE) return 0;
    size_t count = 0;
    SP_DEVINFO_DATA entry = {.cbSize = sizeof(entry)};
    for (DWORD i = 0; count < capacity && SetupDiEnumDeviceInfo(devices, i, &entry); ++i) {
        ULONG state = 0, problem = 0;
        if (CM_Get_DevNode_Status(&state, &problem, entry.DevInst, 0) != CR_SUCCESS || !(state & DN_HAS_PROBLEM)) continue;
        wchar_t ids[1024] = {0};
        bool pci = SetupDiGetDeviceRegistryPropertyW(devices, &entry, SPDRP_COMPATIBLEIDS, NULL,
                       (BYTE *)ids, sizeof(ids) - 2*sizeof(*ids), NULL) && w_network_class(ids, wireless);
        bool net = IsEqualGUID(&entry.ClassGuid, &GUID_DEVCLASS_NET);
        if (!pci && !net) continue;
        wchar_t name[256] = L"Network controller";
        if (!SetupDiGetDeviceRegistryPropertyW(devices, &entry, SPDRP_FRIENDLYNAME, NULL, (BYTE *)name, sizeof(name), NULL))
            SetupDiGetDeviceRegistryPropertyW(devices, &entry, SPDRP_DEVICEDESC, NULL, (BYTE *)name, sizeof(name), NULL);
        if (wireless && !pci && !StrStrIW(name, L"Wi-Fi") && !StrStrIW(name, L"Wireless") &&
            !StrStrIW(name, L"802.11") && !StrStrIW(name, L"WLAN")) continue;
        wchar_t instance[MAX_DEVICE_ID_LEN] = {0};
        SetupDiGetDeviceInstanceIdW(devices, &entry, instance, MAX_DEVICE_ID_LEN, NULL);
        wchar_t *bus = wcschr(instance, L'\\');
        wchar_t *serial = bus ? wcschr(bus+1, L'\\') : NULL;
        if (serial) *serial = 0;
        const wchar_t *reason = problem == CM_PROB_FAILED_INSTALL ? L"Driver missing" :
                                problem == CM_PROB_DISABLED ? L"Disabled" : L"Device problem";
        swprintf(out[count].label, 192, L"%.135ls   ·   %ls", name, reason);
        swprintf(out[count].detail, 256, L"%ls  (Code %lu)\n%.95ls",
                 problem == CM_PROB_FAILED_INSTALL ? L"Install the PC manufacturer's network driver." :
                 problem == CM_PROB_DISABLED ? L"Enable this adapter to make it available." : L"Check the device and its driver.",
                 problem, instance);
        ++count;
    }
    SetupDiDestroyDeviceInfoList(devices);
    return count;
}
