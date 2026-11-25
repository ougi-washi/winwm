#include "desktop/desktop.h"
#include "native_notifications/listener.h"
#include "native_notifications/banners.h"
#include "native_notifications/publish.h"
#include "native_notifications/toast.h"
#include "tray/host.h"
#include <shellapi.h>
#include <stdlib.h>

static bool listening, banners, banner_ready, capture_healthy, explicitly_denied;
static int profile_source = -1;
static enum w_capture_source source;
static enum w_notification_access notification_access;
static PTP_WORK publish_work;
static HANDLE native_receipt;
enum { publish_failed = -1, balloon_shown = 0, balloon_complete, native_submitted, native_complete };
static struct {
    HWND owner;
    wchar_t title[128], body[1024];
    FILETIME started;
    bool busy, recorded, native;
} outbound;

static void w_local_notice(const wchar_t *title, const wchar_t *body, bool toast, const FILETIME *awaiting) {
    w_notice_item item = {.app=L"winwm",.title=title,.body=body};
    if (awaiting) {
        item.time = *awaiting;
        item.await_echo = true;
    } else GetSystemTimeAsFileTime(&item.time);
    w_notice_add(&item,toast);
}

static void w_balloon_shown(void *context) {
    PostMessageW((HWND)context,msg_notification_published,0,balloon_shown);
}

static void CALLBACK w_publish_worker(PTP_CALLBACK_INSTANCE instance, void *context, PTP_WORK work) {
    (void)instance; (void)context; (void)work;
    int result = publish_failed;
    if (outbound.native) {
        if (w_notification_submit(outbound.title,outbound.body)) {
            if (PostMessageW(outbound.owner,msg_notification_published,0,native_submitted))
                WaitForSingleObject(native_receipt,1800);
            w_notification_submit_done();
            result = native_complete;
        }
    } else if (w_notification_publish_observed(outbound.title,outbound.body,w_balloon_shown,outbound.owner))
        result = balloon_complete;
    PostMessageW(outbound.owner,msg_notification_published,0,result);
}

static bool w_queue_notice(const wchar_t *title, const wchar_t *body) {
    if (outbound.busy || (!capture_healthy && (w_tray_is_active() || !FindWindowW(L"Shell_TrayWnd",NULL)))) return false;
    if (capture_healthy) {
        if (!native_receipt) native_receipt = CreateEventW(NULL,TRUE,FALSE,NULL);
        if (!native_receipt || !ResetEvent(native_receipt)) return false;
    }
    if (!publish_work) publish_work = CreateThreadpoolWork(w_publish_worker,NULL,NULL);
    if (!publish_work) return false;
    outbound.owner = desk.root;
    lstrcpynW(outbound.title,title,count_of(outbound.title));
    lstrcpynW(outbound.body,body,count_of(outbound.body));
    outbound.recorded = false;
    outbound.native = capture_healthy;
    GetSystemTimeAsFileTime(&outbound.started);
    outbound.busy = true;
    SubmitThreadpoolWork(publish_work);
    return true;
}

void w_desktop_notification_published(LPARAM result) {
    if (!outbound.busy) return;
    if (result == native_submitted) {
        if (!outbound.recorded) w_notification_refresh();
        return;
    }
    if (!outbound.recorded) {
        w_local_notice(outbound.title,outbound.body,result == publish_failed || result == native_complete,
                       result == native_complete ? &outbound.started : NULL);
        outbound.recorded = true;
    }
    if (result != balloon_shown) outbound.busy = false;
}

static bool w_own_balloon(const w_notice_item *item) {
    return outbound.busy && !outbound.native && (!_wcsicmp(item->app,L"winwm") || !_wcsicmp(item->app,L"winwm.exe")) &&
           !wcscmp(item->title,outbound.title) && !wcscmp(item->body,outbound.body);
}

static bool w_notice_work(HMONITOR monitor, RECT *bounds) {
    for (int i = 0; i < output_limit; ++i) {
        const w_output_state *o = &desk.outputs[i];
        if (o->live && o->monitor == monitor && o->work.w > 0 && o->work.h > 0) {
            *bounds = (RECT){o->work.x,o->work.y,o->work.x + o->work.w,o->work.y + o->work.h};
            return true;
        }
    }
    return false;
}

const char *w_desktop_notification_status(void) {
    if (source == notice_capture_off) return "disabled";
    if (source == notice_capture_visible) return banner_ready ? "visible banners" : "visible banners unavailable";
    if (!listening || notification_access == notification_unavailable) return "unavailable";
    if (notification_access == notification_denied) return "permission denied";
    if (notification_access == notification_unspecified) return "awaiting access";
    return capture_healthy ? "connected" : "temporarily unavailable";
}

static void w_privacy_settings(void) {
    ShellExecuteW(desk.root,L"open",L"ms-settings:privacy-notifications",NULL,NULL,SW_SHOWNORMAL);
}

static void w_capture_options(HWND button);

void w_desktop_tray_balloons(void) {
    w_tray_balloon balloon;
    while (w_tray_next_balloon(&balloon)) {
        w_notice_cancel_balloon(balloon.token);
        if (!*balloon.body) continue;
        if (source == notice_capture_off) {
            w_tray_balloon_event(balloon.token,NIN_BALLOONHIDE);
            continue;
        }
        QUERY_USER_NOTIFICATION_STATE state;
        bool present = FAILED(SHQueryUserNotificationState(&state)) || state == QUNS_ACCEPTS_NOTIFICATIONS ||
                       (state == QUNS_QUIET_TIME && !(balloon.flags & NIIF_RESPECT_QUIET_TIME));
        w_notice_item item = {.app=*balloon.tip ? balloon.tip : L"Application tray",
                              .title=balloon.title,.body=balloon.body,.balloon=balloon.token,
                              .balloon_event=w_tray_balloon_event};
        GetSystemTimeAsFileTime(&item.time);
        w_notice_add(&item,present);
    }
}

static void w_set_source(enum w_capture_source next) {
    if (next != notice_capture_windows && listening) {
        w_notification_stop();
        listening = capture_healthy = false;
        explicitly_denied = false;
        notification_access = notification_unspecified;
    }
    if (next != notice_capture_visible && banners) {
        w_banners_stop();
        banners = banner_ready = false;
    }
    source = next;
    if (source == notice_capture_off) w_notice_access(L"App notification capture is off.",w_capture_options);
    else if (source == notice_capture_visible && !banners) {
        banners = w_banners_start(desk.root,msg_notification_banner);
        w_notice_access(banners ? L"Connecting to visible Windows banners…" : L"Visible banners unavailable.",w_capture_options);
    } else if (source == notice_capture_windows && !listening) {
        listening = w_notification_listen(desk.root,msg_notifications);
        w_notice_access(listening ? L"Checking Windows notification access…" :
                                    L"Windows notification access is unavailable.",w_capture_options);
    }
}

void w_desktop_notifications_configure(void) {
    const w_notification_config *c = &desk.config.notifications;
    w_notification_submit_target(desk.root,msg_notification_open);
    w_notice_work_area(w_notice_work);
    w_notice_configure(desk.root,c,desk.motion_allowed);
    if (profile_source != (int)c->capture) {
        profile_source = c->capture;
        w_set_source(c->capture);
    }
}

static void w_capture_options(HWND button) {
    RECT bounds;
    HWND window = GetAncestor(button,GA_ROOT);
    if (!window || !GetWindowRect(button,&bounds)) return;
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu,MF_STRING,1,L"Read Windows notifications…");
    AppendMenuW(menu,MF_STRING | (source == notice_capture_visible ? MF_CHECKED : 0),2,L"Watch visible Windows banners");
    AppendMenuW(menu,MF_STRING | (source == notice_capture_off ? MF_CHECKED : 0),3,L"Turn capture off");
    AppendMenuW(menu,MF_SEPARATOR,0,NULL);
    AppendMenuW(menu,MF_STRING,4,L"Windows notification privacy settings");
    TPMPARAMS anchor = {.cbSize=sizeof(anchor),.rcExclude=bounds};
    unsigned choice = TrackPopupMenuEx(menu,TPM_RETURNCMD | TPM_NONOTIFY | TPM_VERTICAL,
                                       bounds.left,bounds.bottom,window,&anchor);
    DestroyMenu(menu);
    if (choice == 2) w_set_source(notice_capture_visible);
    else if (choice == 3) w_set_source(notice_capture_off);
    else if (choice == 4) w_privacy_settings();
    else if (choice == 1) {
        w_set_source(notice_capture_windows);
        if (explicitly_denied) { w_privacy_settings(); return; }
        HRESULT hr = w_notification_request(window);
        if (hr == E_ACCESSDENIED) w_privacy_settings();
        else if (FAILED(hr)) w_notice_access(L"Windows could not open the permission prompt.",w_capture_options);
    }
}

void w_desktop_banner_receive(WPARAM status, LPARAM payload) {
    w_banner *banner = (w_banner *)payload;
    if (!banner) {
        banner_ready = SUCCEEDED((HRESULT)status);
        w_notice_access(banner_ready ? L"Watching visible Windows banners." : L"Visible Windows banners unavailable.",w_capture_options);
        return;
    }
    w_notice_item item = {.time=banner->time,.app=banner->app,.title=banner->title,.body=banner->body};
    if (!w_own_balloon(&item)) w_notice_add(&item,false);
    free(banner);
}

void w_desktop_notifications_receive(LPARAM payload) {
    w_notification_snapshot *batch = (w_notification_snapshot *)payload;
    if (!batch) return;
    notification_access = batch->access;
    explicitly_denied = notification_access == notification_denied && batch->error == E_ACCESSDENIED;
    capture_healthy = notification_access == notification_allowed && SUCCEEDED(batch->error);
    switch (notification_access) {
    case notification_allowed:
        w_notice_access(capture_healthy ? L"Windows notifications connected." :
                                          L"Windows notifications temporarily unavailable.",w_capture_options);
        break;
    case notification_denied:
        w_notice_access(L"Windows notification access has not been granted.",w_capture_options);
        break;
    case notification_unspecified:
        w_notice_access(L"Allow winwm to read Windows notifications.",w_capture_options);
        break;
    case notification_unavailable:
        w_notice_access(L"Windows notification history is unavailable.",w_capture_options);
        break;
    }
    w_notice_batch(true);
    for (size_t i = 0; i < batch->count; ++i) {
        const w_native_notification *n = &batch->entries[i];
        ULARGE_INTEGER time = {.QuadPart = (ULONGLONG)n->created};
        w_notice_item item = {.native_id=n->id,.has_native_id=true,.time={time.LowPart,time.HighPart},.own_native=n->from_winwm,
                              .app=n->app,.title=n->title,.body=n->body};
        if (n->from_winwm) item.app = L"winwm";
        ULARGE_INTEGER started = {.LowPart=outbound.started.dwLowDateTime,.HighPart=outbound.started.dwHighDateTime};
        bool receipt = n->toast && n->from_winwm && outbound.busy && outbound.native &&
                       n->created > 0 && time.QuadPart / 10000 >= started.QuadPart / 10000 &&
                       !wcscmp(n->title,outbound.title) && !wcscmp(n->body,outbound.body);
        item.confirmed_receipt = receipt;
        if (!w_own_balloon(&item)) w_notice_add(&item,receipt || (n->toast && !desk.preview));
        if (receipt) {
            outbound.recorded = true;
            SetEvent(native_receipt);
        }
    }
    w_notice_batch(false);
    free(batch);
}

static void w_show_notice(const wchar_t *title, const char *body, bool publish) {
    wchar_t message[1024];
    w_str_copy(desk.status,sizeof(desk.status),body);
    if (!w_os_wide(body,message,count_of(message))) wcscpy(message,L"See winwm.exe doctor for details.");
    if (publish && desk.config.notifications.enabled && w_queue_notice(title,message)) return;
    w_local_notice(title,message,true,NULL);
}

void w_desktop_notice(const char *body) {
    w_show_notice(L"winwm",body,false);
}

void w_desktop_notify(const wchar_t *title, const char *body) {
    w_show_notice(title,body,true);
}

void w_desktop_notifications_open(int output, const w_popup_anchor *anchor) {
    if (listening) w_notification_refresh();
    w_notice_open(desk.outputs[output].monitor,anchor);
}

void w_desktop_notifications_stop(void) {
    w_notification_submit_target(NULL,0);
    if (listening) w_notification_stop();
    if (banners) w_banners_stop();
    if (publish_work) {
        w_notification_publish_stop();
        if (native_receipt) SetEvent(native_receipt);
        WaitForThreadpoolWorkCallbacks(publish_work,FALSE);
        w_notification_submit_stop();
        CloseThreadpoolWork(publish_work);
        publish_work = NULL;
    }
    if (native_receipt) CloseHandle(native_receipt);
    w_notice_shutdown();
}
