#include "theme/dialog.h"
#include "desktop_tools/tools.h"
#include "theme/theme.h"
#include "theme/icon_button.h"
#include "theme/hover.h"
#include "win_platform/cpu.h"
#include <commctrl.h>
#include <psapi.h>
#include <shlwapi.h>
#include <stdio.h>
#include <tlhelp32.h>
#include <windowsx.h>

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
enum { sample_ready = WM_APP + 1, fit_columns = WM_APP + 2 };
static const wchar_t *columns[] = {L"Process", L"PID", L"CPU", L"Memory"};
static const wchar_t search_hint[]=L"Find a process";
static struct { int width; UINT dpi; } user_widths[count_of(columns)];
static bool sizing_columns;
static int text_height, label_width, query_width, column_text[count_of(columns)], summary_height;
static void w_layout(bool resize_font);

static void w_feedback(const wchar_t *text) {
    wchar_t prior[256];
    GetWindowTextW(feedback,prior,count_of(prior));
    if (!wcscmp(text,prior)) return;
    SetWindowTextW(feedback,text);
    w_layout(false);
}

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
    if (!worker) w_feedback(L"Could not start sampling. Press F5 to retry.");
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

static void w_table_colors(void) {
    ListView_SetBkColor(table,w_theme_background());
    ListView_SetTextBkColor(table,w_theme_background());
    ListView_SetTextColor(table,w_theme_foreground());
}

static void w_rebuild(w_process *selection,w_process **previous,int previous_count) {
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
    int old_index = ListView_GetNextItem(table,-1,LVNI_SELECTED);
    if (old_index >= 0 && old_index != selected_index)
        ListView_SetItemState(table, old_index, 0, LVIS_SELECTED | LVIS_FOCUSED);
    if (ListView_GetItemCount(table) != visible_count) {
        RECT before,after;
        GetClientRect(table,&before);
        ListView_SetItemCountEx(table, visible_count, LVSICF_NOSCROLL | LVSICF_NOINVALIDATEALL);
        GetClientRect(table,&after);
        if (after.right!=before.right) PostMessageW(panel,fit_columns,0,0);
    }
    if (selected_index >= 0 && selected_index != old_index) ListView_SetItemState(table, selected_index, LVIS_SELECTED|LVIS_FOCUSED, LVIS_SELECTED|LVIS_FOCUSED);
    if (!previous || previous_count != visible_count) {
        InvalidateRect(table,NULL,FALSE);
    } else {
        int first = ListView_GetTopIndex(table);
        int end = min(visible_count, first+ListView_GetCountPerPage(table)+1);
        for (int i=first; i<end; ++i) {
            const w_process *before = previous[i], *after = visible[i];
            if (before->pid != after->pid || before->created != after->created ||
                before->cpu != after->cpu || before->memory != after->memory ||
                before->has_memory != after->has_memory || wcscmp(before->name,after->name))
                ListView_RedrawItems(table,i,i);
        }
    }
    if (!visible_count)
        w_tool_status(empty,current.count ? L"No processes match this search." : L"Reading processes…");
    if (!!(GetWindowLongW(empty,GWL_STYLE)&WS_VISIBLE) == !!visible_count)
        ShowWindow(empty,visible_count ? SW_HIDE : SW_SHOWNOACTIVATE);
    w_actions();
}

static void w_apply_sample(bool ok) {
    WaitForSingleObject(worker, INFINITE);
    CloseHandle(worker);
    worker = NULL;
    w_process *selected = w_selected();
    w_process **next = ok && !discard_sample ? malloc((size_t)incoming.count * sizeof(*visible)) : NULL;
    if (next) {
        w_process **previous = visible;
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
        swprintf(text, count_of(text), L"CPU  %ls    Memory  %.1f / %.1f GiB    Processes  %d", cpu,
                 (incoming.memory.ullTotalPhys-incoming.memory.ullAvailPhys)/1073741824.0,
                 incoming.memory.ullTotalPhys/1073741824.0, incoming.count);
        wchar_t shown[256];
        GetWindowTextW(summary, shown, count_of(shown));
        if (wcscmp(shown,text)) {
            SetWindowTextW(summary,text);
            RECT area;
            GetClientRect(summary,&area);
            if (w_theme_wrapped_height(font,text,area.right)!=summary_height) w_layout(false);
        }
        w_process *old_rows = current.rows;
        current = incoming;
        incoming.rows = NULL;
        w_rebuild(selected,previous,visible_count);
        free(previous);
        free(old_rows);
    } else if (!discard_sample) {
        w_feedback(L"Process list unavailable. Press F5 to retry.");
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
    w_feedback(copy || !buffer ? L"Clipboard unavailable." : L"Copied to clipboard.");
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
                if (w_dialog(panel,question,L"Close window",MB_YESNO|MB_DEFBUTTON2|MB_ICONQUESTION) == IDYES) {
                    DWORD owner_pid = 0;
                    GetWindowThreadProcessId(target.window,&owner_pid);
                    if (owner_pid != pid || !PostMessageW(target.window, WM_CLOSE, 0, 0)) error = L"The window is no longer available.";
                    else w_feedback(L"Close request sent.");
                }
            }
        } else {
            swprintf(question,count_of(question),L"End %ls (PID %lu) immediately? Unsaved work may be lost.",p->name,pid);
            if (w_dialog(panel,question,L"End process",MB_YESNO|MB_DEFBUTTON2|MB_ICONWARNING) == IDYES) {
                if (!TerminateProcess(process,1)) error = L"Windows could not end this process.";
                else w_feedback(L"Process ended.");
            }
        }
    }
    if (error) w_feedback(error);
    if (process) CloseHandle(process);
}

static void w_column(int column) {
    if (column < 0 || column >= (int)count_of(columns)) return;
    descending = column == sorted_by ? !descending : column >= 2;
    sorted_by = column;
    w_rebuild(w_selected(),NULL,visible_count);
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

static LRESULT w_draw_process(NMLVCUSTOMDRAW *draw) {
    if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
    if (draw->nmcd.dwDrawStage!=CDDS_ITEMPREPAINT || draw->nmcd.dwItemSpec>=(DWORD_PTR)visible_count)
        return CDRF_DODEFAULT;
    int index=(int)draw->nmcd.dwItemSpec;
    RECT row;
    if (!ListView_GetItemRect(table,index,&row,LVIR_BOUNDS)) return CDRF_DODEFAULT;
    HDC dc=draw->nmcd.hdc;
    int saved=SaveDC(dc);
    if (!saved) return CDRF_DODEFAULT;
    if (font) SelectObject(dc,font);
    UINT state=ListView_GetItemState(table,index,LVIS_SELECTED|LVIS_FOCUSED);
    bool selected=(state&LVIS_SELECTED)!=0;
    int amount=w_hover_amount(table,index);
    UINT dpi=GetDpiForWindow(table);
    COLORREF ink=w_hover_row(dc,&row,selected,amount,false);
    int mark_width=max(1,MulDiv(3,dpi,96));
    if (selected) {
        RECT mark=row;
        mark.left=max(0,mark.left);
        mark.right=min(row.right,mark.left+mark_width);
        SetDCBrushColor(dc,w_theme_high_contrast() ? w_theme_selection(true) : w_theme_accent());
        FillRect(dc,&mark,GetStockObject(DC_BRUSH));
    }
    SetBkMode(dc,TRANSPARENT);
    SetTextColor(dc,ink);
    wchar_t text[MAX_PATH];
    NMLVDISPINFOW info={.item={.mask=LVIF_TEXT,.iItem=index,.pszText=text,.cchTextMax=count_of(text)}};
    for (int col=0;col<(int)count_of(columns);++col) {
        RECT cell;
        if (!ListView_GetSubItemRect(table,index,col,LVIR_LABEL,&cell) || !RectVisible(dc,&cell)) continue;
        info.item.iSubItem=col;
        w_cell_text(&info);
        InflateRect(&cell,-w_theme_spacing(dpi),0);
        if (!col) cell.left+=mark_width;
        w_theme_draw_fitted_line(dc,text,-1,&cell,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX|(col ? DT_RIGHT : DT_LEFT));
    }
    if ((state&LVIS_FOCUSED) && GetFocus()==table && !(SendMessageW(table,WM_QUERYUISTATE,0,0)&UISF_HIDEFOCUS))
        DrawFocusRect(dc,&row);
    RestoreDC(dc,saved);
    return CDRF_SKIPDEFAULT;
}

static LRESULT CALLBACK w_header_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
    (void)id; (void)data;
    LRESULT result = DefSubclassProc(w,msg,wp,lp);
    if (msg == HDM_LAYOUT && result) {
        HDLAYOUT *layout = (HDLAYOUT *)lp;
        int height = w_theme_button_size(GetDpiForWindow(w));
        layout->prc->top += height-layout->pwpos->cy;
        layout->pwpos->cy = height;
    }
    return result;
}

static LRESULT CALLBACK w_table_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp,
                                    UINT_PTR id, DWORD_PTR data) {
    (void)id; (void)data;
    if (msg == WM_NOTIFY && ((NMHDR *)lp)->hwndFrom == ListView_GetHeader(w)) {
        NMHDR *note = (NMHDR *)lp;
        if (note->code==HDN_ENDTRACKW || note->code==HDN_ENDTRACKA || note->code==HDN_DIVIDERDBLCLICKW ||
            note->code==HDN_DIVIDERDBLCLICKA) PostMessageW(panel,fit_columns,0,0);
        if (!sizing_columns && (note->code == HDN_ITEMCHANGEDW || note->code == HDN_ITEMCHANGEDA)) {
            NMHEADERW *change = (NMHEADERW *)lp;
            if ((unsigned)change->iItem < count_of(columns) && change->pitem && (change->pitem->mask & HDI_WIDTH)) {
                user_widths[change->iItem].width = change->pitem->cxy;
                user_widths[change->iItem].dpi = GetDpiForWindow(w);
            }
        }
        if (note->code != NM_CUSTOMDRAW) return DefSubclassProc(w,msg,wp,lp);
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
            bool pressed = (draw->uItemState & CDIS_SELECTED) != 0;
            int amount=w_hover_amount(note->hwndFrom,col);
            COLORREF ink=w_hover_row(draw->hdc,&r,false,amount,pressed);
            if (pressed) {
                RECT line = r;
                line.top = max(line.top,line.bottom-max(1,MulDiv(2,GetDpiForWindow(w),96)));
                SetDCBrushColor(draw->hdc,ink);
                FillRect(draw->hdc,&line,GetStockObject(DC_BRUSH));
            }
            if ((draw->uItemState & CDIS_FOCUS) && !(SendMessageW(note->hwndFrom,WM_QUERYUISTATE,0,0)&UISF_HIDEFOCUS))
                DrawFocusRect(draw->hdc,&r);
            SetBkMode(draw->hdc, TRANSPARENT);
            SetTextColor(draw->hdc, ink);
            InflateRect(&r, -w_theme_spacing(GetDpiForWindow(w)), 0);
            w_theme_draw_control_line(note->hwndFrom,draw->hdc,label,&r,DT_VCENTER|DT_END_ELLIPSIS|(col ? DT_RIGHT : DT_LEFT));
            return CDRF_SKIPDEFAULT;
        }
    }
    return DefSubclassProc(w,msg,wp,lp);
}

static void w_fit_columns(void) {
    if (!font || sizing_columns) return;
    UINT dpi=GetDpiForWindow(table);
    int gap=w_theme_spacing(dpi);
    RECT client;
    GetClientRect(table,&client);
    int remaining=client.right;
    sizing_columns=true;
    for (int col=(int)count_of(columns)-1;col>=0;--col) {
        int width=column_text[col]+2*gap;
        if (!col) width=max(remaining,width+MulDiv(3,dpi,96));
        if (user_widths[col].dpi) width=MulDiv(user_widths[col].width,dpi,user_widths[col].dpi);
        if (width!=ListView_GetColumnWidth(table,col)) ListView_SetColumnWidth(table,col,width);
        remaining-=width;
    }
    sizing_columns=false;
}

static void w_measure_activity(void) {
    text_height=w_theme_line_height(font);
    wchar_t label[64];
    GetWindowTextW(search_label,label,count_of(label));
    label_width=w_theme_text_width(font,label,0);
    query_width=w_theme_text_width(font,search_hint,DT_NOPREFIX);
    MEMORYSTATUSEX memory={.dwLength=sizeof(memory)};
    wchar_t memory_text[48]=L"999999.9 MiB";
    if (GlobalMemoryStatusEx(&memory)) swprintf(memory_text,count_of(memory_text),L"%.1f MiB",memory.ullTotalPhys/1048576.0);
    const wchar_t *values[]={L"explorer.exe",L"4294967295",L"100.0%",memory_text};
    for (size_t col=0;col<count_of(columns);++col) {
        swprintf(label,count_of(label),L"%ls  ↓",columns[col]);
        int heading=w_theme_text_width(font,label,DT_NOPREFIX);
        int value=w_theme_text_width(font,values[col],DT_NOPREFIX);
        column_text[col]=max(heading,value);
    }
}

static void w_layout(bool resize_font) {
    if (!end_task) return;
    UINT dpi = GetDpiForWindow(panel);
    HFONT next = resize_font ? w_theme_message_font(dpi) : NULL;
    if (next) {
        HWND controls[] = {summary, search_label, search, pause_button, table, ListView_GetHeader(table), copy_pid,
                           copy_path, close_app, end_task, feedback, empty};
        for (size_t i = 0; i < count_of(controls); ++i)
            SendMessageW(controls[i], WM_SETFONT, (WPARAM)next, FALSE);
        DeleteObject(font);
        font = next;
        w_measure_activity();
    }
    RECT client;
    w_theme_content_rect(panel, &client);
    int gap = w_theme_spacing(dpi), pad = 2*gap, line = w_theme_button_size(dpi);
    int width = max(1,(int)client.right - 2*pad);
    wchar_t text[256];
    GetWindowTextW(summary,text,count_of(text));
    int summary_text=w_theme_wrapped_height(font,text,width);
    summary_height=max(text_height,summary_text);
    w_tool_place(summary,pad,pad,width,summary_height);
    int y=pad+summary_height+gap;
    int label=min(label_width,max(0,width-2*line-2*gap));
    w_tool_place(search_label,pad,y,label,line);
    w_tool_place(search,pad+label+gap,y,max(1,width-line-label-2*gap),line);
    w_tool_place(pause_button, pad+width-line, y, line, line);
    y += line + gap;
    int bottom = client.bottom - pad - line;
    int cluster=2*line+gap;
    int middle=max(1,width-2*(cluster+gap));
    GetWindowTextW(feedback,text,count_of(text));
    int notice=w_theme_wrapped_height(font,text,middle);
    bool own_row=notice>line;
    if (own_row) notice=w_theme_wrapped_height(font,text,width);
    w_tool_visible(feedback,*text!=0);
    if (*text) {
        if (own_row) w_tool_place(feedback,pad,bottom-gap-notice,width,notice);
        else w_tool_place(feedback,pad+cluster+gap,bottom+(line-notice)/2,middle,notice);
    }
    int table_height=max(1,bottom-gap-(own_row ? notice+gap : 0)-y);
    int row_height=text_height+2*min(gap,max(1,(text_height+3)/6));
    w_native_list_row_height(table,row_height);
    w_tool_place(table, pad, y, width, table_height);
    // User-resized columns can add a scrollbar beneath the empty message.
    int body=max(1,table_height-line-GetSystemMetricsForDpi(SM_CYHSCROLL,dpi));
    int message_height=min(body,max(1,text_height));
    w_tool_place(empty,pad+gap,y+line+min(gap,(body-message_height)/2),max(1,width-2*gap),message_height);
    w_tool_place(copy_pid, pad, bottom, line, line);
    w_tool_place(copy_path, pad+line+gap, bottom, line, line);
    w_tool_place(close_app, pad+width-2*line-gap, bottom, line, line);
    w_tool_place(end_task, pad+width-line, bottom, line, line);
    w_fit_columns();
}

static LRESULT CALLBACK w_activity_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    LRESULT result;
    if (w_theme_message(w,msg,wp,lp,&result)) return result;
    if (w_tool_font_change(w,msg,wp,lp)) {
        w_layout(true);
        return 0;
    }
    switch (msg) {
    case fit_columns:
        w_fit_columns();
        return 0;
    case winwm_theme_updated:
    case WM_SYSCOLORCHANGE:
    case WM_THEMECHANGED:
        if (table) w_table_colors();
        break;
    case WM_GETMINMAXINFO: {
        UINT dpi = GetDpiForWindow(w);
        MINMAXINFO *limits = (MINMAXINFO *)lp;
        int side=w_theme_button_size(dpi),gap=w_theme_spacing(dpi),pad=2*gap;
        int text=text_height ? text_height : w_theme_role_line_height(dpi,text_body);
        int width=max(4*side+3*gap,label_width+query_width+side+4*gap);
        int report=GetSystemMetricsForDpi(SM_CXVSCROLL,dpi)+MulDiv(3,dpi,96);
        for (size_t col=0;col<count_of(columns);++col) report+=column_text[col]+2*gap;
        width=max(width,report);
        // Keep at least one readable process row, even with a horizontal
        // scrollbar and a two-line action result at the smallest window size.
        int row=text+2*min(gap,max(1,(text+3)/6));
        int height=2*pad+4*gap+3*side+row+4*text+GetSystemMetricsForDpi(SM_CYHSCROLL,dpi);
        w_tool_minimum(w,limits,width+2*pad,height);
        return 0;
    }
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) w_layout(false);
        return 0;
    case WM_TIMER:
        if (!paused && IsWindowVisible(w) && !IsIconic(w)) w_request_sample();
        return 0;
    case sample_ready:
        if (worker) w_apply_sample(wp != 0);
        return 0;
    case WM_COMMAND: {
        HWND from = (HWND)lp;
        if (from == search && HIWORD(wp) == EN_CHANGE) w_rebuild(w_selected(),NULL,visible_count);
        else if (from == pause_button && HIWORD(wp) == BN_CLICKED) w_toggle_pause();
        else if (from && HIWORD(wp) == BN_CLICKED &&
                 (from == copy_pid || from == copy_path || from == close_app || from == end_task)) w_process_action(from);
        return 0;
    }
    case WM_NOTIFY: {
        NMHDR *header = (NMHDR *)lp;
        if (header->hwndFrom != table) break;
        if (header->code == NM_CUSTOMDRAW) return w_draw_process((NMLVCUSTOMDRAW *)lp);
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
    summary = w_control(L"STATIC",L"Reading system resources…",SS_EDITCONTROL|SS_NOPREFIX,100);
    search_label = w_control(L"STATIC",L"&Name or PID",SS_CENTERIMAGE,110);
    search = w_native_field(panel,101,WS_VISIBLE);
    SendMessageW(search,EM_SETCUEBANNER,TRUE,(LPARAM)search_hint);
    SendMessageW(search,EM_SETLIMITTEXT,255,0);
    DWORD button = WS_TABSTOP|BS_OWNERDRAW;
    pause_button = w_control(L"BUTTON",L"Pause",button,102);
    table = w_control(WC_LISTVIEWW,L"Processes",LVS_REPORT|LVS_OWNERDATA|LVS_SINGLESEL|LVS_SHOWSELALWAYS|WS_TABSTOP,103);
    empty = w_control(L"STATIC",L"Reading processes…",SS_CENTER|SS_CENTERIMAGE,104);
    feedback = w_control(L"STATIC",L"",SS_EDITCONTROL|SS_NOPREFIX,105);
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
    w_icon_button(close_app,L"\uE8BB",L"Close the selected application window…");
    w_icon_button(end_task,L"\uE71A",L"End the selected process…");
    if (SendMessageW(table,CCM_GETVERSION,0,0)<5) SendMessageW(table,CCM_SETVERSION,5,0);
    ListView_SetExtendedListViewStyle(table,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER);
    w_table_colors();
    SetWindowPos(empty,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    SetWindowSubclass(table,w_table_proc,1,0);
    SetWindowSubclass(ListView_GetHeader(table),w_header_proc,1,0);
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
        if (w_native_ime_key(&msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); continue; }
        if (w_tool_repeated_dialog_key(&msg)) continue;
        if (msg.message == WM_KEYDOWN) {
            bool ctrl = GetKeyState(VK_CONTROL) < 0;
            if (msg.wParam == VK_ESCAPE) { SendMessageW(panel,WM_CLOSE,0,0); continue; }
            if (msg.wParam == VK_F5) { w_request_sample(); continue; }
            if (ctrl && msg.wParam == 'F') { SetFocus(search); SendMessageW(search,EM_SETSEL,0,-1); continue; }
            if (msg.hwnd == table && ctrl && msg.wParam == 'C') { w_process_action(copy_pid); continue; }
            if (msg.hwnd == search && (msg.wParam == VK_DOWN || msg.wParam == VK_RETURN) && visible_count) {
                SetFocus(table);
                int row=ListView_GetNextItem(table,-1,LVNI_SELECTED);
                if (row<0) {
                    row=0;
                    ListView_SetItemState(table,row,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
                }
                ListView_EnsureVisible(table,row,FALSE);
                continue;
            }
        }
        if (msg.message == WM_SYSKEYDOWN && msg.wParam >= '1' && msg.wParam <= '4') {
            if (!(HIWORD(msg.lParam)&KF_REPEAT)) w_column((int)msg.wParam-'1');
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
