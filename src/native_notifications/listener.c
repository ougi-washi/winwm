#define COBJMACROS
#define WIDL_using_Windows_UI_Notifications
#define WIDL_using_Windows_ApplicationModel
#define WIDL_using_Windows_Foundation
#define WIDL_using_Windows_Foundation_Collections
#include "native_notifications/listener.h"
#include "native_notifications/toast.h"
#include <roapi.h>
#include <winstring.h>
#include <shobjidl.h>
#include <windows.ui.notifications.h>
#include <stdlib.h>
#include <wchar.h>
#ifndef RO_E_CLOSED
#define RO_E_CLOSED _HRESULT_TYPEDEF_(0x80000013L)
#endif

// The MinGW notification header includes the notification objects, collection
// and vector async specialization. These Management interfaces are the missing
// Windows.Foundation.UniversalApiContract v3 declarations from the Windows SDK.
typedef struct w_listener w_listener;
typedef struct w_access_op w_access_op;
typedef struct w_handler w_handler;
typedef struct {
    IInspectableVtbl base;
    HRESULT (STDMETHODCALLTYPE *request)(w_listener *,w_access_op **);
    HRESULT (STDMETHODCALLTYPE *status)(w_listener *,int *);
    HRESULT (STDMETHODCALLTYPE *subscribe)(w_listener *,w_handler *,EventRegistrationToken *);
    HRESULT (STDMETHODCALLTYPE *unsubscribe)(w_listener *,EventRegistrationToken);
    HRESULT (STDMETHODCALLTYPE *list)(w_listener *,NotificationKinds,IAsyncOperation_IVectorView_UserNotification **);
    HRESULT (STDMETHODCALLTYPE *get)(w_listener *,UINT32,IUserNotification **);
    HRESULT (STDMETHODCALLTYPE *clear)(w_listener *);
    HRESULT (STDMETHODCALLTYPE *remove)(w_listener *,UINT32);
} w_listener_vtbl;
struct w_listener { const w_listener_vtbl *vt; };
typedef struct w_factory w_factory;
typedef struct {
    IInspectableVtbl base;
    HRESULT (STDMETHODCALLTYPE *current)(w_factory *,w_listener **);
} w_factory_vtbl;
struct w_factory { const w_factory_vtbl *vt; };
typedef struct {
    IInspectableVtbl base;
    HRESULT (STDMETHODCALLTYPE *completed)(w_access_op *,w_handler *);
    HRESULT (STDMETHODCALLTYPE *get_completed)(w_access_op *,w_handler **);
    HRESULT (STDMETHODCALLTYPE *results)(w_access_op *,int *);
} w_access_vtbl;
struct w_access_op { const w_access_vtbl *vt; };
static const GUID factory_id = {0xff6123cf,0x4386,0x4aa3,{0xb7,0x3d,0xb8,0x04,0xe5,0xb6,0x3b,0x23}};
static const GUID changed_id = {0x10242902,0xb897,0x5507,{0x99,0x22,0x2c,0x0a,0x7d,0x34,0x46,0x4d}};
static const GUID access_done_id = {0xf09e843a,0x13cb,0x559b,{0xa9,0xfc,0x01,0x57,0x22,0xc2,0xcd,0x57}};
static const GUID list_done_id = {0x9e42ed08,0x45b3,0x5643,{0xb5,0xc7,0xb2,0x16,0xf5,0x78,0x15,0x94}};
static const GUID async_info_id = {0x00000036,0,0,{0xc0,0,0,0,0,0,0,0x46}};
enum { history_limit = 512 };

typedef struct {
    LONG refs;
    HWND window;
    UINT message;
    HANDLE stop, wake, worker;
    SRWLOCK lock;
    DWORD ids[history_limit];
    int pending;
    bool refresh, access_pending, stopping;
    HRESULT consent_error;
    // Owner-thread only.
    bool apartment;
    w_access_op *consent;
    // Worker-thread only.
    enum w_notification_access access;
    HRESULT error;
    bool reported, seeded;
    DWORD changes;
    w_native_notification *seen;
    size_t seen_count, seen_capacity;
} w_capture;
static w_capture *active;

static void w_drop(void *obj) {
    if (obj) IUnknown_Release((IUnknown *)obj);
}

static void w_release(w_capture *c) {
    if (InterlockedDecrement(&c->refs)) return;
    if (c->stop) CloseHandle(c->stop);
    if (c->wake) CloseHandle(c->wake);
    free(c->seen);
    free(c);
}

static HRESULT w_current(w_listener **out) {
    const wchar_t name[] = L"Windows.UI.Notifications.Management.UserNotificationListener";
    HSTRING cls = NULL;
    HRESULT hr = WindowsCreateString(name,ARRAYSIZE(name)-1,&cls);
    w_factory *factory = NULL;
    if (SUCCEEDED(hr)) hr = RoGetActivationFactory(cls,&factory_id,(void **)&factory);
    if (SUCCEEDED(hr)) hr = factory ? factory->vt->current(factory,out) : E_UNEXPECTED;
    if (SUCCEEDED(hr) && !*out) hr = E_UNEXPECTED;
    w_drop(factory);
    WindowsDeleteString(cls);
    return hr;
}

struct w_handler {
    const void *vtable;
    LONG refs;
    const GUID *iid;
    w_capture *capture;
    HANDLE done;
};

static ULONG STDMETHODCALLTYPE w_addref(w_handler *h) { return (ULONG)InterlockedIncrement(&h->refs); }

static ULONG STDMETHODCALLTYPE w_unref(w_handler *h) {
    ULONG count = (ULONG)InterlockedDecrement(&h->refs);
    if (!count) {
        if (h->done) CloseHandle(h->done);
        if (h->capture) w_release(h->capture);
        free(h);
    }
    return count;
}

static HRESULT STDMETHODCALLTYPE w_query(w_handler *h, REFIID id, void **out) {
    if (!out) return E_POINTER;
    *out = NULL;
    if (!IsEqualIID(id,h->iid) && !IsEqualIID(id,&IID_IUnknown) && !IsEqualIID(id,&IID_IAgileObject))
        return E_NOINTERFACE;
    *out = h;
    w_addref(h);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE w_list_done(w_handler *h, void *operation, AsyncStatus status) {
    (void)operation;
    (void)status;
    SetEvent(h->done);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE w_access_done(w_handler *h, void *operation, AsyncStatus status) {
    (void)status;
    w_access_op *op = operation;
    int access;
    HRESULT hr = op->vt->results(op,&access);
    w_capture *c = h->capture;
    AcquireSRWLockExclusive(&c->lock);
    c->access_pending = false;
    c->consent_error = SUCCEEDED(hr) && access == notification_denied ? E_ACCESSDENIED : hr;
    c->refresh = true;
    SetEvent(c->wake);
    ReleaseSRWLockExclusive(&c->lock);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE w_changed(w_handler *h, w_listener *sender, IUserNotificationChangedEventArgs *args) {
    (void)sender;
    w_capture *c = h->capture;
    UserNotificationChangedKind kind;
    UINT32 id = 0;
    HRESULT hr = args ? args->lpVtbl->get_ChangeKind(args,&kind) : E_POINTER;
    bool added = SUCCEEDED(hr) && kind == UserNotificationChangedKind_Added;
    if (added) hr = args->lpVtbl->get_UserNotificationId(args,&id);
    AcquireSRWLockExclusive(&c->lock);
    if (FAILED(hr)) c->refresh = true;
    else if (added && !c->refresh) {
        int i = 0;
        while (i < c->pending && c->ids[i] != id) ++i;
        if (i == history_limit) c->refresh = true;
        else if (i == c->pending) c->ids[c->pending++] = id;
    }
    SetEvent(c->wake);
    ReleaseSRWLockExclusive(&c->lock);
    return S_OK;
}

#define handler_base \
    HRESULT (STDMETHODCALLTYPE *query)(w_handler *,REFIID,void **); \
    ULONG (STDMETHODCALLTYPE *addref)(w_handler *); \
    ULONG (STDMETHODCALLTYPE *release)(w_handler *)
static const struct { handler_base; HRESULT (STDMETHODCALLTYPE *invoke)(w_handler *,void *,AsyncStatus); }
    list_vt = {w_query,w_addref,w_unref,w_list_done}, access_vt = {w_query,w_addref,w_unref,w_access_done};
static const struct { handler_base; HRESULT (STDMETHODCALLTYPE *invoke)(w_handler *,w_listener *,IUserNotificationChangedEventArgs *); }
    change_vt = {w_query,w_addref,w_unref,w_changed};

static w_handler *w_handler_new(w_capture *c, const GUID *iid, const void *vt) {
    w_handler *h = calloc(1,sizeof(*h));
    if (!h) return NULL;
    if (c) InterlockedIncrement(&c->refs);
    h->capture = c;
    h->refs = 1;
    h->iid = iid;
    h->vtable = vt;
    return h;
}

static void w_cancel(void *operation) {
    IAsyncInfo *info = NULL;
    if (operation && SUCCEEDED(IUnknown_QueryInterface((IUnknown *)operation,&async_info_id,(void **)&info))) {
        IAsyncInfo_Cancel(info);
        IAsyncInfo_Release(info);
    }
}

static HRESULT w_list(w_capture *c, w_listener *listener, IVectorView_UserNotification **result) {
    if (WaitForSingleObject(c->stop,0) == WAIT_OBJECT_0) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    IAsyncOperation_IVectorView_UserNotification *op = NULL;
    HRESULT hr = listener->vt->list(listener,NotificationKinds_Toast,&op);
    if (FAILED(hr)) return hr;
    if (!op) return E_UNEXPECTED;
    w_handler *h = w_handler_new(NULL,&list_done_id,&list_vt);
    if (!h || !(h->done = CreateEventW(NULL,TRUE,FALSE,NULL))) hr = E_OUTOFMEMORY;
    if (SUCCEEDED(hr))
        hr = op->lpVtbl->put_Completed(op,(IAsyncOperationCompletedHandler_IVectorView_UserNotification *)h);
    if (SUCCEEDED(hr)) {
        HANDLE wait[] = {c->stop,h->done};
        DWORD which = WaitForMultipleObjects(2,wait,FALSE,15000);
        hr = which == WAIT_OBJECT_0+1 ? op->lpVtbl->GetResults(op,result) :
             HRESULT_FROM_WIN32(which == WAIT_TIMEOUT ? ERROR_TIMEOUT : ERROR_CANCELLED);
    }
    if (FAILED(hr)) w_cancel(op);
    w_drop(op);
    if (h) w_unref(h);
    return hr;
}

static void w_append_text(wchar_t *out, size_t cap, HSTRING value) {
    UINT32 length = 0;
    const wchar_t *s = WindowsGetStringRawBuffer(value,&length);
    size_t n = wcslen(out);
    if (n && length && n+1 < cap) out[n++] = L'\n';
    for (UINT32 i=0; i<length && n+1<cap; ++i)
        out[n++] = s[i] < L' ' && s[i] != L'\n' ? L' ' : s[i];
    if (n && out[n-1] >= 0xd800 && out[n-1] <= 0xdbff) --n;
    out[n] = 0;
}

static void w_application(IUserNotification *user, w_native_notification *entry) {
    IAppInfo *app = NULL;
    IAppDisplayInfo *display = NULL;
    HSTRING id = NULL, name = NULL;
    if (SUCCEEDED(user->lpVtbl->get_AppInfo(user,&app)) && app) {
        if (SUCCEEDED(app->lpVtbl->get_DisplayInfo(app,&display)) && display)
            display->lpVtbl->get_DisplayName(display,&name);
        app->lpVtbl->get_AppUserModelId(app,&id);
    }
    entry->from_winwm = WindowsGetStringLen(id) &&
                        !_wcsicmp(WindowsGetStringRawBuffer(id,NULL),WINWM_NOTIFICATION_AUMID);
    w_append_text(entry->app,ARRAYSIZE(entry->app),WindowsGetStringLen(name) ? name : id);
    if (!*entry->app) wcscpy(entry->app,L"Windows");
    WindowsDeleteString(name);
    WindowsDeleteString(id);
    w_drop(display);
    w_drop(app);
}

static void w_content(IUserNotification *user, HSTRING generic, w_native_notification *entry) {
    INotification *notice = NULL;
    INotificationVisual *visual = NULL;
    INotificationBinding *binding = NULL;
    IVectorView_AdaptiveNotificationText *texts = NULL;
    if (SUCCEEDED(user->lpVtbl->get_Notification(user,&notice)) && notice &&
        SUCCEEDED(notice->lpVtbl->get_Visual(notice,&visual)) && visual &&
        SUCCEEDED(visual->lpVtbl->GetBinding(visual,generic,&binding)) && binding &&
        SUCCEEDED(binding->lpVtbl->GetTextElements(binding,&texts)) && texts) {
        UINT32 count = 0;
        texts->lpVtbl->get_Size(texts,&count);
        for (UINT32 i=0; i<count && wcslen(entry->body)+1 < ARRAYSIZE(entry->body); ++i) {
            IAdaptiveNotificationText *text = NULL;
            HSTRING value = NULL;
            if (SUCCEEDED(texts->lpVtbl->GetAt(texts,i,&text)) && text &&
                SUCCEEDED(text->lpVtbl->get_Text(text,&value))) {
                if (!*entry->title) w_append_text(entry->title,ARRAYSIZE(entry->title),value);
                else w_append_text(entry->body,ARRAYSIZE(entry->body),value);
            }
            WindowsDeleteString(value);
            w_drop(text);
        }
    }
    w_drop(texts);
    w_drop(binding);
    w_drop(visual);
    w_drop(notice);
    if (!*entry->title) wcscpy(entry->title,L"Notification");
}

typedef struct {
    IUserNotification *user;
    DWORD id;
    INT64 created;
} w_item;

static int w_time_order(INT64 a, DWORD x, INT64 b, DWORD y) {
    if (a != b) return a < b ? -1 : 1;
    return (x > y)-(x < y);
}

static int w_compare(const void *left, const void *right) {
    const w_item *a = left, *b = right;
    return w_time_order(a->created,a->id,b->created,b->id);
}

static int w_known(const w_capture *c, const w_native_notification *entry) {
    for (size_t i=0; i<c->seen_count; ++i)
        if (c->seen[i].id == entry->id && c->seen[i].created == entry->created) return (int)i;
    return -1;
}

static void w_remember(w_capture *c, const w_native_notification *entry) {
    int i = w_known(c,entry);
    if (i < 0 && c->seen_count < history_limit) i = (int)c->seen_count++;
    if (i < 0) {
        i = 0;
        for (int j=1; j<history_limit; ++j)
            if (w_time_order(c->seen[j].created,c->seen[j].id,c->seen[i].created,c->seen[i].id) < 0) i=j;
    }
    c->seen[i] = *entry;
}

static bool w_report(w_capture *c, enum w_notification_access access, HRESULT hr,
                     w_native_notification *rows, size_t count) {
    size_t changes = 0, added = 0;
    const w_native_notification *oldest = NULL;
    if (count && c->seen_count == history_limit) {
        oldest = c->seen;
        for (size_t i=1; i<c->seen_count; ++i)
            if (w_time_order(c->seen[i].created,c->seen[i].id,oldest->created,oldest->id)<0) oldest = c->seen+i;
    }
    for (size_t i=0; i<count; ++i) {
        w_native_notification *entry = &rows[i];
        if (i+1 < count && rows[i+1].created == entry->created && rows[i+1].id == entry->id) continue;
        int known = w_known(c,entry);
        if (known < 0 && oldest && w_time_order(entry->created,entry->id,oldest->created,oldest->id)<0) continue;
        if (known >= 0 && !wcscmp(c->seen[known].app,entry->app) && !wcscmp(c->seen[known].title,entry->title) &&
            !wcscmp(c->seen[known].body,entry->body)) continue;
        entry->toast = c->seeded && known < 0;
        if (known < 0) ++added;
        if (changes != i) rows[changes] = *entry;
        ++changes;
    }
    if (!changes && c->reported && c->access == access && c->error == hr) return true;
    size_t needed = min(c->seen_count+added,history_limit);
    if (needed > c->seen_capacity) {
        size_t capacity = min(max(needed,max(c->seen_capacity*2,8)),history_limit);
        w_native_notification *seen = realloc(c->seen,capacity*sizeof(*seen));
        if (!seen) return false;
        c->seen = seen;
        c->seen_capacity = capacity;
    }
    size_t bytes = changes*sizeof(*rows);
    w_notification_snapshot *snapshot = malloc(sizeof(*snapshot)+bytes);
    if (!snapshot) return false;
    snapshot->access = access;
    snapshot->error = hr;
    snapshot->count = changes;
    if (changes) memcpy(snapshot->entries,rows,bytes);
    AcquireSRWLockShared(&c->lock);
    bool posted = !c->stopping && PostMessageW(c->window,c->message,0,(LPARAM)snapshot);
    ReleaseSRWLockShared(&c->lock);
    if (!posted) { free(snapshot); return false; }
    c->reported = true;
    c->access = access;
    c->error = hr;
    if (changes) ++c->changes;
    for (size_t i=0; i<changes; ++i) w_remember(c,&rows[i]);
    return true;
}

static HRESULT w_collect(w_capture *c, w_listener *listener, HSTRING generic, const DWORD *ids,
                         size_t count, bool full) {
    IVectorView_UserNotification *view = NULL;
    UINT32 size = (UINT32)count;
    HRESULT hr = S_OK;
    if (full) {
        hr = w_list(c,listener,&view);
        if (SUCCEEDED(hr)) hr = view ? view->lpVtbl->get_Size(view,&size) : E_UNEXPECTED;
    }
    w_item *items = SUCCEEDED(hr) && size ? calloc(size,sizeof(*items)) : NULL;
    if (SUCCEEDED(hr) && size && !items) hr = E_OUTOFMEMORY;
    size_t used = 0;
    for (UINT32 i=0; SUCCEEDED(hr) && i<size; ++i) {
        if (WaitForSingleObject(c->stop,0) == WAIT_OBJECT_0) {hr=HRESULT_FROM_WIN32(ERROR_CANCELLED);break;}
        IUserNotification *user = NULL;
        HRESULT got = view ? view->lpVtbl->GetAt(view,i,&user) : listener->vt->get(listener,ids[i],&user);
        UINT32 id;
        DateTime time;
        if (SUCCEEDED(got) && user && SUCCEEDED(user->lpVtbl->get_Id(user,&id)) &&
            SUCCEEDED(user->lpVtbl->get_CreationTime(user,&time)))
            items[used++] = (w_item){user,id,time.UniversalTime};
        else w_drop(user); // An item can be dismissed between the event and this read.
    }
    if (used > 1) qsort(items,used,sizeof(*items),w_compare);
    size_t limit = min(used,history_limit), start = limit;
    w_native_notification *rows = limit ? calloc(limit,sizeof(*rows)) : NULL;
    if (limit && !rows) hr = E_OUTOFMEMORY;
    for (size_t i=used; SUCCEEDED(hr) && i && start; --i) {
        if (WaitForSingleObject(c->stop,0) == WAIT_OBJECT_0) {hr=HRESULT_FROM_WIN32(ERROR_CANCELLED);break;}
        w_item *item = &items[i-1];
        w_native_notification *entry = &rows[start-1];
        w_application(item->user,entry);
        entry->id = item->id;
        entry->created = item->created;
        w_content(item->user,generic,entry);
        --start;
    }
    if (SUCCEEDED(hr)) {
        int access = 0;
        hr = listener->vt->status(listener,&access);
        if (SUCCEEDED(hr) && access != notification_allowed) {
            c->seeded = false;
            w_report(c,access == notification_denied ? notification_denied : notification_unspecified,S_OK,NULL,0);
            hr = S_FALSE;
        } else if (SUCCEEDED(hr)) {
            if (w_report(c,notification_allowed,S_OK,rows ? rows+start : NULL,limit-start)) c->seeded = true;
            else hr = E_FAIL;
        }
    }
    for (size_t i=0; i<used; ++i) w_drop(items[i].user);
    free(rows);
    free(items);
    w_drop(view);
    return hr;
}

static bool w_transient(HRESULT hr) {
    return hr == RPC_E_DISCONNECTED || hr == RPC_E_SERVER_DIED || hr == RPC_E_SERVER_DIED_DNE ||
           hr == RO_E_CLOSED || hr == HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE) ||
           hr == HRESULT_FROM_WIN32(RPC_S_CALL_FAILED) || hr == HRESULT_FROM_WIN32(ERROR_TIMEOUT) ||
           hr == HRESULT_FROM_WIN32(ERROR_RETRY) || hr == E_PENDING || hr == E_FAIL || hr == E_ABORT;
}

static DWORD WINAPI w_capture_thread(void *arg) {
    w_capture *c = arg;
    HRESULT apartment = RoInitialize(RO_INIT_MULTITHREADED);
    w_listener *listener = NULL;
    w_handler *changed = NULL;
    EventRegistrationToken token = {0};
    HSTRING generic = NULL;
    HRESULT init = apartment;
    if (SUCCEEDED(init)) init = WindowsCreateString(L"ToastGeneric",12,&generic);
    HANDLE waits[] = {c->stop,c->wake};
    DWORD backoff = 1000, poll = 1000;
    ULONGLONG next_subscription = 0;
    bool timed = false;
    for (;;) {
        if (WaitForSingleObject(c->stop,0) == WAIT_OBJECT_0) break;
        DWORD ids[history_limit];
        AcquireSRWLockExclusive(&c->lock);
        bool requested = c->refresh, full = requested || timed;
        int count = c->pending;
        memcpy(ids,c->ids,(size_t)count*sizeof(*ids));
        c->refresh = false;
        c->pending = 0;
        HRESULT consent = c->consent_error;
        ReleaseSRWLockExclusive(&c->lock);
        HRESULT hr = init;
        if (SUCCEEDED(hr) && !listener) hr = w_current(&listener);
        int status = notification_unspecified;
        if (SUCCEEDED(hr)) hr = listener->vt->status(listener,&status);
        enum w_notification_access access = hr == E_ACCESSDENIED ? notification_denied : FAILED(hr) ? notification_unavailable :
            status == notification_allowed ? notification_allowed :
            status == notification_denied ? notification_denied : notification_unspecified;
        DWORD wait_ms = INFINITE;
        if (access == notification_allowed) {
            if (!changed && (requested || GetTickCount64() >= next_subscription)) {
                changed = w_handler_new(c,&changed_id,&change_vt);
                HRESULT subscribed = changed ? listener->vt->subscribe(listener,changed,&token) : E_OUTOFMEMORY;
                if (FAILED(subscribed) && changed) { w_unref(changed); changed = NULL; }
                next_subscription = GetTickCount64() + (w_transient(subscribed) ? 5000 : 60000);
            }
            DWORD prior = c->changes;
            hr = w_collect(c,listener,generic,ids,count,full || !c->seeded);
            if (FAILED(hr)) w_report(c,access,hr,NULL,0);
            if (hr == S_OK && !changed) {
                // Some brokers support history reads but reject the foreground event.
                // Retry the subscription separately from the adaptive reconciliation.
                poll = c->changes != prior || requested ? 1000 : min(poll*2,5000);
                wait_ms = poll;
            }
        } else {
            if (!w_transient(hr)) c->seeded = false;
            // A status probe is not a user-initiated denial. An unpackaged
            // process may report Denied before it has ever asked for access.
            HRESULT reason = access == notification_denied ? consent : FAILED(hr) ? hr : consent;
            w_report(c,access,reason,NULL,0);
        }
        if (access != notification_allowed || hr != S_OK) {
            if (changed) {
                listener->vt->unsubscribe(listener,token);
                w_unref(changed);
                changed = NULL;
            }
            next_subscription = 0;
            if (FAILED(hr)) { w_drop(listener); listener = NULL; }
        }
        if (w_transient(hr)) {
            wait_ms = backoff;
            backoff = min(backoff*2,30000);
        } else backoff = 1000;
        DWORD wait = WaitForMultipleObjects(2,waits,FALSE,wait_ms);
        timed = wait == WAIT_TIMEOUT;
        if (!timed && wait != WAIT_OBJECT_0+1) break;
    }
    if (changed) {
        listener->vt->unsubscribe(listener,token);
        w_unref(changed);
    }
    w_drop(listener);
    WindowsDeleteString(generic);
    if (SUCCEEDED(apartment)) RoUninitialize();
    w_release(c);
    return 0;
}

bool w_notification_listen(HWND target, UINT message) {
    if (active) return active->window == target && active->message == message;
    if (!target || !message) return false;
    w_capture *c = calloc(1,sizeof(*c));
    if (!c) return false;
    c->refs = 2; // Owner and worker.
    c->window = target;
    c->message = message;
    c->refresh = true;
    c->stop = CreateEventW(NULL,TRUE,FALSE,NULL);
    c->wake = CreateEventW(NULL,FALSE,FALSE,NULL);
    if (c->stop && c->wake) c->worker = CreateThread(NULL,0,w_capture_thread,c,0,NULL);
    if (!c->worker) { c->refs = 1; w_release(c); return false; }
    active = c;
    return true;
}

void w_notification_refresh(void) {
    w_capture *c = active;
    if (!c) return;
    AcquireSRWLockExclusive(&c->lock);
    c->refresh = true;
    SetEvent(c->wake);
    ReleaseSRWLockExclusive(&c->lock);
}

HRESULT w_notification_request(HWND owner) {
    w_capture *c = active;
    if (!c || !owner || GetWindowThreadProcessId(owner,NULL) != GetCurrentThreadId()) return E_INVALIDARG;
    AcquireSRWLockShared(&c->lock);
    bool pending = c->access_pending;
    bool explicit_denial = c->consent_error == E_ACCESSDENIED;
    ReleaseSRWLockShared(&c->lock);
    if (pending) return S_FALSE;
    w_drop(c->consent);
    c->consent = NULL;
    HRESULT hr = S_OK;
    if (!c->apartment) {
        hr = RoInitialize(RO_INIT_SINGLETHREADED);
        c->apartment = SUCCEEDED(hr);
        if (hr == RPC_E_CHANGED_MODE) hr = S_OK;
    }
    w_listener *listener = NULL;
    if (SUCCEEDED(hr)) hr = w_current(&listener);
    int status = notification_unspecified;
    if (SUCCEEDED(hr)) hr = listener->vt->status(listener,&status);
    if (listener && hr == E_ACCESSDENIED && !explicit_denial) {
        status = notification_denied;
        hr = S_OK;
    }
    if (SUCCEEDED(hr) && (status == notification_unspecified || (status == notification_denied && !explicit_denial))) {
        IInitializeWithWindow *host = NULL;
        if (SUCCEEDED(IUnknown_QueryInterface((IUnknown *)listener,&IID_IInitializeWithWindow,(void **)&host))) {
            hr = IInitializeWithWindow_Initialize(host,owner);
            w_drop(host);
        }
        if (SUCCEEDED(hr)) hr = listener->vt->request(listener,&c->consent);
        if (SUCCEEDED(hr) && !c->consent) hr = E_UNEXPECTED;
        if (SUCCEEDED(hr)) {
            w_handler *handler = w_handler_new(c,&access_done_id,&access_vt);
            if (handler) {
                AcquireSRWLockExclusive(&c->lock);
                c->access_pending = true;
                ReleaseSRWLockExclusive(&c->lock);
                hr = c->consent->vt->completed(c->consent,handler);
                w_unref(handler);
            } else hr = E_OUTOFMEMORY;
        }
    } else if (SUCCEEDED(hr) && status == notification_denied) hr = E_ACCESSDENIED;
    if (FAILED(hr)) {
        w_cancel(c->consent);
        AcquireSRWLockExclusive(&c->lock);
        c->access_pending = false;
        c->consent_error = hr;
        ReleaseSRWLockExclusive(&c->lock);
    }
    w_drop(listener);
    w_notification_refresh();
    return hr;
}

void w_notification_stop(void) {
    w_capture *c = active;
    if (!c) return;
    active = NULL;
    AcquireSRWLockExclusive(&c->lock);
    c->stopping = true;
    ReleaseSRWLockExclusive(&c->lock);
    SetEvent(c->stop);
    w_cancel(c->consent);
    w_drop(c->consent);
    CloseHandle(c->worker);
    MSG msg;
    while (PeekMessageW(&msg,c->window,c->message,c->message,PM_REMOVE)) free((void *)msg.lParam);
    if (c->apartment) RoUninitialize();
    w_release(c);
}
