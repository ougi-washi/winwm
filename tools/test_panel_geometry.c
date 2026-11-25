// Build WinWM first, then link this probe against the same theme publisher as the manager (see CI).
// Run: build/test_panel_geometry.exe bin/winwm.exe
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <imm.h>
#include <richedit.h>
#include <stdio.h>
#include <wchar.h>
#include "config/config.h"
#include "theme/surface.h"
#include "theme/hover.h"
#include "theme/control_hover.h"
#include "theme/native_controls.h"
#include "theme/icon_button.h"
#include "theme/disclosure.h"
#include "win_platform/percent_slider.h"
#include "win_platform/popup.h"

typedef struct {
    const wchar_t *mode, *window_class;
    int buttons, combos;
} panel_case;

// Power exposes both a selection-only and an editable dropdown, even when
// Windows has no available power plans. No test selects or applies a setting.
static const panel_case panels[] = {
    {L"date_time", L"WinwmCalendar", 1, 1},
    {L"bar_settings", L"WinwmToolbarLayout", 5, 2},
    {L"power_settings", L"WinwmSettings", 2, 2},
    {L"activity", L"WinwmActivity", 5, 0},
    {L"languages", L"WinwmInputSetup", 3, 1},
    {L"packages", L"WinwmSoftware", 0, 1},
    {L"capture_region", L"NativeCapture", 0, 0},
};
static const wchar_t config[] = L".config\\winwm\\winwm.lua";

static int slider_commits, slider_preview_value;

static LRESULT CALLBACK slider_parent(HWND w,UINT msg,WPARAM wp,LPARAM lp) {
    if (msg==WM_DRAWITEM && ((DRAWITEMSTRUCT *)lp)->CtlType==ODT_COMBOBOX)
        return w_native_draw_combo((DRAWITEMSTRUCT *)lp);
    if (msg==WM_HSCROLL) {
        if (LOWORD(wp)==TB_ENDTRACK) ++slider_commits;
        if (LOWORD(wp)==TB_THUMBTRACK) slider_preview_value=HIWORD(wp);
        return 0;
    }
    return DefWindowProcW(w,msg,wp,lp);
}

static bool check_slider(HWND host,w_slider_mode mode) {
    HWND slider=CreateWindowW(TRACKBAR_CLASSW,L"",WS_CHILD|WS_VISIBLE,0,0,120,32,host,NULL,NULL,NULL);
    if (!slider || !w_percent_slider_init(slider,mode)) return false;
    slider_commits=0;
    SendMessageW(slider,TBM_SETPOS,TRUE,0);
    SendMessageW(slider,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(60,16));
    bool ok=GetCapture()==slider && slider_preview_value==50;
    SetCapture(host);
    int expected=mode==slider_preview ? 0 : 50;
    ok=ok && slider_commits==0 && slider_preview_value==expected && SendMessageW(slider,TBM_GETPOS,0,0)==expected;
    ReleaseCapture();
    SendMessageW(slider,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(60,16));
    SendMessageW(slider,WM_LBUTTONUP,0,MAKELPARAM(110,16));
    ok=ok && !GetCapture() && slider_commits==1 && slider_preview_value==100 && SendMessageW(slider,TBM_GETPOS,0,0)==100;
    SendMessageW(slider,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(10,16));
    SendMessageW(slider,WM_CANCELMODE,0,0);
    expected=mode==slider_preview ? 100 : 0;
    ok=ok && !GetCapture() && slider_commits==1 && slider_preview_value==expected && SendMessageW(slider,TBM_GETPOS,0,0)==expected;
    if (mode==slider_preview) {
        SendMessageW(slider,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(60,16));
        ok=ok && w_percent_slider_sync(slider,30)==50 && SendMessageW(slider,TBM_GETPOS,0,0)==50;
        EnableWindow(slider,FALSE);
        ok=ok && !GetCapture() && slider_commits==1 && slider_preview_value==30 && SendMessageW(slider,TBM_GETPOS,0,0)==30;
    }
    DestroyWindow(slider);
    return ok;
}

static bool check_appearance_updates(HWND host) {
    ValidateRect(host,NULL);
    w_theme_surface_message(host,WM_SETTINGCHANGE,0,(LPARAM)L"Environment");
    w_theme_surface_message(host,WM_SETTINGCHANGE,0,(LPARAM)L"intl");
    if (GetUpdateRect(host,NULL,FALSE)) return false;
    w_theme_surface_message(host,WM_SETTINGCHANGE,0,(LPARAM)L"ImmersiveColorSet");
    if (!GetUpdateRect(host,NULL,FALSE)) return false;
    ValidateRect(host,NULL);
    w_theme_surface_message(host,WM_SETTINGCHANGE,SPI_SETHIGHCONTRAST,0);
    if (!GetUpdateRect(host,NULL,FALSE)) return false;
    HWND label=CreateWindowW(L"STATIC",L"日本語かな",WS_CHILD|WS_VISIBLE,0,0,100,20,host,NULL,NULL,NULL);
    if (!label) return false;
    ValidateRect(host,NULL);
    ValidateRect(label,NULL);
    w_theme_surface_message(host,WM_FONTCHANGE,0,0);
    bool refreshed=GetUpdateRect(host,NULL,FALSE) && GetUpdateRect(label,NULL,FALSE);
    DestroyWindow(label);
    return refreshed;
}

static void pump(DWORD ms) {
    ULONGLONG end=GetTickCount64()+ms;
    do {
        MSG message;
        while (PeekMessageW(&message,NULL,0,0,PM_REMOVE)) DispatchMessageW(&message);
        Sleep(2);
    } while (GetTickCount64()<end);
}

static bool check_hover_pixels(void) {
    BITMAPINFO info={.bmiHeader={.biSize=sizeof(BITMAPINFOHEADER),.biWidth=248,.biHeight=-36,
        .biPlanes=1,.biBitCount=32,.biCompression=BI_RGB}};
    HDC dc=CreateCompatibleDC(NULL);
    void *bits=NULL;
    HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&bits,NULL,0);
    if (!dc || !bitmap) { DeleteDC(dc); DeleteObject(bitmap); return false; }
    HGDIOBJ old=SelectObject(dc,bitmap);
    const RECT full={0,0,248,36},button={2,2,246,34};
    COLORREF base=RGB(18,20,24),target=RGB(90,120,160);
    int amounts[]={0,70,150,255,0};
    bool ok=true;
    for (size_t i=0;i<count_of(amounts);++i) {
        SetDCBrushColor(dc,base);
        FillRect(dc,&full,GetStockObject(DC_BRUSH));
        w_hover_plate(dc,&button,144,base,target,amounts[i],false);
        COLORREF expected=w_theme_high_contrast() && amounts[i] ? GetSysColor(COLOR_HIGHLIGHT) :
                          w_theme_mix(base,target,MulDiv(amounts[i],100,255));
        COLORREF got=GetPixel(dc,60,18);
        // AlphaBlend rounds premultiplied channels to the nearest representable value.
        ok=ok && abs((int)GetRValue(got)-GetRValue(expected))<=2 &&
            abs((int)GetGValue(got)-GetGValue(expected))<=2 && abs((int)GetBValue(got)-GetBValue(expected))<=2 &&
            GetPixel(dc,1,18)==base && GetPixel(dc,247,18)==base && GetPixel(dc,180,18)==got;
    }
    if (w_theme_button_shadows()) {
        // Sample below the raised face on a light surface, where a missing
        // shadow or an opaque black rectangle is particularly noticeable.
        COLORREF light=RGB(220,220,220);
        SetDCBrushColor(dc,light);
        FillRect(dc,&full,GetStockObject(DC_BRUSH));
        w_hover_plate(dc,&button,144,light,target,255,false);
        COLORREF shadow=GetPixel(dc,120,button.bottom-3);
        ok=ok && GetRValue(shadow)<220 && GetRValue(shadow)>120 &&
           GetGValue(shadow)==GetRValue(shadow) && GetBValue(shadow)==GetRValue(shadow);
    }
    COLORREF fills[4];
    for (int i=0;i<4;i++) {
        w_hover_row(dc,&button,i==2,i==1 ? 255 : 0,i==3);
        fills[i]=GetPixel(dc,120,18);
        ok=ok && fills[i]==GetPixel(dc,button.left,button.top) && fills[i]==GetPixel(dc,button.right-1,button.bottom-1);
    }
    ok=ok && fills[0]==w_theme_background() && fills[2]==w_theme_selection(false);
    if (!w_theme_high_contrast()) ok=ok && fills[0]!=fills[1] && fills[1]!=fills[2] && fills[2]!=fills[3];
    SelectObject(dc,old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    return ok;
}

static bool locate_hover(HWND host,INT_PTR key,RECT *bounds,void *data) {
    (void)host;
    if (key!=1) return false;
    *bounds=*(RECT *)data;
    return true;
}

static COLORREF dropdown_pixel(HWND list,const RECT *row) {
    RedrawWindow(list,NULL,NULL,RDW_INVALIDATE|RDW_UPDATENOW);
    HDC dc=GetDC(list);
    COLORREF color=GetPixel(dc,row->right-15,(row->top+row->bottom)/2);
    ReleaseDC(list,dc);
    return color;
}

static bool check_dropdown_hover(HWND host,w_desktop_config *profile) {
    HWND combo=CreateWindowW(L"ComboBox",L"",WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|CBS_OWNERDRAWFIXED|CBS_HASSTRINGS,
                             0,0,120,160,host,NULL,NULL,NULL);
    if (!combo) return false;
    for (int i=0;i<4;++i) SendMessageW(combo,CB_ADDSTRING,0,(LPARAM)L"Item");
    SendMessageW(combo,CB_SETITEMHEIGHT,0,24);
    SendMessageW(combo,CB_SETCURSEL,0,0);
    COMBOBOXINFO info={.cbSize=sizeof(info)};
    RECT first,next;
    bool ok=GetComboBoxInfo(combo,&info)!=FALSE;
    HWND list=info.hwndList;
    if (ok) {
        w_native_hover_control(combo);
        w_native_hover_control(list);
        SendMessageW(combo,CB_SHOWDROPDOWN,TRUE,0);
        ok=SendMessageW(list,LB_GETITEMRECT,1,(LPARAM)&first)!=LB_ERR &&
           SendMessageW(list,LB_GETITEMRECT,2,(LPARAM)&next)!=LB_ERR;
    }
    if (ok) {
        COLORREF idle=dropdown_pixel(list,&first);
        SendMessageW(list,WM_MOUSEMOVE,0,MAKELPARAM(4,first.top+4));
        bool motion=w_theme_motion_enabled();
        if (motion) {
            ok=idle!=CLR_INVALID && dropdown_pixel(list,&first)==idle;
            ULONGLONG deadline=GetTickCount64()+1500;
            // Only advance animation timers. The synthetic pointer lives on a private desktop.
            while (w_hover_amount(list,1)<40 && GetTickCount64()<deadline) {
                MSG timer;
                if (PeekMessageW(&timer,list,WM_TIMER,WM_TIMER,PM_REMOVE)) DispatchMessageW(&timer);
                else Sleep(2);
            }
            COLORREF mid=dropdown_pixel(list,&first);
            ok=ok && w_hover_amount(list,1)<255 && mid!=idle && mid!=w_theme_selection(false);
        }
        profile->animations=motion_off; w_theme_configure(profile);
        COLORREF hot=dropdown_pixel(list,&first);
        COLORREF expected=w_theme_high_contrast() ? w_theme_selection(false) : w_theme_mix(w_theme_background(),w_theme_selection(false),65);
        ok=ok && hot==expected;
        profile->animations=motion_on; w_theme_configure(profile);
        SendMessageW(list,WM_MOUSEMOVE,0,MAKELPARAM(4,next.top+4));
        if (motion) ok=ok && dropdown_pixel(list,&first)==hot && dropdown_pixel(list,&next)==idle;
        SendMessageW(combo,WM_KEYDOWN,VK_DOWN,1);
        int selected=(int)SendMessageW(list,LB_GETCURSEL,0,0);
        ok=ok && selected==3 && SendMessageW(list,LB_GETITEMRECT,selected,(LPARAM)&next)!=LB_ERR &&
           dropdown_pixel(list,&first)==idle && dropdown_pixel(list,&next)==w_theme_selection(false);
    }
    DestroyWindow(combo);
    return ok;
}

static COLORREF printed_border(HWND edit) {
    RECT r;
    if (!GetWindowRect(edit,&r)) return CLR_INVALID;
    HDC screen=GetDC(edit),dc=CreateCompatibleDC(screen);
    HBITMAP bmp=CreateCompatibleBitmap(screen,r.right-r.left,r.bottom-r.top);
    HGDIOBJ prior=SelectObject(dc,bmp);
    SendMessageW(edit,WM_PRINT,(WPARAM)dc,PRF_NONCLIENT|PRF_CLIENT);
    COLORREF edge=GetPixel(dc,0,(r.bottom-r.top)/2);
    SelectObject(dc,prior);
    DeleteObject(bmp);
    DeleteDC(dc);
    ReleaseDC(edit,screen);
    return edge;
}

static bool centered_line(HWND edit,int side,bool standalone) {
    RECT box,r;
    GetWindowRect(edit,&box);
    GetClientRect(edit,&r);
    POINT origin={0,0};
    ClientToScreen(edit,&origin);
    int width=box.right-box.left,height=box.bottom-box.top;
    int x=origin.x-box.left,top=origin.y-box.top,bottom=height-top-r.bottom;
    int line=w_theme_line_height((HFONT)SendMessageW(edit,WM_GETFONT,0,0));
    bool ok=abs(top-bottom)<=1 && r.bottom==line && (!standalone || (height==side && top>=1 && bottom>=1));
    SetWindowTextW(edit,L"HHHHHH");
    SetFocus(GetAncestor(edit,GA_ROOT));
    HDC screen=GetDC(edit),dc=CreateCompatibleDC(screen);
    HBITMAP bitmap=CreateCompatibleBitmap(screen,width,height);
    HGDIOBJ old=SelectObject(dc,bitmap);
    PatBlt(dc,0,0,width,height,BLACKNESS);
    SendMessageW(edit,WM_PRINT,(WPARAM)dc,PRF_NONCLIENT|PRF_CLIENT);
    int raster_top=height,raster_bottom=-1;
    for (int y=top;y<top+r.bottom;y++) {
        COLORREF paper=GetPixel(dc,width-x-2,y);
        for (int cx=x+2;cx<width/2;cx++) if (GetPixel(dc,cx,y)!=paper) {
            raster_top=min(raster_top,y);
            raster_bottom=y;
            break;
        }
    }
    ok=ok && raster_bottom>=raster_top && abs((height-1-raster_bottom)-raster_top)<=max(4,line/3);
    if (standalone && top>=3)
        ok=ok && GetPixel(dc,width/2,1)==w_theme_background() && GetPixel(dc,width/2,height-2)==w_theme_background();
    SelectObject(dc,old); DeleteObject(bitmap); DeleteDC(dc); ReleaseDC(edit,screen);
    if (standalone) {
        LRESULT charpos=SendMessageW(edit,EM_POSFROMCHAR,3,0);
        for (int band=0;band<2;band++) {
            POINT p={origin.x+(short)LOWORD(charpos),band ? box.bottom-1 : box.top};
            ok=ok && SendMessageW(edit,WM_NCHITTEST,0,MAKELPARAM(p.x,p.y))==HTCLIENT;
            ScreenToClient(edit,&p);
            SetFocus(GetParent(edit));
            SendMessageW(edit,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(p.x,p.y));
            SendMessageW(edit,WM_LBUTTONUP,0,MAKELPARAM(p.x,p.y));
            POINT caret;
            ok=ok && GetFocus()==edit && GetCaretPos(&caret) && caret.y>=0 && caret.y<line &&
               SendMessageW(edit,EM_GETSEL,0,0)==MAKELONG(3,3);
        }
    }
    if (!ok) fprintf(stderr,"Native field %dx%d, client y=%d/%d, line %d, raster %d..%d, selection %08lx\n",
                     width,height,top,bottom,line,raster_top,raster_bottom,(unsigned long)SendMessageW(edit,EM_GETSEL,0,0));
    return ok;
}

static bool check_centered_fields(HWND host) {
    const int sizes[]={24,32,48};
    bool ok=true;
    HFONT fonts[]={w_theme_message_font(96),w_theme_font(L"Segoe UI",64,FW_NORMAL)};
    HWND edit=w_native_field(host,0,WS_VISIBLE);
    HWND password=w_native_field(host,0,WS_VISIBLE|ES_PASSWORD);
    HWND combo=CreateWindowW(L"COMBOBOX",L"",WS_CHILD|WS_VISIBLE|CBS_DROPDOWN|CBS_OWNERDRAWFIXED|CBS_HASSTRINGS,
                             0,0,150,160,host,NULL,GetModuleHandleW(NULL),NULL);
    w_native_hover_control(combo);
    COMBOBOXINFO info={.cbSize=sizeof(info)};
    ok=edit && password && GetComboBoxInfo(combo,&info) && info.hwndItem;
    if (!ok) goto done;
    for (size_t i=0;i<sizeof(sizes)/sizeof(*sizes);i++) for (size_t f=0;f<sizeof(fonts)/sizeof(*fonts);f++) {
        HWND fields[]={edit,password};
        for (size_t j=0;j<sizeof(fields)/sizeof(*fields);j++) {
            SetWindowPos(fields[j],NULL,0,0,140,sizes[i],SWP_NOZORDER|SWP_NOACTIVATE);
            SendMessageW(fields[j],WM_SETFONT,(WPARAM)fonts[f],TRUE);
            ok=centered_line(fields[j],sizes[i],true) && ok;
            if (j) ok=ok && SendMessageW(password,EM_GETPASSWORDCHAR,0,0)!=0;
        }
        SendMessageW(combo,WM_SETFONT,(WPARAM)fonts[f],TRUE);
        SendMessageW(combo,CB_SETITEMHEIGHT,(WPARAM)-1,sizes[i]-6);
        ok=centered_line(info.hwndItem,sizes[i],false) && ok;
    }
    // Attach the script resolver after fitting. Its base must survive resizing
    // and changing script. IME composition and keyboard replacement stay native.
    SendMessageW(edit,WM_SETFONT,(WPARAM)fonts[1],TRUE);
    w_theme_native_controls(host);
    SetWindowTextW(edit,L"日本語");
    SetWindowTextW(edit,L"abcd");
    SendMessageW(edit,WM_IME_STARTCOMPOSITION,0,0);
    ok=ok && w_native_edit_ime_notified(edit);
    SendMessageW(edit,WM_IME_ENDCOMPOSITION,0,0);
    ok=ok && !w_native_edit_ime_notified(edit);
    SetFocus(edit);
    SendMessageW(edit,EM_SETSEL,1,3);
    SendMessageW(edit,WM_CHAR,'X',1);
    wchar_t text[16];
    GetWindowTextW(edit,text,16);
    ok=ok && !wcscmp(text,L"aXd") && SendMessageW(edit,EM_GETSEL,0,0)==MAKELONG(2,2);
    SetWindowPos(edit,NULL,0,0,520,150,SWP_NOZORDER|SWP_NOACTIVATE);
    ok=ok && (HFONT)SendMessageW(edit,WM_GETFONT,0,0)==fonts[1] && centered_line(edit,150,true);
done:
    DestroyWindow(edit); DestroyWindow(password); DestroyWindow(combo);
    DeleteObject(fonts[0]); DeleteObject(fonts[1]);
    return ok;
}

static bool check_edit(HWND host,w_desktop_config *profile) {
    profile->animations=motion_off;
    profile->rounding=0;
    w_theme_configure(profile);
    bool ok=w_theme_control_radius(144)==0;
    profile->rounding=16;
    w_theme_configure(profile);
    ok=ok && w_theme_control_radius(144)==3 && w_theme_radius(144)==24;
    HWND edit=CreateWindowW(L"EDIT",L"Field",WS_CHILD|WS_VISIBLE|WS_BORDER,2,2,100,26,host,NULL,NULL,NULL);
    if (!edit) return false;
    w_native_edit_frame(edit,NULL);
    SetFocus(host);
    COLORREF idle=printed_border(edit);
    SendMessageW(edit,WM_MOUSEMOVE,0,MAKELPARAM(3,3));
    COLORREF hot=printed_border(edit);
    int amount=w_hover_amount(edit,0);
    SetFocus(edit);
    SendMessageW(edit,EM_SETSEL,1,4);
    COLORREF focus=printed_border(edit);
    if (!w_theme_high_contrast()) ok=ok && idle!=CLR_INVALID && idle!=hot && hot!=focus && focus==w_theme_accent();
    ok=ok && GetFocus()==edit && SendMessageW(edit,EM_GETSEL,0,0)==MAKELONG(1,4);
    RECT frame={0,0,104,30};
    w_native_edit_frame(edit,&frame);
    SetFocus(host);
    ValidateRect(host,NULL);
    SendMessageW(edit,w_hover_frame_changed,0,0);
    HRGN region=CreateRectRgn(0,0,0,0);
    GetUpdateRgn(host,region,FALSE);
    ok=ok && PtInRegion(region,0,15) && !PtInRegion(region,50,15);
    if (!ok) fprintf(stderr,"Edit frame colors %06lx/%06lx/%06lx (accent %06lx, hover %d, style %08lx/%08lx), selection %08lx, damage %d/%d, radii %d/%d\n",
        (unsigned long)idle,(unsigned long)hot,(unsigned long)focus,(unsigned long)w_theme_accent(),
        amount,(unsigned long)GetWindowLongW(edit,GWL_STYLE),(unsigned long)GetWindowLongW(edit,GWL_EXSTYLE),
        (unsigned long)SendMessageW(edit,EM_GETSEL,0,0),PtInRegion(region,0,15),PtInRegion(region,50,15),
        w_theme_control_radius(144),w_theme_radius(144));
    DeleteObject(region);
    DestroyWindow(edit);
    return ok;
}

static bool check_hover(HWND panel) {
    HWND host=CreateWindowW(L"STATIC",L"",WS_CHILD|WS_VISIBLE,0,0,120,32,panel,NULL,NULL,NULL);
    if (!host) return false;
    static w_desktop_config profile;
    w_config_defaults(&profile);
    profile.animations=motion_on;
    profile.motion[motion_layers_in]=profile.motion[motion_layers_out]=(w_transition){1000,{0,0,1,1},style_default,-1};
    w_theme_configure(&profile);
    bool dropdown=check_dropdown_hover(panel,&profile);
    printf("Dropdown pointer fades and immediate keyboard selection: %s\n",dropdown ? "PASS" : "FAIL");
    bool edit=check_edit(panel,&profile) && check_centered_fields(panel);
    printf("Native edit hover, focus, selection, frame damage and scaled control corners: %s\n",edit ? "PASS" : "FAIL");
    profile.animations=motion_on;
    w_theme_configure(&profile);
    RECT a={0,0,32,32},b={32,0,64,32};
    w_hover_target(host,0,&a);
    bool ok=dropdown && edit;
    if (w_theme_motion_enabled()) {
        ULONGLONG until=GetTickCount64()+1500;
        while (w_hover_amount(host,0)<40 && GetTickCount64()<until) pump(2);
        int before=w_hover_amount(host,0);
        w_hover_target(host,1,&b);
        ok=ok && before>0 && before<255 && w_hover_amount(host,0)==before && w_hover_amount(host,1)==0;
        until=GetTickCount64()+1500;
        while ((!w_hover_amount(host,1) || w_hover_amount(host,0)>=before) && GetTickCount64()<until) pump(2);
        int leaving=w_hover_amount(host,0),entering=w_hover_amount(host,1);
        ok=ok && leaving<before && entering>0 && entering<255;
        w_hover_target(host,0,&a);
        ok=ok && w_hover_amount(host,0)==leaving && w_hover_amount(host,1)==entering;
    }
    profile.animations=motion_off;
    w_theme_configure(&profile);
    ok=ok && w_hover_amount(host,0)==255 && w_hover_amount(host,1)==0;
    w_hover_target(host,1,&b);
    ok=ok && w_hover_amount(host,0)==0 && w_hover_amount(host,1)==255;
    ValidateRect(host,NULL);
    w_hover_target(host,1,&b);
    ok=ok && !GetUpdateRect(host,NULL,FALSE);
    w_hover_relayout(host,locate_hover,&b);
    ok=ok && !GetUpdateRect(host,NULL,FALSE);
    RECT damage;
    w_hover_relayout(host,locate_hover,&a);
    ok=ok && GetUpdateRect(host,&damage,FALSE) && damage.left==a.left && damage.right==b.right;
    bool dark_pixels=check_hover_pixels();
    profile.background=0xf4f5f8;
    profile.foreground=0x171921;
    profile.accent=0x315e9e;
    w_theme_configure(&profile);
    bool light_pixels=check_hover_pixels();
    ok=ok && dark_pixels && light_pixels;
    printf("Light and dark row edges, press states and control shadows: %s\n",dark_pixels && light_pixels ? "PASS" : "FAIL");
    w_theme_surface_message(panel,WM_SHOWWINDOW,FALSE,0);
    ok=ok && !w_hover_amount(host,1);
    w_hover_target(host,0,&a);
    EnableWindow(host,FALSE);
    ok=ok && !w_hover_amount(host,0);
    DestroyWindow(host);
    return ok;
}

static bool check_disclosure(HWND owner) {
    static w_desktop_config profile;
    w_config_defaults(&profile);
    profile.animations=motion_on;
    profile.motion[motion_layers_in]=profile.motion[motion_layers_out]=(w_transition){1000,{0,0,1,1},style_default,-1};
    w_theme_configure(&profile);
    enum { timer=56 };
    w_disclosure row={0};
    w_disclosure_to(owner,&row,timer,true);
    if (w_theme_motion_enabled()) {
        if (!row.running) return false;
        row.start=GetTickCount64()-500;
        ULONGLONG start=row.start;
        if (!w_disclosure_tick(owner,&row,timer) || row.value<125 || row.value>136 ||
            !w_disclosure_to(owner,&row,timer,true) || row.start!=start) return false;
        int midpoint=row.value;
        if (!w_disclosure_to(owner,&row,timer,false) || row.from!=midpoint || row.timing.ms>=950) return false;
        row.start=GetTickCount64()-row.timing.ms;
        if (w_disclosure_tick(owner,&row,timer) || row.value || row.running || KillTimer(owner,timer)) return false;
        w_disclosure_to(owner,&row,timer,true);
    }
    profile.animations=motion_off;
    w_theme_configure(&profile);
    return !w_disclosure_to(owner,&row,timer,true) && row.value==255 && !row.running &&
           !w_disclosure_to(owner,&row,timer,false) && !row.value && !KillTimer(owner,timer);
}

static bool check_tip(HWND host) {
    static w_desktop_config profile;
    w_config_defaults(&profile);
    profile.animations=motion_on;
    profile.panel_material.opacity=255;
    profile.motion[motion_layers_in]=profile.motion[motion_layers_out]=(w_transition){280,{0,0,1,1},style_default,-1};
    w_theme_configure(&profile);
    HWND button=CreateWindowW(L"Button",L"Action",WS_CHILD|WS_VISIBLE,0,0,32,32,host,NULL,NULL,NULL);
    if (!button || !w_icon_button(button,L"?",L"Action")) return false;
    HWND tip=NULL;
    while ((tip=FindWindowExW(NULL,tip,TOOLTIPS_CLASSW,NULL)) && GetWindow(tip,GW_OWNER)!=host) {}
    SetFocus(button);
    ULONGLONG until=GetTickCount64()+GetDoubleClickTime()+1800;
    while (tip && !IsWindowVisible(tip) && GetTickCount64()<until) pump(4);
    bool ok=tip && IsWindowVisible(tip) && GetFocus()==button;
    bool animate=w_theme_motion_enabled();
    if (ok && animate) {
        BYTE alpha=255;
        ok=GetLayeredWindowAttributes(tip,NULL,&alpha,NULL) && alpha<255;
        pump(330);
    }
    SetFocus(host);
    if (ok && animate) {
        pump(50);
        BYTE alpha=255;
        ok=IsWindowVisible(tip) && GetLayeredWindowAttributes(tip,NULL,&alpha,NULL) && alpha>0 && alpha<255;
    }
    profile.animations=motion_off;
    w_theme_configure(&profile);
    ok=ok && !IsWindowVisible(tip);
    SetFocus(button);
    pump(GetDoubleClickTime()+100);
    ok=ok && IsWindowVisible(tip);
    SendMessageW(host,WM_CANCELMODE,0,0);
    ok=ok && !IsWindowVisible(tip);
    DestroyWindow(button);
    return ok;
}

static bool check_selector_width(HWND host);

static bool run_sliders(void) {
    INITCOMMONCONTROLSEX controls={sizeof(controls),ICC_BAR_CLASSES};
    WNDCLASSW wc={.lpfnWndProc=slider_parent,.hInstance=GetModuleHandleW(NULL),.lpszClassName=L"WinwmSliderTest"};
    if (!InitCommonControlsEx(&controls) || !RegisterClassW(&wc)) return false;
    HWND host=CreateWindowW(wc.lpszClassName,L"",WS_POPUP|WS_VISIBLE,0,0,120,32,NULL,NULL,wc.hInstance,NULL);
    bool ok=host && check_slider(host,slider_preview) && check_slider(host,slider_live);
    printf("Slider release, capture transfer and cancellation: %s\n",ok ? "PASS" : "FAIL");
    bool updates=host && check_appearance_updates(host);
    printf("Theme repaint on appearance changes: %s\n",updates ? "PASS" : "FAIL");
    bool hover=host && check_hover(host);
    printf("Hover reversal, reduced motion, idle, hide and disable: %s\n",hover ? "PASS" : "FAIL");
    bool disclosure=host && check_disclosure(host);
    printf("Disclosure reversal, stable targets, reduced motion and timer cleanup: %s\n",disclosure ? "PASS" : "FAIL");
    bool tip=host && check_tip(host);
    printf("Keyboard tooltip entry, exit, focus, reduced motion and dismissal: %s\n",tip ? "PASS" : "FAIL");
    bool selector=host && check_selector_width(host);
    printf("Native dropdown fits long labels, shrinks for replacements and stays inside the monitor: %s\n",selector ? "PASS" : "FAIL");
    if (host) DestroyWindow(host);
    UnregisterClassW(wc.lpszClassName,wc.hInstance);
    return ok && updates && hover && disclosure && tip && selector;
}

static bool check_month(HWND panel,int side) {
    HWND month=FindWindowExW(panel,NULL,L"WinwmMonth",NULL);
    RECT area;
    if (!month || !GetClientRect(month,&area)) return false;
    int buttons=0, dropdowns=0, grid_stops=0;
    for (HWND c=GetWindow(month,GW_CHILD); c; c=GetWindow(c,GW_HWNDNEXT)) {
        wchar_t kind[32];
        RECT r;
        if (!GetClassNameW(c,kind,count_of(kind)) || !GetWindowRect(c,&r)) return false;
        MapWindowPoints(NULL,month,(POINT *)&r,2);
        if (r.left<0 || r.top<0 || r.right>area.right || r.bottom>area.bottom) return false;
        if (!_wcsicmp(kind,L"Button")) {
            bool heading=GetDlgCtrlID(c)==104;
            if ((heading ? r.right-r.left<side : r.right-r.left!=side) || r.bottom-r.top!=side) return false;
            if (GetDlgCtrlID(c)>=200 && (GetWindowLongPtrW(c,GWL_STYLE)&WS_TABSTOP)) ++grid_stops;
            ++buttons;
        } else if (!_wcsicmp(kind,L"ComboBox")) {
            if (r.bottom-r.top!=side) return false;
            ++dropdowns;
        }
    }
    return buttons==45 && dropdowns==2 && grid_stops==1 && !GetDlgItem(month,105);
}

static bool wait_visible(HWND control,bool expected) {
    ULONGLONG until=GetTickCount64()+2500;
    while (!!IsWindowVisible(control)!=expected && GetTickCount64()<until) Sleep(10);
    return !!IsWindowVisible(control)==expected;
}

static bool check_month_editor(HWND window) {
    HWND month=FindWindowExW(window,NULL,L"WinwmMonth",NULL);
    HWND heading=GetDlgItem(month,104),picker=GetDlgItem(month,101),year=GetDlgItem(month,102);
    if (!heading || !year || !picker || IsWindowVisible(picker)) return false;
    RECT before,after;
    GetWindowRect(window,&before);
    PostMessageW(heading,WM_KEYDOWN,VK_RETURN,1);
    if (!wait_visible(year,true)) return false;
    COMBOBOXINFO info={.cbSize=sizeof(info)};
    if (!GetComboBoxInfo(year,&info) || !info.hwndItem) return false;
    HWND edit=info.hwndItem;
    for (int candidate=0;candidate<=1;++candidate) {
        if (candidate) SendMessageW(edit,WM_IME_NOTIFY,IMN_OPENCANDIDATE,1);
        else SendMessageW(edit,WM_IME_STARTCOMPOSITION,0,0);
        SendMessageW(edit,EM_SETSEL,0,0);
        PostMessageW(edit,WM_KEYDOWN,VK_ESCAPE,1);
        PostMessageW(edit,EM_SETSEL,0,-1);
        ULONGLONG until=GetTickCount64()+2000;
        while (!HIWORD(SendMessageW(edit,EM_GETSEL,0,0)) && GetTickCount64()<until) Sleep(10);
        bool kept_editor=IsWindowEnabled(year) && HIWORD(SendMessageW(edit,EM_GETSEL,0,0));
        if (candidate) SendMessageW(edit,WM_IME_NOTIFY,IMN_CLOSECANDIDATE,1);
        else SendMessageW(edit,WM_IME_ENDCOMPOSITION,0,0);
        if (!kept_editor) return false;
    }
    wchar_t previous[8],changed[8];
    if (SendMessageW(edit,WM_GETTEXT,count_of(previous),(LPARAM)previous)!=4) return false;
    wchar_t digit=previous[0]==L'4' ? L'5' : L'4';
    SendMessageW(edit,EM_SETSEL,0,1);
    SendMessageW(edit,WM_CHAR,digit,1);
    if (SendMessageW(edit,WM_GETTEXT,count_of(changed),(LPARAM)changed)!=4 || changed[0]!=digit ||
        wcscmp(previous+1,changed+1) || SendMessageW(edit,EM_GETSEL,0,0)!=MAKELONG(1,1) ||
        !SendMessageW(edit,EM_CANUNDO,0,0)) return false;
    PostMessageW(picker,WM_KEYDOWN,VK_RETURN,1);
    return wait_visible(year,false) && IsWindowVisible(heading) && GetWindowRect(window,&after) && EqualRect(&before,&after);
}

static bool check_zone_preview(HWND window) {
    HWND zone=FindWindowExW(window,NULL,L"ComboBox",NULL);
    HWND apply=FindWindowExW(window,NULL,L"Button",L"Apply selected time zone");
    if (!zone || !apply || IsWindowVisible(apply)) return false;
    ULONGLONG until=GetTickCount64()+4000;
    while (!IsWindowEnabled(zone) && GetTickCount64()<until) Sleep(10);
    int row=(int)SendMessageW(zone,CB_GETCURSEL,0,0);
    if (row<0 || SendMessageW(zone,CB_GETCOUNT,0,0)<2) return true;
    RECT initial,preview,restored;
    GetWindowRect(zone,&initial);
    SendMessageW(zone,CB_SETCURSEL,row ? 0 : 1,0);
    SendMessageW(window,WM_COMMAND,MAKEWPARAM(0,CBN_SELCHANGE),(LPARAM)zone);
    GetWindowRect(zone,&preview);
    bool shown=IsWindowVisible(apply) && preview.right<initial.right;
    SendMessageW(zone,CB_SETCURSEL,row,0);
    SendMessageW(window,WM_COMMAND,MAKEWPARAM(0,CBN_SELCHANGE),(LPARAM)zone);
    GetWindowRect(zone,&restored);
    return shown && !IsWindowVisible(apply) && EqualRect(&initial,&restored);
}

static bool check_children(HWND window, const panel_case *panel, int side) {
    RECT client;
    if (!GetClientRect(window, &client)) return false;
    int expected = MulDiv(side, GetDpiForWindow(window), 96);
    int buttons = 0, combos = 0;
    bool ok = true;
    if (!wcscmp(panel->mode,L"date_time") && !check_month(window,expected)) {
        fprintf(stderr,"Calendar %d DIP: month grid, navigation or dropdown geometry failed\n",side);
        return false;
    }
    HWND caption = FindWindowExW(window, NULL, L"WinwmCaption", NULL);
    if (!caption || !FindWindowExW(caption, NULL, L"Button", L"Close")) {
        fprintf(stderr, "%ls %d DIP: missing accessible close button\n", panel->mode, side);
        return false;
    }
    RECT band;
    GetWindowRect(caption, &band);
    MapWindowPoints(NULL, window, (POINT *)&band, 2);
    for (HWND action = FindWindowExW(caption, NULL, L"Button", NULL); action;
         action = FindWindowExW(caption, action, L"Button", NULL)) {
        if (!IsWindowVisible(action)) continue;
        RECT r;
        GetWindowRect(action, &r);
        if (GetDlgCtrlID(action)==2) {
            LPARAM at=MAKELPARAM((r.left+r.right)/2,(r.top+r.bottom)/2);
            if (SendMessageW(action,WM_NCHITTEST,0,at)!=HTTRANSPARENT ||
                SendMessageW(caption,WM_NCHITTEST,0,at)!=HTTRANSPARENT ||
                SendMessageW(window,WM_NCHITTEST,0,at)!=HTMAXBUTTON) {
                fprintf(stderr,"%ls %d DIP: maximize hit target did not reach Windows\n",panel->mode,side);
                ok=false;
            }
        }
        MapWindowPoints(NULL, window, (POINT *)&r, 2);
        if (r.right-r.left != expected || r.bottom-r.top != expected || r.left < band.left ||
            r.top < band.top || r.right > band.right || r.bottom > band.bottom) {
            fprintf(stderr, "%ls %d DIP: caption action outside band or incorrect size\n", panel->mode, side);
            ok = false;
        }
    }
    // Inspect only panel-owned controls. A native combo's edit and list
    // windows have their own layout and are not panel children.
    for (HWND child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        if (!IsWindowVisible(child)) continue;
        if (child == caption) continue;
        RECT r;
        wchar_t kind[64];
        if (!GetWindowRect(child, &r) || !GetClassNameW(child, kind, count_of(kind))) return false;
        MapWindowPoints(NULL, window, (POINT *)&r, 2);
        bool button = !_wcsicmp(kind, L"Button");
        bool combo = !_wcsicmp(kind, L"ComboBox");
        buttons += button;
        combos += combo;
        int width = r.right - r.left, height = r.bottom - r.top;
        if (width <= 0 || height <= 0 || r.left < 0 || r.top < band.bottom ||
            r.right > client.right || r.bottom > client.bottom ||
            ((button || combo) && height != expected) || (button && width != expected)) {
            fprintf(stderr, "%ls %d DIP: %ls id %d at (%ld,%ld) %dx%d, client %ldx%ld, expected side %d px\n",
                    panel->mode, side, kind, GetDlgCtrlID(child), r.left, r.top, width, height,
                    client.right, client.bottom, expected);
            ok = false;
        }
    }
    if (buttons != panel->buttons || combos != panel->combos) {
        fprintf(stderr, "%ls %d DIP: saw %d buttons and %d dropdowns, expected %d and %d\n",
                panel->mode, side, buttons, combos, panel->buttons, panel->combos);
        ok = false;
    }
    return ok;
}

static bool check_caption_maximize(HWND window) {
    HWND caption=FindWindowExW(window,NULL,L"WinwmCaption",NULL);
    HWND button=FindWindowExW(caption,NULL,L"Button",L"Maximize");
    DWORD thread=GetWindowThreadProcessId(window,NULL);
    GUITHREADINFO before={.cbSize=sizeof(before)},during={.cbSize=sizeof(during)};
    if (!button || !GetGUIThreadInfo(thread,&before)) return false;
    for (int press=0; press<3; ++press) {
        RECT r;
        if (!GetWindowRect(button,&r)) return false;
        LPARAM at=MAKELPARAM((r.left+r.right)/2,(r.top+r.bottom)/2);
        if (SendMessageW(window,WM_NCHITTEST,0,at)!=HTMAXBUTTON) {
            fprintf(stderr,"Maximize action %d lost its native hit region\n",press);
            return false;
        }
        SendMessageW(window,WM_NCLBUTTONDOWN,HTMAXBUTTON,at);
        if (!GetGUIThreadInfo(thread,&during) || during.hwndCapture!=button || during.hwndFocus!=before.hwndFocus) {
            fprintf(stderr,"Maximize action %d lost capture or changed focus\n",press);
            return false;
        }
        // Releasing outside cancels. The next two presses maximize and restore.
        LPARAM release=press ? MAKELPARAM(2,2) : MAKELPARAM(-2,-2);
        SendMessageW(button,WM_MOUSEMOVE,MK_LBUTTON,release);
        SendMessageW(button,WM_LBUTTONUP,0,release);
        bool zoom=press==1;
        ULONGLONG until=GetTickCount64()+2000;
        while (!!IsZoomed(window)!=zoom && GetTickCount64()<until) Sleep(10);
        if (!!IsZoomed(window)!=zoom) {
            fprintf(stderr,"Maximize action %d did not settle at its requested state\n",press);
            return false;
        }
    }
    return true;
}

static bool toolbar_reload(HWND window, const panel_case *panel) {
    MINMAXINFO previous={0};
    DWORD_PTR result;
    if (!SendMessageTimeoutW(window,WM_GETMINMAXINFO,0,(LPARAM)&previous,SMTO_ABORTIFHUNG,3000,&result)) return false;
    MONITORINFO mi={.cbSize=sizeof(mi)};
    if (!GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&mi)) return false;
    int width=previous.ptMinTrackSize.x, height=previous.ptMinTrackSize.y;
    if (!SetWindowPos(window,NULL,mi.rcWork.right-width,mi.rcWork.bottom-height,width,height,
                      SWP_NOZORDER|SWP_NOACTIVATE)) return false;
    // Publish through the manager's real cross-process API on this test desktop.
    WNDCLASSW wc={.lpfnWndProc=DefWindowProcW,.hInstance=GetModuleHandleW(NULL),.lpszClassName=L"WinwmEvents"};
    if (!RegisterClassW(&wc)) return false;
    HWND publisher=CreateWindowW(wc.lpszClassName,L"",WS_POPUP,0,0,1,1,NULL,NULL,wc.hInstance,NULL);
    if (!publisher) return false;
    static w_desktop_config profile;
    profile.background=0x101116; profile.foreground=0xe4e5ea; profile.accent=0x91aaf7;
    profile.panel_material.opacity=255;
    profile.panel_metrics=(w_panel_metrics){button_size_max,panel_spacing_max};
    w_theme_configure(&profile);
    w_theme_publish(publisher);
    ULONGLONG deadline=GetTickCount64()+3000;
    RECT current={0};
    while (GetWindowRect(window,&current) && current.bottom-current.top<=height && GetTickCount64()<deadline)
        Sleep(10);
    MINMAXINFO next={0};
    bool ok=SendMessageTimeoutW(window,WM_GETMINMAXINFO,0,(LPARAM)&next,SMTO_ABORTIFHUNG,3000,&result) &&
            current.bottom-current.top>height && current.bottom-current.top>=next.ptMinTrackSize.y &&
            current.right-current.left>=next.ptMinTrackSize.x &&
            current.left>=mi.rcWork.left && current.top>=mi.rcWork.top &&
            current.right<=mi.rcWork.right && current.bottom<=mi.rcWork.bottom &&
            check_children(window,panel,button_size_max);
    printf("Live toolbar reload: %d to %ld px high, %s\n",height,current.bottom-current.top,ok ? "PASS" : "FAIL");
    DestroyWindow(publisher);
    UnregisterClassW(wc.lpszClassName,wc.hInstance);
    return ok;
}

static bool check_language_browsing(HWND panel) {
    HWND combo=GetDlgItem(panel,101), search=GetDlgItem(panel,102);
    const wchar_t query[]=L"en";
    wchar_t shown[8];
    if (!combo || !search || !SendMessageW(search,WM_SETTEXT,0,(LPARAM)query)) return false;
    RECT box,field;
    COMBOBOXINFO layout={.cbSize=sizeof(layout)};
    if (!GetWindowRect(combo,&box) || !GetWindowRect(search,&field) || !GetComboBoxInfo(combo,&layout)) return false;
    HFONT font=w_theme_message_font(GetDpiForWindow(combo));
    int widest=w_theme_text_width(font,L"Keyboard layouts",DT_NOPREFIX);
    DeleteObject(font);
    if (layout.rcItem.right-layout.rcItem.left-2*(field.left-box.right)<widest) return false;
    int original=(int)SendMessageW(combo,CB_GETCURSEL,0,0);
    SendMessageW(combo,CB_SHOWDROPDOWN,TRUE,0);
    SendMessageW(combo,WM_KEYDOWN,VK_DOWN,1);
    bool ok=SendMessageW(combo,CB_GETDROPPEDSTATE,0,0) && SendMessageW(combo,CB_GETCURSEL,0,0)!=original &&
            SendMessageW(search,WM_GETTEXT,count_of(shown),(LPARAM)shown)==2 && !wcscmp(shown,query);
    PostMessageW(combo,WM_KEYDOWN,VK_ESCAPE,1);
    ULONGLONG deadline=GetTickCount64()+2000;
    while (SendMessageW(combo,CB_GETDROPPEDSTATE,0,0) && GetTickCount64()<deadline) Sleep(10);
    if (!ok || !IsWindow(panel) || SendMessageW(combo,CB_GETDROPPEDSTATE,0,0) ||
        SendMessageW(combo,CB_GETCURSEL,0,0)!=original ||
        SendMessageW(search,WM_GETTEXT,count_of(shown),(LPARAM)shown)!=2 || wcscmp(shown,query)) return false;
    HWND list=GetDlgItem(panel,103);
    int last=(int)SendMessageW(list,LB_GETCOUNT,0,0)-1;
    if (last>=0) {
        SendMessageW(list,LB_SETCURSEL,last,0);
        LRESULT identity=SendMessageW(list,LB_GETITEMDATA,last,0);
        SendMessageW(search,WM_SETTEXT,0,(LPARAM)L"");
        int row=(int)SendMessageW(list,LB_GETCURSEL,0,0),top=(int)SendMessageW(list,LB_GETTOPINDEX,0,0);
        RECT client;
        GetClientRect(list,&client);
        int height=(int)SendMessageW(list,LB_GETITEMHEIGHT,0,0);
        if (row<top || (row-top+1)*height>client.bottom || SendMessageW(list,LB_GETITEMDATA,row,0)!=identity) return false;
    }
    HWND typing=GetDlgItem(panel,108);
    if (!typing || IsWindowVisible(typing)) return false;
    const int pages[]={2,0,2};
    for (size_t i=0;i<count_of(pages);++i) {
        SendMessageW(combo,CB_SETCURSEL,pages[i],0);
        SendMessageW(panel,WM_COMMAND,MAKEWPARAM(101,CBN_SELCHANGE),(LPARAM)combo);
        if (!wait_visible(typing,pages[i]==2)) return false;
        if (!i) SendMessageW(typing,WM_SETTEXT,0,(LPARAM)L"かな");
    }
    return IsWindowEnabled(typing) && SendMessageW(typing,WM_GETTEXT,count_of(shown),(LPARAM)shown)==2 &&
           !wcscmp(shown,L"かな");
}

static bool check_toolbar_enter(HWND panel) {
    HWND list=GetDlgItem(panel,102),available=GetDlgItem(panel,107),save=GetDlgItem(panel,110);
    int count=(int)SendMessageW(list,LB_GETCOUNT,0,0);
    if (count<0 || !available || IsWindowEnabled(save)) return false;
    PostMessageW(available,WM_KEYDOWN,VK_RETURN,1);
    PostMessageW(available,WM_KEYDOWN,VK_RETURN,1L<<30);
    ULONGLONG until=GetTickCount64()+2000;
    while (!IsWindowEnabled(save) && GetTickCount64()<until) Sleep(10);
    if (SendMessageW(list,LB_GETCOUNT,0,0)!=count+1) return false;
    PostMessageW(list,WM_KEYDOWN,VK_DELETE,1);
    until=GetTickCount64()+2000;
    while (IsWindowEnabled(save) && GetTickCount64()<until) Sleep(10);
    return SendMessageW(list,LB_GETCOUNT,0,0)==count && !IsWindowEnabled(save);
}

static bool check_activity_empty(HWND panel) {
    HWND search=GetDlgItem(panel,101), table=GetDlgItem(panel,103), empty=GetDlgItem(panel,104);
    RECT label,field,report,actions,area;
    GetWindowRect(GetDlgItem(panel,110),&label);
    GetWindowRect(search,&field);
    GetWindowRect(table,&report);
    GetWindowRect(GetDlgItem(panel,106),&actions);
    GetClientRect(table,&area);
    UINT dpi=GetDpiForWindow(panel);
    HFONT font=w_theme_message_font(dpi);
    int measured=w_theme_text_width(font,L"&Name or PID",0);
    DeleteObject(font);
    int total=0;
    for (int col=0;col<4;++col) total+=ListView_GetColumnWidth(table,col);
    if (label.right-label.left!=measured || field.left<=label.right || IsWindowVisible(GetDlgItem(panel,105)) ||
        actions.top-report.bottom!=field.left-label.right || total!=area.right-area.left) return false;
    MINMAXINFO limits={0};
    SendMessageW(panel,WM_GETMINMAXINFO,0,(LPARAM)&limits);
    if (!SetWindowPos(panel,NULL,0,0,limits.ptMinTrackSize.x,limits.ptMinTrackSize.y,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE) ||
        !SendMessageW(search,WM_SETTEXT,0,(LPARAM)L"WinWM process name with no matches") ||
        !IsWindowVisible(empty) || ListView_GetItemCount(table)) return false;
    // A user can widen a column before the final process disappears.
    if (!ListView_SetColumnWidth(table,0,limits.ptMinTrackSize.x*2) ||
        !(GetWindowLongW(table,GWL_STYLE)&WS_HSCROLL)) return false;
    RECT header, body, message;
    if (!GetWindowRect(ListView_GetHeader(table),&header) || !GetClientRect(table,&body) ||
        !GetWindowRect(empty,&message)) return false;
    MapWindowPoints(table,NULL,(POINT *)&body,2);
    POINT center={(message.left+message.right)/2,(message.top+message.bottom)/2};
    ScreenToClient(panel,&center);
    return !IsRectEmpty(&message) && message.left>=body.left && message.right<=body.right &&
           message.top>=header.bottom && message.bottom<=body.bottom &&
           ChildWindowFromPointEx(panel,center,CWP_SKIPINVISIBLE)==empty;
}

static bool check_selector_width(HWND host) {
    const wchar_t *long_choice=L"An external display on an extended monitor with a long localized device name";
    HWND combo=CreateWindowW(L"COMBOBOX",L"",WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|CBS_OWNERDRAWFIXED|CBS_HASSTRINGS,
                             0,0,76,160,host,NULL,NULL,NULL);
    if (!combo) return false;
    w_native_hover_control(combo);
    UINT dpi=GetDpiForWindow(combo);
    HFONT font=w_theme_message_font(dpi);
    SendMessageW(combo,WM_SETFONT,(WPARAM)font,FALSE);
    COMBOBOXINFO info={.cbSize=sizeof(info)};
    MONITORINFO monitor={.cbSize=sizeof(monitor)};
    RECT field,menu;
    bool ok=GetWindowRect(combo,&field) && GetComboBoxInfo(combo,&info) &&
            GetMonitorInfoW(MonitorFromWindow(host,MONITOR_DEFAULTTONEAREST),&monitor);
    int width=field.right-field.left;
    int ink=w_theme_text_width(font,long_choice,DT_NOPREFIX);
    SendMessageW(combo,CB_ADDSTRING,0,(LPARAM)long_choice);
    SendMessageW(combo,CB_SHOWDROPDOWN,TRUE,0);
    ok=ok && IsWindowVisible(info.hwndList) && GetWindowRect(info.hwndList,&menu) && menu.right-menu.left>=ink;
    SendMessageW(combo,CB_SHOWDROPDOWN,FALSE,0);
    SendMessageW(combo,CB_RESETCONTENT,0,0);
    SendMessageW(combo,CB_ADDSTRING,0,(LPARAM)L"A");
    SetFocus(combo);
    SendMessageW(combo,WM_KEYDOWN,VK_F4,1);
    ok=ok && IsWindowVisible(info.hwndList) && GetWindowRect(info.hwndList,&menu) && menu.right-menu.left==width;
    SendMessageW(combo,CB_SHOWDROPDOWN,FALSE,0);
    wchar_t wide[2048];
    wmemset(wide,L'W',count_of(wide)-1);
    wide[count_of(wide)-1]=0;
    SendMessageW(combo,CB_INSERTSTRING,0,(LPARAM)wide);
    GetComboBoxInfo(combo,&info);
    LPARAM arrow=MAKELPARAM((info.rcButton.left+info.rcButton.right)/2,(info.rcButton.top+info.rcButton.bottom)/2);
    SendMessageW(combo,WM_LBUTTONDOWN,MK_LBUTTON,arrow);
    ok=ok && IsWindowVisible(info.hwndList) && GetWindowRect(info.hwndList,&menu) &&
       menu.left>=monitor.rcWork.left && menu.right<=monitor.rcWork.right && menu.right-menu.left>width;
    SendMessageW(combo,WM_LBUTTONUP,0,arrow);
    DestroyWindow(combo);
    DeleteObject(font);
    return ok;
}

static bool check_package_transcript(HWND panel) {
    HWND output=GetDlgItem(panel,106);
    HWND rows=GetDlgItem(panel,103);
    RECT search,status;
    if (SendMessageW(rows,LB_GETCOUNT,0,0)!=0 || IsWindowVisible(rows) ||
        !GetWindowRect(GetDlgItem(panel,101),&search) || !GetWindowRect(GetDlgItem(panel,104),&status) ||
        status.top<search.bottom || status.top-search.bottom>search.bottom-search.top) return false;
    wchar_t kind[32],text[64];
    const wchar_t sample[]=L"iiii WWWW 日本語かな\r\n1234";
    if (!GetClassNameW(output,kind,count_of(kind)) || _wcsicmp(kind,MSFTEDIT_CLASS)) return false;
    if (!(SendMessageW(output,EM_GETTEXTMODE,0,0)&TM_PLAINTEXT) ||
        !(GetWindowLongW(output,GWL_STYLE)&ES_READONLY) ||
        !SendMessageW(output,WM_SETTEXT,0,(LPARAM)sample) || SendMessageW(output,EM_GETLINECOUNT,0,0)!=2) return false;
    SendMessageW(output,EM_SETSEL,0,4);
    SendMessageW(output,WM_CHAR,L'x',1);
    SendMessageW(output,WM_GETTEXT,count_of(text),(LPARAM)text);
    // Rich Edit can normalize the line separator when retrieving plain text.
    return !wcscmp(text,sample) || !wcscmp(text,L"iiii WWWW 日本語かな\r1234");
}

static bool run_panel(const wchar_t *exe, wchar_t *desktop_name, const panel_case *panel, int side) {
    wchar_t command[128];
    swprintf(command, count_of(command), L"winwm.exe --tool-%ls", panel->mode);
    STARTUPINFOW startup = {.cb = sizeof(startup), .lpDesktop = desktop_name};
    PROCESS_INFORMATION process = {0};
    if (!CreateProcessW(exe, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process)) {
        fprintf(stderr, "CreateProcess for %ls failed (%lu)\n", panel->mode, GetLastError());
        return false;
    }
    CloseHandle(process.hThread);
    HWND window = NULL;
    ULONGLONG deadline = GetTickCount64() + 10000;
    do {
        DWORD pid = 0;
        window = FindWindowW(panel->window_class, NULL);
        GetWindowThreadProcessId(window, &pid);
        if (pid == process.dwProcessId && IsWindowVisible(window)) break;
        window = NULL;
    } while (WaitForSingleObject(process.hProcess, 25) == WAIT_TIMEOUT && GetTickCount64() < deadline);

    bool ok = false;
    if (window && SendMessageTimeoutW(window, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 3000, NULL)) {
        if (!wcscmp(panel->mode,L"capture_region")) {
            BYTE alpha=0;
            DWORD flags=0;
            COLORREF clear=0;
            UINT cancel=side==button_size_min ? WM_KILLFOCUS : side==button_size_max ? WM_DISPLAYCHANGE : WM_CANCELMODE;
            ok=GetLayeredWindowAttributes(window,&clear,&alpha,&flags) && (flags&(LWA_ALPHA|LWA_COLORKEY))==(LWA_ALPHA|LWA_COLORKEY) &&
               alpha>0 && alpha<255 && clear!=RGB(0,0,0) &&
               PostMessageW(window,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(100,100)) &&
               PostMessageW(window,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(70,60)) && PostMessageW(window,cancel,0,0);
        } else if (!wcscmp(panel->mode,L"date_time")) {
            ok=check_children(window,panel,side) && check_month_editor(window) && check_zone_preview(window);
            HWND caption = FindWindowExW(window,NULL,L"WinwmCaption",NULL);
            HWND close = caption ? FindWindowExW(caption,NULL,L"Button",L"Close") : NULL;
            if (!close || !PostMessageW(close,BM_CLICK,0,0)) ok = false;
        } else {
            ok=check_children(window,panel,side);
            if (ok && !wcscmp(panel->mode,L"bar_settings") && !check_toolbar_enter(window)) {
                fprintf(stderr,"Toolbar Enter and Delete did not restore the original list at %d DIP\n",side);
                ok=false;
            }
            if (ok && side==button_size_default && !wcscmp(panel->mode,L"bar_settings")) ok=check_caption_maximize(window);
            if (ok && side==button_size_min && !wcscmp(panel->mode,L"bar_settings")) ok=toolbar_reload(window,panel);
            if (ok && !wcscmp(panel->mode,L"languages")) ok=check_language_browsing(window);
            if (ok && !wcscmp(panel->mode,L"activity")) ok=check_activity_empty(window);
            if (ok && !wcscmp(panel->mode,L"packages")) ok=check_package_transcript(window);
            if (!PostMessageW(window,WM_KEYDOWN,VK_ESCAPE,1)) ok=false;
        }
    } else {
        fprintf(stderr, "%ls %d DIP: no responsive panel\n", panel->mode, side);
    }
    DWORD exit_code = STILL_ACTIVE;
    if (WaitForSingleObject(process.hProcess, 5000) != WAIT_OBJECT_0) {
        fprintf(stderr, "%ls %d DIP: timed out waiting for dismissal\n", panel->mode, side);
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 3000);
        ok = false;
    } else if (!GetExitCodeProcess(process.hProcess, &exit_code) || exit_code != 0) {
        fprintf(stderr, "%ls %d DIP: process exit %lu\n", panel->mode, side, exit_code);
        ok = false;
    }
    CloseHandle(process.hProcess);
    printf("%ls %d DIP: %s\n", panel->mode, side, ok ? "PASS" : "FAIL");
    return ok;
}

static bool dock_source(w_popup_anchor *source, RECT area, bool bottom, const wchar_t *key) {
    int size=MulDiv(button_size_default,GetDpiForWindow(source->bar),96);
    int y=bottom ? area.bottom-size : area.top;
    if (!SetWindowPos(source->bar,NULL,area.left,y,area.right-area.left,size,
                      SWP_SHOWWINDOW|SWP_NOACTIVATE|SWP_NOZORDER)) return false;
    int x=bottom ? area.right-2*size : area.left+size;
    source->button=(RECT){x,y,x+size,y+size};
    strcpy(source->edge,bottom ? "bottom" : "top");
    return w_popup_publish(source,key);
}

static HWND await_anchor(const PROCESS_INFORMATION *process, const w_popup_anchor *source, RECT area, int spacing) {
    ULONGLONG deadline=GetTickCount64()+10000;
    do {
        HWND window=GetPropW(source->bar,w_popup_child_property);
        RECT r;
        DWORD pid=0;
        if (window && GetWindowThreadProcessId(window,&pid) && pid==process->dwProcessId &&
            IsWindowVisible(window) && GetWindowRect(window,&r)) {
            int gap=MulDiv(spacing,GetDpiForWindow(source->bar),96);
            RECT expected=w_popup_bounds(source,area,r.right-r.left,r.bottom-r.top,gap);
            if (EqualRect(&r,&expected)) return window;
        }
    } while (GetTickCount64()<deadline && WaitForSingleObject(process->hProcess,25)==WAIT_TIMEOUT);
    return NULL;
}

static bool check_wrapped_feedback(HWND window,HWND status,HWND list);

static bool check_package_dock(HWND window) {
    HWND pages=GetDlgItem(window,100),query=GetDlgItem(window,101),list=GetDlgItem(window,103),output=GetDlgItem(window,106);
    RECT initial,search,status,client,viewport;
    if (!GetClientRect(window,&client) || !GetWindowRect(window,&initial) || !GetWindowRect(query,&search) ||
        !GetWindowRect(GetDlgItem(window,104),&status) || IsWindowVisible(list)) return false;
    MapWindowPoints(NULL,window,(POINT *)&search,2);
    MapWindowPoints(NULL,window,(POINT *)&status,2);
    int side=search.bottom-search.top,gap=status.top-search.bottom;
    if (gap<=0 || search.top!=2*gap || client.bottom-status.bottom!=2*gap) return false;
    if (!check_wrapped_feedback(window,GetDlgItem(window,104),list)) return false;
    const wchar_t *requests[]={L"many",L"one",L"error",L"transcript",L"none",L"many",L""};
    const int counts[]={20,1,0,0,0,20,0};
    int short_log=0;
    for (size_t i=0;i<count_of(requests);++i) {
        SendMessageW(query,WM_SETTEXT,0,(LPARAM)requests[i]);
        bool settled=false;
        ULONGLONG deadline=GetTickCount64()+5000;
        do {
            RECT current,field;
            GetWindowRect(window,&current);
            GetClientRect(list,&viewport);
            if (current.left!=initial.left || current.top!=initial.top || current.right!=initial.right) return false;
            if (i==1 && current.bottom-initial.bottom!=6*side+gap) return false;
            if (SendMessageW(list,LB_GETCOUNT,0,0)==counts[i]) {
                if (counts[i]) {
                    settled=IsWindowVisible(list) && !IsWindowVisible(output) && viewport.bottom==6*side &&
                            current.bottom-initial.bottom==6*side+gap;
                } else if (i==2 || i==3) {
                    if (GetWindowRect(output,&field)) {
                        int height=field.bottom-field.top;
                        int lines=(int)SendMessageW(output,EM_GETLINECOUNT,0,0);
                        bool expected=i==2 ? lines>=1 && lines<=2 && height>=side && height<=2*side+gap :
                                            lines>=30 && height>short_log && height<lines*side/2;
                        MapWindowPoints(NULL,window,(POINT *)&field,2);
                        GetClientRect(window,&client);
                        settled=IsWindowVisible(output) && !IsWindowVisible(list) && expected &&
                                current.bottom-initial.bottom==height+gap && client.bottom-field.bottom==2*gap;
                        if (settled && i==2) short_log=height;
                    }
                } else {
                    wchar_t label[80];
                    SendMessageW(GetDlgItem(window,104),WM_GETTEXT,count_of(label),(LPARAM)label);
                    settled=!IsWindowVisible(list) && !IsWindowVisible(output) && EqualRect(&current,&initial) &&
                            (i!=4 || wcsstr(label,L"No matching"));
                }
            }
            if (!settled) Sleep(10);
        } while (!settled && GetTickCount64()<deadline);
        GUITHREADINFO gui={.cbSize=sizeof(gui)};
        if (!settled || !GetGUIThreadInfo(GetWindowThreadProcessId(window,NULL),&gui) || gui.hwndFocus!=query) {
            fprintf(stderr,"Software dock query %ls failed (rows=%lld, list height=%ld, focus=%d)\n",
                    requests[i],(long long)SendMessageW(list,LB_GETCOUNT,0,0),viewport.bottom,gui.hwndFocus==query);
            return false;
        }
        if (i==5) {
            SendMessageW(list,LB_SETCURSEL,0,0);
            SendMessageW(window,WM_COMMAND,MAKEWPARAM(103,LBN_SELCHANGE),(LPARAM)list);
            SendMessageW(window,WM_COMMAND,MAKEWPARAM(105,BN_CLICKED),(LPARAM)GetDlgItem(window,105));
            deadline=GetTickCount64()+4000;
            while (SendMessageW(output,EM_GETLINECOUNT,0,0)<20 && GetTickCount64()<deadline) Sleep(10);
            if (SendMessageW(output,EM_GETLINECOUNT,0,0)<20) return false;
            // Restrict the frame while a full result set and its long receipt are both open.
            int height=initial.bottom-initial.top+3*side+2*gap;
            if (!SetWindowPos(window,NULL,0,0,initial.right-initial.left,height,SWP_NOMOVE|SWP_NOACTIVATE|SWP_NOZORDER))
                return false;
            RECT receipt;
            GetClientRect(list,&viewport);
            GetWindowRect(output,&receipt);
            if (!IsWindowVisible(list) || !IsWindowVisible(output) || viewport.bottom<side ||
                viewport.bottom%side || receipt.bottom-receipt.top<side) return false;
            SendMessageW(list,LB_SETTOPINDEX,8,0);
            if (SendMessageW(list,LB_GETTOPINDEX,0,0)!=8) return false;
        }
    }
    const int providers[]={2,1,0};
    for (size_t i=0;i<count_of(providers);++i) {
        SendMessageW(pages,CB_SETCURSEL,providers[i],0);
        SendMessageW(window,WM_COMMAND,MAKEWPARAM(100,CBN_SELCHANGE),(LPARAM)pages);
        GUITHREADINFO gui={.cbSize=sizeof(gui)};
        RECT after;
        if (!GetWindowRect(window,&after) || !EqualRect(&initial,&after) ||
            !GetGUIThreadInfo(GetWindowThreadProcessId(window,NULL),&gui) || gui.hwndFocus!=(providers[i]==1 ? pages : query))
            return false;
    }
    return true;
}

static bool check_wrapped_feedback(HWND window,HWND status,HWND list) {
    if (!status) return false;
    wchar_t previous[1024];
    SendMessageW(status,WM_GETTEXT,count_of(previous),(LPARAM)previous);
    const wchar_t *messages[]={L"Reading…",
        L"This adapter has a long description that wraps across several lines at the current width. "
        L"The next instruction must stay readable beneath the full description. "
        L"AnUnbrokenPackageIdentifierThatMustRemainReadableWithNoSpacesOrWordBreaksAtTheMinimumPopupWidth"
        L"AndWithAnotherLongSuffixForADifferentBuildVariantThatIncludesTheFinalVersion.\r\nPress Enter to continue.",
        L"Ready."};
    RECT viewport;
    if (!GetClientRect(list,&viewport)) return false;
    int short_frame=0,short_text=0;
    bool ok=true;
    for (size_t i=0;i<count_of(messages) && ok;i++) {
        SendMessageW(status,WM_SETTEXT,0,(LPARAM)messages[i]);
        ULONGLONG end=GetTickCount64()+1500;
        ULONGLONG stable_since=0;
        bool fits=false;
        do {
            RECT frame,text,rows;
            if (!GetClientRect(window,&frame) || !GetClientRect(status,&text) || !GetClientRect(list,&rows)) break;
            if (!i) {
                if (IsWindowVisible(status) && text.bottom>0) {
                    if (!stable_since || short_frame!=frame.bottom || short_text!=text.bottom) stable_since=GetTickCount64();
                    short_frame=frame.bottom;
                    short_text=text.bottom;
                    fits=GetTickCount64()-stable_since>=60;
                }
            } else if (i==1) {
                HFONT font=w_theme_message_font(GetDpiForWindow(window));
                int required=w_theme_wrapped_height(font,messages[i],text.right);
                DeleteObject(font);
                MapWindowPoints(status,window,(POINT *)&text,2);
                fits=frame.bottom>short_frame && text.bottom<=frame.bottom && text.bottom-text.top>=required &&
                     text.bottom-text.top>short_text && rows.bottom==viewport.bottom;
            }
            else fits=frame.bottom==short_frame && text.bottom==short_text && rows.bottom==viewport.bottom;
            if (!fits) Sleep(10);
        } while (!fits && GetTickCount64()<end);
        if (!fits) {
            RECT frame,text,rows;
            GetClientRect(window,&frame); GetClientRect(status,&text); GetClientRect(list,&rows);
            wchar_t actual[512];
            SendMessageW(status,WM_GETTEXT,count_of(actual),(LPARAM)actual);
            printf("Feedback step %u frame %ld/%d, text %ld/%d, list %ld/%ld: %ls\n",
                   (unsigned)i,frame.bottom,short_frame,text.bottom,short_text,rows.bottom,viewport.bottom,actual);
        }
        ok=fits;
    }
    SendMessageW(status,WM_SETTEXT,0,(LPARAM)previous);
    return ok;
}

static bool check_anchored_rows(HWND window,const wchar_t *mode) {
    if (!wcscmp(mode,L"packages")) return check_package_dock(window);
    bool project=!wcscmp(mode,L"project");
    HWND list=FindWindowExW(window,NULL,L"ListBox",NULL);
    if (!list) return false;
    int count=(int)SendMessageW(list,LB_GETCOUNT,0,0);
    int row=(int)SendMessageW(list,LB_GETITEMHEIGHT,0,0);
    if (count<0 || row<=0) return false;
    bool microphone=!wcscmp(mode,L"microphone"), bluetooth=!wcscmp(mode,L"bluetooth");
    if (microphone || bluetooth) {
        HWND status=FindWindowExW(window,NULL,L"Static",NULL);
        HWND action=microphone ? FindWindowExW(window,NULL,WC_TABCONTROLW,NULL) : GetDlgItem(window,105);
        RECT client,text,control,viewport;
        ULONGLONG deadline=GetTickCount64()+8000;
        while (GetTickCount64()<deadline) {
            if (!status || !action || !GetClientRect(window,&client) || !GetClientRect(list,&viewport) ||
                !GetWindowRect(status,&text) || !GetWindowRect(action,&control)) return false;
            if (IsWindowVisible(list) && SendMessageW(list,LB_GETCOUNT,0,0)>0) return viewport.bottom==2*row;
            MapWindowPoints(NULL,window,(POINT *)&text,2);
            MapWindowPoints(NULL,window,(POINT *)&control,2);
            int pad=microphone ? control.top : client.right-control.right;
            if (IsWindowVisible(status) && IsWindowVisible(action) && pad>0 && control.bottom-control.top==row &&
                client.bottom==max(control.bottom,text.bottom)+pad && !IsWindowVisible(GetDlgItem(window,101)) &&
                (microphone ? text.top>=control.bottom : text.right<control.left)) return true;
            Sleep(10);
        }
        return false;
    }
    if (project) {
        HWND label=GetDlgItem(window,103),forget=GetDlgItem(window,105);
        if (!label || !forget || !!IsWindowVisible(list)!=(count>0) ||
            !!IsWindowVisible(label)!=(count>0) || !!IsWindowVisible(forget)!=(count>0)) return false;
        if (!check_wrapped_feedback(window,GetDlgItem(window,106),list)) return false;
        if (count==0) {
            RECT folder,action;
            return GetWindowRect(GetDlgItem(window,101),&folder) && GetWindowRect(GetDlgItem(window,107),&action) &&
                   action.top>=folder.bottom && action.top-folder.bottom<=row;
        }
    }
    RECT viewport;
    if (!wcscmp(mode,L"wifi")) {
        HWND change=GetDlgItem(window,108);
        RECT original,after;
        if (!change || !GetWindowRect(window,&original)) return false;
        const int rows[]={3,4,3};
        for (size_t i=0; i<count_of(rows); ++i) {
            if (i) SendMessageW(change,BM_CLICK,0,0);
            if (!GetWindowRect(window,&after) || after.left!=original.left || after.top!=original.top || after.right!=original.right ||
                !GetClientRect(list,&viewport) || viewport.bottom!=row*rows[i]) return false;
            if (i==1) {
                HWND status=FindWindowExW(window,NULL,L"Static",NULL);
                wchar_t message[512];
                ULONGLONG deadline=GetTickCount64()+5000;
                do {
                    SendMessageW(status,WM_GETTEXT,count_of(message),(LPARAM)message);
                    if (wcscmp(message,L"Reading network adapters…")) break;
                    Sleep(10);
                } while (GetTickCount64()<deadline);
                if (!check_wrapped_feedback(window,status,list)) return false;
                int selected=(int)SendMessageW(list,LB_GETCURSEL,0,0);
                if (selected>=0 && SendMessageW(list,LB_GETITEMDATA,selected,0)>=0) {
                    PostMessageW(window,WM_COMMAND,MAKEWPARAM(0,LBN_DBLCLK),(LPARAM)list);
                    HWND details=NULL;
                    deadline=GetTickCount64()+3000;
                    do {
                        HWND popup=GetLastActivePopup(window);
                        if (popup!=window && IsWindowVisible(popup) && GetDlgItem(popup,303)) { details=popup; break; }
                        Sleep(10);
                    } while (GetTickCount64()<deadline);
                    if (!details) return false;
                    RECT outer;
                    GetWindowRect(details,&outer);
                    SetWindowPos(details,NULL,0,0,MulDiv(296,GetDpiForWindow(details),96),outer.bottom-outer.top,
                                 SWP_NOMOVE|SWP_NOACTIVATE|SWP_NOZORDER);
                    HWND heading=FindWindowExW(details,NULL,L"Static",L"DNS servers");
                    HWND feedback=FindWindowExW(details,heading,L"Static",NULL);
                    bool fits=heading && check_wrapped_feedback(details,feedback,FindWindowExW(details,NULL,L"Edit",NULL));
                    SendMessageW(details,WM_COMMAND,IDCANCEL,0);
                    if (!fits) return false;
                }
            }
        }
        return true;
    }
    return GetClientRect(list,&viewport) && viewport.bottom==row*(project ? min(5,count) : 4);
}

static bool run_anchor(const wchar_t *exe,wchar_t *desktop_name,const wchar_t *mode,int spacing) {
    WNDCLASSW wc={.lpfnWndProc=DefWindowProcW,.hInstance=GetModuleHandleW(NULL),.lpszClassName=L"WinwmBar"};
    if (!RegisterClassW(&wc)) return false;
    HWND bar=CreateWindowW(wc.lpszClassName,L"",WS_POPUP,0,0,1,1,NULL,NULL,wc.hInstance,NULL);
    wchar_t key[popup_event_chars],command[128];
    w_popup_event_name(key,GetCurrentProcessId(),1);
    HANDLE cancel=CreateEventW(NULL,TRUE,FALSE,key);
    RECT area=w_popup_area(bar);
    w_popup_anchor source={.bar=bar};
    STARTUPINFOW startup={.cb=sizeof(startup),.lpDesktop=desktop_name};
    PROCESS_INFORMATION process={0};
    swprintf(command,count_of(command),L"winwm.exe --tool-%ls --bar-popup %llu 1",mode,(unsigned long long)(uintptr_t)bar);
    bool ok=bar && cancel && dock_source(&source,area,false,key) &&
            CreateProcessW(exe,command,NULL,NULL,FALSE,CREATE_SUSPENDED,NULL,NULL,&startup,&process);
    if (ok) {
        // Move the source before the child reads its initial placement, then move it while open.
        HWND window=NULL;
        ok=dock_source(&source,area,true,key) && ResumeThread(process.hThread)!=(DWORD)-1 &&
           (window=await_anchor(&process,&source,area,spacing))!=NULL && dock_source(&source,area,false,key) &&
           PostMessageW(window,winwm_popup_moved,0,0) && await_anchor(&process,&source,area,spacing)==window;
        if (ok && wcscmp(mode,L"date_time")) ok=check_anchored_rows(window,mode);
        RemovePropW(bar,key);
        SetEvent(cancel);
        DWORD code=STILL_ACTIVE;
        if (WaitForSingleObject(process.hProcess,5000)!=WAIT_OBJECT_0) {
            TerminateProcess(process.hProcess,1);
            WaitForSingleObject(process.hProcess,3000);
            ok=false;
        } else if (!GetExitCodeProcess(process.hProcess,&code) || code!=0) ok=false;
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
    if (cancel) CloseHandle(cancel);
    if (bar) DestroyWindow(bar);
    UnregisterClassW(wc.lpszClassName,wc.hInstance);
    printf("Anchored %ls at %d DIP spacing: %s\n",mode,spacing,ok ? "PASS" : "FAIL");
    return ok;
}

static bool run_sizes(const wchar_t *exe, wchar_t *desktop_name) {
    const w_panel_metrics settings[] = {
        {button_size_min, panel_spacing_min}, {button_size_default, panel_spacing_default},
        {button_size_max, panel_spacing_max}
    };
    bool ok = true;
    for (size_t i = 0; i < count_of(settings); ++i) {
        FILE *file = _wfopen(config, L"wb");
        if (!file) { perror("Cannot create test configuration"); return false; }
        int side = settings[i].button_size;
        // The empty middle configuration exercises the application's defaults.
        if (side != button_size_default)
            fprintf(file, "hl.config({decoration={panels={button_size=%d,spacing=%d}},animations={enabled=%s}})\n",
                    side,settings[i].spacing,side==button_size_min ? "true" : "false");
        // The catalog debounce must be able to expire during a dock collapse.
        if (side==button_size_min)
            fputs("hl.animation({leaf='layersOut',enabled=true,speed=6,bezier='linear',style='fade'})\n",file);
        if (fclose(file)) { perror("Cannot write test configuration"); return false; }
        for (size_t j = 0; j < count_of(panels); ++j)
            if (!run_panel(exe, desktop_name, &panels[j], side)) ok = false;
        if (side==button_size_default && !run_anchor(exe,desktop_name,L"date_time",settings[i].spacing)) ok=false;
        if (!run_anchor(exe,desktop_name,L"power_settings",settings[i].spacing)) ok=false;
        if (!run_anchor(exe,desktop_name,L"project",settings[i].spacing)) ok=false;
        if (!run_anchor(exe,desktop_name,L"wifi",settings[i].spacing)) ok=false;
        if (!run_anchor(exe,desktop_name,L"microphone",settings[i].spacing)) ok=false;
        if (!run_anchor(exe,desktop_name,L"bluetooth",settings[i].spacing)) ok=false;
        if (!run_anchor(exe,desktop_name,L"packages",settings[i].spacing)) ok=false;
    }
    return ok;
}

// Install the probe itself as a read-only asynchronous WinGet fixture inside the private profile.
static int package_fixture(const wchar_t *command) {
    if (wcsstr(command,L"--query=none")) return (int)0x8a150014u;
    if (wcsstr(command,L"--query=error")) { fputs("Catalog is unavailable.",stdout); return 3; }
    if (wcsstr(command,L"--query=transcript")) {
        for (int i=0;i<40;++i) printf("Diagnostic line %d\n",i);
        return 3;
    }
    int count=wcsstr(command,L"--query=one") ? 1 : 20;
    puts("Name                Id                       Version\n---------------------------------------------------");
    for (int i=0;i<count;++i) printf("App %-16dFixture.App%-14d1.0\n",i,i);
    return 0;
}

int wmain(int argc, wchar_t **argv) {
    if (argc>=2 && !wcscmp(argv[1],L"list")) return (int)0x8a150014u;
    if (argc>=2 && !wcscmp(argv[1],L"show")) {
        for (int i=0;i<25;++i) printf("Package information line %d\n",i);
        return 0;
    }
    if (argc>=2 && !wcscmp(argv[1],L"search")) {
        for (int i=2;i<argc;++i) if (!wcsncmp(argv[i],L"--query=",8)) return package_fixture(argv[i]);
        return 1;
    }
    wchar_t exe[MAX_PATH], temp[MAX_PATH], profile[MAX_PATH], desktop_name[80];
    DWORD n = argc == 2 ? GetFullPathNameW(argv[1], count_of(exe), exe, NULL) : 0;
    if (!n || n >= count_of(exe)) {
        fputs("Usage: test_panel_geometry.exe path/to/winwm.exe\n", stderr);
        return 1;
    }
    swprintf(desktop_name, count_of(desktop_name), L"WinwmPanelTest-%lu-%llu", GetCurrentProcessId(), GetTickCount64());
    n = GetTempPathW(count_of(temp), temp);
    if (!n || n + wcslen(desktop_name) >= count_of(profile)) return 1;
    swprintf(profile, count_of(profile), L"%ls%ls", temp, desktop_name);
    if (!CreateDirectoryW(profile, NULL) || !SetCurrentDirectoryW(profile)) {
        fprintf(stderr, "Private profile setup failed (%lu)\n", GetLastError());
        return 1;
    }
    HDESK desktop = CreateDesktopW(desktop_name, NULL, NULL, 0, GENERIC_ALL, NULL);
    bool ok = SetEnvironmentVariableW(L"USERPROFILE", profile) && SetEnvironmentVariableW(L"LOCALAPPDATA", profile) &&
              CreateDirectoryW(L".config", NULL) && CreateDirectoryW(L".config\\winwm", NULL) &&
              (AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(),DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) ||
               SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) &&
              desktop && SetThreadDesktop(desktop);
    const wchar_t alias[]=L"Microsoft\\WindowsApps\\winget.exe";
    wchar_t runner[MAX_PATH];
    if (ok) ok=CreateDirectoryW(L"Microsoft",NULL) && CreateDirectoryW(L"Microsoft\\WindowsApps",NULL) &&
               GetModuleFileNameW(NULL,runner,count_of(runner)) && CopyFileW(runner,alias,TRUE);
    if (ok) ok = run_sliders() && run_sizes(exe, desktop_name);
    else fprintf(stderr, "Private desktop or profile setup failed (%lu)\n", GetLastError());
    DeleteFileW(config);
    DeleteFileW(alias);
    RemoveDirectoryW(L"Microsoft\\WindowsApps");
    RemoveDirectoryW(L"Microsoft");
    RemoveDirectoryW(L".config\\winwm");
    RemoveDirectoryW(L".config");
    SetCurrentDirectoryW(temp);
    if (!RemoveDirectoryW(profile)) {
        fprintf(stderr, "Private profile cleanup failed: %ls (%lu)\n", profile, GetLastError());
        ok = false;
    }
    return ok ? 0 : 1;
}
