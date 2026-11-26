#define COBJMACROS
#include "desktop_tools/tools.h"
#include "theme/theme.h"
#include <uiautomationclient.h>
#include <windowsx.h>

// On a shell-less desktop the native XAML AppList can have zero height even
// after its HWND is resized. Present its accessible choices beside Windows'
// own confirmation buttons. The broker keeps foreground and file ownership.
static HWND picker, panel, list, heading;
static DWORD picker_pid;
static HANDLE wake;
static volatile LONG finished, active_view;
static volatile LONG64 requested_choice;
static LONG view_serial;
static bool populated;
enum { provider_changed = WM_APP + 1 };
typedef struct {
    BSTR title, *names;
    int count;
} w_choice_labels;
static SRWLOCK view_lock = SRWLOCK_INIT;
static struct w_changes {
    w_choice_labels labels;
    LONG labels_view, selection_view;
    int selected;
} changes;

// These interfaces stay in the worker's MTA. None of their calls run in a
// window procedure or share the picker's input queue.
static IUIAutomation *uia;
static IUIAutomationCondition *app_list_id, *heading_id, *row_types;
static IUIAutomationElementArray *choices;
static IUIAutomationSelectionPattern *selection;
static IUIAutomationElement *last_selection;
static HFONT font;
static UINT font_dpi;

static bool w_picker_alive(void) {
    DWORD pid = 0;
    return GetWindowThreadProcessId(picker, &pid) && pid == picker_pid && IsWindowVisible(picker);
}

static HRESULT w_find_condition(PROPERTYID property, VARTYPE type, LONG number, const wchar_t *text,
                                IUIAutomationCondition **out) {
    VARIANT value = {.vt=type};
    if (type == VT_BSTR) {
        value.bstrVal = SysAllocString(text);
        if (!value.bstrVal) return E_OUTOFMEMORY;
    } else value.lVal = number;
    HRESULT hr = IUIAutomation_CreatePropertyCondition(uia, property, value, out);
    VariantClear(&value);
    return hr;
}

static HRESULT w_conditions(void) {
    IUIAutomationCondition *item = NULL, *link = NULL;
    HRESULT hr = w_find_condition(UIA_AutomationIdPropertyId, VT_BSTR, 0, L"AppList", &app_list_id);
    if (SUCCEEDED(hr)) hr = w_find_condition(UIA_AutomationIdPropertyId, VT_BSTR, 0, L"HeaderTextBlock", &heading_id);
    if (SUCCEEDED(hr)) hr = w_find_condition(UIA_ControlTypePropertyId, VT_I4, UIA_ListItemControlTypeId, NULL, &item);
    if (SUCCEEDED(hr)) hr = w_find_condition(UIA_ControlTypePropertyId, VT_I4, UIA_HyperlinkControlTypeId, NULL, &link);
    if (SUCCEEDED(hr)) hr = IUIAutomation_CreateOrCondition(uia, item, link, &row_types);
    if (link) IUIAutomationCondition_Release(link);
    if (item) IUIAutomationCondition_Release(item);
    return hr;
}

static void w_clear(void) {
    if (selection) IUIAutomationSelectionPattern_Release(selection);
    if (choices) IUIAutomationElementArray_Release(choices);
    selection = NULL;
    choices = NULL;
    if (last_selection) IUIAutomationElement_Release(last_selection);
    last_selection = NULL;
}

static void w_free_labels(w_choice_labels *labels) {
    if (labels->names) {
        for (int i = 0; i < labels->count; ++i) SysFreeString(labels->names[i]);
        free(labels->names);
    }
    SysFreeString(labels->title);
}

static IUIAutomationElement *w_element(IUIAutomationElementArray *array, int index) {
    IUIAutomationElement *item = NULL;
    if (array && index >= 0) IUIAutomationElementArray_GetElement(array, index, &item);
    return item;
}

static bool w_read_choices(LONG view) {
    IUIAutomationElement *root = NULL, *apps = NULL;
    if (FAILED(IUIAutomation_ElementFromHandle(uia, picker, &root)) || !root) return false;
    bool ready = false;
    IUIAutomationElement_FindFirst(root, TreeScope_Descendants, app_list_id, &apps);
    RECT bounds = {0};
    if (apps && SUCCEEDED(IUIAutomationElement_get_CurrentBoundingRectangle(apps, &bounds))) {
        if (!IsRectEmpty(&bounds)) {
            PostMessageW(panel, WM_CLOSE, 0, 0);
        } else {
            w_clear();
            IUIAutomationElement *header = NULL;
            w_choice_labels labels = {0};
            if (SUCCEEDED(IUIAutomationElement_FindFirst(root, TreeScope_Descendants, heading_id, &header)) && header) {
                IUIAutomationElement_get_CurrentName(header, &labels.title);
                IUIAutomationElement_Release(header);
            }
            IUIAutomationElement_GetCurrentPatternAs(apps, UIA_SelectionPatternId, &IID_IUIAutomationSelectionPattern, (void **)&selection);
            IUIAutomationElement_FindAll(root, TreeScope_Descendants, row_types, &choices);
            if (choices) IUIAutomationElementArray_get_Length(choices, &labels.count);
            if (labels.count > 0) labels.names = calloc(labels.count, sizeof(*labels.names));
            for (int i = 0; labels.names && i < labels.count; ++i) {
                IUIAutomationElement *item = w_element(choices, i);
                if (!item) continue;
                IUIAutomationElement_get_CurrentName(item, &labels.names[i]);
                IUIAutomationElement_Release(item);
                if (labels.names[i] && *labels.names[i]) ready = true;
                if (view != InterlockedCompareExchange(&active_view, 0, 0)) break;
            }
            if (ready) {
                AcquireSRWLockExclusive(&view_lock);
                w_free_labels(&changes.labels);
                changes.labels = labels;
                changes.labels_view = view;
                ReleaseSRWLockExclusive(&view_lock);
                PostMessageW(panel, provider_changed, 0, 0);
            } else w_free_labels(&labels);
        }
    }
    if (apps) IUIAutomationElement_Release(apps);
    IUIAutomationElement_Release(root);
    return ready;
}

static void w_sync_selection(LONG view) {
    if (!selection) return;
    IUIAutomationElementArray *selected = NULL;
    if (FAILED(IUIAutomationSelectionPattern_GetCurrentSelection(selection, &selected)) || !selected) return;
    IUIAutomationElement *item = w_element(selected, 0);
    IUIAutomationElementArray_Release(selected);
    BOOL same = item == last_selection;
    if (!same && item && last_selection) IUIAutomation_CompareElements(uia, item, last_selection, &same);
    if (same) {
        if (item) IUIAutomationElement_Release(item);
        return;
    }
    if (last_selection) IUIAutomationElement_Release(last_selection);
    last_selection = item;
    int count = 0;
    int selected_index = -1;
    if (item && choices) IUIAutomationElementArray_get_Length(choices, &count);
    for (int i = 0; i < count; ++i) {
        IUIAutomationElement *candidate = w_element(choices, i);
        if (!candidate) continue;
        same = FALSE;
        IUIAutomation_CompareElements(uia, item, candidate, &same);
        IUIAutomationElement_Release(candidate);
        if (same) { selected_index = i; break; }
    }
    AcquireSRWLockExclusive(&view_lock);
    changes.selection_view = view;
    changes.selected = selected_index;
    ReleaseSRWLockExclusive(&view_lock);
    PostMessageW(panel, provider_changed, 0, 0);
}

static void w_choose(int index) {
    if (!w_picker_alive() || GetForegroundWindow() != picker || !IsWindowEnabled(picker)) return;
    IUIAutomationElement *item = w_element(choices, index);
    if (!item) return;
    IUIAutomationSelectionItemPattern *select = NULL;
    IUIAutomationInvokePattern *invoke = NULL;
    if (SUCCEEDED(IUIAutomationElement_GetCurrentPatternAs(item, UIA_SelectionItemPatternId,
            &IID_IUIAutomationSelectionItemPattern, (void **)&select)) && select) {
        IUIAutomationSelectionItemPattern_Select(select);
        IUIAutomationSelectionItemPattern_Release(select);
    } else if (SUCCEEDED(IUIAutomationElement_GetCurrentPatternAs(item, UIA_InvokePatternId,
               &IID_IUIAutomationInvokePattern, (void **)&invoke)) && invoke) {
        IUIAutomationInvokePattern_Invoke(invoke);
        IUIAutomationInvokePattern_Release(invoke);
    }
    IUIAutomationElement_Release(item);
}

static DWORD WINAPI w_provider(void *unused) {
    (void)unused;
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) {
        hr = CoCreateInstance(&CLSID_CUIAutomation, NULL, CLSCTX_INPROC_SERVER, &IID_IUIAutomation, (void **)&uia);
        if (SUCCEEDED(hr)) hr = w_conditions();
        LONG loaded = 0;
        while (SUCCEEDED(hr) && !InterlockedCompareExchange(&finished, 0, 0)) {
            LONG view = InterlockedCompareExchange(&active_view, 0, 0);
            LONG64 choice = InterlockedExchange64(&requested_choice, 0);
            if (view && GetForegroundWindow() == picker) {
                if (loaded != view) loaded = w_read_choices(view) ? view : 0;
                if (loaded && (LONG)(choice >> 32) == loaded) w_choose((int)choice);
                if (loaded) w_sync_selection(view);
            }
            WaitForSingleObject(wake, 250);
        }
        w_clear();
        if (app_list_id) IUIAutomationCondition_Release(app_list_id);
        if (heading_id) IUIAutomationCondition_Release(heading_id);
        if (row_types) IUIAutomationCondition_Release(row_types);
        if (uia) IUIAutomation_Release(uia);
        CoUninitialize();
    }
    if (FAILED(hr) && !InterlockedCompareExchange(&finished, 0, 0)) PostMessageW(panel, WM_CLOSE, 0, 0);
    return 0;
}

static void w_request_choice(int row) {
    if (row < 0 || !active_view || GetForegroundWindow() != picker) return;
    LRESULT index = SendMessageW(list, LB_GETITEMDATA, row, 0);
    if (index == LB_ERR) return;
    InterlockedExchange64(&requested_choice, ((LONG64)active_view << 32) | (DWORD)index);
    SetEvent(wake);
}

static LRESULT CALLBACK w_list_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
    (void)data;
    static int pressed = -1;
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK || msg == WM_LBUTTONUP) {
        DWORD hit = (DWORD)SendMessageW(w, LB_ITEMFROMPOINT, 0, lp);
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        RECT rect;
        int row = !HIWORD(hit) && SendMessageW(w, LB_GETITEMRECT, LOWORD(hit), (LPARAM)&rect) != LB_ERR &&
                  PtInRect(&rect, pt) ? LOWORD(hit) : -1;
        if (msg == WM_LBUTTONUP) {
            if (row == pressed) w_request_choice(row);
            pressed = -1;
        } else pressed = row;
        return 0; // The native list box would call SetFocus on mouse-down.
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(w, w_list_proc, id);
    return DefSubclassProc(w, msg, wp, lp);
}

static void w_update(void) {
    if (!w_picker_alive()) { PostMessageW(panel, WM_CLOSE, 0, 0); return; }
    if (GetForegroundWindow() != picker || !IsWindowEnabled(picker)) {
        InterlockedExchange(&active_view, 0);
        if (IsWindowVisible(panel)) ShowWindow(panel, SW_HIDE);
        populated = false;
        return;
    }
    if (!active_view) {
        InterlockedExchange(&active_view, ++view_serial);
        SetEvent(wake);
    }
    if (!populated) {
        if (IsWindowVisible(panel)) ShowWindow(panel, SW_HIDE);
        return;
    }
    RECT r;
    MONITORINFO monitor = {.cbSize=sizeof(monitor)};
    if (!GetWindowRect(picker, &r) || !GetMonitorInfoW(MonitorFromWindow(picker, MONITOR_DEFAULTTONEAREST), &monitor)) return;
    UINT dpi = GetDpiForWindow(picker);
    if (r.bottom - r.top >= MulDiv(160, dpi, 96)) { PostMessageW(panel, WM_CLOSE, 0, 0); return; }
    if (dpi != font_dpi) {
        HFONT next = w_theme_message_font(dpi);
        SendMessageW(list, WM_SETFONT, (WPARAM)next, TRUE);
        SendMessageW(heading, WM_SETFONT, (WPARAM)next, TRUE);
        DeleteObject(font);
        font = next;
        font_dpi = dpi;
    }
    RECT work = monitor.rcWork;
    int side = w_theme_button_size(dpi), gap = w_theme_spacing(dpi);
    int width = min(r.right - r.left, work.right - work.left);
    int count = (int)SendMessageW(list, LB_GETCOUNT, 0, 0);
    int desired = 2 * gap + side * (1 + min(count, 8));
    int above = r.top - work.top, below = work.bottom - r.bottom;
    bool on_top = desired <= above || above >= below;
    int height = min(desired, on_top ? above : below);
    if (height < side + 2 * gap || width <= 2 * gap) return;
    int x = w_clamp_int(r.left, work.left, work.right - width), y = on_top ? r.top - height : r.bottom;
    RECT old;
    if (!GetWindowRect(panel, &old) || old.left != x || old.top != y || old.right != x + width ||
        old.bottom != y + height || !IsWindowVisible(panel)) {
        SetWindowPos(panel, HWND_TOPMOST, x,y,width,height, SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
        SetWindowPos(heading, NULL, gap,gap,width-2*gap,side, SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(list, NULL, gap,gap+side,width-2*gap,height-side-2*gap, SWP_NOZORDER | SWP_NOACTIVATE);
        SendMessageW(list, LB_SETITEMHEIGHT, 0, side);
    }
}

static LRESULT CALLBACK w_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    LRESULT result;
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (msg == WM_TIMER) { w_update(); return 0; }
    if (msg == provider_changed) {
        AcquireSRWLockExclusive(&view_lock);
        struct w_changes next = changes;
        changes = (struct w_changes){0};
        ReleaseSRWLockExclusive(&view_lock);
        LONG view = GetForegroundWindow() == picker ? active_view : 0;
        if (view && next.labels_view == view) {
            const w_choice_labels *labels = &next.labels;
            SendMessageW(list, LB_RESETCONTENT, 0, 0);
            if (labels->title && *labels->title) SetWindowTextW(heading, labels->title);
            for (int i = 0; i < labels->count; ++i) {
                if (!labels->names[i] || !*labels->names[i]) continue;
                LRESULT row = SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)labels->names[i]);
                if (row >= 0) SendMessageW(list, LB_SETITEMDATA, row, i);
            }
            populated = SendMessageW(list, LB_GETCOUNT, 0, 0) > 0;
        }
        w_free_labels(&next.labels);
        if (view && next.selection_view == view) {
            int count = (int)SendMessageW(list, LB_GETCOUNT, 0, 0), row = -1;
            for (int i = 0; i < count; ++i)
                if (SendMessageW(list, LB_GETITEMDATA, i, 0) == next.selected) { row = i; break; }
            SendMessageW(list, LB_SETCURSEL, row, 0);
        }
        w_update();
        return 0;
    }
    if (msg == WM_COMMAND && (HWND)lp == list && HIWORD(wp) == LBN_SELCHANGE) {
        w_request_choice((int)SendMessageW(list, LB_GETCURSEL, 0, 0));
        return 0;
    }
    if (msg == WM_CLOSE || msg == WM_DESTROY) {
        InterlockedExchange(&active_view, 0);
        InterlockedExchange(&finished, TRUE);
        if (wake) SetEvent(wake);
        ShowWindow(w, SW_HIDE);
        PostQuitMessage(0);
        return 0;
    }
    if (w_theme_message(w, msg, wp, lp, &result)) return result;
    return DefWindowProcW(w, msg, wp, lp);
}

int w_open_with_list(const wchar_t *target) {
    unsigned long long value;
    wchar_t extra, cls[32];
    if (swscanf(target, L"%llx-%lu%lc", &value, &picker_pid, &extra) != 2) return ERROR_INVALID_PARAMETER;
    picker = (HWND)(uintptr_t)value;
    if (!w_picker_alive() || !w_os_open_with_broker(picker) || !GetClassNameW(picker, cls, count_of(cls)) ||
        wcscmp(cls, L"Open With")) return 0;
    wchar_t key[96];
    swprintf(key, count_of(key), L"Local\\winwm.open_with.%lu.%llx", picker_pid, value);
    HANDLE instance = CreateMutexW(NULL, FALSE, key);
    HANDLE worker = NULL;
    int code = 1;
    if (!instance) goto done;
    if (GetLastError() == ERROR_ALREADY_EXISTS) { code = 0; goto done; }
    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSW wc = {.hInstance=inst, .lpszClassName=L"WinwmOpenWithList", .lpfnWndProc=w_proc,
                    .hCursor=LoadCursorW(NULL, IDC_ARROW)};
    if (!RegisterClassW(&wc)) goto done;
    panel = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, wc.lpszClassName, L"Open with",
                            WS_POPUP | WS_CLIPCHILDREN, 0,0,1,1, NULL, NULL, inst, NULL);
    if (!panel) goto done;
    heading = CreateWindowW(L"Static", L"Choose an app", WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE | SS_ENDELLIPSIS | SS_NOPREFIX,
                            0,0,1,1, panel, NULL, inst, NULL);
    list = CreateWindowW(L"ListBox", L"Applications", WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_HASSTRINGS | LBS_NOTIFY |
                         LBS_OWNERDRAWFIXED | LBS_NOINTEGRALHEIGHT, 0,0,1,1, panel, NULL, inst, NULL);
    if (!heading || !list || !SetWindowSubclass(list, w_list_proc, 1, 0) || !SetTimer(panel, 1, 250, NULL)) goto done;
    wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!wake) goto done;
    worker = CreateThread(NULL, 0, w_provider, NULL, 0, NULL);
    if (!worker) goto done;
    w_update();
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    code = 0;
done:
    InterlockedExchange(&finished, TRUE);
    if (wake) SetEvent(wake);
    // A provider may be hung in another process. Closing this standalone
    // helper must not wait for it or tear down resources it is still using.
    if (!worker || WaitForSingleObject(worker, 50) == WAIT_OBJECT_0) {
        if (IsWindow(panel)) DestroyWindow(panel);
        if (wake) CloseHandle(wake);
        DeleteObject(font);
        w_free_labels(&changes.labels);
    }
    if (worker) CloseHandle(worker);
    if (instance) CloseHandle(instance);
    return code;
}
