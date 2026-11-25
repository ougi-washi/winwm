#include "bar/tray.h"
#include "bar/tooltip.h"
#include "theme/theme.h"
#include <shellapi.h>
#include <stdio.h>

static bool is_tray(const struct w_bar_hit *hit) { return !strcmp(hit->module,"tray"); }

bool w_bar_tray_bounds(const w_bar_window *b,uint64_t key,RECT *screen) {
    if (!b || !screen || !b->hwnd || !IsWindowVisible(b->hwnd) ||
        (GetWindowLongPtrW(b->hwnd,GWL_EXSTYLE)&WS_EX_TRANSPARENT)) return false;
    UpdateWindow(b->hwnd);
    int fallback=-1,index=-1;
    for (int i=0;i<b->hit_count;++i) {
        const struct w_bar_hit *hit=&b->hits[i];
        if (!is_tray(hit) || IsRectEmpty(&hit->box)) continue;
        if (key && hit->tray_key==key) { index=i;break; }
        if (!hit->tray_key && !hit->tray_more && (fallback<0 || hit->source==b->tray.source)) fallback=i;
    }
    if (!key) index=fallback;
    if (index<0) return false;
    *screen=b->hits[index].box;
    MapWindowPoints(b->hwnd,HWND_DESKTOP,(POINT *)screen,2);
    return true;
}

static void toggle(w_bar_window *b,int source,bool open) {
    if (b->tray.open==open && (!open || b->tray.source==source)) return;
    if (open && b->tray.source!=source) {
        b->tray.first=0;
        b->tray.position=0;
    }
    b->tray.source=source;
    b->tray.open=open;
    b->tray.from=b->tray.position;
    bool animate=b->motion_allowed && !b->high_contrast && b->config->motion[motion_modules].ms>0;
    b->tray.start=animate && SetTimer(b->hwnd,bar_tray_timer,16,NULL) ? GetTickCount64() : 0;
    if (!b->tray.start) {
        b->tray.position=open ? 1 : 0;
        KillTimer(b->hwnd,bar_tray_timer);
    }
    b->tray.dirty=true;
    InvalidateRect(b->hwnd,NULL,FALSE);
}

void w_bar_tray_reset(w_bar_window *b) {
    KillTimer(b->hwnd,bar_tray_timer);
    memset(&b->tray,0,sizeof(b->tray));
    b->tray.source=b->tray.suppressed=-1;
}

void w_bar_tray_hover(w_bar_window *b,int index) {
    if (index>=0 && index<b->hit_count && is_tray(&b->hits[index])) {
        int source=b->hits[index].source;
        if (source!=b->tray.suppressed && b->tray.total>0) toggle(b,source,true);
    } else {
        if (index<0 && b->pointer_inside && b->tray.open && PtInRect(&b->tray.strip,b->pointer)) return;
        b->tray.suppressed=-1;
        toggle(b,b->tray.source,false);
    }
}

static void advance(w_bar_window *b) {
    if (!b->tray.start) return;
    w_transition transition=b->config->motion[motion_modules];
    uint64_t elapsed=GetTickCount64()-b->tray.start;
    double end=b->tray.open ? 1 : 0;
    if (b->motion_allowed && !b->high_contrast && elapsed<(unsigned)max(0,transition.ms)) {
        double t=w_motion_progress(transition,elapsed);
        b->tray.position=b->tray.from+(end-b->tray.from)*max(0.0,min(1.0,t));
    } else {
        b->tray.position=end;
        b->tray.start=0;
        KillTimer(b->hwnd,bar_tray_timer);
    }
}

static int axis_start(RECT r,bool vertical) { return vertical ? r.top : r.left; }
static int axis_end(RECT r,bool vertical) { return vertical ? r.bottom : r.right; }
static void set_axis(RECT *r,bool vertical,int start,int end) {
    if (vertical) {r->top=start;r->bottom=end;}
    else {r->left=start;r->right=end;}
}

static void shift_neighbors(w_bar_window *b,int arrow,bool vertical,int direction,int edge,int limit) {
    for (int i=arrow+direction;i>=0 && i<b->hit_count;i+=direction) {
        RECT *r=&b->hits[i].box;
        int start=axis_start(*r,vertical),end=axis_end(*r,vertical);
        if ((direction>0 && start>=edge) || (direction<0 && end<=edge)) break;
        int width=end-start;
        if (direction>0) {
            start=min(edge,limit);end=min(start+width,limit);edge=end;
        } else {
            end=max(edge,limit);start=max(end-width,limit);edge=start;
        }
        set_axis(r,vertical,start,end);
    }
}

static bar_icon arrow_icon(bool vertical,int direction) {
    return vertical ? (direction>0 ? icon_down : icon_up) : (direction>0 ? icon_right : icon_left);
}

void w_bar_tray_layout(w_bar_window *b,RECT client,bool vertical,int margin) {
    SetRectEmpty(&b->tray.strip);
    int arrow=-1;
    int length=axis_end(client,vertical);
    for (int i=0;i<b->hit_count;++i) {
        struct w_bar_hit *h=&b->hits[i];
        if (!is_tray(h)) continue;
        int dir=axis_start(h->box,vertical)+axis_end(h->box,vertical)<=length ? 1 : -1;
        bool current=h->source==b->tray.source;
        h->active=current && b->tray.open && b->tray.total>0;
        if (h->icon) h->icon=arrow_icon(vertical,current && b->tray.open ? -dir : dir);
        h->empty=!b->tray.total;
        if (current) arrow=i;
    }
    if (arrow<0 || !b->tray.total || !b->tray_read) {
        if (b->tray.open || b->tray.start) {
            int total=b->tray.total;
            w_bar_tray_reset(b);
            b->tray.total=total;
        }
        return;
    }
    advance(b);
    if (b->tray.position<=0) return;
    struct w_bar_hit *trigger=&b->hits[arrow];
    RECT base=trigger->box;
    bool hide_tip=trigger->style && trigger->style->hide_tooltip;
    int left=axis_start(base,vertical)-margin;
    int right=length-margin-axis_end(base,vertical);
    int direction=right>=left ? 1 : -1;
    int room=max(left,right);
    if (room<=0) return;
    int unit=max(1,min(room,w_theme_button_size(b->dpi)));
    int slots=max(1,min(bar_tray_slots,min(room,max(unit,length/2))/unit));
    int page=min(b->tray.total,slots);
    bool more=b->tray.total>page && slots>1;
    if (more) --page;
    b->tray.page=page;
    b->tray.first=w_clamp_int(b->tray.first,0,b->tray.total-page);
    int total=page+(more ? 1 : 0);
    int reveal=(int)(total*unit*b->tray.position+.5);
    int anchor=direction>0 ? axis_end(base,vertical) : axis_start(base,vertical);
    b->tray.strip=base;
    set_axis(&b->tray.strip,vertical,direction>0 ? anchor : anchor-reveal,direction>0 ? anchor+reveal : anchor);
    shift_neighbors(b,arrow,vertical,direction,anchor+direction*reveal,direction>0 ? length-margin : margin);
    int insert=direction>0 ? arrow+1 : arrow;
    int visible=(reveal+unit-1)/unit;
    memmove(&b->hits[insert+visible],&b->hits[insert],(b->hit_count-insert)*sizeof(*b->hits));
    b->hit_count+=visible;
    for (int j=0;j<visible;++j) {
        int offset=j*unit;
        int i=direction>0 ? insert+j : insert+visible-j-1;
        struct w_bar_hit *h=&b->hits[i];
        *h=(struct w_bar_hit){.tray_slot=base,.source=b->tray.source,.tray_hide_tip=hide_tip};
        int start=direction>0 ? anchor+offset : anchor-offset-unit;
        set_axis(&h->tray_slot,vertical,start,start+unit);
        IntersectRect(&h->box,&h->tray_slot,&b->tray.strip);
        strcpy(h->module,"tray");
        if (j==page) {
            h->tray_more=true;
            h->icon=icon_more;
            strcpy(h->instance,"tray-more");
            swprintf(h->tooltip,count_of(h->tooltip),L"Next tray applications (%d–%d of %d)",
                     b->tray.first+1,b->tray.first+page,b->tray.total);
        } else {
            w_bar_tray_item item={0};
            if (!b->tray_read(b->tray.first+j,&item) || !item.key) { SetRectEmpty(&h->box);continue; }
            h->tray_key=item.key;
            h->tray_index=b->tray.first+j;
            snprintf(h->instance,sizeof(h->instance),"tray-item-%llx",(unsigned long long)item.key);
            lstrcpynW(h->tooltip,*item.tooltip ? item.tooltip : L"Application",count_of(h->tooltip));
        }
        wcscpy(h->text,h->tooltip);
    }
    int used=0;
    for (int i=0;i<b->hit_count;++i) if (!IsRectEmpty(&b->hits[i].box)) {
        if (used!=i) b->hits[used]=b->hits[i];
        ++used;
    }
    b->hit_count=used;
}

bool w_bar_tray_scroll(w_bar_window *b,int index,int steps) {
    if (index<0 || index>=b->hit_count || !is_tray(&b->hits[index])) return false;
    w_bar_tip_hide(b);
    if (!b->tray.total) return true;
    if (!b->tray.open || b->tray.source!=b->hits[index].source) toggle(b,b->hits[index].source,true);
    int last=max(0,b->tray.total-max(1,b->tray.page));
    int first=w_clamp_int(b->tray.first-steps,0,last);
    if (b->tray.first==first) return true;
    b->tray.first=first;
    b->tray.dirty=true;
    InvalidateRect(b->hwnd,NULL,FALSE);
    return true;
}

bool w_bar_tray_action(w_bar_window *b,int index,UINT event) {
    if (index<0 || index>=b->hit_count) return false;
    const struct w_bar_hit *hit=&b->hits[index];
    if (!is_tray(hit)) return false;
    w_bar_tip_hide(b);
    if (hit->tray_key) {
        w_popup_anchor anchor;
        if (b->tray_activate && w_bar_anchor(b,index,&anchor)) {
            POINT p={(anchor.button.left+anchor.button.right)/2,(anchor.button.top+anchor.button.bottom)/2};
            if (event!=NIN_KEYSELECT) {
                POINT cursor;
                if (GetCursorPos(&cursor) && PtInRect(&anchor.button,cursor)) p=cursor;
            }
            b->tray_activate(hit->tray_key,event,p);
        }
    } else if (hit->tray_more || (b->tray.open && b->tray.page==1 && b->tray.total>1 && event==NIN_KEYSELECT)) {
        int page=max(1,b->tray.page),last=max(0,b->tray.total-page);
        b->tray.first=b->tray.first>=last ? 0 : (int)min((int64_t)last,(int64_t)b->tray.first+page);
        b->tray.dirty=true;
        InvalidateRect(b->hwnd,NULL,FALSE);
    } else if (event==WM_LBUTTONUP || event==NIN_KEYSELECT) {
        bool open=!(b->tray.open && b->tray.source==hit->source);
        if (open && !b->tray.total) return true;
        b->tray.suppressed=open ? -1 : hit->source;
        toggle(b,hit->source,open);
    }
    return true;
}
