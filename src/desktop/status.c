#include "desktop/desktop.h"
#include "providers/audio.h"
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <stdio.h>

////////////////
//~ Status

typedef struct w_status_sample {
    unsigned mask, generation;
    bool refresh_network, network_error, routed;
    ULONGLONG idle, total, rx, tx, time, link, luid;
    MEMORYSTATUSEX memory;
    wchar_t network[96], adapter[192];
    bar_icon network_icon;
} w_status_sample;
enum { status_network = 1, status_cpu = 2, status_rate = 4, status_memory = 8 };

static w_status_sample previous;
static bool running, sample_again;
static HANDLE network_watch, route_watch;
static LONG network_pending;
static wchar_t cpu[48], rate[64], network[96], memory[48];
static ULONGLONG download, upload;
static bool rate_ready;
static unsigned enabled_mask, generation;
static bool network_dirty;
static UINT poll_interval;

////////////////
//~ Status Functions

static void w_notify_network(void *context) {
    if (!InterlockedExchange(&network_pending, 1) && !PostMessageW((HWND)context, msg_network, 0, 0))
        InterlockedExchange(&network_pending, 0);
}

static void CALLBACK w_network_change(void *context, MIB_IPINTERFACE_ROW *row, MIB_NOTIFICATION_TYPE type) {
    (void)row; (void)type;
    w_notify_network(context);
}

static void CALLBACK w_route_change(void *context, MIB_IPFORWARD_ROW2 *row, MIB_NOTIFICATION_TYPE type) {
    (void)type;
    if (!row || !row->DestinationPrefix.PrefixLength) w_notify_network(context);
}

void w_desktop_network_changed(void) {
    InterlockedExchange(&network_pending, 0);
    if (enabled_mask & (status_network | status_rate)) {
        network_dirty = true;
        w_desktop_status_sample();
    }
}

static ULONGLONG w_file_ticks(FILETIME ft) {
    return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

static void w_read_link(w_status_sample *s, const MIB_IF_ROW2 *row) {
    lstrcpynW(s->network,row->Alias,count_of(s->network));
    lstrcpynW(s->adapter,row->Description,count_of(s->adapter));
    s->network_icon = row->Type == IF_TYPE_IEEE80211 ? icon_network : icon_ethernet;
    s->rx = row->InOctets;
    s->tx = row->OutOctets;
    s->link = row->ReceiveLinkSpeed;
    s->luid = row->InterfaceLuid.Value;
}

static void w_sample_network(w_status_sample *s) {
    MIB_IF_ROW2 row = {0};
    if (!s->refresh_network) {
        if (!(s->mask & status_rate) || !s->luid) return;
        row.InterfaceLuid.Value = s->luid;
        if (GetIfEntry2(&row) == NO_ERROR && row.OperStatus == IfOperStatusUp) {
            w_read_link(s,&row);
            return;
        }
    }
    s->routed = false;
    s->adapter[0] = 0;
    s->luid = s->rx = s->tx = s->link = 0;
    s->network_icon = icon_offline;
    wcscpy(s->network,L"Offline");

    MIB_IPFORWARD_TABLE2 *routes = NULL;
    s->network_error = GetIpForwardTable2(AF_UNSPEC,&routes) != NO_ERROR;
    if (!s->network_error) {
        ULONGLONG best = UINT64_MAX;
        NET_IFINDEX best_index = UINT32_MAX;
        for (ULONG i=0; i<routes->NumEntries; ++i) {
            const MIB_IPFORWARD_ROW2 *r = &routes->Table[i];
            if (r->DestinationPrefix.PrefixLength || r->Loopback || !r->ValidLifetime) continue;
            MIB_IPINTERFACE_ROW ip = {.Family=r->DestinationPrefix.Prefix.si_family,.InterfaceLuid=r->InterfaceLuid};
            if (GetIpInterfaceEntry(&ip) || !ip.Connected) continue;
            ULONGLONG metric = (ULONGLONG)r->Metric + ip.Metric;
            if (metric > best || (metric == best && r->InterfaceIndex >= best_index)) continue;
            row = (MIB_IF_ROW2){.InterfaceLuid=r->InterfaceLuid};
            if (GetIfEntry2(&row) || row.OperStatus != IfOperStatusUp || row.Type == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            best = metric;
            best_index = r->InterfaceIndex;
            w_read_link(s,&row);
            s->routed = true;
        }
        FreeMibTable(routes);
    }
    if (s->routed) return;
    // An isolated LAN may have a working link and no default route.
    MIB_IF_TABLE2 *interfaces = NULL;
    if (GetIfTable2(&interfaces) != NO_ERROR) {
        s->network_error = true;
        wcscpy(s->network,L"Network unavailable");
        return;
    }
    const MIB_IF_ROW2 *local = NULL;
    for (ULONG i=0; i<interfaces->NumEntries; ++i) {
        const MIB_IF_ROW2 *r = &interfaces->Table[i];
        if (r->OperStatus == IfOperStatusUp && r->Type != IF_TYPE_SOFTWARE_LOOPBACK &&
            r->InterfaceAndOperStatusFlags.HardwareInterface && (!local || r->InterfaceIndex < local->InterfaceIndex))
            local = r;
    }
    if (local) w_read_link(s,local);
    FreeMibTable(interfaces);
}

////////////////
//~ Status sampling

static DWORD WINAPI w_sample_thread(void *ptr) {
    w_status_sample *s = ptr;
    s->time = GetTickCount64();
    if (s->mask & (status_network | status_rate)) w_sample_network(s);
    if (s->mask & status_cpu) {
        FILETIME idle, kernel, user;
        s->idle = s->total = 0;
        if (GetSystemTimes(&idle, &kernel, &user)) {
            s->idle = w_file_ticks(idle);
            s->total = w_file_ticks(kernel) + w_file_ticks(user);
        }
    }
    if (s->mask & status_memory) {
        s->memory = (MEMORYSTATUSEX){.dwLength = sizeof(s->memory)};
        if (!GlobalMemoryStatusEx(&s->memory)) s->memory.ullTotalPhys = 0;
    }
    if (!PostMessageW(desk.root, msg_sample, 0, (LPARAM)s))
        free(s);
    return 0;
}

static void w_poll_after(UINT ms) {
    if (ms == poll_interval) return;
    if (ms) poll_interval = SetTimer(desk.root,28,ms,NULL) ? ms : 0;
    else {
        KillTimer(desk.root,28);
        poll_interval = 0;
    }
}

static void w_poll_schedule(void) {
    if ((enabled_mask & status_cpu && previous.total && !*cpu) ||
        (enabled_mask & status_rate && previous.luid && !rate_ready))
        w_poll_after(250); // A percentage or rate needs two samples.
    else if ((enabled_mask & (status_cpu | status_rate | status_memory)) ||
             (enabled_mask & status_network && (!network_watch || !route_watch || previous.network_error)))
        w_poll_after(5000);
    else w_poll_after(0);
}

void w_desktop_status_sample(void) {
    if (!enabled_mask || desk.hide_bars)
        return;
    if (running) {
        sample_again = true;
        return;
    }
    w_status_sample *s = malloc(sizeof(*s));
    if (!s) {
        w_poll_after(5000);
        return;
    }
    *s = previous;
    s->mask = enabled_mask;
    s->generation = generation;
    s->refresh_network = network_dirty || !network_watch || !route_watch || previous.network_error;
    running = QueueUserWorkItem(w_sample_thread, s, WT_EXECUTEDEFAULT) != 0;
    if (running) network_dirty = false;
    else {
        free(s);
        w_poll_after(5000);
    }
}

static void w_changed(const char *module, const wchar_t *s, wchar_t *cached, size_t cap) {
    if (wcscmp(s, cached)) {
        wcsncpy(cached, s, cap - 1);
        cached[cap - 1] = 0;
        for (int i = 0; i < output_limit; i++)
            if (desk.outputs[i].live && !desk.outputs[i].bar_hidden)
                w_bar_invalidate(&desk.outputs[i].bar, module);
    }
}

void w_desktop_status_done(void *ptr) {
    w_status_sample *s = ptr;
    running = false;
    if (!s)
        return;
    if (s->generation == generation && s->mask == enabled_mask) {
        if (s->mask & status_network) {
            for (int i = 0; i < output_limit; i++) {
                w_bar_window *bar = &desk.outputs[i].bar;
                if (desk.outputs[i].live && bar->network_icon != s->network_icon) {
                    bar->network_icon = s->network_icon;
                    w_bar_invalidate(bar, "network");
                }
            }
            w_changed("network", s->network, network, count_of(network));
        }
        if (s->mask & status_cpu) {
            if (!s->total) w_changed("cpu", L"—", cpu, count_of(cpu));
            else if (previous.total && s->total > previous.total && s->idle >= previous.idle) {
                wchar_t t[48];
                double busy = 1.0 - (double)(s->idle - previous.idle) / (double)(s->total - previous.total);
                swprintf(t, count_of(t), L"%d%%", w_clamp_int((int)(busy * 100 + 0.5), 0, 100));
                w_changed("cpu", t, cpu, count_of(cpu));
            }
        }
        if (s->mask & status_rate) {
            wchar_t t[64] = L"—";
            rate_ready = s->luid && s->luid == previous.luid && s->rx >= previous.rx &&
                         s->tx >= previous.tx && s->time > previous.time;
            if (rate_ready) {
                double seconds = (s->time - previous.time) / 1000.0;
                download = (ULONGLONG)((s->rx - previous.rx) / (1024 * seconds));
                upload = (ULONGLONG)((s->tx - previous.tx) / (1024 * seconds));
                swprintf(t, count_of(t), L"↓%llu ↑%llu KiB/s", download, upload);
            }
            w_changed("throughput", t, rate, count_of(rate));
        }
        if (s->mask & status_memory) {
            wchar_t t[48] = L"—";
            if (s->memory.ullTotalPhys)
                swprintf(t, count_of(t), L"%lu%%", s->memory.dwMemoryLoad);
            w_changed("memory", t, memory, count_of(memory));
        }
        previous = *s;
        w_poll_schedule();
    }
    free(s);
    if (sample_again) {
        sample_again = false;
        w_desktop_status_sample();
    }
}

void w_desktop_status_stop(void) {
    w_poll_after(0);
    if (network_watch) {
        CancelMibChangeNotify2(network_watch);
        network_watch = NULL;
    }
    if (route_watch) {
        CancelMibChangeNotify2(route_watch);
        route_watch = NULL;
    }
    ++generation;
    enabled_mask = 0;
    sample_again = false;
}

void w_desktop_status_configure(void) {
    unsigned next_mask = 0;
    if (!desk.hide_bars) {
        if (w_config_has_module(&desk.config, "audio"))
            w_audio_start(desk.root, msg_audio);
        if (w_config_has_module(&desk.config, "network"))
            next_mask |= status_network;
        if (w_config_has_module(&desk.config, "cpu"))
            next_mask |= status_cpu;
        if (w_config_has_module(&desk.config, "memory"))
            next_mask |= status_memory;
        if (w_config_has_module(&desk.config, "throughput"))
            next_mask |= status_rate;
    }
    if (next_mask != enabled_mask) {
        ++generation;
        previous = (w_status_sample){0};
        rate_ready = false;
        *cpu = *rate = *network = *memory = 0;
    }
    enabled_mask = next_mask;
    network_dirty = !!(enabled_mask & (status_network | status_rate));
    if (network_dirty) {
        HANDLE watch = NULL;
        if (!network_watch && !NotifyIpInterfaceChange(AF_UNSPEC,w_network_change,desk.root,FALSE,&watch))
            network_watch = watch;
        if (!route_watch && !NotifyRouteChange2(AF_UNSPEC,w_route_change,desk.root,FALSE,&watch))
            route_watch = watch;
    } else {
        if (network_watch) CancelMibChangeNotify2(network_watch);
        if (route_watch) CancelMibChangeNotify2(route_watch);
        network_watch = route_watch = NULL;
    }
    w_poll_schedule();
    w_desktop_status_sample();
}

////////////////
//~ Status formatting

bool w_desktop_status_text(const char *key, wchar_t *s, int cap, char *action, size_t acap) {
    if (cap <= 0) return false;
    const wchar_t *value = NULL;
    const char *cmd = NULL;
    if (!strcmp(key, "network")) {
        value = *network ? network : L"Network";
        cmd = "wifi";
    } else if (!strcmp(key, "cpu")) {
        value = cpu[0] ? cpu : L"…";
        cmd = "activity";
    } else if (!strcmp(key, "memory")) {
        value = memory[0] ? memory : L"…";
        cmd = "activity";
    } else if (!strcmp(key, "throughput")) {
        value = rate[0] ? rate : L"…";
        cmd = "wifi";
    } else if (!strcmp(key, "media")) {
        value = L"Play / pause";
        cmd = "media_play";
    }
    if (!value)
        return false;
    wcsncpy(s, value, cap - 1);
    s[cap - 1] = 0;
    w_str_copy(action, acap, cmd);
    return true;
}

bool w_desktop_status_detail(const char *key, wchar_t *text, int cap) {
    if (cap <= 0) return false;
    if (!strcmp(key, "audio")) {
        w_audio_state state = w_audio_read();
        if (!state.available || !state.device[0]) return false;
        swprintf(text, cap, L"%ls\n%ls · %d%%", state.device, state.muted ? L"Muted" : L"Output volume", state.percent);
    } else if (!strcmp(key, "memory") && previous.memory.ullTotalPhys) {
        const double gib = 1073741824.0;
        double total = previous.memory.ullTotalPhys / gib;
        swprintf(text, cap, L"%.1f of %.1f GiB in use · %ls", total - previous.memory.ullAvailPhys / gib, total, memory);
    } else if (!strcmp(key, "cpu") && *cpu) {
        SYSTEM_INFO si;
        GetNativeSystemInfo(&si);
        swprintf(text, cap, L"%ls utilization · %lu logical processors", cpu, si.dwNumberOfProcessors);
    } else if (!strcmp(key, "throughput") && rate_ready) {
        swprintf(text, cap, L"%ls\nDownload %llu KiB/s · Upload %llu KiB/s", previous.network, download, upload);
    } else if (!strcmp(key, "network") && previous.luid) {
        swprintf(text,cap,L"%ls · %ls\n%ls",previous.network,previous.routed ? L"Default route" : L"Local link",previous.adapter);
        text[cap-1] = 0;
        if (previous.link >= 1000000 && previous.link != UINT64_MAX) {
            size_t n = wcslen(text);
            swprintf(text+n, cap-n, L" · %llu Mbps", previous.link/1000000);
        }
    } else return false;
    text[cap-1] = 0;
    return true;
}
