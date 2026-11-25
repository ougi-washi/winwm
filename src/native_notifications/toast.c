#define COBJMACROS
#define WIDL_using_Windows_Data_Xml_Dom
#define WIDL_using_Windows_UI_Notifications
#include "native_notifications/toast.h"
#include <roapi.h>
#include <winstring.h>
#include <initguid.h>
#include <windows.data.xml.dom.h>
#include <windows.ui.notifications.h>
#include <wchar.h>

static const wchar_t classes_path[]=L"Software\\Classes\\AppUserModelId";
static const wchar_t marker[]=L"WinwmPortableLease";
static const wchar_t group[]=L"winwm";

typedef struct {
    DWORD version, pid;
    uint64_t birth;
    GUID activator;
} w_toast_owner;

static struct {
    w_toast_owner owner;
    HKEY parent,key;
    DWORD factory;
    CO_MTA_USAGE_COOKIE mta;
} active;
static LONG stopped;
static SRWLOCK target_lock;
static HWND target_window;
static UINT target_message;

static uint64_t w_birth(HANDLE process) {
    FILETIME created,exited,kernel,user;
    if (!GetProcessTimes(process,&created,&exited,&kernel,&user)) return 0;
    return ((uint64_t)created.dwHighDateTime<<32)|created.dwLowDateTime;
}

static bool w_gone(DWORD pid,uint64_t birth) {
    HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,pid);
    if (!process) return GetLastError()==ERROR_INVALID_PARAMETER;
    uint64_t actual=w_birth(process);
    bool gone=(actual && actual!=birth) || WaitForSingleObject(process,0)==WAIT_OBJECT_0;
    CloseHandle(process);
    return gone;
}

// MinGW does not ship NotificationActivationCallback.h. This is the documented
// INotificationActivationCallback ABI from the Windows SDK.
typedef struct w_activation w_activation;
typedef struct { LPCWSTR key,value; } w_notification_input;
typedef struct {
    HRESULT (STDMETHODCALLTYPE *query)(w_activation *,REFIID,void **);
    ULONG (STDMETHODCALLTYPE *retain)(w_activation *);
    ULONG (STDMETHODCALLTYPE *release)(w_activation *);
    HRESULT (STDMETHODCALLTYPE *activate)(w_activation *,LPCWSTR,LPCWSTR,const w_notification_input *,ULONG);
} w_activation_vtbl;
struct w_activation { const w_activation_vtbl *vtbl; };
static const IID activation_iid={0x53e31837,0x6600,0x4a81,{0x93,0x95,0x75,0xcf,0xfe,0x74,0x6f,0x94}};
static LONG activation_refs=1, factory_refs=1;

static ULONG STDMETHODCALLTYPE w_activate_retain(w_activation *self) {
    (void)self; return InterlockedIncrement(&activation_refs);
}
static ULONG STDMETHODCALLTYPE w_activate_release(w_activation *self) {
    (void)self; return InterlockedDecrement(&activation_refs);
}
static HRESULT STDMETHODCALLTYPE w_activate_query(w_activation *self,REFIID iid,void **out) {
    if (!out) return E_POINTER;
    *out=NULL;
    if (!IsEqualIID(iid,&IID_IUnknown) && !IsEqualIID(iid,&activation_iid)) return E_NOINTERFACE;
    w_activate_retain(self);
    *out=self;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE w_activate(w_activation *self,LPCWSTR id,LPCWSTR args,
                                            const w_notification_input *inputs,ULONG count) {
    (void)self; (void)inputs;
    if (!id || wcscmp(id,WINWM_NOTIFICATION_AUMID) || !args || wcscmp(args,L"open") || count)
        return E_INVALIDARG;
    AcquireSRWLockShared(&target_lock);
    bool sent=target_window && PostMessageW(target_window,target_message,0,0);
    ReleaseSRWLockShared(&target_lock);
    return sent ? S_OK : S_FALSE;
}
static const w_activation_vtbl activation_vtbl={w_activate_query,w_activate_retain,w_activate_release,w_activate};
static w_activation activation={&activation_vtbl};

static ULONG STDMETHODCALLTYPE w_factory_retain(IClassFactory *self) {
    (void)self; return InterlockedIncrement(&factory_refs);
}
static ULONG STDMETHODCALLTYPE w_factory_release(IClassFactory *self) {
    (void)self; return InterlockedDecrement(&factory_refs);
}
static HRESULT STDMETHODCALLTYPE w_factory_query(IClassFactory *self,REFIID iid,void **out) {
    if (!out) return E_POINTER;
    *out=NULL;
    if (!IsEqualIID(iid,&IID_IUnknown) && !IsEqualIID(iid,&IID_IClassFactory)) return E_NOINTERFACE;
    w_factory_retain(self);
    *out=self;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE w_factory_create(IClassFactory *self,IUnknown *outer,REFIID iid,void **out) {
    (void)self;
    return outer ? CLASS_E_NOAGGREGATION : w_activate_query(&activation,iid,out);
}
static HRESULT STDMETHODCALLTYPE w_factory_lock(IClassFactory *self,BOOL lock) {
    if (lock) w_factory_retain(self); else w_factory_release(self);
    return S_OK;
}
static IClassFactoryVtbl factory_vtbl={w_factory_query,w_factory_retain,w_factory_release,w_factory_create,w_factory_lock};
static IClassFactory factory={&factory_vtbl};

void w_notification_submit_target(HWND window,UINT message) {
    AcquireSRWLockExclusive(&target_lock);
    target_window=window;
    target_message=message;
    ReleaseSRWLockExclusive(&target_lock);
}

static void w_stage_name(const w_toast_owner *owner,wchar_t name[96]) {
    swprintf(name,96,WINWM_NOTIFICATION_AUMID L".Pending.%lu.%llu",owner->pid,(unsigned long long)owner->birth);
}

static bool w_owner_read(HKEY key,w_toast_owner *owner) {
    DWORD size=sizeof(*owner);
    return RegGetValueW(key,NULL,marker,RRF_RT_REG_BINARY,NULL,owner,&size)==ERROR_SUCCESS &&
           size==sizeof(*owner) && owner->version==1 && owner->pid && owner->birth;
}

static bool w_value_is(HKEY key,const wchar_t *name,const wchar_t *expected) {
    wchar_t value[80]; DWORD bytes=sizeof(value);
    return RegGetValueW(key,NULL,name,RRF_RT_REG_SZ,NULL,value,&bytes)==ERROR_SUCCESS &&
           bytes==(wcslen(expected)+1)*sizeof(wchar_t) && !wcscmp(value,expected);
}

static void w_remove_key(HKEY parent,HKEY key,const w_toast_owner *owner,bool published) {
    w_toast_owner current;
    if (!w_owner_read(key,&current) || memcmp(&current,owner,sizeof(current))) return;
    wchar_t guid[40], stage[96];
    StringFromGUID2(&owner->activator,guid,ARRAYSIZE(guid));
    w_stage_name(owner,stage);
    DWORD children,values;
    if (RegQueryInfoKeyW(key,NULL,NULL,NULL,&children,NULL,NULL,&values,NULL,NULL,NULL,NULL)) return;
    bool display=w_value_is(key,L"DisplayName",L"winwm");
    bool cls=w_value_is(key,L"CustomActivator",guid);
    if (children || values!=1u+display+cls) {
        // A separate owner added or edited something. Preserve its contents.
        if (display) RegDeleteValueW(key,L"DisplayName");
        if (cls) RegDeleteValueW(key,L"CustomActivator");
        RegDeleteValueW(key,marker);
        return;
    }
    // Rename by handle before deleting by name so a subsequent manager can
    // claim the canonical AUMID without being deleted by this cleanup.
    if (!published || !RegRenameKey(key,NULL,stage)) RegDeleteKeyW(parent,stage);
}

static LONG w_write_string(HKEY key,const wchar_t *name,const wchar_t *value) {
    return RegSetValueExW(key,name,0,REG_SZ,(const BYTE *)value,(DWORD)((wcslen(value)+1)*sizeof(wchar_t)));
}

static HKEY w_register_key(HKEY parent,const w_toast_owner *owner) {
    wchar_t stage[96],guid[40];
    w_stage_name(owner,stage);
    if (!StringFromGUID2(&owner->activator,guid,ARRAYSIZE(guid))) return NULL;
    HKEY key;
    DWORD disposition;
    if (RegCreateKeyExW(parent,stage,0,NULL,REG_OPTION_VOLATILE,KEY_READ|KEY_WRITE|DELETE,NULL,&key,&disposition))
        return NULL;
    if (disposition!=REG_CREATED_NEW_KEY) { RegCloseKey(key); return NULL; }
    LONG err=RegSetValueExW(key,marker,0,REG_BINARY,(const BYTE *)owner,sizeof(*owner));
    if (!err) err=w_write_string(key,L"DisplayName",L"winwm");
    if (!err) err=w_write_string(key,L"CustomActivator",guid);
    // Never modify or replace an existing identity. A partially initialized
    // registration is not visible to the broker under the production AUMID.
    if (!err) err=RegRenameKey(key,NULL,WINWM_NOTIFICATION_AUMID);
    if (!err) return key;
    RegDeleteKeyW(parent,stage);
    RegCloseKey(key);
    return NULL;
}

static HRESULT w_string(const wchar_t *s,HSTRING *out) {
    size_t n=wcslen(s);
    return n<=UINT32_MAX ? WindowsCreateString(s,(UINT32)n,out) : E_INVALIDARG;
}

static HRESULT w_winrt_factory(const wchar_t *name,REFIID iid,void **out) {
    HSTRING id=NULL;
    HRESULT hr=w_string(name,&id);
    if (SUCCEEDED(hr)) hr=RoGetActivationFactory(id,iid,out);
    WindowsDeleteString(id);
    return hr;
}

static HRESULT w_xml_text(IXmlNodeList *nodes,UINT32 index,const wchar_t *text) {
    IXmlNode *node=NULL;
    IXmlNodeSerializer *serializer=NULL;
    HSTRING string=NULL;
    HRESULT hr=IXmlNodeList_Item(nodes,index,&node);
    if (SUCCEEDED(hr) && !node) hr=E_UNEXPECTED;
    if (SUCCEEDED(hr)) hr=IXmlNode_QueryInterface(node,&IID_IXmlNodeSerializer,(void **)&serializer);
    if (SUCCEEDED(hr)) hr=w_string(text,&string);
    if (SUCCEEDED(hr)) hr=IXmlNodeSerializer_put_InnerText(serializer,string);
    WindowsDeleteString(string);
    if (serializer) IXmlNodeSerializer_Release(serializer);
    if (node) IXmlNode_Release(node);
    return hr;
}

static HRESULT w_content(const wchar_t *title,const wchar_t *body,IXmlDocument **out) {
    static const wchar_t markup[]=L"<toast launch=\"open\"><visual><binding template=\"ToastGeneric\">"
                                   L"<text/><text/></binding></visual><audio silent=\"true\"/></toast>";
    IInspectable *object=NULL;
    IXmlDocument *doc=NULL;
    IXmlDocumentIO *io=NULL;
    IXmlNodeList *nodes=NULL;
    HSTRING name=NULL,xml=NULL,tag=NULL;
    HRESULT hr=w_string(RuntimeClass_Windows_Data_Xml_Dom_XmlDocument,&name);
    if (SUCCEEDED(hr)) hr=RoActivateInstance(name,&object);
    if (SUCCEEDED(hr)) hr=IInspectable_QueryInterface(object,&IID_IXmlDocumentIO,(void **)&io);
    if (SUCCEEDED(hr)) hr=w_string(markup,&xml);
    if (SUCCEEDED(hr)) hr=IXmlDocumentIO_LoadXml(io,xml);
    if (SUCCEEDED(hr)) hr=IInspectable_QueryInterface(object,&IID_IXmlDocument,(void **)&doc);
    if (SUCCEEDED(hr)) hr=w_string(L"text",&tag);
    if (SUCCEEDED(hr)) hr=IXmlDocument_GetElementsByTagName(doc,tag,&nodes);
    if (SUCCEEDED(hr)) hr=w_xml_text(nodes,0,title);
    if (SUCCEEDED(hr)) hr=w_xml_text(nodes,1,body);
    WindowsDeleteString(tag); WindowsDeleteString(xml); WindowsDeleteString(name);
    if (nodes) IXmlNodeList_Release(nodes);
    if (io) IXmlDocumentIO_Release(io);
    if (object) IInspectable_Release(object);
    if (SUCCEEDED(hr)) *out=doc; else if (doc) IXmlDocument_Release(doc);
    return hr;
}

static HRESULT w_tag(const w_toast_owner *owner,HSTRING *tag) {
    wchar_t value[17];
    swprintf(value,ARRAYSIZE(value),L"%08lx%04x%04x",owner->activator.Data1,
             (unsigned)owner->activator.Data2,(unsigned)owner->activator.Data3);
    return w_string(value,tag);
}

static HRESULT w_send(IToastNotifier *notifier,const w_toast_owner *owner,const wchar_t *title,const wchar_t *body) {
    IXmlDocument *doc=NULL;
    IToastNotificationFactory *maker=NULL;
    IToastNotification *toast=NULL;
    IToastNotification2 *options=NULL;
    HSTRING label=NULL,category=NULL;
    HRESULT hr=w_content(title,body,&doc);
    if (SUCCEEDED(hr)) hr=w_winrt_factory(RuntimeClass_Windows_UI_Notifications_ToastNotification,
                                          &IID_IToastNotificationFactory,(void **)&maker);
    if (SUCCEEDED(hr)) hr=IToastNotificationFactory_CreateToastNotification(maker,doc,&toast);
    if (SUCCEEDED(hr)) hr=IToastNotification_QueryInterface(toast,&IID_IToastNotification2,(void **)&options);
    if (SUCCEEDED(hr)) hr=w_tag(owner,&label);
    if (SUCCEEDED(hr)) hr=w_string(group,&category);
    if (SUCCEEDED(hr)) hr=IToastNotification2_put_Tag(options,label);
    if (SUCCEEDED(hr)) hr=IToastNotification2_put_Group(options,category);
    if (SUCCEEDED(hr)) hr=IToastNotification2_put_SuppressPopup(options,TRUE);
    if (SUCCEEDED(hr)) hr=IToastNotifier_Show(notifier,toast);
    WindowsDeleteString(category); WindowsDeleteString(label);
    if (options) IToastNotification2_Release(options);
    if (toast) IToastNotification_Release(toast);
    if (maker) IToastNotificationFactory_Release(maker);
    if (doc) IXmlDocument_Release(doc);
    return hr;
}

static void w_remove_toast(const w_toast_owner *owner) {
    IToastNotificationManagerStatics2 *manager=NULL;
    IToastNotificationHistory *history=NULL;
    HSTRING id=NULL,label=NULL,category=NULL;
    HRESULT hr=w_winrt_factory(RuntimeClass_Windows_UI_Notifications_ToastNotificationManager,
                               &IID_IToastNotificationManagerStatics2,(void **)&manager);
    if (SUCCEEDED(hr)) hr=IToastNotificationManagerStatics2_get_History(manager,&history);
    if (SUCCEEDED(hr)) hr=w_string(WINWM_NOTIFICATION_AUMID,&id);
    if (SUCCEEDED(hr)) hr=w_tag(owner,&label);
    if (SUCCEEDED(hr)) hr=w_string(group,&category);
    if (SUCCEEDED(hr)) IToastNotificationHistory_RemoveGroupedTagWithId(history,label,category,id);
    WindowsDeleteString(category); WindowsDeleteString(label); WindowsDeleteString(id);
    if (history) IToastNotificationHistory_Release(history);
    if (manager) IToastNotificationManagerStatics2_Release(manager);
}

static void w_release_registration(void) {
    if (active.key) {
        w_remove_key(active.parent,active.key,&active.owner,true);
        RegCloseKey(active.key);
    }
    if (active.parent) RegCloseKey(active.parent);
    if (active.factory) CoRevokeClassObject(active.factory);
    if (active.mta) CoDecrementMTAUsage(active.mta);
    ZeroMemory(&active,sizeof(active));
}

static HRESULT w_enabled_notifier(IToastNotificationManagerStatics *manager,HSTRING id,IToastNotifier **out) {
    NotificationSetting setting=NotificationSetting_DisabledForApplication;
    HRESULT hr=IToastNotificationManagerStatics_CreateToastNotifierWithId(manager,id,out);
    if (SUCCEEDED(hr)) hr=IToastNotifier_get_Setting(*out,&setting);
    return SUCCEEDED(hr) && setting!=NotificationSetting_Enabled ? E_ACCESSDENIED : hr;
}

bool w_notification_submit(const wchar_t *title,const wchar_t *body) {
    if (!title || !body || !*body || active.key || InterlockedCompareExchange(&stopped,0,0)) return false;
    if (FAILED(RoInitialize(RO_INIT_MULTITHREADED))) return false;
    IToastNotificationManagerStatics *manager=NULL;
    IToastNotifier *notifier=NULL;
    HSTRING id=NULL;
    HRESULT hr=w_winrt_factory(RuntimeClass_Windows_UI_Notifications_ToastNotificationManager,
                               &IID_IToastNotificationManagerStatics,(void **)&manager);
    if (SUCCEEDED(hr)) hr=w_string(WINWM_NOTIFICATION_AUMID,&id);
    if (SUCCEEDED(hr)) hr=w_enabled_notifier(manager,id,&notifier);
    if (notifier) { IToastNotifier_Release(notifier); notifier=NULL; }
    if (SUCCEEDED(hr)) hr=HRESULT_FROM_WIN32(RegOpenKeyExW(HKEY_CURRENT_USER,classes_path,0,KEY_READ|KEY_WRITE,&active.parent));
    if (SUCCEEDED(hr)) {
        active.owner.version=1;
        active.owner.pid=GetCurrentProcessId();
        active.owner.birth=w_birth(GetCurrentProcess());
        hr=active.owner.birth ? CoCreateGuid(&active.owner.activator) : E_FAIL;
    }
    if (SUCCEEDED(hr)) hr=CoIncrementMTAUsage(&active.mta);
    if (SUCCEEDED(hr)) hr=CoRegisterClassObject(&active.owner.activator,(IUnknown *)&factory,CLSCTX_LOCAL_SERVER,
                                               REGCLS_MULTIPLEUSE,&active.factory);
    if (SUCCEEDED(hr)) {
        active.key=w_register_key(active.parent,&active.owner);
        if (!active.key) hr=E_FAIL;
    }
    if (SUCCEEDED(hr)) hr=w_enabled_notifier(manager,id,&notifier);
    if (SUCCEEDED(hr)) hr=w_send(notifier,&active.owner,title,body);
    if (FAILED(hr)) w_release_registration();
    WindowsDeleteString(id);
    if (notifier) IToastNotifier_Release(notifier);
    if (manager) IToastNotificationManagerStatics_Release(manager);
    RoUninitialize();
    return SUCCEEDED(hr);
}

void w_notification_submit_done(void) {
    if (!active.key) return;
    HRESULT hr=RoInitialize(RO_INIT_MULTITHREADED);
    if (SUCCEEDED(hr)) w_remove_toast(&active.owner);
    w_release_registration();
    if (SUCCEEDED(hr)) RoUninitialize();
}

void w_notification_submit_stop(void) {
    InterlockedExchange(&stopped,1);
    w_notification_submit_target(NULL,0);
}

void w_notification_submit_recover(DWORD pid,uint64_t birth) {
    if (!pid || !birth || !w_gone(pid,birth)) return;
    HKEY parent;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,classes_path,0,KEY_READ|KEY_WRITE,&parent)) return;
    w_toast_owner expected={.pid=pid,.birth=birth};
    wchar_t stage[96];
    w_stage_name(&expected,stage);
    const wchar_t *names[]={WINWM_NOTIFICATION_AUMID,stage};
    for (size_t i=0;i<ARRAYSIZE(names);++i) {
        HKEY key;
        if (RegOpenKeyExW(parent,names[i],0,KEY_READ|KEY_WRITE|DELETE,&key)) continue;
        w_toast_owner owner;
        if (w_owner_read(key,&owner) && owner.pid==pid && owner.birth==birth) {
            if (!i) {
                HRESULT hr=RoInitialize(RO_INIT_MULTITHREADED);
                if (hr==RPC_E_CHANGED_MODE) hr=RoInitialize(RO_INIT_SINGLETHREADED);
                if (SUCCEEDED(hr)) { w_remove_toast(&owner); RoUninitialize(); }
            }
            w_remove_key(parent,key,&owner,i==0);
        }
        RegCloseKey(key);
    }
    RegCloseKey(parent);
}
