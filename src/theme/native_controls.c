#include "theme/native_controls.h"
#include "theme/control_hover.h"
#include "theme/fonts.h"
#include <commctrl.h>
#include <stdlib.h>
#include <uxtheme.h>
#include <wchar.h>

typedef enum { native_system = 1, native_light, native_dark, native_dark_combo } w_native_style;

bool w_native_list_row_height(HWND list,int height) {
    if (!list || height<2) return false;
    int width=0,pixels=0;
    HIMAGELIST current=ListView_GetImageList(list,LVSIL_SMALL);
    // Report rows include a physical pixel below the image rectangle.
    if (current && ImageList_GetIconSize(current,&width,&pixels) && pixels==height-1) return true;
    HIMAGELIST next=ImageList_Create(1,height-1,ILC_COLOR32,1,1);
    if (!next) return false;
    HIMAGELIST previous=ListView_SetImageList(list,next,LVSIL_SMALL);
    if (previous) ImageList_Destroy(previous);
    return true;
}

enum w_text_control { native_label, native_edit, native_combo, native_tip };
enum { refresh_text_font = WM_APP + 0x43c };
static const wchar_t font_property[]=L"winwm.native.textFont";
typedef struct {
    enum w_text_control kind;
    HFONT base, fallback;
    const wchar_t *family;
    DWORD candidate_lists;
    bool new_family, pending, composing;
} w_control_font;

typedef struct { LRESULT field, row; } w_combo_size;

bool w_native_edit_ime_notified(HWND edit) {
    w_control_font *state=(w_control_font *)GetPropW(edit,font_property);
    return state && state->kind==native_edit && (state->composing || state->candidate_lists);
}

static w_combo_size w_combo_heights(HWND w,bool combo) {
    if (!combo) return (w_combo_size){-1,-1};
    return (w_combo_size){SendMessageW(w,CB_GETITEMHEIGHT,(WPARAM)-1,0),
                          SendMessageW(w,CB_GETITEMHEIGHT,0,0)};
}

static void w_restore_heights(HWND w,w_combo_size size) {
    if (size.row>=0 && size.row!=SendMessageW(w,CB_GETITEMHEIGHT,0,0))
        SendMessageW(w,CB_SETITEMHEIGHT,0,size.row);
    if (size.field>=0 && size.field!=SendMessageW(w,CB_GETITEMHEIGHT,(WPARAM)-1,0))
        SendMessageW(w,CB_SETITEMHEIGHT,(WPARAM)-1,size.field);
}

static const wchar_t *w_control_family(HWND w,int row) {
    LRESULT size=row<0 ? GetWindowTextLengthW(w) : SendMessageW(w,CB_GETLBTEXTLEN,row,0);
    if (size<=0 || size>=INT_MAX) return NULL;
    wchar_t local[256], *text=local;
    if (size>=(int)(sizeof(local)/sizeof(*local))) {
        text=malloc(((size_t)size+1)*sizeof(*text));
        if (!text) return NULL;
    }
    LRESULT n=row<0 ? GetWindowTextW(w,text,(int)size+1) : SendMessageW(w,CB_GETLBTEXT,row,(LPARAM)text);
    const wchar_t *family=n>0 ? w_theme_text_family(text,(int)n) : NULL;
    if (text!=local) free(text);
    return family;
}

static bool w_combo_strings(HWND w) {
    LONG flags=GetWindowLongW(w,GWL_STYLE);
    return !(flags&(CBS_OWNERDRAWFIXED|CBS_OWNERDRAWVARIABLE)) || (flags&CBS_HASSTRINGS);
}

static const wchar_t *w_contents_family(HWND w,enum w_text_control kind) {
    if (kind!=native_combo) return w_control_family(w,-1);
    if (!w_combo_strings(w)) return NULL;
    int count=(int)SendMessageW(w,CB_GETCOUNT,0,0);
    for (int i=0;i<count;++i) {
        const wchar_t *family=w_control_family(w,i);
        if (family) return family;
    }
    return NULL;
}

static void w_choose_font(HWND w,w_control_font *state,const wchar_t *family) {
    if (family==state->family && !state->new_family) return;
    // A native combo can change its field and item heights on WM_SETFONT.
    // Keep the dimensions already established by the panel layout.
    w_combo_size size=w_combo_heights(w,state->kind==native_combo);
    state->family=family;
    state->new_family=true;
    SendMessageW(w,WM_SETFONT,(WPARAM)state->base,TRUE);
    w_restore_heights(w,size);
}

static void w_queue_read(HWND w,w_control_font *state) {
    if (!state->pending && !state->composing)
        state->pending=PostMessageW(w,refresh_text_font,0,0)!=FALSE;
}

static HFONT w_control_variant(w_control_font *s) {
    if (!s->base || !s->family) return NULL;
    if (s->kind==native_edit) {
        // Plain EDIT uses one face for the entire buffer. Preserve fixed-width
        // layout when the user's message font or a transcript uses a monospace face.
        HDC dc=CreateCompatibleDC(NULL);
        if (!dc) return NULL;
        HGDIOBJ old=SelectObject(dc,s->base);
        TEXTMETRICW metrics;
        bool fixed=GetTextMetricsW(dc,&metrics) && !(metrics.tmPitchAndFamily&TMPF_FIXED_PITCH);
        SelectObject(dc,old);
        DeleteDC(dc);
        if (fixed) return NULL;
    }
    return w_theme_font_for_family(s->base,s->family);
}

static void w_edit_text_message(HWND w,w_control_font *s,UINT msg,WPARAM wp,LPARAM lp) {
    if (msg==WM_IME_NOTIFY && wp==IMN_OPENCANDIDATE) s->candidate_lists|=(DWORD)lp;
    if (msg==WM_IME_NOTIFY && wp==IMN_CLOSECANDIDATE) s->candidate_lists&=~(DWORD)lp;
    if (msg==WM_KILLFOCUS || (msg==WM_IME_SETCONTEXT && !wp)) s->candidate_lists=0;
    if (msg==WM_IME_STARTCOMPOSITION) s->composing=true;
    if (msg==WM_IME_ENDCOMPOSITION || (msg==WM_KILLFOCUS && s->composing)) {
        s->composing=false;
        w_queue_read(w,s);
    }
    // Let the IME finish replacing its composition before changing edit metrics.
    if (s->composing) return;
    if (msg==EM_REPLACESEL || (msg==WM_CHAR && wp>=32)) {
        wchar_t ch=(wchar_t)wp;
        const wchar_t *added=msg==EM_REPLACESEL ? w_theme_text_family((const wchar_t *)lp,-1) :
                                                 w_theme_text_family(&ch,1);
        if (!added && !s->family) return;
        bool readonly=(GetWindowLongW(w,GWL_STYLE)&ES_READONLY)!=0;
        if (readonly && msg!=EM_REPLACESEL) return;
        DWORD start=0,end=0;
        SendMessageW(w,EM_GETSEL,(WPARAM)&start,(LPARAM)&end);
        if (readonly && start==end) {
            // Appending to a transcript only requires inspecting the inserted text.
            if (!s->family && added) w_choose_font(w,s,added);
        } else if ((added && added!=s->family) || (s->family && start!=end))
            w_queue_read(w,s);
        return;
    }
    // The edit can translate Ctrl+V, Ctrl+X and Ctrl+Z internally.
    if (msg==WM_PASTE || msg==WM_UNDO || msg==EM_UNDO || msg==WM_IME_CHAR ||
        (msg==WM_CHAR && (wp==22 || wp==26)) ||
        (s->family && (msg==WM_CUT || msg==WM_CLEAR || (msg==WM_KEYDOWN && wp==VK_DELETE) ||
                       (msg==WM_CHAR && (wp==VK_BACK || wp==24))))) w_queue_read(w,s);
}

static LRESULT CALLBACK w_control_font_proc(HWND w,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR data) {
    w_control_font *s=(w_control_font *)data;
    if (msg==WM_FONTCHANGE && s->kind==native_tip) {
        w_theme_fonts_changed();
        if (s->family) {
            s->new_family=true;
            w_choose_font(w,s,s->family);
        }
        if (IsWindowVisible(w)) SendMessageW(w,TTM_UPDATE,0,0);
    }
    if (msg==WM_NCDESTROY) {
        RemovePropW(w,font_property);
        RemoveWindowSubclass(w,w_control_font_proc,id);
        LRESULT result=DefSubclassProc(w,msg,wp,lp);
        DeleteObject(s->fallback);
        free(s);
        return result;
    }
    if (msg==WM_SETFONT) {
        HFONT base=(HFONT)wp,old=NULL;
        if (base!=s->fallback || !s->fallback) {
            if (s->new_family || base!=s->base) {
                s->new_family=false;
                old=s->fallback;
                s->base=base;
                s->fallback=w_control_variant(s);
            }
            wp=(WPARAM)(s->fallback ? s->fallback : base);
        }
        LRESULT result=DefSubclassProc(w,msg,wp,lp);
        DeleteObject(old);
        return result;
    }
    if (msg==refresh_text_font) {
        s->pending=false;
        if (!s->composing) w_choose_font(w,s,w_contents_family(w,s->kind));
        return 0;
    }
    if (s->kind==native_combo) {
        if (msg==CB_RESETCONTENT) w_choose_font(w,s,NULL);
        if (msg==CB_DELETESTRING && s->family) w_queue_read(w,s);
        if ((msg==CB_ADDSTRING || msg==CB_INSERTSTRING) && !s->family && w_combo_strings(w))
            w_choose_font(w,s,w_theme_text_family((const wchar_t *)lp,-1));
    } else if (msg==WM_SETTEXT && s->kind!=native_tip && !s->composing) {
        w_choose_font(w,s,w_theme_text_family((const wchar_t *)lp,-1));
    } else if (s->kind==native_edit) w_edit_text_message(w,s,msg,wp,lp);
    return DefSubclassProc(w,msg,wp,lp);
}

static w_control_font *w_watch_text(HWND w,enum w_text_control kind) {
    w_control_font *existing=(w_control_font *)GetPropW(w,font_property);
    if (existing) return existing;
    w_control_font *s=calloc(1,sizeof(*s));
    if (!s) return NULL;
    s->kind=kind;
    s->base=w_native_requested_font(w);
    if (!SetPropW(w,font_property,s) || !SetWindowSubclass(w,w_control_font_proc,1,(DWORD_PTR)s)) {
        RemovePropW(w,font_property);
        free(s);
        return NULL;
    }
    if (kind!=native_tip) w_choose_font(w,s,w_contents_family(w,kind));
    return s;
}

void w_native_tooltip_text(HWND tip,const wchar_t *text) {
    w_control_font *s=w_watch_text(tip,native_tip);
    if (s) w_choose_font(tip,s,w_theme_text_family(text,-1));
}

static BOOL CALLBACK w_refresh_font(HWND w,LPARAM unused) {
    (void)unused;
    w_control_font *s=(w_control_font *)GetPropW(w,font_property);
    if (s && s->family) {
        s->new_family=true;
        if (!s->composing) w_choose_font(w,s,s->family);
    }
    return TRUE;
}

void w_native_controls_font_changed(HWND parent) {
    EnumChildWindows(parent,w_refresh_font,0);
}

static void w_visual_style(HWND control, w_native_style style, bool combo) {
    static const wchar_t property[] = L"winwm.native.style";
    if (!control || (UINT_PTR)GetPropW(control, property) == (UINT_PTR)style) return;
    const wchar_t *name = style == native_system ? NULL : style == native_dark ? L"DarkMode_Explorer" :
                          style == native_dark_combo ? L"DarkMode_CFD" : L"Explorer";
    w_combo_size size=w_combo_heights(control,combo);
    if (SUCCEEDED(SetWindowTheme(control, name, NULL)))
        SetPropW(control, property, (HANDLE)(UINT_PTR)style);
    w_restore_heights(control,size);
}

static BOOL CALLBACK w_style_child(HWND control, LPARAM arg) {
    w_native_style style = (w_native_style)arg;
    wchar_t kind[48];
    if (!GetClassNameW(control, kind, 48)) return TRUE;
    w_native_hover_control(control);
    if (!_wcsicmp(kind, L"ComboBox")) {
        COMBOBOXINFO info = {.cbSize = sizeof(info)};
        w_visual_style(control, style == native_dark ? native_dark_combo : style,true);
        if (GetComboBoxInfo(control, &info)) {
            w_visual_style(info.hwndList, style,false);
            w_native_hover_control(info.hwndList);
        }
        if (w_combo_strings(control)) w_watch_text(control,native_combo);
    } else if (!_wcsicmp(kind, L"ListBox") || !_wcsicmp(kind, WC_LISTVIEWW) ||
               !_wcsicmp(kind, L"Edit") || !_wcsicmp(kind,L"RICHEDIT50W") ||
               !_wcsicmp(kind, WC_HEADERW) || !_wcsicmp(kind, UPDOWN_CLASSW)) {
        w_visual_style(control, style,false);
        if (!_wcsicmp(kind,L"Edit") && !(GetWindowLongW(control,GWL_STYLE)&ES_PASSWORD))
            w_watch_text(control,native_edit);
    } else if (!_wcsicmp(kind,L"Static")) {
        LONG type=GetWindowLongW(control,GWL_STYLE)&SS_TYPEMASK;
        if (type==SS_LEFT || type==SS_CENTER || type==SS_RIGHT || type==SS_SIMPLE || type==SS_LEFTNOWORDWRAP)
            w_watch_text(control,native_label);
    }
    return TRUE;
}

void w_native_controls_theme(HWND parent, bool contrast, COLORREF background) {
    unsigned luminance = 299u * GetRValue(background) + 587u * GetGValue(background) + 114u * GetBValue(background);
    w_native_style style = contrast ? native_system : luminance < 128000 ? native_dark : native_light;
    EnumChildWindows(parent, w_style_child, style);
}
