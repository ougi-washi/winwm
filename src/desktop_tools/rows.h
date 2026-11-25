#pragma once
#include "win_platform/win_platform.h"

// Reconcile an unsorted native list box in display order. Existing strings stay
// in place across identical snapshots, including the focused and scrolled rows.
typedef struct {
    HWND list;
    int written, original, top;
} w_tool_rows;

enum { w_rows_show_selection = -1 };

static inline int w_rows_top_showing(int selected, int top, int height, int row_height) {
    int visible = row_height > 0 ? max(1,height/row_height) : 1;
    return max(selected-visible+1,min(top,selected));
}

static inline w_tool_rows w_rows_begin(HWND list) {
    return (w_tool_rows){list,0,(int)SendMessageW(list,LB_GETCOUNT,0,0),
                         (int)SendMessageW(list,LB_GETTOPINDEX,0,0)};
}

static inline int w_rows_put(w_tool_rows *r, const wchar_t *text, LPARAM data) {
    int i=r->written;
    bool same=false;
    if (i<r->original) {
        wchar_t local[512];
        LRESULT length=SendMessageW(r->list,LB_GETTEXTLEN,i,0);
        if (length >= 0 && (size_t)length == wcslen(text)) {
            wchar_t *displayed = (size_t)length < count_of(local) ? local : malloc((length+1)*sizeof(*displayed));
            same = displayed && SendMessageW(r->list,LB_GETTEXT,i,(LPARAM)displayed)>=0 && !wcscmp(displayed,text);
            if (displayed != local) free(displayed);
        }
        if (!same) {
            SendMessageW(r->list,LB_DELETESTRING,i,0);
            --r->original;
        }
    }
    if (!same) {
        if (SendMessageW(r->list,LB_INSERTSTRING,i,(LPARAM)text)<0) return -1;
        ++r->original;
    }
    if (!same || SendMessageW(r->list,LB_GETITEMDATA,i,0)!=data)
        SendMessageW(r->list,LB_SETITEMDATA,i,data);
    return r->written++;
}

static inline void w_rows_end(w_tool_rows *r, int selected, int top) {
    while (r->original>r->written) SendMessageW(r->list,LB_DELETESTRING,--r->original,0);
    if (selected >= r->written) selected=-1;
    if (SendMessageW(r->list,LB_GETCURSEL,0,0)!=selected)
        SendMessageW(r->list,LB_SETCURSEL,selected,0);
    if (top == w_rows_show_selection) {
        RECT client={0};
        int height = (int)SendMessageW(r->list,LB_GETITEMHEIGHT,0,0);
        GetClientRect(r->list,&client);
        top = w_rows_top_showing(selected,r->top,client.bottom,height);
    }
    top=max(0,min(top,r->written-1));
    if (r->written && SendMessageW(r->list,LB_GETTOPINDEX,0,0)!=top)
        SendMessageW(r->list,LB_SETTOPINDEX,top,0);
}
