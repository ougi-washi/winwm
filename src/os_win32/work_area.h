#pragma once
#include "os_win32/os_win32.h"

bool w_work_areas_open(DWORD session_pid, bool recovery);
RECT w_work_area_available(int output, const MONITORINFOEXW *info);
void w_work_area_publish(int output, HMONITOR monitor, RECT work);
void w_work_areas_close(void);
