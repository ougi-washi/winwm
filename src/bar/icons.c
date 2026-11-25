#include "bar/icons.h"
#include <stdlib.h>

////////////////
//~ Icons

/* Geometry is in a 24-unit square. A negative x starts a new stroke. */
typedef struct w_vertex {
    signed char x, y;
} w_vertex;

static const w_vertex menu[] = {{3,4},{10,4},{10,10},{3,10},{3,4},{-1,0},
    {14,4},{21,4},{21,10},{14,10},{14,4},{-1,0},
    {3,14},{10,14},{10,20},{3,20},{3,14},{-1,0},{14,14},{21,14},{21,20},{14,20},{14,14}};
static const w_vertex bluetooth[] = {{6, 6}, {17, 16}, {11, 22}, {11, 2}, {17, 8}, {6, 18}};
static const w_vertex network[] = {{2, 8},   {5, 6},  {8, 5},   {12, 4},  {16, 5},  {19, 6},
                                 {22, 8},  {-1, 0}, {5, 12},  {8, 10},  {12, 9},  {16, 10},
                                 {19, 12}, {-1, 0}, {8, 16},  {10, 15}, {12, 14}, {14, 15},
                                 {16, 16}, {-1, 0}, {11, 19}, {12, 20}, {13, 19}};
static const w_vertex speaker[] = {{3, 9}, {7, 9}, {12, 5}, {12, 19}, {7, 15}, {3, 15}, {3, 9}};
static const w_vertex waves[] = {{15, 9}, {16, 11}, {16, 13}, {15, 15}, {-1, 0},
                               {18, 6}, {20, 9},  {21, 12}, {20, 15}, {18, 18}};
static const w_vertex cross[] = {{16, 9}, {21, 15}, {-1, 0}, {21, 9}, {16, 15}};
static const w_vertex battery[] = {{2, 7},   {18, 7},  {18, 17}, {2, 17},  {2, 7},  {-1, 0},
                                 {21, 10}, {21, 14}, {-1, 0},  {5, 10},  {5, 14}, {-1, 0},
                                 {8, 10},  {8, 14},  {-1, 0},  {11, 10}, {11, 14}};
static const w_vertex chip[] = {{6, 6},   {18, 6},  {18, 18}, {6, 18},  {6, 6},  {-1, 0},  {10, 10},
                              {14, 10}, {14, 14}, {10, 14}, {10, 10}, {-1, 0}, {9, 3},   {9, 6},
                              {-1, 0},  {15, 3},  {15, 6},  {-1, 0},  {9, 18}, {9, 21},  {-1, 0},
                              {15, 18}, {15, 21}, {-1, 0},  {3, 9},   {6, 9},  {-1, 0},  {3, 15},
                              {6, 15},  {-1, 0},  {18, 9},  {21, 9},  {-1, 0}, {18, 15}, {21, 15}};
static const w_vertex play[] = {{7, 4}, {20, 12}, {7, 20}, {7, 4}};
static const w_vertex resize[] = {{4,10},{4,4},{10,4},{-1,0},{4,4},{20,20},{-1,0},{14,20},{20,20},{20,14}};
static const w_vertex awake[] = {{4,9},{16,9},{16,15},{14,18},{7,18},{4,15},{4,9},{-1,0},
    {16,10},{20,10},{21,12},{20,14},{16,14},{-1,0},{3,21},{19,21},{-1,0},{8,6},{8,3},{-1,0},{12,6},{12,2}};
static const w_vertex ram[] = {{2,6},{22,6},{22,16},{2,16},{2,6},{-1,0},{6,10},{6,12},
    {-1,0},{10,10},{10,12},{-1,0},{14,10},{14,12},{-1,0},{18,10},{18,12},
    {-1,0},{5,17},{5,20},{-1,0},{10,17},{10,20},{-1,0},{15,17},{15,20},{-1,0},{20,17},{20,20}};
static const w_vertex keyboard[]={{2,5},{22,5},{22,19},{2,19},{2,5},{-1,0},{6,9},{7,9},
    {-1,0},{11,9},{12,9},{-1,0},{16,9},{17,9},{-1,0},{7,14},{17,14}};
static const w_vertex clock_face[]={{12,2},{19,5},{22,12},{19,19},{12,22},{5,19},{2,12},{5,5},{12,2},
    {-1,0},{12,6},{12,12},{16,14}};
static const w_vertex traffic[]={{7,4},{7,20},{-1,0},{3,8},{7,4},{11,8},{-1,0},
    {17,20},{17,4},{-1,0},{13,16},{17,20},{21,16}};
static const w_vertex monitor[]={{2,4},{22,4},{22,16},{2,16},{2,4},{-1,0},
    {12,16},{12,20},{-1,0},{8,20},{16,20}};
static const w_vertex project[]={{2,6},{8,6},{11,9},{21,9},{21,19},{2,19},{2,6},
    {-1,0},{7,12},{9,14},{7,16},{-1,0},{12,16},{16,16}};
static const w_vertex clipboard[]={{9,4},{9,2},{15,2},{15,6},{9,6},{9,4},{-1,0},
    {8,4},{5,4},{5,21},{19,21},{19,4},{16,4},{-1,0},{9,11},{15,11},{-1,0},{9,15},{13,15}};
static const w_vertex bell[]={{4,17},{6,14},{6,10},{7,6},{9,4},{12,3},{15,4},{17,6},
    {18,10},{18,14},{20,17},{4,17},{-1,0},{9,20},{11,21},{13,21},{15,20}};
static const w_vertex left[]={{15,5},{8,12},{15,19}},right[]={{9,5},{16,12},{9,19}};
static const w_vertex up[]={{5,15},{12,8},{19,15}},down[]={{5,9},{12,16},{19,9}};
static const w_vertex more[]={{4,12},{6,12},{-1,0},{11,12},{13,12},{-1,0},{18,12},{20,12}};
static const w_vertex plug[] = {{8, 3}, {8, 8}, {-1,0}, {16, 3}, {16, 8}, {-1,0}, {5, 8}, {19, 8},
                                {18, 14}, {15, 17}, {9, 17}, {6, 14}, {5, 8}, {-1,0}, {12,17}, {12,22}};
static const w_vertex ethernet[] = {{3,5},{21,5},{21,16},{17,16},{17,20},{7,20},{7,16},{3,16},{3,5},
                                    {-1,0},{8,6},{8,10},{-1,0},{12,6},{12,10},{-1,0},{16,6},{16,10}};
static const w_vertex offline[] = {{3,8},{7,6},{12,5},{17,6},{21,8},{-1,0},{6,12},{10,10},{14,10},
                                   {-1,0},{10,17},{12,19},{-1,0},{3,3},{21,21}};

////////////////
//~ Icons Functions

static void w_stroke(HDC dc, const w_vertex *path, size_t count, int width) {
    bool start = true;
    for (size_t i = 0; i < count; ++i) {
        if (path[i].x < 0) {
            start = true;
            continue;
        }
        int x = MulDiv(path[i].x, width, 24), y = MulDiv(path[i].y, width, 24);
        if (start)
            MoveToEx(dc, x, y, NULL);
        else
            LineTo(dc, x, y);
        start = false;
    }
}

void w_icons_free(w_icon_atlas *atlas) {
    free(atlas->coverage);
    free(atlas->pixels);
    *atlas = (w_icon_atlas){0};
}

void w_icons_resize(w_icon_atlas *atlas, int dpi) {
    const int size = w_clamp_int(MulDiv(17, dpi, 96), 8, 128);
    if (atlas->coverage && atlas->size == size)
        return;
    w_icons_free(atlas);
    enum { samples = 4 };
    int width = size * samples;
    BITMAPINFO info = {.bmiHeader = {.biSize = sizeof(BITMAPINFOHEADER),
                                     .biWidth = width,
                                     .biHeight = -width,
                                     .biPlanes = 1,
                                     .biBitCount = 32,
                                     .biCompression = BI_RGB}};
    DWORD *bitmap = NULL;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP dib = CreateDIBSection(dc, &info, DIB_RGB_COLORS, (void **)&bitmap, NULL, 0);
    LOGBRUSH white = {.lbStyle = BS_SOLID, .lbColor = RGB(255, 255, 255)};
    HPEN pen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND,
                            max(1, MulDiv(width, 2, 24)), &white, 0, NULL);
    atlas->size = size;
    atlas->coverage = calloc((size_t)icon_count * size * size, sizeof(BYTE));
    atlas->pixels = malloc((size_t)size * size * sizeof(DWORD));
    if (dc && dib && pen && atlas->coverage && atlas->pixels) {
        HGDIOBJ old_bitmap = SelectObject(dc, dib), old_pen = SelectObject(dc, pen);
        static const struct w_icon_shape {
            const w_vertex *path;
            size_t count;
        } shapes[] = {[icon_menu] = {menu, count_of(menu)},
                      [icon_bluetooth] = {bluetooth, count_of(bluetooth)},
                      [icon_network] = {network, count_of(network)},
                      [icon_audio] = {speaker, count_of(speaker)},
                      [icon_muted] = {speaker, count_of(speaker)},
                      [icon_battery] = {battery, count_of(battery)},
                      [icon_memory] = {ram,count_of(ram)},
                      [icon_cpu] = {chip,count_of(chip)},
                      [icon_keyboard] = {keyboard,count_of(keyboard)},
                      [icon_clock] = {clock_face,count_of(clock_face)},
                      [icon_traffic] = {traffic,count_of(traffic)},
                      [icon_display] = {monitor,count_of(monitor)},
                      [icon_project] = {project,count_of(project)},
                      [icon_clipboard] = {clipboard,count_of(clipboard)},
                      [icon_bell] = {bell,count_of(bell)},
                      [icon_resize] = {resize,count_of(resize)},
                      [icon_awake] = {awake,count_of(awake)},
                      [icon_play] = {play, count_of(play)},
                      [icon_plug] = {plug, count_of(plug)},
                      [icon_quiet] = {speaker, count_of(speaker)},
                      [icon_ethernet] = {ethernet, count_of(ethernet)},
                      [icon_offline] = {offline, count_of(offline)},
                      [icon_left] = {left,count_of(left)}, [icon_right] = {right,count_of(right)},
                      [icon_up] = {up,count_of(up)}, [icon_down] = {down,count_of(down)},
                      [icon_more] = {more,count_of(more)}};
        for (int i = 1; i < icon_count; ++i) {
            PatBlt(dc, 0, 0, width, width, BLACKNESS);
            w_stroke(dc, shapes[i].path, shapes[i].count, width);
            if (i == icon_audio)
                w_stroke(dc, waves, count_of(waves), width);
            if (i == icon_muted)
                w_stroke(dc, cross, count_of(cross), width);
            GdiFlush();
            BYTE *mask = atlas->coverage + (size_t)i * size * size;
            for (int y = 0; y < size; ++y)
                for (int x = 0; x < size; ++x) {
                    unsigned sum = 0;
                    for (int yy = 0; yy < samples; ++yy)
                        for (int xx = 0; xx < samples; ++xx)
                            sum += bitmap[(samples * y + yy) * width + samples * x + xx] & 255;
                    mask[y * size + x] = (BYTE)(sum / (samples * samples));
                }
        }
        SelectObject(dc, old_pen);
        SelectObject(dc, old_bitmap);
    } else
        w_icons_free(atlas);
    DeleteObject(pen);
    DeleteObject(dib);
    DeleteDC(dc);
}

void w_icons_draw(const w_icon_atlas *atlas, HDC dc, bar_icon icon, RECT box, COLORREF ink, COLORREF paper) {
    int n = atlas->size;
    if (!n || icon <= icon_none || icon >= icon_count)
        return;
    const BYTE *mask = atlas->coverage + (size_t)icon * n * n;
    for (int i = 0; i < n * n; ++i) {
        int a = mask[i], b = 255 - a;
        int r = (a * GetRValue(ink) + b * GetRValue(paper)) / 255;
        int g = (a * GetGValue(ink) + b * GetGValue(paper)) / 255;
        int blue = (a * GetBValue(ink) + b * GetBValue(paper)) / 255;
        atlas->pixels[i] = (DWORD)(r << 16 | g << 8 | blue);
    }
    BITMAPINFO info = {.bmiHeader = {.biSize = sizeof(BITMAPINFOHEADER),
                                     .biWidth = n,
                                     .biHeight = -n,
                                     .biPlanes = 1,
                                     .biBitCount = 32,
                                     .biCompression = BI_RGB}};
    int saved = 0;
    if (box.right - box.left < n || box.bottom - box.top < n) {
        saved = SaveDC(dc);
        if (!saved) return;
        IntersectClipRect(dc, box.left, box.top, box.right, box.bottom);
    }
    SetDIBitsToDevice(dc, (box.left + box.right - n) / 2, (box.top + box.bottom - n) / 2, n, n, 0, 0, 0, n,
                      atlas->pixels, &info, DIB_RGB_COLORS);
    if (saved) RestoreDC(dc, saved);
}
