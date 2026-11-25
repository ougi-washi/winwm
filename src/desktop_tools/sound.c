#define COBJMACROS
#include "desktop_tools/sound.h"
#include "desktop_tools/rows.h"
#include <initguid.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <stdio.h>

typedef struct {
    LPWSTR id;
    IAudioEndpointVolume *endpoint;
    ISimpleAudioVolume *session;
    bool is_default, calls_default;
    wchar_t label[512];
} w_sound_row;

static w_sound_row *rows;
static int used;
typedef struct {
    w_sound_row *rows;
    int used, capacity;
} w_sound_inventory;
static IMMDeviceEnumerator *devices;
static bool watching;
static LONG changed;
static int last_view = -1;
static wchar_t *selection[sound_view_count];
static int scroll[sound_view_count];

// The notification object lives for the lifetime of this helper process. Audio
// service callbacks only mark the inventory dirty; the panel owns all COM rows.
static ULONG STDMETHODCALLTYPE w_ref(IMMNotificationClient *self) { (void)self; return 2; }
static ULONG STDMETHODCALLTYPE w_unref(IMMNotificationClient *self) { (void)self; return 1; }
static HRESULT STDMETHODCALLTYPE w_query(IMMNotificationClient *self, REFIID id, void **out) {
    if (!out) return E_POINTER;
    *out = NULL;
    if (!IsEqualIID(id,&IID_IUnknown) && !IsEqualIID(id,&IID_IMMNotificationClient)) return E_NOINTERFACE;
    *out = self;
    w_ref(self);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE w_device_event(IMMNotificationClient *self, LPCWSTR id) {
    (void)self; (void)id;
    InterlockedExchange(&changed,1);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE w_state_event(IMMNotificationClient *self, LPCWSTR id, DWORD state) {
    (void)state;
    return w_device_event(self,id);
}
static HRESULT STDMETHODCALLTYPE w_role_event(IMMNotificationClient *self, EDataFlow flow, ERole role, LPCWSTR id) {
    (void)flow; (void)role;
    return w_device_event(self,id);
}
static HRESULT STDMETHODCALLTYPE w_property_event(IMMNotificationClient *self, LPCWSTR id, const PROPERTYKEY key) {
    if (key.pid == PKEY_Device_FriendlyName.pid && IsEqualGUID(&key.fmtid,&PKEY_Device_FriendlyName.fmtid))
        return w_device_event(self,id);
    return S_OK;
}
static IMMNotificationClientVtbl listener_methods = {
    w_query,w_ref,w_unref,w_state_event,w_device_event,w_device_event,w_role_event,w_property_event
};
static IMMNotificationClient listener = {&listener_methods};

bool w_sound_watch(void) {
    if (!devices && FAILED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_INPROC_SERVER,
                                           &IID_IMMDeviceEnumerator, (void **)&devices))) return false;
    if (!watching) watching = SUCCEEDED(IMMDeviceEnumerator_RegisterEndpointNotificationCallback(devices,&listener));
    return watching;
}

bool w_sound_inventory_changed(void) { return InterlockedExchange(&changed,0) != 0; }

static void w_sound_release(w_sound_row row) {
    CoTaskMemFree(row.id);
    if (row.endpoint) IAudioEndpointVolume_Release(row.endpoint);
    if (row.session) ISimpleAudioVolume_Release(row.session);
}

static void w_sound_clear(void) {
    for (int i = 0; i < used; ++i)
        w_sound_release(rows[i]);
    free(rows);
    rows = NULL;
    used = 0;
}

void w_sound_stop(void) {
    w_sound_clear();
    for (int i=0; i<sound_view_count; ++i) { free(selection[i]); selection[i]=NULL; }
    if (watching) IMMDeviceEnumerator_UnregisterEndpointNotificationCallback(devices,&listener);
    if (devices) IMMDeviceEnumerator_Release(devices);
    devices = NULL;
    watching = false;
    last_view = -1;
}

static void w_add_sound(w_sound_inventory *inventory, const wchar_t *label, w_sound_row row) {
    if (inventory->used == inventory->capacity) {
        int size = inventory->capacity ? inventory->capacity * 2 : 16;
        w_sound_row *next = realloc(inventory->rows, (size_t)size * sizeof(*next));
        if (!next) { w_sound_release(row); return; }
        inventory->rows = next;
        inventory->capacity = size;
    }
    lstrcpynW(row.label,label,count_of(row.label));
    inventory->rows[inventory->used++] = row;
}

static void w_sessions(w_sound_inventory *inventory, IMMDevice *device, const wchar_t *endpoint) {
    IAudioSessionManager2 *manager = NULL;
    if (FAILED(IMMDevice_Activate(device, &IID_IAudioSessionManager2, CLSCTX_ALL, NULL, (void **)&manager)))
        return;
    IAudioSessionEnumerator *sessions = NULL;
    if (SUCCEEDED(IAudioSessionManager2_GetSessionEnumerator(manager, &sessions))) {
        int count = 0;
        IAudioSessionEnumerator_GetCount(sessions, &count);
        for (int i = 0; i < count; ++i) {
            IAudioSessionControl *control = NULL;
            if (FAILED(IAudioSessionEnumerator_GetSession(sessions, i, &control))) continue;
            IAudioSessionControl2 *info = NULL;
            AudioSessionState state = AudioSessionStateExpired;
            w_sound_row row = {0};
            if (SUCCEEDED(IAudioSessionControl_GetState(control, &state)) && state != AudioSessionStateExpired &&
                SUCCEEDED(IAudioSessionControl_QueryInterface(control, &IID_IAudioSessionControl2, (void **)&info)) &&
                SUCCEEDED(IAudioSessionControl2_GetSessionInstanceIdentifier(info, &row.id)) &&
                SUCCEEDED(IAudioSessionControl_QueryInterface(control, &IID_ISimpleAudioVolume, (void **)&row.session))) {
                DWORD pid = 0, length = MAX_PATH;
                wchar_t path[MAX_PATH] = {0}, label[512];
                LPWSTR display = NULL;
                IAudioSessionControl_GetDisplayName(control, &display);
                IAudioSessionControl2_GetProcessId(info, &pid);
                HANDLE process = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : NULL;
                if (process) {
                    if (!QueryFullProcessImageNameW(process, 0, path, &length)) path[0] = 0;
                    CloseHandle(process);
                }
                const wchar_t *base = wcsrchr(path, L'\\');
                const wchar_t *name = display && *display && *display != L'@' ? display :
                    *path ? (base ? base + 1 : path) :
                    IAudioSessionControl2_IsSystemSoundsSession(info) == S_OK ? L"System sounds" : L"Audio session";
                swprintf(label, count_of(label), L"%.225ls   ·   %.225ls", name, endpoint);
                w_add_sound(inventory, label, row);
                CoTaskMemFree(display);
            } else w_sound_release(row);
            if (info) IAudioSessionControl2_Release(info);
            IAudioSessionControl_Release(control);
        }
        IAudioSessionEnumerator_Release(sessions);
    }
    IAudioSessionManager2_Release(manager);
}

static LPWSTR w_default_id(EDataFlow flow, ERole role) {
    IMMDevice *active = NULL;
    LPWSTR id = NULL;
    if (SUCCEEDED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(devices,flow,role,&active))) {
        IMMDevice_GetId(active,&id);
        IMMDevice_Release(active);
    }
    return id;
}

void w_sound_refresh(HWND list, enum w_sound_view view) {
    LRESULT selected = SendMessageW(list, LB_GETCURSEL, 0, 0);
    LRESULT top = SendMessageW(list, LB_GETTOPINDEX, 0, 0);
    int previous = selected < 0 ? -1 : (int)SendMessageW(list, LB_GETITEMDATA, selected, 0);
    if (last_view >= 0) {
        scroll[last_view] = (int)top;
        if (previous >= 0 && previous < used) {
            wchar_t *copy = _wcsdup(rows[previous].id);
            if (copy) { free(selection[last_view]); selection[last_view] = copy; }
        }
    }
    last_view = view;
    const wchar_t *id = selection[view];
    w_sound_inventory next = {0};
    EDataFlow flow = view == sound_inputs ? eCapture : eRender;
    IMMDeviceCollection *collection = NULL;
    if (devices && SUCCEEDED(IMMDeviceEnumerator_EnumAudioEndpoints(devices,flow,DEVICE_STATE_ACTIVE,&collection))) {
        LPWSTR default_id = view == sound_apps ? NULL : w_default_id(flow,eConsole);
        LPWSTR media_id = view == sound_apps ? NULL : w_default_id(flow,eMultimedia);
        LPWSTR calls_id = view == sound_apps ? NULL : w_default_id(flow,eCommunications);
        UINT count = 0;
        IMMDeviceCollection_GetCount(collection, &count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice *device = NULL;
            if (FAILED(IMMDeviceCollection_Item(collection, i, &device))) continue;
            PROPVARIANT value;
            PropVariantInit(&value);
            IPropertyStore *properties = NULL;
            if (SUCCEEDED(IMMDevice_OpenPropertyStore(device, STGM_READ, &properties))) {
                IPropertyStore_GetValue(properties, &PKEY_Device_FriendlyName, &value);
                IPropertyStore_Release(properties);
            }
            const wchar_t *name = value.vt == VT_LPWSTR && value.pwszVal ? value.pwszVal : L"Audio device";
            if (view == sound_apps) w_sessions(&next, device, name);
            else {
                w_sound_row row = {0};
                if (SUCCEEDED(IMMDevice_GetId(device, &row.id)) &&
                    SUCCEEDED(IMMDevice_Activate(device, &IID_IAudioEndpointVolume, CLSCTX_ALL, NULL, (void **)&row.endpoint))) {
                    row.is_default = default_id && media_id && !wcscmp(default_id,row.id) && !wcscmp(media_id,row.id);
                    row.calls_default = calls_id && !wcscmp(calls_id,row.id);
                    wchar_t label[512];
                    swprintf(label, count_of(label), L"%.410ls%ls%ls", name,
                             row.is_default ? L"   ·   Default" : L"", row.calls_default ? L"   ·   Calls" : L"");
                    w_add_sound(&next, label, row);
                } else w_sound_release(row);
            }
            PropVariantClear(&value);
            IMMDevice_Release(device);
        }
        CoTaskMemFree(default_id);
        CoTaskMemFree(media_id);
        CoTaskMemFree(calls_id);
        IMMDeviceCollection_Release(collection);
    }
    w_sound_clear();
    rows = next.rows;
    used = next.used;
    w_tool_rows items=w_rows_begin(list);
    selected = id ? -1 : 0;
    for (int i=0; i<used; ++i) {
        int index=w_rows_put(&items,rows[i].label,i);
        if ((id && !wcscmp(id,rows[i].id)) || (!id && rows[i].is_default)) selected=index;
    }
    w_rows_end(&items,selected,id ? scroll[view] : w_rows_show_selection);
}

bool w_sound_state(int index, int *percent, BOOL *muted) {
    if (index < 0 || index >= used) return false;
    w_sound_row *r = &rows[index];
    float level = 0;
    HRESULT hr = r->session ? ISimpleAudioVolume_GetMasterVolume(r->session, &level)
                            : IAudioEndpointVolume_GetMasterVolumeLevelScalar(r->endpoint, &level);
    if (FAILED(hr)) return false;
    *percent = w_clamp_int((int)(level * 100 + 0.5f), 0, 100);
    return SUCCEEDED(r->session ? ISimpleAudioVolume_GetMute(r->session, muted) :
                                  IAudioEndpointVolume_GetMute(r->endpoint, muted));
}

bool w_sound_change(int index, int percent) {
    if (index < 0 || index >= used) return false;
    w_sound_row *r = &rows[index];
    if (percent < 0) {
        BOOL muted;
        HRESULT hr = r->session ? ISimpleAudioVolume_GetMute(r->session,&muted) :
                                  IAudioEndpointVolume_GetMute(r->endpoint,&muted);
        if (FAILED(hr)) return false;
        return SUCCEEDED(r->session ? ISimpleAudioVolume_SetMute(r->session, !muted, NULL) :
                                      IAudioEndpointVolume_SetMute(r->endpoint, !muted, NULL));
    }
    float volume = w_clamp_int(percent, 0, 100) / 100.0f;
    return SUCCEEDED(r->session ? ISimpleAudioVolume_SetMasterVolume(r->session, volume, NULL) :
                                  IAudioEndpointVolume_SetMasterVolumeLevelScalar(r->endpoint, volume, NULL));
}

bool w_sound_can_default(int index, bool calls) {
    return index >= 0 && index < used && rows[index].endpoint &&
           !(calls ? rows[index].calls_default : rows[index].is_default);
}

// Windows exposes default endpoint selection through the PolicyConfig COM service.
// The Win7 interface IID identifies this specific vtable on supported versions.
typedef struct w_policy w_policy;
struct w_policy {
    struct {
        IUnknownVtbl unknown;
        void (STDMETHODCALLTYPE *format_and_property_methods[10])(void);
        HRESULT (STDMETHODCALLTYPE *set_default)(w_policy *, LPCWSTR, ERole);
    } *vt;
};

bool w_sound_make_default(int index, bool calls) {
    static const GUID clsid = {0x870af99c,0x171d,0x4f9e,{0xaf,0x0d,0xe6,0x3d,0xf4,0x0c,0x2b,0xc9}};
    static const GUID iid = {0xf8679f50,0x850a,0x41cf,{0x9c,0x72,0x43,0x0f,0x29,0x02,0x90,0xc8}};
    w_policy *policy = NULL;
    if (!w_sound_can_default(index,calls) || FAILED(CoCreateInstance(&clsid,NULL,CLSCTX_ALL,&iid,(void **)&policy))) return false;
    HRESULT hr;
    if (calls) hr = policy->vt->set_default(policy,rows[index].id,eCommunications);
    else {
        EDataFlow flow = last_view == sound_inputs ? eCapture : eRender;
        LPWSTR prior = w_default_id(flow,eMultimedia);
        hr = policy->vt->set_default(policy,rows[index].id,eMultimedia);
        if (SUCCEEDED(hr)) {
            hr = policy->vt->set_default(policy,rows[index].id,eConsole);
            if (FAILED(hr) && prior) {
                LPWSTR current = w_default_id(flow,eMultimedia);
                if (current && !wcscmp(current,rows[index].id)) policy->vt->set_default(policy,prior,eMultimedia);
                CoTaskMemFree(current);
            }
        }
        CoTaskMemFree(prior);
    }
    policy->vt->unknown.Release((IUnknown *)policy);
    return SUCCEEDED(hr);
}
