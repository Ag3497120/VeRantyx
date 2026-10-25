// Input as Windows delivers it through child windows, driven from outside by tests/input_route_check.py (Command
// Clips, which acts on the top-level window only):
//   a top-level window with three children: A (left half), C (right half) and B over C's top half, which answers
//   WM_NCHITTEST with HTTRANSPARENT. A click on the top-level window lands in the child under the point (B's area: C,
//   beneath it), in that child's client coordinates; A captures the mouse on its button press.
//   Keys and characters go to the focus window (A, which takes the focus when clicked; Tab moves it to C). Ctrl+V in A
//   pastes the clipboard's text (GetKeyState sees Ctrl with the V), Ctrl+C copies made-up text to it. S in A has the
//   program send itself Z with SendInput: TranslateMessage makes 'z' of it, between Z's WM_KEYDOWN and WM_KEYUP.
//   The wheel goes to the focus (A), which leaves it to DefWindowProc, which hands it to the parent.
//   A composition committed in A is read with ImmGetCompositionString; in C, DefWindowProc makes WM_IME_CHAR and then
//   WM_CHAR of it.
// At WM_CLOSE it prints what each window got, for the check to compare.
#include <windows.h>
#include <imm.h>
#include <stdio.h>
#include <string.h>

static HWND top, a, b, c;
static char log_a[512], log_b[64], log_c[512], log_top[128];
static WCHAR typed_a[64], typed_c[64], ime_a[64];
static int na, nc, captured_up, ctrl_with_v, sendinput_order;
static void add(char *log, size_t cap, const char *fmt, int x, int y) {
    size_t n = strlen(log);
    snprintf(log + n, cap - n, fmt, x, y);
}
static void append(WCHAR *to, int *n, WCHAR ch) { if (*n < 63) { to[(*n)++] = ch; to[*n] = 0; } }

static LRESULT CALLBACK child_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
    switch (m) {
    case WM_NCHITTEST: return h == b ? HTTRANSPARENT : HTCLIENT;
    case WM_LBUTTONDOWN:
        if (h == a) { add(log_a, sizeof log_a, "down %d,%d;", x, y); SetFocus(a); SetCapture(a); }
        else add(h == b ? log_b : log_c, h == b ? sizeof log_b : sizeof log_c, "down %d,%d;", x, y);
        return 0;
    case WM_LBUTTONUP:
        if (h == a) { captured_up++; ReleaseCapture(); }
        return 0;
    case WM_KEYDOWN:
        if (h == a && wp == 'V' && GetKeyState(VK_CONTROL) < 0) {
            ctrl_with_v = 1;
            if (OpenClipboard(a)) {
                HANDLE d = GetClipboardData(CF_UNICODETEXT);
                const WCHAR *t = d ? GlobalLock(d) : NULL;
                for (int i = 0; t && t[i]; i++) append(typed_a, &na, t[i]);
                if (d) GlobalUnlock(d);
                CloseClipboard();
            }
        } else if (h == a && wp == 'C' && GetKeyState(VK_CONTROL) < 0) {
            static const WCHAR copy[] = L"made-up copy";
            HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, sizeof copy);
            memcpy(GlobalLock(g), copy, sizeof copy);
            GlobalUnlock(g);
            if (OpenClipboard(a)) { EmptyClipboard(); SetClipboardData(CF_UNICODETEXT, g); CloseClipboard(); }
        } else if (h == a && wp == VK_TAB) SetFocus(c);
        else if (h == a && wp == 'S') {
            INPUT in[2] = { { .type = INPUT_KEYBOARD }, { .type = INPUT_KEYBOARD } };
            in[0].ki.wVk = 'Z'; in[1].ki.wVk = 'Z'; in[1].ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(2, in, sizeof in[0]);
        } else if (h == a && wp == 'Z') sendinput_order = 1;
        return 0;
    case WM_KEYUP:
        if (h == a && wp == 'Z' && sendinput_order == 2) sendinput_order = 3;
        return 0;
    case WM_CHAR:
        if (h == a && wp == 'z' && sendinput_order == 1) { sendinput_order = 2; return 0; }
        if (h == a) append(typed_a, &na, (WCHAR)wp);
        else if (h == c) append(typed_c, &nc, (WCHAR)wp);
        return 0;
    case WM_IME_COMPOSITION:
        if (h == a && (lp & GCS_RESULTSTR)) {
            HIMC imc = ImmGetContext(a);
            LONG bytes = ImmGetCompositionStringW(imc, GCS_RESULTSTR, NULL, 0);
            if (bytes > 0 && bytes < (LONG)sizeof ime_a) ImmGetCompositionStringW(imc, GCS_RESULTSTR, ime_a, sizeof ime_a);
            ImmReleaseContext(a, imc);
            return 0;
        }
        break;
    }
    return DefWindowProcW(h, m, wp, lp);
}
static LRESULT CALLBACK top_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_MOUSEWHEEL: add(log_top, sizeof log_top, "wheel %d from A;", (short)HIWORD(wp), 0); return 0;
    case WM_LBUTTONDOWN: add(log_top, sizeof log_top, "down %d,%d;", (short)LOWORD(lp), (short)HIWORD(lp)); return 0;
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}
static void utf8(const WCHAR *w, char *out, int cap) { WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL); }

int main(void) {
    WNDCLASSW wc = { 0 };
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpfnWndProc = top_proc; wc.lpszClassName = L"InputRoute"; wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);
    wc.lpfnWndProc = child_proc; wc.lpszClassName = L"InputRouteChild";
    RegisterClassW(&wc);
    RECT r = { 0, 0, 300, 200 };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    top = CreateWindowW(L"InputRoute", L"Input routing test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, r.right - r.left, r.bottom - r.top,
                        NULL, NULL, wc.hInstance, NULL);
    a = CreateWindowW(L"InputRouteChild", L"A", WS_CHILD | WS_VISIBLE, 0, 0, 150, 200, top, (HMENU)1, wc.hInstance, NULL);
    c = CreateWindowW(L"InputRouteChild", L"C", WS_CHILD | WS_VISIBLE, 150, 0, 150, 200, top, (HMENU)3, wc.hInstance, NULL);
    b = CreateWindowW(L"InputRouteChild", L"B", WS_CHILD | WS_VISIBLE, 150, 0, 150, 100, top, (HMENU)2, wc.hInstance, NULL);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    char ta[256], tc[256], ia[256];
    utf8(typed_a, ta, sizeof ta); utf8(typed_c, tc, sizeof tc); utf8(ime_a, ia, sizeof ia);
    printf("A: %s up=%d ctrl+v=%d typed=[%s] ime=[%s] sendinput=%d\nB: %s\nC: %s typed=[%s]\ntop: %s\n", log_a, captured_up, ctrl_with_v,
           ta, ia, sendinput_order, log_b, log_c, tc, log_top);
    return 0;
}
