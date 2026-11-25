#include "theme/menu.h"
#include "theme/hover.h"
#include "theme/surface.h"
#include <commctrl.h>
#include <oleacc.h>
#include <shellscalingapi.h>
#include <wctype.h>

typedef struct {
    HMENU menu;
    HFONT font, icons;
    int row_height,gap,glyph_width;
    HWND popup;
    int hot;
    bool pointer;
    const w_theme_menu_item *items;
    size_t count;
    MSAAMENUINFO names[];
} w_menu;

static LRESULT CALLBACK w_menu_proc(HWND owner,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR data) {
    w_menu *m=(w_menu *)data;
    if (msg==WM_MENUSELECT && (HMENU)lp==m->menu) {
        bool pointer=(HIWORD(wp)&MF_MOUSESELECT)!=0;
        if (pointer!=m->pointer) {
            m->pointer=pointer;
            m->hot=-1;
            if (m->popup) {
                w_hover_clear(m->popup);
                InvalidateRect(m->popup,NULL,FALSE);
            }
        }
    }
    if (msg==WM_MEASUREITEM || msg==WM_DRAWITEM) {
        MEASUREITEMSTRUCT *measure=(MEASUREITEMSTRUCT *)lp;
        DRAWITEMSTRUCT *draw=(DRAWITEMSTRUCT *)lp;
        if (measure->CtlType!=ODT_MENU) return DefSubclassProc(owner,msg,wp,lp);
        ULONG_PTR name=msg==WM_MEASUREITEM ? measure->itemData : draw->itemData;
        size_t i=(name-(ULONG_PTR)m->names)/sizeof(*m->names);
        if (i>=m->count || name!=(ULONG_PTR)&m->names[i]) return DefSubclassProc(owner,msg,wp,lp);
        const w_theme_menu_item *item=&m->items[i];
        if (msg==WM_MEASUREITEM) {
            measure->itemHeight=item->id ? m->row_height : m->gap+1;
            measure->itemWidth=item->id ? w_theme_text_width(m->font,item->label,DT_NOPREFIX)+
                2*m->gap+(m->glyph_width ? m->glyph_width+m->gap : 0) : 1;
        } else {
            HDC dc=draw->hDC;
            int saved=SaveDC(dc);
            RECT r=draw->rcItem;
            bool selected=(draw->itemState & ODS_SELECTED)!=0;
            // Track the menu window that owns the row DC, including in the
            // modal menu loop. The command owner can be on another monitor.
            if (!m->popup) {
                HWND popup=WindowFromDC(dc);
                wchar_t kind[24];
                if (popup && GetClassNameW(popup,kind,count_of(kind)) && !wcscmp(kind,L"#32768")) m->popup=popup;
            }
            if (m->pointer && m->popup && item->id && (selected || m->hot==(int)i)) {
                m->hot=selected ? (int)i : -1;
                w_hover_target(m->popup,m->hot,selected ? &r : NULL);
            }
            int amount=m->pointer && m->popup ? w_hover_amount(m->popup,(INT_PTR)i) : 0;
            COLORREF ink=w_hover_row(dc,&r,selected && (!m->pointer || !m->popup),amount,false);
            r.left+=m->gap; r.right-=m->gap;
            if (item->id) {
                SetTextColor(dc,ink);
                SetBkMode(dc,TRANSPARENT);
                RECT icon=r;
                icon.right=icon.left+m->glyph_width;
                const wchar_t *glyph=item->checked ? L"\uE73E" : item->glyph;
                if (glyph && m->icons) {
                    SelectObject(dc,m->icons);
                    DrawTextW(dc,glyph,-1,&icon,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
                }
                if (m->glyph_width) r.left=icon.right+m->gap;
                SelectObject(dc,m->font ? m->font : GetStockObject(DEFAULT_GUI_FONT));
                w_theme_draw_fitted_line(dc,item->label,-1,&r,DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
            } else {
                r.top+=(r.bottom-r.top)/2; r.bottom=r.top+1;
                SetDCBrushColor(dc,w_theme_separator());
                FillRect(dc,&r,GetStockObject(DC_BRUSH));
            }
            RestoreDC(dc,saved);
        }
        return TRUE;
    }
    if (msg==WM_MENUCHAR && (HMENU)lp==m->menu) {
        size_t selected=0;
        for (size_t i=0; i<m->count; ++i)
            if (GetMenuState(m->menu,(UINT)i,MF_BYPOSITION)&MF_HILITE) selected=i+1;
        int first=-1, matches=0;
        for (size_t n=0; n<m->count; ++n) {
            size_t i=(n+selected)%m->count;
            if (m->items[i].id && towupper(*m->items[i].label)==towupper(LOWORD(wp))) {
                if (!matches) first=(int)i;
                ++matches;
            }
        }
        if (matches) return MAKELRESULT(first,matches==1 ? MNC_EXECUTE : MNC_SELECT);
    }
    if (msg==WM_NCDESTROY) RemoveWindowSubclass(owner,w_menu_proc,id);
    return DefSubclassProc(owner,msg,wp,lp);
}

UINT w_theme_track_menu(HWND owner, POINT at, UINT flags, TPMPARAMS *anchor,
                       const w_theme_menu_item *items, size_t count) {
    if (!owner || !items || !count || count>USHRT_MAX) return 0;
    w_menu *m=calloc(1,sizeof(*m)+count*sizeof(*m->names));
    if (!m) return 0;
    UINT dpi=96, vertical=96;
    GetDpiForMonitor(MonitorFromPoint(at,MONITOR_DEFAULTTONEAREST),MDT_EFFECTIVE_DPI,&dpi,&vertical);
    m->gap=w_theme_spacing(dpi);
    m->hot=-1;
    m->font=w_theme_message_font(dpi);
    m->row_height=max(w_theme_button_size(dpi),w_theme_line_height(m->font));
    for (size_t i=0;i<count;++i) {
        if (!items[i].id || (!items[i].checked && !items[i].glyph)) continue;
        m->glyph_width=w_theme_icon_size(dpi);
        m->icons=w_theme_font(L"Segoe MDL2 Assets",m->glyph_width,FW_NORMAL);
        break;
    }
    m->items=items; m->count=count; m->menu=CreatePopupMenu();
    HBRUSH brush=CreateSolidBrush(w_theme_background());
    MENUINFO info={.cbSize=sizeof(info),.fMask=MIM_STYLE|MIM_BACKGROUND,.hbrBack=brush,.dwStyle=MNS_NOCHECK};
    bool ready=m->menu && SetMenuInfo(m->menu,&info);
    for (size_t i=0; ready && i<count; ++i) {
        const w_theme_menu_item *item=&items[i];
        m->names[i]=(MSAAMENUINFO){MSAA_MENU_SIG,item->label ? (DWORD)wcslen(item->label) : 0,(LPWSTR)item->label};
        MENUITEMINFOW entry={.cbSize=sizeof(entry),.fMask=MIIM_FTYPE|MIIM_ID|MIIM_STATE|MIIM_DATA,
                             .fType=MFT_OWNERDRAW|(item->id ? 0 : MFT_SEPARATOR),.wID=item->id,
                             .fState=item->checked ? MFS_CHECKED : MFS_UNCHECKED,.dwItemData=(ULONG_PTR)&m->names[i]};
        ready=(!item->id || item->label) && InsertMenuItemW(m->menu,(UINT)i,TRUE,&entry);
    }
    UINT chosen=0;
    if (ready && SetWindowSubclass(owner,w_menu_proc,(UINT_PTR)m,(DWORD_PTR)m)) {
        if (!w_theme_motion_enabled()) flags|=TPM_NOANIMATION;
        chosen=TrackPopupMenuEx(m->menu,flags|TPM_RETURNCMD,at.x,at.y,owner,anchor);
        RemoveWindowSubclass(owner,w_menu_proc,(UINT_PTR)m);
    }
    if (m->menu) DestroyMenu(m->menu);
    DeleteObject(brush);
    DeleteObject(m->font);
    DeleteObject(m->icons);
    free(m);
    return chosen;
}
