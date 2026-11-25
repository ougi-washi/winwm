#include "desktop_tools/tools.h"
#include "theme/theme.h"
#include "theme/icon_button.h"
#include <stdio.h>

enum { layout_capacity = sizeof(((w_desktop_config *)0)->bar_left), save_done = WM_APP + 20 };
typedef struct { char group[3][layout_capacity]; } w_toolbar_layout;
static const wchar_t *const sections[] = {L"Left", L"Center", L"Right"};
static const char *const fields[] = {"modules_left", "modules_center", "modules_right"};
static const char marker[] = "# winwm toolbar layout (edited by Configure toolbar)";
static const char end_marker[] = "# end winwm toolbar layout";
static HWND window, group_label, group_box, list, available_label, available, add, up, down, remove_button, apply, status;
static HFONT font;
static w_toolbar_layout initial, draft;
static int group = 2;
static HANDLE worker;
static wchar_t path[os_path_cap], feedback[256];
static bool saved, closing;
// The picker includes every built-in, provider, explicit style and token in the three groups.
static struct { wchar_t key[56], label[96]; } choices[48 + provider_limit + 32 + 3*layout_capacity];
static int choice_count;

static void w_layout_read(w_toolbar_layout *layout, const w_desktop_config *cfg) {
    strcpy(layout->group[0], cfg->bar_left);
    strcpy(layout->group[1], cfg->bar_center);
    strcpy(layout->group[2], cfg->bar_right);
}

// Only complete marker lines belong to the editor. All other bytes are copied unchanged.
static char *w_marker_line(char *data, const char *line) {
    size_t length = strlen(line);
    for (char *p=data; *p;) {
        if (!strncmp(p,line,length) && (!p[length] || p[length]=='\r' || p[length]=='\n')) return p;
        p = strchr(p,'\n');
        if (!p) break;
        ++p;
    }
    return NULL;
}

static bool w_write_bytes(HANDLE file, const char *s, size_t size) {
    DWORD written;
    return WriteFile(file,s,(DWORD)size,&written,NULL) && written == size;
}

static bool w_write_layout(HANDLE file, char *source) {
    char *begin = w_marker_line(source,marker), *end = w_marker_line(source,end_marker);
    if ((begin == NULL) != (end == NULL) || (begin && (end < begin ||
        w_marker_line(begin+strlen(marker),marker) || w_marker_line(end+strlen(end_marker),end_marker)))) {
        wcscpy(feedback,L"Check the toolbar layout markers in winwm.conf.");
        return false;
    }
    if (begin) {
        char *after = strchr(end,'\n');
        char *out = begin;
        for (char *p=strchr(begin,'\n')+1; p<end;) {
            char *next=strchr(p,'\n')+1;
            const char *key=p+strspn(p," \t");
            bool assignment=false;
            if (!strncmp(key,"bar:",4)) {
                key+=4;
                for (size_t i=0;i<count_of(fields);i++) {
                    size_t n=strlen(fields[i]);
                    if (!strncmp(key,fields[i],n) && key[n+strspn(key+n," \t")]=='=') assignment=true;
                }
            }
            if (!assignment) { memmove(out,p,(size_t)(next-p)); out+=next-p; }
            p=next;
        }
        memmove(out, after ? after+1 : end+strlen(end), after ? strlen(after+1)+1 : 1);
    }
    size_t n = strlen(source);
    const char *nl = strstr(source,"\r\n") ? "\r\n" : "\n";
    if (!w_write_bytes(file,source,n) || (n && source[n-1]!='\n' && !w_write_bytes(file,nl,strlen(nl))) ||
        !w_write_bytes(file,marker,strlen(marker)) || !w_write_bytes(file,nl,strlen(nl))) return false;
    for (size_t i=0;i<count_of(fields);i++) {
        char escaped[2*layout_capacity], *out = escaped;
        for (const char *p=draft.group[i]; *p; ++p) {
            if (*p=='#') *out++='#';
            *out++=*p;
        }
        *out=0;
        char line[sizeof(escaped)+40];
        int length=snprintf(line,sizeof(line),"bar:%s = %s%s",fields[i],escaped,nl);
        if (length < 0 || (size_t)length >= sizeof(line) || !w_write_bytes(file,line,length)) return false;
    }
    return w_write_bytes(file,end_marker,strlen(end_marker)) && w_write_bytes(file,nl,strlen(nl));
}

static bool w_same_file(HANDLE original) {
    BY_HANDLE_FILE_INFORMATION a,b;
    HANDLE check=CreateFileW(path,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_DELETE,NULL,
                             OPEN_EXISTING,0,NULL);
    if (check==INVALID_HANDLE_VALUE) return false;
    bool same=GetFileInformationByHandle(original,&a) && GetFileInformationByHandle(check,&b) &&
              a.dwVolumeSerialNumber==b.dwVolumeSerialNumber && a.nFileIndexHigh==b.nFileIndexHigh &&
              a.nFileIndexLow==b.nFileIndexLow;
    CloseHandle(check);
    return same;
}

static bool w_save_file(void) {
    bool ok=false, created=false;
    wchar_t temp[os_path_cap], destination[os_path_cap];
    char error[256]={0};
    char *source=NULL;
    DWORD size, bytes;
    w_desktop_config *config=malloc(sizeof(*config));
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);
    HANDLE output=INVALID_HANDLE_VALUE;
    if (file==INVALID_HANDLE_VALUE || !config) goto done;
    // Follow a profile symlink to its real file for replacement; validate includes relative to the configured path.
    DWORD len=GetFinalPathNameByHandleW(file,destination,count_of(destination),FILE_NAME_NORMALIZED);
    if (!len || len>=count_of(destination)) goto done;
    size=GetFileSize(file,NULL);
    if (size>256*1024) goto done;
    source=malloc((size_t)size+1);
    if (!source || !ReadFile(file,source,size,&bytes,NULL) || bytes!=size || memchr(source,0,size)) goto done;
    source[size]=0;
    if (!w_os_config_read_path(path,config,error,sizeof(error))) goto done;
    w_toolbar_layout current;
    w_layout_read(&current,config);
    for (int i=0;i<3;i++) {
        if (strcmp(current.group[i],initial.group[i])) {
            wcscpy(feedback,L"The layout changed on disk. Reopen Configure toolbar to load it.");
            goto done;
        }
    }
    // ReplaceFile requires both files on the same volume, even when the profile links elsewhere.
    if (swprintf(temp,count_of(temp),L"%ls.%lu.%llu.tmp",destination,GetCurrentProcessId(),GetTickCount64())<0) goto done;
    output=CreateFileW(temp,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
    if (output==INVALID_HANDLE_VALUE) goto done;
    created=true;
    if (!w_write_layout(output,source) || !FlushFileBuffers(output)) goto done;
    CloseHandle(output);
    output=INVALID_HANDLE_VALUE;
    if (!w_os_config_read_candidate(path,temp,config,error,sizeof(error))) goto done;
    w_layout_read(&current,config);
    for (int i=0;i<3;i++) {
        if (strcmp(current.group[i],draft.group[i])) {
            wcscpy(feedback,L"A module name cannot be represented in this configuration.");
            goto done;
        }
    }
    if (!w_same_file(file)) {
        wcscpy(feedback,L"Another editor replaced winwm.conf. Reopen this panel.");
        goto done;
    }
    ok=ReplaceFileW(destination,temp,NULL,0,NULL,NULL)!=0;
done:
    if (error[0]) w_os_wide(error,feedback,count_of(feedback));
    if (output!=INVALID_HANDLE_VALUE) CloseHandle(output);
    if (created && !ok) DeleteFileW(temp);
    if (file!=INVALID_HANDLE_VALUE) CloseHandle(file);
    free(config);
    free(source);
    return ok;
}

static DWORD WINAPI w_save_toolbar(void *unused) {
    (void)unused;
    wcscpy(feedback,L"Could not save winwm.conf. Check file permissions and try again.");
    HANDLE mutex=CreateMutexW(NULL,FALSE,L"Local\\winwm.toolbar-layout");
    DWORD wait=mutex ? WaitForSingleObject(mutex,1500) : WAIT_FAILED;
    saved=false;
    if (wait==WAIT_OBJECT_0 || wait==WAIT_ABANDONED) {
        saved=w_save_file();
        ReleaseMutex(mutex);
    } else wcscpy(feedback,L"Another toolbar editor is saving. Try again.");
    if (mutex) CloseHandle(mutex);
    if (saved) {
        wchar_t exe[os_path_cap], line[]=L"winwm.exe ctl reload-bar";
        STARTUPINFOW startup={.cb=sizeof(startup)};
        PROCESS_INFORMATION child={0};
        bool accepted=false;
        if (w_os_self(exe,count_of(exe)) && CreateProcessW(exe,line,NULL,NULL,FALSE,CREATE_NO_WINDOW,
                NULL,NULL,&startup,&child)) {
            DWORD code;
            accepted=WaitForSingleObject(child.hProcess,3000)==WAIT_OBJECT_0 &&
                     GetExitCodeProcess(child.hProcess,&code) && code==0;
            CloseHandle(child.hThread);
            CloseHandle(child.hProcess);
        }
        wcscpy(feedback,accepted ? L"Layout saved." : L"Layout saved. Press Win+Shift+R to reload.");
    }
    PostMessageW(window,save_done,0,0);
    return 0;
}

static void w_controls(void) {
    int selected=(int)SendMessageW(list,LB_GETCURSEL,0,0);
    int count=(int)SendMessageW(list,LB_GETCOUNT,0,0);
    EnableWindow(up,!worker && selected>0);
    EnableWindow(down,!worker && selected>=0 && selected+1<count);
    EnableWindow(remove_button,!worker && selected>=0);
    EnableWindow(add,!worker && SendMessageW(available,CB_GETCURSEL,0,0)!=CB_ERR);
    EnableWindow(apply,!worker && (strcmp(initial.group[0],draft.group[0]) ||
        strcmp(initial.group[1],draft.group[1]) || strcmp(initial.group[2],draft.group[2])));
    EnableWindow(group_box,!worker);
    EnableWindow(available,!worker);
}

static int w_offer(const wchar_t *name) {
    static const struct { const wchar_t *key, *label; } labels[] = {
        {L"audio",L"Volume"}, {L"battery",L"Battery and power"}, {L"bluetooth",L"Bluetooth"},
        {L"clock",L"Clock"}, {L"cpu",L"CPU usage"}, {L"keyboard",L"Input language"},
        {L"media",L"Media controls"}, {L"memory",L"Memory usage"}, {L"menu",L"Menu"},
        {L"mode",L"Desktop mode"}, {L"network",L"Network"}, {L"throughput",L"Network traffic"},
        {L"title",L"Focused window"}, {L"workspaces",L"Workspaces"},
        {L"display",L"Display"}, {L"clipboard",L"Clipboard history"},
        {L"notifications",L"Desktop notifications"}, {L"project",L"Projects"}
    };
    for (int i=0;i<choice_count;i++) if (!wcscmp(choices[i].key,name)) return i;
    if (choice_count >= (int)count_of(choices)) return -1;
    int id=choice_count++;
    lstrcpynW(choices[id].key,name,count_of(choices[id].key));
    const wchar_t *tag=wcschr(name,L'#'), *label=name;
    size_t n=tag ? (size_t)(tag-name) : wcslen(name);
    for (size_t i=0;i<count_of(labels);i++) {
        if (wcslen(labels[i].key)==n && !wcsncmp(labels[i].key,name,n)) { label=labels[i].label; break; }
    }
    if (tag && label!=name) swprintf(choices[id].label,count_of(choices[id].label),L"%ls · %ls",label,tag+1);
    else lstrcpynW(choices[id].label,label,count_of(choices[id].label));
    LRESULT row=SendMessageW(available,CB_ADDSTRING,0,(LPARAM)choices[id].label);
    if (row>=0) SendMessageW(available,CB_SETITEMDATA,row,id);
    return id;
}

static int w_list_insert(int index,int id) {
    if ((unsigned)id >= (unsigned)choice_count) return -1;
    int row=(int)SendMessageW(list,LB_INSERTSTRING,index,(LPARAM)choices[id].label);
    if (row>=0) SendMessageW(list,LB_SETITEMDATA,row,id);
    return row;
}

static void w_show_group(int selection) {
    SendMessageW(list,LB_RESETCONTENT,0,0);
    char spec[layout_capacity], *ctx=NULL;
    strcpy(spec,draft.group[group]);
    for (char *p=strtok_s(spec," ",&ctx);p;p=strtok_s(NULL," ",&ctx)) {
        wchar_t key[56];
        if (w_os_wide(p,key,count_of(key))) {
            w_list_insert(-1,w_offer(key));
        }
    }
    SendMessageW(list,LB_SETCURSEL,selection,0);
    w_controls();
}

static void w_modify(HWND source) {
    bool button_had_focus = GetFocus() == source;
    int selected=(int)SendMessageW(list,LB_GETCURSEL,0,0);
    if (source==add) {
        int choice=(int)SendMessageW(available,CB_GETCURSEL,0,0);
        if (choice<0) return;
        selected=w_list_insert(-1,(int)SendMessageW(available,CB_GETITEMDATA,choice,0));
    } else if (selected>=0) {
        int id=(int)SendMessageW(list,LB_GETITEMDATA,selected,0);
        SendMessageW(list,LB_DELETESTRING,selected,0);
        if (source!=remove_button) {
            selected+=source==up ? -1 : 1;
            w_list_insert(selected,id);
        } else selected=min(selected,(int)SendMessageW(list,LB_GETCOUNT,0,0)-1);
    }
    char spec[layout_capacity]={0};
    size_t used=0;
    for (int i=0;i<SendMessageW(list,LB_GETCOUNT,0,0);i++) {
        int id=(int)SendMessageW(list,LB_GETITEMDATA,i,0);
        if ((unsigned)id>=(unsigned)choice_count) { w_show_group(-1); return; }
        if (used && used+1<sizeof(spec)) spec[used++]=' ';
        if (!w_os_utf8(choices[id].key,spec+used,(int)(sizeof(spec)-used))) {
            w_show_group(-1);
            SetWindowTextW(status,L"This section is full.");
            return;
        }
        used=strlen(spec);
    }
    strcpy(draft.group[group],spec);
    SendMessageW(list,LB_SETCURSEL,selected,0);
    SetWindowTextW(status,L"");
    w_controls();
    if (button_had_focus && !IsWindowEnabled(source))
        SetFocus(selected >= 0 ? list : available);
}

static void w_arrange(bool fonts) {
    if (!apply) return;
    RECT r;
    GetClientRect(window,&r);
    UINT dpi=GetDpiForWindow(window);
    int p=MulDiv(14,dpi,96), gap=MulDiv(8,dpi,96), h=MulDiv(31,dpi,96), label=MulDiv(22,dpi,96);
    int width=max(1,(int)r.right-2*p), side=min(width/3,h);
    int list_y=p+label+h+gap, bottom=r.bottom-p, pick_y=bottom-2*h-gap;
    int list_h=max(1,pick_y-label-gap-list_y);
    if (fonts) {
        HFONT next=CreateFontW(-MulDiv(14,dpi,96),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,
                               CLEARTYPE_QUALITY,0,L"Segoe UI");
        if (next) {
            for(HWND c=GetWindow(window,GW_CHILD);c;c=GetWindow(c,GW_HWNDNEXT))
                SendMessageW(c,WM_SETFONT,(WPARAM)next,TRUE);
            DeleteObject(font);
            font=next;
        }
        SendMessageW(list,LB_SETITEMHEIGHT,0,h);
    }
    MoveWindow(group_label,p,p,width,label,TRUE);
    MoveWindow(group_box,p,p+label,width,h*6,TRUE);
    MoveWindow(list,p,list_y,width-side-gap,list_h,TRUE);
    HWND edits[]={up,down,remove_button};
    for(int i=0;i<3;i++) MoveWindow(edits[i],p+width-side,list_y+i*(h+gap),side,h,TRUE);
    MoveWindow(available_label,p,pick_y-label,width,label,TRUE);
    MoveWindow(available,p,pick_y,width-side-gap,h*8,TRUE);
    MoveWindow(add,p+width-side,pick_y,side,h,TRUE);
    MoveWindow(status,p,bottom-h,width-side-gap,h,TRUE);
    MoveWindow(apply,p+width-side,bottom-h,side,h,TRUE);
}

static LRESULT CALLBACK w_toolbar_proc(HWND w,UINT msg,WPARAM wp,LPARAM lp) {
    LRESULT result;
    if (w_theme_message(w,msg,wp,lp,&result)) return result;
    if (msg==WM_SIZE && wp!=SIZE_MINIMIZED) { w_arrange(false); return 0; }
    if (msg==WM_DPICHANGED) { w_tool_resize_for_dpi(w,(RECT *)lp); w_arrange(true); return 0; }
    if (msg==WM_GETMINMAXINFO) {
        UINT dpi=GetDpiForWindow(w);
        w_tool_minimum(w,(MINMAXINFO *)lp,MulDiv(325,dpi,96),MulDiv(375,dpi,96)); return 0;
    }
    if (msg==WM_COMMAND && !worker) {
        HWND c=(HWND)lp;
        if (c==group_box && HIWORD(wp)==CBN_SELCHANGE) {
            group=(int)SendMessageW(c,CB_GETCURSEL,0,0); w_show_group(0);
        } else if (c==list && HIWORD(wp)==LBN_SELCHANGE) w_controls();
        else if (HIWORD(wp)==BN_CLICKED && (c==add || c==up || c==down || c==remove_button)) w_modify(c);
        else if (c==apply && HIWORD(wp)==BN_CLICKED) {
            worker=CreateThread(NULL,0,w_save_toolbar,NULL,0,NULL);
            SetWindowTextW(status,worker ? L"Saving…" : L"Could not start the save.");
            w_controls();
        }
        return 0;
    }
    if (msg==save_done && worker) {
        WaitForSingleObject(worker,INFINITE); CloseHandle(worker); worker=NULL;
        if (closing) { DestroyWindow(w); return 0; }
        if (saved) initial=draft;
        SetWindowTextW(status,feedback);
        w_controls();
        return 0;
    }
    if (msg==WM_CLOSE && worker) {
        closing=true;
        ShowWindow(w,SW_HIDE);
        return 0;
    }
    if (msg==WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(w,msg,wp,lp);
}

static HWND w_control(const wchar_t *kind,const wchar_t *text,DWORD style,int id) {
    return CreateWindowExW(0,kind,text,WS_CHILD|WS_VISIBLE|style,0,0,1,1,window,(HMENU)(INT_PTR)id,GetModuleHandleW(NULL),NULL);
}

int w_toolbar_run(void) {
    w_desktop_config *cfg=malloc(sizeof(*cfg));
    char error[256]="Could not open winwm.conf.";
    if (!cfg || !w_os_config_load(cfg,error,sizeof(error)) || !w_os_config_path(path,count_of(path))) {
        wchar_t text[256]=L"Could not open winwm.conf.";
        w_os_wide(error,text,count_of(text));
        MessageBoxW(NULL,text,L"Toolbar layout",MB_OK|MB_ICONERROR);
        free(cfg); return 1;
    }
    w_layout_read(&initial,cfg); draft=initial;
    w_theme_colors(cfg->background,cfg->foreground,cfg->accent);
    HINSTANCE instance=GetModuleHandleW(NULL);
    WNDCLASSW wc={.lpfnWndProc=w_toolbar_proc,.hInstance=instance,.lpszClassName=L"WinwmToolbarLayout",
                  .hCursor=LoadCursorW(NULL,IDC_ARROW)};
    RegisterClassW(&wc);
    MONITORINFO mi={.cbSize=sizeof(mi)};
    UINT dpi=w_tool_work_area(&mi);
    RECT a=mi.rcWork;
    int width=min(MulDiv(420,dpi,96),a.right-a.left), height=min(MulDiv(456,dpi,96),a.bottom-a.top);
    window=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_CONTROLPARENT,wc.lpszClassName,L"Toolbar layout",
        WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,(a.left+a.right-width)/2,(a.top+a.bottom-height)/2,width,height,NULL,NULL,instance,NULL);
    if (!window) { free(cfg); return 1; }
    DWORD combo=WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL, button=WS_TABSTOP|BS_OWNERDRAW;
    group_label=w_control(L"STATIC",L"Toolbar &section",SS_CENTERIMAGE,100);
    group_box=w_control(L"COMBOBOX",L"",combo,101);
    list=w_control(L"LISTBOX",L"Modules in display order",WS_TABSTOP|WS_VSCROLL|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT|LBS_OWNERDRAWFIXED|LBS_HASSTRINGS,102);
    up=w_control(L"BUTTON",L"Move up",button,103);
    down=w_control(L"BUTTON",L"Move down",button,104);
    remove_button=w_control(L"BUTTON",L"Remove",button,105);
    available_label=w_control(L"STATIC",L"Available &modules",SS_CENTERIMAGE,106);
    available=w_control(L"COMBOBOX",L"",combo|CBS_SORT,107);
    add=w_control(L"BUTTON",L"Add",button,108);
    status=w_control(L"STATIC",L"",SS_EDITCONTROL,109);
    apply=w_control(L"BUTTON",L"Apply",button,110);
    if (!group_label || !group_box || !list || !up || !down || !remove_button || !available_label || !available || !add || !status || !apply) {
        DestroyWindow(window); free(cfg); return 1;
    }
    w_icon_button(up,L"\uE74A",L"Move module up (Alt+Up in list)");
    w_icon_button(down,L"\uE74B",L"Move module down (Alt+Down in list)");
    w_icon_button(remove_button,L"\uE738",L"Remove module from section (Delete in list)");
    w_icon_button(add,L"\uE710",L"Add selected module to this section");
    w_icon_button(apply,L"\uE74E",L"Save toolbar layout");
    for(int i=0;i<3;i++) SendMessageW(group_box,CB_ADDSTRING,0,(LPARAM)sections[i]);
    SendMessageW(group_box,CB_SETCURSEL,group,0);
    for(int i=0;w_config_module_name(i);i++) {
        wchar_t key[56];
        if (w_os_wide(w_config_module_name(i),key,count_of(key))) w_offer(key);
    }
    for(int i=0;i<cfg->provider_count;i++) {
        wchar_t name[40],key[56];
        if (w_os_wide(cfg->providers[i].name,name,count_of(name))) {
            swprintf(key,count_of(key),L"custom/%ls",name); w_offer(key);
        }
    }
    for(int i=0;i<cfg->style_count;i++) {
        wchar_t key[56];
        if (w_os_wide(cfg->styles[i].name,key,count_of(key))) w_offer(key);
    }
    free(cfg);
    w_show_group(0);
    SendMessageW(available,CB_SETCURSEL,0,0); w_controls();
    w_arrange(true);
    ShowWindow(window,SW_SHOW); SetFocus(list);
    MSG m;
    while (GetMessageW(&m,NULL,0,0)>0) {
        if (m.hwnd==list && !worker && (m.message==WM_KEYDOWN || m.message==WM_SYSKEYDOWN)) {
            HWND edit=NULL;
            if (m.wParam==VK_DELETE) edit=remove_button;
            if (GetKeyState(VK_MENU)<0 && m.wParam==VK_UP) edit=up;
            if (GetKeyState(VK_MENU)<0 && m.wParam==VK_DOWN) edit=down;
            if (edit && IsWindowEnabled(edit)) { w_modify(edit); continue; }
        }
        if (m.message==WM_KEYDOWN && m.wParam==VK_ESCAPE &&
            !SendMessageW(group_box,CB_GETDROPPEDSTATE,0,0) && !SendMessageW(available,CB_GETDROPPEDSTATE,0,0)) {
            SendMessageW(window,WM_CLOSE,0,0); continue;
        }
        if (!IsDialogMessageW(window,&m)) { TranslateMessage(&m); DispatchMessageW(&m); }
    }
    if (font) DeleteObject(font);
    return 0;
}
