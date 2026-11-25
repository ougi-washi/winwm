#include "tray/host.h"
#include <shellapi.h>
#include <stdlib.h>
#include <string.h>

// Shell32 marshals these handles as 32-bit values even for a 64-bit caller.
// Current Windows appends implementation data after the notification structure.
typedef struct {
    DWORD size, window, id, flags, callback, icon;
    WCHAR tip[128];
    DWORD state, mask;
    WCHAR body[256];
    DWORD version;
    WCHAR title[64];
    DWORD info_flags;
    GUID guid;
    DWORD balloon_icon;
} w_wire_icon;

typedef struct { DWORD signature, operation; w_wire_icon icon; } w_wire_update;
typedef struct { DWORD signature, operation, size, reserved, window, id; GUID guid; } w_wire_query;
_Static_assert(sizeof(w_wire_icon)==956 && sizeof(w_wire_query)==40,"shell tray wire layout");

typedef struct {
    w_tray_icon item;
    UINT callback, version;
    DWORD thread;
    uint64_t balloon_sequence;
    w_tray_balloon balloon;
} w_record;

static const wchar_t tray_class[]=L"Shell_TrayWnd";
static const DWORD wire_signature=0x34753423;
static HWND tray_window, notify_window;
static UINT notify_message;
static bool posted, registered;
static w_record *records;
static size_t used, capacity;
static uint64_t next_token, next_balloon;
static bool (*locate_bounds)(uint64_t,RECT *);

static bool w_has_guid(const GUID *guid) {
    static const GUID empty;
    return memcmp(guid,&empty,sizeof(*guid))!=0;
}

static void w_changed(void) {
    if (!posted && notify_window) posted=PostMessageW(notify_window,notify_message,0,0)!=0;
}

void w_tray_ack(void) { posted=false; }
bool w_tray_is_active(void) { return tray_window!=NULL; }
size_t w_tray_count(void) { return used; }

bool w_tray_get(size_t index,w_tray_icon *out) {
    if (!out || index>=used) return false;
    *out=records[index].item;
    return true;
}

static w_record *w_by_token(uint64_t token) {
    for (size_t i=0;i<used;++i) if (records[i].item.token==token) return records+i;
    return NULL;
}

static w_record *w_find(HWND owner,UINT id,const GUID *guid) {
    for (size_t i=0;i<used;++i) {
        w_tray_icon *item=&records[i].item;
        if (guid ? IsEqualGUID(guid,&item->guid) :
            (!w_has_guid(&item->guid) && item->owner==owner && item->id==id)) return records+i;
    }
    return NULL;
}

static bool w_alive(const w_record *r) {
    DWORD pid=0,thread=GetWindowThreadProcessId(r->item.owner,&pid);
    return thread && thread==r->thread && pid==r->item.pid;
}

static void w_remove(w_record *r) {
    if (r->item.icon) DestroyIcon(r->item.icon);
    size_t index=(size_t)(r-records);
    memmove(r,r+1,(used-index-1)*sizeof(*r));
    if (!--used) KillTimer(tray_window,1);
    w_changed();
}

void w_tray_prune(HWND owner) {
    size_t live=0;
    for (size_t i=0;i<used;++i) {
        w_record *r=records+i;
        if ((!owner || owner==r->item.owner) && !w_alive(r)) {
            if (r->item.icon) DestroyIcon(r->item.icon);
            continue;
        }
        if (live!=i) records[live]=*r;
        ++live;
    }
    if (live!=used) {
        used=live;
        if (!used) KillTimer(tray_window,1);
        w_changed();
    }
}

static w_record *w_new(void) {
    // Bound USER handles and memory even when one client supplies arbitrary IDs.
    if (used>=4096 || next_token>=INT64_MAX) return NULL;
    if (used==capacity) {
        size_t n=capacity ? capacity*2 : 16;
        w_record *p=realloc(records,n*sizeof(*p));
        if (!p) return NULL;
        records=p; capacity=n;
    }
    if (!used) SetCoalescableTimer(tray_window,1,15000,NULL,3000);
    w_record *r=records+used++;
    ZeroMemory(r,sizeof(*r));
    r->item.token=++next_token;
    return r;
}

static bool w_send(const w_record *r,UINT event,POINT screen) {
    if (!r->callback) return false;
    WPARAM wp=r->item.id;
    LPARAM lp=event;
    if (r->version>=NOTIFYICON_VERSION_4) {
        wp=MAKEWPARAM(screen.x,screen.y);
        lp=MAKELPARAM(event,r->item.id);
    }
    return PostMessageW(r->item.owner,r->callback,wp,lp)!=0;
}

bool w_tray_action(uint64_t token,enum w_tray_action action,POINT screen) {
    w_record *r=w_by_token(token);
    if (!r || r->item.hidden || !r->callback) return false;
    if (!w_alive(r)) { w_remove(r); return false; }
    UINT down,up,select=0;
    switch (action) {
    case w_tray_keyboard:
        if (r->version>=NOTIFYICON_VERSION) {
            AllowSetForegroundWindow(r->item.pid);
            return w_send(r,NIN_KEYSELECT,screen);
        }
        // Legacy keyboard activation is a left click.
        /* fall through */
    case w_tray_primary: down=WM_LBUTTONDOWN; up=WM_LBUTTONUP; select=NIN_SELECT; break;
    case w_tray_double: down=WM_LBUTTONDBLCLK; up=WM_LBUTTONUP; select=NIN_SELECT; break;
    case w_tray_context: down=WM_RBUTTONDOWN; up=WM_RBUTTONUP; select=WM_CONTEXTMENU; break;
    case w_tray_middle: down=WM_MBUTTONDOWN; up=WM_MBUTTONUP; break;
    default: return false;
    }
    AllowSetForegroundWindow(r->item.pid);
    return w_send(r,down,screen) && w_send(r,up,screen) &&
           (!select || r->version<NOTIFYICON_VERSION || w_send(r,select,screen));
}

bool w_tray_next_balloon(w_tray_balloon *out) {
    if (!out) return false;
    w_record *oldest=NULL;
    for (size_t i=0;i<used;++i) {
        w_record *r=records+i;
        if (r->balloon_sequence && (!oldest || r->balloon_sequence<oldest->balloon_sequence)) oldest=r;
    }
    if (!oldest) return false;
    *out=oldest->balloon;
    oldest->balloon_sequence=0;
    return true;
}

void w_tray_balloon_event(uint64_t token,UINT event) {
    if (event!=NIN_BALLOONSHOW && event!=NIN_BALLOONHIDE && event!=NIN_BALLOONTIMEOUT &&
        event!=NIN_BALLOONUSERCLICK) return;
    w_record *r=w_by_token(token);
    if (r && w_alive(r)) {
        RECT bounds={0};
        if (locate_bounds) locate_bounds(token,&bounds);
        POINT p={bounds.left,bounds.bottom};
        if (event==NIN_BALLOONUSERCLICK) AllowSetForegroundWindow(r->item.pid);
        w_send(r,event,p);
    }
}

static void w_copy_text(wchar_t *out,size_t cap,const wchar_t *in,size_t n) {
    size_t i=0;
    while (i<n && i+1<cap && in[i]) { out[i]=in[i]; ++i; }
    out[i]=0;
}

#define W_WIRE_END(field) (offsetof(w_wire_icon,field)+sizeof(((w_wire_icon *)0)->field))

static LRESULT w_update(const COPYDATASTRUCT *data,HWND sender) {
    if (data->cbData<offsetof(w_wire_update,icon)+W_WIRE_END(icon)) return FALSE;
    const w_wire_update *request=data->lpData;
    const w_wire_icon *wire=&request->icon;
    if (request->signature!=wire_signature || wire->size<W_WIRE_END(icon) ||
        wire->size>data->cbData - offsetof(w_wire_update,icon)) return FALSE;
    HWND owner=LongToHandle((LONG)wire->window);
    if (owner!=sender) return FALSE;
    DWORD pid=0, thread=owner ? GetWindowThreadProcessId(owner,&pid) : 0;
    const GUID *guid=NULL;
    if (wire->flags & NIF_GUID) {
        if (wire->size<W_WIRE_END(guid) || !w_has_guid(&wire->guid)) return FALSE;
        guid=&wire->guid;
    }
    if (!thread && !guid) return FALSE;
    w_record *r=w_find(owner,wire->id,guid);
    if (r && !w_alive(r)) { w_remove(r); r=NULL; }
    if (r && owner && (!thread || r->item.pid!=pid)) return FALSE;
    switch (request->operation) {
    case NIM_DELETE:
        if (!r) return FALSE;
        w_remove(r); return TRUE;
    case NIM_SETFOCUS: return r!=NULL;
    case NIM_SETVERSION:
        if (!r || wire->size<W_WIRE_END(version) || wire->version>NOTIFYICON_VERSION_4) return FALSE;
        r->version=wire->version; return TRUE;
    case NIM_ADD:
        if (r || !thread) return FALSE;
        break;
    case NIM_MODIFY:
        if (!r) return FALSE;
        break;
    default: return FALSE;
    }
    if ((wire->flags & NIF_STATE) && wire->size<W_WIRE_END(mask)) return FALSE;
    if ((wire->flags & NIF_INFO) && wire->size<W_WIRE_END(info_flags)) return FALSE;
    HICON icon=NULL;
    if ((wire->flags & NIF_ICON) && wire->icon) {
        icon=CopyIcon((HICON)LongToHandle((LONG)wire->icon));
        if (!icon) return FALSE;
    }
    bool changed=false;
    if (!r) {
        r=w_new();
        if (!r) { if (icon) DestroyIcon(icon); return FALSE; }
        r->item.id=wire->id;
        if (guid) r->item.guid=*guid;
        changed=true;
    }
    if (thread && r->item.owner!=owner) {
        r->item.owner=owner; r->item.pid=pid; r->thread=thread; r->item.id=wire->id;
        changed=true;
    }
    if (wire->flags & NIF_MESSAGE)
        r->callback=wire->callback>=WM_USER && wire->callback<=0xffffu ? wire->callback : 0;
    if (wire->flags & NIF_ICON) {
        if (r->item.icon) DestroyIcon(r->item.icon);
        r->item.icon=icon;
        changed=true;
    }
    if (wire->flags & NIF_TIP) {
        wchar_t tip[128];
        size_t n=(wire->size - offsetof(w_wire_icon,tip))/sizeof(wchar_t);
        w_copy_text(tip,ARRAYSIZE(tip),wire->tip,n);
        if (wcscmp(r->item.tip,tip)) { wcscpy(r->item.tip,tip); changed=true; }
    }
    if ((wire->flags & NIF_STATE) && (wire->mask & NIS_HIDDEN)) {
        bool hidden=(wire->state & NIS_HIDDEN)!=0;
        changed|=hidden!=r->item.hidden;
        r->item.hidden=hidden;
    }
    if (wire->flags & NIF_INFO) {
        w_tray_balloon *b=&r->balloon;
        b->token=r->item.token; b->owner=r->item.owner; b->pid=r->item.pid;
        wcscpy(b->tip,r->item.tip);
        w_copy_text(b->body,ARRAYSIZE(b->body),wire->body,ARRAYSIZE(wire->body));
        w_copy_text(b->title,ARRAYSIZE(b->title),wire->title,ARRAYSIZE(wire->title));
        b->flags=wire->info_flags;
        r->balloon_sequence=++next_balloon;
        changed=true;
    }
    if (changed) w_changed();
    return TRUE;
}

static LRESULT w_geometry(const COPYDATASTRUCT *data) {
    if (data->cbData<sizeof(w_wire_query)) return 0;
    const w_wire_query *q=data->lpData;
    if (q->signature!=wire_signature || q->size<sizeof(*q)-8) return 0;
    w_record *r=w_find(LongToHandle((LONG)q->window),q->id,w_has_guid(&q->guid) ? &q->guid : NULL);
    if (!r) return 0;
    RECT rect;
    if (!locate_bounds || !locate_bounds(r->item.token,&rect) || IsRectEmpty(&rect)) return 0;
    if (q->operation==1) return MAKELRESULT(rect.left,rect.top);
    if (q->operation==2) return MAKELRESULT(rect.right-rect.left,rect.bottom-rect.top);
    return 0;
}

static LRESULT CALLBACK w_host_proc(HWND w,UINT msg,WPARAM wp,LPARAM lp) {
    if (msg==WM_COPYDATA) {
        const COPYDATASTRUCT *data=(const COPYDATASTRUCT *)lp;
        if (!data || !data->lpData) return FALSE;
        if (data->dwData==1) return w_update(data,(HWND)wp);
        if (data->dwData==3) return w_geometry(data);
        return FALSE;
    }
    if (msg==WM_TIMER && wp==1) { w_tray_prune(NULL); return 0; }
    if (msg==WM_CLOSE) return 0;
    return DefWindowProcW(w,msg,wp,lp);
}

bool w_tray_start(HWND owner,UINT changed_message,bool (*locate)(uint64_t,RECT *)) {
    if (tray_window) return true;
    if (!IsWindow(owner) || !changed_message || FindWindowW(tray_class,NULL)) return false;
    HINSTANCE module=GetModuleHandleW(NULL);
    WNDCLASSW wc={.hInstance=module,.lpfnWndProc=w_host_proc,.lpszClassName=tray_class};
    if (!RegisterClassW(&wc)) return false;
    registered=true;
    tray_window=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,tray_class,L"",WS_POPUP,
                                0,0,0,0,NULL,NULL,module,NULL);
    if (!tray_window || FindWindowW(tray_class,NULL)!=tray_window) { w_tray_stop(); return false; }
    notify_window=owner;
    notify_message=changed_message;
    locate_bounds=locate;
    UINT created=RegisterWindowMessageW(L"TaskbarCreated");
    if (created) PostMessageW(HWND_BROADCAST,created,0,0);
    return true;
}

void w_tray_stop(void) {
    locate_bounds=NULL;
    notify_window=NULL;
    notify_message=0;
    posted=false;
    for (size_t i=0;i<used;++i) if (records[i].item.icon) DestroyIcon(records[i].item.icon);
    free(records); records=NULL; used=capacity=0;
    if (tray_window) DestroyWindow(tray_window);
    tray_window=NULL;
    if (registered) {
        UnregisterClassW(tray_class,GetModuleHandleW(NULL));
        registered=false;
    }
}
