// GDI to the screen (src/present.c), checked: made-up gradients drawn into a window through every path software
// presentation uses (BitBlt from top-down and bottom-up DIB sections, a DIB section over a file mapping, StretchBlt,
// SetDIBitsToDevice in bands, StretchDIBits, AlphaBlend, TransparentBlt, viewport origin and clip region, GetDIBits /
// SetDIBits, DSTINVERT), WM_PAINT with an update region (BeginPaint's clip, GetUpdateRect / GetUpdateRgn, ValidateRect),
// ScrollWindowEx, raster operations that read no source (from their truth tables, the source DC NULL), DefWindowProc's
// WM_ERASEBKGND with a class brush (only the update region erased: InvalidateRect of a part, GetUpdateRect with bErase,
// ScrollWindowEx with SW_ERASE; a memory DC passed gets the background), a child window, layered windows
// (UpdateLayeredWindow with per-pixel alpha, SetLayeredWindowAttributes with a color key and alpha), DwmFlush, and the
// cost of a 1134x704 frame (BitBlt and ReleaseDC). The program checks
// what GDI drew (GetPixel); tests/present_check.py checks through Command Clips what the Mac windows show, then closes
// the main window. Alone ("present.exe --alone") it closes itself.
#include <windows.h>
#include <dwmapi.h>
#include <stdio.h>
#include <string.h>
static int failures;
#define CHECK(c) do { if (!(c)) { printf("check failed (line %d): %s\n", __LINE__, #c); failures++; } } while (0)
#define CHECKPX(dc, x, y, want) do { COLORREF _g = GetPixel(dc, x, y), _w = (want); if (_g != _w) { printf("check failed (line %d): pixel (%d, %d) is %06lx, not %06lx\n", __LINE__, x, y, (unsigned long)_g, (unsigned long)_w); failures++; } } while (0)

enum { W = 320, H = 240, PW = 1134, PH = 704 };
static HWND g_main, g_child, g_alpha, g_key, g_perf;
static int g_phase, g_paint_ok = -1;

// The made-up gradient of a 64x64 tile: R = 4x, G = 4y, B = 0x40 (as a BGRA pixel and as a COLORREF).
static DWORD grad(int x, int y) { return 0xff000000u | (DWORD)((x * 4) & 0xff) << 16 | (DWORD)((y * 4) & 0xff) << 8 | 0x40; }
static COLORREF grad_cr(int x, int y) { return RGB((x * 4) & 0xff, (y * 4) & 0xff, 0x40); }
static BITMAPINFO info(int w, int h, int bpp) { BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), w, h, 1, (WORD)bpp, BI_RGB } }; return bi; }
static HBITMAP dib(HDC dc, int w, int h, DWORD **bits) { BITMAPINFO bi = info(w, h, 32); return CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)bits, NULL, 0); }
static void fill(HDC dc, int l, int t, int r, int b, COLORREF c) { RECT rc = { l, t, r, b }; HBRUSH br = CreateSolidBrush(c); FillRect(dc, &rc, br); DeleteObject(br); }

static void draw_tiles(void) {
    HDC dc = GetDC(g_main), mem = CreateCompatibleDC(dc);
    DWORD *px;
    // A (0, 0): BitBlt from a top-down DIB section
    HBITMAP a = dib(dc, 64, -64, &px), old = SelectObject(mem, a);
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) px[y * 64 + x] = grad(x, y);
    GdiFlush();
    CHECK(BitBlt(dc, 0, 0, 64, 64, mem, 0, 0, SRCCOPY));
    // B (64, 0): from a bottom-up one (its first row in memory is the bottom one)
    HBITMAP b = dib(dc, 64, 64, &px);
    SelectObject(mem, b);
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) px[(63 - y) * 64 + x] = grad(x, y);
    CHECK(BitBlt(dc, 64, 0, 64, 64, mem, 0, 0, SRCCOPY));
    // C (128, 0): StretchBlt of a 32x32 tile to twice its size (each source pixel 2x2)
    HBITMAP c = dib(dc, 32, -32, &px);
    SelectObject(mem, c);
    for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) px[y * 32 + x] = grad(2 * x, 2 * y);
    SetStretchBltMode(dc, COLORONCOLOR);
    CHECK(StretchBlt(dc, 128, 0, 64, 64, mem, 0, 0, 32, 32, SRCCOPY));
    // D (192, 0): SetDIBitsToDevice from a bottom-up DIB, in two bands of 32 scan lines (the bottom one first)
    static DWORD up[64 * 64];
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) up[(63 - y) * 64 + x] = grad(x, y);
    BITMAPINFO bi = info(64, 64, 32);
    CHECK(SetDIBitsToDevice(dc, 192, 0, 64, 64, 0, 0, 0, 32, up, &bi, DIB_RGB_COLORS) == 32);
    CHECK(SetDIBitsToDevice(dc, 192, 0, 64, 64, 0, 0, 32, 32, up + 32 * 64, &bi, DIB_RGB_COLORS) == 32);
    // E (256, 0): StretchDIBits of a top-down 32x32 DIB to 64x64
    static DWORD small[32 * 32];
    for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) small[y * 32 + x] = grad(2 * x, 2 * y);
    BITMAPINFO bs = info(32, -32, 32);
    CHECK(StretchDIBits(dc, 256, 0, 64, 64, 0, 0, 32, 32, small, &bs, DIB_RGB_COLORS, SRCCOPY) == 32);
    // F (0, 64): a DIB section over a file mapping at an offset that is not a page boundary; the pixels are written
    // through another view of the mapping after the section was made
    HANDLE map = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 64 * 64 * 4 + 8192, NULL);
    CHECK(map != NULL);
    DWORD *view = MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    BITMAPINFO bf = info(64, -64, 32);
    DWORD *sec = NULL;
    HBITMAP f = CreateDIBSection(dc, &bf, DIB_RGB_COLORS, (void **)&sec, map, 4100);
    CHECK(f && sec && view);
    if (view && sec) {
        DWORD *at = (DWORD *)((BYTE *)view + 4100);
        for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) at[y * 64 + x] = grad(x, y);
        CHECK(sec[5 * 64 + 7] == grad(7, 5));
        DIBSECTION ds;
        CHECK(GetObjectA(f, sizeof ds, &ds) == sizeof ds && ds.dshSection == map && ds.dsOffset == 4100 && ds.dsBm.bmBits == sec);
        SelectObject(mem, f);
        CHECK(BitBlt(dc, 0, 64, 64, 64, mem, 0, 0, SRCCOPY));
    }
    // G (64, 64): AlphaBlend of premultiplied white at half alpha over blue: (128, 128, 128 + 127)
    fill(dc, 64, 64, 128, 128, RGB(0, 0, 255));
    HBITMAP g = dib(dc, 64, -64, &px);
    SelectObject(mem, g);
    for (int i = 0; i < 64 * 64; i++) px[i] = 0x80808080;
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    CHECK(AlphaBlend(dc, 64, 64, 64, 64, mem, 0, 0, 64, 64, blend));
    CHECK(!AlphaBlend(dc, 64, 64, 64, 64, mem, 8, 8, 64, 64, blend));          // (the source rectangle is not inside)
    // H (128, 64): TransparentBlt leaves out the magenta of the source: a green square over red
    fill(dc, 128, 64, 192, 128, RGB(255, 0, 0));
    HBITMAP hb = dib(dc, 64, -64, &px);
    SelectObject(mem, hb);
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) px[y * 64 + x] = x >= 16 && x < 48 && y >= 16 && y < 48 ? 0xff00ff00 : 0xffff00ff;
    CHECK(TransparentBlt(dc, 128, 64, 64, 64, mem, 0, 0, 64, 64, RGB(255, 0, 255)));
    // I (192, 64): the viewport origin moves drawing, the clip region (logical units) limits it, SaveDC keeps both
    fill(dc, 192, 64, 256, 128, RGB(128, 128, 128));
    int saved = SaveDC(dc);
    POINT o;
    SetViewportOrgEx(dc, 192, 64, NULL);
    CHECK(IntersectClipRect(dc, 0, 0, 32, 32) == SIMPLEREGION);
    RECT box;
    CHECK(GetClipBox(dc, &box) == SIMPLEREGION && box.left == 0 && box.top == 0 && box.right == 32 && box.bottom == 32);
    fill(dc, 0, 0, 64, 64, RGB(0, 255, 255));
    CHECK(RestoreDC(dc, saved) && GetViewportOrgEx(dc, &o) && o.x == 0 && o.y == 0);
    CHECK(GetClipBox(dc, &box) == SIMPLEREGION && box.right == W && box.bottom == H);
    // J (256, 64): SetDIBits / GetDIBits through a bitmap, BitBlt, then DSTINVERT on its lower half
    HBITMAP j = CreateCompatibleBitmap(dc, 64, 64);
    BITMAPINFO bj = info(64, 64, 32);
    CHECK(SetDIBits(dc, j, 0, 64, up, &bj, DIB_RGB_COLORS) == 64);
    static BYTE back[64 * 64 * 3];
    BITMAPINFO b24 = info(64, -64, 24);
    CHECK(GetDIBits(dc, j, 0, 64, back, &b24, DIB_RGB_COLORS) == 64);
    CHECK(back[(5 * 64 + 7) * 3 + 2] == 28 && back[(5 * 64 + 7) * 3 + 1] == 20 && back[(5 * 64 + 7) * 3] == 0x40);
    BITMAPINFO bq = { { sizeof(BITMAPINFOHEADER) } };
    CHECK(GetDIBits(dc, j, 0, 0, NULL, &bq, DIB_RGB_COLORS) && bq.bmiHeader.biWidth == 64 && bq.bmiHeader.biBitCount == 32);
    SelectObject(mem, j);
    CHECK(BitBlt(dc, 256, 64, 64, 64, mem, 0, 0, SRCCOPY));
    CHECK(PatBlt(dc, 256, 96, 64, 32, DSTINVERT));
    SelectObject(mem, old);
    DeleteObject(a); DeleteObject(b); DeleteObject(c); DeleteObject(f); DeleteObject(g); DeleteObject(hb); DeleteObject(j);
    if (view) UnmapViewOfFile(view);
    CloseHandle(map);
    DeleteDC(mem);
    // what GDI drew
    CHECKPX(dc, 7, 5, grad_cr(7, 5)); CHECKPX(dc, 63, 63, grad_cr(63, 63));
    CHECKPX(dc, 64 + 7, 5, grad_cr(7, 5)); CHECKPX(dc, 64 + 60, 2, grad_cr(60, 2)); CHECKPX(dc, 64 + 3, 61, grad_cr(3, 61));
    CHECKPX(dc, 128 + 7, 5, grad_cr(6, 4)); CHECKPX(dc, 128 + 63, 63, grad_cr(62, 62));
    CHECKPX(dc, 192 + 7, 5, grad_cr(7, 5)); CHECKPX(dc, 192 + 9, 40, grad_cr(9, 40));
    CHECKPX(dc, 256 + 7, 5, grad_cr(6, 4)); CHECKPX(dc, 256 + 40, 63, grad_cr(40, 62));
    CHECKPX(dc, 9, 64 + 33, grad_cr(9, 33));
    CHECKPX(dc, 64 + 20, 64 + 20, RGB(128, 128, 255));
    CHECKPX(dc, 128 + 20, 64 + 20, RGB(0, 255, 0)); CHECKPX(dc, 128 + 4, 64 + 4, RGB(255, 0, 0));
    CHECKPX(dc, 192 + 10, 64 + 10, RGB(0, 255, 255)); CHECKPX(dc, 192 + 40, 64 + 10, RGB(128, 128, 128)); CHECKPX(dc, 192 + 10, 64 + 40, RGB(128, 128, 128));
    CHECKPX(dc, 256 + 7, 64 + 5, grad_cr(7, 5)); CHECKPX(dc, 256 + 7, 64 + 40, RGB(255 - 28, 255 - 160, 0xbf));
    ReleaseDC(g_main, dc);
}

// WM_PAINT: what was invalidated (less what was validated) is BeginPaint's clip region and rcPaint.
static void paint_test(void) {
    HDC dc = GetDC(g_main);
    fill(dc, 0, 128, 160, 240, RGB(255, 255, 255));
    ReleaseDC(g_main, dc);
    RECT r1 = { 0, 128, 80, 240 }, r2 = { 80, 128, 160, 184 }, v = { 0, 128, 80, 160 }, u;
    InvalidateRect(g_main, &r1, FALSE); InvalidateRect(g_main, &r2, FALSE); ValidateRect(g_main, &v);
    CHECK(GetUpdateRect(g_main, &u, FALSE) && u.left == 0 && u.top == 128 && u.right == 160 && u.bottom == 240);
    HRGN rgn = CreateRectRgn(0, 0, 0, 0);
    CHECK(GetUpdateRgn(g_main, rgn, FALSE) == COMPLEXREGION && PtInRegion(rgn, 40, 200) && !PtInRegion(rgn, 40, 140) && !PtInRegion(rgn, 120, 220));
    DeleteObject(rgn);
    g_phase = 1;
    UpdateWindow(g_main);
    g_phase = 0;
    CHECK(g_paint_ok == 1);
    CHECK(!GetUpdateRect(g_main, &u, FALSE));
    dc = GetDC(g_main);
    CHECKPX(dc, 40, 140, RGB(255, 255, 255)); CHECKPX(dc, 40, 200, RGB(0, 100, 0)); CHECKPX(dc, 120, 150, RGB(0, 100, 0)); CHECKPX(dc, 120, 220, RGB(255, 255, 255));
    ReleaseDC(g_main, dc);
}

// ScrollWindowEx: stripes moved up 16 rows inside their rectangle; the uncovered rows are the update region.
static void scroll_test(void) {
    HDC dc = GetDC(g_main);
    for (int k = 0; k < 112; k++) fill(dc, 160, 128 + k, 320, 129 + k, RGB(2 * k, 0x80, 0x10));
    ReleaseDC(g_main, dc);
    RECT s = { 160, 128, 320, 240 }, upd;
    HRGN rgn = CreateRectRgn(0, 0, 0, 0);
    CHECK(ScrollWindowEx(g_main, 0, -16, &s, &s, rgn, &upd, SW_INVALIDATE) == SIMPLEREGION);
    CHECK(upd.left == 160 && upd.top == 224 && upd.right == 320 && upd.bottom == 240);
    RECT u;
    CHECK(GetUpdateRect(g_main, &u, FALSE) && u.top == 224 && u.bottom == 240 && u.left == 160);
    ValidateRect(g_main, NULL);
    DeleteObject(rgn);
    dc = GetDC(g_main);
    fill(dc, 160, 224, 320, 240, RGB(0, 0, 0));
    CHECKPX(dc, 200, 130, RGB(36, 0x80, 0x10)); CHECKPX(dc, 300, 223, RGB(2 * 111, 0x80, 0x10)); CHECKPX(dc, 300, 230, RGB(0, 0, 0));
    ReleaseDC(g_main, dc);
}

// Raster operations from their truth tables on a DIB section (made-up colors; the three color bytes compared): those
// that read no source with a NULL source DC, two that read the source, and SRCCOPY without a source (an error).
static void rop_test(void) {
    HDC screen = GetDC(NULL), mem = CreateCompatibleDC(screen), src = CreateCompatibleDC(screen);
    DWORD *px, *spx;
    HBITMAP bm = dib(screen, 8, -1, &px), old = SelectObject(mem, bm);
    HBITMAP sb = dib(screen, 8, -1, &spx), sold = SelectObject(src, sb);
    for (int i = 0; i < 8; i++) { px[i] = 0xff3c5a96; spx[i] = 0xffa5c3e1; }      // D = (3c, 5a, 96), S = (a5, c3, e1)
    HBRUSH br = CreateSolidBrush(RGB(0x0f, 0xf0, 0x33)), oldbr = SelectObject(mem, br);   // P = (0f, f0, 33)
    static const struct { DWORD rop; int with_src; DWORD want; } ops[8] = {
        { 0x00AA0029, 0, 0x3c5a96 },                                // D (nothing changes)
        { 0x000A0329, 0, 0x300a84 },                                // DPna: D & ~P
        { PATCOPY, 0, 0x0ff033 },                                   // P
        { 0x00FA0089, 0, 0x3ffab7 },                                // DPo: D | P
        { BLACKNESS, 0, 0x000000 },
        { WHITENESS, 0, 0xffffff },
        { MERGECOPY, 1, 0x05c021 },                                 // P & S
        { SRCINVERT, 1, 0x999977 },                                 // S ^ D
    };
    GdiFlush();
    CHECK(!BitBlt(mem, 0, 0, 1, 1, NULL, 0, 0, SRCCOPY));          // (SRCCOPY reads a source: none given)
    for (int i = 0; i < 8; i++) {
        if (i == 4) CHECK(StretchBlt(mem, i, 0, 1, 1, NULL, 0, 0, 0, 0, ops[i].rop));
        else CHECK(BitBlt(mem, i, 0, 1, 1, ops[i].with_src ? src : NULL, i, 0, ops[i].rop));
    }
    GdiFlush();
    for (int i = 0; i < 8; i++)
        if ((px[i] & 0xffffff) != ops[i].want) { printf("check failed: ROP %08lx gives %06lx, not %06lx\n", (unsigned long)ops[i].rop, (unsigned long)(px[i] & 0xffffff), (unsigned long)ops[i].want); failures++; }
    SelectObject(mem, oldbr); DeleteObject(br);
    SelectObject(src, sold); DeleteObject(sb); DeleteDC(src);
    SelectObject(mem, old); DeleteObject(bm); DeleteDC(mem); ReleaseDC(NULL, screen);
}

// WM_ERASEBKGND by DefWindowProc for a class with a background brush: erased through the DC the message carries, so
// only the update region is. A window filled with one color (made up) gets three parts invalidated with erasing and
// painted by a WM_PAINT that draws nothing: everything else must keep its color.
enum { EW = 128, EH = 64 };
#define C_FACE RGB(180, 90, 60)
#define C_BACK RGB(40, 80, 150)
#define C_MOVED RGB(60, 150, 60)
static HWND g_erase;
static int g_erase_phase, g_erase_flag = -1;
static RECT g_erase_paint;
static const RECT ER_A = { 8, 8, 40, 32 }, ER_B = { 48, 8, 80, 32 }, ER_S = { 88, 0, 128, 64 };
// The picture the test leaves (drawn again by any later WM_PAINT, clipped to its update region).
static void erase_picture(HDC dc) {
    fill(dc, 0, 0, EW, EH, C_FACE);
    fill(dc, ER_A.left, ER_A.top, ER_A.right, ER_A.bottom, C_BACK);
    fill(dc, ER_B.left, ER_B.top, ER_B.right, ER_B.bottom, C_BACK);
    fill(dc, ER_S.left, ER_S.top, ER_S.right, ER_S.bottom - 16, C_MOVED);
    fill(dc, ER_S.left, ER_S.bottom - 16, ER_S.right, ER_S.bottom, C_BACK);
}
static LRESULT CALLBACK erase_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        g_erase_flag = ps.fErase; g_erase_paint = ps.rcPaint;
        if (g_erase_phase == 0) fill(dc, 0, 0, EW, EH, C_FACE);
        else if (g_erase_phase == 2) erase_picture(dc);
        EndPaint(h, &ps);                                           // (phase 1: nothing drawn)
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}
static void erase_test(void) {
    g_erase = CreateWindowExA(WS_EX_TOOLWINDOW, "VXPresentErase", "VX present erase", WS_POPUP, 420, 320, EW, EH, NULL, NULL, NULL, NULL);
    ShowWindow(g_erase, SW_SHOWNOACTIVATE);
    UpdateWindow(g_erase);
    HDC dc = GetDC(g_erase);
    fill(dc, 0, 0, EW, EH, C_FACE);
    ReleaseDC(g_erase, dc);
    ValidateRect(g_erase, NULL);
    g_erase_phase = 1;
    // a part invalidated with erasing: BeginPaint's WM_ERASEBKGND erases that part, and DefWindowProc did the erasing
    InvalidateRect(g_erase, &ER_A, TRUE);
    g_erase_flag = -1;
    UpdateWindow(g_erase);
    CHECK(g_erase_flag == 0 && EqualRect(&g_erase_paint, &ER_A));
    // GetUpdateRect with bErase: the pending erasing done there, on the update region only
    RECT u;
    InvalidateRect(g_erase, &ER_B, TRUE);
    CHECK(GetUpdateRect(g_erase, &u, TRUE) && EqualRect(&u, &ER_B));
    ValidateRect(g_erase, NULL);
    // ScrollWindowEx with SW_ERASE: the rows uncovered erased, those moved kept
    dc = GetDC(g_erase);
    fill(dc, ER_S.left, ER_S.top, ER_S.right, ER_S.bottom, C_MOVED);
    ReleaseDC(g_erase, dc);
    CHECK(ScrollWindowEx(g_erase, 0, -16, &ER_S, &ER_S, NULL, NULL, SW_INVALIDATE | SW_ERASE) == SIMPLEREGION);
    g_erase_flag = -1;
    UpdateWindow(g_erase);
    CHECK(g_erase_flag == 0 && g_erase_paint.left == 88 && g_erase_paint.top == 48 && g_erase_paint.right == 128 && g_erase_paint.bottom == 64);
    // DefWindowProc given a memory DC: the memory bitmap gets the background, the window nothing
    HDC screen = GetDC(NULL), mem = CreateCompatibleDC(screen);
    DWORD *px;
    HBITMAP bm = dib(screen, EW, -EH, &px), old = SelectObject(mem, bm);
    memset(px, 0, EW * EH * 4);
    CHECK(DefWindowProcA(g_erase, WM_ERASEBKGND, (WPARAM)mem, 0) != 0);
    GdiFlush();
    CHECK((px[0] & 0xffffff) == 0x285096 && (px[EW * EH - 1] & 0xffffff) == 0x285096);   // C_BACK as a pixel
    SelectObject(mem, old); DeleteObject(bm); DeleteDC(mem); ReleaseDC(NULL, screen);
    g_erase_phase = 2;
    dc = GetDC(g_erase);
    CHECKPX(dc, 4, 4, C_FACE); CHECKPX(dc, 20, 20, C_BACK); CHECKPX(dc, 44, 20, C_FACE); CHECKPX(dc, 60, 20, C_BACK);
    CHECKPX(dc, 60, 50, C_FACE); CHECKPX(dc, 100, 20, C_MOVED); CHECKPX(dc, 100, 56, C_BACK); CHECKPX(dc, 4, 56, C_FACE);
    ReleaseDC(g_erase, dc);
}

static LRESULT CALLBACK main_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (g_phase == 1) {
            g_paint_ok = ps.rcPaint.left == 0 && ps.rcPaint.top == 128 && ps.rcPaint.right == 160 && ps.rcPaint.bottom == 240;
            fill(dc, 0, 0, W, H, RGB(0, 100, 0));                   // (clipped to the update region)
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(h, m, w, l);
}
static LRESULT CALLBACK child_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); fill(dc, 0, 0, 48, 12, RGB(255, 255, 0)); fill(dc, 0, 12, 48, 24, RGB(255, 128, 0)); EndPaint(h, &ps); return 0; }
    return DefWindowProcA(h, m, w, l);
}
static LRESULT CALLBACK key_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        fill(dc, 0, 0, 48, 48, RGB(0, 0, 255)); fill(dc, 16, 16, 32, 32, RGB(255, 0, 255));
        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}
static void pump(void) { MSG msg; while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); } }
static ATOM klass(const char *name, WNDPROC proc, HBRUSH background) {
    WNDCLASSEXA wc = { sizeof wc, 0, proc, 0, 0, GetModuleHandleA(NULL), NULL, LoadCursor(NULL, IDC_ARROW), background, NULL, name, NULL };
    return RegisterClassExA(&wc);
}

static void layered(void) {
    // UpdateLayeredWindow: left half opaque red, right half green at half alpha (premultiplied)
    g_alpha = CreateWindowExA(WS_EX_LAYERED | WS_EX_TOOLWINDOW, "VXPresentPopup", "VX layered alpha", WS_POPUP, 40, 40, 10, 10, NULL, NULL, NULL, NULL);
    HDC screen = GetDC(NULL), mem = CreateCompatibleDC(screen);
    DWORD *px;
    HBITMAP bm = dib(screen, 64, -64, &px), old = SelectObject(mem, bm);
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) px[y * 64 + x] = x < 32 ? 0xffff0000 : 0x80008000;
    POINT dst = { 100, 100 }, src = { 0, 0 };
    SIZE size = { 64, 64 };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    ShowWindow(g_alpha, SW_SHOWNOACTIVATE);
    CHECK(UpdateLayeredWindow(g_alpha, screen, &dst, &size, mem, &src, 0, &blend, ULW_ALPHA));
    RECT r;
    CHECK(GetWindowRect(g_alpha, &r) && r.left == 100 && r.top == 100 && r.right - r.left == 64 && r.bottom - r.top == 64);
    COLORREF key; BYTE a; DWORD fl;
    CHECK(!GetLayeredWindowAttributes(g_alpha, &key, &a, &fl));        // (set up by UpdateLayeredWindow)
    CHECK(!SetLayeredWindowAttributes(g_main, 0, 128, LWA_ALPHA));     // (not WS_EX_LAYERED)
    CHECK(BitBlt(screen, 0, 0, 4, 4, mem, 0, 0, SRCCOPY));               // (the screen: accepted, nothing drawn here)
    SelectObject(mem, old); DeleteObject(bm); DeleteDC(mem); ReleaseDC(NULL, screen);
    // SetLayeredWindowAttributes: painted with WM_PAINT, magenta transparent, the whole at half alpha
    g_key = CreateWindowExA(WS_EX_LAYERED | WS_EX_TOOLWINDOW, "VXPresentKey", "VX layered key", WS_POPUP, 200, 100, 48, 48, NULL, NULL, NULL, NULL);
    CHECK(SetLayeredWindowAttributes(g_key, RGB(255, 0, 255), 128, LWA_COLORKEY | LWA_ALPHA));
    CHECK(GetLayeredWindowAttributes(g_key, &key, &a, &fl) && key == RGB(255, 0, 255) && a == 128 && fl == (LWA_COLORKEY | LWA_ALPHA));
    ShowWindow(g_key, SW_SHOWNOACTIVATE);
    UpdateWindow(g_key);
}

// The cost of a frame as Chromium's software output gives it: a DIB section blitted to the window's DC.
static double perf(int *frames_out, DWORD *last) {
    g_perf = CreateWindowExA(0, "VXPresentMain", "VX present perf", WS_POPUP | WS_VISIBLE, 0, 0, PW, PH, NULL, NULL, NULL, NULL);
    pump();
    HDC dc = GetDC(g_perf), mem = CreateCompatibleDC(dc);
    DWORD *px;
    HBITMAP bm = dib(dc, PW, -PH, &px), old = SelectObject(mem, bm);
    ReleaseDC(g_perf, dc);
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    double total = 0;
    int frames = 120;
    for (int k = 0; k < frames; k++) {
        DWORD v = 0xff000000u | (DWORD)(k & 0xff) << 16 | 0x2000 | (DWORD)(255 - (k & 0xff));
        for (int i = 0; i < PW * PH; i++) px[i] = v;
        QueryPerformanceCounter(&t0);
        dc = GetDC(g_perf);
        BitBlt(dc, 0, 0, PW, PH, mem, 0, 0, SRCCOPY);
        ReleaseDC(g_perf, dc);
        QueryPerformanceCounter(&t1);
        total += (double)(t1.QuadPart - t0.QuadPart) / f.QuadPart;
        *last = v;
        if (k % 8 == 7) Sleep(1);
    }
    SelectObject(mem, old); DeleteObject(bm); DeleteDC(mem);
    *frames_out = frames;
    return total / frames * 1e6;
}

int main(int argc, char **argv) {
    int alone = argc > 1 && !strcmp(argv[1], "--alone");
    CHECK(klass("VXPresentMain", main_proc, NULL) && klass("VXPresentChild", child_proc, NULL) && klass("VXPresentPopup", DefWindowProcA, NULL) &&
          klass("VXPresentKey", key_proc, NULL) && klass("VXPresentErase", erase_proc, CreateSolidBrush(C_BACK)));
    RECT r = { 0, 0, W, H };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_main = CreateWindowExA(0, "VXPresentMain", "VX present test", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, NULL, NULL);
    ShowWindow(g_main, SW_SHOW);
    UpdateWindow(g_main);
    pump();
    draw_tiles();
    paint_test();
    scroll_test();
    rop_test();
    erase_test();
    g_child = CreateWindowExA(0, "VXPresentChild", "", WS_CHILD | WS_VISIBLE, 8, 210, 48, 24, g_main, NULL, NULL, NULL);
    UpdateWindow(g_child);
    POINT o = { 0, 0 }, c = { 0, 0 };
    ClientToScreen(g_main, &o); ClientToScreen(g_child, &c);
    CHECK(c.x == o.x + 8 && c.y == o.y + 210);
    RECT cr;
    CHECK(GetWindowRect(g_child, &cr) && cr.left == o.x + 8 && cr.top == o.y + 210 && cr.right == o.x + 56);
    CHECK(WindowFromPoint((POINT){ o.x + 20, o.y + 220 }) == g_child);
    layered();
    int frames; DWORD last = 0;
    double us = perf(&frames, &last);
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    for (int i = 0; i < 5; i++) CHECK(DwmFlush() == S_OK);
    QueryPerformanceCounter(&t1);
    double flush_ms = (double)(t1.QuadPart - t0.QuadPart) / f.QuadPart * 1e3 / 5;
    CHECK(flush_ms > 1 && flush_ms < 60);
    printf("present: ready; perf frame %08lx\n", (unsigned long)last);
    fflush(stdout);
    if (alone) PostMessageA(g_main, WM_CLOSE, 0, 0);
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    printf("present: 10 tiles, WM_PAINT update region, ScrollWindowEx, 8 ROP3 codes, WM_ERASEBKGND on the update region, child, 2 layered windows; %dx%d frame by BitBlt + ReleaseDC %.0f us (%d frames), DwmFlush %.1f ms; %d failed checks -> %s\n",
           PW, PH, us, frames, flush_ms, failures, failures ? "FAILED" : "ok");
    return failures;
}
