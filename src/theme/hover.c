#include "theme/hover.h"
#include "theme/surface.h"
#include <commctrl.h>
#include <math.h>
#include <stdlib.h>
#include <uxtheme.h>

typedef struct {
    INT_PTR key;
    RECT bounds;
    ULONGLONG start;
    w_transition timing;
    int from, value, to, full_ms;
} w_hover_item;

typedef struct w_hover_host {
    struct w_hover_host *next;
    HWND window;
    INT_PTR hot;
    UINT_PTR timer;
    bool frame;
    size_t count, capacity;
    w_hover_item *items;
} w_hover_host;

static _Thread_local w_hover_host *hosts;
static _Thread_local bool buffered;
static const wchar_t property[]=L"winwm.hover";
static void w_drop_plates(void);

static void w_damage(w_hover_host *s,const RECT *bounds) {
    if (s->frame) PostMessageW(s->window,w_hover_frame_changed,0,0);
    else InvalidateRect(s->window,bounds,FALSE);
}

static void w_stop(w_hover_host *s) {
    if (s->timer) KillTimer(s->window,s->timer);
    s->timer=0;
}

static void w_tick(w_hover_host *s,bool finish) {
    ULONGLONG now=GetTickCount64();
    bool running=false;
    for (size_t i=0; i<s->count;) {
        w_hover_item *item=&s->items[i];
        ULONGLONG elapsed=now-item->start;
        int value=item->to;
        if (!finish && item->timing.ms>0 && elapsed<(unsigned)item->timing.ms) {
            double t=w_motion_progress(item->timing,elapsed);
            value=w_clamp_int((int)(item->from+(item->to-item->from)*t+0.5),0,255);
            running=true;
        } else item->timing.ms=0;
        if (value!=item->value) {
            item->value=value;
            w_damage(s,&item->bounds);
        }
        if (!value && !item->to && !item->timing.ms) s->items[i]=s->items[--s->count];
        else ++i;
    }
    if (!running) w_stop(s);
}

static void w_aim(w_hover_item *item,int to,bool animate) {
    item->from=item->value;
    item->to=to;
    item->start=GetTickCount64();
    item->timing=w_theme_layer_timing(to!=0);
    item->full_ms=item->timing.ms;
    // A short reversal covers only the distance left from the last painted frame.
    item->timing.ms=animate ? MulDiv(item->timing.ms,abs(to-item->value),255) : 0;
}

void w_hover_preferences_changed(void) {
    if (!hosts) return;
    bool animate=w_theme_motion_enabled();
    for (w_hover_host *s=hosts; s; s=s->next) {
        if (!s->timer) continue;
        for (size_t i=0; i<s->count; ++i) {
            w_hover_item *item=&s->items[i];
            if (!item->timing.ms) continue;
            w_transition next=w_theme_layer_timing(item->to!=0);
            if (!animate || next.ms!=item->full_ms || memcmp(&next.curve,&item->timing.curve,sizeof(next.curve)))
                w_aim(item,item->to,animate);
        }
        w_tick(s,!animate);
    }
}

static LRESULT CALLBACK w_host_proc(HWND window,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR data) {
    w_hover_host *s=(w_hover_host *)data;
    if (msg==WM_TIMER && s->timer && wp==s->timer) {
        if (IsWindowVisible(window)) w_tick(s,false);
        else w_hover_clear(window);
        return 0;
    }
    if ((msg==WM_SHOWWINDOW && !wp) || (msg==WM_ENABLE && !wp) || msg==WM_CANCELMODE)
        w_hover_clear(window);
    if (msg==WM_NCDESTROY) {
        w_stop(s);
        RemoveWindowSubclass(window,w_host_proc,id);
        RemovePropW(window,property);
        w_hover_host **link=&hosts;
        while (*link && *link!=s) link=&(*link)->next;
        if (*link) *link=s->next;
        free(s->items);
        free(s);
        if (!hosts) {
            w_drop_plates();
            if (buffered) BufferedPaintUnInit();
            buffered=false;
        }
    }
    return DefSubclassProc(window,msg,wp,lp);
}

static w_hover_host *w_attach(HWND window) {
    w_hover_host *s=calloc(1,sizeof(*s));
    if (!s) return NULL;
    s->window=window;
    s->hot=-1;
    if (!SetPropW(window,property,s) || !SetWindowSubclass(window,w_host_proc,1,(DWORD_PTR)s)) {
        RemovePropW(window,property);
        free(s);
        return NULL;
    }
    if (!hosts) buffered=SUCCEEDED(BufferedPaintInit());
    s->next=hosts;
    hosts=s;
    return s;
}

static w_hover_item *w_item(w_hover_host *s,INT_PTR key) {
    for (size_t i=0; i<s->count; ++i) if (s->items[i].key==key) return &s->items[i];
    return NULL;
}

int w_hover_amount(HWND host,INT_PTR key) {
    w_hover_host *s=(w_hover_host *)GetPropW(host,property);
    w_hover_item *item=s && key>=0 ? w_item(s,key) : NULL;
    return item ? item->value : 0;
}

void w_hover_clear(HWND host) {
    w_hover_host *s=(w_hover_host *)GetPropW(host,property);
    if (!s) return;
    w_stop(s);
    for (size_t i=0; i<s->count; ++i) w_damage(s,&s->items[i].bounds);
    s->count=0;
    s->hot=-1;
}

void w_hover_clear_children(HWND parent) {
    for (w_hover_host *s=hosts; s; s=s->next)
        if (s->count && (s->window==parent || IsChild(parent,s->window))) w_hover_clear(s->window);
}

void w_hover_relayout(HWND host,bool (*locate)(HWND,INT_PTR,RECT *,void *),void *data) {
    w_hover_host *s=(w_hover_host *)GetPropW(host,property);
    if (!s) return;
    for (size_t i=0;i<s->count;) {
        w_hover_item *item=&s->items[i];
        RECT bounds;
        if (!locate(host,item->key,&bounds,data) || IsRectEmpty(&bounds)) {
            w_damage(s,&item->bounds);
            if (s->hot==item->key) s->hot=-1;
            s->items[i]=s->items[--s->count];
            continue;
        }
        if (!EqualRect(&item->bounds,&bounds)) {
            w_damage(s,&item->bounds);
            item->bounds=bounds;
            w_damage(s,&bounds);
        }
        ++i;
    }
    if (!s->count) w_stop(s);
}

static void w_target(HWND host,INT_PTR key,const RECT *bounds,bool frame) {
    if (!host) return;
    if (key<0 || !bounds || IsRectEmpty(bounds) || !IsWindowEnabled(host)) key=-1;
    w_hover_host *s=(w_hover_host *)GetPropW(host,property);
    if (!s && (key<0 || !(s=w_attach(host)))) return;
    s->frame=frame;
    w_hover_item *next=key<0 ? NULL : w_item(s,key);
    if (next && !EqualRect(&next->bounds,bounds)) {
        w_damage(s,&next->bounds);
        next->bounds=*bounds;
        w_damage(s,bounds);
    }
    if (s->hot==key) return;
    if (key>=0 && !next) {
        if (s->count==s->capacity) {
            size_t capacity=s->capacity ? s->capacity*2 : 2;
            w_hover_item *items=realloc(s->items,capacity*sizeof(*items));
            if (!items) { w_hover_clear(host); return; }
            s->items=items;
            s->capacity=capacity;
        }
        next=&s->items[s->count++];
        *next=(w_hover_item){.key=key,.bounds=*bounds};
    }
    bool animate=w_theme_motion_enabled();
    w_hover_item *previous=w_item(s,s->hot);
    if (previous) w_aim(previous,0,animate);
    if (next) w_aim(next,255,animate);
    s->hot=key;
    if (animate && !s->timer && ((previous && previous->timing.ms) || (next && next->timing.ms)))
        s->timer=SetTimer(host,(UINT_PTR)s,16,NULL);
    w_tick(s,!s->timer);
}

void w_hover_target(HWND host,INT_PTR key,const RECT *bounds) { w_target(host,key,bounds,false); }

void w_hover_frame_target(HWND field,bool hot) {
    // The nonclient outline is repainted at its current size when its amount changes.
    RECT frame={0,0,1,1};
    w_target(field,hot ? 0 : -1,&frame,true);
}

// A cached premultiplied sprite keeps glyphs out of the opacity pass. Large
// controls stretch only the uniform middle, preserving the shadow in pixels.
typedef struct {
    int width,height,radius,unit;
    COLORREF base,face;
    bool pressed,shadow;
} w_plate_key;

static _Thread_local struct {
    HDC dc;
    unsigned next;
    struct { w_plate_key key; HBITMAP bitmap; } entries[12];
} plates;

static void w_drop_plates(void) {
    for (size_t i=0;i<count_of(plates.entries);++i)
        if (plates.entries[i].bitmap) DeleteObject(plates.entries[i].bitmap);
    if (plates.dc) DeleteDC(plates.dc);
    memset(&plates,0,sizeof(plates));
}

static double w_distance(double x,double y,double half_width,double half_height,double radius) {
    double dx=fabs(x)-half_width+radius,dy=fabs(y)-half_height+radius;
    double outside_x=max(0.,dx),outside_y=max(0.,dy);
    return sqrt(outside_x*outside_x+outside_y*outside_y)+min(max(dx,dy),0.)-radius;
}

static double w_coverage(double edge) { return max(0.,min(1.,edge)); }

static HBITMAP w_raster_plate(w_plate_key k) {
    BITMAPINFO info={.bmiHeader={.biSize=sizeof(BITMAPINFOHEADER),.biWidth=k.width,.biHeight=-k.height,
                                .biPlanes=1,.biBitCount=32,.biCompression=BI_RGB}};
    DWORD *pixels=NULL;
    HBITMAP bitmap=CreateDIBSection(plates.dc,&info,DIB_RGB_COLORS,(void **)&pixels,NULL,0);
    if (!bitmap) return NULL;
    double unit=k.unit, pad=min(unit,min(k.width,k.height)/6.);
    double bottom=k.shadow ? 2.5*pad : pad;
    double half_width=(k.width-2*pad)/2.,half_height=(k.height-pad-bottom)/2.;
    double radius=min(k.radius,min(half_width,half_height));
    double center_y=pad+half_height;
    int luma=299*GetRValue(k.base)+587*GetGValue(k.base)+114*GetBValue(k.base);
    COLORREF rim=w_theme_mix(k.face,luma<128000 ? RGB(255,255,255) : RGB(0,0,0),k.pressed ? 18 : 14);
    for (int y=0;y<k.height;++y) for (int x=0;x<k.width;++x) {
        double dx=x+.5-k.width/2.,dy=y+.5-center_y;
        double distance=w_distance(dx,dy,half_width,half_height,radius);
        double face=w_coverage(.5-distance), interior=w_coverage(.5-distance-unit);
        double alpha=face;
        if (k.shadow) {
            double shadow=w_distance(dx,dy-unit,half_width,half_height,radius);
            double falloff=w_coverage(.65-shadow/(2.*unit));
            alpha+=(1.-face)*.40*falloff*falloff;
        }
        double edge=face-interior;
        DWORD red=(DWORD)(GetRValue(k.face)*interior+GetRValue(rim)*edge+.5);
        DWORD green=(DWORD)(GetGValue(k.face)*interior+GetGValue(rim)*edge+.5);
        DWORD blue=(DWORD)(GetBValue(k.face)*interior+GetBValue(rim)*edge+.5);
        pixels[y*k.width+x]=((DWORD)(255.*alpha+.5)<<24)|(red<<16)|(green<<8)|blue;
    }
    return bitmap;
}

static HBITMAP w_plate_bitmap(w_plate_key key) {
    if (!plates.dc) plates.dc=CreateCompatibleDC(NULL);
    if (!plates.dc) return NULL;
    for (size_t i=0;i<count_of(plates.entries);++i) {
        w_plate_key k=plates.entries[i].key;
        if (k.width==key.width && k.height==key.height && k.unit==key.unit && k.radius==key.radius &&
            k.base==key.base && k.face==key.face && k.shadow==key.shadow && k.pressed==key.pressed)
            return plates.entries[i].bitmap;
    }
    HBITMAP bitmap=w_raster_plate(key);
    if (!bitmap) return NULL;
    size_t i=plates.next++%count_of(plates.entries);
    if (plates.entries[i].bitmap) DeleteObject(plates.entries[i].bitmap);
    plates.entries[i].key=key;
    plates.entries[i].bitmap=bitmap;
    return bitmap;
}

typedef struct { int src,dst,src_length,dst_length; } w_span;
static int w_split(int src,int dst,w_span spans[3]) {
    if (src==dst) { spans[0]=(w_span){0,0,src,dst}; return 1; }
    int cap=src/2;
    spans[0]=(w_span){0,0,cap,cap};
    spans[1]=(w_span){cap,cap,1,dst-2*cap};
    spans[2]=(w_span){cap+1,dst-cap,cap,cap};
    return 3;
}

void w_hover_plate(HDC dc,const RECT *bounds,UINT dpi,COLORREF base,COLORREF target,int amount,bool pressed) {
    if (!dc || !bounds || IsRectEmpty(bounds) || (!pressed && amount<=0)) return;
    amount=pressed ? 255 : min(amount,255);
    int w=bounds->right-bounds->left,h=bounds->bottom-bounds->top;
    int unit=max(1,MulDiv(1,dpi ? dpi : 96,96));
    int radius=min(w_theme_control_radius(dpi),min(w,h)/2);
    int cap=max(32,radius+4*unit);
    w_plate_key key={min(w,2*cap+1),min(h,2*cap+1),radius,unit,base,target,pressed,!pressed && w_theme_button_shadows()};
    bool contrast=w_theme_high_contrast();
    HBITMAP bitmap=contrast ? NULL : w_plate_bitmap(key);
    if (bitmap) {
        HGDIOBJ previous=SelectObject(plates.dc,bitmap);
        w_span columns[3],rows[3];
        int nx=w_split(key.width,w,columns),ny=w_split(key.height,h,rows);
        BLENDFUNCTION blend={AC_SRC_OVER,0,(BYTE)amount,AC_SRC_ALPHA};
        bool ok=true;
        for (int y=0;y<ny;++y) for (int x=0;x<nx;++x) {
            w_span col=columns[x],row=rows[y];
            if (!AlphaBlend(dc,bounds->left+col.dst,bounds->top+row.dst,col.dst_length,row.dst_length,
                            plates.dc,col.src,row.src,col.src_length,row.src_length,blend)) ok=false;
        }
        SelectObject(plates.dc,previous);
        if (ok) return;
    }
    COLORREF old=SetDCBrushColor(dc,contrast ? GetSysColor(COLOR_HIGHLIGHT) : w_theme_mix(base,target,MulDiv(amount,100,255)));
    FillRect(dc,bounds,GetStockObject(DC_BRUSH));
    SetDCBrushColor(dc,old);
}

COLORREF w_hover_row(HDC dc,const RECT *bounds,bool selected,int amount,bool pressed) {
    COLORREF paper=w_theme_background(),ink=w_theme_foreground();
    if (w_theme_high_contrast()) {
        if (selected || pressed || amount>0) {
            paper=w_theme_selection(false);
            ink=w_theme_selection(true);
        }
    } else {
        COLORREF selection=w_theme_selection(false);
        if (pressed) paper=w_theme_mix(selection,ink,15);
        else if (selected) paper=w_theme_mix(selection,ink,MulDiv(w_clamp_int(amount,0,255),9,255));
        else if (amount>0) paper=w_theme_mix(paper,selection,MulDiv(min(amount,255),65,255));
    }
    COLORREF previous=SetDCBrushColor(dc,paper);
    FillRect(dc,bounds,GetStockObject(DC_BRUSH));
    SetDCBrushColor(dc,previous);
    return ink;
}
