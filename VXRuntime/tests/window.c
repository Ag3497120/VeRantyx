// Milestone 4a: a window on AppKit. Creation messages, timers, posted and cross-thread sent messages, painting
// with GDI (FillRect, StretchDIBits, a memory DC and BitBlt, text) checked with GetPixel, and the
// WM_CLOSE -> WM_DESTROY -> WM_QUIT shutdown. "window.exe --interactive" stays open and logs input instead.
#include <windows.h>
#include <stdio.h>
static int failures, interactive;
#define CHECK(c) do { if (!(c)) { printf("check failed (line %d): %s\n", __LINE__, #c); failures++; } } while (0)
static int created, sized, ticks, custom, painted, destroyed, sent_value;
static HWND g_hwnd;

static void draw_and_check(HWND h) {
    HDC dc = GetDC(h);
    RECT r = { 0, 0, 320, 240 };
    HBRUSH blue = CreateSolidBrush(RGB(0, 0, 255));
    FillRect(dc, &r, blue);
    DeleteObject(blue);
    CHECK(GetPixel(dc, 10, 10) == RGB(0, 0, 255));
    // A 4x2 bottom-up 32-bpp DIB: the first row in memory is the bottom one.
    static const DWORD px[8] = { 0x00FF0000, 0x00FF0000, 0x00FF0000, 0x00FF0000,     // bottom row: red (BGRX 0x00RRGGBB)
                                 0x0000FF00, 0x0000FF00, 0x0000FF00, 0x0000FF00 };   // top row: green
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), 4, 2, 1, 32, BI_RGB } };
    StretchDIBits(dc, 100, 100, 40, 20, 0, 0, 4, 2, px, &bi, DIB_RGB_COLORS, SRCCOPY);
    CHECK(GetPixel(dc, 110, 105) == RGB(0, 255, 0));          // top half green
    CHECK(GetPixel(dc, 110, 115) == RGB(255, 0, 0));          // bottom half red
    // Double buffering: draw into a memory DC, copy with BitBlt.
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, 50, 50);
    HGDIOBJ old = SelectObject(mem, bmp);
    RECT mr = { 0, 0, 50, 50 };
    HBRUSH yellow = CreateSolidBrush(RGB(255, 255, 0));
    FillRect(mem, &mr, yellow);
    SetPixel(mem, 5, 7, RGB(1, 2, 3));
    BitBlt(dc, 200, 20, 50, 50, mem, 0, 0, SRCCOPY);
    CHECK(GetPixel(dc, 220, 40) == RGB(255, 255, 0));
    CHECK(GetPixel(dc, 205, 27) == RGB(1, 2, 3));
    SelectObject(mem, old);
    DeleteObject(bmp); DeleteObject(yellow); DeleteDC(mem);
    // Text: something non-background gets drawn.
    SetTextColor(dc, RGB(255, 255, 255));
    SetBkMode(dc, TRANSPARENT);
    TextOutA(dc, 10, 200, "VXRuntime", 9);
    SIZE ext; GetTextExtentPoint32A(dc, "VXRuntime", 9, &ext);
    CHECK(ext.cx > 20 && ext.cy > 5);
    int lit = 0;
    for (int x = 10; x < 10 + ext.cx; x++) for (int y = 200; y < 200 + ext.cy; y++) lit += GetPixel(dc, x, y) != RGB(0, 0, 255);
    CHECK(lit > 10);
    ReleaseDC(h, dc);
}

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: created = ((CREATESTRUCTA *)l)->lpCreateParams == (void *)0x1234; return 0;
    case WM_SIZE: if (LOWORD(l) == 320 && HIWORD(l) == 240) sized = 1; return 0;
    case WM_TIMER:
        if (++ticks == 3) { KillTimer(h, 1); PostMessageA(h, WM_USER + 1, 42, 0); }
        return 0;
    case WM_USER + 1:
        custom = w == 42;
        draw_and_check(h);
        if (!interactive) PostMessageA(h, WM_CLOSE, 0, 0);
        return 0;
    case WM_USER + 2: return 777;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); painted++; EndPaint(h, &ps); return 0; }
    case WM_KEYDOWN: if (interactive) printf("WM_KEYDOWN vk=%#llx scan=%#llx\n", (unsigned long long)w, (unsigned long long)((l >> 16) & 0x1ff)); return 0;
    case WM_CHAR: if (interactive) printf("WM_CHAR %#llx\n", (unsigned long long)w); return 0;
    case WM_LBUTTONDOWN: if (interactive) printf("WM_LBUTTONDOWN %d,%d\n", (short)LOWORD(l), (short)HIWORD(l)); return 0;
    case WM_MOUSEWHEEL: if (interactive) printf("WM_MOUSEWHEEL %d\n", (short)HIWORD(w)); return 0;
    case WM_DESTROY: destroyed = 1; PostQuitMessage(5); return 0;
    }
    return DefWindowProcA(h, m, w, l);
}
static DWORD WINAPI sender(void *p) { (void)p; return (DWORD)SendMessageA(g_hwnd, WM_USER + 2, 0, 0); }

int main(int argc, char **argv) {
    interactive = argc > 1 && !strcmp(argv[1], "--interactive");
    WNDCLASSEXA wc = { sizeof wc, CS_HREDRAW | CS_VREDRAW, proc, 0, 0, GetModuleHandleA(NULL), NULL,
                       LoadCursor(NULL, IDC_ARROW), (HBRUSH)(COLOR_WINDOW + 1), NULL, "VXTestWindow", NULL };
    CHECK(RegisterClassExA(&wc) != 0);
    RECT r = { 0, 0, 320, 240 };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowExA(0, "VXTestWindow", "VXRuntime window test", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, (void *)0x1234);
    CHECK(g_hwnd != NULL);
    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);
    RECT cr; GetClientRect(g_hwnd, &cr);
    CHECK(cr.right == 320 && cr.bottom == 240);
    CHECK(IsWindow(g_hwnd) && IsWindowVisible(g_hwnd) && FindWindowA("VXTestWindow", NULL) == g_hwnd);
    SetTimer(g_hwnd, 1, 20, NULL);
    HANDLE t = CreateThread(NULL, 0, sender, NULL, 0, NULL);
    DWORD start = timeGetTime();
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    DWORD sent = 0;
    WaitForSingleObject(t, 5000);
    GetExitCodeThread(t, &sent);
    CHECK(sent == 777);
    CHECK(msg.wParam == 5);
    CHECK(!IsWindow(g_hwnd));
    printf("window: created=%d sized=%d timer ticks=%d posted=%d painted=%d destroyed=%d, %lu ms, %d failed checks -> %s\n",
           created, sized, ticks, custom, painted > 0, destroyed, timeGetTime() - start, failures,
           !failures && created && sized && ticks == 3 && custom && painted && destroyed ? "ok" : "FAILED");
    return failures;
}
