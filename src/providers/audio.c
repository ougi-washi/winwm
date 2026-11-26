#define COBJMACROS
#include "providers/audio.h"
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>

/* All endpoint interfaces belong to the worker's COM apartment. RPC callbacks
   only enqueue work, so a device change cannot race a volume adjustment. */
enum audio_event { endpoint_changed = 1, volume_changed, adjust_level, set_level, stop_worker };

////////////////
//~ Audio

static SRWLOCK lock = SRWLOCK_INIT;
static HANDLE queue, worker;
static HWND owner;
static UINT changed;
static w_audio_state current;
static LONG refresh_pending;

////////////////
//~ Audio Functions

static bool w_enqueue(enum audio_event event, int delta) {
    AcquireSRWLockShared(&lock);
    bool ok = queue && PostQueuedCompletionStatus(queue, (DWORD)delta, event, NULL);
    ReleaseSRWLockShared(&lock);
    return ok;
}

/* Both listeners have process lifetime. The audio service may retain a reference
   during endpoint unregistration. A late callback sees an empty queue. */
#define listener_unknown(kind, name)                                                                         \
    static ULONG STDMETHODCALLTYPE name##_ref(kind *self) {                                                  \
        (void)self;                                                                                          \
        return 2;                                                                                            \
    }                                                                                                        \
    static ULONG STDMETHODCALLTYPE name##_release(kind *self) {                                              \
        (void)self;                                                                                          \
        return 1;                                                                                            \
    }                                                                                                        \
    static HRESULT STDMETHODCALLTYPE name##_query(kind *self, REFIID id, void **result) {                    \
        if (!result)                                                                                         \
            return E_POINTER;                                                                                \
        *result = NULL;                                                                                      \
        if (!IsEqualIID(id, &IID_IUnknown) && !IsEqualIID(id, &IID_##kind))                                  \
            return E_NOINTERFACE;                                                                            \
        *result = self;                                                                                      \
        name##_ref(self);                                                                                    \
        return S_OK;                                                                                         \
    }
listener_unknown(IMMNotificationClient, w_device) listener_unknown(IAudioEndpointVolumeCallback, w_volume)

    static HRESULT STDMETHODCALLTYPE w_device_state(IMMNotificationClient *self, LPCWSTR id, DWORD state) {
    (void)self;
    (void)id;
    (void)state;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE w_device_inventory(IMMNotificationClient *self, LPCWSTR id) {
    (void)self;
    (void)id;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE w_device_default(IMMNotificationClient *self, EDataFlow flow, ERole role,
                                                LPCWSTR id) {
    (void)self;
    (void)id;
    if (flow == eRender && role == eMultimedia)
        w_enqueue(endpoint_changed, 0);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE w_device_property(IMMNotificationClient *self, LPCWSTR id,
                                                 const PROPERTYKEY key) {
    (void)self;
    (void)id;
    (void)key;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE w_volume_notify(IAudioEndpointVolumeCallback *self,
                                               AUDIO_VOLUME_NOTIFICATION_DATA *data) {
    (void)self;
    (void)data;
    if (!InterlockedExchange(&refresh_pending, 1))
        w_enqueue(volume_changed, 0);
    return S_OK;
}

static IMMNotificationClientVtbl device_vtbl = {w_device_query,   w_device_ref,       w_device_release,
                                                w_device_state,   w_device_inventory, w_device_inventory,
                                                w_device_default, w_device_property};
static IMMNotificationClient devices = {&device_vtbl};
static IAudioEndpointVolumeCallbackVtbl volume_vtbl = {w_volume_query, w_volume_ref, w_volume_release,
                                                       w_volume_notify};
static IAudioEndpointVolumeCallback levels = {&volume_vtbl};

////////////////
//~ Audio endpoint

static IAudioEndpointVolume *w_default_output(IMMDeviceEnumerator *enumerator, wchar_t *name, int cap) {
    IAudioEndpointVolume *volume = NULL;
    IMMDevice *endpoint = NULL;
    *name = 0;
    if (enumerator &&
        SUCCEEDED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, eRender, eMultimedia, &endpoint))) {
        IMMDevice_Activate(endpoint, &IID_IAudioEndpointVolume, CLSCTX_INPROC_SERVER, NULL, (void **)&volume);
        IPropertyStore *props = NULL;
        if (SUCCEEDED(IMMDevice_OpenPropertyStore(endpoint, STGM_READ, &props))) {
            PROPVARIANT value = {0};
            if (SUCCEEDED(IPropertyStore_GetValue(props, &PKEY_Device_FriendlyName, &value)) &&
                value.vt == VT_LPWSTR && value.pwszVal)
                lstrcpynW(name, value.pwszVal, cap);
            PropVariantClear(&value);
            IPropertyStore_Release(props);
        }
        IMMDevice_Release(endpoint);
    }
    if (volume)
        IAudioEndpointVolume_RegisterControlChangeNotify(volume, &levels);
    return volume;
}

static void w_disconnect(IAudioEndpointVolume *volume) {
    if (volume) {
        IAudioEndpointVolume_UnregisterControlChangeNotify(volume, &levels);
        IAudioEndpointVolume_Release(volume);
    }
}

static void w_publish(IAudioEndpointVolume *volume, const wchar_t *name) {
    float level = 0;
    BOOL muted = FALSE;
    w_audio_state next = {0};
    if (volume && SUCCEEDED(IAudioEndpointVolume_GetMasterVolumeLevelScalar(volume, &level)) &&
        SUCCEEDED(IAudioEndpointVolume_GetMute(volume, &muted)))
        next = (w_audio_state){.available=true, .muted=muted != FALSE, .percent=(int)(level * 100 + 0.5f)};
    lstrcpynW(next.device,name,count_of(next.device));
    AcquireSRWLockExclusive(&lock);
    bool dirty =
        next.available != current.available || next.percent != current.percent || next.muted != current.muted ||
        wcscmp(next.device,current.device);
    current = next;
    ReleaseSRWLockExclusive(&lock);
    if (dirty)
        PostMessageW(owner, changed, 0, 0);
}

static HRESULT w_adjust(IAudioEndpointVolume *volume, int delta) {
    if (!volume)
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    if (!delta) {
        BOOL muted = FALSE;
        HRESULT hr = IAudioEndpointVolume_GetMute(volume, &muted);
        return FAILED(hr) ? hr : IAudioEndpointVolume_SetMute(volume, !muted, NULL);
    }
    float level = 0;
    HRESULT hr = IAudioEndpointVolume_GetMasterVolumeLevelScalar(volume, &level);
    if (FAILED(hr))
        return hr;
    int next = w_clamp_int((int)(level * 100 + .5f) + delta, 0, 100);
    return IAudioEndpointVolume_SetMasterVolumeLevelScalar(volume, next / 100.f, NULL);
}

////////////////
//~ Audio worker

static DWORD WINAPI w_audio_worker(void *arg) {
    HANDLE port = arg;
    IMMDeviceEnumerator *enumerator = NULL;
    bool initialized = SUCCEEDED(CoInitializeEx(NULL, COINIT_MULTITHREADED));
    if (initialized && SUCCEEDED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_INPROC_SERVER,
                                                  &IID_IMMDeviceEnumerator, (void **)&enumerator)))
        IMMDeviceEnumerator_RegisterEndpointNotificationCallback(enumerator, &devices);
    wchar_t name[count_of(current.device)] = {0};
    IAudioEndpointVolume *volume = w_default_output(enumerator,name,count_of(name));
    w_publish(volume,name);
    DWORD delta;
    ULONG_PTR event;
    OVERLAPPED *unused;
    while (GetQueuedCompletionStatus(port, &delta, &event, &unused, INFINITE) && event != stop_worker) {
        if (event == endpoint_changed) {
            w_disconnect(volume);
            volume = w_default_output(enumerator,name,count_of(name));
        } else if (event == adjust_level || event == set_level) {
            if (!volume)
                volume = w_default_output(enumerator,name,count_of(name));
            HRESULT hr = event == adjust_level ? w_adjust(volume, (int)(LONG)delta)
                         : volume ? IAudioEndpointVolume_SetMasterVolumeLevelScalar(volume, delta / 100.f, NULL)
                                  : HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
            if (FAILED(hr))
                PostMessageW(owner, changed, (WPARAM)(DWORD)hr, 0);
        } else
            InterlockedExchange(&refresh_pending, 0);
        w_publish(volume,name);
    }
    w_disconnect(volume);
    if (enumerator) {
        IMMDeviceEnumerator_UnregisterEndpointNotificationCallback(enumerator, &devices);
        IMMDeviceEnumerator_Release(enumerator);
    }
    if (initialized)
        CoUninitialize();
    return 0;
}

////////////////
//~ Audio controls

bool w_audio_start(HWND w, UINT message) {
    if (worker)
        return true;
    owner = w;
    changed = message;
    HANDLE port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    if (!port)
        return false;
    AcquireSRWLockExclusive(&lock);
    queue = port;
    worker = CreateThread(NULL, 0, w_audio_worker, port, 0, NULL);
    if (!worker) {
        queue = NULL;
        CloseHandle(port);
    }
    ReleaseSRWLockExclusive(&lock);
    return worker != NULL;
}

bool w_audio_adjust(int delta) {
    return w_enqueue(adjust_level, delta);
}

bool w_audio_set_percent(int percent) {
    return (unsigned)percent <= 100 && w_enqueue(set_level, percent);
}

w_audio_state w_audio_read(void) {
    AcquireSRWLockShared(&lock);
    w_audio_state state = current;
    ReleaseSRWLockShared(&lock);
    return state;
}

void w_audio_stop(void) {
    if (!worker)
        return;
    AcquireSRWLockExclusive(&lock);
    HANDLE port = queue;
    queue = NULL;
    PostQueuedCompletionStatus(port, 0, stop_worker, NULL);
    ReleaseSRWLockExclusive(&lock);
    WaitForSingleObject(worker, INFINITE);
    CloseHandle(worker);
    CloseHandle(port);
    worker = NULL;
    current = (w_audio_state){0};
    InterlockedExchange(&refresh_pending, 0);
}
