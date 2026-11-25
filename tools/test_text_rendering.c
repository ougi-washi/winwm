#include "theme/metrics.h"
#include <stdio.h>

enum { width=512, height=96 };

static bool check_sizes(UINT dpi) {
    const enum w_text_role roles[]={text_secondary,text_body,text_emphasis,text_heading,text_title,text_mono};
    int heights[count_of(roles)];
    for (size_t i=0;i<count_of(roles);++i) {
        HFONT font=w_theme_role_font(dpi,roles[i]);
        if (!font) return false;
        heights[i]=w_theme_line_height(font);
        int natural=w_theme_text_width(font,L"Name or PID",DT_NOPREFIX);
        int mnemonic=w_theme_text_width(font,L"&Name or PID",0);
        int wrapped=w_theme_wrapped_height(font,L"Name or PID",natural/2);
        int identifier=w_theme_wrapped_height(font,L"Vendor.VeryLongPackageIdentifierWithNoSpacesOrHyphens",natural);
        bool ok=natural>0 && mnemonic==natural && wrapped>heights[i] && identifier>=3*heights[i] &&
                w_theme_role_line_height(dpi,roles[i])==heights[i];
        DeleteObject(font);
        if (!ok) return false;
    }
    if (!(heights[0]<=heights[1] && heights[1]<=heights[3] && heights[3]<=heights[4])) return false;
    for (int side=button_size_min;side<=button_size_max;++side) {
        for (int gap=panel_spacing_min;gap<=panel_spacing_max;++gap) {
            w_theme_metrics_set((w_panel_metrics){side,gap});
            if (w_theme_button_size(dpi)!=MulDiv(side,dpi,96) || w_theme_spacing(dpi)!=MulDiv(gap,dpi,96) ||
                w_theme_icon_size(dpi)>=w_theme_button_size(dpi)) return false;
        }
    }
    w_theme_metrics_set((w_panel_metrics){button_size_default,panel_spacing_default});
    return true;
}

static bool check_font(HDC dc, const uint32_t *pixels, HFONT font, const wchar_t *sample) {
    if (!font) return false;
    HGDIOBJ old=SelectObject(dc,font);
    RECT area={0,0,width,height};
    FillRect(dc,&area,GetStockObject(BLACK_BRUSH));
    SIZE extent;
    RECT measured={0};
    bool ok=w_theme_text_extent(dc,sample,lstrlenW(sample),&extent) &&
            w_theme_draw_text(dc,sample,-1,&measured,DT_SINGLELINE|DT_CALCRECT|DT_NOPREFIX)>0 &&
            measured.right==extent.cx &&
            w_theme_draw_text(dc,sample,-1,&area,DT_SINGLELINE|DT_NOPREFIX)>0 &&
            GetCurrentObject(dc,OBJ_FONT)==font;
    GdiFlush();
    SelectObject(dc,old);
    DeleteObject(font);
    bool seen[256]={false};
    int shades=0;
    for (int i=0; i<width*height; ++i) {
        uint32_t color=pixels[i];
        BYTE value=GetRValue(color);
        if (GetGValue(color)!=value || GetBValue(color)!=value) return false;
        if (value && value!=255 && !seen[value]) { seen[value]=true; ++shades; }
    }
    return ok && shades>=2;
}

static bool check_script(HDC dc,const uint32_t *pixels,UINT dpi,const wchar_t *sample,const wchar_t *expected) {
    const wchar_t *family=w_theme_text_family(sample,-1);
    if (!family || lstrcmpW(family,expected)) return false;
    HFONT base=w_theme_font(L"Segoe UI",MulDiv(12,dpi,96),FW_NORMAL);
    if (!base) return false;
    // The language font is optional. Check its presence independently, so a
    // regression that always returns NULL still fails on an installed font.
    LOGFONTW request={.lfHeight=-12,.lfCharSet=DEFAULT_CHARSET};
    lstrcpynW(request.lfFaceName,family,LF_FACESIZE);
    HFONT candidate=CreateFontIndirectW(&request);
    if (!candidate) { DeleteObject(base); return false; }
    HGDIOBJ previous=SelectObject(dc,candidate);
    wchar_t actual[LF_FACESIZE]={0};
    GetTextFaceW(dc,LF_FACESIZE,actual);
    SelectObject(dc,previous);
    DeleteObject(candidate);
    if (lstrcmpiW(actual,family)) { DeleteObject(base); return true; }
    HFONT fallback=w_theme_font_for_family(base,family);
    if (!fallback) { DeleteObject(base); return false; }
    DeleteObject(fallback);
    return check_font(dc,pixels,base,sample);
}

static bool check_han(HDC dc,const uint32_t *pixels,UINT dpi) {
    LANGID language=GetUserDefaultUILanguage();
    const wchar_t *local=NULL;
    if (PRIMARYLANGID(language)==LANG_JAPANESE) local=L"Yu Gothic UI";
    if (PRIMARYLANGID(language)==LANG_KOREAN) local=L"Malgun Gothic";
    if (PRIMARYLANGID(language)==LANG_CHINESE) {
        bool simplified=SUBLANGID(language)==SUBLANG_CHINESE_SIMPLIFIED || SUBLANGID(language)==SUBLANG_CHINESE_SINGAPORE;
        local=simplified ? L"Microsoft YaHei UI" : L"Microsoft JhengHei UI";
    }
    return check_script(dc,pixels,dpi,L"简体中文",local ? local : L"Microsoft YaHei UI") &&
           check_script(dc,pixels,dpi,L"繁體中文",local ? local : L"Microsoft JhengHei UI");
}

static bool check_fitted_lines(HDC dc) {
    HFONT large=w_theme_font(L"Segoe UI",52,FW_SEMIBOLD);
    if (!large) return false;
    HGDIOBJ prior=SelectObject(dc,large);
    const wchar_t *labels[]={L"Review package",L"日本語かな"};
    bool ok=true;
    for (size_t i=0;i<count_of(labels);++i) {
        RECT original={0},fit={0,0,0,24};
        w_theme_draw_text(dc,labels[i],-1,&original,DT_CALCRECT|DT_SINGLELINE|DT_NOPREFIX);
        w_theme_draw_fitted_line(dc,labels[i],-1,&fit,DT_CALCRECT|DT_NOPREFIX);
        ok=ok && original.bottom>24 && fit.bottom>=20 && fit.bottom<=24 && fit.right>0 &&
           fit.right<original.right && GetCurrentObject(dc,OBJ_FONT)==large;
    }
    SelectObject(dc,prior);
    DeleteObject(large);
    return ok;
}

int main(void) {
    BITMAPINFO info={.bmiHeader={.biSize=sizeof(BITMAPINFOHEADER),.biWidth=width,.biHeight=-height,
                                .biPlanes=1,.biBitCount=32,.biCompression=BI_RGB}};
    HDC dc=CreateCompatibleDC(NULL);
    void *pixels=NULL;
    HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,NULL,0);
    if (!dc || !bitmap) return 1;
    HGDIOBJ old=SelectObject(dc,bitmap);
    SetTextColor(dc,RGB(255,255,255));
    SetBkMode(dc,TRANSPARENT);
    const UINT scales[]={96,120,144,192};
    bool ok=check_fitted_lines(dc);
    printf("Oversized Latin and Japanese labels fit the fixed 24px row: %s\n",ok ? "PASS" : "FAIL");
    for (size_t i=0; i<count_of(scales); ++i) {
        UINT dpi=scales[i];
        bool body=check_font(dc,pixels,w_theme_message_font(dpi),L"Network adapters 06:38");
        bool heading=check_font(dc,pixels,w_theme_role_font(dpi,text_heading),L"Display settings");
        bool mono=check_font(dc,pixels,w_theme_role_font(dpi,text_mono),L"Package output");
        bool icon=check_font(dc,pixels,w_theme_font(L"Segoe MDL2 Assets",w_theme_icon_size(dpi),FW_NORMAL),L"\uE711\uE72A");
        bool languages=!w_theme_text_family(L"Keyboard",-1) && check_han(dc,pixels,dpi) &&
                       check_script(dc,pixels,dpi,L"日本語かな",L"Yu Gothic UI") &&
                       check_script(dc,pixels,dpi,L"ㄅ繁體中文",L"Microsoft JhengHei UI") &&
                       check_script(dc,pixels,dpi,L"한국어",L"Malgun Gothic");
        printf("%u DPI text coverage: body=%d heading=%d monospace=%d icon=%d languages=%d\n",dpi,body,heading,mono,icon,languages);
        bool sizing=check_sizes(dpi);
        printf("%u DPI font roles, content measurements and 275 metric combinations: %s\n",dpi,sizing ? "PASS" : "FAIL");
        if (!body || !heading || !mono || !icon || !languages || !sizing) ok=false;
    }
    SelectObject(dc,old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    return ok ? 0 : 1;
}
