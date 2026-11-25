#include "desktop_tools/adapter.h"
#include "desktop_tools/tools.h"
#include "theme/theme.h"
#include <commctrl.h>
#include <shellapi.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <wchar.h>

// Windows SDK DNS_INTERFACE_SETTINGS version 1, available on Windows 10 2004+.
typedef struct {
    ULONG version;
    ULONG64 flags;
    PWSTR domain, names, search;
    ULONG registration, register_name, llmnr, query_adapter;
    PWSTR profile_names;
} w_dns_settings;

typedef DWORD (WINAPI *w_dns_setter)(GUID, const w_dns_settings *);

static w_dns_setter w_dns_api(void) {
    HMODULE lib = GetModuleHandleW(L"iphlpapi.dll");
    return lib ? (w_dns_setter)GetProcAddress(lib, "SetInterfaceDnsSettings") : NULL;
}

static bool w_dns_normalize(int family, const wchar_t *input, wchar_t *out, size_t cap) {
    size_t used = 0;
    const wchar_t *separators = L",; \t\r\n";
    out[0] = 0;
    while (*(input += wcsspn(input, separators))) {
        size_t length = wcscspn(input, separators);
        wchar_t token[INET6_ADDRSTRLEN], normalized[INET6_ADDRSTRLEN];
        if (length >= count_of(token)) return false;
        wmemcpy(token, input, length);
        token[length] = 0;
        union { IN_ADDR v4; IN6_ADDR v6; } value;
        if (InetPtonW(family, token, &value) != 1 || !InetNtopW(family, &value, normalized, count_of(normalized)))
            return false;
        size_t n = wcslen(normalized);
        if (used + (used != 0) + n >= cap) return false;
        if (used) out[used++] = L',';
        wcscpy(out + used, normalized);
        used += n;
        input += length;
    }
    return true;
}

DWORD w_adapter_apply_dns(const wchar_t *request) {
    // Only a GUID, address family and numeric addresses cross the elevation boundary.
    if (wcslen(request) < 41 || (request[0] != L'4' && request[0] != L'6') || request[1] != L':' ||
        request[2] != L'{' || request[39] != L'}' || request[40] != L':') return ERROR_INVALID_PARAMETER;
    wchar_t id[39], servers[1024];
    wmemcpy(id, request + 2, 38);
    id[38] = 0;
    GUID guid;
    int family = request[0] == L'6' ? AF_INET6 : AF_INET;
    if (FAILED(CLSIDFromString(id, &guid)) || !w_dns_normalize(family, request + 41, servers, count_of(servers)))
        return ERROR_INVALID_PARAMETER;
    if (request[41] && !servers[0]) return ERROR_INVALID_PARAMETER;
    w_dns_setter set = w_dns_api();
    if (!set) return ERROR_NOT_SUPPORTED;
    w_dns_settings settings = {.version = 1, .flags = family == AF_INET6 ? 3 : 2, .names = servers};
    return set(guid, &settings);
}

typedef struct {
    const IP_ADAPTER_ADDRESSES *adapter;
    GUID id;
    HWND details, heading, family, entry, apply, automatic, status;
    HFONT font;
    int dpi;
    HANDLE updating;
} w_adapter_panel;

static void w_append(HWND edit, const wchar_t *text) {
    SendMessageW(edit, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
    SendMessageW(edit, EM_REPLACESEL, FALSE, (LPARAM)text);
}

static bool w_address(const SOCKET_ADDRESS *address, wchar_t *out, size_t cap) {
    if (!address->lpSockaddr) return false;
    SOCKADDR *sa = address->lpSockaddr;
    DWORD size = (DWORD)cap;
    return WSAAddressToStringW(sa, address->iSockaddrLength, NULL, out, &size) == 0;
}

static void w_details(w_adapter_panel *p) {
    const IP_ADAPTER_ADDRESSES *a = p->adapter;
    wchar_t value[320], ip[96];
    SetWindowTextW(p->details, a->Description && *a->Description ? a->Description : L"Network adapter");
    if (a->OperStatus == IfOperStatusUp && a->ReceiveLinkSpeed != UINT64_MAX && a->TransmitLinkSpeed != UINT64_MAX) {
        swprintf(value,count_of(value),L"\r\nLink speed    %llu Mbps receive / %llu Mbps send\r\n",
                 a->ReceiveLinkSpeed / 1000000, a->TransmitLinkSpeed / 1000000);
        w_append(p->details, value);
    } else w_append(p->details,L"\r\nLink speed unavailable\r\n");
    for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u; u = u->Next)
        if (w_address(&u->Address, ip, count_of(ip))) {
            swprintf(value,count_of(value),L"%ls    %ls/%u\r\n",u->Address.lpSockaddr->sa_family == AF_INET6 ?
                     L"IPv6" : L"IPv4",ip,(unsigned)u->OnLinkPrefixLength);
            w_append(p->details,value);
        }
    for (IP_ADAPTER_GATEWAY_ADDRESS *g = a->FirstGatewayAddress; g; g = g->Next)
        if (w_address(&g->Address, ip, count_of(ip))) {
            swprintf(value,count_of(value),L"Gateway    %ls\r\n",ip);
            w_append(p->details,value);
        }
    for (IP_ADAPTER_DNS_SERVER_ADDRESS *d = a->FirstDnsServerAddress; d; d = d->Next)
        if (w_address(&d->Address, ip, count_of(ip))) {
            swprintf(value,count_of(value),L"DNS    %ls\r\n",ip);
            w_append(p->details,value);
        }
    if (a->DnsSuffix && *a->DnsSuffix) { w_append(p->details,L"DNS suffix    "); w_append(p->details,a->DnsSuffix); }
    SendMessageW(p->details, EM_SETSEL, 0, 0);
    SendMessageW(p->details, EM_SCROLLCARET, 0, 0);
}

static void w_dns_result(w_adapter_panel *p, DWORD error) {
    SetWindowTextW(p->status, !error ? L"DNS settings saved. Refresh the adapter list for current addresses." :
        error == ERROR_CANCELLED ? L"Change cancelled." : error == ERROR_ACCESS_DENIED ? L"Windows denied access." :
        error == ERROR_NOT_SUPPORTED ? L"This Windows version cannot change interface DNS." :
        L"Windows could not change DNS for this adapter.");
}

static void w_save_dns(HWND w, w_adapter_panel *p, bool automatic) {
    wchar_t input[1024] = {0}, servers[1024], guid[40], request[1120];
    bool ipv6 = SendMessageW(p->family, CB_GETCURSEL, 0, 0) == 1;
    if (!automatic) GetWindowTextW(p->entry,input,count_of(input));
    if (!w_dns_normalize(ipv6 ? AF_INET6 : AF_INET, input, servers, count_of(servers)) || (!automatic && !*servers)) {
        SetWindowTextW(p->status, L"Enter one or more addresses of the selected IP version.");
        SetFocus(p->entry);
        return;
    }
    StringFromGUID2(&p->id, guid, count_of(guid));
    swprintf(request,count_of(request),L"%lc:%ls:%ls",ipv6 ? L'6' : L'4',guid,servers);
    DWORD error = w_adapter_apply_dns(request);
    if (error == ERROR_ACCESS_DENIED) {
        wchar_t exe[os_path_cap], arg[1160];
        swprintf(arg,count_of(arg),L"--tool-dns-%ls",request);
        SHELLEXECUTEINFOW run = {.cbSize=sizeof(run), .fMask=SEE_MASK_NOCLOSEPROCESS|SEE_MASK_NOASYNC|SEE_MASK_FLAG_NO_UI,
            .hwnd=w,.lpVerb=L"runas",.lpFile=exe,.lpParameters=arg,.nShow=SW_HIDE};
        if (!w_os_self(exe,count_of(exe))) { w_dns_result(p,ERROR_FILE_NOT_FOUND); return; }
        if (ShellExecuteExW(&run)) {
            p->updating = run.hProcess;
            EnableWindow(p->apply,FALSE); EnableWindow(p->automatic,FALSE);
            SetWindowTextW(p->status,L"Applying DNS settings…");
            if (!SetTimer(w,1,150,NULL)) {
                CloseHandle(p->updating);
                p->updating = NULL;
                SetWindowTextW(p->status,L"Refresh the adapter after Windows completes the DNS change.");
            }
            return;
        }
        error = GetLastError();
    }
    w_dns_result(p,error);
}

static void w_position_dns(HWND w, w_adapter_panel *p) {
    int dpi = (int)GetDpiForWindow(w);
    RECT client;
    GetClientRect(w,&client);
    int right = max(1,MulDiv(client.right,96,dpi)-14);
    int dy = MulDiv(client.bottom,96,dpi)-328;
    HWND children[] = {p->details,p->heading,p->family,p->entry,p->apply,p->automatic,p->status};
    RECT bounds[] = {{14,12,right,150+dy}, {14,162,right,186}, {14,193,123,330}, {134,191,right,221},
                     {14,235,min(159,right/2),268}, {min(168,right/2+8),235,min(338,right),268}, {14,281,right,320}};
    HFONT current = NULL;
    if (!p->font || p->dpi != dpi)
        current = CreateFontW(-MulDiv(15,dpi,96),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
                              0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
    for (size_t i=0;i<count_of(children);i++) {
        RECT r = bounds[i];
        if (children[i] != p->details) OffsetRect(&r,0,dy);
        MoveWindow(children[i],MulDiv(r.left,dpi,96),MulDiv(r.top,dpi,96),
                   MulDiv(max(1,r.right-r.left),dpi,96),MulDiv(max(1,r.bottom-r.top),dpi,96),TRUE);
        if (current) SendMessageW(children[i],WM_SETFONT,(WPARAM)current,TRUE);
    }
    if (current) {
        if (p->font) DeleteObject(p->font);
        p->font = current;
        p->dpi = dpi;
    }
}

static INT_PTR CALLBACK w_dns_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    w_adapter_panel *p = (w_adapter_panel *)GetWindowLongPtrW(w, DWLP_USER);
    if (msg == WM_INITDIALOG) {
        p = (w_adapter_panel *)lp;
        SetWindowLongPtrW(w, DWLP_USER, (LONG_PTR)p);
        const IP_ADAPTER_ADDRESSES *a=p->adapter;
        SetWindowTextW(w, a->FriendlyName ? a->FriendlyName : L"Adapter details");
        HINSTANCE inst=GetModuleHandleW(NULL);
        LRESULT ignored;
        w_theme_message(w,WM_CREATE,0,0,&ignored);
        DWORD child=WS_CHILD|WS_VISIBLE|WS_TABSTOP;
        p->details=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",child|ES_MULTILINE|ES_READONLY|WS_VSCROLL,
                                   0,0,0,0,w,NULL,inst,NULL);
        p->heading=CreateWindowW(L"STATIC",L"DNS servers",WS_CHILD|WS_VISIBLE,0,0,0,0,w,NULL,inst,NULL);
        p->family=CreateWindowW(L"COMBOBOX",L"DNS address family",child|CBS_DROPDOWNLIST,0,0,0,0,w,NULL,inst,NULL);
        p->entry=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",child|ES_AUTOHSCROLL,0,0,0,0,w,NULL,inst,NULL);
        p->apply=CreateWindowW(L"BUTTON",L"Apply DNS",child|BS_OWNERDRAW,0,0,0,0,w,(HMENU)303,inst,NULL);
        p->automatic=CreateWindowW(L"BUTTON",L"Use automatic DNS",child|BS_OWNERDRAW,0,0,0,0,w,(HMENU)304,inst,NULL);
        p->status=CreateWindowW(L"STATIC",L"",WS_CHILD|WS_VISIBLE,0,0,0,0,w,NULL,inst,NULL);
        SendMessageW(p->family,CB_ADDSTRING,0,(LPARAM)L"IPv4 DNS");
        SendMessageW(p->family,CB_ADDSTRING,0,(LPARAM)L"IPv6 DNS");
        SendMessageW(p->family,CB_SETCURSEL,0,0);
        SendMessageW(p->entry,EM_LIMITTEXT,1023,0);
        SendMessageW(p->entry,EM_SETCUEBANNER,TRUE,(LPARAM)L"DNS servers, separated by commas");
        SendMessageW(p->details,EM_LIMITTEXT,0,0);
        w_details(p);
        bool available = ConvertInterfaceLuidToGuid(&a->Luid,&p->id) == NO_ERROR && w_dns_api();
        EnableWindow(p->apply,available); EnableWindow(p->automatic,available);
        SetWindowTextW(p->status,available ? L"Separate addresses with commas. Windows may request administrator approval." :
                                            L"DNS editing is unavailable on this adapter or Windows version.");
        UINT dpi=GetDpiForWindow(w);
        RECT size={0,0,MulDiv(548,dpi,96),MulDiv(328,dpi,96)}, owner;
        AdjustWindowRectExForDpi(&size,GetWindowLongW(w,GWL_STYLE),FALSE,GetWindowLongW(w,GWL_EXSTYLE),dpi);
        GetWindowRect(GetParent(w),&owner);
        int width=size.right-size.left,height=size.bottom-size.top;
        LONG left=(owner.left+owner.right-width)/2, top=(owner.top+owner.bottom-height)/2;
        RECT bounds=w_tool_fit_to_monitor((RECT){left,top,left+width,top+height},
                                          MonitorFromWindow(GetParent(w),MONITOR_DEFAULTTONEAREST));
        SetWindowPos(w,NULL,bounds.left,bounds.top,bounds.right-bounds.left,bounds.bottom-bounds.top,SWP_NOZORDER);
        w_position_dns(w,p);
        SetFocus(p->family);
        return FALSE;
    }
    LRESULT result;
    if (w_theme_message(w,msg,wp,lp,&result)) {
        if (msg == WM_DRAWITEM || msg == WM_MEASUREITEM) { SetWindowLongPtrW(w,DWLP_MSGRESULT,result); return TRUE; }
        return result;
    }
    if (!p) return FALSE;
    if (msg == WM_SIZE && p->status && wp != SIZE_MINIMIZED) {
        w_position_dns(w,p);
        return TRUE;
    }
    if (msg == WM_COMMAND) {
        if (LOWORD(wp)==IDCANCEL) EndDialog(w,0);
        else if (LOWORD(wp)==303 || LOWORD(wp)==304 || (LOWORD(wp)==IDOK && GetFocus()==p->entry)) {
            if (!p->updating) w_save_dns(w,p,LOWORD(wp)==304);
        } else return FALSE;
        return TRUE;
    }
    if (msg == WM_TIMER && p->updating && WaitForSingleObject(p->updating,0)==WAIT_OBJECT_0) {
        DWORD code=ERROR_GEN_FAILURE;
        GetExitCodeProcess(p->updating,&code);
        CloseHandle(p->updating); p->updating=NULL;
        KillTimer(w,1);
        EnableWindow(p->apply,TRUE); EnableWindow(p->automatic,TRUE);
        w_dns_result(p,code);
        return TRUE;
    }
    if (msg == WM_DPICHANGED) {
        w_tool_resize_for_dpi(w,(RECT *)lp);
        w_position_dns(w,p); return TRUE;
    }
    return FALSE;
}

bool w_adapter_show(HWND owner, const IP_ADAPTER_ADDRESSES *adapter) {
    struct { DLGTEMPLATE dialog; WORD menu, window_class, title; } tmpl = {0};
    tmpl.dialog.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME;
    tmpl.dialog.dwExtendedStyle = WS_EX_TOOLWINDOW;
    w_adapter_panel p={.adapter=adapter};
    WSADATA data;
    bool sockets=WSAStartup(MAKEWORD(2,2),&data)==0;
    INT_PTR result = DialogBoxIndirectParamW(GetModuleHandleW(NULL),&tmpl.dialog,owner,w_dns_proc,(LPARAM)&p);
    if (sockets) WSACleanup();
    if (p.updating) CloseHandle(p.updating);
    if (p.font) DeleteObject(p.font);
    return result != -1;
}
