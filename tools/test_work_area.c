#include "os_win32/work_area.h"
#include <stdio.h>

// Simulate a two-monitor display. No SPI or broadcast reaches the test user's desktop.
static MONITORINFOEXW screens[2];
static RECT dock;
static int writes, notifications, failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"Work area test failed at line %d: %s\n",__LINE__,#x); ++failures; } } while (0)

static BOOL fake_info(HMONITOR m, LPMONITORINFO out) {
    MONITORINFOEXW *src = (MONITORINFOEXW *)m;
    memcpy(out,src,out->cbSize);
    return TRUE;
}
static BOOL spi(UINT action, UINT param, void *value, UINT flags) {
    CHECK(action == SPI_SETWORKAREA && !param && !flags);
    RECT *r = value;
    for (size_t i=0;i<count_of(screens);++i) {
        RECT bounds;
        if (IntersectRect(&bounds,r,&screens[i].rcMonitor) && EqualRect(r,&bounds)) {
            screens[i].rcWork = *r;
            ++writes;
            return TRUE;
        }
    }
    return FALSE;
}
static BOOL queue(LPTHREAD_START_ROUTINE worker, void *arg, ULONG flags) {
    (void)flags; worker(arg); return TRUE;
}
static LRESULT sent(HWND w, UINT msg, WPARAM wp, LPARAM lp, UINT flags, UINT ms, PDWORD_PTR result) {
    (void)result;
    CHECK(w==HWND_BROADCAST && msg==WM_SETTINGCHANGE && wp==SPI_SETWORKAREA && !lp && flags && ms);
    ++notifications;
    return TRUE;
}
static BOOL enumerate(WNDENUMPROC callback, LPARAM arg) {
    if (!IsRectEmpty(&dock)) callback((HWND)1,arg);
    return TRUE;
}
static int dock_class(HWND w, LPWSTR name, int cap) { (void)w; lstrcpynW(name,L"DockFixture",cap); return 11; }
static BOOL dock_rect(HWND w, RECT *r) { (void)w; *r=dock; return TRUE; }
bool w_os_same_process(HWND w) { (void)w; return false; }
bool w_os_cloaked(HWND w) { (void)w; return false; }
DWORD w_os_shell_desktop(void) { return ERROR_REQUIRES_INTERACTIVE_WINDOWSTATION; }

#define GetMonitorInfoW fake_info
#define EnumWindows enumerate
#define IsWindowVisible(w) TRUE
#define IsIconic(w) FALSE
#define GetPropW(w,key) NULL
#define GetWindowRect dock_rect
#define GetClassNameW dock_class
#define SystemParametersInfoW spi
#define SendMessageTimeoutW sent
#define QueueUserWorkItem queue
#include "../src/os_win32/work_area.c"

int main(void) {
    CHECK(!w_work_areas_open(1,true) && !w_work_areas_open(1,false));
    CHECK(!areas && !mapping);
    w_work_area records[output_limit] = {0};
    areas=records;
    screens[0]=(MONITORINFOEXW){.rcMonitor={0,0,3794,2066},.rcWork={0,0,3794,2018},.szDevice=L"main"};
    screens[1]=(MONITORINFOEXW){.rcMonitor={-1920,-120,0,960},.rcWork={-1870,-120,0,912},.szDevice=L"left"};
    dock=(RECT){-1920,-120,-1870,960};
    RECT base=w_work_area_available(0,&screens[0]);
    CHECK(EqualRect(&base,&screens[0].rcMonitor));
    RECT normal=base; normal.bottom-=32;
    w_work_area_publish(0,(HMONITOR)&screens[0],normal);
    CHECK(screens[0].rcWork.bottom==2034 && writes==1 && notifications==1);
    base=w_work_area_available(0,&screens[0]);
    CHECK(base.bottom==2066);
    w_work_area_publish(0,(HMONITOR)&screens[0],normal);
    CHECK(writes==1);
    // Hide for fullscreen, then move the bar to the top.
    w_work_area_publish(0,(HMONITOR)&screens[0],base);
    CHECK(screens[0].rcWork.bottom==2066);
    base.top+=40;
    w_work_area_publish(0,(HMONITOR)&screens[0],base);
    CHECK(screens[0].rcWork.top==40);
    base=w_work_area_available(1,&screens[1]);
    CHECK(base.left==-1870 && base.bottom==960);
    base.left+=24;
    w_work_area_publish(1,(HMONITOR)&screens[1],base);
    CHECK(screens[1].rcWork.left==-1846 && screens[1].rcWork.bottom==960);
    // An unrelated dock changes the right edge just before a crash.
    screens[0].rcWork.right-=55;
    w_restore_work((HMONITOR)&screens[0],NULL,NULL,0);
    CHECK(screens[0].rcWork.top==0 && screens[0].rcWork.right==3739 && screens[0].rcWork.bottom==2018);
    // The monitor never applies obsolete coordinates after a topology change.
    screens[1].rcMonitor.right=100;
    int before=writes;
    w_restore_work((HMONITOR)&screens[1],NULL,NULL,0);
    CHECK(writes==before);
    screens[1].rcMonitor.right=0;
    w_restore_work((HMONITOR)&screens[1],NULL,NULL,0);
    CHECK(screens[1].rcWork.left==-1870 && screens[1].rcWork.bottom==912 && writes==notifications);
    return failures ? 1 : 0;
}
