#include "desktop_tools/adapter.h"
#include "desktop_tools/tools.h"
#include "theme/theme.h"
#include "theme/icon_button.h"
#include <commctrl.h>
#include <shellapi.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <wchar.h>

DWORD w_adapter_read_all(IP_ADAPTER_ADDRESSES **out) {
    *out = NULL;
    ULONG bytes = 16 * 1024;
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_INCLUDE_ALL_INTERFACES;
    IP_ADAPTER_ADDRESSES *all = NULL;
    DWORD result = ERROR_NOT_ENOUGH_MEMORY;
    for (int attempt=0; attempt<4; ++attempt) {
        IP_ADAPTER_ADDRESSES *next = realloc(all, bytes);
        if (!next) { result = ERROR_NOT_ENOUGH_MEMORY; break; }
        all = next;
        result = GetAdaptersAddresses(AF_UNSPEC, flags, NULL, all, &bytes);
        if (result != ERROR_BUFFER_OVERFLOW) break;
    }
    if (!result) *out = all;
    else free(all);
    return result == ERROR_NO_DATA ? NO_ERROR : result;
}

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
    return lib ? (w_dns_setter)(void *)GetProcAddress(lib, "SetInterfaceDnsSettings") : NULL;
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
    LONG refs;
    IP_ADAPTER_ADDRESSES *all;
    DWORD error;
} w_adapter_read;

typedef struct {
    const IP_ADAPTER_ADDRESSES *adapter;
    GUID id;
    HWND details, heading, family, entry, apply, automatic, status;
    w_tool_choice family_choice;
    wchar_t servers[2][1024];
    HFONT font;
    int dpi, family_ink;
    w_tool_feedback feedback;
    HANDLE updating;
    HANDLE reading;
    w_adapter_read *read;
    bool read_again, editable, present, sizing;
    wchar_t *report;
} w_adapter_panel;

typedef struct {
    wchar_t *text;
    size_t length, capacity;
    bool failed;
} w_adapter_text;

static void w_append(w_adapter_text *report, const wchar_t *text) {
    if (report->failed) return;
    size_t n = wcslen(text), size = report->length + n + 1;
    if (size > report->capacity) {
        size_t capacity = max(size,2*report->capacity);
        wchar_t *memory = realloc(report->text,capacity*sizeof(wchar_t));
        if (!memory) { report->failed = true; return; }
        report->text = memory;
        report->capacity = capacity;
    }
    wmemcpy(report->text+report->length,text,n+1);
    report->length += n;
}

static bool w_address(const SOCKET_ADDRESS *address, wchar_t *out, size_t cap) {
    if (!address->lpSockaddr) return false;
    SOCKADDR *sa = address->lpSockaddr;
    DWORD size = (DWORD)cap;
    return WSAAddressToStringW(sa, address->iSockaddrLength, NULL, out, &size) == 0;
}

static void w_details(w_adapter_panel *p,const IP_ADAPTER_ADDRESSES *a) {
    w_adapter_text report = {0};
    wchar_t value[320], ip[96];
    w_append(&report,a->Description && *a->Description ? a->Description : L"Network adapter");
    if (a->OperStatus == IfOperStatusUp && a->ReceiveLinkSpeed != UINT64_MAX && a->TransmitLinkSpeed != UINT64_MAX) {
        swprintf(value,count_of(value),L"\r\nLink speed    %llu Mbps receive / %llu Mbps send\r\n",
                 a->ReceiveLinkSpeed / 1000000, a->TransmitLinkSpeed / 1000000);
        w_append(&report,value);
    } else w_append(&report,L"\r\nLink speed unavailable\r\n");
    for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u; u = u->Next)
        if (w_address(&u->Address, ip, count_of(ip))) {
            swprintf(value,count_of(value),L"%ls    %ls/%u\r\n",u->Address.lpSockaddr->sa_family == AF_INET6 ?
                     L"IPv6" : L"IPv4",ip,(unsigned)u->OnLinkPrefixLength);
            w_append(&report,value);
        }
    for (IP_ADAPTER_GATEWAY_ADDRESS *g = a->FirstGatewayAddress; g; g = g->Next)
        if (w_address(&g->Address, ip, count_of(ip))) {
            swprintf(value,count_of(value),L"Gateway    %ls\r\n",ip);
            w_append(&report,value);
        }
    for (IP_ADAPTER_DNS_SERVER_ADDRESS *d = a->FirstDnsServerAddress; d; d = d->Next)
        if (w_address(&d->Address, ip, count_of(ip))) {
            swprintf(value,count_of(value),L"DNS    %ls\r\n",ip);
            w_append(&report,value);
        }
    if (a->DnsSuffix && *a->DnsSuffix) { w_append(&report,L"DNS suffix    "); w_append(&report,a->DnsSuffix); }
    if (report.failed || (p->report && !wcscmp(p->report,report.text))) {
        free(report.text);
        return;
    }
    DWORD start=0,end=0;
    SendMessageW(p->details,EM_GETSEL,(WPARAM)&start,(LPARAM)&end);
    int top=(int)SendMessageW(p->details,EM_GETFIRSTVISIBLELINE,0,0);
    SetWindowTextW(p->details,report.text);
    SendMessageW(p->details,EM_SETSEL,start,end);
    SendMessageW(p->details,EM_LINESCROLL,0,top-SendMessageW(p->details,EM_GETFIRSTVISIBLELINE,0,0));
    free(p->report);
    p->report=report.text;
    PostMessageW(GetParent(p->details),w_tool_feedback_changed,0,0);
}

enum { elevation_timer=1, adapter_timer=2, read_interval_ms=10000, read_completion_ms=150 };

static void w_release_read(w_adapter_read *read) {
    if (read && !InterlockedDecrement(&read->refs)) {
        free(read->all);
        free(read);
    }
}

static DWORD WINAPI w_read_details(void *argument) {
    w_adapter_read *read = argument;
    read->error = w_adapter_read_all(&read->all);
    w_release_read(read);
    return 0;
}

static void w_dns_buttons(w_adapter_panel *p) {
    bool ready = p->editable && p->present && !p->updating;
    EnableWindow(p->apply,ready);
    EnableWindow(p->automatic,ready);
}

// The modal dialog owns its own timer. Workers never touch HWNDs or borrow
// stack memory and can safely finish after the dialog is dismissed.
static void w_refresh_details(HWND w,w_adapter_panel *p,bool after_change) {
    if (p->reading) {
        p->read_again |= after_change;
        if (WaitForSingleObject(p->reading,0) != WAIT_OBJECT_0) return;
        CloseHandle(p->reading);
        p->reading = NULL;
        if (!p->read_again && !p->read->error) {
            const IP_ADAPTER_ADDRESSES *a=p->read->all;
            while (a && a->Luid.Value != p->adapter->Luid.Value) a=a->Next;
            if (a) w_details(p,a);
            if (p->present != (a!=NULL)) {
                p->present = a!=NULL;
                w_dns_buttons(p);
                w_tool_status(p->status,a ? L"Adapter available." : L"Adapter disconnected. Showing its last addresses.");
            }
        }
        w_release_read(p->read);
        p->read = NULL;
        if (!p->read_again) {
            SetTimer(w,adapter_timer,read_interval_ms,NULL);
            return;
        }
        p->read_again=false;
    }
    if (p->updating || !IsWindowVisible(w) || IsIconic(w)) return;
    p->read=calloc(1,sizeof(*p->read));
    if (p->read) {
        p->read->refs=2;
        p->reading=CreateThread(NULL,0,w_read_details,p->read,0,NULL);
    }
    if (!p->reading) { free(p->read); p->read=NULL; }
    SetTimer(w,adapter_timer,p->reading ? read_completion_ms : read_interval_ms,NULL);
}

static void w_dns_result(w_adapter_panel *p, DWORD error) {
    SetWindowTextW(p->status, !error ? L"DNS settings saved." :
        error == ERROR_CANCELLED ? L"Change cancelled." : error == ERROR_ACCESS_DENIED ? L"Windows denied access." :
        error == ERROR_NOT_SUPPORTED ? L"This Windows version cannot change interface DNS." :
        L"Windows could not change DNS for this adapter.");
    if (!error) w_refresh_details(GetParent(p->details),p,true);
}

static void w_save_dns(HWND w, w_adapter_panel *p, bool automatic) {
    wchar_t input[1024] = {0}, servers[1024], guid[40], request[1120];
    bool ipv6 = p->family_choice.row == 1;
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
        if (!SetTimer(w,elevation_timer,read_completion_ms,NULL)) {
            SetWindowTextW(p->status,L"Windows could not monitor the DNS change. Try again.");
            return;
        }
        if (ShellExecuteExW(&run)) {
            p->updating = run.hProcess;
            w_dns_buttons(p);
            SetWindowTextW(p->status,L"Applying DNS settings…");
            return;
        }
        error = GetLastError();
        KillTimer(w,elevation_timer);
    }
    w_dns_result(p,error);
}

static const wchar_t *const families[]={L"IPv4 DNS",L"IPv6 DNS"};

static void w_center_dns(HWND w,w_adapter_panel *p) {
    UINT dpi=GetDpiForWindow(w);
    RECT bounds={0,0,MulDiv(548,dpi,96),1},anchor;
    w_theme_adjust_rect(&bounds,GetWindowLongW(w,GWL_STYLE),GetWindowLongW(w,GWL_EXSTYLE),dpi);
    GetWindowRect(GetParent(w),&anchor);
    int width=bounds.right-bounds.left,height=bounds.bottom-bounds.top;
    int left=(anchor.left+anchor.right-width)/2,top=(anchor.top+anchor.bottom-height)/2;
    bounds=w_tool_fit_to_monitor((RECT){left,top,left+width,top+height},
                                 MonitorFromRect(&anchor,MONITOR_DEFAULTTONEAREST));
    p->sizing=true;
    SetWindowPos(w,NULL,bounds.left,bounds.top,bounds.right-bounds.left,bounds.bottom-bounds.top,
                 SWP_NOZORDER|SWP_NOACTIVATE);
    p->sizing=false;
}

static void w_position_dns(HWND w,w_adapter_panel *p,bool opening) {
    if (p->sizing) return;
    p->sizing=true;
    int dpi = (int)GetDpiForWindow(w);
    RECT client;
    w_theme_content_rect(w,&client);
    int gap=w_theme_spacing(dpi), pad=2*gap, side=w_theme_button_size(dpi);
    int width=max(1,client.right-2*pad);
    int line=w_theme_role_line_height(dpi,text_body);
    if (!p->font || p->dpi != dpi) {
        HFONT font=w_theme_message_font(dpi);
        if (font) {
            for (HWND c=GetWindow(w,GW_CHILD); c; c=GetWindow(c,GW_HWNDNEXT))
                SendMessageW(c,WM_SETFONT,(WPARAM)font,FALSE);
            DeleteObject(p->font);
            p->font=font;
            p->dpi=dpi;
            p->family_ink=w_tool_widest_option(font,families,count_of(families));
        }
    }
    RECT report;
    GetWindowRect(p->details,&report);
    if (report.right-report.left!=width)
        w_tool_place(p->details,pad,pad,width,max(1,report.bottom-report.top));
    int rows=(int)SendMessageW(p->details,EM_GETLINECOUNT,0,0);
    size_t length=p->report ? wcslen(p->report) : 0;
    if (rows>1 && length && p->report[length-1]==L'\n') --rows;
    int report_height=min(10,max(1,rows))*line+2*GetSystemMetricsForDpi(SM_CYEDGE,dpi);
    int feedback=w_tool_feedback_height(p->status,&p->feedback,width);
    int height=7*gap+line+2*side+report_height+(feedback ? gap+feedback : 0);
    w_tool_fit_height(w,height-client.bottom,opening);
    w_theme_content_rect(w,&client);
    int status=client.bottom-pad-feedback;
    int actions=status-(feedback ? gap : 0)-side, input=actions-gap-side, heading=input-gap-line;
    w_tool_place(p->details,pad,pad,width,heading-gap-pad);
    w_tool_place(p->heading,pad,heading,width,line);
    int family=min(w_tool_selector_width(p->family,p->family_ink,dpi),width/2);
    w_tool_place_combo(p->family,pad,input,family,side,4);
    w_tool_place(p->entry,pad+family+gap,input,width-family-gap,side);
    w_tool_place(p->apply,pad,actions,side,side);
    w_tool_place(p->automatic,pad+side+gap,actions,side,side);
    w_tool_visible(p->status,feedback>0);
    if (feedback) w_tool_place(p->status,pad,status,width,feedback);
    p->sizing=false;
}

static LRESULT CALLBACK w_dns_entry(HWND entry,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR data) {
    (void)data;
    if (msg == WM_GETDLGCODE && lp && (wp==VK_RETURN || w_native_ime_key((const MSG *)lp)))
        return DefSubclassProc(entry,msg,wp,lp) | DLGC_WANTMESSAGE;
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        if (w_native_ime_key(&(MSG){.hwnd=entry,.message=msg,.wParam=wp,.lParam=lp}))
            return DefSubclassProc(entry,msg,wp,lp);
        if (!(HIWORD(lp) & KF_REPEAT)) SendMessageW(GetParent(entry),WM_COMMAND,IDOK,0);
        return 0;
    }
    if (msg == WM_CHAR && wp == L'\r') return 0;
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(entry,w_dns_entry,id);
    return DefSubclassProc(entry,msg,wp,lp);
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
        p->details=CreateWindowExW(0,L"EDIT",L"",child|WS_BORDER|ES_MULTILINE|ES_READONLY|WS_VSCROLL,
                                   0,0,0,0,w,NULL,inst,NULL);
        p->heading=CreateWindowW(L"STATIC",L"DNS servers",WS_CHILD|WS_VISIBLE,0,0,0,0,w,NULL,inst,NULL);
        p->family=CreateWindowW(L"COMBOBOX",L"DNS address family",child|w_tool_dropdown_list,0,0,0,0,w,NULL,inst,NULL);
        p->entry=w_native_field(w,0,WS_VISIBLE);
        w_native_edit_frame(p->details,NULL);
        SetWindowSubclass(p->entry,w_dns_entry,1,0);
        p->apply=CreateWindowW(L"BUTTON",L"Apply DNS",child|BS_OWNERDRAW,0,0,0,0,w,(HMENU)303,inst,NULL);
        p->automatic=CreateWindowW(L"BUTTON",L"Use automatic DNS",child|BS_OWNERDRAW,0,0,0,0,w,(HMENU)304,inst,NULL);
        p->status=CreateWindowW(L"STATIC",L"",WS_CHILD|WS_VISIBLE|SS_EDITCONTROL|SS_NOPREFIX,0,0,0,0,w,NULL,inst,NULL);
        w_tool_track_feedback(p->status,&p->feedback);
        w_icon_button(p->apply,L"\uE74E",L"Apply entered DNS servers");
        w_icon_button(p->automatic,L"\uE895",L"Use automatically assigned DNS servers");
        w_theme_follow(w);
        for (size_t i=0;i<count_of(families);i++) SendMessageW(p->family,CB_ADDSTRING,0,(LPARAM)families[i]);
        SendMessageW(p->family,CB_SETCURSEL,0,0);
        SendMessageW(p->entry,EM_LIMITTEXT,1023,0);
        SendMessageW(p->entry,EM_SETCUEBANNER,TRUE,(LPARAM)L"DNS servers, separated by commas");
        SendMessageW(p->details,EM_LIMITTEXT,0,0);
        w_details(p,a);
        p->present = true;
        p->editable = ConvertInterfaceLuidToGuid(&a->Luid,&p->id) == NO_ERROR && w_dns_api();
        w_dns_buttons(p);
        SetTimer(w,adapter_timer,read_interval_ms,NULL);
        SetWindowTextW(p->status,p->editable ? L"Separate addresses with commas. Press Enter to apply." :
                                            L"DNS editing is unavailable on this adapter or Windows version.");
        w_center_dns(w,p);
        w_position_dns(w,p,true);
        SetFocus(p->family);
        return FALSE;
    }
    LRESULT result;
    if (w_theme_message(w,msg,wp,lp,&result)) {
        if (msg == WM_DRAWITEM || msg == WM_MEASUREITEM) { SetWindowLongPtrW(w,DWLP_MSGRESULT,result); return TRUE; }
        return result;
    }
    if (!p) return FALSE;
    if ((msg==w_tool_feedback_changed || (msg==WM_SIZE && wp!=SIZE_MINIMIZED)) && p->status) {
        w_position_dns(w,p,false);
        return TRUE;
    }
    if (msg == WM_COMMAND) {
        if (p->family && (HWND)lp == p->family) {
            int previous = p->family_choice.row;
            if (w_tool_choice_changed(&p->family_choice,p->family,HIWORD(wp))) {
                GetWindowTextW(p->entry,p->servers[previous],count_of(p->servers[0]));
                SetWindowTextW(p->entry,p->servers[p->family_choice.row]);
            }
        } else if (LOWORD(wp)==IDCANCEL) EndDialog(w,0);
        else if (LOWORD(wp)==IDOK && GetFocus()==p->family) SetFocus(p->entry);
        else if (LOWORD(wp)==303 || LOWORD(wp)==304 || (LOWORD(wp)==IDOK && GetFocus()==p->entry)) {
            if (!p->updating && IsWindowEnabled(p->apply)) w_save_dns(w,p,LOWORD(wp)==304);
        } else return FALSE;
        return TRUE;
    }
    if (msg == WM_TIMER && wp == adapter_timer) {
        w_refresh_details(w,p,false);
        return TRUE;
    }
    if (msg == WM_TIMER && wp == elevation_timer && p->updating && WaitForSingleObject(p->updating,0)==WAIT_OBJECT_0) {
        DWORD code=ERROR_GEN_FAILURE;
        GetExitCodeProcess(p->updating,&code);
        CloseHandle(p->updating); p->updating=NULL;
        KillTimer(w,elevation_timer);
        w_dns_buttons(p);
        w_dns_result(p,code);
        return TRUE;
    }
    if (w_tool_font_change(w,msg,wp,lp)) {
        if (msg == WM_SETTINGCHANGE) p->dpi = 0;
        w_position_dns(w,p,false); return TRUE;
    }
    return FALSE;
}

bool w_adapter_show(HWND owner, const IP_ADAPTER_ADDRESSES *adapter) {
    struct { DLGTEMPLATE dialog; WORD menu, window_class, title; } tmpl = {0};
    tmpl.dialog.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN | DS_MODALFRAME;
    tmpl.dialog.dwExtendedStyle = WS_EX_TOOLWINDOW;
    w_adapter_panel p={.adapter=adapter};
    WSADATA data;
    bool sockets=WSAStartup(MAKEWORD(2,2),&data)==0;
    INT_PTR result = DialogBoxIndirectParamW(GetModuleHandleW(NULL),&tmpl.dialog,owner,w_dns_proc,(LPARAM)&p);
    if (sockets) WSACleanup();
    if (p.updating) CloseHandle(p.updating);
    if (p.reading) CloseHandle(p.reading);
    w_release_read(p.read);
    free(p.report);
    if (p.font) DeleteObject(p.font);
    return result != -1;
}
