#include "theme/dialog.h"
#include "theme/caption.h"
#include "theme/theme.h"
#include "theme/icon_button.h"
#include "win_platform/win_platform.h"
#include <stdlib.h>

typedef struct {
    const wchar_t *message, *caption;
    UINT flags;
    int accept, cancel;
    HWND text, icon, primary, secondary;
    HFONT font;
    UINT dpi;
    int text_width, lines;
    bool laying_out;
} w_prompt;

void w_theme_load_profile(void) {
    if (w_theme_configured()) return;
    w_desktop_config *profile = malloc(sizeof(*profile));
    if (!profile) return;
    w_config_defaults(profile);
    if (!w_os_cache_read(profile)) {
        wchar_t path[os_path_cap];
        char error[256];
        if (w_os_config_path(path, count_of(path)) && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES &&
            !w_os_config_load(profile, error, sizeof(error)))
            w_config_defaults(profile);
    }
    w_theme_configure(profile);
    free(profile);
}

static void w_layout_prompt(HWND w, w_prompt *p, HMONITOR monitor, const RECT *anchor) {
    if (p->laying_out || !p->text) return;
    p->laying_out = true;
    UINT dpi = GetDpiForWindow(w);
    int first=(int)SendMessageW(p->text,EM_GETFIRSTVISIBLELINE,0,0);
    LRESULT first_char=first>0 ? SendMessageW(p->text,EM_LINEINDEX,first,0) : -1;
    if (!p->font || dpi != p->dpi) {
        HFONT next = w_theme_message_font(dpi);
        if (next) {
            HWND controls[] = {p->text, p->primary, p->secondary};
            for (size_t i=0;i<count_of(controls);++i)
                if (controls[i]) SendMessageW(controls[i],WM_SETFONT,(WPARAM)next,FALSE);
            DeleteObject(p->font);
            p->font = next;
            p->dpi = dpi;
            p->text_width=0;
        }
    }
    HFONT font = p->font ? p->font : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    MONITORINFO mi = {.cbSize = sizeof(mi)};
    if (!GetMonitorInfoW(monitor, &mi)) {
        p->laying_out = false;
        return;
    }
    RECT work = mi.rcWork, frame = {0};
    w_theme_adjust_rect(&frame, (DWORD)GetWindowLongPtrW(w, GWL_STYLE),
                       (DWORD)GetWindowLongPtrW(w, GWL_EXSTYLE), dpi);
    int gap = w_theme_spacing(dpi), pad = gap * 2, side = w_theme_button_size(dpi);
    int max_width=max(1,work.right-work.left-(frame.right-frame.left));
    pad=min(pad,max(0,(max_width-side)/2));
    int usable=max(1,work.bottom-work.top-(frame.bottom-frame.top));
    int action_height=min(side,max(1,usable-1));
    int vertical_pad=min(pad,max(0,(usable-action_height-1)/2));
    int section_gap=min(gap,max(0,usable-2*vertical_pad-action_height-1));
    int max_body=max(1,usable-2*vertical_pad-section_gap-action_height);
    int icon_side=w_theme_icon_size(dpi);
    bool show_icon=p->icon && max_body>=icon_side && max_width-2*pad>icon_side+2*gap;
    int inset = show_icon ? icon_side+gap : 0;
    HDC dc = GetDC(w);
    HFONT text_font=(HFONT)SendMessageW(p->text,WM_GETFONT,0,0);
    HGDIOBJ old = SelectObject(dc,text_font ? text_font : font);
    TEXTMETRICW metrics={0};
    GetTextMetricsW(dc,&metrics);
    int line_height=max(1,metrics.tmHeight);
    // Long diagnostics remain selectable and scrollable inside a prompt with
    // at most twenty complete lines visible on a roomy monitor.
    max_body=min(max_body,20*line_height);
    RECT natural={0};
    w_theme_draw_text(dc,p->message ? p->message : L"",-1,&natural,DT_CALCRECT|DT_EXPANDTABS|DT_NOPREFIX);
    SelectObject(dc, old);
    ReleaseDC(w, dc);
    // Limit a prose line by its font's average character width. A short
    // message needs only its own text, caption and action group.
    DWORD margins=(DWORD)SendMessageW(p->text,EM_GETMARGINS,0,0);
    int body_width=min(natural.right,64*max(1,metrics.tmAveCharWidth))+LOWORD(margins)+HIWORD(margins)+1;
    if (natural.bottom>max_body) body_width+=GetSystemMetricsForDpi(SM_CXVSCROLL,dpi);
    int action_width=side;
    HWND actions[]={p->primary,p->secondary};
    for (size_t i=0; i<count_of(actions); ++i) {
        wchar_t caption[32];
        if (!actions[i] || !GetWindowTextW(actions[i],caption,count_of(caption))) continue;
        int required=w_labeled_button_width(font,caption,dpi);
        if (required>action_width) action_width=required;
    }
    int action_span=p->secondary ? 2*action_width+gap : action_width;
    int caption_width=w_theme_caption_width(w);
    int width=min(max_width,max(caption_width,2*pad+max(body_width+inset,action_span)));
    int content=max(1,width-2*pad);
    // EDIT also wraps unbroken paths and accounts for its own text margins.
    // Measure its actual line layout at the final width before sizing the dialog.
    int text_width=max(1,content-inset);
    if (p->text_width!=text_width) {
        RECT previous;
        GetWindowRect(p->text,&previous);
        if (GetWindowLongW(p->text,GWL_STYLE)&WS_VSCROLL) ShowScrollBar(p->text,SB_VERT,FALSE);
        SetWindowPos(p->text,NULL,0,0,text_width,max(line_height,previous.bottom-previous.top),
                     SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        int measured=(int)SendMessageW(p->text,EM_GETLINECOUNT,0,0);
        p->lines=max(1,measured);
        p->text_width=text_width;
    }
    bool overflow=p->lines>max_body/line_height;
    int text_height=overflow ? max(1,max_body/line_height)*line_height : p->lines*line_height;
    text_height=min(max_body,text_height);
    int body_height=max(text_height,show_icon ? icon_side : 0);
    int height=2*vertical_pad+body_height+section_gap+action_height;
    int full_w = width + frame.right-frame.left, full_h = height + frame.bottom-frame.top;
    int x = w_clamp_int((anchor->left+anchor->right-full_w)/2, work.left, work.right-full_w);
    int y = w_clamp_int((anchor->top+anchor->bottom-full_h)/2, work.top, work.bottom-full_h);
    SetWindowPos(w, NULL, x, y, full_w, full_h, SWP_NOZORDER | SWP_NOACTIVATE);
    int offset=w_theme_caption_height(w);
    if (p->icon) ShowWindow(p->icon,show_icon ? SW_SHOWNOACTIVATE : SW_HIDE);
    if (show_icon) {
        LPCWSTR resource = (p->flags & MB_ICONMASK) == MB_ICONERROR ? IDI_ERROR :
                           (p->flags & MB_ICONMASK) == MB_ICONWARNING ? IDI_WARNING : IDI_INFORMATION;
        HICON icon = LoadImageW(NULL,resource,IMAGE_ICON,icon_side,icon_side,LR_SHARED);
        SendMessageW(p->icon, STM_SETIMAGE, IMAGE_ICON, (LPARAM)icon);
        SetWindowPos(p->icon,NULL,pad,offset+vertical_pad+(body_height-icon_side)/2,icon_side,icon_side,SWP_NOZORDER | SWP_NOACTIVATE);
    }
    SetWindowPos(p->text,NULL,pad+inset,offset+vertical_pad+(body_height-text_height)/2,text_width,text_height,SWP_NOZORDER|SWP_NOACTIVATE);
    if (!!(GetWindowLongW(p->text,GWL_STYLE)&WS_VSCROLL)!=overflow) ShowScrollBar(p->text,SB_VERT,overflow);
    if (first_char>=0 && overflow) {
        int next=(int)SendMessageW(p->text,EM_LINEFROMCHAR,first_char,0);
        int scroll=next-(int)SendMessageW(p->text,EM_GETFIRSTVISIBLELINE,0,0);
        if (scroll) SendMessageW(p->text,EM_LINESCROLL,0,scroll);
    }
    int between=min(gap,max(0,content-2));
    int bw=max(1,min(action_width,p->secondary ? (content-between)/2 : content));
    int right = width-pad-bw, top = offset+height-vertical_pad-action_height;
    if (p->secondary)
        SetWindowPos(p->secondary, NULL, right, top, bw, action_height, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(p->primary, NULL, p->secondary ? right-between-bw : right, top, bw, action_height, SWP_NOZORDER | SWP_NOACTIVATE);
    p->laying_out = false;
    if (GetDpiForWindow(w) != dpi) w_layout_prompt(w,p,monitor,anchor);
}

static INT_PTR CALLBACK w_prompt_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    w_prompt *p = (void *)GetWindowLongPtrW(w, DWLP_USER);
    if (msg == WM_INITDIALOG) {
        p = (void *)lp;
        SetWindowLongPtrW(w, DWLP_USER, (LONG_PTR)p);
        SetWindowTextW(w, p->caption);
        LRESULT ignored;
        w_theme_message(w,WM_CREATE,0,0,&ignored);
        const DWORD bs = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW;
        HINSTANCE instance = GetModuleHandleW(NULL);
        if ((p->flags & MB_ICONMASK) && (p->flags & MB_ICONMASK) != MB_ICONQUESTION)
            p->icon = CreateWindowW(L"STATIC",NULL,WS_CHILD | WS_VISIBLE | SS_ICON,0,0,0,0,w,NULL,instance,NULL);
        p->text = CreateWindowExW(0, L"EDIT", p->message, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE |
                        ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 0,0,0,0,w,NULL,instance,NULL);
        p->primary = CreateWindowW(L"BUTTON", p->accept == IDYES ? L"&Yes" : L"OK",
                        bs, 0,0,0,0,w,(HMENU)(INT_PTR)p->accept,instance,NULL);
        if (p->cancel != IDOK)
            p->secondary = CreateWindowW(L"BUTTON", p->cancel == IDNO ? L"&No" : L"Cancel",
                        bs, 0,0,0,0,w,(HMENU)(INT_PTR)p->cancel,instance,NULL);
        if (!p->text || !p->primary || (p->cancel != IDOK && !p->secondary)) {
            EndDialog(w, -1);
            return TRUE;
        }
        w_labeled_button(p->primary,L"\uE73E",p->accept==IDYES ? L"&Yes" : L"OK");
        if (p->secondary)
            w_labeled_button(p->secondary,L"\uE711",p->cancel==IDNO ? L"&No" : L"Cancel");
        w_theme_follow(w);
        HWND owner = GetWindow(w, GW_OWNER);
        if (!owner) owner = GetForegroundWindow();
        HMONITOR monitor = MonitorFromWindow(owner, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi = {.cbSize = sizeof(mi)};
        GetMonitorInfoW(monitor, &mi);
        RECT anchor = mi.rcWork;
        if (owner && IsWindowVisible(owner) && !IsIconic(owner)) GetWindowRect(owner, &anchor);
        w_layout_prompt(w,p,monitor,&anchor);
        SetFocus(p->secondary && (p->flags & MB_DEFMASK) == MB_DEFBUTTON2 ? p->secondary : p->primary);
        return FALSE;
    }
    if (!p) return FALSE;
    LRESULT result;
    if (w_theme_message(w, msg, wp, lp, &result)) {
        if (msg>=WM_CTLCOLORMSGBOX && msg<=WM_CTLCOLORSTATIC) return result;
        SetWindowLongPtrW(w, DWLP_MSGRESULT, result);
        return TRUE;
    }
    bool metrics=msg==WM_SETTINGCHANGE && wp==SPI_SETNONCLIENTMETRICS;
    if (msg==WM_DPICHANGED || msg==WM_SIZE || msg==WM_DISPLAYCHANGE || msg==WM_FONTCHANGE || metrics ||
        (msg==WM_SETTINGCHANGE && wp==SPI_SETWORKAREA)) {
        RECT anchor;
        if (metrics || msg==WM_FONTCHANGE) p->dpi=0;
        if (msg == WM_DPICHANGED) anchor = *(RECT *)lp;
        else GetWindowRect(w, &anchor);
        w_layout_prompt(w, p, MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST), &anchor);
        return TRUE;
    }
    if (msg == WM_CLOSE || (msg == WM_COMMAND && LOWORD(wp) == IDCANCEL)) {
        EndDialog(w, p->cancel);
        return TRUE;
    }
    if (msg == WM_COMMAND && (LOWORD(wp) == p->accept || LOWORD(wp) == p->cancel)) {
        EndDialog(w, LOWORD(wp));
        return TRUE;
    }
    if (msg == DM_GETDEFID) {
        int def = p->secondary && (p->flags & MB_DEFMASK) == MB_DEFBUTTON2 ? p->cancel : p->accept;
        HWND focus = GetFocus();
        if (focus && (focus == p->primary || focus == p->secondary)) def = GetDlgCtrlID(focus);
        SetWindowLongPtrW(w, DWLP_MSGRESULT, MAKELONG(def, DC_HASDEFID));
        return TRUE;
    }
    if (msg == WM_NCDESTROY && p->font) DeleteObject(p->font);
    return FALSE;
}

int w_dialog(HWND owner, const wchar_t *message, const wchar_t *caption, UINT flags) {
    w_theme_load_profile();
    _Alignas(DWORD) struct { DLGTEMPLATE form; WORD menu, window_class, title; } layout = {
        .form = {.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN | DS_MODALFRAME,
                 .cx = 180, .cy = 75}
    };
    UINT type = flags & MB_TYPEMASK;
    w_prompt prompt = {.message = message, .caption = caption, .flags = flags,
                       .accept = type == MB_YESNO ? IDYES : IDOK,
                       .cancel = type == MB_YESNO ? IDNO : type == MB_OKCANCEL ? IDCANCEL : IDOK};
    // Native multiline EDIT controls expect CRLF. Callers also supply LF-only
    // messages and Windows error text can already contain CRLF.
    wchar_t *normalized=NULL;
    if (message && wcspbrk(message,L"\r\n")) {
        size_t length=wcslen(message);
        if (length>(SIZE_MAX/sizeof(wchar_t)-1)/2) return -1;
        normalized=malloc((2*length+1)*sizeof(*normalized));
        if (!normalized) return -1;
        wchar_t *out=normalized;
        for (const wchar_t *input=message; *input; ++input) {
            if (*input==L'\r' || *input==L'\n') {
                if (*input==L'\r' && input[1]==L'\n') ++input;
                *out++=L'\r';
                *out++=L'\n';
            } else *out++=*input;
        }
        *out=0;
        prompt.message=normalized;
    }
    int result=(int)DialogBoxIndirectParamW(GetModuleHandleW(NULL), &layout.form, owner, w_prompt_proc, (LPARAM)&prompt);
    free(normalized);
    return result;
}
