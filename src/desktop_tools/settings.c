#define COBJMACROS
#include "theme/theme.h"
#include "desktop_tools/tools.h"
#include "desktop_tools/adapter.h"
#include "desktop_tools/display.h"
#include <initguid.h>
#include "desktop_tools/sound.h"
#include "desktop_tools/network.h"
#include <wlanapi.h>
#ifndef ERROR_NDIS_DOT11_POWER_STATE_INVALID
#define ERROR_NDIS_DOT11_POWER_STATE_INVALID 0x80342002UL
#endif
#include "desktop_tools/bluetooth.h"
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include "desktop_tools/power.h"
#include <commctrl.h>
#include <commdlg.h>
#include <stdio.h>
#include <wchar.h>
enum page { page_audio, page_wifi, page_bluetooth, page_display, page_power, page_network };

////////////////
//~ Settings

static enum page page;
static HWND window, list, apply, lower, raise_button, status;
static HWND password, network_switch, saved_action;
static w_adapter_problem network_problems[32];
static size_t problem_count;
static IP_ADAPTER_ADDRESSES *adapters;
static bool mixer;
static HWND sound_slider, default_device, refresh_sound;
static ULONGLONG sound_refresh_due;
static HANDLE wlan;
static struct w_wifi_profile {
    GUID adapter;
    wchar_t profile[WLAN_MAX_NAME_LENGTH];
    WLAN_AVAILABLE_NETWORK network;
} profiles[256];
static int profile_count;
static void w_selected(void);

static bool w_same_network(const struct w_wifi_profile *a, const struct w_wifi_profile *b) {
    return IsEqualGUID(&a->adapter, &b->adapter) && !wcscmp(a->profile, b->profile) &&
           a->network.dot11DefaultAuthAlgorithm == b->network.dot11DefaultAuthAlgorithm &&
           a->network.dot11Ssid.uSSIDLength == b->network.dot11Ssid.uSSIDLength &&
           !memcmp(a->network.dot11Ssid.ucSSID, b->network.dot11Ssid.ucSSID, a->network.dot11Ssid.uSSIDLength);
}

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

static void w_show_network_problems(bool wireless) {
    problem_count = w_network_problems(network_problems, count_of(network_problems), wireless);
    for (size_t i = 0; i < problem_count; ++i)
        w_item(network_problems[i].label, -2 - (int)i);
}

static int w_current_item(void) {
    LRESULT row = SendMessageW(list, LB_GETCURSEL, 0, 0);
    return row == LB_ERR ? -1 : (int)SendMessageW(list, LB_GETITEMDATA, row, 0);
}

static void w_audio_selection(void) {
    int id = w_current_item(), percent = 0;
    BOOL muted = FALSE;
    bool ready = w_sound_state(id, &percent, &muted);
    EnableWindow(sound_slider, ready);
    EnableWindow(apply, ready);
    EnableWindow(default_device, ready && w_sound_can_default(id));
    SendMessageW(sound_slider, TBM_SETPOS, TRUE, percent);
    SetWindowTextW(apply, muted ? L"Unmute" : L"Mute");
    wchar_t level[64];
    swprintf(level, count_of(level), L"Volume   %d%%%ls", percent, muted ? L"   ·   Muted" : L"");
    w_message(ready ? level : id >= 0 ? L"Disconnected. Refresh." : mixer ? L"No audio sessions" : L"No sound devices");
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
    __typeof__(&WlanSaveTemporaryProfile) save;
    __typeof__(&WlanDeleteProfile) forget;
    __typeof__(&WlanQueryInterface) query;
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
    wifi.save = (__typeof__(wifi.save))GetProcAddress(lib, "WlanSaveTemporaryProfile");
    wifi.forget = (__typeof__(wifi.forget))GetProcAddress(lib, "WlanDeleteProfile");
    wifi.query = (__typeof__(wifi.query))GetProcAddress(lib, "WlanQueryInterface");
    wifi.free_memory = (void(WINAPI *)(PVOID))GetProcAddress(lib, "WlanFreeMemory");
    return wifi.open && wifi.close && wifi.interfaces && wifi.profiles && wifi.connect && wifi.free_memory;
}

enum { wifi_result = WM_APP + 1, wifi_scan_done };

static void WINAPI w_wireless_event(WLAN_NOTIFICATION_DATA *event, void *context) {
    if (event->NotificationSource != WLAN_NOTIFICATION_SOURCE_ACM)
        return;
    if (event->NotificationCode == wlan_notification_acm_scan_complete ||
        event->NotificationCode == wlan_notification_acm_disconnected ||
        event->NotificationCode == wlan_notification_acm_interface_arrival ||
        event->NotificationCode == wlan_notification_acm_interface_removal)
        PostMessageW(context, wifi_scan_done, 0, 0);
    if (event->NotificationCode == wlan_notification_acm_connection_complete ||
        event->NotificationCode == wlan_notification_acm_connection_attempt_fail) {
        WLAN_CONNECTION_NOTIFICATION_DATA *data = event->pData;
        if (data && event->dwDataSize >= offsetof(WLAN_CONNECTION_NOTIFICATION_DATA, strProfileXml))
            PostMessageW(context, wifi_result, data->wlanReasonCode, 0);
    }
}

static void w_wifi_items(bool scan) {
    LRESULT row = SendMessageW(list, LB_GETCURSEL, 0, 0);
    int selected = row == LB_ERR ? -1 : (int)SendMessageW(list, LB_GETITEMDATA, row, 0);
    bool had_selection = selected >= 0 && selected < profile_count;
    struct w_wifi_profile previous = {0};
    if (had_selection)
        previous = profiles[selected];
    profile_count = 0;
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    ShowWindow(password, SW_HIDE);
    problem_count = 0;
    if (!wlan) {
        DWORD version = 0;
        DWORD error = ERROR_NOT_SUPPORTED;
        if ((wifi.open || w_load_wifi()) && wifi.close && wifi.interfaces && wifi.profiles && wifi.connect && wifi.free_memory)
            error = wifi.open(2, NULL, &version, &wlan);
        if (error != ERROR_SUCCESS) {
            w_show_network_problems(true);
            w_message(error == ERROR_SERVICE_NOT_ACTIVE ? L"WLAN AutoConfig is stopped. Start the service to scan." :
                      error == ERROR_ACCESS_DENIED ? L"Windows denied access to Wi-Fi." : L"Windows Wi-Fi service unavailable.");
            return;
        }
        if (wifi.notify)
            wifi.notify(wlan, WLAN_NOTIFICATION_SOURCE_ACM, TRUE, w_wireless_event, window, NULL, NULL);
    }
    WLAN_INTERFACE_INFO_LIST *interfaces = NULL;
    if (wifi.interfaces(wlan, NULL, &interfaces) != ERROR_SUCCESS) {
        wifi.close(wlan, NULL);
        wlan = NULL;
        w_show_network_problems(true);
        w_message(L"Windows could not list Wi-Fi adapters.");
        return;
    }
    bool restricted = false, radio_off = false, scanning = false;
    DWORD interface_count = interfaces->dwNumberOfItems;
    for (DWORD i = 0; i < interfaces->dwNumberOfItems; i++) {
        WLAN_INTERFACE_INFO *adapter = &interfaces->InterfaceInfo[i];
        if (scan && wifi.scan) {
            DWORD result = wifi.scan(wlan, &adapter->InterfaceGuid, NULL, NULL, NULL);
            scanning |= result == ERROR_SUCCESS;
            restricted |= result == ERROR_ACCESS_DENIED;
            radio_off |= result == ERROR_NDIS_DOT11_POWER_STATE_INVALID;
        }
        WLAN_AVAILABLE_NETWORK_LIST *nearby = NULL;
        DWORD result = wifi.available ? wifi.available(wlan, &adapter->InterfaceGuid, 0, NULL, &nearby) : ERROR_NOT_SUPPORTED;
        if (result == ERROR_SUCCESS) {
            for (DWORD j = 0; j < nearby->dwNumberOfItems && profile_count < 256; j++) {
                WLAN_AVAILABLE_NETWORK *n = &nearby->Network[j];
                if (!n->dot11Ssid.uSSIDLength || n->dot11Ssid.uSSIDLength > DOT11_SSID_MAX_LENGTH)
                    continue;
                struct w_wifi_profile *p = profiles + profile_count;
                *p = (struct w_wifi_profile){.adapter = adapter->InterfaceGuid, .network = *n};
                if (n->dwFlags & WLAN_AVAILABLE_NETWORK_HAS_PROFILE) wcscpy(p->profile, n->strProfileName);
                wchar_t ssid[33] = {0}, label[440];
                MultiByteToWideChar(CP_UTF8, 0, (LPCSTR)n->dot11Ssid.ucSSID, n->dot11Ssid.uSSIDLength, ssid, 32);
                swprintf(label, count_of(label), L"%ls   %lu%%   %ls%ls", *ssid ? ssid : L"Network", n->wlanSignalQuality,
                         n->dwFlags & WLAN_AVAILABLE_NETWORK_CONNECTED ? L"Connected" : *p->profile ? L"Saved" :
                         n->bSecurityEnabled ? L"Secured" : L"Open", n->bNetworkConnectable ? L"" : L"  (unavailable)");
                if (interface_count > 1) {
                    size_t used = wcslen(label);
                    swprintf(label + used, count_of(label) - used, L"   ·   %.160ls", adapter->strInterfaceDescription);
                }
                w_item(label, profile_count++);
            }
            wifi.free_memory(nearby);
        }
        restricted |= result == ERROR_ACCESS_DENIED;
        radio_off |= result == ERROR_NDIS_DOT11_POWER_STATE_INVALID;
        WLAN_PROFILE_INFO_LIST *saved = NULL;
        if (wifi.profiles(wlan, &adapter->InterfaceGuid, NULL, &saved) != ERROR_SUCCESS)
            continue;
        for (DWORD j = 0; j < saved->dwNumberOfItems && profile_count < 256; j++) {
            const wchar_t *name = saved->ProfileInfo[j].strProfileName;
            bool listed = false;
            for (int k = 0; k < profile_count; ++k)
                if (IsEqualGUID(&profiles[k].adapter, &adapter->InterfaceGuid) && !wcscmp(profiles[k].profile, name)) {
                    listed = true;
                    break;
                }
            if (listed) continue;
            profiles[profile_count] = (struct w_wifi_profile){.adapter = adapter->InterfaceGuid};
            wcscpy(profiles[profile_count].profile, name);
            wchar_t label[512];
            swprintf(label, count_of(label), L"%.250ls   ·   Saved%ls%.160ls", name, interface_count > 1 ? L"   ·   " : L"",
                     interface_count > 1 ? adapter->strInterfaceDescription : L"");
            w_item(label, profile_count);
            ++profile_count;
        }
        wifi.free_memory(saved);
    }
    wifi.free_memory(interfaces);
    w_show_network_problems(true);
    if (scanning)
        SetTimer(window, 3, 4000, NULL);
    if (profile_count)
        SendMessageW(list, LB_SETCURSEL, 0, 0);
    if (had_selection) {
        for (int i = 0; i < profile_count; i++) {
            const struct w_wifi_profile *p = &profiles[i];
            if (w_same_network(&previous, p)) {
                SendMessageW(list, LB_SETCURSEL, i, 0);
                break;
            }
        }
    }
    w_message(restricted ? L"Allow location access for desktop apps to scan. Saved profiles are shown."
              : radio_off ? L"The Wi-Fi radio is off. Turn it on to scan."
              : scanning ? L"Scanning for nearby networks…"
              : !interface_count ? L"No usable Wi-Fi adapter. Check the network hardware and its driver."
              : profile_count ? L"Select a network." : L"No nearby Wi-Fi networks found. Refresh to scan again.");
}

static const wchar_t *w_personal_auth(const WLAN_AVAILABLE_NETWORK *n) {
    switch (n->dot11DefaultAuthAlgorithm) {
    case DOT11_AUTH_ALGO_WPA_PSK: return L"WPAPSK";
    case DOT11_AUTH_ALGO_RSNA_PSK: return L"WPA2PSK";
    case DOT11_AUTH_ALGO_WPA3_SAE: return L"WPA3SAE";
    default: return NULL;
    }
}

static void w_saved_profile(void) {
    int id = w_current_item();
    if (id < 0 || id >= profile_count) return;
    struct w_wifi_profile p = profiles[id];
    DWORD error = ERROR_INVALID_STATE;
    if (*p.profile && wifi.forget) {
        wchar_t question[480];
        swprintf(question,count_of(question),L"Forget %.250ls? Its saved credentials will be removed from this adapter.",p.profile);
        if (MessageBoxW(window,question,L"Forget network",MB_YESNO|MB_DEFBUTTON2|MB_ICONQUESTION) != IDYES) return;
        error = wifi.forget(wlan,&p.adapter,p.profile,NULL);
    } else if (wifi.save && wifi.query && (p.network.dwFlags & WLAN_AVAILABLE_NETWORK_CONNECTED)) {
        DWORD size = 0;
        WLAN_CONNECTION_ATTRIBUTES *current = NULL;
        error = wifi.query(wlan,&p.adapter,wlan_intf_opcode_current_connection,NULL,&size,(void **)&current,NULL);
        if (error == ERROR_SUCCESS) {
            const DOT11_SSID *ssid = &p.network.dot11Ssid;
            if (size >= sizeof(*current) && ssid->uSSIDLength && ssid->uSSIDLength <= DOT11_SSID_MAX_LENGTH &&
                current->wlanAssociationAttributes.dot11Ssid.uSSIDLength == ssid->uSSIDLength &&
                !memcmp(current->wlanAssociationAttributes.dot11Ssid.ucSSID, ssid->ucSSID, ssid->uSSIDLength)) {
                wchar_t name[65] = {0};
                int n = MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,(const char *)ssid->ucSSID,ssid->uSSIDLength,name,32);
                if (!n || (int)wcslen(name) != n)
                    for (DWORD i=0;i<ssid->uSSIDLength;i++) swprintf(name+2*i,3,L"%02X",ssid->ucSSID[i]);
                error = wifi.save(wlan,&p.adapter,name,NULL,WLAN_PROFILE_USER,FALSE,NULL);
            } else error = ERROR_INVALID_STATE;
            wifi.free_memory(current);
        }
    }
    w_wifi_items(false);
    w_selected();
    w_message(error == ERROR_SUCCESS ? (*p.profile ? L"Saved network forgotten." : L"Network saved for your account.") :
              error == ERROR_ALREADY_EXISTS ? L"A profile with that name already exists." :
              error == ERROR_ACCESS_DENIED ? L"Windows denied permission to change this profile." :
                                             L"The profile or connection changed. Refresh and try again.");
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


static IP_ADAPTER_ADDRESSES *w_adapter_at(int index) {
    if (index < 0) return NULL;
    IP_ADAPTER_ADDRESSES *a = adapters;
    while (index-- > 0 && a) a = a->Next;
    return a;
}

static void w_network_items(void) {
    IP_ADAPTER_ADDRESSES *previous = w_adapter_at(w_current_item());
    ULONG64 previous_luid = previous ? previous->Luid.Value : 0;
    ULONG bytes = 16 * 1024;
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_INCLUDE_ALL_INTERFACES;
    IP_ADAPTER_ADDRESSES *all = NULL;
    ULONG result = ERROR_NOT_ENOUGH_MEMORY;
    for (int attempt=0; attempt<4; ++attempt) {
        IP_ADAPTER_ADDRESSES *next = realloc(all, bytes);
        if (!next) { result = ERROR_NOT_ENOUGH_MEMORY; break; }
        all = next;
        result = GetAdaptersAddresses(AF_UNSPEC, flags, NULL, all, &bytes);
        if (result != ERROR_BUFFER_OVERFLOW) break;
    }
    if (result != NO_ERROR && result != ERROR_NO_DATA) {
        free(all);
        w_message(L"Windows could not refresh the network adapters.");
        return;
    }
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    free(adapters);
    adapters = result == ERROR_NO_DATA ? NULL : all;
    if (!adapters) free(all);
    LRESULT select = -1;
    int i = 0;
    for (IP_ADAPTER_ADDRESSES *a = adapters; a; a = a->Next, ++i) {
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        wchar_t label[320];
        swprintf(label, count_of(label), L"%.185ls   ·   %ls%ls", a->FriendlyName ? a->FriendlyName : L"Network adapter",
                 a->IfType == IF_TYPE_IEEE80211 ? L"Wi-Fi   ·   " : L"",
                 a->OperStatus == IfOperStatusUp ? L"Connected" : L"Disconnected");
        w_item(label, i);
        if (previous_luid == a->Luid.Value || (select < 0 && a->OperStatus == IfOperStatusUp))
            select = SendMessageW(list, LB_GETCOUNT, 0, 0) - 1;
    }
    w_show_network_problems(false);
    SendMessageW(list, LB_SETCURSEL, select >= 0 ? select : 0, 0);
    w_message(adapters || problem_count ? L"Select an adapter for addresses, DNS and link details." : L"No network adapters found.");
}

static void w_selected(void) {
    LRESULT row = SendMessageW(list, LB_GETCURSEL, 0, 0);
    int id = row == LB_ERR ? -1 : (int)SendMessageW(list, LB_GETITEMDATA, row, 0);
    EnableWindow(apply, id >= 0);
    if (page == page_audio) w_audio_selection();
    if (page == page_power) w_power_selected();
    if (page == page_bluetooth) w_bluetooth_selected();
    if (page == page_network && id >= 0) {
        IP_ADAPTER_ADDRESSES *a = w_adapter_at(id);
        EnableWindow(apply, a != NULL);
        if (a && a->Description) w_message(a->Description);
    }
    if (page == page_wifi) {
        static struct w_wifi_profile key_target;
        struct w_wifi_profile *p = id >= 0 && id < profile_count ? &profiles[id] : NULL;
        bool connected = p && (p->network.dwFlags & WLAN_AVAILABLE_NETWORK_CONNECTED);
        SetWindowTextW(apply, connected ? L"Disconnect" : L"Connect");
        SetWindowTextW(saved_action, p && *p->profile ? L"Forget network…" : L"Save network");
        EnableWindow(saved_action, p && (*p->profile ? wifi.forget != NULL : connected && wifi.save && wifi.query));
        bool needs_key = p && !*p->profile && p->network.bSecurityEnabled && w_personal_auth(&p->network);
        if (!needs_key || !w_same_network(&key_target, p))
            SetWindowTextW(password, L"");
        key_target = needs_key ? *p : (struct w_wifi_profile){0};
        ShowWindow(password, needs_key ? SW_SHOW : SW_HIDE);
        EnableWindow(apply, p && (connected || *p->profile || p->network.bNetworkConnectable));
    }
    if ((page == page_wifi || page == page_network) && id <= -2 && (size_t)(-2-id) < problem_count)
        w_message(network_problems[-2-id].detail);
}

static void w_size_controls(bool update_font);

static void w_open_network_page(enum page next) {
    page = next;
    bool wireless = page == page_wifi;
    SetWindowTextW(window, wireless ? L"Wi-Fi networks" : L"Network adapters");
    SetWindowTextW(network_switch, wireless ? L"Network adapters" : L"Wi-Fi networks");
    ShowWindow(saved_action, wireless ? SW_SHOWNA : SW_HIDE);
    SetWindowTextW(password, L"");
    ShowWindow(password, SW_HIDE);
    SetWindowTextW(apply, wireless ? L"Connect" : L"Adapter details…");
    ShowWindow(apply, SW_SHOWNA);
    KillTimer(window, 3);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    if (wireless) w_wifi_items(true);
    else w_network_items();
    if (SendMessageW(list, LB_GETCURSEL, 0, 0) == LB_ERR)
        SendMessageW(list, LB_SETCURSEL, 0, 0);
    w_selected();
    w_size_controls(false);
}


static void w_use_selection(void) {
    LRESULT row = SendMessageW(list, LB_GETCURSEL, 0, 0);
    if (row == LB_ERR)
        return;
    int id = (int)SendMessageW(list, LB_GETITEMDATA, row, 0);
    if (page == page_audio) {
        bool ok = w_sound_change(id, -1);
        w_audio_selection();
        if (!ok) w_message(L"Unable to change mute.");
    } else if (page == page_wifi && id >= 0 && id < profile_count && IsWindowEnabled(apply)) {
        if (profiles[id].network.dwFlags & WLAN_AVAILABLE_NETWORK_CONNECTED) {
            if (wifi.disconnect && wifi.disconnect(wlan, &profiles[id].adapter, NULL) == ERROR_SUCCESS) {
                w_wifi_items(false);
                w_message(L"Disconnect requested.");
            } else w_message(L"Windows could not disconnect this adapter.");
        } else w_join(&profiles[id]);
    } else if (page == page_network) {
        IP_ADAPTER_ADDRESSES *a = w_adapter_at(id);
        if (a && w_adapter_show(window, a)) {
            w_network_items();
            w_selected();
        }
    } else if (page == page_power) {
        w_power_save();
    } else if (page == page_bluetooth) {
        w_bluetooth_apply();
    } else if (page == page_display)
        w_display_preview();
}

static HFONT panel_font;
static struct { HWND control; RECT logical; int dropdown_height; } positions[32];
static size_t position_count;

static void w_measure_controls(void) {
    for (HWND control = GetWindow(window, GW_CHILD); control && position_count < count_of(positions);
         control = GetWindow(control, GW_HWNDNEXT)) {
        RECT r;
        GetWindowRect(control, &r);
        MapWindowPoints(NULL, window, (POINT *)&r, 2);
        positions[position_count].control = control;
        wchar_t kind[32];
        RECT dropped;
        if (GetClassNameW(control, kind, count_of(kind)) && !wcscmp(kind, L"ComboBox") &&
            SendMessageW(control, CB_GETDROPPEDCONTROLRECT, 0, (LPARAM)&dropped))
            positions[position_count].dropdown_height = dropped.bottom - dropped.top;
        positions[position_count++].logical = r;
    }
}

static void w_size_controls(bool update_font) {
    if (!position_count) return;
    UINT dpi = GetDpiForWindow(window);
    HFONT old = NULL;
    if (update_font) {
        old = panel_font;
        panel_font = CreateFontW(-MulDiv(15, dpi, 96), 0,0,0, FW_NORMAL, FALSE,FALSE,FALSE,
                                 DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        SendMessageW(list, LB_SETITEMHEIGHT, 0, MulDiv(30, dpi, 96));
    }
    RECT area;
    GetClientRect(window, &area);
    int width = MulDiv(area.right, 96, dpi);
    int dx = width - 544, dy = MulDiv(area.bottom, 96, dpi) - 344;
    bool wrap = width < 456 && (page == page_display || page == page_bluetooth || page == page_wifi ||
                               (page == page_audio && !mixer));
    int extra_row = wrap ? 38 : 0;
    for (size_t i = 0; i < position_count; ++i) {
        HWND c = positions[i].control;
        RECT r = positions[i].logical;
        if (c == list) r.bottom += dy - extra_row;
        else if (r.top >= 248) OffsetRect(&r, 0, dy - extra_row);
        if (wrap && positions[i].logical.top >= 300 && positions[i].logical.left >= (page == page_display ? 160 : 270)) {
            int first_x = page == page_display ? 162 : positions[i].logical.left;
            OffsetRect(&r, 12 - first_x, extra_row);
        }
        if (c == list || (c == status && page != page_audio) || c == password || c == sound_slider ||
            (page == page_display && w_display_stretches(c))) r.right += dx;
        if (wrap && (c == password || (page == page_display && positions[i].logical.left >= 320)))
            r.right = width - 12;
        if (page == page_power && positions[i].logical.top < 48) {
            if (r.left > 12) OffsetRect(&r, dx, 0);
            else r.right += dx;
        }
        if (r.right > width - 12) r.right = width - 12;
        int height = max(1, r.bottom - r.top);
        if (positions[i].dropdown_height) height = max(height, positions[i].dropdown_height);
        if (update_font) SendMessageW(c, WM_SETFONT, (WPARAM)panel_font, FALSE);
        MoveWindow(c, MulDiv(r.left,dpi,96), MulDiv(r.top,dpi,96),
                   MulDiv(max(1,r.right-r.left),dpi,96), MulDiv(height,dpi,96), TRUE);
    }
    if (old) DeleteObject(old);
}

static LRESULT CALLBACK w_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    LRESULT color_result;
    if (w_theme_message(w, msg, wp, lp, &color_result))
        return color_result;
    if (page == page_display && w_display_message(msg, wp, lp)) return 0;
    if (page == page_power && w_power_message(msg, wp, lp)) return 0;
    if (msg == WM_GETMINMAXINFO) {
        UINT dpi = GetDpiForWindow(w);
        RECT r = {0, 0, MulDiv(356,dpi,96), MulDiv(266,dpi,96)};
        AdjustWindowRectExForDpi(&r, GetWindowLongW(w, GWL_STYLE), FALSE, GetWindowLongW(w, GWL_EXSTYLE), dpi);
        w_tool_minimum(w,(MINMAXINFO *)lp,r.right-r.left,r.bottom-r.top);
        return 0;
    }
    if (msg == WM_DPICHANGED) {
        w_tool_resize_for_dpi(w,(RECT *)lp);
        w_size_controls(true);
        return 0;
    }
    if (msg == WM_SIZE && wp != SIZE_MINIMIZED) {
        w_size_controls(false);
        return 0;
    }
    if (msg == WM_COMMAND) {
        if (page == page_wifi && saved_action && (HWND)lp == saved_action) {
            w_saved_profile();
            return 0;
        }
        if (network_switch && (HWND)lp == network_switch) {
            w_open_network_page(page == page_wifi ? page_network : page_wifi);
            SetFocus(list);
            return 0;
        }
        if ((HWND)lp == list && HIWORD(wp) == LBN_SELCHANGE)
            w_selected();
        if (LOWORD(wp) == 101 || ((HWND)lp == list && HIWORD(wp) == LBN_DBLCLK))
            w_use_selection();
        if (LOWORD(wp) == 104) {
            if (page == page_bluetooth) w_bluetooth_scan(true);
            if (page == page_wifi) w_wifi_items(true);
            if (page == page_network) w_network_items();
            w_selected();
        }
        if (page == page_bluetooth && LOWORD(wp) == 105 &&
            !w_os_launch_provider("%SystemRoot%\\System32\\DevicePairingWizard.exe", NULL))
            w_message(L"Windows discovery for additional devices is unavailable.");
        if (page == page_audio && (LOWORD(wp) == 106 || LOWORD(wp) == 107)) {
            bool ok = LOWORD(wp) == 106 || w_sound_make_default(w_current_item());
            w_sound_refresh(list, mixer);
            w_audio_selection();
            if (!ok) w_message(L"Default device change failed.");
        }
        return 0;
    }
    if ((msg == wifi_result || msg == wifi_scan_done || (msg == WM_TIMER && wp == 3)) && page == page_wifi) {
        KillTimer(window, 3);
        if (!IsWindowVisible(password) || !GetWindowTextLengthW(password)) {
            w_wifi_items(false);
            w_selected();
        }
        if (msg == wifi_result)
            w_message(wp == WLAN_REASON_CODE_SUCCESS ? L"Connected." : L"Connection failed. Check the password and network availability.");
        return 0;
    }
    if (msg == WM_HSCROLL && sound_slider && (HWND)lp == sound_slider) {
        bool ok = w_sound_change(w_current_item(), (int)SendMessageW(sound_slider, TBM_GETPOS, 0, 0));
        w_audio_selection();
        if (!ok) w_message(L"Volume change failed.");
        return 0;
    }
    if (msg == WM_TIMER && wp == 2 && page == page_audio) {
        if (!GetCapture() && !IsIconic(w)) {
            if (GetTickCount64() >= sound_refresh_due) {
                w_sound_refresh(list, mixer);
                sound_refresh_due = GetTickCount64() + 5000;
            }
            w_audio_selection();
        }
        return 0;
    }
    if (msg == WM_DESTROY) {
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
    mixer = !wcscmp(name, L"volume_mixer");
    INITCOMMONCONTROLSEX common = {sizeof(common), ICC_BAR_CLASSES | ICC_DATE_CLASSES};
    InitCommonControlsEx(&common);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSW wc = {.lpfnWndProc = w_proc,
                    .hInstance = inst,
                    .lpszClassName = L"WinwmSettings",
                    .hCursor = LoadCursorW(NULL, IDC_ARROW),
                    .hbrBackground = NULL};
    const wchar_t *titles[] = {mixer ? L"Application volume mixer" : L"Sound devices", L"Wi-Fi networks", L"Bluetooth devices", L"Display settings",
                               L"Power plans", L"Network adapters"};
    RegisterClassW(&wc);
    MONITORINFO monitor = {.cbSize = sizeof(monitor)};
    UINT dpi = w_tool_work_area(&monitor);
    DWORD style = WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MAXIMIZEBOX;
    RECT bounds = {0, 0, MulDiv(544, dpi, 96), MulDiv(344, dpi, 96)};
    AdjustWindowRectExForDpi(&bounds, style, FALSE, WS_EX_TOOLWINDOW, dpi);
    int width = min(bounds.right - bounds.left, monitor.rcWork.right - monitor.rcWork.left);
    int height = min(bounds.bottom - bounds.top, monitor.rcWork.bottom - monitor.rcWork.top);
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
        sound_slider = CreateWindowW(TRACKBAR_CLASSW, L"Volume", WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_NOTICKS,
                                     164, 252, 368, 36, window, NULL, inst, NULL);
        MoveWindow(status, 12, 258, 148, 34, FALSE);
        SendMessageW(sound_slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        SendMessageW(sound_slider, TBM_SETPAGESIZE, 0, 5);
        default_device = CreateWindowW(L"BUTTON", L"Set as default", BS_OWNERDRAW | WS_CHILD | WS_TABSTOP |
                                       (mixer ? 0 : WS_VISIBLE), 160, 300, 150, 30, window, (HMENU)107, inst, NULL);
        refresh_sound = CreateWindowW(L"BUTTON", L"Refresh", BS_OWNERDRAW | WS_CHILD | WS_TABSTOP | WS_VISIBLE,
                                      mixer ? 160 : 320, 300, 110, 30, window, (HMENU)106, inst, NULL);
    }
    if (page == page_network || page == page_wifi || page == page_bluetooth) {
        lower = CreateWindowW(L"BUTTON", page == page_bluetooth ? L"Scan nearby" : L"Refresh",
                              BS_OWNERDRAW | WS_CHILD | WS_VISIBLE | WS_TABSTOP, 160,
                              300, 110, 30, window, (HMENU)104, inst, NULL);
    }
    if (page == page_bluetooth)
        raise_button = CreateWindowW(L"BUTTON", L"More devices…", BS_OWNERDRAW | WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                     280, 300, 150, 30, window, (HMENU)105, inst, NULL);
    if (page == page_wifi || page == page_network) {
        MoveWindow(list, 12, 49, 520, 199, FALSE);
        network_switch = CreateWindowW(L"BUTTON", L"", BS_OWNERDRAW | WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                       12, 10, 165, 30, window, (HMENU)108, inst, NULL);
        saved_action = CreateWindowW(L"BUTTON", L"Save network", BS_OWNERDRAW | WS_CHILD | WS_TABSTOP,
                                     188, 10, 155, 30, window, (HMENU)109, inst, NULL);
        password = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_TABSTOP | WS_BORDER | ES_PASSWORD | ES_AUTOHSCROLL,
                                 284, 300, 246, 30, window, NULL, inst, NULL);
        SendMessageW(password, EM_SETCUEBANNER, TRUE, (LPARAM)L"Wi-Fi password");
        SendMessageW(password, EM_SETLIMITTEXT, 64, 0);
    }
    if (page == page_display) w_display_create(window, list, status, apply);
    if (page == page_power) w_power_create(window, list, status, apply);
    w_measure_controls();
    w_size_controls(true);
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    if (page == page_audio) {
        w_sound_refresh(list, mixer);
        sound_refresh_due = GetTickCount64() + 5000;
        SetTimer(window, 2, 1000, NULL);
    }
    else if (page == page_wifi || page == page_network)
        w_open_network_page(page);
    else if (page == page_bluetooth)
        w_bluetooth_open(window, list, status, apply, lower);
    if (page == page_bluetooth)
        SendMessageW(list, LB_SETCURSEL, 0, 0);
    w_selected();
    SetFocus(list);
    MSG m;
    for (;;) {
        HANDLE inquiry = w_bluetooth_pending();
        if (inquiry) {
            DWORD wait = MsgWaitForMultipleObjectsEx(1, &inquiry, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (wait == WAIT_OBJECT_0) {
                w_bluetooth_finish();
                continue;
            }
            if (wait == WAIT_FAILED) break;
            if (!PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) continue;
            if (m.message == WM_QUIT) break;
        } else if (GetMessageW(&m, NULL, 0, 0) <= 0) break;
        if (m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE) {
            bool dropped = false;
            for (size_t i = 0; i < position_count && !dropped; ++i)
                dropped = positions[i].dropdown_height && SendMessageW(positions[i].control, CB_GETDROPPEDSTATE, 0, 0);
            if (dropped) DispatchMessageW(&m);
            else DestroyWindow(window);
            continue;
        }
        if (m.message == WM_KEYDOWN && m.wParam == VK_RETURN && m.hwnd == list) {
            w_use_selection();
            continue;
        }
        if (!IsDialogMessageW(window, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    w_sound_clear();
    free(adapters);
    w_bluetooth_close();
    if (wlan)
        wifi.close(wlan, NULL);
    DeleteObject(panel_font);
    CoUninitialize();
    return 0;
}
