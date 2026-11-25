#define COBJMACROS
#include "native_notifications/banners.h"
#include <uiautomationclient.h>
#include <dwmapi.h>
#include <stdint.h>
#include <stdlib.h>
#include <wchar.h>

// WindowOpened is also raised for virtual XAML windows. A top-level HWND
// show hook alone misses subsequent toasts hosted by the same CoreWindow.
typedef struct {
    IUIAutomationEventHandler iface;
    LONG refs;
    SRWLOCK lock;
    HANDLE stop;
    HWND receiver;
    UINT message;
    IUIAutomationCacheRequest *cache;
    IUIAutomationCondition *text;
    IUIAutomationTreeWalker *walker;
    struct { uint64_t element, content; ULONGLONG tick; } seen[16];
    unsigned next;
} w_banner_subscription;

static w_banner_subscription *active;

static ULONG STDMETHODCALLTYPE w_retain(IUIAutomationEventHandler *self) {
    return InterlockedIncrement(&((w_banner_subscription *)self)->refs);
}

static ULONG STDMETHODCALLTYPE w_release(IUIAutomationEventHandler *self) {
    w_banner_subscription *s=(w_banner_subscription *)self;
    LONG refs=InterlockedDecrement(&s->refs);
    if (!refs) { CloseHandle(s->stop);free(s); }
    return refs;
}

static HRESULT STDMETHODCALLTYPE w_query(IUIAutomationEventHandler *self,REFIID iid,void **out) {
    if (!out) return E_POINTER;
    *out=NULL;
    if (!IsEqualIID(iid,&IID_IUnknown) && !IsEqualIID(iid,&IID_IUIAutomationEventHandler)) return E_NOINTERFACE;
    *out=self;w_retain(self);return S_OK;
}

static bool w_shell_process(DWORD pid) {
    HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    if (!process) return false;
    wchar_t image[MAX_PATH],windows[MAX_PATH];
    DWORD length=ARRAYSIZE(image);
    BOOL read=QueryFullProcessImageNameW(process,0,image,&length);
    CloseHandle(process);
    UINT n=GetSystemWindowsDirectoryW(windows,ARRAYSIZE(windows));
    if (!read || !n || n>=ARRAYSIZE(windows) || length<=n || _wcsnicmp(windows,image,n)) return false;
    const wchar_t *relative=image+n;
    return !_wcsicmp(relative,L"\\SystemApps\\ShellExperienceHost_cw5n1h2txyewy\\ShellExperienceHost.exe") ||
           !_wcsicmp(relative,L"\\System32\\ShellHost.exe");
}

static bool w_toast_id(const wchar_t *id) {
    if (!id) return false;
    static const wchar_t *const names[]={L"ToastView",L"NormalToastView",L"PriorityToastView",L"FlexibleToastView",
                                         L"FlexibleNormalToastView",L"FlexiblePriorityToastView"};
    // Some versions prefix the XAML view identifier with its namespace.
    const wchar_t *name=wcsrchr(id,L'.');
    name=name ? name+1 : id;
    for (size_t i=0;i<ARRAYSIZE(names);++i) if (!wcscmp(name,names[i])) return true;
    return false;
}

static bool w_shell_window(HWND window,DWORD pid) {
    HWND root=GetAncestor(window,GA_ROOT);
    DWORD owner=0,cloaked=0;
    GetWindowThreadProcessId(root,&owner);
    if (owner!=pid || !IsWindowVisible(root)) return false;
    if (SUCCEEDED(DwmGetWindowAttribute(root,DWMWA_CLOAKED,&cloaked,sizeof(cloaked))) && cloaked) return false;
    wchar_t cls[100];
    return GetClassNameW(root,cls,ARRAYSIZE(cls)) &&
           (!wcscmp(cls,L"Windows.UI.Core.CoreWindow") || !wcscmp(cls,L"XamlExplorerHostIslandWindow") ||
            !wcscmp(cls,L"Windows.UI.Composition.DesktopWindowContentBridge"));
}

static HWND w_host_window(IUIAutomationElement *element,IUIAutomationTreeWalker *walker) {
    IUIAutomationElement_AddRef(element);
    UIA_HWND handle=NULL;
    for (int i=0;i<12 && element;++i) {
        IUIAutomationElement *parent=NULL;
        IUIAutomationElement_get_CurrentNativeWindowHandle(element,&handle);
        if (!handle) IUIAutomationTreeWalker_GetParentElement(walker,element,&parent);
        IUIAutomationElement_Release(element);
        element=parent;
        if (handle) break;
    }
    if (element) IUIAutomationElement_Release(element);
    return (HWND)handle;
}

static uint64_t w_hash(uint64_t value,const void *data,size_t bytes) {
    const unsigned char *p=data;
    while (bytes--) { value^=*p++;value*=UINT64_C(1099511628211); }
    return value;
}

static uint64_t w_runtime_id(IUIAutomationElement *element,HWND fallback) {
    SAFEARRAY *id=NULL;
    uint64_t hash=(uintptr_t)fallback;
    if (SUCCEEDED(IUIAutomationElement_GetRuntimeId(element,&id)) && id) {
        LONG first,last;
        if (SafeArrayGetDim(id)==1 && SafeArrayGetElemsize(id)==sizeof(int) &&
            SUCCEEDED(SafeArrayGetLBound(id,1,&first)) && SUCCEEDED(SafeArrayGetUBound(id,1,&last)) &&
            last>=first && (LONGLONG)last-first<64) {
            void *data=NULL;
            if (SUCCEEDED(SafeArrayAccessData(id,&data))) {
                hash=w_hash(hash,data,((size_t)((LONGLONG)last-first)+1)*sizeof(int));
                SafeArrayUnaccessData(id);
            }
        }
        SafeArrayDestroy(id);
    }
    return hash;
}

static void w_append(wchar_t *out,size_t cap,const wchar_t *text) {
    size_t n=wcslen(out);
    if (n && n+1<cap) out[n++]=L'\n';
    while (*text && n+1<cap) out[n++]=*text++;
    if (n && IS_HIGH_SURROGATE(out[n-1])) --n;
    out[n]=0;
}

static w_banner *w_read_text(IUIAutomationElement *el,IUIAutomationCacheRequest *cache,IUIAutomationCondition *text) {
    IUIAutomationElementArray *items=NULL;
    if (FAILED(IUIAutomationElement_FindAllBuildCache(el,TreeScope_Descendants,text,cache,&items)) || !items) return NULL;
    w_banner *b=calloc(1,sizeof(*b));
    int n=0;
    IUIAutomationElementArray_get_Length(items,&n);
    for (int i=0;b && i<n && i<64;++i) {
        IUIAutomationElement *child=NULL;
        if (FAILED(IUIAutomationElementArray_GetElement(items,i,&child)) || !child) continue;
        BSTR name=NULL,id=NULL;
        BOOL offscreen=TRUE;
        if (SUCCEEDED(IUIAutomationElement_get_CachedIsOffscreen(child,&offscreen)) && !offscreen) {
            IUIAutomationElement_get_CachedName(child,&name);
            IUIAutomationElement_get_CachedAutomationId(child,&id);
        }
        if (name && *name) {
            if (id && (!wcscmp(id,L"AppName") || !wcscmp(id,L"AppNameTextBlock") || !wcscmp(id,L"AppDisplayName")))
                lstrcpynW(b->app,name,ARRAYSIZE(b->app));
            else if (!*b->title) w_append(b->title,ARRAYSIZE(b->title),name);
            else w_append(b->body,ARRAYSIZE(b->body),name);
        }
        SysFreeString(id);SysFreeString(name);IUIAutomationElement_Release(child);
    }
    IUIAutomationElementArray_Release(items);
    if (!b) return NULL;
    if (!*b->title && !*b->body) {
        // Some toast providers expose a single accessible announcement.
        BSTR name=NULL;
        if (SUCCEEDED(IUIAutomationElement_get_CurrentName(el,&name)) && name && *name)
            w_append(b->body,ARRAYSIZE(b->body),name);
        SysFreeString(name);
    }
    if (!*b->title && !*b->body) {free(b);return NULL;}
    if (!*b->app) wcscpy(b->app,L"Windows");
    GetSystemTimeAsFileTime(&b->time);
    return b;
}

static HRESULT STDMETHODCALLTYPE w_opened(IUIAutomationEventHandler *self,IUIAutomationElement *sender,EVENTID event) {
    w_banner_subscription *s=(w_banner_subscription *)self;
    if (!sender || event!=UIA_Window_WindowOpenedEventId || WaitForSingleObject(s->stop,0)!=WAIT_TIMEOUT) return S_OK;
    int pid=0;
    if (FAILED(IUIAutomationElement_get_CachedProcessId(sender,&pid)) || !pid || !w_shell_process((DWORD)pid)) return S_OK;
    BOOL offscreen=TRUE;
    CONTROLTYPEID type=0;
    BSTR id=NULL;
    RECT bounds={0};
    if (FAILED(IUIAutomationElement_get_CurrentControlType(sender,&type)) || type!=UIA_WindowControlTypeId ||
        FAILED(IUIAutomationElement_get_CurrentAutomationId(sender,&id))) return S_OK;
    bool toast=w_toast_id(id);SysFreeString(id);
    if (!toast || FAILED(IUIAutomationElement_get_CurrentIsOffscreen(sender,&offscreen)) || offscreen ||
        FAILED(IUIAutomationElement_get_CurrentBoundingRectangle(sender,&bounds)) || IsRectEmpty(&bounds) ||
        !MonitorFromRect(&bounds,MONITOR_DEFAULTTONULL)) return S_OK;

    // Unsubscription may finish while an already queued UIA callback unwinds.
    AcquireSRWLockShared(&s->lock);
    IUIAutomationTreeWalker *walker=s->walker;
    IUIAutomationCondition *text=s->text;
    IUIAutomationCacheRequest *cache=s->cache;
    if (walker) IUIAutomationTreeWalker_AddRef(walker);
    if (text) IUIAutomationCondition_AddRef(text);
    if (cache) IUIAutomationCacheRequest_AddRef(cache);
    ReleaseSRWLockShared(&s->lock);
    w_banner *b=NULL;
    uint64_t runtime=0;
    if (walker && text && cache) {
        HWND hwnd=w_host_window(sender,walker);
        if (hwnd && w_shell_window(hwnd,(DWORD)pid)) {
            runtime=w_runtime_id(sender,hwnd);
            b=w_read_text(sender,cache,text);
        }
    }
    if (walker) IUIAutomationTreeWalker_Release(walker);
    if (text) IUIAutomationCondition_Release(text);
    if (cache) IUIAutomationCacheRequest_Release(cache);
    if (!b) return S_OK;
    uint64_t content=w_hash(0,b->app,wcslen(b->app)*sizeof(wchar_t));
    content=w_hash(content,b->title,(wcslen(b->title)+1)*sizeof(wchar_t));
    content=w_hash(content,b->body,wcslen(b->body)*sizeof(wchar_t));
    ULONGLONG tick=GetTickCount64();
    AcquireSRWLockExclusive(&s->lock);
    bool duplicate=false;
    for (size_t i=0;i<ARRAYSIZE(s->seen);++i)
        if (s->seen[i].element==runtime && s->seen[i].content==content && tick-s->seen[i].tick<1000) duplicate=true;
    if (!duplicate && s->receiver && PostMessageW(s->receiver,s->message,0,(LPARAM)b)) {
        b=NULL;
        unsigned i=s->next++%ARRAYSIZE(s->seen);
        s->seen[i].element=runtime;s->seen[i].content=content;s->seen[i].tick=tick;
    }
    ReleaseSRWLockExclusive(&s->lock);
    free(b);
    return S_OK;
}

static IUIAutomationEventHandlerVtbl handler={w_query,w_retain,w_release,w_opened};

static DWORD WINAPI w_listen(void *arg) {
    w_banner_subscription *s=arg;
    IUIAutomation2 *uia=NULL;
    IUIAutomationElement *root=NULL;
    IUIAutomationCacheRequest *event_cache=NULL;
    HRESULT init=CoInitializeEx(NULL,COINIT_MULTITHREADED);
    HRESULT hr=init;
    if (SUCCEEDED(hr)) hr=CoCreateInstance(&CLSID_CUIAutomation8,NULL,CLSCTX_INPROC_SERVER,
                                           &IID_IUIAutomation2,(void **)&uia);
    if (SUCCEEDED(hr)) {
        IUIAutomation2_put_ConnectionTimeout(uia,600);
        IUIAutomation2_put_TransactionTimeout(uia,900);
        hr=IUIAutomation2_GetRootElement(uia,&root);
    }
    if (SUCCEEDED(hr)) hr=IUIAutomation2_CreateCacheRequest(uia,&event_cache);
    if (SUCCEEDED(hr)) hr=IUIAutomationCacheRequest_AddProperty(event_cache,UIA_ProcessIdPropertyId);
    if (SUCCEEDED(hr)) hr=IUIAutomation2_get_ControlViewWalker(uia,&s->walker);
    VARIANT type={.vt=VT_I4,.lVal=UIA_TextControlTypeId};
    VARIANT on_screen={.vt=VT_BOOL,.boolVal=VARIANT_FALSE};
    IUIAutomationCondition *text_type=NULL,*visible=NULL;
    if (SUCCEEDED(hr)) hr=IUIAutomation2_CreatePropertyCondition(uia,UIA_ControlTypePropertyId,type,&text_type);
    if (SUCCEEDED(hr)) hr=IUIAutomation2_CreatePropertyCondition(uia,UIA_IsOffscreenPropertyId,on_screen,&visible);
    if (SUCCEEDED(hr)) hr=IUIAutomation2_CreateAndCondition(uia,text_type,visible,&s->text);
    if (visible) IUIAutomationCondition_Release(visible);
    if (text_type) IUIAutomationCondition_Release(text_type);
    if (SUCCEEDED(hr)) hr=IUIAutomation2_CreateCacheRequest(uia,&s->cache);
    const PROPERTYID properties[]={UIA_NamePropertyId,UIA_AutomationIdPropertyId,UIA_IsOffscreenPropertyId};
    for (size_t i=0;SUCCEEDED(hr) && i<ARRAYSIZE(properties);++i)
        hr=IUIAutomationCacheRequest_AddProperty(s->cache,properties[i]);
    if (SUCCEEDED(hr)) hr=IUIAutomationCacheRequest_put_AutomationElementMode(s->cache,AutomationElementMode_None);
    if (SUCCEEDED(hr) && WaitForSingleObject(s->stop,0)==WAIT_TIMEOUT)
        hr=IUIAutomation2_AddAutomationEventHandler(uia,UIA_Window_WindowOpenedEventId,root,TreeScope_Subtree,event_cache,&s->iface);
    else if (SUCCEEDED(hr)) hr=E_ABORT;
    AcquireSRWLockShared(&s->lock);
    if (s->receiver) PostMessageW(s->receiver,s->message,(WPARAM)hr,0);
    ReleaseSRWLockShared(&s->lock);
    if (SUCCEEDED(hr)) {
        WaitForSingleObject(s->stop,INFINITE);
        IUIAutomation2_RemoveAutomationEventHandler(uia,UIA_Window_WindowOpenedEventId,root,&s->iface);
    }
    AcquireSRWLockExclusive(&s->lock);
    IUIAutomationTreeWalker *walker=s->walker; s->walker=NULL;
    IUIAutomationCondition *text=s->text; s->text=NULL;
    IUIAutomationCacheRequest *cache=s->cache; s->cache=NULL;
    ReleaseSRWLockExclusive(&s->lock);
    if (walker) IUIAutomationTreeWalker_Release(walker);
    if (text) IUIAutomationCondition_Release(text);
    if (cache) IUIAutomationCacheRequest_Release(cache);
    if (event_cache) IUIAutomationCacheRequest_Release(event_cache);
    if (root) IUIAutomationElement_Release(root);
    if (uia) IUIAutomation2_Release(uia);
    if (SUCCEEDED(init)) CoUninitialize();
    w_release(&s->iface);
    return 0;
}

bool w_banners_start(HWND receiver,UINT message) {
    if (active) return active->receiver==receiver && active->message==message;
    if (!receiver || !message) return false;
    w_banner_subscription *s=calloc(1,sizeof(*s));
    if (!s) return false;
    s->iface.lpVtbl=&handler;s->refs=2;s->receiver=receiver;s->message=message;
    s->stop=CreateEventW(NULL,TRUE,FALSE,NULL);
    HANDLE thread=s->stop ? CreateThread(NULL,0,w_listen,s,0,NULL) : NULL;
    if (!thread) {if (s->stop) CloseHandle(s->stop);free(s);return false;}
    CloseHandle(thread);
    active=s;
    return true;
}

void w_banners_stop(void) {
    w_banner_subscription *s=active;
    if (!s) return;
    active=NULL;
    AcquireSRWLockExclusive(&s->lock);
    HWND receiver=s->receiver;
    s->receiver=NULL;
    SetEvent(s->stop);
    ReleaseSRWLockExclusive(&s->lock);
    MSG queued;
    while (PeekMessageW(&queued,receiver,s->message,s->message,PM_REMOVE)) free((void *)queued.lParam);
    w_release(&s->iface);
}
