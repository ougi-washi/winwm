#define COBJMACROS
#include "desktop_tools/sound.h"
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
    bool is_default;
} w_sound_row;

static w_sound_row *rows;
static int used, capacity;

static void w_sound_release(w_sound_row row) {
    CoTaskMemFree(row.id);
    if (row.endpoint) IAudioEndpointVolume_Release(row.endpoint);
    if (row.session) ISimpleAudioVolume_Release(row.session);
}

void w_sound_clear(void) {
    for (int i = 0; i < used; ++i)
        w_sound_release(rows[i]);
    free(rows);
    rows = NULL;
    used = capacity = 0;
}

static void w_add_sound(HWND list, const wchar_t *label, w_sound_row row) {
    if (used == capacity) {
        int size = capacity ? capacity * 2 : 16;
        w_sound_row *next = realloc(rows, (size_t)size * sizeof(*rows));
        if (!next) {
            w_sound_release(row);
            return;
        }
        rows = next;
        capacity = size;
    }
    LRESULT index = SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)label);
    if (index < 0) {
        w_sound_release(row);
        return;
    }
    SendMessageW(list, LB_SETITEMDATA, index, used);
    rows[used++] = row;
}

static void w_sessions(HWND list, IMMDevice *device, const wchar_t *endpoint) {
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
                w_add_sound(list, label, row);
                CoTaskMemFree(display);
            } else w_sound_release(row);
            if (info) IAudioSessionControl2_Release(info);
            IAudioSessionControl_Release(control);
        }
        IAudioSessionEnumerator_Release(sessions);
    }
    IAudioSessionManager2_Release(manager);
}

void w_sound_refresh(HWND list, bool sessions) {
    LRESULT selected = SendMessageW(list, LB_GETCURSEL, 0, 0);
    LRESULT top = SendMessageW(list, LB_GETTOPINDEX, 0, 0);
    int previous = selected < 0 ? -1 : (int)SendMessageW(list, LB_GETITEMDATA, selected, 0);
    wchar_t *id = previous >= 0 && previous < used ? _wcsdup(rows[previous].id) : NULL;
    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    w_sound_clear();
    IMMDeviceEnumerator *devices = NULL;
    if (SUCCEEDED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_INPROC_SERVER,
                                   &IID_IMMDeviceEnumerator, (void **)&devices))) {
        for (EDataFlow flow = eRender; flow <= (sessions ? eRender : eCapture); ++flow) {
            IMMDeviceCollection *collection = NULL;
            if (FAILED(IMMDeviceEnumerator_EnumAudioEndpoints(devices, flow, DEVICE_STATE_ACTIVE, &collection))) continue;
            IMMDevice *active = NULL;
            LPWSTR default_id = NULL;
            if (SUCCEEDED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(devices, flow, eConsole, &active))) {
                IMMDevice_GetId(active, &default_id);
                IMMDevice_Release(active);
            }
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
                if (sessions) w_sessions(list, device, name);
                else {
                    w_sound_row row = {0};
                    if (SUCCEEDED(IMMDevice_GetId(device, &row.id)) &&
                        SUCCEEDED(IMMDevice_Activate(device, &IID_IAudioEndpointVolume, CLSCTX_ALL, NULL, (void **)&row.endpoint))) {
                        row.is_default = default_id && !wcscmp(default_id, row.id);
                        wchar_t label[512];
                        swprintf(label, count_of(label), L"%ls   ·   %.400ls%ls", flow == eRender ? L"Output" : L"Input",
                                 name, row.is_default ? L"   ·   Default" : L"");
                        w_add_sound(list, label, row);
                    } else w_sound_release(row);
                }
                PropVariantClear(&value);
                IMMDevice_Release(device);
            }
            CoTaskMemFree(default_id);
            IMMDeviceCollection_Release(collection);
        }
        IMMDeviceEnumerator_Release(devices);
    }
    selected = id ? -1 : 0;
    for (int i = 0; i < used; ++i) {
        if (id && !wcscmp(id, rows[i].id)) { selected = i; break; }
        if (!id && rows[i].is_default) { selected = i; break; }
    }
    free(id);
    SendMessageW(list, LB_SETCURSEL, selected, 0);
    if (top > 0 && top < used) SendMessageW(list, LB_SETTOPINDEX, top, 0);
    SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list, NULL, TRUE);
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
    int level;
    BOOL muted;
    if (!w_sound_state(index, &level, &muted)) return false;
    w_sound_row *r = &rows[index];
    if (percent < 0)
        return SUCCEEDED(r->session ? ISimpleAudioVolume_SetMute(r->session, !muted, NULL) :
                                      IAudioEndpointVolume_SetMute(r->endpoint, !muted, NULL));
    float volume = w_clamp_int(percent, 0, 100) / 100.0f;
    return SUCCEEDED(r->session ? ISimpleAudioVolume_SetMasterVolume(r->session, volume, NULL) :
                                  IAudioEndpointVolume_SetMasterVolumeLevelScalar(r->endpoint, volume, NULL));
}

bool w_sound_can_default(int index) {
    return index >= 0 && index < used && rows[index].endpoint && !rows[index].is_default;
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

bool w_sound_make_default(int index) {
    static const GUID clsid = {0x870af99c,0x171d,0x4f9e,{0xaf,0x0d,0xe6,0x3d,0xf4,0x0c,0x2b,0xc9}};
    static const GUID iid = {0xf8679f50,0x850a,0x41cf,{0x9c,0x72,0x43,0x0f,0x29,0x02,0x90,0xc8}};
    w_policy *policy = NULL;
    if (!w_sound_can_default(index) || FAILED(CoCreateInstance(&clsid, NULL, CLSCTX_ALL, &iid, (void **)&policy))) return false;
    // Keep the dedicated communications choice intact.
    HRESULT hr = policy->vt->set_default(policy, rows[index].id, eMultimedia);
    if (SUCCEEDED(hr)) hr = policy->vt->set_default(policy, rows[index].id, eConsole);
    policy->vt->unknown.Release((IUnknown *)policy);
    return SUCCEEDED(hr);
}
