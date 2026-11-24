#include "ipc/ipc.h"
#include <sddl.h>
#include <stdio.h>

////////////////
//~ Ipc

static int subscribers;
enum { connecting, reading, writing, subscribed };

////////////////
//~ Ipc Functions

bool w_ipc_name(wchar_t *out, int cap) {
    DWORD sid;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &sid))
        return false;
    return swprintf(out, cap, L"\\\\.\\pipe\\winwm.v1.%lu", sid) > 0;
}

static void w_listen_again(w_ipc_channel *c) {
    DisconnectNamedPipe(c->pipe);
    ResetEvent(c->event);
    c->op = (OVERLAPPED){.hEvent = c->event};
    c->phase = connecting;
    if (c->stream)
        subscribers--;
    c->stream = false;
    c->head = c->count = 0;
    if (ConnectNamedPipe(c->pipe, &c->op) || GetLastError() == ERROR_PIPE_CONNECTED)
        SetEvent(c->event);
}

static void w_send_reply(w_ipc_channel *c) {
    ResetEvent(c->event);
    c->op = (OVERLAPPED){.hEvent = c->event};
    c->phase = writing;
    DWORD sent;
    if (WriteFile(c->pipe, c->data, (DWORD)strlen(c->data) + 1, &sent, &c->op))
        SetEvent(c->event);
    else if (GetLastError() != ERROR_IO_PENDING)
        w_listen_again(c);
}

bool w_ipc_start(w_ipc_channel *channels) {
    wchar_t name[96];
    if (!w_ipc_name(name, count_of(name)))
        return false;
    HANDLE token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    DWORD bytes = 0;
    GetTokenInformation(token, TokenLogonSid, NULL, 0, &bytes);
    TOKEN_GROUPS *groups = LocalAlloc(LPTR, bytes);
    LPWSTR sid = NULL;
    bool ok = groups && GetTokenInformation(token, TokenLogonSid, groups, bytes, &bytes) &&
              groups->GroupCount && ConvertSidToStringSidW(groups->Groups[0].Sid, &sid);
    CloseHandle(token);
    LocalFree(groups);
    if (!ok)
        return false;
    wchar_t sddl[256];
    swprintf(sddl, count_of(sddl), L"D:P(A;;GA;;;%ls)", sid);
    LocalFree(sid);
    PSECURITY_DESCRIPTOR sd = NULL;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, NULL))
        return false;
    SECURITY_ATTRIBUTES sa = {sizeof(sa), sd, FALSE};
    for (int i = 0; i < ipc_slots; i++) {
        w_ipc_channel *c = &channels[i];
        c->event = CreateEventW(NULL, TRUE, FALSE, NULL);
        c->pipe = CreateNamedPipeW(
            name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (i ? 0 : FILE_FLAG_FIRST_PIPE_INSTANCE),
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, ipc_slots,
            ipc_capacity, ipc_capacity, 0, &sa);
        if (!c->event || c->pipe == INVALID_HANDLE_VALUE) {
            ok = false;
            break;
        }
        w_listen_again(c);
    }
    LocalFree(sd);
    if (!ok)
        w_ipc_close(channels);
    return ok;
}

void w_ipc_pump(w_ipc_channel *c, ipc_handler handler) {
    DWORD n = 0;
    if (c->phase == subscribed)
        return;
    if (c->phase == connecting) {
        ResetEvent(c->event);
        c->op = (OVERLAPPED){.hEvent = c->event};
        c->phase = reading;
        if (ReadFile(c->pipe, c->data, sizeof(c->data), &n, &c->op))
            SetEvent(c->event);
        else if (GetLastError() != ERROR_IO_PENDING)
            w_listen_again(c);
        return;
    }
    if (!GetOverlappedResult(c->pipe, &c->op, &n, FALSE)) {
        w_listen_again(c);
        return;
    }
    if (c->phase == reading) {
        if (n < 2 || c->data[n - 1] || memchr(c->data, 0, n - 1)) {
            w_listen_again(c);
            return;
        }
        if (!strcmp(c->data, "subscribe")) {
            if (subscribers >= ipc_slots - 2) {
                strcpy(c->data, "error: subscriber limit");
                w_send_reply(c);
                return;
            }
            subscribers++;
            c->stream = true;
            c->phase = subscribed;
            ResetEvent(c->event);
            return;
        }
        char reply[ipc_capacity] = {0};
        handler(c->data, reply, sizeof(reply));
        w_str_copy(c->data, sizeof(c->data), reply);
        w_send_reply(c);
    } else if (c->stream) {
        if (c->count) {
            strcpy(c->data, c->queue[c->head]);
            c->head = (c->head + 1) % 16;
            c->count--;
            w_send_reply(c);
        } else {
            c->phase = subscribed;
            ResetEvent(c->event);
        }
    } else {
        // Wait for client acknowledgement (or disconnect) before discarding pipe buffers.
        c->phase = reading;
        ResetEvent(c->event);
        c->op = (OVERLAPPED){.hEvent = c->event};
        if (ReadFile(c->pipe, c->data, sizeof(c->data), &n, &c->op))
            SetEvent(c->event);
        else if (GetLastError() != ERROR_IO_PENDING)
            w_listen_again(c);
    }
}

void w_ipc_publish(w_ipc_channel *channels, const char *event) {
    for (int i = 0; i < ipc_slots; i++) {
        w_ipc_channel *c = channels + i;
        if (!c->stream)
            continue;
        if (c->phase == subscribed) {
            snprintf(c->data, sizeof(c->data), "%s", event);
            w_send_reply(c);
        } else if (c->count < 16) {
            snprintf(c->queue[(c->head + c->count) % 16], 64, "%s", event);
            c->count++;
        } else {
            c->head = 0;
            c->count = 1;
            strcpy(c->queue[0], "resync");
        }
    }
}

void w_ipc_close(w_ipc_channel *channels) {
    for (int i = 0; i < ipc_slots; i++) {
        w_ipc_channel *c = channels + i;
        if (c->pipe && c->pipe != INVALID_HANDLE_VALUE) {
            CancelIoEx(c->pipe, NULL);
            CloseHandle(c->pipe);
        }
        if (c->event)
            CloseHandle(c->event);
        c->pipe = c->event = NULL;
    }
}
