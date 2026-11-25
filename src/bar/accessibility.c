#define COBJMACROS
#include "bar/accessibility.h"
#include "bar/tooltip.h"
#include <oleacc.h>
#include <stdio.h>

typedef struct {
    LONG id;
    int source, workspace;
    char instance[56], action[128];
    wchar_t value[256];
    RECT bounds;
    bool active;
} w_access_cell;

typedef struct w_bar_access {
    IAccessible iface;
    LONG refs, last_id;
    w_bar_window *bar;
    w_access_cell *cells;
    int count;
    ITypeInfo *type_info;
} w_bar_access;

typedef struct {
    IEnumVARIANT iface;
    IAccessible *owner;
    LONG refs;
    ULONG pos, count;
    LONG *ids;
} w_child_enum;

static HRESULT w_enum_copy(const LONG *ids, ULONG count, ULONG pos, IEnumVARIANT **out);

static HRESULT STDMETHODCALLTYPE w_enum_query(IEnumVARIANT *iface, REFIID iid, void **out) {
    if (!out) return E_POINTER;
    *out=NULL;
    w_child_enum *e=(w_child_enum *)iface;
    if (!IsEqualIID(iid,&IID_IEnumVARIANT)) {
        if (e->owner) return IAccessible_QueryInterface(e->owner,iid,out);
        if (!IsEqualIID(iid,&IID_IUnknown)) return E_NOINTERFACE;
    }
    *out=iface;
    IEnumVARIANT_AddRef(iface);
    return S_OK;
}
static ULONG STDMETHODCALLTYPE w_enum_add(IEnumVARIANT *iface) {
    return InterlockedIncrement(&((w_child_enum *)iface)->refs);
}
static ULONG STDMETHODCALLTYPE w_enum_release(IEnumVARIANT *iface) {
    LONG refs=InterlockedDecrement(&((w_child_enum *)iface)->refs);
    if (!refs) {
        w_child_enum *e=(w_child_enum *)iface;
        if (e->owner) IAccessible_Release(e->owner);
        free(e->ids);
        free(e);
    }
    return refs;
}
static HRESULT STDMETHODCALLTYPE w_enum_next(IEnumVARIANT *iface, ULONG count, VARIANT *children, ULONG *fetched) {
    if (!children || (!fetched && count!=1)) return E_POINTER;
    w_child_enum *e=(w_child_enum *)iface;
    ULONG n=min(count,e->count-e->pos);
    for (ULONG i=0; i<n; ++i)
        children[i]=(VARIANT){.vt=VT_I4,.lVal=e->ids[e->pos++]};
    if (fetched) *fetched=n;
    return n==count ? S_OK : S_FALSE;
}
static HRESULT STDMETHODCALLTYPE w_enum_skip(IEnumVARIANT *iface, ULONG count) {
    w_child_enum *e=(w_child_enum *)iface;
    ULONG n=min(count,e->count-e->pos);
    e->pos+=n;
    return n==count ? S_OK : S_FALSE;
}
static HRESULT STDMETHODCALLTYPE w_enum_reset(IEnumVARIANT *iface) {
    w_child_enum *e=(w_child_enum *)iface;
    e->pos=0;
    if (e->owner) {
        // The standard cross-process proxy can retain this enumerator between
        // AccessibleChildren calls. Reset begins a snapshot of the current layout.
        w_bar_access *a=(w_bar_access *)e->owner;
        if (!a->bar) return CO_E_OBJNOTCONNECTED;
        if ((ULONG)a->count!=e->count) {
            LONG *ids=a->count ? malloc(sizeof(*ids)*(size_t)a->count) : NULL;
            if (a->count && !ids) return E_OUTOFMEMORY;
            free(e->ids);
            e->ids=ids;
            e->count=(ULONG)a->count;
        }
        for (int i=0; i<a->count; ++i) e->ids[i]=a->cells[i].id;
    }
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE w_enum_clone(IEnumVARIANT *iface, IEnumVARIANT **out) {
    w_child_enum *e=(w_child_enum *)iface;
    return w_enum_copy(e->ids,e->count,e->pos,out);
}
static IEnumVARIANTVtbl enum_methods={w_enum_query,w_enum_add,w_enum_release,w_enum_next,w_enum_skip,w_enum_reset,w_enum_clone};

static HRESULT w_enum_copy(const LONG *ids, ULONG count, ULONG pos, IEnumVARIANT **out) {
    if (!out) return E_POINTER;
    *out=NULL;
    w_child_enum *e=malloc(sizeof(*e));
    if (!e) return E_OUTOFMEMORY;
    e->ids=count ? malloc(sizeof(LONG)*count) : NULL;
    if (count && !e->ids) { free(e); return E_OUTOFMEMORY; }
    e->iface.lpVtbl=&enum_methods;
    e->owner=NULL;
    e->refs=1;
    e->pos=pos;
    e->count=count;
    if (ids && count) memcpy(e->ids,ids,count*sizeof(*ids));
    *out=&e->iface;
    return S_OK;
}

static int w_index(w_bar_access *a,LONG id) {
    if (id>0)
        for (int i=0; i<a->count; ++i) if (a->cells[i].id==id) return i;
    return -1;
}

static HRESULT STDMETHODCALLTYPE w_query(IAccessible *iface, REFIID iid, void **out) {
    if (!out) return E_POINTER;
    *out=NULL;
    if (IsEqualIID(iid,&IID_IUnknown) || IsEqualIID(iid,&IID_IDispatch) || IsEqualIID(iid,&IID_IAccessible)) {
        *out=iface;
        IAccessible_AddRef(iface);
        return S_OK;
    }
    if (IsEqualIID(iid,&IID_IEnumVARIANT)) {
        IEnumVARIANT *children;
        HRESULT hr=w_enum_copy(NULL,0,0,&children);
        if (FAILED(hr)) return hr;
        w_child_enum *e=(w_child_enum *)children;
        e->owner=iface;
        IAccessible_AddRef(iface);
        hr=w_enum_reset(children);
        if (FAILED(hr)) { IEnumVARIANT_Release(children); return hr; }
        *out=children;
        return S_OK;
    }
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE w_add(IAccessible *iface) {
    return InterlockedIncrement(&((w_bar_access *)iface)->refs);
}
static ULONG STDMETHODCALLTYPE w_release(IAccessible *iface) {
    w_bar_access *a=(w_bar_access *)iface;
    LONG refs=InterlockedDecrement(&a->refs);
    if (!refs) {
        if (a->type_info) ITypeInfo_Release(a->type_info);
        free(a->cells);
        free(a);
    }
    return refs;
}
static HRESULT STDMETHODCALLTYPE w_type_count(IAccessible *iface,UINT *out) {
    (void)iface;
    if (!out) return E_POINTER;
    *out=1;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE w_type(IAccessible *iface,UINT index,LCID locale,ITypeInfo **out) {
    if (!out) return E_POINTER;
    *out=NULL;
    if (index) return DISP_E_BADINDEX;
    w_bar_access *a=(w_bar_access *)iface;
    if (!a->type_info) {
        ITypeLib *lib=NULL;
        HRESULT hr=LoadRegTypeLib(&LIBID_Accessibility,1,1,locale,&lib);
        if (FAILED(hr)) return hr;
        hr=ITypeLib_GetTypeInfoOfGuid(lib,&IID_IAccessible,&a->type_info);
        ITypeLib_Release(lib);
        if (FAILED(hr)) return hr;
    }
    ITypeInfo_AddRef(a->type_info);
    *out=a->type_info;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE w_ids(IAccessible *iface,REFIID iid,LPOLESTR *names,UINT count,LCID locale,DISPID *out) {
    if (!IsEqualIID(iid,&IID_NULL)) return DISP_E_UNKNOWNINTERFACE;
    ITypeInfo *info=NULL;
    HRESULT hr=w_type(iface,0,locale,&info);
    if (SUCCEEDED(hr)) { hr=DispGetIDsOfNames(info,names,count,out); ITypeInfo_Release(info); }
    return hr;
}
static HRESULT STDMETHODCALLTYPE w_dispatch(IAccessible *iface,DISPID member,REFIID iid,LCID locale,WORD flags,
                                           DISPPARAMS *params,VARIANT *result,EXCEPINFO *exception,UINT *argerr) {
    if (!IsEqualIID(iid,&IID_NULL)) return DISP_E_UNKNOWNINTERFACE;
    ITypeInfo *info=NULL;
    HRESULT hr=w_type(iface,0,locale,&info);
    if (SUCCEEDED(hr)) { hr=DispInvoke(iface,info,member,flags,params,result,exception,argerr); ITypeInfo_Release(info); }
    return hr;
}

static HRESULT w_find(IAccessible *iface,VARIANT child,struct w_bar_hit **hit) {
    w_bar_access *a=(w_bar_access *)iface;
    *hit=NULL;
    if (!a->bar) return CO_E_OBJNOTCONNECTED;
    if (child.vt!=VT_I4) return E_INVALIDARG;
    if (child.lVal==CHILDID_SELF) return S_OK;
    int i=w_index(a,child.lVal);
    if (i<0 || i>=a->bar->hit_count) return E_INVALIDARG;
    *hit=&a->bar->hits[i];
    return S_OK;
}
static HRESULT w_string(BSTR *out,const wchar_t *text) {
    if (!text || !*text) return S_FALSE;
    *out=SysAllocString(text);
    return *out ? S_OK : E_OUTOFMEMORY;
}
static HRESULT STDMETHODCALLTYPE w_parent(IAccessible *iface,IDispatch **out) {
    if (!out) return E_POINTER;
    *out=NULL;
    w_bar_window *b=((w_bar_access *)iface)->bar;
    return b ? AccessibleObjectFromWindow(b->hwnd,OBJID_WINDOW,&IID_IDispatch,(void **)out) : CO_E_OBJNOTCONNECTED;
}
static HRESULT STDMETHODCALLTYPE w_count(IAccessible *iface,LONG *out) {
    if (!out) return E_POINTER;
    w_bar_access *a=(w_bar_access *)iface;
    *out=a->count;
    return a->bar ? S_OK : CO_E_OBJNOTCONNECTED;
}
static HRESULT STDMETHODCALLTYPE w_child(IAccessible *iface,VARIANT child,IDispatch **out) {
    if (!out) return E_POINTER;
    *out=NULL;
    struct w_bar_hit *hit;
    HRESULT hr=w_find(iface,child,&hit);
    return SUCCEEDED(hr) ? S_FALSE : hr;
}
static HRESULT STDMETHODCALLTYPE w_name(IAccessible *iface,VARIANT child,BSTR *out) {
    if (!out) return E_POINTER;
    *out=NULL;
    struct w_bar_hit *hit;
    HRESULT hr=w_find(iface,child,&hit);
    if (FAILED(hr)) return hr;
    wchar_t name[160];
    if (!hit) swprintf(name,count_of(name),L"winwm toolbar, monitor %d",((w_bar_access *)iface)->bar->output+1);
    else {
        const w_bar_description *info=w_bar_describe(hit->module);
        if (hit->tray_more) return w_string(out,hit->tooltip);
        if (hit->tray_key) {
            *out=SysAllocStringLen(hit->tooltip,(UINT)wcscspn(hit->tooltip,L"\r\n"));
            return *out ? S_OK : E_OUTOFMEMORY;
        }
        if (hit->workspace) swprintf(name,count_of(name),L"Workspace %d",hit->workspace);
        else if (info) wcscpy(name,info->heading);
        else if (!w_os_wide(!strncmp(hit->module,"custom/",7) ? hit->module+7 : hit->module,name,count_of(name)))
            wcscpy(name,L"Toolbar module");
        const char *tag=strchr(hit->instance,'#');
        wchar_t suffix[56];
        if (tag && w_os_wide(tag+1,suffix,count_of(suffix))) { wcscat(name,L" · "); wcscat(name,suffix); }
    }
    return w_string(out,name);
}
static HRESULT STDMETHODCALLTYPE w_value(IAccessible *iface,VARIANT child,BSTR *out) {
    if (!out) return E_POINTER;
    *out=NULL;
    struct w_bar_hit *hit;
    HRESULT hr=w_find(iface,child,&hit);
    return FAILED(hr) ? hr : hit ? w_string(out,hit->tooltip) : S_FALSE;
}
static bool w_actionable(const struct w_bar_hit *hit) {
    return *hit->action || !strcmp(hit->module,"audio") || !strcmp(hit->module,"tray");
}
static HRESULT STDMETHODCALLTYPE w_role(IAccessible *iface,VARIANT child,VARIANT *out) {
    if (!out) return E_POINTER;
    VariantInit(out);
    struct w_bar_hit *hit;
    HRESULT hr=w_find(iface,child,&hit);
    if (FAILED(hr)) return hr;
    out->vt=VT_I4;
    out->lVal=!hit ? ROLE_SYSTEM_TOOLBAR : hit->workspace ? ROLE_SYSTEM_RADIOBUTTON :
              w_actionable(hit) ? ROLE_SYSTEM_PUSHBUTTON : ROLE_SYSTEM_STATICTEXT;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE w_acc_state(IAccessible *iface,VARIANT child,VARIANT *out) {
    if (!out) return E_POINTER;
    VariantInit(out);
    struct w_bar_hit *hit;
    HRESULT hr=w_find(iface,child,&hit);
    if (FAILED(hr)) return hr;
    HWND hwnd=((w_bar_access *)iface)->bar->hwnd;
    out->vt=VT_I4;
    out->lVal=IsWindowVisible(hwnd) ? 0 : STATE_SYSTEM_INVISIBLE;
    if ((GetWindowLongPtrW(hwnd,GWL_EXSTYLE)&WS_EX_TRANSPARENT) || !IsWindowEnabled(hwnd))
        out->lVal|=STATE_SYSTEM_UNAVAILABLE;
    if (hit && hit->workspace && hit->active) out->lVal|=STATE_SYSTEM_CHECKED;
    if (hit && !hit->tray_key && !hit->tray_more && !strcmp(hit->module,"tray")) {
        w_bar_window *b=((w_bar_access *)iface)->bar;
        out->lVal|=b->tray.open && b->tray.source==hit->source ? STATE_SYSTEM_EXPANDED : STATE_SYSTEM_COLLAPSED;
        if (!b->tray.total) out->lVal|=STATE_SYSTEM_UNAVAILABLE;
    }
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE w_no_text(IAccessible *iface,VARIANT child,BSTR *out) {
    if (!out) return E_POINTER;
    *out=NULL;
    struct w_bar_hit *hit;
    HRESULT hr=w_find(iface,child,&hit);
    return FAILED(hr) ? hr : S_FALSE;
}
static HRESULT STDMETHODCALLTYPE w_help_topic(IAccessible *iface,BSTR *file,VARIANT child,LONG *topic) {
    if (!topic) return E_POINTER;
    *topic=0;
    return w_no_text(iface,child,file);
}
static HRESULT STDMETHODCALLTYPE w_none(IAccessible *iface,VARIANT *out) {
    if (!out) return E_POINTER;
    VariantInit(out);
    return ((w_bar_access *)iface)->bar ? S_FALSE : CO_E_OBJNOTCONNECTED;
}
static HRESULT STDMETHODCALLTYPE w_default(IAccessible *iface,VARIANT child,BSTR *out) {
    if (!out) return E_POINTER;
    *out=NULL;
    struct w_bar_hit *hit;
    HRESULT hr=w_find(iface,child,&hit);
    if (FAILED(hr) || !hit || !w_actionable(hit)) return FAILED(hr) ? hr : S_FALSE;
    if (hit->tray_key) return w_string(out,L"Activate application");
    if (hit->tray_more) return w_string(out,L"Next applications");
    const w_bar_description *info=w_bar_describe(hit->module);
    if (!(hit->style && *hit->style->action) && info && info->click) return w_string(out,info->click);
    wchar_t command[128];
    return w_os_wide(hit->action,command,count_of(command)) ? w_string(out,command) : S_FALSE;
}
static HRESULT STDMETHODCALLTYPE w_select(IAccessible *iface,LONG flags,VARIANT child) {
    (void)flags;
    struct w_bar_hit *hit;
    HRESULT hr=w_find(iface,child,&hit);
    return FAILED(hr) ? hr : E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE w_location(IAccessible *iface,LONG *x,LONG *y,LONG *w,LONG *h,VARIANT child) {
    if (!x || !y || !w || !h) return E_POINTER;
    *x=*y=*w=*h=0;
    struct w_bar_hit *hit;
    HRESULT hr=w_find(iface,child,&hit);
    if (FAILED(hr)) return hr;
    HWND hwnd=((w_bar_access *)iface)->bar->hwnd;
    RECT r;
    if (hit) r=hit->box;
    else if (!GetClientRect(hwnd,&r)) return E_FAIL;
    MapWindowPoints(hwnd,NULL,(POINT *)&r,2);
    *x=r.left; *y=r.top; *w=r.right-r.left; *h=r.bottom-r.top;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE w_navigate(IAccessible *iface,LONG direction,VARIANT from,VARIANT *out) {
    if (!out) return E_POINTER;
    VariantInit(out);
    struct w_bar_hit *hit;
    HRESULT hr=w_find(iface,from,&hit);
    if (FAILED(hr)) return hr;
    w_bar_access *a=(w_bar_access *)iface;
    int i=hit ? (int)(hit-a->bar->hits) : -1;
    RECT client; GetClientRect(a->bar->hwnd,&client);
    bool vertical=client.bottom>client.right;
    if (!hit && direction==NAVDIR_FIRSTCHILD) i=0;
    else if (!hit && direction==NAVDIR_LASTCHILD) i=a->count-1;
    else if (hit && (direction==NAVDIR_NEXT || direction==(vertical ? NAVDIR_DOWN : NAVDIR_RIGHT))) ++i;
    else if (hit && (direction==NAVDIR_PREVIOUS || direction==(vertical ? NAVDIR_UP : NAVDIR_LEFT))) --i;
    else return S_FALSE;
    if (i<0 || i>=a->count) return S_FALSE;
    *out=(VARIANT){.vt=VT_I4,.lVal=a->cells[i].id};
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE w_hit_test(IAccessible *iface,LONG x,LONG y,VARIANT *out) {
    if (!out) return E_POINTER;
    VariantInit(out);
    w_bar_access *a=(w_bar_access *)iface;
    if (!a->bar) return CO_E_OBJNOTCONNECTED;
    RECT r; POINT p={x,y};
    if (!IsWindowVisible(a->bar->hwnd) || !ScreenToClient(a->bar->hwnd,&p) ||
        !GetClientRect(a->bar->hwnd,&r) || !PtInRect(&r,p)) return S_FALSE;
    out->vt=VT_I4;
    out->lVal=CHILDID_SELF;
    for (int i=0; i<a->count; ++i) if (PtInRect(&a->cells[i].bounds,p)) { out->lVal=a->cells[i].id; break; }
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE w_invoke(IAccessible *iface,VARIANT child) {
    struct w_bar_hit *hit;
    HRESULT hr=w_find(iface,child,&hit);
    if (FAILED(hr)) return hr;
    if (!hit || !w_actionable(hit)) return S_FALSE;
    HWND hwnd=((w_bar_access *)iface)->bar->hwnd;
    if (!IsWindowVisible(hwnd) || !IsWindowEnabled(hwnd) || (GetWindowLongPtrW(hwnd,GWL_EXSTYLE)&WS_EX_TRANSPARENT))
        return S_FALSE;
    return PostMessageW(hwnd,bar_access_activate,(WPARAM)child.lVal,0) ? S_OK : E_FAIL;
}
static HRESULT STDMETHODCALLTYPE w_readonly(IAccessible *iface,VARIANT child,BSTR value) {
    (void)value;
    struct w_bar_hit *hit;
    HRESULT hr=w_find(iface,child,&hit);
    return FAILED(hr) ? hr : E_NOTIMPL;
}
static IAccessibleVtbl methods={
    w_query,w_add,w_release,w_type_count,w_type,w_ids,w_dispatch,w_parent,w_count,w_child,w_name,w_value,w_no_text,
    w_role,w_acc_state,w_no_text,w_help_topic,w_no_text,w_none,w_none,w_default,w_select,w_location,w_navigate,w_hit_test,
    w_invoke,w_readonly,w_readonly
};

static bool w_same_cell(const w_access_cell *old,const struct w_bar_hit *hit) {
    return old->source==hit->source && old->workspace==hit->workspace &&
           !strcmp(old->instance,hit->instance) && !strcmp(old->action,hit->action);
}

// An in-process accessibility hook may synchronously query or rearrange the bar.
// Deliver its event after the current paint has released all layout pointers.
static void w_event(w_bar_window *bar,DWORD event,LONG child) {
    if (IsWinEventHookInstalled(event)) PostMessageW(bar->hwnd,bar_access_event,event,(LPARAM)child);
}
void w_bar_access_notify(w_bar_window *bar,DWORD event,LONG child) {
    if (!bar->access) return;
    if (event==EVENT_OBJECT_REORDER && child==CHILDID_SELF)
        NotifyWinEvent(event,bar->hwnd,OBJID_CLIENT,child);
    else if ((event==EVENT_OBJECT_VALUECHANGE || event==EVENT_OBJECT_STATECHANGE || event==EVENT_OBJECT_LOCATIONCHANGE) &&
             w_index(bar->access,child)>=0)
        NotifyWinEvent(event,bar->hwnd,OBJID_CLIENT,child);
}

void w_bar_access_layout(w_bar_window *bar) {
    w_bar_access *a=bar->access;
    int count=bar->hit_count;
    bool reordered=a->count!=count;
    for (int i=0; !reordered && i<count; ++i) reordered=!w_same_cell(&a->cells[i],&bar->hits[i]);
    if (reordered) {
        w_access_cell *next=count ? calloc((size_t)count,sizeof(*next)) : NULL;
        if (count && !next) count=0;
        for (int i=0; i<count; ++i) {
            const struct w_bar_hit *hit=&bar->hits[i];
            w_access_cell *cell=&next[i];
            for (int j=0; j<a->count; ++j) {
                if (w_same_cell(&a->cells[j],hit)) { cell->id=a->cells[j].id; break; }
            }
            if (!cell->id) {
                // Retired IDs are never recycled, including after desktop topology changes.
                if (a->last_id==LONG_MAX) { count=i; break; }
                cell->id=++a->last_id;
            }
            cell->source=hit->source; cell->workspace=hit->workspace;
            strcpy(cell->instance,hit->instance); strcpy(cell->action,hit->action);
            cell->bounds=hit->box; cell->active=hit->active; wcscpy(cell->value,hit->tooltip);
        }
        free(a->cells); a->cells=next; a->count=count;
        w_event(bar,EVENT_OBJECT_REORDER,CHILDID_SELF);
    } else {
        for (int i=0; i<count; ++i) {
            w_access_cell *cell=&a->cells[i];
            const struct w_bar_hit *hit=&bar->hits[i];
            if (wcscmp(cell->value,hit->tooltip)) {
                wcscpy(cell->value,hit->tooltip);
                w_event(bar,EVENT_OBJECT_VALUECHANGE,cell->id);
            }
            if (hit->active!=cell->active) {
                cell->active=hit->active;
                w_event(bar,EVENT_OBJECT_STATECHANGE,cell->id);
            }
            if (!EqualRect(&cell->bounds,&hit->box)) {
                cell->bounds=hit->box;
                w_event(bar,EVENT_OBJECT_LOCATIONCHANGE,cell->id);
            }
        }
    }
}

LRESULT w_bar_access_object(w_bar_window *bar,WPARAM request) {
    if (!bar->access) {
        // Marshal calls back to the UI apartment. The session's normal loop pumps COM messages.
        if (FAILED(CoInitializeEx(NULL,COINIT_APARTMENTTHREADED))) return 0;
        w_bar_access *a=calloc(1,sizeof(*a));
        if (!a) { CoUninitialize(); return 0; }
        a->iface.lpVtbl=&methods; a->refs=1; a->bar=bar;
        bar->access=a;
        w_bar_access_layout(bar);
    }
    return LresultFromObject(&IID_IAccessible,request,(IUnknown *)&bar->access->iface);
}
void w_bar_access_activate(w_bar_window *bar,LONG child) {
    if (!bar->access || !IsWindowVisible(bar->hwnd) || !IsWindowEnabled(bar->hwnd) ||
        (GetWindowLongPtrW(bar->hwnd,GWL_EXSTYLE)&WS_EX_TRANSPARENT)) return;
    int i=w_index(bar->access,child);
    if (i>=0 && i<bar->hit_count && w_same_cell(&bar->access->cells[i],&bar->hits[i]))
        w_bar_activate_hit(bar,i);
}
void w_bar_access_close(w_bar_window *bar) {
    w_bar_access *a=bar->access;
    if (!a) return;
    bar->access=NULL;
    a->bar=NULL;
    a->count=0;
    CoDisconnectObject((IUnknown *)&a->iface,0);
    IAccessible_Release(&a->iface);
    CoUninitialize();
}
