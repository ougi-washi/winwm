#define COBJMACROS
#include "theme/icon_button.h"
#include "theme/control_hover.h"
#include "theme/disclosure.h"
#include "theme/theme.h"
#include "theme/hover.h"
#include "desktop_tools/tools.h"
#include "desktop_tools/adapter.h"
#include "theme/dialog.h"
#include "desktop_tools/rows.h"
#include "desktop_tools/display.h"
#include "desktop_tools/sound.h"
#include "win_platform/percent_slider.h"
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
#include "win_platform/paint.h"
#include <windowsx.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
enum page { page_audio, page_wifi, page_bluetooth, page_display, page_power, page_network };

////////////////
//~ Settings

static enum page page;
static HWND window, list, apply, raise_button, status;
static w_tool_feedback settings_feedback;
static HWND password, network_switch, saved_action;
static w_disclosure key_disclosure;
static bool key_required;
static enum { inventory_waiting,inventory_compact,inventory_populated } frame_state;
static w_adapter_problem network_problems[32];
static size_t problem_count;
static IP_ADAPTER_ADDRESSES *adapters;
static enum w_sound_view sound_view;
static const wchar_t *sound_tabs[] = {L"Output", L"Input", L"App mixer"};
static HWND sound_slider, sound_tab, default_device, calls_device;
static ULONGLONG sound_refresh_due, sound_error_until;
static bool sound_watching;
static HANDLE wlan, interface_watch;
enum { adapter_changed = WM_APP + 81, inventory_timer = 3, network_tick = 4, bluetooth_tick = 5,
       password_tick = 6, bluetooth_change = 7 };
static struct w_wifi_profile {
    GUID adapter;
    wchar_t profile[WLAN_MAX_NAME_LENGTH];
    WLAN_AVAILABLE_NETWORK network;
} profiles[256];
static int profile_count;
static void w_selected(void);

static bool w_same_network(const struct w_wifi_profile *a, const struct w_wifi_profile *b) {
    if (!IsEqualGUID(&a->adapter, &b->adapter)) return false;
    // A saved profile keeps its identity when its SSID is out of scan range.
    if (*a->profile || *b->profile) return wcscmp(a->profile, b->profile) == 0;
    return a->network.dot11DefaultAuthAlgorithm == b->network.dot11DefaultAuthAlgorithm &&
           a->network.dot11Ssid.uSSIDLength == b->network.dot11Ssid.uSSIDLength &&
           !memcmp(a->network.dot11Ssid.ucSSID, b->network.dot11Ssid.ucSSID, a->network.dot11Ssid.uSSIDLength);
}

////////////////
//~ Settings Functions

static void w_message(const wchar_t *s) {
    w_tool_status(status,s);
}

static int w_current_item(void) {
    LRESULT row = SendMessageW(list, LB_GETCURSEL, 0, 0);
    return row == LB_ERR ? -1 : (int)SendMessageW(list, LB_GETITEMDATA, row, 0);
}

static void w_size_controls(bool update_font);

static bool w_inventory_empty(void) {
    return (page==page_audio || page==page_bluetooth || page==page_wifi || page==page_network) &&
           SendMessageW(list,LB_GETCOUNT,0,0)==0;
}

static bool w_inventory_state(bool settled) {
    bool empty=w_inventory_empty();
    int previous=frame_state;
    if (!empty) frame_state=inventory_populated;
    else if (settled && frame_state==inventory_waiting) frame_state=inventory_compact;
    return previous!=(int)frame_state || !!(GetWindowLongW(list,GWL_STYLE)&WS_VISIBLE)==empty;
}

static bool w_compact_inventory(void) {
    return frame_state==inventory_compact && w_tool_popup_monitor() && w_inventory_empty();
}

static HWND w_inventory_focus(void) {
    if (page==page_audio) return sound_tab;
    if (page==page_bluetooth) return raise_button;
    return network_switch;
}

static void w_reveal_password(void) {
    if (!password) return;
    bool visible = (GetWindowLongW(password,GWL_STYLE) & WS_VISIBLE) != 0;
    if (!key_disclosure.value) {
        if (visible) ShowWindow(password,SW_HIDE);
        return;
    }
    RECT bounds;
    GetWindowRect(password,&bounds);
    RECT part={0,0,MulDiv(bounds.right-bounds.left,key_disclosure.value,255),bounds.bottom-bounds.top};
    w_disclosure_clip(password,key_disclosure.value==255 ? NULL : &part);
    if (!visible) ShowWindow(password,SW_SHOWNA);
}

static void w_audio_selection(void) {
    int id = w_current_item(), percent = 0;
    BOOL muted = FALSE;
    bool ready = w_sound_state(id, &percent, &muted);
    EnableWindow(sound_slider, ready);
    EnableWindow(apply, ready);
    EnableWindow(default_device, w_sound_can_default(id,false));
    EnableWindow(calls_device, w_sound_can_default(id,true));
    w_percent_slider_sync(sound_slider,percent);
    if (sound_view == sound_inputs)
        w_icon_button(apply, muted ? L"\uEC54" : L"\uE720", muted ? L"Unmute microphone" : L"Mute microphone");
    else {
        const wchar_t *label = sound_view == sound_apps ? (muted ? L"Unmute application" : L"Mute application") :
                                                         (muted ? L"Unmute output" : L"Mute output");
        w_icon_button(apply, muted ? L"\uE74F" : L"\uE767", label);
    }
    wchar_t old[96];
    wchar_t level[64];
    swprintf(level, count_of(level), L"%ls   %d%%%ls", sound_view == sound_inputs ? L"Input" : L"Volume",
             percent, muted ? L"   ·   Muted" : L"");
    const wchar_t *text = ready ? level : id >= 0 ? L"Device unavailable" :
        SendMessageW(list,LB_GETCOUNT,0,0)>0 ? L"Select a device or app" :
        sound_view == sound_apps ? L"No audio sessions" : sound_view == sound_inputs ? L"No input device" : L"No output device";
    GetWindowTextW(status,old,count_of(old));
    if (GetTickCount64() >= sound_error_until && wcscmp(text,old)) w_message(text);
}

static void w_audio_error(const wchar_t *text) {
    sound_error_until = GetTickCount64()+4000;
    w_message(text);
}

static void w_audio_open_view(enum w_sound_view next) {
    if (next >= sound_view_count || next < sound_outputs) return;
    sound_view = next;
    sound_error_until = 0;
    if (next != sound_apps) {
        w_icon_button(default_device,L"\uE734",next == sound_inputs ? L"Set as default input device" : L"Set as default output device");
        w_icon_button(calls_device,L"\uE717",next == sound_inputs ? L"Use this microphone for calls" : L"Use this output for calls");
    }
    SetWindowTextW(list, next == sound_apps ? L"Applications" : next == sound_inputs ? L"Input devices" : L"Output devices");
    SetWindowTextW(sound_slider, next == sound_inputs ? L"Microphone level" : L"Volume");
    sound_watching = w_sound_watch();
    w_sound_refresh(list,next);
    sound_refresh_due = GetTickCount64()+5000;
    w_audio_selection();
    w_inventory_state(true);
    w_size_controls(false);
}

static bool w_sound_tab_rect(HWND tab,int item,RECT *bounds) {
    RECT native,client;
    return TabCtrl_GetItemRect(tab,item,&native) && GetClientRect(tab,&client) && IntersectRect(bounds,&native,&client);
}

static LRESULT CALLBACK w_sound_tab_proc(HWND tab, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
    static int hot=-1, down=-1;
    if (msg==WM_MOUSEMOVE || msg==WM_LBUTTONDOWN || msg==WM_LBUTTONDBLCLK) {
        TCHITTESTINFO hit={.pt={GET_X_LPARAM(lp),GET_Y_LPARAM(lp)}};
        int item=TabCtrl_HitTest(tab,&hit);
        RECT r;
        if (item>=0 && !w_sound_tab_rect(tab,item,&r)) item=-1;
        if (hot!=item) {
            if (hot<0 && item>=0) {
                TRACKMOUSEEVENT track={sizeof(track),TME_LEAVE,tab,0};
                TrackMouseEvent(&track);
            }
            hot=item;
        }
        w_hover_target(tab,item,item<0 ? NULL : &r);
        if (msg!=WM_MOUSEMOVE && hot>=0) {
            down=hot;
            InvalidateRect(tab,&r,FALSE);
        }
    } else if (msg==WM_MOUSELEAVE) {
        hot=-1;
        if (GetCapture()!=tab) down=-1;
        w_hover_target(tab,-1,NULL);
    } else if (msg==WM_LBUTTONUP || msg==WM_CANCELMODE || msg==WM_CAPTURECHANGED) {
        RECT r;
        if (down>=0 && w_sound_tab_rect(tab,down,&r)) InvalidateRect(tab,&r,FALSE);
        down=-1;
        if (msg==WM_CANCELMODE || (msg==WM_CAPTURECHANGED && lp)) {
            hot=-1;
            w_hover_target(tab,-1,NULL);
        }
    } else if (msg==WM_SIZE || (msg==WM_ENABLE && !wp) || (msg==WM_SHOWWINDOW && !wp)) {
        hot=down=-1;
        w_hover_clear(tab);
    }
    if (msg == WM_ERASEBKGND) return TRUE;
    if (msg == WM_PAINT || msg == WM_PRINTCLIENT || msg == WM_PRINT) {
        w_paint_frame frame;
        HDC dc=msg==WM_PAINT ? w_begin_frame(tab,&frame) : (HDC)wp;
        if (!dc) {
            if (msg==WM_PAINT) w_end_frame(tab,&frame);
            return 0;
        }
        int saved = SaveDC(dc);
        RECT r;
        GetClientRect(tab,&r);
        FillRect(dc,msg==WM_PAINT ? &frame.paint.rcPaint : &r,w_theme_brush());
        SelectObject(dc,(HFONT)SendMessageW(tab,WM_GETFONT,0,0));
        SetBkMode(dc,TRANSPARENT);
        int active = TabCtrl_GetCurSel(tab);
        UINT dpi=GetDpiForWindow(tab);
        for (int i=0; i<(int)count_of(sound_tabs); ++i) {
            if (!w_sound_tab_rect(tab,i,&r) || !RectVisible(dc,&r)) continue;
            InflateRect(&r,-1,-1);
            int amount=w_hover_amount(tab,i);
            bool pressed=down==i && hot==i;
            COLORREF ink=w_hover_row(dc,&r,i==active,amount,pressed);
            SetTextColor(dc,i==active || amount || pressed ? ink : w_theme_hint());
            w_theme_draw_fitted_line(dc,sound_tabs[i],-1,&r,DT_SINGLELINE|DT_VCENTER|DT_CENTER);
            if (i==active) {
                RECT accent = r;
                accent.top = accent.bottom-max(1,MulDiv(2,dpi,96));
                SetDCBrushColor(dc,w_theme_high_contrast() ? ink : w_theme_accent());
                FillRect(dc,&accent,GetStockObject(DC_BRUSH));
                if (GetFocus()==tab && !(SendMessageW(tab,WM_QUERYUISTATE,0,0)&UISF_HIDEFOCUS))
                    DrawFocusRect(dc,&r);
            }
        }
        RestoreDC(dc,saved);
        if (msg == WM_PAINT) w_end_frame(tab,&frame);
        return 0;
    }
    if (msg == WM_NCDESTROY) {
        hot=down=-1;
        RemoveWindowSubclass(tab,w_sound_tab_proc,id);
        if (data) BufferedPaintUnInit();
    }
    return DefSubclassProc(tab,msg,wp,lp);
}

// Some Windows editions omit WLAN AutoConfig. Resolve the system DLL only when Wi-Fi is opened.
#define WIFI_FUNCTIONS(X) \
    X(open, WlanOpenHandle) \
    X(close, WlanCloseHandle) \
    X(interfaces, WlanEnumInterfaces) \
    X(profiles, WlanGetProfileList) \
    X(available, WlanGetAvailableNetworkList) \
    X(scan, WlanScan) \
    X(disconnect, WlanDisconnect) \
    X(notify, WlanRegisterNotification) \
    X(connect, WlanConnect) \
    X(save, WlanSaveTemporaryProfile) \
    X(forget, WlanDeleteProfile) \
    X(query, WlanQueryInterface) \
    X(free_memory, WlanFreeMemory)

static struct w_wifi_api {
#define WIFI_MEMBER(field, api) __typeof__(&api) field;
    WIFI_FUNCTIONS(WIFI_MEMBER)
#undef WIFI_MEMBER
} wifi;

////////////////
//~ Wireless networks

static bool w_load_wifi(void) {
    if (wifi.open) return true;
    HMODULE lib = LoadLibraryExW(L"wlanapi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!lib) return false;
    struct w_wifi_api next;
#define WIFI_RESOLVE(field, api) { \
        FARPROC address = GetProcAddress(lib, #api); \
        _Static_assert(sizeof(next.field) == sizeof(address), "Win32 procedure pointer size"); \
        memcpy(&next.field, &address, sizeof(address)); \
    }
    WIFI_FUNCTIONS(WIFI_RESOLVE)
#undef WIFI_RESOLVE
    if (!next.open || !next.close || !next.interfaces || !next.profiles || !next.connect || !next.free_memory) {
        FreeLibrary(lib);
        return false;
    }
    wifi = next;
    return true;
}
#undef WIFI_FUNCTIONS

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

// Workers own their provider handles and buffers until a complete inventory
// reaches the UI. They can finish safely after the window has closed.
typedef struct {
    LONG refs;
    UINT generation;
    enum page page;
    bool scan, wifi_available, scanning;
    HANDLE wlan;
    IP_ADAPTER_ADDRESSES *adapters;
    w_adapter_problem problems[32];
    size_t problem_count;
    struct w_wifi_entry {
        struct w_wifi_profile profile;
        wchar_t label[440];
    } entries[256];
    int count;
    const wchar_t *status;
} w_network_snapshot;

static w_network_snapshot *inventory;
static HANDLE inventory_thread;
static UINT network_generation;
static bool network_again, scan_again;
static ULONGLONG network_feedback_until;

static void w_network_feedback(const wchar_t *text) {
    network_feedback_until = GetTickCount64()+4000;
    w_message(text);
}

static void w_release_inventory(w_network_snapshot *s) {
    if (s && !InterlockedDecrement(&s->refs)) {
        if (s->wlan) wifi.close(s->wlan,NULL);
        free(s->adapters);
        free(s);
    }
}

static void w_query_wifi(w_network_snapshot *s) {
    DWORD version = 0;
    DWORD error = s->wifi_available ? wifi.open(2,NULL,&version,&s->wlan) : ERROR_NOT_SUPPORTED;
    if (error != ERROR_SUCCESS) {
        s->status = error == ERROR_SERVICE_NOT_ACTIVE ? L"WLAN AutoConfig is stopped. Start the service to scan." :
                    error == ERROR_ACCESS_DENIED ? L"Windows denied access to Wi-Fi." : L"Windows Wi-Fi service unavailable.";
        return;
    }
    WLAN_INTERFACE_INFO_LIST *interfaces = NULL;
    if (wifi.interfaces(s->wlan, NULL, &interfaces) != ERROR_SUCCESS) {
        wifi.close(s->wlan,NULL);
        s->wlan = NULL;
        s->status = L"Windows could not list Wi-Fi adapters.";
        return;
    }
    bool restricted = false, radio_off = false, scanning = false;
    DWORD interface_count = interfaces->dwNumberOfItems;
    for (DWORD i = 0; i < interfaces->dwNumberOfItems; i++) {
        WLAN_INTERFACE_INFO *adapter = &interfaces->InterfaceInfo[i];
        if (s->scan && wifi.scan) {
            DWORD result = wifi.scan(s->wlan, &adapter->InterfaceGuid, NULL, NULL, NULL);
            scanning |= result == ERROR_SUCCESS;
            restricted |= result == ERROR_ACCESS_DENIED;
            radio_off |= result == ERROR_NDIS_DOT11_POWER_STATE_INVALID;
        }
        WLAN_AVAILABLE_NETWORK_LIST *nearby = NULL;
        DWORD result = wifi.available ? wifi.available(s->wlan, &adapter->InterfaceGuid, 0, NULL, &nearby) : ERROR_NOT_SUPPORTED;
        if (result == ERROR_SUCCESS) {
            for (DWORD j = 0; j < nearby->dwNumberOfItems && s->count < (int)count_of(s->entries); j++) {
                WLAN_AVAILABLE_NETWORK *n = &nearby->Network[j];
                if (!n->dot11Ssid.uSSIDLength || n->dot11Ssid.uSSIDLength > DOT11_SSID_MAX_LENGTH)
                    continue;
                struct w_wifi_entry *entry = &s->entries[s->count++];
                struct w_wifi_profile *p = &entry->profile;
                *p = (struct w_wifi_profile){.adapter = adapter->InterfaceGuid, .network = *n};
                if (n->dwFlags & WLAN_AVAILABLE_NETWORK_HAS_PROFILE) wcscpy(p->profile, n->strProfileName);
                wchar_t ssid[33] = {0};
                MultiByteToWideChar(CP_UTF8, 0, (LPCSTR)n->dot11Ssid.ucSSID, n->dot11Ssid.uSSIDLength, ssid, 32);
                swprintf(entry->label, count_of(entry->label), L"%ls   %lu%%   %ls%ls", *ssid ? ssid : L"Network", n->wlanSignalQuality,
                         n->dwFlags & WLAN_AVAILABLE_NETWORK_CONNECTED ? L"Connected" : *p->profile ? L"Saved" :
                         n->bSecurityEnabled ? L"Secured" : L"Open", n->bNetworkConnectable ? L"" : L"  (unavailable)");
                if (interface_count > 1) {
                    size_t used = wcslen(entry->label);
                    swprintf(entry->label + used, count_of(entry->label) - used, L"   ·   %.160ls", adapter->strInterfaceDescription);
                }
            }
            wifi.free_memory(nearby);
        }
        restricted |= result == ERROR_ACCESS_DENIED;
        radio_off |= result == ERROR_NDIS_DOT11_POWER_STATE_INVALID;
        WLAN_PROFILE_INFO_LIST *saved = NULL;
        if (wifi.profiles(s->wlan, &adapter->InterfaceGuid, NULL, &saved) != ERROR_SUCCESS)
            continue;
        for (DWORD j = 0; j < saved->dwNumberOfItems && s->count < (int)count_of(s->entries); j++) {
            const wchar_t *name = saved->ProfileInfo[j].strProfileName;
            bool listed = false;
            for (int k = 0; k < s->count; ++k)
                if (IsEqualGUID(&s->entries[k].profile.adapter, &adapter->InterfaceGuid) && !wcscmp(s->entries[k].profile.profile, name)) {
                    listed = true;
                    break;
                }
            if (listed) continue;
            struct w_wifi_entry *entry = &s->entries[s->count++];
            entry->profile.adapter = adapter->InterfaceGuid;
            wcscpy(entry->profile.profile,name);
            swprintf(entry->label, count_of(entry->label), L"%.250ls   ·   Saved%ls%.160ls", name, interface_count > 1 ? L"   ·   " : L"",
                     interface_count > 1 ? adapter->strInterfaceDescription : L"");
        }
        wifi.free_memory(saved);
    }
    wifi.free_memory(interfaces);
    s->scanning=scanning && !restricted && !radio_off;
    s->status = restricted ? L"Allow location access for desktop apps to scan. Saved profiles are shown."
              : radio_off ? L"The Wi-Fi radio is off. Turn it on to scan."
              : scanning ? L"Scanning for nearby networks…"
              : !interface_count ? L"No usable Wi-Fi adapter. Check the network hardware and its driver."
              : s->count ? L"Select a network." : L"No nearby Wi-Fi networks found.";
}

static const wchar_t *w_personal_auth(const WLAN_AVAILABLE_NETWORK *n) {
    switch (n->dot11DefaultAuthAlgorithm) {
    case DOT11_AUTH_ALGO_WPA_PSK: return L"WPAPSK";
    case DOT11_AUTH_ALGO_RSNA_PSK: return L"WPA2PSK";
    case DOT11_AUTH_ALGO_WPA3_SAE: return L"WPA3SAE";
    default: return NULL;
    }
}

static void w_request_network(bool scan);

static void w_saved_profile(void) {
    int id = w_current_item();
    if (id < 0 || id >= profile_count) return;
    struct w_wifi_profile p = profiles[id];
    DWORD error = ERROR_INVALID_STATE;
    if (*p.profile && wifi.forget) {
        wchar_t question[480];
        swprintf(question,count_of(question),L"Forget %.250ls? Its saved credentials will be removed from this adapter.",p.profile);
        if (w_dialog(window,question,L"Forget network",MB_YESNO|MB_DEFBUTTON2|MB_ICONQUESTION) != IDYES) return;
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
    w_request_network(false);
    w_network_feedback(error == ERROR_SUCCESS ? (*p.profile ? L"Saved network forgotten." : L"Network saved for your account.") :
              error == ERROR_ALREADY_EXISTS ? L"A profile with that name already exists." :
              error == ERROR_ACCESS_DENIED ? L"Windows denied permission to change this profile." :
                                             L"The profile or connection changed. Select the network and try again.");
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
            w_message(L"Enter the Wi-Fi password and press Enter to connect.");
            w_disclosure_stop(window,&key_disclosure,password_tick,true);
            w_reveal_password();
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
    w_network_feedback(error == ERROR_SUCCESS ? L"Connection requested." : L"Windows rejected the connection request.");
}


static IP_ADAPTER_ADDRESSES *w_adapter_at(int index) {
    if (index < 0) return NULL;
    IP_ADAPTER_ADDRESSES *a = adapters;
    while (index-- > 0 && a) a = a->Next;
    return a;
}

static DWORD WINAPI w_query_network(void *param) {
    w_network_snapshot *s = param;
    if (s->page == page_wifi) w_query_wifi(s);
    else if (w_adapter_read_all(&s->adapters) != NO_ERROR)
        s->status = L"Windows could not read the network adapters.";
    s->problem_count = w_network_problems(s->problems,count_of(s->problems),s->page == page_wifi);
    w_release_inventory(s);
    return 0;
}

static void w_request_network(bool scan) {
    if (inventory_thread) {
        network_again = true;
        scan_again |= scan;
        return;
    }
    inventory = calloc(1,sizeof(*inventory));
    if (inventory) {
        inventory->refs = 2;
        inventory->generation = network_generation;
        inventory->page = page;
        inventory->scan = scan;
        inventory->wifi_available = page == page_wifi && w_load_wifi();
        inventory_thread = CreateThread(NULL,0,w_query_network,inventory,0,NULL);
    }
    if (!inventory_thread) {
        free(inventory);
        inventory = NULL;
        w_message(L"Could not read the network configuration.");
        if (w_inventory_state(true)) w_size_controls(false);
    }
}

static int w_show_adapters(w_network_snapshot *s, w_tool_rows *items) {
    IP_ADAPTER_ADDRESSES *previous = w_adapter_at(w_current_item());
    ULONG64 previous_luid = previous ? previous->Luid.Value : 0;
    free(adapters);
    adapters = s->adapters;
    s->adapters = NULL;
    LRESULT select = -1;
    int i = 0;
    for (IP_ADAPTER_ADDRESSES *a = adapters; a; a = a->Next, ++i) {
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        wchar_t label[320];
        swprintf(label, count_of(label), L"%.185ls   ·   %ls%ls", a->FriendlyName ? a->FriendlyName : L"Network adapter",
                 a->IfType == IF_TYPE_IEEE80211 ? L"Wi-Fi   ·   " : L"",
                 a->OperStatus == IfOperStatusUp ? L"Connected" : L"Disconnected");
        int row=w_rows_put(items,label,i);
        if (previous_luid == a->Luid.Value || (select < 0 && a->OperStatus == IfOperStatusUp))
            select = row;
    }
    return (int)select;
}

static int w_show_wifi(w_network_snapshot *s,w_tool_rows *items) {
    int id = w_current_item();
    bool restore = id >= 0 && id < profile_count;
    struct w_wifi_profile previous = restore ? profiles[id] : (struct w_wifi_profile){0};
    if (!s->wlan && wlan) {
        wifi.close(wlan,NULL);
        wlan = NULL;
    } else if (!wlan && s->wlan) {
        wlan = s->wlan;
        s->wlan = NULL;
        if (wifi.notify) wifi.notify(wlan,WLAN_NOTIFICATION_SOURCE_ACM,TRUE,w_wireless_event,window,NULL,NULL);
    }
    profile_count = s->count;
    int selected = -1;
    for (int i=0; i<s->count; ++i) {
        profiles[i] = s->entries[i].profile;
        int row = w_rows_put(items,s->entries[i].label,i);
        if (restore && w_same_network(&previous,&profiles[i])) selected = row;
    }
    return selected;
}

static void w_finish_network(void) {
    CloseHandle(inventory_thread);
    inventory_thread = NULL;
    w_network_snapshot *s = inventory;
    inventory = NULL;
    bool current = s->generation == network_generation && IsWindowEnabled(window);
    bool cached_failure = s->status && ((page == page_network && !s->adapters && adapters) ||
                                        (page == page_wifi && !s->wlan && profile_count));
    if (current && cached_failure) {
        // Keep the selection and password draft across a failed inventory.
        w_message(s->status);
    } else if (current) {
        int old_item = w_current_item();
        wchar_t problem[192] = L"";
        if (old_item <= -2 && (size_t)(-2-old_item) < problem_count)
            wcscpy(problem,network_problems[-2-old_item].label);
        w_tool_rows items = w_rows_begin(list);
        int select = page == page_wifi ? w_show_wifi(s,&items) : w_show_adapters(s,&items);
        problem_count = s->problem_count;
        memcpy(network_problems,s->problems,problem_count*sizeof(network_problems[0]));
        for (size_t i=0; i<problem_count; ++i) {
            int row = w_rows_put(&items,network_problems[i].label,-2-(int)i);
            if (*problem && !wcscmp(problem,network_problems[i].label)) select = row;
        }
        w_rows_end(&items,select >= 0 ? select : 0,old_item == -1 ? w_rows_show_selection : items.top);
        if (GetTickCount64() >= network_feedback_until && !(page == page_network && w_current_item() >= 0))
            w_message(s->status ? s->status : items.written ? L"Select an adapter for addresses, DNS and link details." :
                                                             L"No network adapters found.");
        w_selected();
        if (w_inventory_state(!s->scanning)) w_size_controls(false);
        if (page == page_wifi && s->scan) SetTimer(window,inventory_timer,4000,NULL);
    }
    w_release_inventory(s);
    if (network_again) {
        bool scan = scan_again;
        network_again = scan_again = false;
        w_request_network(scan && page == page_wifi);
    }
}

static void w_selected(void) {
    // The display page owns Preview readiness while its mode list is being scanned.
    if (page == page_display) return;
    LRESULT row = SendMessageW(list, LB_GETCURSEL, 0, 0);
    int id = row == LB_ERR ? -1 : (int)SendMessageW(list, LB_GETITEMDATA, row, 0);
    if (page == page_audio) w_audio_selection();
    if (page == page_power) w_power_selected();
    if (page == page_bluetooth) w_bluetooth_selected();
    if (page == page_network && id >= 0) {
        IP_ADAPTER_ADDRESSES *a = w_adapter_at(id);
        if (a) {
            wchar_t hint[360];
            swprintf(hint,count_of(hint),L"%.230ls\r\nEnter or double-click for addresses and DNS.",
                     a->Description ? a->Description : L"Network adapter");
            w_message(hint);
        }
    }
    if (page == page_wifi) {
        static struct w_wifi_profile key_target;
        struct w_wifi_profile *p = id >= 0 && id < profile_count ? &profiles[id] : NULL;
        bool connected = p && (p->network.dwFlags & WLAN_AVAILABLE_NETWORK_CONNECTED);
        w_icon_button(apply,connected ? L"\uE8CD" : L"\uE701",connected ? L"Disconnect from network" : L"Connect to network");
        bool saved = p && *p->profile;
        w_icon_button(saved_action, saved ? L"\uE74D" : L"\uE74E", saved ? L"Forget saved network…" : L"Save network");
        EnableWindow(saved_action, p && (*p->profile ? wifi.forget != NULL : connected && wifi.save && wifi.query));
        bool needs_key = p && !connected && !*p->profile && p->network.bSecurityEnabled && w_personal_auth(&p->network);
        if (key_required && (!needs_key || !w_same_network(&key_target, p)))
            SetWindowTextW(password, L"");
        key_target = needs_key ? *p : (struct w_wifi_profile){0};
        if (needs_key != key_required) {
            key_required = needs_key;
            if (!needs_key && GetFocus()==password) SetFocus(list);
            EnableWindow(password,needs_key);
            w_disclosure_to(window,&key_disclosure,password_tick,needs_key);
            w_reveal_password();
        }
        EnableWindow(apply, p && (connected || *p->profile || p->network.bNetworkConnectable));
    }
    if ((page == page_wifi || page == page_network) && id <= -2 && (size_t)(-2-id) < problem_count)
        w_message(network_problems[-2-id].detail);
}

static void CALLBACK w_interface_changed(void *context, PMIB_IPINTERFACE_ROW row, MIB_NOTIFICATION_TYPE event) {
    (void)row; (void)event;
    PostMessageW((HWND)context,adapter_changed,0,0);
}

static void w_open_network_page(enum page next) {
    ++network_generation;
    page = next;
    bool wireless = page == page_wifi;
    if (!interface_watch && !wireless)
        NotifyIpInterfaceChange(AF_UNSPEC,w_interface_changed,window,FALSE,&interface_watch);
    SetWindowTextW(window, wireless ? L"Wi-Fi networks" : L"Network adapters");
    w_icon_button(network_switch, wireless ? L"\uE839" : L"\uE701",
                  wireless ? L"Show network adapters" : L"Show Wi-Fi networks");
    key_required = false;
    w_disclosure_stop(window,&key_disclosure,password_tick,false);
    SetWindowTextW(password, L"");
    w_reveal_password();
    if (wireless) w_icon_button(apply,L"\uE701",L"Connect to network");
    KillTimer(window, inventory_timer);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    KillTimer(window,network_tick);
    SetTimer(window,network_tick,wireless || interface_watch ? 30000 : 10000,NULL);
    profile_count = 0;
    problem_count = 0;
    network_feedback_until = 0;
    w_message(wireless ? L"Reading Wi-Fi networks…" : L"Reading network adapters…");
    w_request_network(wireless);
    w_selected();
    w_size_controls(false);
}


static void w_use_selection(bool from_list) {
    LRESULT row = SendMessageW(list, LB_GETCURSEL, 0, 0);
    if (row == LB_ERR)
        return;
    int id = (int)SendMessageW(list, LB_GETITEMDATA, row, 0);
    if (page == page_audio) {
        bool ok = w_sound_change(id, -1);
        w_audio_selection();
        if (!ok) w_audio_error(L"Mute change failed");
    } else if (page == page_wifi && id >= 0 && id < profile_count && IsWindowEnabled(apply)) {
        if (profiles[id].network.dwFlags & WLAN_AVAILABLE_NETWORK_CONNECTED) {
            if (wifi.disconnect && wifi.disconnect(wlan, &profiles[id].adapter, NULL) == ERROR_SUCCESS) {
                w_request_network(false);
                w_network_feedback(L"Disconnect requested.");
            } else w_message(L"Windows could not disconnect this adapter.");
        } else w_join(&profiles[id]);
    } else if (page == page_network) {
        IP_ADAPTER_ADDRESSES *a = w_adapter_at(id);
        if (a && w_adapter_show(window, a)) w_request_network(false);
    } else if (page == page_power) {
        if (from_list) w_power_enter(list);
        else w_power_save();
    } else if (page == page_bluetooth) {
        w_bluetooth_apply();
    } else if (page == page_display)
        w_display_preview();
}

static HFONT panel_font;

static int w_settings_status_height(UINT dpi,int width) {
    // Both network views share one frame. The adapter details use the Wi-Fi footer's gap.
    int resting=(page==page_audio ? 1 : 2)*w_theme_role_line_height(dpi,text_body);
    int actual=w_tool_feedback_height(status,&settings_feedback,width);
    return max(resting,actual)+(page==page_network ? w_theme_spacing(dpi) : 0);
}

static int w_empty_feedback(UINT dpi,int width) {
    int side=w_theme_button_size(dpi),gap=w_theme_spacing(dpi);
    int text_width=page==page_audio ? width : width-side-gap;
    int height=w_tool_feedback_height(status,&settings_feedback,text_width);
    return max(height,w_theme_role_line_height(dpi,text_body));
}

static int w_popup_rows(void);
static int w_settings_height(UINT dpi,int rows,int width);

static void w_place_settings_list(int x,int y,int width,int height,int side) {
    RECT before={0}, after={0}, item;
    GetClientRect(list,&before);
    int row_height=(int)SendMessageW(list,LB_GETITEMHEIGHT,0,0);
    int top=(int)SendMessageW(list,LB_GETTOPINDEX,0,0);
    int selected=(int)SendMessageW(list,LB_GETCURSEL,0,0);
    bool visible=selected>=0 && SendMessageW(list,LB_GETITEMRECT,selected,(LPARAM)&item)!=LB_ERR &&
                 item.bottom>0 && item.top<before.bottom;
    if (row_height!=side) SendMessageW(list,LB_SETITEMHEIGHT,0,side);
    w_tool_place(list,x,y,width,height);
    GetClientRect(list,&after);
    if (top>=0 && (row_height!=side || before.bottom!=after.bottom)) {
        if (visible) top=w_rows_top_showing(selected,top,after.bottom,side);
        if (top!=SendMessageW(list,LB_GETTOPINDEX,0,0)) SendMessageW(list,LB_SETTOPINDEX,top,0);
    }
}

static void w_size_controls(bool update_font) {
    if (!list || !apply || !status) return;
    UINT dpi = GetDpiForWindow(window);
    int gap = w_theme_spacing(dpi), pad = 2*gap, side = w_theme_button_size(dpi);
    if (update_font) {
        HFONT next = w_theme_message_font(dpi);
        if (next) {
            for (HWND child=GetWindow(window,GW_CHILD); child; child=GetWindow(child,GW_HWNDNEXT))
                SendMessageW(child,WM_SETFONT,(WPARAM)next,FALSE);
            DeleteObject(panel_font);
            panel_font=next;
        }
    }
    RECT client;
    w_theme_content_rect(window,&client);
    int width = max(1,client.right-2*pad);
    bool attached=w_tool_popup_monitor()!=NULL;
    int required=w_settings_height(dpi,attached ? w_popup_rows() : 1,width);
    if (required!=client.bottom && (attached || required>client.bottom)) {
        w_tool_fit_height(window,required-client.bottom,false);
        w_theme_content_rect(window,&client);
        width=max(1,client.right-2*pad);
    }
    int right = pad+width, footer = client.bottom-pad-side;
    int first = page == page_bluetooth ? pad : pad+side+gap;
    int info_bottom=page==page_network ? client.bottom-pad : footer-gap;
    bool empty=w_inventory_empty(),compact=w_compact_inventory();
    int info_height=compact ? 0 : w_settings_status_height(dpi,width);
    info_height=min(info_height,max(1,info_bottom-first-gap-side));
    int info=info_bottom-info_height;
    if (empty) {
        HWND focus=GetFocus();
        if (focus && (focus==list || focus==apply || focus==password || focus==saved_action ||
                      focus==default_device || focus==calls_device || focus==sound_slider))
            SetFocus(w_inventory_focus());
    }
    w_tool_visible(list,!empty);
    w_tool_visible(apply,!empty && page!=page_network);
    w_place_settings_list(pad,first,width,compact ? w_popup_rows()*side : info-gap-first,side);
    if (compact) {
        int feedback=w_empty_feedback(dpi,width);
        if (page==page_audio) w_tool_place(status,pad,pad+side+gap,width,feedback);
        else {
            int x=page==page_bluetooth ? pad : pad+side+gap;
            w_tool_place(status,x,pad+max(0,(side-feedback)/2),width-side-gap,feedback);
        }
    } else w_tool_place(status,pad,empty ? first : info,width,empty ? max(info_height,footer-first-gap) : info_height);
    w_tool_place(apply,pad,footer,side,side);
    switch (page) {
    case page_audio: {
        int count = sound_view == sound_apps ? 0 : 2;
        w_tool_visible(sound_slider,!empty);
        w_tool_visible(default_device,!empty && count);
        w_tool_visible(calls_device,!empty && count);
        w_tool_place(sound_tab,pad,pad,width,side);
        int tab_width=max(1,(width-max(4,MulDiv(4,dpi,96)))/(int)count_of(sound_tabs));
        TabCtrl_SetItemSize(sound_tab,tab_width,side);
        w_tool_place(sound_slider,pad+side+gap,footer,width-(count+1)*(side+gap),side);
        w_tool_place(default_device,right-2*side-gap,footer,side,side);
        w_tool_place(calls_device,right-side,footer,side,side);
        break;
    }
    case page_network:
    case page_wifi:
        w_tool_visible(saved_action,page==page_wifi && !empty);
        w_tool_place(network_switch,pad,compact ? max(pad,(client.bottom-side)/2) : pad,side,side);
        w_tool_place(saved_action,pad+side+gap,pad,side,side);
        w_tool_place(password,pad+side+gap,footer,width-side-gap,side);
        w_reveal_password();
        break;
    case page_bluetooth:
        w_tool_place(raise_button,right-side,compact ? max(pad,(client.bottom-side)/2) : footer,side,side);
        break;
    case page_display:
        w_display_layout(pad,width,footer,side,gap,update_font);
        break;
    case page_power:
        w_power_layout(pad,width,footer,side,gap);
        break;
    }
}

// Attached hardware panels keep a stable viewport as inventories arrive or change.
static int w_popup_rows(void) {
    switch (page) {
    case page_audio: case page_bluetooth: return 2;
    case page_power: case page_network: return 4;
    default: return 3;
    }
}

static int w_settings_height(UINT dpi,int rows,int width) {
    if (w_compact_inventory()) {
        int side=w_theme_button_size(dpi),gap=w_theme_spacing(dpi),feedback=w_empty_feedback(dpi,width);
        return 4*gap+(page==page_audio ? side+gap+feedback : max(side,feedback));
    }
    int bands=(page!=page_bluetooth)+(page!=page_network);
    return (rows+bands)*w_theme_button_size(dpi)+(5+bands)*w_theme_spacing(dpi)+
           w_settings_status_height(dpi,width);
}

static LRESULT CALLBACK w_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    LRESULT color_result;
    if (w_theme_message(w, msg, wp, lp, &color_result))
        return color_result;
    if (msg == WM_NOTIFY && page == page_display && ((NMHDR *)lp)->code == NM_CUSTOMDRAW &&
        w_display_slider(((NMHDR *)lp)->hwndFrom))
        return w_percent_slider_draw(((NMHDR *)lp)->hwndFrom,(NMCUSTOMDRAW *)lp,w_theme_background(),w_theme_separator(),
                                     w_theme_accent(),w_theme_foreground(),w_native_slider_feedback(((NMHDR *)lp)->hwndFrom));
    if (page == page_display && w_display_message(msg, wp, lp)) return 0;
    if (page == page_power && w_power_message(msg, wp, lp)) return 0;
    if (msg == WM_NOTIFY && sound_slider && ((NMHDR *)lp)->hwndFrom == sound_slider && ((NMHDR *)lp)->code == NM_CUSTOMDRAW)
        return w_percent_slider_draw(sound_slider,(NMCUSTOMDRAW *)lp,w_theme_background(),w_theme_separator(),
                                     w_theme_accent(),w_theme_foreground(),w_native_slider_feedback(sound_slider));
    if (msg == WM_NOTIFY && sound_tab && ((NMHDR *)lp)->hwndFrom == sound_tab && ((NMHDR *)lp)->code == TCN_SELCHANGE) {
        w_audio_open_view(TabCtrl_GetCurSel(sound_tab));
        return 0;
    }
    if (msg == WM_GETMINMAXINFO) {
        UINT dpi = GetDpiForWindow(w);
        bool attached=w_tool_popup_monitor()!=NULL;
        MINMAXINFO *limits=(MINMAXINFO *)lp;
        RECT client;
        w_theme_content_rect(w,&client);
        w_tool_minimum(w,limits,MulDiv(356,dpi,96),
                       w_settings_height(dpi,attached ? w_popup_rows() : 1,client.right-4*w_theme_spacing(dpi)));
        if (attached) limits->ptMaxTrackSize.y=limits->ptMinTrackSize.y;
        return 0;
    }
    if (msg==w_tool_feedback_changed) { w_size_controls(false); return 0; }
    if (msg == WM_TIMER && wp == password_tick) {
        w_disclosure_tick(w,&key_disclosure,password_tick);
        w_reveal_password();
        return 0;
    }
    if (key_disclosure.running && (msg == winwm_motion_updated ||
        (msg == WM_SETTINGCHANGE && (wp == SPI_SETCLIENTAREAANIMATION || wp == SPI_SETHIGHCONTRAST)))) {
        w_disclosure_to(w,&key_disclosure,password_tick,key_required);
        w_reveal_password();
    }
    if (w_tool_font_change(w,msg,wp,lp)) {
        w_size_controls(true);
        return 0;
    }
    if (msg == WM_SIZE && wp != SIZE_MINIMIZED) {
        w_size_controls(false);
        return 0;
    }
    if ((msg == adapter_changed && page == page_network) ||
        ((msg == wifi_result || msg == wifi_scan_done) && page == page_wifi)) {
        SetTimer(w,inventory_timer,200,NULL);
        if (msg == wifi_result)
            w_network_feedback(wp == WLAN_REASON_CODE_SUCCESS ? L"Connected." : L"Connection failed. Check the password and network availability.");
        return 0;
    }
    if (page == page_bluetooth && msg == WM_DEVICECHANGE) {
        SetTimer(w,bluetooth_change,200,NULL);
        return 0;
    }
    if (page == page_bluetooth && msg == WM_TIMER && (wp == bluetooth_tick || wp == bluetooth_change)) {
        if (wp == bluetooth_change) KillTimer(w,bluetooth_change);
        w_bluetooth_poll(wp == bluetooth_change);
        return 0;
    }
    if (msg == WM_TIMER && (wp == inventory_timer || wp == network_tick)) {
        if (wp == inventory_timer) KillTimer(w,inventory_timer);
        if ((page == page_wifi || page == page_network) && IsWindowVisible(w) && !IsIconic(w) && IsWindowEnabled(w))
            w_request_network(page == page_wifi && wp == network_tick);
        return 0;
    }
    if (msg == WM_COMMAND) {
        if (page == page_wifi && saved_action && (HWND)lp == saved_action) {
            w_saved_profile();
            return 0;
        }
        if (network_switch && (HWND)lp == network_switch) {
            w_open_network_page(page == page_wifi ? page_network : page_wifi);
            SetFocus(w_inventory_empty() ? w_inventory_focus() : list);
            return 0;
        }
        if ((HWND)lp == list && HIWORD(wp) == LBN_SELCHANGE)
            w_selected();
        if (((HWND)lp == apply && HIWORD(wp) == BN_CLICKED) || ((HWND)lp == list && HIWORD(wp) == LBN_DBLCLK))
            w_use_selection((HWND)lp == list);
        if (page == page_bluetooth && LOWORD(wp) == 105 &&
            !w_os_launch_provider("%SystemRoot%\\System32\\DevicePairingWizard.exe", NULL, false))
            w_message(L"Windows discovery for additional devices is unavailable.");
        if (page == page_audio && ((HWND)lp == default_device || (HWND)lp == calls_device)) {
            bool ok = w_sound_make_default(w_current_item(), (HWND)lp == calls_device);
            w_audio_open_view(sound_view);
            if (!ok) w_audio_error(L"Default change failed");
        }
        return 0;
    }
    if (msg == WM_HSCROLL && sound_slider && (HWND)lp == sound_slider) {
        if (LOWORD(wp) == TB_ENDTRACK) return 0;
        bool ok = w_sound_change(w_current_item(), (int)SendMessageW(sound_slider, TBM_GETPOS, 0, 0));
        w_audio_selection();
        if (!ok) w_audio_error(L"Volume change failed");
        return 0;
    }
    if (msg == WM_TIMER && wp == 2 && page == page_audio) {
        if (!GetCapture() && IsWindowVisible(w) && !IsIconic(w)) {
            if (w_sound_inventory_changed() || ((sound_view == sound_apps || !sound_watching) && GetTickCount64() >= sound_refresh_due)) {
                if (!sound_watching) sound_watching = w_sound_watch();
                w_sound_refresh(list, sound_view);
                if (w_inventory_state(true)) w_size_controls(false);
                sound_refresh_due = GetTickCount64() + 5000;
            }
            w_audio_selection();
        }
        return 0;
    }
    if (msg == WM_DESTROY) {
        w_disclosure_stop(w,&key_disclosure,password_tick,false);
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
    sound_view = !wcscmp(name,L"volume_mixer") ? sound_apps : !wcscmp(name,L"microphone") ? sound_inputs : sound_outputs;
    INITCOMMONCONTROLSEX common = {sizeof(common), ICC_BAR_CLASSES | ICC_TAB_CLASSES};
    InitCommonControlsEx(&common);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSW wc = {.lpfnWndProc = w_proc,
                    .hInstance = inst,
                    .lpszClassName = L"WinwmSettings",
                    .hCursor = LoadCursorW(NULL, IDC_ARROW),
                    .hbrBackground = NULL};
    const wchar_t *titles[] = {L"Sound", L"Wi-Fi networks", L"Bluetooth devices", L"Display settings",
                               L"Power plans", L"Network adapters"};
    RegisterClassW(&wc);
    MONITORINFO monitor = {.cbSize = sizeof(monitor)};
    UINT dpi = w_tool_work_area(&monitor);
    DWORD style = WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MAXIMIZEBOX | WS_CLIPCHILDREN;
    bool radio_page = page == page_bluetooth || page == page_wifi || page == page_network;
    bool compact = radio_page && w_tool_popup_monitor();
    int content_width=MulDiv(compact ? 472 : 544,dpi,96);
    int preferred_height=w_settings_height(dpi,w_popup_rows()+(w_tool_popup_monitor() ? 0 : 3),
                                           content_width-4*w_theme_spacing(dpi));
    RECT bounds = {0,0,content_width,preferred_height};
    w_theme_adjust_rect(&bounds, style, WS_EX_TOOLWINDOW, dpi);
    int width = min(bounds.right - bounds.left, monitor.rcWork.right - monitor.rcWork.left);
    int height = min(bounds.bottom - bounds.top, monitor.rcWork.bottom - monitor.rcWork.top);
    window = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, titles[page], style,
                            (monitor.rcWork.left + monitor.rcWork.right - width) / 2,
                            (monitor.rcWork.top + monitor.rcWork.bottom - height) / 2,
                            width, height, NULL, NULL, inst, NULL);
    if (!window)
        return 1;
    list = CreateWindowExW(0, L"LISTBOX", L"",
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_HASSTRINGS |
                           LBS_OWNERDRAWFIXED | LBS_NOINTEGRALHEIGHT, 12, 10, 520, 238,
                           window, NULL, inst, NULL);
    const wchar_t *initial_status=page!=page_audio ? L"" :
        sound_view==sound_apps ? L"Reading audio sessions…" : L"Reading audio devices…";
    status = CreateWindowW(L"STATIC",initial_status,WS_CHILD|WS_VISIBLE|SS_EDITCONTROL|SS_NOPREFIX,12,258,520,34,window,NULL,inst,NULL);
    w_tool_track_feedback(status,&settings_feedback);
    const wchar_t *labels[] = {L"Mute / unmute", L"Connect", L"Device properties", L"Preview", L"Activate", L"Details"};
    apply = CreateWindowW(L"BUTTON", labels[page],
                          BS_OWNERDRAW | WS_CHILD | WS_TABSTOP | (page == page_network ? 0 : WS_VISIBLE),
                          12, 300, 140, 30, window, (HMENU)101, inst, NULL);
    if (page == page_audio) {
        sound_tab = CreateWindowW(WC_TABCONTROLW,L"Sound controls",WS_CHILD|WS_VISIBLE|WS_TABSTOP|
                                  TCS_FIXEDWIDTH|TCS_FOCUSONBUTTONDOWN,
                                  12,10,478,32,window,NULL,inst,NULL);
        bool buffered=SUCCEEDED(BufferedPaintInit());
        if (!SetWindowSubclass(sound_tab,w_sound_tab_proc,1,buffered) && buffered) BufferedPaintUnInit();
        for (size_t i=0; i<count_of(sound_tabs); ++i) {
            TCITEMW item = {.mask=TCIF_TEXT,.pszText=(wchar_t *)sound_tabs[i]};
            TabCtrl_InsertItem(sound_tab,(int)i,&item);
        }
        TabCtrl_SetCurSel(sound_tab,sound_view);
        sound_slider = CreateWindowW(TRACKBAR_CLASSW, L"Volume", WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_NOTICKS,
                                     58, 298, 382, 34, window, NULL, inst, NULL);
        w_percent_slider_init(sound_slider,slider_live);
        default_device = CreateWindowW(L"BUTTON",L"Set as default",BS_OWNERDRAW|WS_CHILD|WS_TABSTOP,
                                       450,298,36,34,window,(HMENU)107,inst,NULL);
        calls_device = CreateWindowW(L"BUTTON",L"Use for calls",BS_OWNERDRAW|WS_CHILD|WS_TABSTOP,
                                     496,298,36,34,window,(HMENU)110,inst,NULL);
        // Match keyboard traversal to the visible rows.
        UINT position_flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;
        SetWindowPos(sound_tab,HWND_TOP,0,0,0,0,position_flags);
        SetWindowPos(sound_slider,apply,0,0,0,0,position_flags);
    }
    if (page == page_bluetooth) {
        raise_button = CreateWindowW(L"BUTTON", L"More devices…", BS_OWNERDRAW | WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                     204, 300, 36, 30, window, (HMENU)105, inst, NULL);
        w_icon_button(raise_button,L"\uE836",L"Open Windows device pairing");
    }
    if (page == page_wifi || page == page_network) {
        network_switch = CreateWindowW(L"BUTTON", L"", BS_OWNERDRAW | WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                       12, 10, 36, 30, window, (HMENU)108, inst, NULL);
        saved_action = CreateWindowW(L"BUTTON", L"Save network", BS_OWNERDRAW | WS_CHILD | WS_TABSTOP,
                                     56, 10, 36, 30, window, (HMENU)109, inst, NULL);
        password = w_native_field(window,0,ES_PASSWORD);
        SendMessageW(password, EM_SETCUEBANNER, TRUE, (LPARAM)L"Wi-Fi password");
        SendMessageW(password, EM_SETLIMITTEXT, 64, 0);
    }
    if (page == page_display) w_display_create(window, list, status, apply);
    if (page == page_power) w_power_create(window, list, status, apply);
    w_size_controls(true);
    if (w_tool_show(window)) {
        UpdateWindow(window);
        if (page == page_audio) {
            w_audio_open_view(sound_view);
            SetTimer(window, 2, 1000, NULL);
        } else if (page == page_wifi || page == page_network)
            w_open_network_page(page);
        else if (page == page_display) w_display_start();
        else if (page == page_power) w_power_start();
        else if (page == page_bluetooth) {
            w_bluetooth_open(window, list, status, apply);
            if (!w_bluetooth_pending() && w_inventory_state(true)) w_size_controls(false);
            SetTimer(window,bluetooth_tick,10000,NULL);
            SendMessageW(list, LB_SETCURSEL, 0, 0);
        }
        w_selected();
        SetFocus(w_inventory_empty() ? w_inventory_focus() : list);
    }
    MSG m;
    for (;;) {
        HANDLE jobs[2];
        int count = 0;
        if (page == page_display) count = w_display_pending(jobs);
        else {
            jobs[0] = page == page_power ? w_power_pending() : page == page_bluetooth ? w_bluetooth_pending() : inventory_thread;
            count = jobs[0] != NULL;
        }
        if (count) {
            DWORD wait = MsgWaitForMultipleObjectsEx(count,jobs,INFINITE,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
            if (wait < WAIT_OBJECT_0+(DWORD)count) {
                if (page == page_display) w_display_finish(jobs[wait-WAIT_OBJECT_0]);
                else if (page == page_power) w_power_finish();
                else if (page == page_wifi || page == page_network) {
                    w_finish_network();
                }
                else {
                    bool changed=w_bluetooth_finish();
                    if (w_inventory_state(!w_bluetooth_pending()) || changed) w_size_controls(false);
                }
                continue;
            }
            if (wait == WAIT_FAILED) break;
            if (!PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) continue;
            if (m.message == WM_QUIT) break;
        } else if (GetMessageW(&m, NULL, 0, 0) <= 0) break;
        if (w_native_ime_key(&m)) { TranslateMessage(&m); DispatchMessageW(&m); continue; }
        if (w_tool_repeated_dialog_key(&m)) continue;
        if (m.message == WM_KEYDOWN && page == page_display && w_display_key(m.hwnd,m.wParam)) continue;
        if (m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE) {
            bool dropped = false;
            HWND focus = GetFocus();
            wchar_t kind[32];
            if (focus && GetClassNameW(focus,kind,count_of(kind))) {
                if (!_wcsicmp(kind,L"Edit")) focus=GetParent(focus);
                dropped = focus && GetClassNameW(focus,kind,count_of(kind)) && !_wcsicmp(kind,L"ComboBox") &&
                          SendMessageW(focus,CB_GETDROPPEDSTATE,0,0);
            }
            if (dropped) DispatchMessageW(&m);
            else SendMessageW(window,WM_CLOSE,0,0);
            continue;
        }
        if (m.message == WM_KEYDOWN && m.wParam == VK_RETURN) {
            if (m.hwnd == list || (page == page_wifi && key_required && m.hwnd == password)) {
                w_use_selection(m.hwnd == list);
                continue;
            }
            if (page == page_power && w_power_enter(m.hwnd)) continue;
        }
        if (!IsDialogMessageW(window, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    w_sound_stop();
    if (interface_watch) CancelMibChangeNotify2(interface_watch);
    if (inventory_thread) CloseHandle(inventory_thread);
    w_release_inventory(inventory);
    free(adapters);
    w_bluetooth_close();
    if (wlan)
        wifi.close(wlan, NULL);
    DeleteObject(panel_font);
    CoUninitialize();
    return 0;
}
