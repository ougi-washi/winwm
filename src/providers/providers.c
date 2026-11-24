#include "providers/providers.h"
#include "base/json.h"
#include <stdlib.h>
#include <stdio.h>

////////////////
//~ Providers

typedef struct w_run_state {
    LONG refs;
    SRWLOCK lock;
    HANDLE job;
    bool cancelled, done, pending;
    HWND owner;
    UINT message;
    w_provider_config spec;
    w_provider_value value;
} w_run_state;

typedef struct w_provider_slot {
    w_provider_config spec;
    w_run_state *run;
    ULONGLONG due, deadline;
    w_provider_value value;
} w_provider_slot;

static w_provider_slot slots[provider_limit];
static int slot_count;
static HWND owner;
static UINT changed;

////////////////
//~ Providers Functions

static void w_release(w_run_state *r) {
    if (InterlockedDecrement(&r->refs) == 0) {
        if (r->job)
            CloseHandle(r->job);
        free(r);
    }
}

static void w_signal_owner(w_run_state *r) {
    if (!r->pending && !r->cancelled)
        r->pending = PostMessageW(r->owner, r->message, 0, 0) != 0;
}

static void w_publish(w_run_state *r, const char *line) {
    w_provider_value v = {.percent = -1};
    if (*line == '{') {
        w_json_token tokens[64];
        w_json_doc doc;
        if (!w_json_parse(&doc, line, tokens, 64) || tokens[0].type != json_object)
            return;
        for (int i = 1; i < tokens[0].after; i = tokens[i + 1].after) {
            char key[32], s[1024];
            if (!w_json_read(tokens + i, key, sizeof(key)))
                continue;
            if (!strcmp(key, "percentage") && tokens[i + 1].type == json_scalar) {
                v.percent = w_clamp_int(atoi(tokens[i + 1].text), 0, 100);
                continue;
            }
            if (!w_json_read(tokens + i + 1, s, sizeof(s)))
                continue;
            if (!strcmp(key, "text"))
                w_os_wide(s, v.text, count_of(v.text));
            else if (!strcmp(key, "tooltip"))
                w_os_wide(s, v.tooltip, count_of(v.tooltip));
            else if (!strcmp(key, "class"))
                w_str_copy(v.class_name, sizeof(v.class_name), s);
        }
    } else if (!w_os_wide(line, v.text, count_of(v.text)))
        return;
    AcquireSRWLockExclusive(&r->lock);
    r->value = v;
    w_signal_owner(r);
    ReleaseSRWLockExclusive(&r->lock);
}

static DWORD WINAPI w_run_worker(void *ptr) {
    w_run_state *r = ptr;
    HANDLE rd = NULL, wr = NULL, nul = NULL;
    wchar_t exe[os_path_cap], line[os_path_cap + 4];
    PROCESS_INFORMATION pi = {0};
    STARTUPINFOEXW si = {0};
    bool attributes = false;
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    if (!w_os_provider(r->spec.executable, exe, count_of(exe)) || !CreatePipe(&rd, &wr, &sa, 4096))
        goto done;
    if (!SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0))
        goto done;
    nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                      OPEN_EXISTING, 0, NULL);
    if (nul == INVALID_HANDLE_VALUE)
        goto done;
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &bytes);
    si.lpAttributeList = malloc(bytes);
    if (!si.lpAttributeList || !InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &bytes))
        goto done;
    attributes = true;
    HANDLE allow[] = {wr, nul};
    if (!UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, allow,
                                   sizeof(allow), NULL, NULL))
        goto done;
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdOutput = wr;
    si.StartupInfo.hStdError = nul;
    si.StartupInfo.hStdInput = nul;
    swprintf(line, count_of(line), L"\"%ls\"", exe);
    if (CreateProcessW(exe, line, NULL, NULL, TRUE,
                       CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, NULL, NULL,
                       &si.StartupInfo, &pi)) {
        AcquireSRWLockExclusive(&r->lock);
        if (r->cancelled || !AssignProcessToJobObject(r->job, pi.hProcess) ||
            ResumeThread(pi.hThread) == (DWORD)-1)
            TerminateProcess(pi.hProcess, 1);
        ReleaseSRWLockExclusive(&r->lock);
    }
    if (!pi.hProcess)
        goto done;
    CloseHandle(wr);
    wr = NULL;
    char record[4096], buf[512];
    size_t used = 0;
    bool overflow = false;
    DWORD n;
    while (ReadFile(rd, buf, sizeof(buf), &n, NULL) && n) {
        for (DWORD i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                if (!overflow) {
                    record[used] = 0;
                    w_publish(r, record);
                }
                used = 0;
                overflow = false;
            } else if (buf[i] != '\r') {
                if (used + 1 < sizeof(record))
                    record[used++] = buf[i];
                else
                    overflow = true;
            }
        }
    }
    if (used && !overflow) {
        record[used] = 0;
        w_publish(r, record);
    }
done:
    if (!pi.hProcess)
        w_publish(r, "unavailable");
    if (pi.hProcess)
        CloseHandle(pi.hProcess);
    if (pi.hThread)
        CloseHandle(pi.hThread);
    if (nul && nul != INVALID_HANDLE_VALUE)
        CloseHandle(nul);
    if (wr)
        CloseHandle(wr);
    if (rd)
        CloseHandle(rd);
    if (attributes)
        DeleteProcThreadAttributeList(si.lpAttributeList);
    free(si.lpAttributeList);
    AcquireSRWLockExclusive(&r->lock);
    r->done = true;
    w_signal_owner(r);
    ReleaseSRWLockExclusive(&r->lock);
    w_release(r);
    return 0;
}

static void w_stop_slot(w_provider_slot *s) {
    w_run_state *r = s->run;
    if (!r)
        return;
    AcquireSRWLockExclusive(&r->lock);
    r->cancelled = true;
    TerminateJobObject(r->job, 1);
    ReleaseSRWLockExclusive(&r->lock);
    s->run = NULL;
    w_release(r);
}

static void w_start_slot(w_provider_slot *s, ULONGLONG now) {
    s->due = 0;
    w_run_state *r = calloc(1, sizeof(*r));
    if (!r)
        return;
    r->refs = 2;
    r->spec = s->spec;
    r->owner = owner;
    r->message = changed;
    r->job = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!r->job ||
        !SetInformationJobObject(r->job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
        !QueueUserWorkItem(w_run_worker, r, WT_EXECUTELONGFUNCTION)) {
        r->refs = 1;
        w_release(r);
        return;
    }
    s->run = r;
    s->deadline = r->spec.stream ? 0 : now + (ULONGLONG)r->spec.timeout * 1000;
}

////////////////
//~ Provider lifecycle

void w_providers_tick(void) {
    KillTimer(owner, provider_timer);
    ULONGLONG now = GetTickCount64(), next = 0;
    for (int i = 0; i < slot_count; i++) {
        w_provider_slot *s = &slots[i];
        if (s->run) {
            w_run_state *r = s->run;
            AcquireSRWLockExclusive(&r->lock);
            bool done = r->done;
            s->value = r->value;
            r->pending = false;
            ReleaseSRWLockExclusive(&r->lock);
            if (done || (s->deadline && now >= s->deadline)) {
                if (!done)
                    wcscpy(s->value.text, L"timed out");
                w_stop_slot(s);
                s->deadline = 0;
                s->due = s->spec.interval ? now + 1000ull * s->spec.interval : 0;
            }
        }
        if (!s->run && s->due && s->due <= now)
            w_start_slot(s, now);
        ULONGLONG when = s->run ? s->deadline : s->due;
        if (when && (!next || when < next))
            next = when;
    }
    if (next)
        SetTimer(owner, provider_timer, next > now ? (UINT)(next - now) : 1, NULL);
}

void w_providers_stop(void) {
    if (owner)
        KillTimer(owner, provider_timer);
    for (int i = 0; i < slot_count; i++)
        w_stop_slot(&slots[i]);
    slot_count = 0;
}

void w_providers_configure(HWND w, UINT message, const w_desktop_config *cfg) {
    w_providers_stop();
    owner = w;
    changed = message;
    for (int i = 0; i < cfg->provider_count; i++) {
        char token[56];
        snprintf(token, sizeof(token), "custom/%s", cfg->providers[i].name);
        if (!w_config_has_module(cfg, token))
            continue;
        slots[slot_count++] = (w_provider_slot){.spec = cfg->providers[i], .due = 1};
    }
    w_providers_tick();
}

bool w_providers_value(const char *name, w_provider_value *value, char *action, size_t cap) {
    for (int i = 0; i < slot_count; i++)
        if (!strcmp(name, slots[i].spec.name)) {
            *value = slots[i].value;
            w_str_copy(action, cap, slots[i].spec.action);
            return true;
        }
    return false;
}

void w_providers_refresh(void) {
    for (int i = 0; i < slot_count; i++)
        if (!slots[i].run)
            slots[i].due = 1;
    w_providers_tick();
}
