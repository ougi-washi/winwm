#include "bar/bar.h"
#include "theme/surface.h"
#include "model/model.h"
#include <stdio.h>

static w_bar_window bars[2];
static w_desktop_model model;
static w_desktop_config config;
static bool valid=true;
static bool expect_flyout;
static unsigned actions;

static void select_workspace(int output,const char *action,const w_popup_anchor *anchor) {
    if (!action) return;
    w_command command;
    if ((unsigned)output>=count_of(bars) || !anchor || anchor->bar!=bars[output].hwnd ||
        !w_command_parse(action,&command) || command.kind!=cmd_workspace_select ||
        w_bar_menu_active()!=expect_flyout) {
        valid=false;
        return;
    }
    ++actions;
    if (expect_flyout) {
        w_bar_menu_close(NULL,false);
        expect_flyout=false;
    }
    model.output=output;
    w_model_workspace(&model,atoi(command.arg)-1);
}

static POINT center(const w_bar_window *bar,int workspace) {
    for (int i=0;i<bar->hit_count;++i) {
        const struct w_bar_hit *hit=&bar->hits[i];
        if (hit->workspace==workspace)
            return (POINT){(hit->box.left+hit->box.right)/2,(hit->box.top+hit->box.bottom)/2};
    }
    valid=false;
    return (POINT){0};
}

static void wheel(HWND bar,POINT point,int delta) {
    ClientToScreen(bar,&point);
    SendMessageW(bar,WM_MOUSEWHEEL,MAKEWPARAM(0,delta),MAKELPARAM(point.x,point.y));
}

static bool exercise(void) {
    w_model_init(&model);
    w_config_defaults(&config);
    strcpy(config.bar_left,"workspaces battery");
    config.bar_center[0]=config.bar_right[0]=0;
    config.bar_workspace_count=workspace_limit;
    config.animations=motion_off;
    w_theme_configure(&config);
    model.selected[1]=8;
    for (unsigned i=0;i<count_of(bars);++i) {
        w_bar_window *bar=&bars[i];
        if (!w_bar_create(bar,i,&model.selected[i],&config,select_workspace)) return false;
        w_bar_update(bar,&(w_bar_view){.selected=model.selected[i]});
        w_rect area={0,(int)i*100,840,90};
        w_bar_position(bar,area,area,96,false);
        UpdateWindow(bar->hwnd);
    }
    HWND hwnd=bars[1].hwnd;
    int power=bars[1].hit_count-1;
    if (power<0 || strcmp(bars[1].hits[power].module,"battery") ||
        !w_bar_activate_hit(&bars[1],power) || !w_bar_menu_active()) return false;
    expect_flyout=true;
    POINT third=center(&bars[1],3),fourth=center(&bars[1],4);
    LPARAM click=MAKELPARAM(third.x,third.y);
    SendMessageW(hwnd,WM_LBUTTONDOWN,MK_LBUTTON,click);
    SendMessageW(hwnd,WM_LBUTTONUP,0,click);
    if (!valid || actions!=1 || model.output!=1 || model.selected[1]!=2) return false;
    // Mouse, keyboard and IPC can all change the model before the next bar paint.
    wheel(hwnd,third,-WHEEL_DELTA);
    wheel(hwnd,third,-WHEEL_DELTA);
    if (actions!=3 || model.selected[1]!=4) return false;
    w_model_workspace(&model,9);
    wheel(hwnd,third,-WHEEL_DELTA);
    if (model.selected[1]!=0) return false;
    wheel(hwnd,third,WHEEL_DELTA/2);
    SendMessageW(hwnd,WM_MOUSEMOVE,0,MAKELPARAM(fourth.x,fourth.y));
    wheel(hwnd,fourth,WHEEL_DELTA/2);
    if (actions!=5 || model.selected[1]!=9) return false;
    POINT first=center(&bars[0],1);
    wheel(bars[0].hwnd,first,-2*WHEEL_DELTA);
    return valid && actions==6 && model.output==0 && model.selected[0]==2 && model.selected[1]==9;
}

int wmain(void) {
    wchar_t name[80];
    swprintf(name,count_of(name),L"WinwmWorkspaceBarTest-%lu-%llu",GetCurrentProcessId(),GetTickCount64());
    HDESK original=GetThreadDesktop(GetCurrentThreadId());
    HDESK desktop=CreateDesktopW(name,NULL,NULL,0,GENERIC_ALL,NULL);
    if (!desktop || !SetThreadDesktop(desktop)) return 1;
    bool ok=exercise();
    for (unsigned i=0;i<count_of(bars);++i) if (bars[i].hwnd) DestroyWindow(bars[i].hwnd);
    SetThreadDesktop(original);
    CloseDesktop(desktop);
    printf("Workspace click, wheel burst, high-resolution wheel and output routing: %s\n",ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
