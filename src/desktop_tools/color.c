#include "desktop_tools/tools.h"
#include "theme/theme.h"
#include <shellscalingapi.h>
#include <stdio.h>

typedef struct {
    HDC image;
    const DWORD *pixels;
    HMONITOR monitor;
    RECT desktop, lens;
    POINT point;
    COLORREF sample;
    HFONT font;
    UINT dpi;
    bool busy;
} w_picker;

static int w_px(const w_picker *p, int value) { return MulDiv(value, p->dpi, 96); }

static DWORD w_pixel(const w_picker *p, int x, int y) {
    int width = p->desktop.right-p->desktop.left, height = p->desktop.bottom-p->desktop.top;
    return p->pixels[(size_t)max(0,min(y,height-1))*width + max(0,min(x,width-1))];
}

static void w_sample(HWND w, w_picker *p, POINT point) {
    MONITORINFO mi = {.cbSize = sizeof(mi)};
    HMONITOR monitor = MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
    GetMonitorInfoW(monitor, &mi);
    if (p->monitor != monitor) {
        p->monitor = monitor;
        UINT dpi, vertical;
        if (SUCCEEDED(GetDpiForMonitor(monitor,MDT_EFFECTIVE_DPI,&dpi,&vertical))) p->dpi = dpi;
        HFONT next = CreateFontW(-w_px(p,13),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,
                                 CLEARTYPE_QUALITY,0,L"Segoe UI");
        if (next) { if (p->font) DeleteObject(p->font); p->font = next; }
    }
    point.x = max(mi.rcMonitor.left, min(point.x, mi.rcMonitor.right-1));
    point.y = max(mi.rcMonitor.top, min(point.y, mi.rcMonitor.bottom-1));
    p->point = point;
    DWORD pixel = w_pixel(p,point.x-p->desktop.left,point.y-p->desktop.top);
    p->sample = RGB(pixel >> 16, pixel >> 8, pixel);
    InvalidateRect(w, &p->lens, FALSE);
    int pad = w_px(p, 18), width = w_px(p, 188), height = w_px(p, 88);
    int x = point.x + pad, y = point.y + pad;
    if (x + width > mi.rcMonitor.right) x = point.x - width - pad;
    if (y + height > mi.rcMonitor.bottom) y = point.y - height - pad;
    x = max(x, mi.rcMonitor.left) - p->desktop.left;
    y = max(y, mi.rcMonitor.top) - p->desktop.top;
    p->lens = (RECT){x, y, x+width, y+height};
    InvalidateRect(w, &p->lens, FALSE);
}

static void w_hex(COLORREF c, wchar_t out[8]) {
    swprintf(out, 8, L"#%02X%02X%02X", (unsigned)GetRValue(c), (unsigned)GetGValue(c), (unsigned)GetBValue(c));
}

static bool w_copy_sample(HWND w, COLORREF color) {
    if (color == CLR_INVALID) return false;
    HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE, 8*sizeof(wchar_t));
    wchar_t *s = data ? GlobalLock(data) : NULL;
    if (s) {
        w_hex(color, s);
        GlobalUnlock(data);
        if (OpenClipboard(w)) {
            if (EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, data)) data = NULL;
            CloseClipboard();
            if (!data) return true;
        }
    }
    if (data) GlobalFree(data);
    return false;
}

static void w_paint_picker(HWND w, const w_picker *p) {
    PAINTSTRUCT paint;
    HDC dc = BeginPaint(w, &paint);
    RECT r = paint.rcPaint, lens = p->lens;
    BitBlt(dc, r.left,r.top,r.right-r.left,r.bottom-r.top,p->image,r.left,r.top,SRCCOPY);
    FillRect(dc,&lens,w_theme_brush());
    SetDCBrushColor(dc,w_theme_accent());
    FrameRect(dc,&lens,GetStockObject(DC_BRUSH));
    int step = max(1,w_px(p,7)), left = lens.left+w_px(p,10), top = lens.top+w_px(p,12);
    int sx = p->point.x-p->desktop.left, sy = p->point.y-p->desktop.top;
    // Clamp each source pixel at the edge of the desktop so the center of
    // the loupe stays on the chosen pixel even in the four corners.
    DWORD patch[9][9];
    for (int y=0;y<9;++y) for (int x=0;x<9;++x) patch[y][x] = w_pixel(p,sx+x-4,sy+y-4);
    BITMAPINFO format = {.bmiHeader={.biSize=sizeof(BITMAPINFOHEADER),.biWidth=9,.biHeight=-9,
                                     .biPlanes=1,.biBitCount=32,.biCompression=BI_RGB}};
    SetStretchBltMode(dc,COLORONCOLOR);
    StretchDIBits(dc,left,top,9*step,9*step,0,0,9,9,patch,&format,DIB_RGB_COLORS,SRCCOPY);
    RECT center = {left+4*step,top+4*step,left+5*step,top+5*step};
    FrameRect(dc,&center,GetStockObject(WHITE_BRUSH));
    InflateRect(&center,1,1);
    FrameRect(dc,&center,GetStockObject(BLACK_BRUSH));
    wchar_t hex[8];
    w_hex(p->sample,hex);
    SetBkMode(dc,TRANSPARENT);
    SetTextColor(dc,w_theme_foreground());
    HGDIOBJ old = SelectObject(dc,p->font);
    RECT label = {left+9*step+w_px(p,9),top,lens.right-w_px(p,5),lens.bottom};
    DrawTextW(dc,hex,-1,&label,DT_SINGLELINE|DT_NOPREFIX);
    label.top += w_px(p,24);
    DrawTextW(dc,p->busy ? L"Clipboard busy" : L"Enter to copy",-1,&label,DT_SINGLELINE);
    label.top += w_px(p,18);
    DrawTextW(dc,L"Esc to cancel",-1,&label,DT_SINGLELINE);
    SelectObject(dc,old);
    EndPaint(w,&paint);
}

static LRESULT CALLBACK w_picker_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE)
        SetWindowLongPtrW(w,GWLP_USERDATA,(LONG_PTR)((CREATESTRUCTW *)lp)->lpCreateParams);
    w_picker *p = (w_picker *)GetWindowLongPtrW(w,GWLP_USERDATA);
    if (!p) return DefWindowProcW(w,msg,wp,lp);
    if (msg == WM_MOUSEMOVE || msg == WM_LBUTTONUP) {
        POINT point;
        if (GetCursorPos(&point)) w_sample(w,p,point);
    }
    if (msg == WM_LBUTTONUP || (msg == WM_KEYDOWN && (wp == VK_RETURN ||
        (wp == 'C' && GetKeyState(VK_CONTROL) < 0)))) {
        if (w_copy_sample(w,p->sample)) DestroyWindow(w);
        else { p->busy = true; InvalidateRect(w,&p->lens,FALSE); }
        return 0;
    }
    if (msg == WM_KEYDOWN && wp >= VK_LEFT && wp <= VK_DOWN) {
        int step = GetKeyState(VK_SHIFT) < 0 ? 10 : 1;
        POINT pt = p->point;
        pt.x += (wp == VK_RIGHT ? step : wp == VK_LEFT ? -step : 0);
        pt.y += (wp == VK_DOWN ? step : wp == VK_UP ? -step : 0);
        w_sample(w,p,pt);
        SetCursorPos(p->point.x,p->point.y);
        return 0;
    }
    if (msg == WM_DISPLAYCHANGE || msg == WM_KILLFOCUS || msg == WM_RBUTTONUP ||
        (msg == WM_KEYDOWN && wp == VK_ESCAPE)) { DestroyWindow(w); return 0; }
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_PAINT) { w_paint_picker(w,p); return 0; }
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(w,msg,wp,lp);
}

int w_color_run(void) {
    MONITORINFO info = {.cbSize = sizeof(info)};
    w_picker p = {.dpi = w_tool_work_area(&info)};
    int x=GetSystemMetrics(SM_XVIRTUALSCREEN), y=GetSystemMetrics(SM_YVIRTUALSCREEN);
    int width=GetSystemMetrics(SM_CXVIRTUALSCREEN), height=GetSystemMetrics(SM_CYVIRTUALSCREEN);
    p.desktop = (RECT){x,y,x+width,y+height};
    HDC screen = GetDC(NULL);
    p.image = CreateCompatibleDC(screen);
    BITMAPINFO format = {.bmiHeader={.biSize=sizeof(BITMAPINFOHEADER),.biWidth=width,.biHeight=-height,
                                     .biPlanes=1,.biBitCount=32,.biCompression=BI_RGB}};
    void *pixels = NULL;
    HBITMAP bmp = CreateDIBSection(screen,&format,DIB_RGB_COLORS,&pixels,NULL,0);
    p.pixels = pixels;
    HGDIOBJ previous = bmp && p.image ? SelectObject(p.image,bmp) : NULL;
    bool captured = previous && BitBlt(p.image,0,0,width,height,screen,x,y,SRCCOPY|CAPTUREBLT) && GdiFlush();
    ReleaseDC(NULL,screen);
    HWND window = NULL;
    if (captured) {
        HINSTANCE inst = GetModuleHandleW(NULL);
        WNDCLASSW wc = {.lpfnWndProc=w_picker_proc,.hInstance=inst,.lpszClassName=L"WinwmColorPicker",
                        .hCursor=LoadCursorW(NULL,IDC_CROSS)};
        RegisterClassW(&wc);
        window = CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW,wc.lpszClassName,L"Pick screen color",
                                 WS_POPUP,x,y,width,height,NULL,NULL,inst,&p);
        if (window) {
            POINT point = {x,y};
            GetCursorPos(&point);
            w_sample(window,&p,point);
            ShowWindow(window,SW_SHOW);
            SetForegroundWindow(window);
            SetFocus(window);
            MSG m;
            while (GetMessageW(&m,NULL,0,0)>0) { TranslateMessage(&m); DispatchMessageW(&m); }
        }
    }
    if (previous) SelectObject(p.image,previous);
    if (bmp) DeleteObject(bmp);
    if (p.image) DeleteDC(p.image);
    if (p.font) DeleteObject(p.font);
    return window ? 0 : 1;
}
