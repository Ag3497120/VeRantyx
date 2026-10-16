// Command Clips (src/control.c): a small window that tests/clip_check.py drives without looking at the screen. It
// draws, through a memory DC and BitBlt as most programs double-buffer, a counter, a "[ Add ]" button that a click
// increments it with, the characters typed, and takes R (the key) to reset the counter. It exits on WM_CLOSE.
#include <windows.h>
#include <stdio.h>
static int count;
static char typed[64];

static void paint(HWND h) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, 320, 160);
    HGDIOBJ old = SelectObject(mem, bmp);
    RECT all = { 0, 0, 320, 160 }, button = { 20, 60, 120, 90 };
    HBRUSH bg = CreateSolidBrush(RGB(240, 240, 240)), face = CreateSolidBrush(RGB(200, 210, 230));
    FillRect(mem, &all, bg);
    FillRect(mem, &button, face);
    SetBkMode(mem, TRANSPARENT);
    char line[96];
    snprintf(line, sizeof line, "Count: %d", count);
    TextOutA(mem, 20, 20, line, (int)strlen(line));
    DrawTextA(mem, "[ Add ]", -1, &button, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    snprintf(line, sizeof line, "Typed: %s", typed);
    TextOutA(mem, 20, 110, line, (int)strlen(line));
    BitBlt(dc, 0, 0, 320, 160, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp); DeleteObject(bg); DeleteObject(face); DeleteDC(mem);
    EndPaint(h, &ps);
}

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_PAINT: paint(h); return 0;
    case WM_LBUTTONDOWN: {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        if (x >= 20 && x < 120 && y >= 60 && y < 90) { count++; InvalidateRect(h, NULL, FALSE); }
        return 0;
    }
    case WM_KEYDOWN: if (wp == 'R') { count = 0; InvalidateRect(h, NULL, FALSE); } return 0;
    case WM_CHAR: {
        size_t n = strlen(typed);
        if (wp >= 0x20 && wp < 0x7F && n + 1 < sizeof typed) { typed[n] = (char)wp; typed[n + 1] = 0; InvalidateRect(h, NULL, FALSE); }
        return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(h, m, wp, lp);
}

int main(void) {
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = proc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "ClipTest";
    RegisterClassA(&wc);
    HWND h = CreateWindowA("ClipTest", "Command Clips test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 340, 200, NULL, NULL, wc.hInstance, NULL);
    if (!h) { printf("clip: no window\n"); return 1; }
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    printf("clip: count=%d typed=%s\n", count, typed);
    return 0;
}
