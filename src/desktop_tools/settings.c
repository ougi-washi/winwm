#define COBJMACROS
#include "theme/theme.h"
#include "desktop_tools/tools.h"
#include <initguid.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wlanapi.h>
#include <bluetoothapis.h>
#include <commdlg.h>
#include <stdio.h>
#include <wchar.h>
enum page { page_audio, page_wifi, page_bluetooth, page_display };

////////////////
//~ Settings

static enum page page;
static HWND window, list, apply, lower, raise_button, status;
static IMMDevice *endpoints[64];
static int endpoint_count;
static HANDLE wlan;
static struct w_wifi_profile {
    GUID adapter;
    wchar_t profile[WLAN_MAX_NAME_LENGTH];
} profiles[256];
static int profile_count;
static struct w_display_mode {
    wchar_t device[32];
    DEVMODEW mode;
} modes[1024];
static int mode_count;
static DEVMODEW old_mode;
static wchar_t preview_device[32];
static ULONGLONG preview_deadline;

////////////////
//~ Settings Functions

static void w_message(const wchar_t *s) {
    SetWindowTextW(status, s);
}

static void w_item(const wchar_t *s, int id) {
    LRESULT n = SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)s);
    if (n >= 0)
        SendMessageW(list, LB_SETITEMDATA, n, id);
}

static void w_audio_items(void) {
    IMMDeviceEnumerator *en = NULL;
    IMMDeviceCollection *all = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_INPROC_SERVER,
                                  &IID_IMMDeviceEnumerator, (void **)&en);
    if (SUCCEEDED(hr))
        hr = IMMDeviceEnumerator_EnumAudioEndpoints(en, eAll, DEVICE_STATE_ACTIVE, &all);
    if (SUCCEEDED(hr)) {
        UINT count = 0;
        IMMDeviceCollection_GetCount(all, &count);
        for (UINT i = 0; i < count && endpoint_count < 64; i++) {
            IMMDevice *device = NULL;
            if (FAILED(IMMDeviceCollection_Item(all, i, &device)))
                continue;
            IPropertyStore *props = NULL;
            PROPVARIANT name;
            PropVariantInit(&name);
            if (SUCCEEDED(IMMDevice_OpenPropertyStore(device, STGM_READ, &props))) {
                IPropertyStore_GetValue(props, &PKEY_Device_FriendlyName, &name);
                IPropertyStore_Release(props);
            }
            w_item(name.vt == VT_LPWSTR ? name.pwszVal : L"Audio endpoint", endpoint_count);
            endpoints[endpoint_count++] = device;
            PropVariantClear(&name);
        }
    }
    if (all)
        IMMDeviceCollection_Release(all);
    if (en)
        IMMDeviceEnumerator_Release(en);
    w_message(endpoint_count ? L"Select an endpoint to adjust its volume or mute state."
                           : L"No active audio endpoints.");
}

static void w_audio_change(int id, int delta) {
    IAudioEndpointVolume *v = NULL;
    if (id < 0 || id >= endpoint_count ||
        FAILED(IMMDevice_Activate(endpoints[id], &IID_IAudioEndpointVolume, CLSCTX_INPROC_SERVER, NULL,
                                  (void **)&v)))
        return;
    float level = 0;
    BOOL mute = FALSE;
    HRESULT hr;
    if (delta) {
        hr = IAudioEndpointVolume_GetMasterVolumeLevelScalar(v, &level);
        if (SUCCEEDED(hr))
            hr = IAudioEndpointVolume_SetMasterVolumeLevelScalar(
                v, w_clamp_int((int)(level * 100 + 0.5f) + delta, 0, 100) / 100.0f, NULL);
    } else {
        hr = IAudioEndpointVolume_GetMute(v, &mute);
        if (SUCCEEDED(hr))
            hr = IAudioEndpointVolume_SetMute(v, !mute, NULL);
    }
    if (SUCCEEDED(hr) && SUCCEEDED(IAudioEndpointVolume_GetMasterVolumeLevelScalar(v, &level)) &&
        SUCCEEDED(IAudioEndpointVolume_GetMute(v, &mute))) {
        wchar_t s[100];
        swprintf(s, count_of(s), L"Volume %d%%%ls", (int)(level * 100 + 0.5f), mute ? L"   Muted" : L"");
        w_message(s);
    } else
        w_message(L"Audio endpoint could not apply the change.");
    IAudioEndpointVolume_Release(v);
}

static struct w_wifi_api {
    DWORD(WINAPI *open)(DWORD, PVOID, PDWORD, PHANDLE);
    DWORD(WINAPI *close)(HANDLE, PVOID);
    DWORD(WINAPI *interfaces)(HANDLE, PVOID, PWLAN_INTERFACE_INFO_LIST *);
    DWORD(WINAPI *profiles)(HANDLE, const GUID *, PVOID, PWLAN_PROFILE_INFO_LIST *);
    DWORD(WINAPI *connect)(HANDLE, const GUID *, const WLAN_CONNECTION_PARAMETERS *, PVOID);
    void(WINAPI *free_memory)(PVOID);
} wifi;

////////////////
//~ Wireless networks

static bool w_load_wifi(void) {
    HMODULE lib = LoadLibraryExW(L"wlanapi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!lib)
        return false;
    wifi.open = (DWORD(WINAPI *)(DWORD, PVOID, PDWORD, PHANDLE))GetProcAddress(lib, "WlanOpenHandle");
    wifi.close = (DWORD(WINAPI *)(HANDLE, PVOID))GetProcAddress(lib, "WlanCloseHandle");
    wifi.interfaces = (DWORD(WINAPI *)(HANDLE, PVOID, PWLAN_INTERFACE_INFO_LIST *))GetProcAddress(
        lib, "WlanEnumInterfaces");
    wifi.profiles = (DWORD(WINAPI *)(HANDLE, const GUID *, PVOID, PWLAN_PROFILE_INFO_LIST *))GetProcAddress(
        lib, "WlanGetProfileList");
    wifi.connect = (DWORD(WINAPI *)(HANDLE, const GUID *, const WLAN_CONNECTION_PARAMETERS *,
                                    PVOID))GetProcAddress(lib, "WlanConnect");
    wifi.free_memory = (void(WINAPI *)(PVOID))GetProcAddress(lib, "WlanFreeMemory");
    return wifi.open && wifi.close && wifi.interfaces && wifi.profiles && wifi.connect && wifi.free_memory;
}

static void w_wifi_items(void) {
    DWORD version = 0;
    if (!w_load_wifi() || wifi.open(2, NULL, &version, &wlan) != ERROR_SUCCESS) {
        w_message(L"Wi-Fi service unavailable.");
        return;
    }
    WLAN_INTERFACE_INFO_LIST *interfaces = NULL;
    if (wifi.interfaces(wlan, NULL, &interfaces) != ERROR_SUCCESS)
        return;
    for (DWORD i = 0; i < interfaces->dwNumberOfItems; i++) {
        WLAN_INTERFACE_INFO *adapter = &interfaces->InterfaceInfo[i];
        WLAN_PROFILE_INFO_LIST *saved = NULL;
        if (wifi.profiles(wlan, &adapter->InterfaceGuid, NULL, &saved) != ERROR_SUCCESS)
            continue;
        for (DWORD j = 0; j < saved->dwNumberOfItems && profile_count < 256; j++) {
            profiles[profile_count].adapter = adapter->InterfaceGuid;
            wcscpy(profiles[profile_count].profile, saved->ProfileInfo[j].strProfileName);
            w_item(profiles[profile_count].profile, profile_count);
            ++profile_count;
        }
        wifi.free_memory(saved);
    }
    wifi.free_memory(interfaces);
    w_message(profile_count ? L"Select a saved Wi-Fi profile." : L"No saved Wi-Fi profiles.");
}

static void w_bluetooth_items(void) {
    HMODULE lib = LoadLibraryExW(L"bthprops.cpl", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!lib) {
        w_message(L"Bluetooth unavailable.");
        return;
    }
    typedef HBLUETOOTH_DEVICE_FIND(WINAPI * find_first_fn)(const BLUETOOTH_DEVICE_SEARCH_PARAMS *,
                                                           BLUETOOTH_DEVICE_INFO *);
    typedef BOOL(WINAPI * find_next_fn)(HBLUETOOTH_DEVICE_FIND, BLUETOOTH_DEVICE_INFO *);
    typedef BOOL(WINAPI * find_close_fn)(HBLUETOOTH_DEVICE_FIND);
    find_first_fn first = (find_first_fn)GetProcAddress(lib, "BluetoothFindFirstDevice");
    find_next_fn next = (find_next_fn)GetProcAddress(lib, "BluetoothFindNextDevice");
    find_close_fn close_find = (find_close_fn)GetProcAddress(lib, "BluetoothFindDeviceClose");
    if (!first || !next || !close_find) {
        w_message(L"Bluetooth inventory unavailable.");
        return;
    }
    BLUETOOTH_DEVICE_SEARCH_PARAMS search = {.dwSize = sizeof(search),
                                             .fReturnAuthenticated = TRUE,
                                             .fReturnRemembered = TRUE,
                                             .fReturnConnected = TRUE};
    BLUETOOTH_DEVICE_INFO info = {.dwSize = sizeof(info)};
    HBLUETOOTH_DEVICE_FIND find = first(&search, &info);
    if (find) {
        do {
            wchar_t s[320];
            swprintf(s, count_of(s), L"%ls   %ls", info.szName, info.fConnected ? L"Connected" : L"Paired");
            w_item(s, 0);
        } while (next(find, &info));
        close_find(find);
    }
    w_message(find ? L"Known devices on local Bluetooth radios." : L"No paired Bluetooth devices found.");
}

////////////////
//~ Display settings

static void w_display_items(void) {
    DISPLAY_DEVICEW d = {.cb = sizeof(d)};
    for (DWORD i = 0; EnumDisplayDevicesW(NULL, i, &d, 0) && mode_count < 1024; i++) {
        if (!(d.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP))
            continue;
        DEVMODEW current = {.dmSize = sizeof(current)};
        if (!EnumDisplaySettingsW(d.DeviceName, ENUM_CURRENT_SETTINGS, &current))
            continue;
        DEVMODEW m = {.dmSize = sizeof(m)};
        for (DWORD j = 0; EnumDisplaySettingsW(d.DeviceName, j, &m) && mode_count < 1024; j++) {
            if (m.dmBitsPerPel != current.dmBitsPerPel ||
                m.dmDisplayOrientation != current.dmDisplayOrientation)
                continue;
            wchar_t s[160];
            swprintf(s, count_of(s), L"%ls   %lu × %lu   %lu Hz", d.DeviceName, m.dmPelsWidth, m.dmPelsHeight,
                     m.dmDisplayFrequency);
            wcscpy(modes[mode_count].device, d.DeviceName);
            modes[mode_count].mode = m;
            w_item(s, mode_count++);
        }
    }
    w_message(L"Preview a mode, then keep it within 12 seconds.");
}

static void w_revert(void) {
    if (!preview_deadline)
        return;
    ChangeDisplaySettingsExW(preview_device, &old_mode, NULL, 0, NULL);
    preview_deadline = 0;
    KillTimer(window, 1);
    SetWindowTextW(apply, L"Preview");
    w_message(L"Original mode restored.");
}

static void w_use_selection(int delta) {
    LRESULT row = SendMessageW(list, LB_GETCURSEL, 0, 0);
    if (row == LB_ERR)
        return;
    int id = (int)SendMessageW(list, LB_GETITEMDATA, row, 0);
    if (page == page_audio)
        w_audio_change(id, delta);
    else if (page == page_wifi && id >= 0 && id < profile_count) {
        WLAN_CONNECTION_PARAMETERS params = {.wlanConnectionMode = wlan_connection_mode_profile,
                                             .strProfile = profiles[id].profile,
                                             .dot11BssType = dot11_BSS_type_any};
        w_message(wifi.connect(wlan, &profiles[id].adapter, &params, NULL) == ERROR_SUCCESS
                    ? L"Connection requested."
                    : L"Connection request failed.");
    } else if (page == page_display && id >= 0 && id < mode_count) {
        if (preview_deadline) {
            preview_deadline = 0;
            KillTimer(window, 1);
            SetWindowTextW(apply, L"Preview");
            w_message(L"Display mode kept for this session.");
            return;
        }
        wcscpy(preview_device, modes[id].device);
        old_mode = (DEVMODEW){.dmSize = sizeof(old_mode)};
        if (!EnumDisplaySettingsW(preview_device, ENUM_CURRENT_SETTINGS, &old_mode))
            return;
        DEVMODEW *m = &modes[id].mode;
        if (ChangeDisplaySettingsExW(preview_device, m, NULL, CDS_TEST, NULL) != DISP_CHANGE_SUCCESSFUL ||
            ChangeDisplaySettingsExW(preview_device, m, NULL, 0, NULL) != DISP_CHANGE_SUCCESSFUL) {
            w_message(L"Display rejected the requested mode.");
            return;
        }
        preview_deadline = GetTickCount64() + 12000;
        SetWindowTextW(apply, L"Keep");
        w_message(L"Keep this mode within 12 seconds.");
        SetTimer(window, 1, 12000, NULL);
    }
}

static LRESULT CALLBACK w_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    LRESULT color_result;
    if (w_theme_message(w, msg, wp, lp, &color_result))
        return color_result;
    if (msg == WM_COMMAND) {
        if (LOWORD(wp) == 101 || ((HWND)lp == list && HIWORD(wp) == LBN_DBLCLK))
            w_use_selection(0);
        if (LOWORD(wp) == 102)
            w_use_selection(-5);
        if (LOWORD(wp) == 103)
            w_use_selection(5);
        return 0;
    }
    if (msg == WM_TIMER) {
        w_revert();
        return 0;
    }
    if (msg == WM_DESTROY) {
        w_revert();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

int w_settings_run(const wchar_t *name) {
    if (!wcscmp(name, L"wallpaper")) {
        wchar_t path[os_path_cap] = {0};
        OPENFILENAMEW picker = {.lStructSize = sizeof(picker),
                                .lpstrFilter = L"Images\0*.bmp;*.jpg;*.jpeg;*.png\0\0",
                                .lpstrFile = path,
                                .nMaxFile = count_of(path),
                                .Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR};
        if (!GetOpenFileNameW(&picker))
            return 0;
        return !SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, path, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
    }
    page = !wcscmp(name, L"wifi")        ? page_wifi
           : !wcscmp(name, L"display")   ? page_display
           : !wcscmp(name, L"bluetooth") ? page_bluetooth
                                         : page_audio;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSW wc = {.lpfnWndProc = w_proc,
                    .hInstance = inst,
                    .lpszClassName = L"WinwmSettings",
                    .hCursor = LoadCursorW(NULL, IDC_ARROW),
                    .hbrBackground = NULL};
    const wchar_t *titles[] = {L"Audio endpoints", L"Wi-Fi profiles", L"Bluetooth devices", L"Display modes"};
    RegisterClassW(&wc);
    window = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, titles[page], WS_CAPTION | WS_SYSMENU,
                           CW_USEDEFAULT, 0, 560, 390, NULL, NULL, inst, NULL);
    if (!window)
        return 1;
    list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY, 12, 10, 520, 238,
                           window, NULL, inst, NULL);
    status = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 12, 258, 520, 34, window, NULL, inst, NULL);
    const wchar_t *labels[] = {L"Mute / unmute", L"Connect", L"", L"Preview"};
    apply = CreateWindowW(L"BUTTON", labels[page],
                          BS_OWNERDRAW | WS_CHILD | WS_TABSTOP | (page == page_bluetooth ? 0 : WS_VISIBLE),
                          12, 300, 140, 30, window, (HMENU)101, inst, NULL);
    if (page == page_audio) {
        lower = CreateWindowW(L"BUTTON", L"Volume −", BS_OWNERDRAW | WS_CHILD | WS_VISIBLE | WS_TABSTOP, 160,
                              300, 110, 30, window, (HMENU)102, inst, NULL);
        raise_button =
            CreateWindowW(L"BUTTON", L"Volume +", BS_OWNERDRAW | WS_CHILD | WS_VISIBLE | WS_TABSTOP, 280, 300,
                          110, 30, window, (HMENU)103, inst, NULL);
    }
    HWND controls[] = {list, status, apply, lower, raise_button};
    for (size_t i = 0; i < count_of(controls); i++)
        if (controls[i])
            SendMessageW(controls[i], WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    if (page == page_audio)
        w_audio_items();
    else if (page == page_wifi)
        w_wifi_items();
    else if (page == page_bluetooth)
        w_bluetooth_items();
    else
        w_display_items();
    SendMessageW(list, LB_SETCURSEL, 0, 0);
    SetFocus(list);
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE) {
            DestroyWindow(window);
            continue;
        }
        if (m.message == WM_KEYDOWN && m.wParam == VK_RETURN && m.hwnd == list) {
            w_use_selection(0);
            continue;
        }
        if (!IsDialogMessageW(window, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    for (int i = 0; i < endpoint_count; i++)
        IMMDevice_Release(endpoints[i]);
    if (wlan)
        wifi.close(wlan, NULL);
    CoUninitialize();
    return 0;
}
