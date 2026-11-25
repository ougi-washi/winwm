#include "ipc/ipc.h"
#include <sddl.h>
#include <stdio.h>

////////////////
//~ Ipc

enum { connecting, reading, writing, subscribed };

////////////////
//~ Ipc Functions

bool w_ipc_name(wchar_t *out, int cap) {
    DWORD sid;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &sid))
        return false;
    return swprintf(out, cap, L"\\\\.\\pipe\\winwm.v1.%lu", sid) > 0;
}

static void w_drain_io(HANDLE pipe, OVERLAPPED *op, bool *pending) {
    if (*pending) {
        DWORD transferred;
        CancelIoEx(pipe, op);
        GetOverlappedResult(pipe, op, &transferred, TRUE);
        *pending = false;
    }
}

static void w_listen(w_ipc_channel *c) {
    ResetEvent(c->event);
    c->op = (OVERLAPPED){.hEvent = c->event};
    c->phase = connecting;
    c->op_pending = !ConnectNamedPipe(c->pipe, &c->op) && GetLastError() == ERROR_IO_PENDING;
    if (!c->op_pending)
        SetEvent(c->event);
}

void w_ipc_disconnect(w_ipc_channel *c) {
    w_drain_io(c->pipe, &c->op, &c->op_pending);
    w_drain_io(c->pipe, &c->peer, &c->peer_pending);
    DisconnectNamedPipe(c->pipe);
    ResetEvent(c->peer_event);
    c->stream = false;
    c->head = c->count = 0;
    w_listen(c);
}

static void w_read_request(w_ipc_channel *c) {
    DWORD n;
    ResetEvent(c->event);
    c->op = (OVERLAPPED){.hEvent = c->event};
    c->phase = reading;
    if (ReadFile(c->pipe, c->data, sizeof(c->data), &n, &c->op))
        SetEvent(c->event);
    else if (GetLastError() == ERROR_IO_PENDING)
        c->op_pending = true;
    else
        w_ipc_disconnect(c);
}

static void w_send_reply(w_ipc_channel *c) {
    ResetEvent(c->event);
    c->op = (OVERLAPPED){.hEvent = c->event};
    c->phase = writing;
    DWORD sent;
    if (WriteFile(c->pipe, c->data, (DWORD)strlen(c->data) + 1, &sent, &c->op))
        SetEvent(c->event);
    else if (GetLastError() == ERROR_IO_PENDING)
        c->op_pending = true;
    else
        w_ipc_disconnect(c);
}

bool w_ipc_start(w_ipc_channel *channels, const wchar_t *name) {
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
        c->peer_event = CreateEventW(NULL, TRUE, FALSE, NULL);
        c->pipe = CreateNamedPipeW(
            name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (i ? 0 : FILE_FLAG_FIRST_PIPE_INSTANCE),
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, ipc_slots,
            ipc_capacity, ipc_capacity, 0, &sa);
        if (!c->event || !c->peer_event || c->pipe == INVALID_HANDLE_VALUE) {
            ok = false;
            break;
        }
        w_listen(c);
    }
    LocalFree(sd);
    if (!ok)
        w_ipc_close(channels);
    return ok;
}

void w_ipc_pump(w_ipc_channel *channels, int slot, ipc_handler handler) {
    w_ipc_channel *c = &channels[slot];
    DWORD n = 0;
    if (c->phase == subscribed)
        return;
    c->op_pending = false;
    if (c->phase == connecting) {
        w_read_request(c);
        return;
    }
    if (!GetOverlappedResult(c->pipe, &c->op, &n, FALSE)) {
        w_ipc_disconnect(c);
        return;
    }
    if (c->phase == reading) {
        if (n < 2 || c->data[n - 1] || memchr(c->data, 0, n - 1)) {
            w_ipc_disconnect(c);
            return;
        }
        if (!strcmp(c->data, "subscribe")) {
            int subscribers = 0;
            for (int i = 0; i < ipc_slots; i++)
                subscribers += channels[i].stream;
            if (subscribers >= ipc_slots - 2) {
                strcpy(c->data, "error: subscriber limit");
                w_send_reply(c);
                return;
            }
            c->stream = true;
            c->phase = subscribed;
            ResetEvent(c->event);
            // Subscribers only receive events. A read completes when the peer
            // leaves (or sends unexpected input), even on an idle desktop.
            c->peer = (OVERLAPPED){.hEvent = c->peer_event};
            if (ReadFile(c->pipe, &c->peer_byte, 1, &n, &c->peer) || GetLastError() != ERROR_IO_PENDING)
                SetEvent(c->peer_event);
            else
                c->peer_pending = true;
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
        w_read_request(c);
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
            w_drain_io(c->pipe, &c->op, &c->op_pending);
            w_drain_io(c->pipe, &c->peer, &c->peer_pending);
            CloseHandle(c->pipe);
        }
        if (c->event)
            CloseHandle(c->event);
        if (c->peer_event)
            CloseHandle(c->peer_event);
        *c = (w_ipc_channel){0};
    }
}
