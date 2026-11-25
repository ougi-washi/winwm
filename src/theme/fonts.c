#include "theme/fonts.h"
#include <wchar.h>

static HFONT w_outline_font(LOGFONTW *font) {
    // Grayscale coverage works on translucent surfaces and remote displays
    // without assuming an RGB stripe order.
    font->lfOutPrecision=OUT_TT_ONLY_PRECIS;
    font->lfQuality=ANTIALIASED_QUALITY;
    return CreateFontIndirectW(font);
}

HFONT w_theme_font(const wchar_t *family, int pixel_height, int weight) {
    LOGFONTW font={.lfHeight=-max(1,pixel_height),.lfWeight=weight,.lfCharSet=DEFAULT_CHARSET};
    lstrcpynW(font.lfFaceName,family,LF_FACESIZE);
    return w_outline_font(&font);
}

// Proportions are relative to the user's Windows message font at the target
// monitor DPI. Keep the configured font family, charset and weight.
static LOGFONTW w_role_description(UINT dpi,enum w_text_role role) {
    NONCLIENTMETRICSW metrics={.cbSize=sizeof(metrics)};
    if (!dpi) dpi=USER_DEFAULT_SCREEN_DPI;
    if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS,sizeof(metrics),&metrics,0,dpi) ||
        !metrics.lfMessageFont.lfHeight) {
        metrics.lfMessageFont=(LOGFONTW){.lfHeight=-MulDiv(9,dpi,72),.lfWeight=FW_NORMAL,.lfCharSet=DEFAULT_CHARSET};
        lstrcpyW(metrics.lfMessageFont.lfFaceName,L"Segoe UI");
    }
    LOGFONTW font=metrics.lfMessageFont;
    int scale=role==text_secondary ? 12 : role==text_heading ? 15 : role==text_title ? 18 : 13;
    font.lfHeight=MulDiv(font.lfHeight,scale,12);
    font.lfWidth=MulDiv(font.lfWidth,scale,12);
    if (role==text_emphasis || role==text_heading || role==text_title) font.lfWeight=max(font.lfWeight,FW_SEMIBOLD);
    if (role==text_mono) {
        lstrcpyW(font.lfFaceName,L"Consolas");
        font.lfPitchAndFamily=FIXED_PITCH|FF_MODERN;
    }
    return font;
}

HFONT w_theme_role_font(UINT dpi,enum w_text_role role) {
    LOGFONTW font=w_role_description(dpi,role);
    return w_outline_font(&font);
}

HFONT w_theme_message_font(UINT dpi) { return w_theme_role_font(dpi,text_body); }

int w_theme_line_height(HFONT font) {
    HDC dc=GetDC(NULL);
    if (!dc) return 1;
    HGDIOBJ old=SelectObject(dc,font ? font : GetStockObject(DEFAULT_GUI_FONT));
    TEXTMETRICW tm;
    int height=GetTextMetricsW(dc,&tm) ? tm.tmHeight : 1;
    SelectObject(dc,old);
    ReleaseDC(NULL,dc);
    return height;
}

static struct { LOGFONTW font; int height; } line_cache[24];
static unsigned next_line;

int w_theme_role_line_height(UINT dpi,enum w_text_role role) {
    LOGFONTW desc=w_role_description(dpi,role);
    for (size_t i=0;i<count_of(line_cache);++i)
        if (line_cache[i].height && !memcmp(&line_cache[i].font,&desc,sizeof(desc))) return line_cache[i].height;
    LOGFONTW actual=desc;
    HFONT font=w_outline_font(&actual);
    if (!font) return max(1,abs(desc.lfHeight));
    int height=w_theme_line_height(font);
    DeleteObject(font);
    unsigned slot=next_line++%count_of(line_cache);
    line_cache[slot].font=desc;
    line_cache[slot].height=height;
    return height;
}

static SIZE w_measure(HFONT font,const wchar_t *text,int width,UINT flags) {
    SIZE size={0};
    if (!text || !*text) return size;
    HDC dc=GetDC(NULL);
    if (!dc) return size;
    HGDIOBJ old=SelectObject(dc,font ? font : GetStockObject(DEFAULT_GUI_FONT));
    RECT bounds={0,0,width,0};
    w_theme_draw_text(dc,text,-1,&bounds,DT_CALCRECT|flags);
    SelectObject(dc,old);
    ReleaseDC(NULL,dc);
    return (SIZE){bounds.right,bounds.bottom};
}

int w_theme_text_width(HFONT font,const wchar_t *text,UINT flags) {
    return w_measure(font,text,0,DT_SINGLELINE|flags).cx;
}

int w_theme_wrapped_height(HFONT font,const wchar_t *text,int width) {
    return w_measure(font,text,max(1,width),DT_WORDBREAK|DT_EDITCONTROL|DT_NOPREFIX).cy;
}

static const wchar_t japanese[]=L"Yu Gothic UI", korean[]=L"Malgun Gothic",
                     simplified[]=L"Microsoft YaHei UI", traditional[]=L"Microsoft JhengHei UI";
static const wchar_t *const families[]={japanese,korean,simplified,traditional};
static signed char available[count_of(families)];
static struct { LOGFONTW key; HFONT font; } draw_fonts[24];
static unsigned next_draw_font;

static int CALLBACK w_found_family(const LOGFONTW *font,const TEXTMETRICW *metrics,DWORD type,LPARAM found) {
    (void)font; (void)metrics;
    if (!(type&TRUETYPE_FONTTYPE)) return 1;
    *(bool *)found=true;
    return 0;
}

void w_theme_fonts_changed(void) {
    memset(line_cache,0,sizeof(line_cache));
    next_line=0;
    memset(available,0,sizeof(available));
    for (size_t i=0; i<count_of(draw_fonts); ++i) {
        if (draw_fonts[i].font) DeleteObject(draw_fonts[i].font);
        draw_fonts[i].font=NULL;
    }
    next_draw_font=0;
}

static bool w_installed_family(const wchar_t *family) {
    for (size_t i=0; i<count_of(families); ++i) {
        if (families[i]!=family && lstrcmpiW(families[i],family)) continue;
        if (!available[i]) {
            HDC dc=GetDC(NULL);
            if (!dc) return false;
            LOGFONTW request={.lfCharSet=DEFAULT_CHARSET};
            lstrcpynW(request.lfFaceName,family,LF_FACESIZE);
            bool found=false;
            EnumFontFamiliesExW(dc,&request,w_found_family,(LPARAM)&found,0);
            ReleaseDC(NULL,dc);
            available[i]=found ? 1 : -1;
        }
        return available[i]>0;
    }
    return false;
}

// Older GDI font links can select embedded bitmap strikes at small sizes.
// Resolve explicit phonetic scripts first. For Han-only text prefer the user's
// language and distinguish traditional forms when the UI language is unrelated.
const wchar_t *w_theme_text_family(const wchar_t *text,int length) {
    if (!text) return NULL;
    bool han=false;
    int n=0;
    for (; (length<0 || n<length) && text[n]; ++n) {
        unsigned c=text[n];
        if (c<0x1100) continue;
        if ((c>=0x3040 && c<=0x30ff) || (c>=0x31f0 && c<=0x31ff) || (c>=0xff66 && c<=0xff9f)) return japanese;
        if ((c>=0x1100 && c<=0x11ff) || (c>=0x3130 && c<=0x318f) || (c>=0xa960 && c<=0xa97f) ||
            (c>=0xac00 && c<=0xd7ff) || (c>=0xffa0 && c<=0xffdc)) return korean;
        if ((c>=0x3100 && c<=0x312f) || (c>=0x31a0 && c<=0x31bf)) return traditional;
        if ((c>=0x3400 && c<=0x9fff) || (c>=0xf900 && c<=0xfaff) || (c>=0xd840 && c<=0xd88d)) han=true;
    }
    if (!han) return NULL;
    LANGID language=GetUserDefaultUILanguage();
    switch (PRIMARYLANGID(language)) {
    case LANG_JAPANESE: return japanese;
    case LANG_KOREAN: return korean;
    case LANG_CHINESE:
        return SUBLANGID(language)==SUBLANG_CHINESE_SIMPLIFIED || SUBLANGID(language)==SUBLANG_CHINESE_SINGAPORE ?
               simplified : traditional;
    }
    wchar_t mapped[96];
    for (int i=0; i<n;) {
        int count=min(n-i,(int)count_of(mapped));
        int written=LCMapStringEx(L"zh-CN",LCMAP_SIMPLIFIED_CHINESE,text+i,count,mapped,count_of(mapped),NULL,NULL,0);
        if (written>0 && (written!=count || wmemcmp(mapped,text+i,count))) return traditional;
        i+=count;
    }
    return simplified;
}

static bool w_variant(LOGFONTW *font,HFONT base,const wchar_t *family) {
    if (!base || !family || GetObjectW(base,sizeof(*font),font)!=sizeof(*font) ||
        !lstrcmpiW(font->lfFaceName,family) || !w_installed_family(family)) return false;
    font->lfCharSet=DEFAULT_CHARSET;
    wmemset(font->lfFaceName,0,LF_FACESIZE);
    lstrcpynW(font->lfFaceName,family,LF_FACESIZE);
    return true;
}

HFONT w_theme_font_for_family(HFONT base,const wchar_t *family) {
    LOGFONTW font;
    return w_variant(&font,base,family) ? w_outline_font(&font) : NULL;
}

// Native controls own their replacement fonts. Only transient measure/draw
// operations use this bounded UI-thread cache, keyed by the actual font metrics.
static HFONT w_select_draw_font(HDC dc,LOGFONTW font) {
    for (size_t i=0; i<count_of(draw_fonts); ++i)
        if (draw_fonts[i].font && !memcmp(&draw_fonts[i].key,&font,sizeof(font))) return SelectObject(dc,draw_fonts[i].font);
    LOGFONTW key=font;
    HFONT created=w_outline_font(&font);
    if (!created) return NULL;
    HFONT current=GetCurrentObject(dc,OBJ_FONT);
    for (size_t i=0;i<count_of(draw_fonts);++i) {
        unsigned slot=next_draw_font++%count_of(draw_fonts);
        if (draw_fonts[slot].font && (draw_fonts[slot].font==current || !DeleteObject(draw_fonts[slot].font))) continue;
        draw_fonts[slot].key=key;
        draw_fonts[slot].font=created;
        return SelectObject(dc,created);
    }
    DeleteObject(created);
    return NULL;
}

HFONT w_theme_select_family(HDC dc,const wchar_t *family) {
    LOGFONTW font;
    return family && w_variant(&font,GetCurrentObject(dc,OBJ_FONT),family) ? w_select_draw_font(dc,font) : NULL;
}

int w_theme_draw_text(HDC dc,const wchar_t *text,int length,RECT *bounds,UINT format) {
    HFONT previous=w_theme_select_family(dc,w_theme_text_family(text,length));
    int result=DrawTextW(dc,text,length,bounds,format);
    if (previous) SelectObject(dc,previous);
    return result;
}

BOOL w_theme_text_extent(HDC dc,const wchar_t *text,int length,SIZE *size) {
    HFONT previous=w_theme_select_family(dc,w_theme_text_family(text,length));
    BOOL result=GetTextExtentPoint32W(dc,text,length,size);
    if (previous) SelectObject(dc,previous);
    return result;
}

static bool w_fit_description(HFONT base,int height,int limit,LOGFONTW *font) {
    if (limit<=0 || height<=limit || GetObjectW(base,sizeof(*font),font)!=sizeof(*font)) return false;
    // A positive LOGFONT height requests the entire cell, including internal leading.
    font->lfHeight=limit;
    if (font->lfWidth) font->lfWidth=MulDiv(font->lfWidth,limit,height);
    return true;
}

HFONT w_theme_create_fitting_font(HFONT base,int height,int limit) {
    LOGFONTW font;
    return w_fit_description(base,height,limit,&font) ? CreateFontIndirectW(&font) : NULL;
}

HFONT w_theme_select_fitting_font(HDC dc,int max_height) {
    TEXTMETRICW metrics;
    LOGFONTW font;
    if (!GetTextMetricsW(dc,&metrics) ||
        !w_fit_description(GetCurrentObject(dc,OBJ_FONT),metrics.tmHeight,max_height,&font)) return NULL;
    return w_select_draw_font(dc,font);
}

int w_theme_draw_fitted_line(HDC dc,const wchar_t *text,int length,RECT *bounds,UINT flags) {
    HFONT family=w_theme_select_family(dc,w_theme_text_family(text,length));
    HFONT previous=w_theme_select_fitting_font(dc,bounds->bottom-bounds->top);
    int result=DrawTextW(dc,text,length,bounds,flags|DT_SINGLELINE);
    if (previous) SelectObject(dc,previous);
    if (family) SelectObject(dc,family);
    return result;
}

int w_theme_draw_control_line(HWND control,HDC dc,const wchar_t *text,RECT *bounds,UINT flags) {
    // Owner-draw and WM_PRINT can supply a DC with a stock font selected.
    HFONT font=(HFONT)SendMessageW(control,WM_GETFONT,0,0);
    HGDIOBJ previous=font ? SelectObject(dc,font) : NULL;
    int height=w_theme_draw_fitted_line(dc,text,-1,bounds,flags);
    if (previous) SelectObject(dc,previous);
    return height;
}
