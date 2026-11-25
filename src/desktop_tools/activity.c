#include "desktop_tools/tools.h"
#include "theme/theme.h"
#include "theme/icon_button.h"
#include "win_platform/cpu.h"
#include <commctrl.h>
#include <psapi.h>
#include <shlwapi.h>
#include <stdio.h>
#include <tlhelp32.h>

typedef struct {
    wchar_t name[MAX_PATH];
    DWORD pid;
    uint64_t created, time, memory;
    double cpu; // Percent of all logical processors, or -1 until measured.
    bool has_memory;
} w_process;

typedef struct {
    w_process *rows;
    int count;
    w_cpu_times cpu;
    uint64_t sampled;
    MEMORYSTATUSEX memory;
} w_sample;

static HWND panel, search_label, search, table, summary, pause_button, copy_pid, copy_path, close_app, end_task, feedback, empty;
static HANDLE worker;
static HFONT font;
static w_sample current, incoming;
static w_process **visible;
static int visible_count, sorted_by = 2;
static bool descending = true, paused, discard_sample;
enum { sample_ready = WM_APP + 1 };
static const wchar_t *columns[] = {L"Process", L"PID", L"CPU", L"Memory"};

static uint64_t w_timestamp(FILETIME t) {
    return ((uint64_t)t.dwHighDateTime << 32) | t.dwLowDateTime;
}

static int w_by_pid(const void *a, const void *b) {
    DWORD left = ((const w_process *)a)->pid, right = ((const w_process *)b)->pid;
    return (left > right) - (left < right);
}

static bool w_collect(w_sample *s) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry = {.dwSize = sizeof(entry)};
    int capacity = 0;
    BOOL more = Process32FirstW(snapshot, &entry);
    while (more) {
        if (s->count == capacity) {
            capacity = capacity ? capacity * 2 : 128;
            w_process *rows = realloc(s->rows, (size_t)capacity * sizeof(*rows));
            if (!rows) {
                CloseHandle(snapshot);
                return false;
            }
            s->rows = rows;
        }
        w_process *p = &s->rows[s->count++];
        *p = (w_process){.pid = entry.th32ProcessID, .cpu = -1};
        lstrcpynW(p->name, entry.szExeFile, count_of(p->name));
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, p->pid);
        if (process) {
            FILETIME start, stop, kernel, user;
            PROCESS_MEMORY_COUNTERS mem = {.cb = sizeof(mem)};
            if (GetProcessTimes(process, &start, &stop, &kernel, &user)) {
                p->created = w_timestamp(start);
                p->time = w_timestamp(kernel) + w_timestamp(user);
            }
            if (GetProcessMemoryInfo(process, &mem, sizeof(mem))) {
                p->has_memory = true;
                p->memory = mem.WorkingSetSize;
            }
            CloseHandle(process);
        }
        more = Process32NextW(snapshot, &entry);
    }
    DWORD error = GetLastError();
    CloseHandle(snapshot);
    if (error != ERROR_NO_MORE_FILES || !s->count) return false;
    s->sampled = GetTickCount64();
    s->cpu = w_cpu_read();
    s->memory.dwLength = sizeof(s->memory);
    GlobalMemoryStatusEx(&s->memory);
    qsort(s->rows, s->count, sizeof(*s->rows), w_by_pid);
    return true;
}

static DWORD WINAPI w_sample_thread(void *arg) {
    bool ok = w_collect(&incoming);
    PostMessageW((HWND)arg, sample_ready, ok, 0);
    return 0;
}

static void w_request_sample(void) {
    if (worker || (current.sampled && GetTickCount64() - current.sampled < 750)) return;
    worker = CreateThread(NULL, 0, w_sample_thread, panel, 0, NULL);
    if (!worker) SetWindowTextW(feedback, L"Could not start sampling. Press F5 to retry.");
}

static int w_sort_rows(const void *a, const void *b) {
    const w_process *left = *(w_process *const *)a, *right = *(w_process *const *)b;
    int result = 0;
    if (sorted_by == 0) result = CompareStringOrdinal(left->name, -1, right->name, -1, TRUE) - CSTR_EQUAL;
    else if (sorted_by == 1) result = w_by_pid(left, right);
    else if (sorted_by == 2) {
        if ((left->cpu < 0) != (right->cpu < 0)) return left->cpu < 0 ? 1 : -1;
        result = (left->cpu > right->cpu) - (left->cpu < right->cpu);
    } else {
        if (left->has_memory != right->has_memory) return left->has_memory ? -1 : 1;
        result = (left->memory > right->memory) - (left->memory < right->memory);
    }
    if (result) return descending ? -result : result;
    result = CompareStringOrdinal(left->name, -1, right->name, -1, TRUE) - CSTR_EQUAL;
    return result ? result : w_by_pid(left, right);
}

static w_process *w_selected(void) {
    int index = ListView_GetNextItem(table, -1, LVNI_SELECTED);
    return index < 0 || index >= visible_count ? NULL : visible[index];
}

static void w_actions(void) {
    w_process *p = w_selected();
    bool identifiable = p && p->created;
    bool editable = identifiable && p->pid != GetCurrentProcessId();
    EnableWindow(copy_pid, p != NULL);
    EnableWindow(copy_path, identifiable);
    EnableWindow(close_app, editable);
    EnableWindow(end_task, editable);
}

static void w_rebuild(w_process *selection) {
    wchar_t query[256], pid[32];
    GetWindowTextW(search, query, count_of(query));
    StrTrimW(query, L" \t");
    // Selection can refer to the same table positions being reordered. Store the identity first.
    DWORD selected_pid = selection ? selection->pid : 0;
    uint64_t selected_start = selection ? selection->created : 0;
    visible_count = 0;
    for (int i = 0; i < current.count; i++) {
        w_process *p = &current.rows[i];
        if (*query && !StrStrIW(p->name, query)) {
            swprintf(pid, count_of(pid), L"%lu", p->pid);
            if (wcscmp(query, pid)) continue;
        }
        visible[visible_count++] = p;
    }
    if (visible_count) qsort(visible, visible_count, sizeof(*visible), w_sort_rows);
    int selected_index = -1;
    for (int i = 0; selection && i < visible_count; i++) {
        if (visible[i]->pid == selected_pid && visible[i]->created == selected_start) {
            selected_index = i;
            break;
        }
    }
    SendMessageW(table, WM_SETREDRAW, FALSE, 0);
    ListView_SetItemState(table, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetItemCountEx(table, visible_count, LVSICF_NOSCROLL | LVSICF_NOINVALIDATEALL);
    if (selected_index >= 0) ListView_SetItemState(table, selected_index, LVIS_SELECTED|LVIS_FOCUSED, LVIS_SELECTED|LVIS_FOCUSED);
    SendMessageW(table, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(table, NULL, FALSE);
    SetWindowTextW(empty, current.count ? L"No processes match this search." : L"Reading processes…");
    ShowWindow(empty, visible_count ? SW_HIDE : SW_SHOWNOACTIVATE);
    w_actions();
}

static void w_apply_sample(bool ok) {
    WaitForSingleObject(worker, INFINITE);
    CloseHandle(worker);
    worker = NULL;
    w_process *selected = w_selected();
    w_process **next = ok && !discard_sample ? realloc(visible, (size_t)incoming.count * sizeof(*visible)) : NULL;
    if (next) {
        visible = next;
        int j = 0;
        double capacity = (double)(incoming.sampled-current.sampled) * 10000.0 * incoming.cpu.processors;
        for (int i = 0; i < incoming.count && capacity > 0; i++) {
            w_process *p = &incoming.rows[i];
            while (j < current.count && current.rows[j].pid < p->pid) j++;
            if (j < current.count && current.rows[j].pid == p->pid && p->created && p->created == current.rows[j].created &&
                incoming.cpu.processors == current.cpu.processors && p->time >= current.rows[j].time) {
                double percent = 100.0 * (p->time-current.rows[j].time) / capacity;
                p->cpu = min(percent, 100.0);
            }
        }
        wchar_t cpu[24], text[192];
        w_cpu_format(cpu, count_of(cpu), w_cpu_percent(current.cpu, incoming.cpu));
        swprintf(text, count_of(text), L"CPU   %ls      Memory   %.1f / %.1f GiB      Processes   %d", cpu,
                 (incoming.memory.ullTotalPhys-incoming.memory.ullAvailPhys)/1073741824.0,
                 incoming.memory.ullTotalPhys/1073741824.0, incoming.count);
        SetWindowTextW(summary, text);
        w_process *old_rows = current.rows;
        current = incoming;
        incoming.rows = NULL;
        w_rebuild(selected);
        free(old_rows);
    } else if (!discard_sample) {
        SetWindowTextW(feedback, L"Process list unavailable. Press F5 to retry.");
    }
    free(incoming.rows);
    incoming = (w_sample){0};
    discard_sample = false;
}

static void w_toggle_pause(void) {
    paused = !paused;
    discard_sample = paused && worker;
    w_icon_button(pause_button, paused ? L"\uE768" : L"\uE769",
                  paused ? L"Resume process updates" : L"Pause process updates");
    if (!paused) w_request_sample();
}

static void w_copy_text(const wchar_t *text) {
    size_t bytes = (wcslen(text)+1)*sizeof(*text);
    HGLOBAL copy = GlobalAlloc(GMEM_MOVEABLE, bytes);
    wchar_t *buffer = copy ? GlobalLock(copy) : NULL;
    if (buffer) {
        memcpy(buffer, text, bytes);
        GlobalUnlock(copy);
        if (OpenClipboard(panel)) {
            if (EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, copy)) copy = NULL;
            CloseClipboard();
        }
    }
    SetWindowTextW(feedback, copy || !buffer ? L"Clipboard unavailable." : L"Copied to clipboard.");
    if (copy) GlobalFree(copy);
}

typedef struct { DWORD pid; HWND window; } w_main_window;

static BOOL CALLBACK w_find_window(HWND w, LPARAM data) {
    w_main_window *result = (w_main_window *)data;
    DWORD pid;
    GetWindowThreadProcessId(w, &pid);
    if (pid != result->pid || !IsWindowVisible(w) || GetWindow(w, GW_OWNER) ||
        (GetWindowLongPtrW(w, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) || w == GetShellWindow()) return TRUE;
    result->window = w;
    return FALSE;
}

static void w_process_action(HWND button) {
    w_process *p = w_selected();
    if (!p) return;
    if (button == copy_pid) {
        wchar_t pid[32];
        swprintf(pid, count_of(pid), L"%lu", p->pid);
        w_copy_text(pid);
        return;
    }
    DWORD pid = p->pid;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE |
                                 (button == end_task ? PROCESS_TERMINATE : 0), FALSE, pid);
    FILETIME start, stop, kernel, user;
    const wchar_t *error = NULL;
    if (!process || !GetProcessTimes(process, &start, &stop, &kernel, &user) ||
        !p->created || p->created != w_timestamp(start) || WaitForSingleObject(process,0) != WAIT_TIMEOUT)
        error = L"The process exited or Windows denied access.";
    else if (button == copy_path) {
        wchar_t path[32768];
        DWORD size = count_of(path);
        if (QueryFullProcessImageNameW(process, 0, path, &size)) w_copy_text(path);
        else error = L"Windows could not read the executable path.";
    } else {
        DWORD session, this_session, shell_pid = 0;
        BOOL critical = FALSE;
        wchar_t path[32768], self[32768], question[384];
        DWORD size = count_of(path);
        GetWindowThreadProcessId(GetShellWindow(), &shell_pid);
        if (!ProcessIdToSessionId(pid,&session) || !ProcessIdToSessionId(GetCurrentProcessId(),&this_session) ||
            session != this_session || pid == shell_pid || !IsProcessCritical(process,&critical) || critical ||
            !QueryFullProcessImageNameW(process,0,path,&size) ||
            _wcsicmp(PathFindFileNameW(path),p->name) || !GetModuleFileNameW(NULL,self,count_of(self)) ||
            !_wcsicmp(path,self)) {
            error = L"This system, desktop, or session process cannot be closed here.";
        } else if (button == close_app) {
            w_main_window target = {.pid = pid};
            EnumWindows(w_find_window, (LPARAM)&target);
            if (!target.window) error = L"This process has no visible app window.";
            else {
                swprintf(question,count_of(question),L"Ask %ls (PID %lu) to close its window?",p->name,pid);
                if (MessageBoxW(panel,question,L"Close window",MB_YESNO|MB_DEFBUTTON2|MB_ICONQUESTION) == IDYES) {
                    DWORD owner_pid = 0;
                    GetWindowThreadProcessId(target.window,&owner_pid);
                    if (owner_pid != pid || !PostMessageW(target.window, WM_CLOSE, 0, 0)) error = L"The window is no longer available.";
                    else SetWindowTextW(feedback,L"Close request sent.");
                }
            }
        } else {
            swprintf(question,count_of(question),L"End %ls (PID %lu) immediately? Unsaved work may be lost.",p->name,pid);
            if (MessageBoxW(panel,question,L"End process",MB_YESNO|MB_DEFBUTTON2|MB_ICONWARNING) == IDYES) {
                if (!TerminateProcess(process,1)) error = L"Windows could not end this process.";
                else SetWindowTextW(feedback,L"Process ended.");
            }
        }
    }
    if (error) SetWindowTextW(feedback, error);
    if (process) CloseHandle(process);
}

static void w_column(int column) {
    if (column < 0 || column >= (int)count_of(columns)) return;
    descending = column == sorted_by ? !descending : column >= 2;
    sorted_by = column;
    w_rebuild(w_selected());
    HWND header = ListView_GetHeader(table);
    for (int i = 0; i < (int)count_of(columns); i++) {
        HDITEMW item = {.mask = HDI_FORMAT};
        if (Header_GetItem(header,i,&item)) {
            item.fmt &= ~(HDF_SORTUP|HDF_SORTDOWN);
            if (i == column) item.fmt |= descending ? HDF_SORTDOWN : HDF_SORTUP;
            Header_SetItem(header,i,&item);
        }
    }
    InvalidateRect(header, NULL, FALSE);
}

static void w_cell_text(NMLVDISPINFOW *info) {
    LVITEMW *item = &info->item;
    if (!(item->mask & LVIF_TEXT) || !item->pszText || item->cchTextMax < 1 ||
        item->iItem < 0 || item->iItem >= visible_count) return;
    w_process *p = visible[item->iItem];
    wchar_t text[64];
    const wchar_t *value = text;
    switch (item->iSubItem) {
    case 0: value = p->name; break;
    case 1: swprintf(text, count_of(text), L"%lu", p->pid); break;
    case 2:
        w_cpu_format(text, count_of(text), p->cpu);
        break;
    case 3:
        if (p->has_memory) swprintf(text, count_of(text), L"%.1f MiB", p->memory/1048576.0);
        else value = L"—";
        break;
    default: value = L"";
    }
    lstrcpynW(item->pszText, value, item->cchTextMax);
}

static LRESULT CALLBACK w_table_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp,
                                    UINT_PTR id, DWORD_PTR data) {
    (void)id; (void)data;
    if (msg == WM_NOTIFY && ((NMHDR *)lp)->hwndFrom == ListView_GetHeader(w) && ((NMHDR *)lp)->code == NM_CUSTOMDRAW) {
        NMCUSTOMDRAW *draw = (NMCUSTOMDRAW *)lp;
        if (draw->dwDrawStage == CDDS_PREPAINT) {
            FillRect(draw->hdc, &draw->rc, w_theme_brush());
            return CDRF_NOTIFYITEMDRAW;
        }
        if (draw->dwDrawStage == CDDS_ITEMPREPAINT && draw->dwItemSpec < count_of(columns)) {
            int col = (int)draw->dwItemSpec;
            wchar_t label[64];
            swprintf(label, count_of(label), L"%ls%ls", columns[col],
                     col != sorted_by ? L"" : descending ? L"  ↓" : L"  ↑");
            RECT r = draw->rc;
            FillRect(draw->hdc, &r, w_theme_brush());
            SetBkMode(draw->hdc, TRANSPARENT);
            SetTextColor(draw->hdc, w_theme_foreground());
            InflateRect(&r, -MulDiv(8, GetDpiForWindow(w), 96), 0);
            DrawTextW(draw->hdc, label, -1, &r, DT_SINGLELINE | DT_VCENTER | (col ? DT_RIGHT : DT_LEFT));
            return CDRF_SKIPDEFAULT;
        }
    }
    return DefSubclassProc(w,msg,wp,lp);
}

static void w_layout(bool resize_font) {
    if (!end_task) return;
    UINT dpi = GetDpiForWindow(panel);
    if (resize_font) {
        HFONT next = CreateFontW(-MulDiv(15, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HWND controls[] = {summary, search_label, search, pause_button, table, ListView_GetHeader(table), copy_pid,
                           copy_path, close_app, end_task, feedback, empty};
        for (size_t i = 0; i < count_of(controls); ++i)
            SendMessageW(controls[i], WM_SETFONT, (WPARAM)next, TRUE);
        DeleteObject(font);
        font = next;
    }
    RECT client;
    GetClientRect(panel, &client);
    int pad = MulDiv(14,dpi,96), gap = MulDiv(8,dpi,96), line = MulDiv(30,dpi,96);
    int width = max(1,(int)client.right - 2*pad);
    int y = pad;
    MoveWindow(summary, pad, y, width, line, TRUE);
    y += line + gap;
    int label_width = MulDiv(93,dpi,96);
    MoveWindow(search_label, pad, y, label_width, line, TRUE);
    MoveWindow(search, pad+label_width, y, max(1,width-line-label_width-gap), line, TRUE);
    MoveWindow(pause_button, pad+width-line, y, line, line, TRUE);
    y += line + gap;
    int bottom = client.bottom - pad - line;
    MoveWindow(table, pad, y, width, max(1,bottom-line-gap-y), TRUE);
    MoveWindow(empty, 2*pad, y+2*line, max(1,width-2*pad), line, TRUE);
    MoveWindow(feedback, pad, bottom-line, width, line, TRUE);
    MoveWindow(copy_pid, pad, bottom, line, line, TRUE);
    MoveWindow(copy_path, pad+line+gap, bottom, line, line, TRUE);
    int action = min(MulDiv(124,dpi,96), max(1,(width-2*line-3*gap)/2));
    MoveWindow(close_app, pad+width-2*action-gap, bottom, action, line, TRUE);
    MoveWindow(end_task, pad+width-action, bottom, action, line, TRUE);
    RECT box;
    GetClientRect(table,&box);
    // The first asynchronous sample can introduce a vertical scrollbar after the layout.
    if (!(GetWindowLongW(table,GWL_STYLE) & WS_VSCROLL))
        box.right -= GetSystemMetricsForDpi(SM_CXVSCROLL,dpi);
    int numeric = min((int)box.right/5, MulDiv(110,dpi,96));
    for (int i = 1; i < 4; i++) ListView_SetColumnWidth(table,i,numeric);
    ListView_SetColumnWidth(table,0,max(1,(int)box.right-3*numeric));
}

static LRESULT CALLBACK w_activity_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    LRESULT result;
    if (w_theme_message(w,msg,wp,lp,&result)) return result;
    switch (msg) {
    case WM_DPICHANGED: {
        w_tool_resize_for_dpi(w,(RECT *)lp);
        w_layout(true);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        UINT dpi = GetDpiForWindow(w);
        MINMAXINFO *limits = (MINMAXINFO *)lp;
        w_tool_minimum(w,limits,MulDiv(445,dpi,96),MulDiv(325,dpi,96));
        return 0;
    }
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) w_layout(false);
        return 0;
    case WM_TIMER:
        if (!paused && !IsIconic(w)) w_request_sample();
        return 0;
    case sample_ready:
        if (worker) w_apply_sample(wp != 0);
        return 0;
    case WM_COMMAND: {
        HWND from = (HWND)lp;
        if (from == search && HIWORD(wp) == EN_CHANGE) w_rebuild(w_selected());
        else if (from == pause_button && HIWORD(wp) == BN_CLICKED) w_toggle_pause();
        else if (from && HIWORD(wp) == BN_CLICKED &&
                 (from == copy_pid || from == copy_path || from == close_app || from == end_task)) w_process_action(from);
        return 0;
    }
    case WM_NOTIFY: {
        NMHDR *header = (NMHDR *)lp;
        if (header->hwndFrom != table) break;
        if (header->code == LVN_GETDISPINFOW) w_cell_text((NMLVDISPINFOW *)lp);
        else if (header->code == LVN_COLUMNCLICK) w_column(((NMLISTVIEW *)lp)->iSubItem);
        else if (header->code == LVN_ITEMCHANGED || header->code == LVN_ODSTATECHANGED) w_actions();
        return 0;
    }
    case WM_DESTROY:
        KillTimer(w,1);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w,msg,wp,lp);
}

static HWND w_control(const wchar_t *kind, const wchar_t *label, DWORD style, int id) {
    return CreateWindowExW(0,kind,label,WS_CHILD|WS_VISIBLE|style,0,0,1,1,panel,(HMENU)(INT_PTR)id,GetModuleHandleW(NULL),NULL);
}

int w_activity_run(void) {
    INITCOMMONCONTROLSEX common = {sizeof(common), ICC_LISTVIEW_CLASSES};
    if (!InitCommonControlsEx(&common)) return 1;
    HINSTANCE instance = GetModuleHandleW(NULL);
    WNDCLASSW wc = {.lpfnWndProc = w_activity_proc, .hInstance = instance, .lpszClassName = L"WinwmActivity",
                    .hCursor = LoadCursorW(NULL,IDC_ARROW)};
    RegisterClassW(&wc);
    MONITORINFO monitor = {.cbSize = sizeof(monitor)};
    UINT dpi = w_tool_work_area(&monitor);
    RECT area = monitor.rcWork;
    int width = min(MulDiv(750,dpi,96),area.right-area.left);
    int height = min(MulDiv(550,dpi,96),area.bottom-area.top);
    panel = CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_CONTROLPARENT,wc.lpszClassName,L"Activity monitor",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,
                            (area.left+area.right-width)/2,(area.top+area.bottom-height)/2,width,height,NULL,NULL,instance,NULL);
    if (!panel) return 1;
    summary = w_control(L"STATIC",L"Reading system resources…",SS_CENTERIMAGE,100);
    search_label = w_control(L"STATIC",L"&Name or PID",SS_CENTERIMAGE,110);
    search = w_control(L"EDIT",L"",WS_BORDER|ES_AUTOHSCROLL|WS_TABSTOP,101);
    SendMessageW(search,EM_SETCUEBANNER,TRUE,(LPARAM)L"Find a process by name or PID");
    SendMessageW(search,EM_SETLIMITTEXT,255,0);
    DWORD button = WS_TABSTOP|BS_OWNERDRAW;
    pause_button = w_control(L"BUTTON",L"Pause",button,102);
    table = w_control(WC_LISTVIEWW,L"Processes",LVS_REPORT|LVS_OWNERDATA|LVS_SINGLESEL|LVS_SHOWSELALWAYS|WS_TABSTOP,103);
    empty = w_control(L"STATIC",L"Reading processes…",SS_CENTER,104);
    feedback = w_control(L"STATIC",L"",SS_CENTERIMAGE|SS_ENDELLIPSIS,105);
    copy_pid = w_control(L"BUTTON",L"Copy PID",button,106);
    copy_path = w_control(L"BUTTON",L"Copy path",button,107);
    close_app = w_control(L"BUTTON",L"Close window…",button,108);
    end_task = w_control(L"BUTTON",L"End process…",button,109);
    if (!summary || !search_label || !search || !pause_button || !table || !empty || !feedback ||
        !copy_pid || !copy_path || !close_app || !end_task) {
        DestroyWindow(panel);
        return 1;
    }
    w_icon_button(pause_button,L"\uE769",L"Pause process updates");
    w_icon_button(copy_pid,L"\uE8C8",L"Copy process ID");
    w_icon_button(copy_path,L"\uE71B",L"Copy executable path");
    ListView_SetExtendedListViewStyle(table,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER);
    ListView_SetBkColor(table,w_theme_background());
    ListView_SetTextBkColor(table,w_theme_background());
    ListView_SetTextColor(table,w_theme_foreground());
    SetWindowSubclass(table,w_table_proc,1,0);
    for (int i=0; i < (int)count_of(columns); i++) {
        LVCOLUMNW column = {.mask = LVCF_TEXT|LVCF_FMT|LVCF_WIDTH, .pszText = (wchar_t *)columns[i],
                            .fmt = i ? LVCFMT_RIGHT : LVCFMT_LEFT, .cx = 80};
        ListView_InsertColumn(table,i,&column);
    }
    w_actions();
    w_layout(true);
    if (w_tool_show(panel)) {
        SetFocus(search);
        SetTimer(panel,1,2000,NULL);
        w_request_sample();
    }
    MSG msg;
    while (GetMessageW(&msg,NULL,0,0) > 0) {
        if (msg.message == WM_KEYDOWN) {
            bool ctrl = GetKeyState(VK_CONTROL) < 0;
            if (msg.wParam == VK_ESCAPE) { DestroyWindow(panel); continue; }
            if (msg.wParam == VK_F5) { w_request_sample(); continue; }
            if (ctrl && msg.wParam == 'F') { SetFocus(search); SendMessageW(search,EM_SETSEL,0,-1); continue; }
            if (msg.hwnd == table && ctrl && msg.wParam == 'C') { w_process_action(copy_pid); continue; }
            if (msg.hwnd == search && msg.wParam == VK_DOWN && visible_count) {
                SetFocus(table);
                if (!w_selected()) ListView_SetItemState(table,0,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
                continue;
            }
        }
        if (msg.message == WM_SYSKEYDOWN && msg.wParam >= '1' && msg.wParam <= '4') {
            w_column((int)msg.wParam-'1');
            continue;
        }
        if (!IsDialogMessageW(panel,&msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    if (worker) {
        WaitForSingleObject(worker,INFINITE);
        CloseHandle(worker);
    }
    free(incoming.rows);
    free(current.rows);
    free(visible);
    DeleteObject(font);
    return 0;
}
