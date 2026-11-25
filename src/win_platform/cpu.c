#include "win_platform/cpu.h"
#include <pdh.h>
#include <pdhmsg.h>
#include <stdio.h>

static uint64_t w_ticks(FILETIME t) {
    return ((uint64_t)t.dwHighDateTime << 32) | t.dwLowDateTime;
}

w_cpu_times w_cpu_read(void) {
    w_cpu_times times = {.processors = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)};
    if (GetActiveProcessorGroupCount() == 1) {
        FILETIME idle, kernel, user;
        if (GetSystemTimes(&idle, &kernel, &user)) {
            times.idle = w_ticks(idle);
            times.total = w_ticks(kernel) + w_ticks(user);
        }
    } else {
        // GetSystemTimes reports only the calling thread's primary group.
        PDH_HQUERY query;
        PDH_HCOUNTER counter;
        PDH_RAW_COUNTER raw;
        if (PdhOpenQueryW(NULL, 0, &query) == ERROR_SUCCESS) {
            if (PdhAddEnglishCounterW(query, L"\\Processor Information(_Total)\\% Idle Time", 0, &counter) == ERROR_SUCCESS &&
                PdhCollectQueryData(query) == ERROR_SUCCESS &&
                PdhGetRawCounterValue(counter, NULL, &raw) == ERROR_SUCCESS &&
                (raw.CStatus == PDH_CSTATUS_VALID_DATA || raw.CStatus == PDH_CSTATUS_NEW_DATA) &&
                raw.FirstValue >= 0 && raw.SecondValue > 0) {
                times.idle = raw.FirstValue;
                times.total = raw.SecondValue;
            }
            PdhCloseQuery(query);
        }
    }
    return times;
}

double w_cpu_percent(w_cpu_times before, w_cpu_times after) {
    if (!before.total || !before.processors || before.processors != after.processors ||
        after.total <= before.total || after.idle < before.idle) return -1;
    uint64_t elapsed = after.total - before.total, idle = after.idle - before.idle;
    if (idle > elapsed) return -1;
    return 100.0 * (elapsed - idle) / elapsed;
}

void w_cpu_format(wchar_t *text, size_t cap, double percent) {
    if (percent < 0) swprintf(text, cap, L"—");
    else if (percent == 0) swprintf(text, cap, L"0%%");
    else if (percent < 0.05) swprintf(text, cap, L"<0.1%%");
    else swprintf(text, cap, L"%.1f%%", percent);
}
