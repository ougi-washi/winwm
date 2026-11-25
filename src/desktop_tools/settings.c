#define COBJMACROS
#include "theme/theme.h"
#include "desktop_tools/tools.h"
#include <initguid.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wlanapi.h>
#include <bluetoothapis.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <powrprof.h>
#include <commctrl.h>
#include <commdlg.h>
#include <stdio.h>
#include <wchar.h>
enum page { page_audio, page_wifi, page_bluetooth, page_display, page_power, page_network };

////////////////
//~ Settings

static enum page page;
static HWND window, list, apply, lower, raise_button, status;
static HWND password;
static BLUETOOTH_DEVICE_INFO paired[256];
static int paired_count;
static HMODULE bluetooth;
static GUID plans[128];
static int plan_count;
static wchar_t adapter_details[128][256];
static int adapter_count;
static IMMDevice *endpoints[64];
static int endpoint_count;
static HANDLE wlan;
static struct w_wifi_profile {
    GUID adapter;
    wchar_t profile[WLAN_MAX_NAME_LENGTH];
    WLAN_AVAILABLE_NETWORK network;
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
    DWORD(WINAPI *available)(HANDLE, const GUID *, DWORD, PVOID, PWLAN_AVAILABLE_NETWORK_LIST *);
    DWORD(WINAPI *scan)(HANDLE, const GUID *, const DOT11_SSID *, const WLAN_RAW_DATA *, PVOID);
    DWORD(WINAPI *disconnect)(HANDLE, const GUID *, PVOID);
    DWORD(WINAPI *notify)(HANDLE, DWORD, BOOL, WLAN_NOTIFICATION_CALLBACK, PVOID, PVOID, PDWORD);
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
    wifi.available = (__typeof__(wifi.available))GetProcAddress(lib, "WlanGetAvailableNetworkList");
    wifi.scan = (__typeof__(wifi.scan))GetProcAddress(lib, "WlanScan");
    wifi.disconnect = (__typeof__(wifi.disconnect))GetProcAddress(lib, "WlanDisconnect");
    wifi.notify = (__typeof__(wifi.notify))GetProcAddress(lib, "WlanRegisterNotification");
    wifi.connect = (DWORD(WINAPI *)(HANDLE, const GUID *, const WLAN_CONNECTION_PARAMETERS *,
                                    PVOID))GetProcAddress(lib, "WlanConnect");
    wifi.free_memory = (void(WINAPI *)(PVOID))GetProcAddress(lib, "WlanFreeMemory");
    return wifi.open && wifi.close && wifi.interfaces && wifi.profiles && wifi.connect && wifi.free_memory;
}

enum { wifi_result = WM_APP + 1, wifi_scan_done };

static void WINAPI w_wireless_event(WLAN_NOTIFICATION_DATA *event, void *context) {
    if (event->NotificationSource != WLAN_NOTIFICATION_SOURCE_ACM)
        return;
    if (event->NotificationCode == wlan_notification_acm_scan_complete)
        PostMessageW(context, wifi_scan_done, 0, 0);
    if (event->NotificationCode == wlan_notification_acm_connection_complete ||
        event->NotificationCode == wlan_notification_acm_connection_attempt_fail) {
        WLAN_CONNECTION_NOTIFICATION_DATA *data = event->pData;
        if (data && event->dwDataSize >= offsetof(WLAN_CONNECTION_NOTIFICATION_DATA, strProfileXml))
            PostMessageW(context, wifi_result, data->wlanReasonCode, 0);
    }
}

static void w_wifi_items(bool scan) {
    DWORD version = 0;
    if (!wlan) {
        if ((!wifi.open && !w_load_wifi()) || !wifi.close || !wifi.interfaces || !wifi.profiles || !wifi.connect ||
            !wifi.free_memory || wifi.open(2, NULL, &version, &wlan) != ERROR_SUCCESS) {
            w_message(L"Wi-Fi service unavailable.");
            return;
        }
        if (wifi.notify)
            wifi.notify(wlan, WLAN_NOTIFICATION_SOURCE_ACM, TRUE, w_wireless_event, window, NULL, NULL);
    }
    LRESULT row = SendMessageW(list, LB_GETCURSEL, 0, 0);
    int selected = row == LB_ERR ? -1 : (int)SendMessageW(list, LB_GETITEMDATA, row, 0);
    bool had_selection = selected >= 0 && selected < profile_count;
    struct w_wifi_profile previous = {0};
    if (had_selection)
        previous = profiles[selected];
    profile_count = 0;
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    SetWindowTextW(password, L"");
    ShowWindow(password, SW_HIDE);
    WLAN_INTERFACE_INFO_LIST *interfaces = NULL;
    if (wifi.interfaces(wlan, NULL, &interfaces) != ERROR_SUCCESS) {
        w_message(L"Windows could not list Wi-Fi adapters.");
        return;
    }
    bool restricted = false;
    for (DWORD i = 0; i < interfaces->dwNumberOfItems; i++) {
        WLAN_INTERFACE_INFO *adapter = &interfaces->InterfaceInfo[i];
        if (scan && wifi.scan)
            wifi.scan(wlan, &adapter->InterfaceGuid, NULL, NULL, NULL);
        WLAN_AVAILABLE_NETWORK_LIST *nearby = NULL;
        DWORD result = wifi.available ? wifi.available(wlan, &adapter->InterfaceGuid, 0, NULL, &nearby) : ERROR_NOT_SUPPORTED;
        if (result == ERROR_SUCCESS) {
            for (DWORD j = 0; j < nearby->dwNumberOfItems && profile_count < 256; j++) {
                WLAN_AVAILABLE_NETWORK *n = &nearby->Network[j];
                if (!n->dot11Ssid.uSSIDLength || n->dot11Ssid.uSSIDLength > DOT11_SSID_MAX_LENGTH)
                    continue;
                struct w_wifi_profile *p = profiles + profile_count;
                *p = (struct w_wifi_profile){.adapter = adapter->InterfaceGuid, .network = *n};
                wcscpy(p->profile, n->strProfileName);
                wchar_t ssid[33] = {0}, label[440];
                MultiByteToWideChar(CP_UTF8, 0, (LPCSTR)n->dot11Ssid.ucSSID, n->dot11Ssid.uSSIDLength, ssid, 32);
                swprintf(label, count_of(label), L"%ls   %lu%%   %ls%ls", *ssid ? ssid : L"Network", n->wlanSignalQuality,
                         n->dwFlags & WLAN_AVAILABLE_NETWORK_CONNECTED ? L"Connected" : *p->profile ? L"Saved" :
                         n->bSecurityEnabled ? L"Secured" : L"Open", n->bNetworkConnectable ? L"" : L"  (unavailable)");
                w_item(label, profile_count++);
            }
            wifi.free_memory(nearby);
            continue;
        }
        restricted |= result == ERROR_ACCESS_DENIED;
        WLAN_PROFILE_INFO_LIST *saved = NULL;
        if (wifi.profiles(wlan, &adapter->InterfaceGuid, NULL, &saved) != ERROR_SUCCESS)
            continue;
        for (DWORD j = 0; j < saved->dwNumberOfItems && profile_count < 256; j++) {
            profiles[profile_count] = (struct w_wifi_profile){.adapter = adapter->InterfaceGuid};
            wcscpy(profiles[profile_count].profile, saved->ProfileInfo[j].strProfileName);
            w_item(profiles[profile_count].profile, profile_count);
            ++profile_count;
        }
        wifi.free_memory(saved);
    }
    wifi.free_memory(interfaces);
    if (had_selection) {
        for (int i = 0; i < profile_count; i++) {
            const struct w_wifi_profile *p = &profiles[i];
            if (IsEqualGUID(&previous.adapter, &p->adapter) && !wcscmp(previous.profile, p->profile) &&
                previous.network.dot11Ssid.uSSIDLength == p->network.dot11Ssid.uSSIDLength &&
                !memcmp(previous.network.dot11Ssid.ucSSID, p->network.dot11Ssid.ucSSID, p->network.dot11Ssid.uSSIDLength) &&
                previous.network.dot11DefaultAuthAlgorithm == p->network.dot11DefaultAuthAlgorithm) {
                SendMessageW(list, LB_SETCURSEL, i, 0);
                break;
            }
        }
    }
    w_message(restricted ? L"Windows restricted nearby networks. Saved profiles are shown."
              : profile_count ? L"Select a network." : L"No Wi-Fi networks available.");
}

static const wchar_t *w_personal_auth(const WLAN_AVAILABLE_NETWORK *n) {
    switch (n->dot11DefaultAuthAlgorithm) {
    case DOT11_AUTH_ALGO_WPA_PSK: return L"WPAPSK";
    case DOT11_AUTH_ALGO_RSNA_PSK: return L"WPA2PSK";
    case DOT11_AUTH_ALGO_WPA3_SAE: return L"WPA3SAE";
    default: return NULL;
    }
}

static void w_join(struct w_wifi_profile *p) {
    WLAN_AVAILABLE_NETWORK *n = &p->network;
    WLAN_CONNECTION_PARAMETERS params = {.wlanConnectionMode = wlan_connection_mode_profile,
                                         .dot11BssType = dot11_BSS_type_any, .strProfile = p->profile};
    wchar_t xml[1800] = {0};
    if (!*p->profile && !n->bSecurityEnabled) {
        params.wlanConnectionMode = wlan_connection_mode_discovery_unsecure;
        params.strProfile = NULL;
        params.pDot11Ssid = &n->dot11Ssid;
    } else if (!*p->profile) {
        const wchar_t *auth = w_personal_auth(n);
        const wchar_t *cipher = n->dot11DefaultCipherAlgorithm == DOT11_CIPHER_ALGO_CCMP ? L"AES" :
                                n->dot11DefaultCipherAlgorithm == DOT11_CIPHER_ALGO_TKIP ? L"TKIP" : NULL;
        if (!auth || !cipher) {
            w_message(L"This network requires a provisioned Windows Wi-Fi profile.");
            return;
        }
        wchar_t key[66], escaped[400], hex[65] = {0};
        int length = GetWindowTextW(password, key, count_of(key));
        bool raw = length == 64 && wcsspn(key, L"0123456789abcdefABCDEF") == 64 && wcscmp(auth, L"WPA3SAE");
        if (!raw && (length < 8 || length > 63)) {
            SecureZeroMemory(key, sizeof(key));
            w_message(L"Enter the Wi-Fi password, then choose Connect.");
            SetFocus(password);
            return;
        }
        wchar_t *out = escaped;
        for (int i = 0; i < length; i++) {
            const wchar_t *entity = key[i] == L'&' ? L"&amp;" : key[i] == L'<' ? L"&lt;" : key[i] == L'>' ? L"&gt;" : NULL;
            if (entity) { wcscpy(out, entity); out += wcslen(entity); }
            else *out++ = key[i];
        }
        *out = 0;
        SecureZeroMemory(key, sizeof(key));
        for (ULONG i = 0; i < n->dot11Ssid.uSSIDLength; i++)
            swprintf(hex + i * 2, 3, L"%02X", n->dot11Ssid.ucSSID[i]);
        swprintf(xml, count_of(xml), L"<WLANProfile xmlns=\"http://www.microsoft.com/networking/WLAN/profile/v1\">"
            L"<name>%ls</name><SSIDConfig><SSID><hex>%ls</hex></SSID></SSIDConfig><connectionType>ESS</connectionType>"
            L"<connectionMode>manual</connectionMode><MSM><security><authEncryption><authentication>%ls</authentication>"
            L"<encryption>%ls</encryption><useOneX>false</useOneX></authEncryption><sharedKey><keyType>%ls</keyType>"
            L"<protected>false</protected><keyMaterial>%ls</keyMaterial></sharedKey></security></MSM></WLANProfile>",
            hex, hex, auth, cipher, raw ? L"networkKey" : L"passPhrase", escaped);
        SecureZeroMemory(escaped, sizeof(escaped));
        params.strProfile = xml;
        params.wlanConnectionMode = wlan_connection_mode_temporary_profile;
    }
    DWORD error = wifi.connect(wlan, &p->adapter, &params, NULL);
    SecureZeroMemory(xml, sizeof(xml));
    SetWindowTextW(password, L"");
    w_message(error == ERROR_SUCCESS ? L"Connection requested." : L"Windows rejected the connection request.");
}

static void w_bluetooth_items(void) {
    if (!bluetooth)
        bluetooth = LoadLibraryExW(L"bthprops.cpl", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE lib = bluetooth;
    paired_count = 0;
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
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
            paired[paired_count] = info;
            w_item(s, paired_count++);
        } while (paired_count < (int)count_of(paired) && next(find, &info));
        close_find(find);
    }
    w_message(find ? L"Known devices on local Bluetooth radios." : L"No paired Bluetooth devices found.");
}

static void w_power_items(void) {
    GUID *active = NULL;
    PowerGetActiveScheme(NULL, &active);
    int selected = -1;
    for (DWORD i = 0; i < count_of(plans); i++) {
        DWORD size = sizeof(GUID);
        if (PowerEnumerate(NULL, NULL, NULL, ACCESS_SCHEME, i, (BYTE *)&plans[plan_count], &size) != ERROR_SUCCESS)
            break;
        wchar_t label[192];
        size = sizeof(label);
        if (PowerReadFriendlyName(NULL, &plans[plan_count], NULL, NULL, (BYTE *)label, &size) == ERROR_SUCCESS) {
            if (active && IsEqualGUID(active, &plans[plan_count]))
                selected = plan_count;
            w_item(label, plan_count++);
        }
    }
    LocalFree(active);
    SendMessageW(list, LB_SETCURSEL, selected, 0);
    w_message(plan_count ? L"Select a power plan to activate it." : L"No power plans available.");
}

static void w_network_items(void) {
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    adapter_count = 0;
    ULONG bytes = 0;
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    GetAdaptersAddresses(AF_UNSPEC, flags, NULL, NULL, &bytes);
    IP_ADAPTER_ADDRESSES *all = bytes ? malloc(bytes) : NULL;
    if (all && GetAdaptersAddresses(AF_UNSPEC, flags, NULL, all, &bytes) == NO_ERROR) {
        for (IP_ADAPTER_ADDRESSES *a = all; a && adapter_count < (int)count_of(adapter_details); a = a->Next) {
            if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || a->IfType == IF_TYPE_TUNNEL)
                continue;
            wchar_t label[320], address[64] = L"No assigned address";
            for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u; u = u->Next) {
                SOCKADDR *sock = u->Address.lpSockaddr;
                if (!sock) continue;
                if (sock->sa_family == AF_INET) {
                    InetNtopW(AF_INET, &((SOCKADDR_IN *)sock)->sin_addr, address, count_of(address));
                    break;
                }
                if (sock->sa_family == AF_INET6)
                    InetNtopW(AF_INET6, &((SOCKADDR_IN6 *)sock)->sin6_addr, address, count_of(address));
            }
            swprintf(label, count_of(label), L"%.240ls   \u00b7   %ls", a->FriendlyName,
                     a->OperStatus == IfOperStatusUp ? L"Connected" : L"Disconnected");
            swprintf(adapter_details[adapter_count], count_of(adapter_details[0]), L"%ls   \u00b7   %llu Mbps",
                     address, a->OperStatus == IfOperStatusUp ? a->TransmitLinkSpeed / 1000000 : 0);
            w_item(label, adapter_count++);
        }
    }
    free(all);
    w_message(adapter_count ? L"Select an adapter to see its address and link speed." : L"No network adapters found.");
}

static void w_selected(void) {
    LRESULT row = SendMessageW(list, LB_GETCURSEL, 0, 0);
    int id = row == LB_ERR ? -1 : (int)SendMessageW(list, LB_GETITEMDATA, row, 0);
    EnableWindow(apply, id >= 0);
    if (page == page_network && id >= 0 && id < adapter_count)
        w_message(adapter_details[id]);
    if (page == page_wifi) {
        struct w_wifi_profile *p = id >= 0 && id < profile_count ? &profiles[id] : NULL;
        bool connected = p && (p->network.dwFlags & WLAN_AVAILABLE_NETWORK_CONNECTED);
        SetWindowTextW(apply, connected ? L"Disconnect" : L"Connect");
        SetWindowTextW(password, L"");
        bool needs_key = p && !*p->profile && p->network.bSecurityEnabled && w_personal_auth(&p->network);
        ShowWindow(password, needs_key ? SW_SHOW : SW_HIDE);
        EnableWindow(apply, p && (connected || *p->profile || p->network.bNetworkConnectable));
    }
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
        if (profiles[id].network.dwFlags & WLAN_AVAILABLE_NETWORK_CONNECTED) {
            if (wifi.disconnect && wifi.disconnect(wlan, &profiles[id].adapter, NULL) == ERROR_SUCCESS) {
                w_wifi_items(false);
                w_message(L"Disconnect requested.");
            } else w_message(L"Windows could not disconnect this adapter.");
        } else w_join(&profiles[id]);
    } else if (page == page_power && id >= 0 && id < plan_count) {
        w_message(PowerSetActiveScheme(NULL, &plans[id]) == ERROR_SUCCESS ? L"Power plan active." : L"Unable to change power plan.");
    } else if (page == page_bluetooth && id >= 0 && id < paired_count) {
        typedef BOOL (WINAPI *properties_fn)(HWND, BLUETOOTH_DEVICE_INFO *);
        properties_fn properties = (properties_fn)GetProcAddress(bluetooth, "BluetoothDisplayDeviceProperties");
        if (!properties || !properties(window, &paired[id]))
            w_message(L"Windows could not open device properties.");
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
        if ((HWND)lp == list && HIWORD(wp) == LBN_SELCHANGE)
            w_selected();
        if (LOWORD(wp) == 101 || ((HWND)lp == list && HIWORD(wp) == LBN_DBLCLK))
            w_use_selection(0);
        if (LOWORD(wp) == 104) {
            if (page == page_bluetooth) w_bluetooth_items();
            if (page == page_wifi) w_wifi_items(true);
            if (page == page_network) w_network_items();
            w_selected();
        }
        if (LOWORD(wp) == 105 && !w_os_launch_provider("%SystemRoot%\\System32\\DevicePairingWizard.exe", NULL))
            w_message(L"Windows pairing wizard unavailable.");
        if (LOWORD(wp) == 102)
            w_use_selection(-5);
        if (LOWORD(wp) == 103)
            w_use_selection(5);
        return 0;
    }
    if (msg == wifi_result || msg == wifi_scan_done) {
        if (!IsWindowVisible(password) || !GetWindowTextLengthW(password)) {
            w_wifi_items(false);
            w_selected();
        }
        if (msg == wifi_result)
            w_message(wp == WLAN_REASON_CODE_SUCCESS ? L"Connected." : L"Connection failed. Check the password and network availability.");
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
    page = !wcscmp(name, L"power_settings") ? page_power
           : !wcscmp(name, L"network_settings") ? page_network
           : !wcscmp(name, L"wifi")        ? page_wifi
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
    const wchar_t *titles[] = {L"Audio endpoints", L"Wi-Fi networks", L"Bluetooth devices", L"Display modes",
                               L"Power plans", L"Network adapters"};
    RegisterClassW(&wc);
    HWND foreground = GetForegroundWindow();
    UINT dpi = GetDpiForWindow(foreground);
    if (!dpi)
        dpi = 96;
    MONITORINFO monitor = {.cbSize = sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(foreground, MONITOR_DEFAULTTOPRIMARY), &monitor);
    DWORD style = WS_CAPTION | WS_SYSMENU;
    RECT bounds = {0, 0, MulDiv(544, dpi, 96), MulDiv(344, dpi, 96)};
    AdjustWindowRectExForDpi(&bounds, style, FALSE, WS_EX_TOOLWINDOW, dpi);
    int width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
    window = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, titles[page], style,
                            (monitor.rcWork.left + monitor.rcWork.right - width) / 2,
                            (monitor.rcWork.top + monitor.rcWork.bottom - height) / 2,
                            width, height, NULL, NULL, inst, NULL);
    if (!window)
        return 1;
    list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_HASSTRINGS |
                           LBS_OWNERDRAWFIXED | LBS_NOINTEGRALHEIGHT, 12, 10, 520, 238,
                           window, NULL, inst, NULL);
    status = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 12, 258, 520, 34, window, NULL, inst, NULL);
    const wchar_t *labels[] = {L"Mute / unmute", L"Connect", L"Device properties", L"Preview", L"Activate", L"Details"};
    apply = CreateWindowW(L"BUTTON", labels[page],
                          BS_OWNERDRAW | WS_CHILD | WS_TABSTOP | (page == page_network ? 0 : WS_VISIBLE),
                          12, 300, 140, 30, window, (HMENU)101, inst, NULL);
    if (page == page_audio) {
        lower = CreateWindowW(L"BUTTON", L"Volume −", BS_OWNERDRAW | WS_CHILD | WS_VISIBLE | WS_TABSTOP, 160,
                              300, 110, 30, window, (HMENU)102, inst, NULL);
        raise_button =
            CreateWindowW(L"BUTTON", L"Volume +", BS_OWNERDRAW | WS_CHILD | WS_VISIBLE | WS_TABSTOP, 280, 300,
                          110, 30, window, (HMENU)103, inst, NULL);
    }
    if (page == page_network || page == page_wifi || page == page_bluetooth) {
        int x = page == page_network ? 12 : 160;
        lower = CreateWindowW(L"BUTTON", L"Refresh", BS_OWNERDRAW | WS_CHILD | WS_VISIBLE | WS_TABSTOP, x,
                              300, 110, 30, window, (HMENU)104, inst, NULL);
    }
    if (page == page_bluetooth)
        raise_button = CreateWindowW(L"BUTTON", L"Pair new device", BS_OWNERDRAW | WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                     280, 300, 150, 30, window, (HMENU)105, inst, NULL);
    if (page == page_wifi) {
        password = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_TABSTOP | WS_BORDER | ES_PASSWORD | ES_AUTOHSCROLL,
                                 284, 300, 246, 30, window, NULL, inst, NULL);
        SendMessageW(password, EM_SETCUEBANNER, TRUE, (LPARAM)L"Wi-Fi password");
        SendMessageW(password, EM_SETLIMITTEXT, 64, 0);
    }
    HFONT font = CreateFontW(-MulDiv(15, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HWND controls[] = {list, status, apply, lower, raise_button, password};
    for (size_t i = 0; i < count_of(controls); i++) {
        HWND control = controls[i];
        if (!control)
            continue;
        RECT r;
        GetWindowRect(control, &r);
        MapWindowPoints(HWND_DESKTOP, window, (POINT *)&r, 2);
        MoveWindow(control, MulDiv(r.left, dpi, 96), MulDiv(r.top, dpi, 96),
                   MulDiv(r.right - r.left, dpi, 96), MulDiv(r.bottom - r.top, dpi, 96), FALSE);
        SendMessageW(control, WM_SETFONT, (WPARAM)font, FALSE);
    }
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    if (page == page_audio)
        w_audio_items();
    else if (page == page_wifi)
        w_wifi_items(false);
    else if (page == page_bluetooth)
        w_bluetooth_items();
    else if (page == page_network)
        w_network_items();
    else if (page == page_power)
        w_power_items();
    else
        w_display_items();
    if (page != page_power)
        SendMessageW(list, LB_SETCURSEL, 0, 0);
    w_selected();
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
    DeleteObject(font);
    CoUninitialize();
    return 0;
}
