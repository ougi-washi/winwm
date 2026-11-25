#include "theme/control_hover.h"
#include "theme/hover.h"
#include "theme/surface.h"
#include "win_platform/paint.h"
#include <commctrl.h>
#include <windowsx.h>

enum w_hit_kind { listbox_hit, listview_hit, combo_hit, combo_list_hit, slider_hit, header_hit };
typedef struct {
    enum w_hit_kind kind;
    bool tracking,previewing;
    int anchor,option_width;
    POINT point;
} w_native_hit;

static const wchar_t hit_property[]=L"winwm.native.hover";
static const wchar_t edit_property[]=L"winwm.native.edit.border";

static void w_fit_dropdown(HWND combo,w_native_hit *hit) {
    int count=(int)SendMessageW(combo,CB_GETCOUNT,0,0);
    if (count<0) return;
    if (hit->option_width<0) {
        HDC dc=GetDC(combo);
        if (!dc) return;
        HFONT font=(HFONT)SendMessageW(combo,WM_GETFONT,0,0);
        HGDIOBJ old=SelectObject(dc,font ? font : GetStockObject(DEFAULT_GUI_FONT));
        int widest=0;
        for (int i=0;i<count;i++) {
            LRESULT n=SendMessageW(combo,CB_GETLBTEXTLEN,i,0);
            if (n<=0 || n>=INT_MAX) continue;
            wchar_t local[256],*text=n<(int)count_of(local) ? local : malloc(((size_t)n+1)*sizeof(*text));
            if (!text) { widest=-1; break; }
            SIZE size={0};
            if (SendMessageW(combo,CB_GETLBTEXT,i,(LPARAM)text)!=CB_ERR && w_theme_text_extent(dc,text,(int)n,&size))
                widest=max(widest,size.cx);
            if (text!=local) free(text);
        }
        SelectObject(dc,old);
        ReleaseDC(combo,dc);
        if (widest<0) return;
        hit->option_width=widest;
    }
    MONITORINFO mi={.cbSize=sizeof(mi)};
    RECT field,open;
    if (!GetWindowRect(combo,&field) ||
        !GetMonitorInfoW(MonitorFromRect(&field,MONITOR_DEFAULTTONEAREST),&mi)) return;
    UINT dpi=GetDpiForWindow(combo);
    int edge=GetSystemMetricsForDpi(SM_CXEDGE,dpi);
    int width=hit->option_width+2*(w_theme_spacing(dpi)+edge);
    LRESULT row=SendMessageW(combo,CB_GETITEMHEIGHT,0,0);
    if (row>0 && SendMessageW(combo,CB_GETDROPPEDCONTROLRECT,0,(LPARAM)&open)) {
        int available=(open.bottom-open.top)-(field.bottom-field.top)-2*edge;
        if (count>max(0,available)/(int)row) width+=GetSystemMetricsForDpi(SM_CXVSCROLL,dpi);
    }
    width=min(mi.rcWork.right-mi.rcWork.left,max(field.right-field.left,width));
    if (SendMessageW(combo,CB_GETDROPPEDWIDTH,0,0)!=width) SendMessageW(combo,CB_SETDROPPEDWIDTH,width,0);
}

// Windows sends CBN_DROPDOWN before placement for keyboard, pointer and
// CB_SHOWDROPDOWN opens. Measure after the owner has populated any choices.
static LRESULT CALLBACK w_dropdown_owner(HWND w,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR data) {
    (void)data;
    if (msg==WM_NCDESTROY) RemoveWindowSubclass(w,w_dropdown_owner,id);
    LRESULT result=DefSubclassProc(w,msg,wp,lp);
    if (msg==WM_COMMAND && HIWORD(wp)==CBN_DROPDOWN && lp && IsWindow((HWND)lp)) {
        w_native_hit *hit=(w_native_hit *)GetPropW((HWND)lp,hit_property);
        if (hit && hit->kind==combo_hit && (GetWindowLongW((HWND)lp,GWL_STYLE)&CBS_HASSTRINGS))
            w_fit_dropdown((HWND)lp,hit);
    }
    return result;
}

static bool w_target_bounds(HWND w,INT_PTR key,RECT *rect,void *data) {
    w_native_hit *hit=data;
    if (key<0 || key>INT_MAX) return false;
    switch (hit->kind) {
    case listbox_hit:
    case combo_list_hit:
        if (SendMessageW(w,LB_GETITEMRECT,(int)key,(LPARAM)rect)==LB_ERR) return false;
        break;
    case listview_hit:
        if (!ListView_GetItemRect(w,(int)key,rect,LVIR_BOUNDS)) return false;
        break;
    case header_hit:
        if (!Header_GetItemRect(w,(int)key,rect)) return false;
        break;
    case slider_hit:
        return !key && GetClientRect(w,rect) && !IsRectEmpty(rect);
    case combo_hit: {
        if (key) return false;
        if ((GetWindowLongW(w,GWL_STYLE)&3)==CBS_DROPDOWNLIST) return GetClientRect(w,rect) && !IsRectEmpty(rect);
        COMBOBOXINFO info={.cbSize=sizeof(info)};
        if (!GetComboBoxInfo(w,&info) || IsRectEmpty(&info.rcButton)) return false;
        *rect=info.rcButton;
        return true;
    }
    }
    RECT client,visible;
    return GetClientRect(w,&client) && IntersectRect(&visible,rect,&client);
}

static void w_hit_at(HWND control,w_native_hit *hit) {
    RECT rect;
    int key=-1;
    POINT pt=hit->point;
    if (GetClientRect(control,&rect) && PtInRect(&rect,pt) && IsWindowEnabled(control)) {
        switch (hit->kind) {
        case slider_hit:
        case combo_hit:
            if (w_target_bounds(control,0,&rect,hit) && PtInRect(&rect,pt)) key=0;
            break;
        case listview_hit: {
            LVHITTESTINFO row={.pt=pt};
            int i=(int)SendMessageW(control,LVM_SUBITEMHITTEST,0,(LPARAM)&row);
            if (w_target_bounds(control,i,&rect,hit)) key=i;
            break;
        }
        case header_hit: {
            HDHITTESTINFO header={.pt=pt};
            int i=(int)SendMessageW(control,HDM_HITTEST,0,(LPARAM)&header);
            if ((header.flags&HHT_ONHEADER) && w_target_bounds(control,i,&rect,hit)) key=i;
            break;
        }
        case combo_list_hit:
        case listbox_hit: {
            LRESULT at=SendMessageW(control,LB_ITEMFROMPOINT,0,MAKELPARAM(pt.x,pt.y));
            if (!HIWORD(at) && w_target_bounds(control,LOWORD(at),&rect,hit) &&
                PtInRect(&rect,pt)) key=LOWORD(at);
            break;
        }
        }
    }
    // A ComboLBox immediately sets its native selection as the mouse crosses
    // a row. Keep the opening/keyboard choice underneath the mouse preview so
    // the incoming and outgoing rows can use their actual animation coverage.
    if (key>=0 && hit->kind==combo_list_hit && !hit->previewing && !w_theme_high_contrast()) {
        hit->anchor=(int)SendMessageW(control,LB_GETCURSEL,0,0);
        hit->previewing=true;
    }
    w_hover_target(control,key,key>=0 ? &rect : NULL);
}

static void w_end_preview(HWND list) {
    w_native_hit *hit=(w_native_hit *)GetPropW(list,hit_property);
    if (!hit || !hit->previewing) return;
    hit->previewing=false;
    hit->tracking=false;
    w_hover_clear(list);
    InvalidateRect(list,NULL,FALSE);
}

static bool w_changed_rows(enum w_hit_kind kind,UINT msg) {
    if (msg==WM_SIZE) return true;
    if (kind==combo_hit || kind==slider_hit) return false;
    if (msg==WM_HSCROLL || msg==WM_VSCROLL || msg==WM_MOUSEWHEEL ||
        msg==WM_MOUSEHWHEEL || msg==WM_KEYDOWN) return true;
    if (kind==header_hit) return msg==HDM_SETITEMW || msg==HDM_SETITEMA || msg==HDM_SETORDERARRAY;
    if (kind==listview_hit) return msg==LVM_SETITEMCOUNT || msg==LVM_DELETEITEM || msg==LVM_DELETEALLITEMS ||
        msg==LVM_INSERTITEMW || msg==LVM_SCROLL || msg==LVM_ENSUREVISIBLE || msg==LVM_SETCOLUMNWIDTH;
    if (kind==listbox_hit || kind==combo_list_hit) return msg==LB_SETTOPINDEX || msg==LB_DELETESTRING ||
        msg==LB_INSERTSTRING || msg==LB_RESETCONTENT || msg==LB_SETCOUNT || msg==LB_SETITEMHEIGHT || msg==LB_SETCURSEL;
    return false;
}

static void w_arrow(HWND combo,HDC dc) {
    COMBOBOXINFO info={.cbSize=sizeof(info)};
    if (!dc || !GetComboBoxInfo(combo,&info) || IsRectEmpty(&info.rcButton)) return;
    RECT r=info.rcButton;
    int amount=w_hover_amount(combo,0);
    bool pressed=SendMessageW(combo,CB_GETDROPPEDSTATE,0,0)!=0;
    bool enabled=IsWindowEnabled(combo)!=FALSE;
    int saved=SaveDC(dc);
    if (!saved) return;
    IntersectClipRect(dc,r.left,r.top,r.right,r.bottom);
    COLORREF ink=w_hover_row(dc,&r,false,enabled ? amount : 0,enabled && pressed);
    if (!enabled) ink=w_theme_hint();
    int x=(r.left+r.right)/2, y=(r.top+r.bottom)/2;
    int half=min(min((r.right-r.left)/3,(r.bottom-r.top)/3),max(2,MulDiv(4,GetDpiForWindow(combo),96)));
    // Stroked chevron, centered independently of the edit's selection and caret.
    SelectObject(dc,GetStockObject(DC_PEN));
    SetDCPenColor(dc,ink);
    MoveToEx(dc,x-half,y-half/2,NULL); LineTo(dc,x,y+(half+1)/2); LineTo(dc,x+half+1,y-half/2-1);
    RestoreDC(dc,saved);
    RECT frame;
    HWND focus=GetFocus();
    if (GetClientRect(combo,&frame)) w_theme_input_frame(dc,&frame,amount,pressed || focus==combo || IsChild(combo,focus),enabled);
}

typedef struct {
    RECT parent_frame;
    HFONT requested,fitted;
    int font_height,target_height,line_height,padding;
    bool tracking,client_frame,field,embedded;
} w_edit_border;

// The EDIT owns its text, caret, selection and IME. Center its actual client
// area so all four share one coordinate system, including during composition.
static void w_field_font(HWND edit,w_edit_border *border,HFONT requested,int height) {
    if (requested && requested==border->fitted) requested=border->requested;
    bool changed=requested!=border->requested || !border->font_height;
    if (changed) {
        border->requested=requested;
        border->font_height=w_theme_line_height(requested ? requested : GetStockObject(SYSTEM_FONT));
    }
    int inset=border->embedded ? 0 : 2;
    int limit=height>max(1,inset) ? height-inset : border->font_height;
    int line=min(limit,border->font_height);
    if (!changed && border->target_height==line) return;
    HFONT fitted=w_theme_create_fitting_font(requested ? requested : GetStockObject(SYSTEM_FONT),border->font_height,limit);
    HFONT old=border->fitted;
    border->fitted=fitted;
    border->target_height=line;
    border->line_height=fitted ? w_theme_line_height(fitted) : border->font_height;
    DefSubclassProc(edit,WM_SETFONT,(WPARAM)(fitted ? fitted : requested),FALSE);
    DeleteObject(old);
}

static RECT w_field_client(HWND edit,const RECT *window) {
    RECT r;
    GetClientRect(edit,&r);
    MapWindowPoints(edit,NULL,(POINT *)&r,2);
    OffsetRect(&r,-window->left,-window->top);
    return r;
}

static void w_edit_frame(HWND edit,const w_edit_border *border,HDC print_dc,bool background) {
    if (!IsRectEmpty(&border->parent_frame)) {
        if (print_dc) return;
        HWND parent=GetParent(edit);
        if (!parent) return;
        RECT r=border->parent_frame;
        RECT edges[]={{r.left,r.top,r.right,r.top+1},{r.left,r.bottom-1,r.right,r.bottom},
                      {r.left,r.top+1,r.left+1,r.bottom-1},{r.right-1,r.top+1,r.right,r.bottom-1}};
        for (size_t i=0;i<count_of(edges);++i) InvalidateRect(parent,&edges[i],FALSE);
        return;
    }
    RECT box;
    if ((!border->field && w_theme_high_contrast()) || !GetWindowRect(edit,&box)) return;
    RECT client={0};
    if (border->field && background) client=w_field_client(edit,&box);
    OffsetRect(&box,-box.left,-box.top);
    HDC dc=print_dc ? print_dc : GetWindowDC(edit);
    if (dc) {
        if (border->field && background) {
            int saved=SaveDC(dc);
            if (saved) {
                ExcludeClipRect(dc,client.left,client.top,client.right,client.bottom);
                FillRect(dc,&box,w_theme_brush());
                RestoreDC(dc,saved);
            }
        }
        if (!border->embedded) w_theme_input_frame(dc,&box,w_hover_amount(edit,0),GetFocus()==edit,IsWindowEnabled(edit)!=FALSE);
        if (!print_dc) ReleaseDC(edit,dc);
    }
}

// Classic single-line EDIT draws WS_BORDER inside its client. Multiline and
// rich edits can have a nonclient border. Both receive only outline damage.
static LRESULT CALLBACK w_edit_frame_proc(HWND w,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR data) {
    w_edit_border *border=(w_edit_border *)data;
    if (msg==WM_NCDESTROY) {
        HFONT fitted=border->fitted;
        RemovePropW(w,edit_property);
        RemoveWindowSubclass(w,w_edit_frame_proc,id);
        free(border);
        LRESULT result=DefSubclassProc(w,msg,wp,lp);
        DeleteObject(fitted);
        return result;
    }
    if (border->field) {
        if (msg==WM_NCCALCSIZE) {
            RECT *r=(RECT *)lp; // The first member of NCCALCSIZE_PARAMS is rgrc[0].
            int height=r->bottom-r->top,width=r->right-r->left;
            w_field_font(w,border,border->requested,height);
            int line=min(height,border->line_height);
            border->padding=border->embedded ? 0 : max(1,w_theme_spacing(GetDpiForWindow(w))/2);
            int pad=min(width/2,border->padding);
            r->left+=pad;
            r->right-=pad;
            r->top+=(height-line)/2;
            r->bottom=r->top+line;
            return 0;
        }
        if (msg==WM_SETFONT) {
            RECT window;
            GetWindowRect(w,&window);
            int previous=border->line_height;
            w_field_font(w,border,(HFONT)wp,window.bottom-window.top);
            if (previous!=border->line_height || (!border->embedded && border->padding!=max(1,w_theme_spacing(GetDpiForWindow(w))/2)))
                SetWindowPos(w,NULL,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
            if (lp) InvalidateRect(w,NULL,FALSE);
            return 0;
        }
        if (msg==WM_NCHITTEST) {
            RECT window;
            POINT p={GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
            if (GetWindowRect(w,&window) && PtInRect(&window,p)) return HTCLIENT;
        }
        if (msg==WM_LBUTTONDOWN || msg==WM_LBUTTONDBLCLK || msg==WM_LBUTTONUP || (msg==WM_MOUSEMOVE && (wp&MK_LBUTTON))) {
            RECT client;
            GetClientRect(w,&client);
            lp=MAKELPARAM(GET_X_LPARAM(lp),w_clamp_int(GET_Y_LPARAM(lp),0,max(0,client.bottom-1)));
        }
        if (msg==WM_PRINT) {
            HDC dc=(HDC)wp;
            if ((lp&PRF_CHECKVISIBLE) && !IsWindowVisible(w)) return 0;
            if (lp&PRF_NONCLIENT) w_edit_frame(w,border,dc,true);
            if (lp&PRF_CLIENT) {
                RECT window;
                GetWindowRect(w,&window);
                RECT client=w_field_client(w,&window);
                int saved=SaveDC(dc);
                if (saved) {
                    IntersectClipRect(dc,client.left,client.top,client.right,client.bottom);
                    OffsetViewportOrgEx(dc,client.left,client.top,NULL);
                    DefSubclassProc(w,WM_PRINTCLIENT,wp,lp);
                    RestoreDC(dc,saved);
                }
            }
            return 0;
        }
    }
    if (msg==WM_MOUSEMOVE && !border->embedded && IsWindowEnabled(w)) {
        if (!border->tracking) {
            TRACKMOUSEEVENT leave={sizeof(leave),TME_LEAVE,w,0};
            border->tracking=TrackMouseEvent(&leave)!=FALSE;
        }
        if (border->tracking) w_hover_frame_target(w,true);
    } else if (border->tracking && (msg==WM_MOUSELEAVE || msg==WM_CANCELMODE ||
               (msg==WM_ENABLE && !wp) || (msg==WM_SHOWWINDOW && !wp))) {
        border->tracking=false;
        w_hover_frame_target(w,false);
    }
    if (msg==w_hover_frame_changed) { if (IsWindowVisible(w)) w_edit_frame(w,border,NULL,false); return 0; }
    bool client_frame=border->client_frame;
    bool repaint=msg==WM_NCPAINT || msg==WM_SETFOCUS || msg==WM_KILLFOCUS || msg==WM_ENABLE ||
                 (client_frame && msg==WM_PAINT);
    bool print=(msg==WM_PRINT && (lp&(PRF_NONCLIENT|(client_frame ? PRF_CLIENT : 0)))) ||
               (client_frame && (msg==WM_PRINTCLIENT || (msg==WM_PAINT && wp)));
    if (!repaint && !print) return DefSubclassProc(w,msg,wp,lp);
    // An edit notification during native focus processing can destroy its dialog.
    w_edit_border frame=*border;
    int saved=print ? SaveDC((HDC)wp) : 0;
    LRESULT result=DefSubclassProc(w,msg,wp,lp);
    if (saved) RestoreDC((HDC)wp,saved);
    if (IsWindow(w)) w_edit_frame(w,&frame,print ? (HDC)wp : NULL,msg==WM_NCPAINT);
    return result;
}

static void w_center_field(HWND edit,bool embedded) {
    if (!edit) return;
    w_native_edit_frame(edit,NULL);
    w_edit_border *border=(w_edit_border *)GetPropW(edit,edit_property);
    if (border) {
        border->field=true;
        border->client_frame=false;
        border->embedded=embedded;
        HFONT font=(HFONT)SendMessageW(edit,WM_GETFONT,0,0);
        SendMessageW(edit,WM_SETFONT,(WPARAM)(font ? font : GetStockObject(DEFAULT_GUI_FONT)),FALSE);
    }
}

HWND w_native_field(HWND parent,int id,DWORD style) {
    HWND edit=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_TABSTOP|ES_AUTOHSCROLL|style,0,0,1,1,
                              parent,(HMENU)(INT_PTR)id,GetModuleHandleW(NULL),NULL);
    w_center_field(edit,false);
    return edit;
}

HFONT w_native_requested_font(HWND control) {
    w_edit_border *border=(w_edit_border *)GetPropW(control,edit_property);
    // Script fallback must retain the owner's font, including when the field
    // was fitted before that subclass was installed.
    return border && border->field ? border->requested : (HFONT)SendMessageW(control,WM_GETFONT,0,0);
}

void w_native_edit_frame(HWND edit,const RECT *frame) {
    if (!edit) return;
    w_edit_border *border=(w_edit_border *)GetPropW(edit,edit_property);
    if (!border) {
        border=calloc(1,sizeof(*border));
        if (!border) return;
        if (!SetPropW(edit,edit_property,border) || !SetWindowSubclass(edit,w_edit_frame_proc,1,(DWORD_PTR)border)) {
            RemovePropW(edit,edit_property);
            free(border);
            return;
        }
    }
    if (frame) border->parent_frame=*frame;
    border->client_frame=IsRectEmpty(&border->parent_frame) && !(GetWindowLongW(edit,GWL_STYLE)&WS_BORDER) &&
                         !(GetWindowLongW(edit,GWL_EXSTYLE)&WS_EX_CLIENTEDGE);
}

static LRESULT CALLBACK w_native_hit_proc(HWND w,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR data) {
    w_native_hit *hit=(w_native_hit *)data;
    if (hit->kind==combo_hit && (msg==CB_ADDSTRING || msg==CB_INSERTSTRING || msg==CB_DELETESTRING ||
        msg==CB_RESETCONTENT || msg==CB_DIR || msg==WM_SETFONT || msg==WM_FONTCHANGE || msg==WM_DPICHANGED_AFTERPARENT))
        hit->option_width=-1;
    if (msg==WM_NCDESTROY) {
        RemovePropW(w,hit_property);
        RemoveWindowSubclass(w,w_native_hit_proc,id);
        free(hit);
        return DefSubclassProc(w,msg,wp,lp);
    }
    if (msg==WM_KEYDOWN || msg==WM_CHAR || msg==WM_SYSKEYDOWN || msg==CB_SETCURSEL) {
        if (hit->kind==combo_list_hit) w_end_preview(w);
        if (hit->kind==combo_hit) {
            COMBOBOXINFO info={.cbSize=sizeof(info)};
            if (GetComboBoxInfo(w,&info)) w_end_preview(info.hwndList);
        }
    }
    if ((msg==WM_MOUSEMOVE || msg==WM_LBUTTONDOWN) && IsWindowEnabled(w)) {
        hit->point=(POINT){GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
        if (!hit->tracking) {
            TRACKMOUSEEVENT leave={sizeof(leave),TME_LEAVE,w,0};
            hit->tracking=TrackMouseEvent(&leave)!=FALSE;
        }
        w_hit_at(w,hit);
    } else if (msg==WM_MOUSELEAVE) {
        hit->tracking=false;
        w_hover_target(w,-1,NULL);
    } else if (msg==WM_CANCELMODE || (msg==WM_ENABLE && !wp) || (msg==WM_SHOWWINDOW && !wp)) {
        hit->tracking=false;
        hit->previewing=false;
        w_hover_clear(w);
    }
    if (hit->kind==combo_hit && !w_theme_high_contrast()) {
        if (msg==WM_PRINTCLIENT || msg==WM_PRINT || (msg==WM_PAINT && wp)) {
            int saved=SaveDC((HDC)wp);
            LRESULT result=DefSubclassProc(w,msg,wp,lp);
            if (saved) RestoreDC((HDC)wp,saved);
            if (msg!=WM_PRINT || (lp&PRF_CLIENT)) w_arrow(w,(HDC)wp);
            return result;
        }
        if (msg==WM_PAINT) {
            w_paint_frame frame;
            HDC dc=w_begin_frame(w,&frame);
            if (dc) {
                int saved=SaveDC(dc);
                DefSubclassProc(w,WM_PRINTCLIENT,(WPARAM)dc,PRF_CLIENT);
                if (saved) RestoreDC(dc,saved);
                w_arrow(w,dc);
            }
            w_end_frame(w,&frame);
            return 0;
        }
    }
    bool reconcile=w_changed_rows(hit->kind,msg);
    LRESULT result=DefSubclassProc(w,msg,wp,lp);
    if (reconcile && GetPropW(w,hit_property)==hit) {
        // A scroll can move several fading rows, including after pointer leave.
        w_hover_relayout(w,w_target_bounds,hit);
        if (hit->tracking) w_hit_at(w,hit);
    }
    return result;
}

void w_native_hover_control(HWND control) {
    if (!control || GetPropW(control,hit_property)) return;
    wchar_t name[40];
    if (!GetClassNameW(control,name,count_of(name))) return;
    enum w_hit_kind kind;
    if (!_wcsicmp(name,L"Edit") || !_wcsicmp(name,L"RICHEDIT50W")) {
        if (GetWindowLongW(control,GWL_STYLE)&WS_BORDER) w_native_edit_frame(control,NULL);
        return;
    }
    if (!_wcsicmp(name,L"ComboBox")) kind=combo_hit;
    else if (!_wcsicmp(name,L"ComboLBox")) kind=combo_list_hit;
    else if (!_wcsicmp(name,L"ListBox") && (GetWindowLongW(control,GWL_STYLE)&(LBS_OWNERDRAWFIXED|LBS_OWNERDRAWVARIABLE))) kind=listbox_hit;
    else if (!_wcsicmp(name,WC_LISTVIEWW) && (GetWindowLongW(control,GWL_STYLE)&LVS_TYPEMASK)==LVS_REPORT) kind=listview_hit;
    else if (!_wcsicmp(name,WC_HEADERW)) kind=header_hit;
    else if (!_wcsicmp(name,TRACKBAR_CLASSW)) kind=slider_hit;
    else return;
    w_native_hit *hit=calloc(1,sizeof(*hit));
    if (!hit) return;
    hit->kind=kind;
    if (kind==combo_hit) {
        hit->option_width=-1;
        COMBOBOXINFO info={.cbSize=sizeof(info)};
        if ((GetWindowLongW(control,GWL_STYLE)&3)==CBS_DROPDOWN && GetComboBoxInfo(control,&info))
            w_center_field(info.hwndItem,true);
        SetWindowSubclass(GetParent(control),w_dropdown_owner,1,0);
    }
    if (!SetPropW(control,hit_property,hit) || !SetWindowSubclass(control,w_native_hit_proc,1,(DWORD_PTR)hit)) {
        RemovePropW(control,hit_property);
        free(hit);
    }
}

w_slider_feedback w_native_slider_feedback(HWND slider) {
    return (w_slider_feedback){w_hover_amount(slider,0),w_theme_button_shadows(),w_theme_high_contrast()};
}

bool w_native_draw_combo(const DRAWITEMSTRUCT *item) {
    HWND combo=item->hwndItem;
    if (!(GetWindowLongW(combo,GWL_STYLE)&CBS_HASSTRINGS)) return false;
    bool field=(item->itemState&ODS_COMBOBOXEDIT)!=0;
    bool selected=(item->itemState&ODS_SELECTED)!=0, disabled=(item->itemState&ODS_DISABLED)!=0;
    COMBOBOXINFO info={.cbSize=sizeof(info)};
    if (!GetComboBoxInfo(combo,&info)) return false;
    w_native_hit *list=(w_native_hit *)GetPropW(info.hwndList,hit_property);
    bool preview=!field && list && list->previewing && !w_theme_high_contrast();
    if (preview) selected=item->itemID!=(UINT)-1 && (int)item->itemID==list->anchor;
    UINT dpi=GetDpiForWindow(combo);
    HDC dc=item->hDC;
    int saved=SaveDC(dc);
    if (!saved) return false;
    RECT r=item->rcItem;
    int amount=0;
    if (!disabled) {
        if (field) amount=w_hover_amount(combo,0);
        else if (item->itemID!=(UINT)-1) amount=w_hover_amount(info.hwndList,item->itemID);
    }
    // The closed selector and its chevron form one field. The committed item
    // remains highlighted in the open list underneath a separate pointer preview.
    bool native_selection=selected && (!field || w_theme_high_contrast());
    bool open=field && !disabled && SendMessageW(combo,CB_GETDROPPEDSTATE,0,0);
    COLORREF ink=w_hover_row(dc,&r,native_selection,amount,open);
    if (item->itemID!=(UINT)-1) {
        LRESULT n=SendMessageW(combo,CB_GETLBTEXTLEN,item->itemID,0);
        if (n>=0 && n<INT_MAX) {
            wchar_t local[256], *text=n<(int)count_of(local) ? local : malloc(((size_t)n+1)*sizeof(*text));
            if (text && SendMessageW(combo,CB_GETLBTEXT,item->itemID,(LPARAM)text)!=CB_ERR) {
                InflateRect(&r,-w_theme_spacing(dpi),0);
                SetBkMode(dc,TRANSPARENT);
                SetTextColor(dc,disabled ? w_theme_hint() : ink);
                w_theme_draw_control_line(combo,dc,text,&r,DT_LEFT|DT_VCENTER|DT_NOPREFIX|DT_END_ELLIPSIS);
            }
            if (text!=local) free(text);
        }
    }
    if (!preview && (!field || w_theme_high_contrast()) &&
        (item->itemState&(ODS_FOCUS|ODS_NOFOCUSRECT))==ODS_FOCUS) DrawFocusRect(dc,&item->rcItem);
    RestoreDC(dc,saved);
    return true;
}
