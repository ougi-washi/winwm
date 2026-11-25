#include "desktop_tools/bluetooth.h"
#include <bluetoothapis.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <devguid.h>
#include <stdio.h>

static struct {
    __typeof__(&BluetoothFindFirstRadio) first_radio;
    __typeof__(&BluetoothFindRadioClose) close_radio;
    __typeof__(&BluetoothFindFirstDevice) first;
    __typeof__(&BluetoothFindNextDevice) next;
    __typeof__(&BluetoothFindDeviceClose) close;
    __typeof__(&BluetoothAuthenticateDeviceEx) pair;
    __typeof__(&BluetoothRemoveDevice) forget;
} api;

typedef struct {
    LONG refs;
    bool inquire;
    BLUETOOTH_DEVICE_INFO *devices;
    int count, capacity;
    wchar_t message[256];
} w_bt_result;

static HWND panel, devices_list, status_text, action_button, scan_button;
static HANDLE scan_thread;
static w_bt_result *pending, *shown;

static void w_release_result(w_bt_result *result) {
    if (result && !InterlockedDecrement(&result->refs)) {
        free(result->devices);
        free(result);
    }
}

static void w_adapter_status(wchar_t *text, size_t cap) {
    HDEVINFO devices = SetupDiGetClassDevsW(&GUID_DEVCLASS_BLUETOOTH, NULL, NULL, DIGCF_PRESENT);
    if (devices == INVALID_HANDLE_VALUE) return;
    SP_DEVINFO_DATA entry = {.cbSize = sizeof(entry)};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(devices, i, &entry); ++i) {
        ULONG state, problem;
        if (CM_Get_DevNode_Status(&state, &problem, entry.DevInst, 0) != CR_SUCCESS || !(state & DN_HAS_PROBLEM))
            continue;
        wchar_t name[160] = L"Bluetooth device";
        SetupDiGetDeviceRegistryPropertyW(devices, &entry, SPDRP_DEVICEDESC, NULL, (BYTE *)name, sizeof(name), NULL);
        swprintf(text, cap, L"%.120ls   ·   Windows Code %lu. %ls", name, problem,
                 problem == CM_PROB_DISABLED ? L"Enable the adapter to use Bluetooth." :
                 problem == CM_PROB_FAILED_INSTALL ? L"The adapter needs a driver." : L"Check the device and its driver.");
        break;
    }
    SetupDiDestroyDeviceInfoList(devices);
}

static DWORD WINAPI w_query_devices(void *arg) {
    w_bt_result *result = arg;
    HANDLE radio = NULL;
    BLUETOOTH_FIND_RADIO_PARAMS params = {.dwSize = sizeof(params)};
    HBLUETOOTH_RADIO_FIND radios = api.first_radio(&params, &radio);
    if (!radios) {
        wcscpy(result->message, L"No active Bluetooth radio. Check the adapter and its driver.");
        w_adapter_status(result->message, count_of(result->message));
    } else {
        CloseHandle(radio);
        api.close_radio(radios);
        // A null hRadio enumerates devices across all installed radios.
        BLUETOOTH_DEVICE_SEARCH_PARAMS search = {.dwSize = sizeof(search), .fReturnAuthenticated = TRUE,
            .fReturnRemembered = TRUE, .fReturnConnected = TRUE, .fReturnUnknown = result->inquire,
            .fIssueInquiry = result->inquire, .cTimeoutMultiplier = 4};
        BLUETOOTH_DEVICE_INFO info = {.dwSize = sizeof(info)};
        HBLUETOOTH_DEVICE_FIND find = api.first(&search, &info);
        DWORD error = find ? ERROR_SUCCESS : GetLastError();
        if (find) {
            do {
                if (result->count == result->capacity) {
                    int size = result->capacity ? 2 * result->capacity : 16;
                    void *memory = realloc(result->devices, size * sizeof(info));
                    if (!memory) { error = ERROR_OUTOFMEMORY; break; }
                    result->devices = memory;
                    result->capacity = size;
                }
                result->devices[result->count++] = info;
                info = (BLUETOOTH_DEVICE_INFO){.dwSize = sizeof(info)};
            } while (api.next(find, &info));
            if (error == ERROR_SUCCESS) error = GetLastError();
            api.close(find);
        }
        if (error && error != ERROR_NO_MORE_ITEMS && error != ERROR_NOT_FOUND)
            swprintf(result->message, count_of(result->message), L"Bluetooth search returned Windows error %lu.", error);
        else if (result->inquire)
            wcscpy(result->message, result->count ? L"Select a device to pair or manage it." :
                   L"No nearby devices found. Make the device discoverable, then scan again.");
        else wcscpy(result->message, L"Scan for nearby devices. Use More devices for Bluetooth LE.");
    }
    w_release_result(result);
    return 0;
}

void w_bluetooth_open(HWND window, HWND list, HWND status, HWND action, HWND scan) {
    panel = window;
    devices_list = list;
    status_text = status;
    action_button = action;
    scan_button = scan;
    // These optional system components may be absent on minimal Windows installations.
    HMODULE dll = LoadLibraryExW(L"bthprops.cpl", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (dll) {
#define BT_PROC(field, name) api.field = (__typeof__(api.field))(void *)GetProcAddress(dll, #name)
        BT_PROC(first_radio, BluetoothFindFirstRadio);
        BT_PROC(close_radio, BluetoothFindRadioClose);
        BT_PROC(first, BluetoothFindFirstDevice);
        BT_PROC(next, BluetoothFindNextDevice);
        BT_PROC(close, BluetoothFindDeviceClose);
        BT_PROC(pair, BluetoothAuthenticateDeviceEx);
        BT_PROC(forget, BluetoothRemoveDevice);
#undef BT_PROC
    }
    w_bluetooth_scan(false);
}

void w_bluetooth_scan(bool inquire) {
    if (scan_thread) return;
    if (!api.first_radio || !api.close_radio || !api.first || !api.next || !api.close) {
        SetWindowTextW(status_text, L"Windows Bluetooth discovery is unavailable.");
        EnableWindow(scan_button, FALSE);
        return;
    }
    pending = calloc(1, sizeof(*pending));
    if (pending) {
        pending->refs = 2;
        pending->inquire = inquire;
        scan_thread = CreateThread(NULL, 0, w_query_devices, pending, 0, NULL);
    }
    if (!scan_thread) {
        free(pending);
        pending = NULL;
        SetWindowTextW(status_text, L"Bluetooth search could not start.");
        return;
    }
    EnableWindow(scan_button, FALSE);
    EnableWindow(action_button, FALSE);
    SetWindowTextW(status_text, inquire ? L"Searching for nearby Bluetooth devices…" : L"Reading Bluetooth devices…");
}

HANDLE w_bluetooth_pending(void) { return scan_thread; }

static BLUETOOTH_DEVICE_INFO *w_selected_device(void) {
    LRESULT row = SendMessageW(devices_list, LB_GETCURSEL, 0, 0);
    LRESULT id = row < 0 ? -1 : SendMessageW(devices_list, LB_GETITEMDATA, row, 0);
    return shown && id >= 0 && id < shown->count ? &shown->devices[id] : NULL;
}

static bool w_paired(const BLUETOOTH_DEVICE_INFO *d) { return d->fAuthenticated || d->fRemembered; }

void w_bluetooth_selected(void) {
    BLUETOOTH_DEVICE_INFO *device = w_selected_device();
    bool paired = device && w_paired(device);
    SetWindowTextW(action_button, paired ? L"Forget device…" : L"Pair device");
    EnableWindow(action_button, !scan_thread && device && (paired ? api.forget != NULL : api.pair != NULL));
}

void w_bluetooth_finish(void) {
    BLUETOOTH_DEVICE_INFO *old = w_selected_device();
    ULONGLONG address = old ? old->Address.ullLong : 0;
    CloseHandle(scan_thread);
    scan_thread = NULL;
    w_release_result(shown);
    shown = pending;
    pending = NULL;
    SendMessageW(devices_list, WM_SETREDRAW, FALSE, 0);
    SendMessageW(devices_list, LB_RESETCONTENT, 0, 0);
    LRESULT selected = -1;
    for (int i = 0; i < shown->count; ++i) {
        BLUETOOTH_DEVICE_INFO *d = &shown->devices[i];
        wchar_t label[384];
        swprintf(label, count_of(label), L"%.220ls   ·   %ls   ·   %012llX",
                 *d->szName ? d->szName : L"Bluetooth device", d->fConnected ? L"Connected" :
                 w_paired(d) ? L"Paired" : L"Available", (unsigned long long)d->Address.ullLong);
        LRESULT row = SendMessageW(devices_list, LB_ADDSTRING, 0, (LPARAM)label);
        if (row >= 0) {
            SendMessageW(devices_list, LB_SETITEMDATA, row, i);
            if (d->Address.ullLong == address || (!address && selected < 0)) selected = row;
        }
    }
    SendMessageW(devices_list, LB_SETCURSEL, selected, 0);
    SendMessageW(devices_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(devices_list, NULL, TRUE);
    SetWindowTextW(status_text, shown->message);
    EnableWindow(scan_button, TRUE);
    w_bluetooth_selected();
}

void w_bluetooth_apply(void) {
    BLUETOOTH_DEVICE_INFO *selected = w_selected_device();
    if (scan_thread || !selected) return;
    BLUETOOTH_DEVICE_INFO device = *selected;
    DWORD error;
    if (w_paired(&device)) {
        if (!api.forget) return;
        wchar_t question[384];
        swprintf(question, count_of(question), L"Forget %.200ls (%012llX)? You will need to pair it again.",
                 *device.szName ? device.szName : L"this device", (unsigned long long)device.Address.ullLong);
        if (MessageBoxW(panel, question, L"Forget Bluetooth device", MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION) != IDYES)
            return;
        error = api.forget(&device.Address);
    } else {
        if (!api.pair) return;
        error = api.pair(panel, NULL, &device, NULL, MITMProtectionNotDefined);
    }
    if (error == ERROR_SUCCESS || error == ERROR_NO_MORE_ITEMS) w_bluetooth_scan(false);
    else if (error != ERROR_CANCELLED) {
        wchar_t text[128];
        swprintf(text, count_of(text), L"Windows could not complete the device request (error %lu).", error);
        SetWindowTextW(status_text, text);
    }
}

void w_bluetooth_close(void) {
    if (scan_thread) CloseHandle(scan_thread);
    w_release_result(pending);
    w_release_result(shown);
}
