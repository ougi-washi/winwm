#pragma once
#include "base/base.h"
#include <windows.h>
enum { ipc_slots = 6, ipc_capacity = 16384 };

////////////////
//~ Ipc

typedef struct w_ipc_channel {
    HANDLE pipe, event, peer_event;
    OVERLAPPED op, peer;
    char peer_byte;
    int phase;
    bool stream, op_pending, peer_pending;
    int head, count;
    char queue[16][64];
    char data[ipc_capacity];
} w_ipc_channel;

typedef void (*ipc_handler)(const char *request, char *reply, size_t cap);

////////////////
//~ Ipc Interface

bool w_ipc_start(w_ipc_channel *channels, const wchar_t *name);
void w_ipc_pump(w_ipc_channel *channels, int slot, ipc_handler handler);
void w_ipc_disconnect(w_ipc_channel *channel);
void w_ipc_publish(w_ipc_channel *channels, const char *event);
void w_ipc_close(w_ipc_channel *channels);
bool w_ipc_name(wchar_t *out, int cap);
