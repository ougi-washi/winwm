#include "desktop/desktop.h"
#include "providers/audio.h"
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <stdio.h>

////////////////
//~ Status

typedef struct w_status_sample {
    unsigned mask;
    ULONGLONG idle, total, bytes, time;
    wchar_t network[96];
} w_status_sample;
enum { status_network = 1, status_cpu = 2, status_rate = 4 };

static w_status_sample previous;
static bool running, sample_again;
static HANDLE network_watch;
static LONG network_pending;
static wchar_t cpu[48], rate[48], network[96];
static unsigned enabled_mask;

////////////////
//~ Status Functions

static void CALLBACK w_network_change(void *context, MIB_IPINTERFACE_ROW *row, MIB_NOTIFICATION_TYPE type) {
    (void)row;
    (void)type;
    if (!InterlockedExchange(&network_pending, 1) && !PostMessageW((HWND)context, msg_network, 0, 0))
        InterlockedExchange(&network_pending, 0);
}

void w_desktop_network_changed(void) {
    InterlockedExchange(&network_pending, 0);
    if (enabled_mask & status_network)
        w_desktop_status_sample();
}

static ULONGLONG w_file_ticks(FILETIME ft) {
    return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

////////////////
//~ Status sampling

static DWORD WINAPI w_sample_thread(void *ptr) {
    w_status_sample *s = ptr;
    s->time = GetTickCount64();
    if (s->mask & (status_network | status_rate)) {
        wcscpy(s->network, L"Offline");
        MIB_IF_TABLE2 *table = NULL;
        if (GetIfTable2(&table) == NO_ERROR) {
            for (ULONG i = 0; i < table->NumEntries; i++) {
                MIB_IF_ROW2 *r = &table->Table[i];
                if (r->OperStatus != IfOperStatusUp || r->Type == IF_TYPE_SOFTWARE_LOOPBACK ||
                    !r->InterfaceAndOperStatusFlags.HardwareInterface)
                    continue;
                if (!wcscmp(s->network, L"Offline")) {
                    wcsncpy(s->network, r->Alias, count_of(s->network) - 1);
                }
                s->bytes += r->InOctets + r->OutOctets;
            }
            FreeMibTable(table);
        }
    }
    if (s->mask & status_cpu) {
        FILETIME idle, kernel, user;
        if (GetSystemTimes(&idle, &kernel, &user)) {
            s->idle = w_file_ticks(idle);
            s->total = w_file_ticks(kernel) + w_file_ticks(user);
        }
    }
    if (!PostMessageW(desk.root, msg_sample, 0, (LPARAM)s))
        free(s);
    return 0;
}

void w_desktop_status_sample(void) {
    if (!enabled_mask || desk.hide_bars)
        return;
    if (running) {
        sample_again = true;
        return;
    }
    w_status_sample *s = calloc(1, sizeof(*s));
    if (!s)
        return;
    s->mask = enabled_mask;
    running = QueueUserWorkItem(w_sample_thread, s, WT_EXECUTEDEFAULT) != 0;
    if (!running)
        free(s);
}

static void w_changed(const wchar_t *s, wchar_t *cached, size_t cap) {
    if (wcscmp(s, cached)) {
        wcsncpy(cached, s, cap - 1);
        for (int i = 0; i < output_limit; i++)
            if (desk.outputs[i].live)
                InvalidateRect(desk.outputs[i].bar.hwnd, NULL, FALSE);
    }
}

void w_desktop_status_done(void *ptr) {
    w_status_sample *s = ptr;
    running = false;
    if (!s)
        return;
    if (s->mask == enabled_mask) {
        if (s->mask & status_network)
            w_changed(s->network, network, count_of(network));
        if ((s->mask & status_cpu) && previous.total && s->total > previous.total) {
            wchar_t t[48];
            double busy = 1.0 - (double)(s->idle - previous.idle) / (double)(s->total - previous.total);
            swprintf(t, count_of(t), L"%d%% CPU", w_clamp_int((int)(busy * 100 + 0.5), 0, 100));
            w_changed(t, cpu, count_of(cpu));
        }
        if ((s->mask & status_rate) && previous.time && s->bytes >= previous.bytes &&
            s->time > previous.time) {
            wchar_t t[48];
            swprintf(
                t, count_of(t), L"%llu KiB/s",
                (unsigned long long)((s->bytes - previous.bytes) * 1000 / (s->time - previous.time) / 1024));
            w_changed(t, rate, count_of(rate));
        }
        previous = *s;
    }
    free(s);
    if (sample_again) {
        sample_again = false;
        w_desktop_status_sample();
    }
}

void w_desktop_status_stop(void) {
    KillTimer(desk.root, 28);
    if (network_watch) {
        CancelMibChangeNotify2(network_watch);
        network_watch = NULL;
    }
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
        if (w_config_has_module(&desk.config, "throughput"))
            next_mask |= status_rate;
    }
    if (next_mask != enabled_mask)
        previous = (w_status_sample){0};
    enabled_mask = next_mask;
    if (!(enabled_mask & status_network) && network_watch) {
        CancelMibChangeNotify2(network_watch);
        network_watch = NULL;
    } else if ((enabled_mask & status_network) && !network_watch) {
        HANDLE watch = NULL;
        if (NotifyIpInterfaceChange(AF_UNSPEC, w_network_change, desk.root, FALSE, &watch) == NO_ERROR)
            network_watch = watch;
    }
    KillTimer(desk.root, 28);
    w_desktop_status_sample();
    if ((enabled_mask & (status_cpu | status_rate)) || ((enabled_mask & status_network) && !network_watch))
        SetTimer(desk.root, 28, 5000, NULL);
}

////////////////
//~ Status formatting

bool w_desktop_status_text(const char *key, wchar_t *s, int cap, char *action, size_t acap) {
    const wchar_t *value = NULL;
    const char *cmd = NULL;
    if (!strcmp(key, "network")) {
        value = *network ? network : L"Network";
        cmd = "wifi";
    }
    if (!strcmp(key, "cpu")) {
        value = cpu;
        cmd = "open_tasks";
    }
    if (!strcmp(key, "throughput")) {
        value = rate;
        cmd = "wifi";
    }
    if (!strcmp(key, "media")) {
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
