/* Win32 window: presents the GS display output, keyboard -> pad. */
#include "soft.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define FB_MAX 1024

static HWND hwnd;
static uint32_t *framebuf;
static int fb_w = 640, fb_h = 448;
static int have_frame;
static volatile int quit_requested;

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CLOSE:
        quit_requested = 1;
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        if (have_frame) {
            BITMAPINFO bi;
            memset(&bi, 0, sizeof bi);
            bi.bmiHeader.biSize = sizeof bi.bmiHeader;
            bi.bmiHeader.biWidth = FB_MAX;
            bi.bmiHeader.biHeight = -fb_h;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            SetStretchBltMode(dc, COLORONCOLOR);
            StretchDIBits(dc, 0, 0, rc.right, rc.bottom, 0, 0, fb_w, fb_h, framebuf, &bi,
                          DIB_RGB_COLORS, SRCCOPY);
        } else {
            FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    default:
        return DefWindowProcA(h, msg, wp, lp);
    }
}

void display_init(void)
{
    framebuf = calloc(FB_MAX * FB_MAX, 4);
    if (ps2_cfg.no_window)
        return;
    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "ps2native";
    RegisterClassA(&wc);
    int scale = ps2_cfg.scale > 0 ? ps2_cfg.scale : 1;
    RECT r = {0, 0, 640 * scale, 448 * scale};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    char title[300];
    snprintf(title, sizeof title, "%s - ps2native", ps2_game_name);
    hwnd = CreateWindowA("ps2native", title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                         r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
}

void display_present(void)
{
    int w, h;
    if (gs_read_display(framebuf, &w, &h, FB_MAX, FB_MAX)) {
        fb_w = w;
        fb_h = h;
        have_frame = 1;
    }
    if (hwnd)
        InvalidateRect(hwnd, NULL, FALSE);
}

void display_pump(void)
{
    MSG msg;
    if (!hwnd)
        return;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

int display_should_quit(void) { return quit_requested; }

/* PS2 pad bits (active low): SELECT L3 R3 START UP RIGHT DOWN LEFT
 * L2 R2 L1 R1 TRIANGLE CIRCLE CROSS SQUARE */
uint16_t display_pad_buttons(void)
{
    static const struct { int vk; uint16_t bit; } map[] = {
        {VK_BACK, 0x0001}, {VK_RETURN, 0x0008}, {VK_UP, 0x0010}, {VK_RIGHT, 0x0020},
        {VK_DOWN, 0x0040}, {VK_LEFT, 0x0080}, {'Q', 0x0100}, {'E', 0x0200},
        {'1', 0x0400}, {'3', 0x0800}, {'W', 0x1000}, {'D', 0x2000},
        {'S', 0x4000}, {'A', 0x8000},
    };
    uint16_t pressed = 0;
    if (hwnd && GetForegroundWindow() == hwnd)
        for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
            if (GetAsyncKeyState(map[i].vk) & 0x8000)
                pressed |= map[i].bit;
    return (uint16_t)~pressed;
}

void display_save_bmp(const char *path)
{
    int w, h;
    if (!gs_read_display(framebuf, &w, &h, FB_MAX, FB_MAX)) {
        w = fb_w;
        h = fb_h;
        if (!have_frame)
            memset(framebuf, 0, (size_t)FB_MAX * FB_MAX * 4);
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        ps2_log("cannot write %s", path);
        return;
    }
    uint32_t row = (uint32_t)(w * 3 + 3) & ~3u;
    uint32_t size = 54 + row * (uint32_t)h;
    uint8_t hdr[54] = {'B', 'M'};
    wr32le(hdr + 2, size);
    wr32le(hdr + 10, 54);
    wr32le(hdr + 14, 40);
    wr32le(hdr + 18, (uint32_t)w);
    wr32le(hdr + 22, (uint32_t)h);
    hdr[26] = 1;
    hdr[28] = 24;
    wr32le(hdr + 34, row * (uint32_t)h);
    fwrite(hdr, 1, 54, f);
    uint8_t *line = calloc(row, 1);
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            uint32_t c = framebuf[y * FB_MAX + x];
            line[x * 3 + 0] = (uint8_t)c;
            line[x * 3 + 1] = (uint8_t)(c >> 8);
            line[x * 3 + 2] = (uint8_t)(c >> 16);
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
    if (ps2_verbose)
        ps2_log("saved frame %dx%d to %s", w, h, path);
}
