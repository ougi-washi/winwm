#define COBJMACROS
#include "desktop_tools/tools.h"
#include <dwmapi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <shlobj.h>
#include <stdio.h>

////////////////
//~ Capture

static POINT anchor, cursor;
static bool dragging, selected;
static RECT selection;

////////////////
//~ Capture Functions

static RECT w_region(void) {
    RECT r = {min(anchor.x, cursor.x), min(anchor.y, cursor.y), max(anchor.x, cursor.x),
              max(anchor.y, cursor.y)};
    return r;
}

static LRESULT CALLBACK w_region_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_LBUTTONDOWN) {
        GetCursorPos(&anchor);
        cursor = anchor;
        dragging = true;
        SetCapture(w);
        return 0;
    }
    if (msg == WM_MOUSEMOVE && dragging) {
        GetCursorPos(&cursor);
        InvalidateRect(w, NULL, TRUE);
        return 0;
    }
    if (msg == WM_LBUTTONUP && dragging) {
        GetCursorPos(&cursor);
        selection = w_region();
        selected = !IsRectEmpty(&selection);
        ReleaseCapture();
        DestroyWindow(w);
        return 0;
    }
    if (msg == WM_RBUTTONUP || (msg == WM_KEYDOWN && wp == VK_ESCAPE)) {
        DestroyWindow(w);
        return 0;
    }
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        if (dragging) {
            RECT r = w_region();
            MapWindowPoints(NULL, w, (POINT *)&r, 2);
            FrameRect(dc, &r, (HBRUSH)GetStockObject(WHITE_BRUSH));
        }
        EndPaint(w, &ps);
        return 0;
    }
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

static bool w_choose_region(void) {
    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSW wc = {.hInstance = inst,
                    .lpfnWndProc = w_region_proc,
                    .lpszClassName = L"NativeCapture",
                    .hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH),
                    .hCursor = LoadCursorW(NULL, IDC_CROSS)};
    RegisterClassW(&wc);
    HWND w = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED, wc.lpszClassName,
                             L"Select capture area", WS_POPUP, GetSystemMetrics(SM_XVIRTUALSCREEN),
                             GetSystemMetrics(SM_YVIRTUALSCREEN), GetSystemMetrics(SM_CXVIRTUALSCREEN),
                             GetSystemMetrics(SM_CYVIRTUALSCREEN), NULL, NULL, inst, NULL);
    if (!w)
        return false;
    SetLayeredWindowAttributes(w, 0, 90, LWA_ALPHA);
    ShowWindow(w, SW_SHOW);
    SetForegroundWindow(w);
    SetFocus(w);
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    DwmFlush();
    return selected;
}

////////////////
//~ Frame capture

static bool w_window_bounds(HWND window, DWORD pid, RECT *frame, RECT *area) {
    DWORD current = 0, cloaked = 0, affinity = 0;
    GetWindowThreadProcessId(window,&current);
    if (!pid || current != pid || !IsWindowVisible(window) || IsIconic(window) ||
        window == GetDesktopWindow() || window == GetShellWindow() ||
        (GetWindowDisplayAffinity(window,&affinity) && affinity != WDA_NONE) ||
        (SUCCEEDED(DwmGetWindowAttribute(window,DWMWA_CLOAKED,&cloaked,sizeof(cloaked))) && cloaked) ||
        !GetWindowRect(window,frame) || IsRectEmpty(frame)) return false;
    RECT visible;
    if (FAILED(DwmGetWindowAttribute(window,DWMWA_EXTENDED_FRAME_BOUNDS,&visible,sizeof(visible))) ||
        !IntersectRect(area,frame,&visible)) *area = *frame;
    return true;
}

static bool w_capture_rect(RECT r, HWND window, DWORD pid) {
    RECT frame;
    if (window && !w_window_bounds(window,pid,&frame,&r)) return false;
    HWND owner =
        CreateWindowW(L"STATIC", L"", WS_POPUP, 0, 0, 0, 0, NULL, NULL, GetModuleHandleW(NULL), NULL);
    HDC src = GetDC(NULL), dst = CreateCompatibleDC(src);
    HBITMAP bmp = CreateCompatibleBitmap(src, r.right - r.left, r.bottom - r.top);
    bool ok = false;
    if (bmp && dst) {
        HGDIOBJ old = SelectObject(dst, bmp);
        if (window) {
            SetViewportOrgEx(dst,frame.left-r.left,frame.top-r.top,NULL);
            ok = PrintWindow(window,dst,PW_RENDERFULLCONTENT) != 0;
            RECT after, area;
            if (ok && w_window_bounds(window,pid,&after,&area)) {
                OffsetRect(&area,frame.left-after.left,frame.top-after.top);
                ok = EqualRect(&r,&area);
            } else ok = false;
        } else {
            ok = BitBlt(dst, 0, 0, r.right - r.left, r.bottom - r.top, src, r.left, r.top,
                        SRCCOPY | CAPTUREBLT) != 0;
        }
        SelectObject(dst, old);
        if (ok && owner && OpenClipboard(owner)) {
            if (EmptyClipboard() && SetClipboardData(CF_BITMAP, bmp))
                bmp = NULL;
            CloseClipboard();
        }
    }
    if (bmp) {
        DeleteObject(bmp);
        ok = false;
    }
    if (dst)
        DeleteDC(dst);
    ReleaseDC(NULL, src);
    DestroyWindow(owner);
    return ok;
}

static struct w_media_api {
    HRESULT(WINAPI *startup)(ULONG, DWORD);
    HRESULT(WINAPI *shutdown)(void);
    HRESULT(WINAPI *create_type)(IMFMediaType **);
    HRESULT(WINAPI *create_buffer)(DWORD, IMFMediaBuffer **);
    HRESULT(WINAPI *create_sample)(IMFSample **);
    HRESULT(WINAPI *create_writer)(LPCWSTR, IMFByteStream *, IMFAttributes *, IMFSinkWriter **);
} media;

static bool w_load_encoder(void) {
    HMODULE platform = LoadLibraryExW(L"mfplat.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE writer = LoadLibraryExW(L"mfreadwrite.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!platform || !writer)
        return false;
    media.startup = (HRESULT(WINAPI *)(ULONG, DWORD))GetProcAddress(platform, "MFStartup");
    media.shutdown = (HRESULT(WINAPI *)(void))GetProcAddress(platform, "MFShutdown");
    media.create_type = (HRESULT(WINAPI *)(IMFMediaType **))GetProcAddress(platform, "MFCreateMediaType");
    media.create_buffer =
        (HRESULT(WINAPI *)(DWORD, IMFMediaBuffer **))GetProcAddress(platform, "MFCreateMemoryBuffer");
    media.create_sample = (HRESULT(WINAPI *)(IMFSample **))GetProcAddress(platform, "MFCreateSample");
    media.create_writer =
        (HRESULT(WINAPI *)(LPCWSTR, IMFByteStream *, IMFAttributes *, IMFSinkWriter **))GetProcAddress(
            writer, "MFCreateSinkWriterFromURL");
    return media.startup && media.shutdown && media.create_type && media.create_buffer &&
           media.create_sample && media.create_writer;
}

////////////////
//~ Video recording

static HRESULT w_media_type(IMFMediaType **type, const GUID *sub, UINT w, UINT h) {
    HRESULT hr = media.create_type(type);
    if (SUCCEEDED(hr))
        hr = IMFMediaType_SetGUID(*type, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    if (SUCCEEDED(hr))
        hr = IMFMediaType_SetGUID(*type, &MF_MT_SUBTYPE, sub);
    if (SUCCEEDED(hr))
        hr = IMFMediaType_SetUINT64(*type, &MF_MT_FRAME_SIZE, ((UINT64)w << 32) | h);
    if (SUCCEEDED(hr))
        hr = IMFMediaType_SetUINT64(*type, &MF_MT_FRAME_RATE, ((UINT64)15 << 32) | 1);
    if (SUCCEEDED(hr))
        hr = IMFMediaType_SetUINT64(*type, &MF_MT_PIXEL_ASPECT_RATIO, ((UINT64)1 << 32) | 1);
    if (SUCCEEDED(hr))
        hr = IMFMediaType_SetUINT32(*type, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    return hr;
}

static int w_record(void) {
    HANDLE stop = CreateEventW(NULL, TRUE, FALSE, L"Local\\winwm.record.stop");
    if (!stop)
        return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        SetEvent(stop);
        CloseHandle(stop);
        return 0;
    }
    if (!w_load_encoder()) {
        CloseHandle(stop);
        MessageBoxW(NULL, L"Install the Windows Media Feature Pack to record video.", L"Recording", MB_OK);
        return 1;
    }
    POINT p;
    GetCursorPos(&p);
    MONITORINFO mi = {.cbSize = sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(p, MONITOR_DEFAULTTOPRIMARY), &mi);
    RECT r = mi.rcMonitor;
    int w = (r.right - r.left) & ~1, h = (r.bottom - r.top) & ~1;
    HDC src = GetDC(NULL), dc = CreateCompatibleDC(src);
    BITMAPINFO bi = {0};
    bi.bmiHeader = (BITMAPINFOHEADER){.biSize = sizeof(BITMAPINFOHEADER),
                                      .biWidth = w,
                                      .biHeight = h,
                                      .biPlanes = 1,
                                      .biBitCount = 32,
                                      .biCompression = BI_RGB};
    void *pixels = NULL;
    HBITMAP bitmap = CreateDIBSection(src, &bi, DIB_RGB_COLORS, &pixels, NULL, 0);
    HGDIOBJ old = NULL;
    if (bitmap && dc)
        old = SelectObject(dc, bitmap);
    wchar_t file[os_path_cap] = {0};
    PWSTR folder = NULL;
    SYSTEMTIME now;
    GetLocalTime(&now);
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    bool com = SUCCEEDED(hr), mf = false;
    if (SUCCEEDED(hr)) {
        hr = media.startup(MF_VERSION, MFSTARTUP_FULL);
        mf = SUCCEEDED(hr);
    }
    if (SUCCEEDED(hr))
        hr = SHGetKnownFolderPath(&FOLDERID_Videos, KF_FLAG_CREATE, NULL, &folder);
    if (SUCCEEDED(hr) &&
        swprintf(file, count_of(file), L"%ls\\winwm-%04u%02u%02u-%02u%02u%02u-%03u.mp4", folder, now.wYear,
                 now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds) < 0)
        hr = E_FAIL;
    if (!bitmap || !dc || !pixels || w <= 0 || h <= 0 || (uint64_t)w * h > UINT32_MAX / 4)
        hr = E_FAIL;
    IMFSinkWriter *writer = NULL;
    IMFMediaType *output = NULL, *input = NULL;
    DWORD stream = 0;
    if (SUCCEEDED(hr))
        hr = media.create_writer(file, NULL, NULL, &writer);
    if (SUCCEEDED(hr))
        hr = w_media_type(&output, &MFVideoFormat_H264, w, h);
    if (SUCCEEDED(hr))
        hr = IMFMediaType_SetUINT32(output, &MF_MT_AVG_BITRATE, 8000000);
    if (SUCCEEDED(hr))
        hr = IMFSinkWriter_AddStream(writer, output, &stream);
    if (SUCCEEDED(hr))
        hr = w_media_type(&input, &MFVideoFormat_RGB32, w, h);
    if (SUCCEEDED(hr))
        hr = IMFSinkWriter_SetInputMediaType(writer, stream, input, NULL);
    if (SUCCEEDED(hr))
        hr = IMFSinkWriter_BeginWriting(writer);
    DWORD bytes = (DWORD)((size_t)w * h * 4);
    ULONGLONG start = GetTickCount64(), next = start;
    bool began = SUCCEEDED(hr);
    while (SUCCEEDED(hr)) {
        ULONGLONG tick = GetTickCount64();
        if (WaitForSingleObject(stop, next > tick ? (DWORD)(next - tick) : 0) == WAIT_OBJECT_0)
            break;
        tick = GetTickCount64();
        if (!BitBlt(dc, 0, 0, w, h, src, r.left, r.top, SRCCOPY | CAPTUREBLT)) {
            hr = E_FAIL;
            break;
        }
        GdiFlush();
        IMFMediaBuffer *buffer = NULL;
        IMFSample *sample = NULL;
        BYTE *dst = NULL;
        hr = media.create_buffer(bytes, &buffer);
        if (SUCCEEDED(hr))
            hr = IMFMediaBuffer_Lock(buffer, &dst, NULL, NULL);
        if (SUCCEEDED(hr)) {
            memcpy(dst, pixels, bytes);
            hr = IMFMediaBuffer_Unlock(buffer);
        }
        if (SUCCEEDED(hr))
            hr = IMFMediaBuffer_SetCurrentLength(buffer, bytes);
        if (SUCCEEDED(hr))
            hr = media.create_sample(&sample);
        if (SUCCEEDED(hr))
            hr = IMFSample_AddBuffer(sample, buffer);
        if (SUCCEEDED(hr))
            hr = IMFSample_SetSampleTime(sample, (LONGLONG)(tick - start) * 10000);
        if (SUCCEEDED(hr))
            hr = IMFSample_SetSampleDuration(sample, 10000000 / 15);
        if (SUCCEEDED(hr))
            hr = IMFSinkWriter_WriteSample(writer, stream, sample);
        if (sample)
            IMFSample_Release(sample);
        if (buffer)
            IMFMediaBuffer_Release(buffer);
        next = tick + 67;
    }
    if (began) {
        HRESULT final = IMFSinkWriter_Finalize(writer);
        if (SUCCEEDED(hr))
            hr = final;
    }
    if (input)
        IMFMediaType_Release(input);
    if (output)
        IMFMediaType_Release(output);
    if (writer)
        IMFSinkWriter_Release(writer);
    if (folder)
        CoTaskMemFree(folder);
    if (mf)
        media.shutdown();
    if (com)
        CoUninitialize();
    if (old)
        SelectObject(dc, old);
    if (bitmap)
        DeleteObject(bitmap);
    if (dc)
        DeleteDC(dc);
    ReleaseDC(NULL, src);
    CloseHandle(stop);
    if (FAILED(hr)) {
        wchar_t why[160];
        swprintf(why, count_of(why), L"Screen recording failed (0x%08lx).", hr);
        MessageBoxW(NULL, why, L"Recording", MB_OK);
    }
    return FAILED(hr);
}

int w_capture_run(const wchar_t *mode) {
    if (!wcscmp(mode, L"record"))
        return w_record();
    if (!wcscmp(mode, L"capture_region")) {
        if (!w_choose_region())
            return 0;
        return !w_capture_rect(selection,NULL,0);
    }
    HWND target;
    DWORD pid = 0;
    if (!wcscmp(mode,L"capture_window")) {
        target = GetForegroundWindow();
        GetWindowThreadProcessId(target,&pid);
    } else {
        unsigned long long handle;
        wchar_t extra;
        if (swscanf(mode,L"capture_window-%llx-%lu%lc",&handle,&pid,&extra) != 2 ||
            !handle || handle > UINTPTR_MAX || !pid) return 1;
        target = (HWND)(uintptr_t)handle;
    }
    return !target || !w_capture_rect((RECT){0},target,pid);
}
