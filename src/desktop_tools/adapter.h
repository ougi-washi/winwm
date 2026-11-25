#pragma once
#include "win_platform/win_platform.h"
#include <ws2tcpip.h>
#include <iphlpapi.h>

bool w_adapter_show(HWND owner, const IP_ADAPTER_ADDRESSES *adapter);
DWORD w_adapter_apply_dns(const wchar_t *request);
