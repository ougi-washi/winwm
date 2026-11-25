#define COBJMACROS
#include "theme/theme.h"
#include "desktop_tools/tools.h"
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
static HWND password, network_switch;
static w_adapter_problem network_problems[32];
static size_t problem_count;
static GUID plans[128];
static int plan_count;
static wchar_t adapter_details[128][256];
static int adapter_count;
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
    SetWindowTextW(password, L"");
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
                wcscpy(p->profile, n->strProfileName);
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
            continue;
        }
        restricted |= result == ERROR_ACCESS_DENIED;
        radio_off |= result == ERROR_NDIS_DOT11_POWER_STATE_INVALID;
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
    w_show_network_problems(true);
    if (scanning)
        SetTimer(window, 3, 4000, NULL);
    if (profile_count)
        SendMessageW(list, LB_SETCURSEL, 0, 0);
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
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_ALL_INTERFACES;
    IP_ADAPTER_ADDRESSES *all = NULL;
    ULONG result;
    while ((result = GetAdaptersAddresses(AF_UNSPEC, flags, NULL, all, &bytes)) == ERROR_BUFFER_OVERFLOW) {
        IP_ADAPTER_ADDRESSES *next = realloc(all, bytes);
        if (!next) break;
        all = next;
    }
    if (result == NO_ERROR) {
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
            swprintf(label, count_of(label), L"%.185ls   ·   %ls%ls", a->FriendlyName ? a->FriendlyName : L"Network adapter",
                     a->IfType == IF_TYPE_IEEE80211 ? L"Wi-Fi   ·   " : L"",
                     a->OperStatus == IfOperStatusUp ? L"Connected" : L"Disconnected");
            swprintf(adapter_details[adapter_count], count_of(adapter_details[0]), L"%ls   \u00b7   %llu Mbps",
                     address, a->OperStatus == IfOperStatusUp ? a->TransmitLinkSpeed / 1000000 : 0);
            w_item(label, adapter_count++);
        }
    }
    free(all);
    w_show_network_problems(false);
    w_message(adapter_count || problem_count ? L"Select an adapter for connection or driver details." : L"No network adapters found.");
}

static void w_selected(void) {
    LRESULT row = SendMessageW(list, LB_GETCURSEL, 0, 0);
    int id = row == LB_ERR ? -1 : (int)SendMessageW(list, LB_GETITEMDATA, row, 0);
    EnableWindow(apply, id >= 0);
    if (page == page_audio) w_audio_selection();
    if (page == page_bluetooth) w_bluetooth_selected();
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
    if ((page == page_wifi || page == page_network) && id <= -2 && (size_t)(-2-id) < problem_count)
        w_message(network_problems[-2-id].detail);
}

static void w_open_network_page(enum page next) {
    page = next;
    bool wireless = page == page_wifi;
    SetWindowTextW(window, wireless ? L"Wi-Fi networks" : L"Network adapters");
    SetWindowTextW(network_switch, wireless ? L"Network adapters" : L"Wi-Fi networks");
    SetWindowTextW(password, L"");
    ShowWindow(password, SW_HIDE);
    ShowWindow(apply, wireless ? SW_SHOWNA : SW_HIDE);
    KillTimer(window, 3);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    if (wireless) w_wifi_items(true);
    else w_network_items();
    if (SendMessageW(list, LB_GETCURSEL, 0, 0) == LB_ERR)
        SendMessageW(list, LB_SETCURSEL, 0, 0);
    w_selected();
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
    } else if (page == page_power && id >= 0 && id < plan_count) {
        w_message(PowerSetActiveScheme(NULL, &plans[id]) == ERROR_SUCCESS ? L"Power plan active." : L"Unable to change power plan.");
    } else if (page == page_bluetooth) {
        w_bluetooth_apply();
    } else if (page == page_display)
        w_display_preview();
}

static HFONT panel_font;
static struct { HWND control; RECT logical; } positions[32];
static size_t position_count;

static void w_measure_controls(void) {
    for (HWND control = GetWindow(window, GW_CHILD); control && position_count < count_of(positions);
         control = GetWindow(control, GW_HWNDNEXT)) {
        RECT r;
        GetWindowRect(control, &r);
        MapWindowPoints(NULL, window, (POINT *)&r, 2);
        positions[position_count].control = control;
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
    int dx = MulDiv(area.right, 96, dpi) - 544, dy = MulDiv(area.bottom, 96, dpi) - 344;
    for (size_t i = 0; i < position_count; ++i) {
        HWND c = positions[i].control;
        RECT r = positions[i].logical;
        if (c == list) r.bottom += dy;
        else if (r.top >= 248) OffsetRect(&r, 0, dy);
        if (c == list || (c == status && page != page_audio) || c == password || c == sound_slider ||
            (page == page_display && w_display_stretches(c))) r.right += dx;
        if (update_font) SendMessageW(c, WM_SETFONT, (WPARAM)panel_font, FALSE);
        MoveWindow(c, MulDiv(r.left,dpi,96), MulDiv(r.top,dpi,96),
                   MulDiv(r.right-r.left,dpi,96), MulDiv(r.bottom-r.top,dpi,96), TRUE);
    }
    if (old) DeleteObject(old);
}

static LRESULT CALLBACK w_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    LRESULT color_result;
    if (w_theme_message(w, msg, wp, lp, &color_result))
        return color_result;
    if (page == page_display && w_display_message(msg, wp, lp)) return 0;
    if (msg == WM_GETMINMAXINFO) {
        UINT dpi = GetDpiForWindow(w);
        RECT r = {0, 0, MulDiv(544,dpi,96), MulDiv(246,dpi,96)};
        AdjustWindowRectExForDpi(&r, GetWindowLongW(w, GWL_STYLE), FALSE, GetWindowLongW(w, GWL_EXSTYLE), dpi);
        ((MINMAXINFO *)lp)->ptMinTrackSize = (POINT){r.right-r.left, r.bottom-r.top};
        return 0;
    }
    if (msg == WM_DPICHANGED) {
        RECT *r = (RECT *)lp;
        SetWindowPos(w, NULL, r->left, r->top, r->right-r->left, r->bottom-r->top, SWP_NOZORDER);
        w_size_controls(true);
        return 0;
    }
    if (msg == WM_SIZE && wp != SIZE_MINIMIZED) {
        w_size_controls(false);
        return 0;
    }
    if (msg == WM_COMMAND) {
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
        password = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_TABSTOP | WS_BORDER | ES_PASSWORD | ES_AUTOHSCROLL,
                                 284, 300, 246, 30, window, NULL, inst, NULL);
        SendMessageW(password, EM_SETCUEBANNER, TRUE, (LPARAM)L"Wi-Fi password");
        SendMessageW(password, EM_SETLIMITTEXT, 64, 0);
    }
    if (page == page_display) w_display_create(window, list, status, apply);
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
    else if (page == page_power)
        w_power_items();
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
            DestroyWindow(window);
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
    w_bluetooth_close();
    if (wlan)
        wifi.close(wlan, NULL);
    DeleteObject(panel_font);
    CoUninitialize();
    return 0;
}
