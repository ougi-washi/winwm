#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include "native_notifications/publish.h"

enum { shell_callback = WM_APP + 1, acknowledge_ms = 1000, max_display_ms = 10000 };
static const wchar_t window_class[] = L"winwm.ShellNotification";
static SRWLOCK publish_lock = SRWLOCK_INIT;
static HWND active_window;
static bool stopped;

typedef struct {
    bool visible, finished;
    ULONGLONG deadline;
    w_notification_shown shown;
    void *context;
} w_balloon;

static void w_acknowledge(w_balloon *state) {
    if (state->visible) return;
    state->visible=true;
    UINT seconds=5;
    SystemParametersInfoW(SPI_GETMESSAGEDURATION,0,&seconds,0);
    state->deadline=GetTickCount64() + (seconds<max_display_ms/1000 ? seconds*1000 : max_display_ms);
    if (state->shown) state->shown(state->context);
}

static LRESULT CALLBACK w_balloon_window(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_NCCREATE) {
        CREATESTRUCTW *create = (CREATESTRUCTW *)lp;
        SetWindowLongPtrW(window,GWLP_USERDATA,(LONG_PTR)create->lpCreateParams);
    } else if (message == WM_CANCELMODE) {
        w_balloon *state=(w_balloon *)GetWindowLongPtrW(window,GWLP_USERDATA);
        if (state) state->finished=true;
    } else if (message == shell_callback) {
        w_balloon *state = (w_balloon *)GetWindowLongPtrW(window,GWLP_USERDATA);
        if (!state || state->finished) return 0;
        switch (LOWORD(lp)) {
        case NIN_BALLOONSHOW:
            w_acknowledge(state);
            break;
        case NIN_BALLOONUSERCLICK:
            w_acknowledge(state);
            state->finished = true;
            break;
        case NIN_BALLOONHIDE:
        case NIN_BALLOONTIMEOUT:
            state->finished = true;
            break;
        }
        return 0;
    }
    return DefWindowProcW(window,message,wp,lp);
}

static void w_wait_for_balloon(HWND owner, w_balloon *state) {
    while (!state->finished) {
        MSG message;
        while (PeekMessageW(&message,owner,0,0,PM_REMOVE)) {
            if (message.message==WM_QUIT) {
                PostQuitMessage((int)message.wParam);
                return;
            }
            DispatchMessageW(&message);
        }
        ULONGLONG now=GetTickCount64();
        if (state->finished || now>=state->deadline) return;
        if (MsgWaitForMultipleObjects(0,NULL,FALSE,(DWORD)(state->deadline-now),
                                      QS_POSTMESSAGE|QS_SENDMESSAGE)==WAIT_FAILED) return;
    }
}

// A notification-area balloon requires Explorer. Confirm actual display: Windows may
// accept NIM_MODIFY and then suppress the balloon during quiet time or presentation.
bool w_notification_publish_observed(const wchar_t *title, const wchar_t *body,
                                     w_notification_shown shown, void *context) {
    HWND tray = FindWindowW(L"Shell_TrayWnd",NULL);
    NOTIFYICONDATAW icon = {.cbSize=sizeof(icon), .uID=1, .uCallbackMessage=shell_callback};
    if (!tray || IsHungAppWindow(tray) || !title || !body || !*body ||
        wcslen(title)>=ARRAYSIZE(icon.szInfoTitle) || wcslen(body)>=ARRAYSIZE(icon.szInfo)) return false;
    HINSTANCE instance = GetModuleHandleW(NULL);
    WNDCLASSW cls = {.lpfnWndProc=w_balloon_window, .hInstance=instance, .lpszClassName=window_class};
    if (!RegisterClassW(&cls) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) return false;
    w_balloon state = {.shown=shown,.context=context};
    icon.hWnd = CreateWindowExW(0,window_class,L"",0,0,0,0,0,HWND_MESSAGE,NULL,instance,&state);
    if (!icon.hWnd) return false;
    AcquireSRWLockExclusive(&publish_lock);
    bool available=!stopped && !active_window;
    if (available) active_window=icon.hWnd;
    ReleaseSRWLockExclusive(&publish_lock);
    if (!available) {
        DestroyWindow(icon.hWnd);
        return false;
    }
    icon.uFlags = NIF_ICON|NIF_MESSAGE|NIF_TIP;
    icon.hIcon = LoadIconW(NULL,IDI_APPLICATION);
    wcscpy(icon.szTip,L"winwm");
    if (Shell_NotifyIconW(NIM_ADD,&icon)) {
        icon.uVersion = NOTIFYICON_VERSION_4;
        if (Shell_NotifyIconW(NIM_SETVERSION,&icon)) {
            icon.uFlags = NIF_INFO|NIF_REALTIME;
            icon.dwInfoFlags = NIIF_WARNING|NIIF_RESPECT_QUIET_TIME;
            wcscpy(icon.szInfoTitle,title);
            wcscpy(icon.szInfo,body);
            state.deadline = GetTickCount64()+acknowledge_ms;
            if (Shell_NotifyIconW(NIM_MODIFY,&icon)) w_wait_for_balloon(icon.hWnd,&state);
        }
        Shell_NotifyIconW(NIM_DELETE,&icon);
    }
    AcquireSRWLockExclusive(&publish_lock);
    active_window=NULL;
    ReleaseSRWLockExclusive(&publish_lock);
    DestroyWindow(icon.hWnd);
    return state.visible;
}

bool w_notification_publish(const wchar_t *title, const wchar_t *body) {
    return w_notification_publish_observed(title,body,NULL,NULL);
}

void w_notification_publish_stop(void) {
    AcquireSRWLockExclusive(&publish_lock);
    stopped=true;
    if (active_window) PostMessageW(active_window,WM_CANCELMODE,0,0);
    ReleaseSRWLockExclusive(&publish_lock);
}
