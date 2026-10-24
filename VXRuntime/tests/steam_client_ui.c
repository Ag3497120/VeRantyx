// What the Windows Steam client (steam.exe, its vgui windows and tier0) needed past its "Steam Service Error" box,
// each as Microsoft documents it: TlsAlloc past the TEB's 64 slots (the 1024 expansion slots), GetClassWord(GCW_ATOM),
// GetClassInfo, WindowFromPoint, and SetWindowPos sending WM_WINDOWPOSCHANGING and WM_WINDOWPOSCHANGED to the window procedure.
// And what its web helper (Chromium) does many times a second, sampling its main thread: SuspendThread,
// GetThreadContext, GetThreadId, ResumeThread while that thread is busy in the runtime's own calls.
// Made-up class names and values only.
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("check failed (line %d): %s (last error %lu)\n", __LINE__, #c, GetLastError()); failures++; } } while (0)

// ---- TLS: 64 + 1024 indexes; values per thread; freed indexes come back -----------------------------------------------
static DWORD g_idx[200];
static DWORD WINAPI tls_other_thread(void *arg) {
    (void)arg;
    int ok = 1;
    for (int i = 0; i < 200; i++) if (TlsGetValue(g_idx[i]) != NULL) ok = 0;          // a new thread starts with NULLs
    for (int i = 0; i < 200; i++) if (!TlsSetValue(g_idx[i], (void *)(ULONG_PTR)(0x5000 + i))) ok = 0;
    for (int i = 0; i < 200; i++) if (TlsGetValue(g_idx[i]) != (void *)(ULONG_PTR)(0x5000 + i)) ok = 0;
    return ok;
}
static void test_tls(void) {
    int n = 0, distinct = 1;
    for (; n < 200; n++) { g_idx[n] = TlsAlloc(); if (g_idx[n] == TLS_OUT_OF_INDEXES) break; }
    CHECK(n == 200);                                                                     // more than the TEB's 64
    for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) if (g_idx[i] == g_idx[j]) distinct = 0;
    CHECK(distinct);
    int high = 0;
    for (int i = 0; i < n; i++) if (g_idx[i] >= 64) high++;
    CHECK(high > 0);
    for (int i = 0; i < n; i++) CHECK(TlsSetValue(g_idx[i], (void *)(ULONG_PTR)(0x1000 + i)));
    int same = 1;
    for (int i = 0; i < n; i++) if (TlsGetValue(g_idx[i]) != (void *)(ULONG_PTR)(0x1000 + i)) same = 0;
    CHECK(same);
    SetLastError(1234);
    CHECK(TlsGetValue(g_idx[n - 1]) != NULL && GetLastError() == 0);                     // success clears the last error
    HANDLE t = CreateThread(NULL, 0, tls_other_thread, NULL, 0, NULL);
    DWORD code = 0;
    CHECK(t && WaitForSingleObject(t, 10000) == WAIT_OBJECT_0 && GetExitCodeThread(t, &code) && code == 1);
    if (t) CloseHandle(t);
    same = 1;
    for (int i = 0; i < n; i++) if (TlsGetValue(g_idx[i]) != (void *)(ULONG_PTR)(0x1000 + i)) same = 0;
    CHECK(same);                                                                         // the other thread's are its own
    SetLastError(0);
    CHECK(TlsGetValue(1088) == NULL && GetLastError() == ERROR_INVALID_PARAMETER);       // past the last index
    CHECK(!TlsSetValue(1088, (void *)1) && GetLastError() == ERROR_INVALID_PARAMETER);
    DWORD last = g_idx[n - 1];
    CHECK(TlsFree(last));
    CHECK(!TlsFree(last));                                                               // not allocated now
    DWORD again = TlsAlloc();
    CHECK(again != TLS_OUT_OF_INDEXES);
    for (int i = 0; i < n - 1; i++) CHECK(TlsFree(g_idx[i]));
    CHECK(TlsFree(again));
}

// ---- windows ----------------------------------------------------------------------------------------------------------
static int g_changing, g_changed;
static WINDOWPOS g_last_changed;
static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_WINDOWPOSCHANGING) {
        WINDOWPOS *p = (WINDOWPOS *)lp;
        g_changing++;
        if (p->cx == 333) p->cx = 300;                                                   // the procedure may change it
    }
    if (m == WM_WINDOWPOSCHANGED) { g_changed++; g_last_changed = *(WINDOWPOS *)lp; }
    return DefWindowProcA(h, m, wp, lp);
}
static void test_windows(void) {
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = proc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "MadeUpVguiLikeClass";
    ATOM atom = RegisterClassA(&wc);
    CHECK(atom != 0);
    HWND h = CreateWindowExA(0, "MadeUpVguiLikeClass", "made-up", WS_POPUP, 100, 120, 200, 80, NULL, NULL, wc.hInstance, NULL);
    CHECK(h != NULL);
    if (!h) return;
    CHECK(GetClassWord(h, GCW_ATOM) == atom);
    WNDCLASSW cw; memset(&cw, 0xcc, sizeof cw);
    CHECK(GetClassInfoW(wc.hInstance, L"MadeUpVguiLikeClass", &cw) == atom);              // (no Ex: ANGLE's libGLESv2)
    CHECK(cw.lpfnWndProc == proc && cw.hInstance == wc.hInstance && cw.cbClsExtra == 0 && cw.lpszClassName != NULL);
    WNDCLASSA ca;
    CHECK(GetClassInfoA(wc.hInstance, "MadeUpVguiLikeClass", &ca) == atom && ca.lpfnWndProc == proc);
    CHECK(!GetClassInfoW(wc.hInstance, L"NoSuchMadeUpClass", &cw) && GetLastError() == ERROR_CLASS_DOES_NOT_EXIST);
    CHECK((ATOM)GetClassLongPtrW(h, GCW_ATOM) == atom);

    g_changing = g_changed = 0;
    CHECK(SetWindowPos(h, NULL, 140, 160, 333, 90, SWP_NOZORDER | SWP_NOACTIVATE));
    CHECK(g_changing == 1 && g_changed == 1);
    CHECK(g_last_changed.hwnd == h && g_last_changed.x == 140 && g_last_changed.y == 160 && g_last_changed.cx == 300 &&
          g_last_changed.cy == 90);
    RECT r; GetWindowRect(h, &r);
    CHECK(r.left == 140 && r.top == 160 && r.right - r.left == 300 && r.bottom - r.top == 90);
    g_changing = g_changed = 0;
    CHECK(SetWindowPos(h, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOSENDCHANGING));
    CHECK(g_changing == 0 && g_changed == 1);                                            // SWP_NOSENDCHANGING
    CHECK(g_last_changed.x == 140 && g_last_changed.cx == 300);                          // where it is
    g_changing = g_changed = 0;
    CHECK(MoveWindow(h, 150, 170, 210, 70, FALSE));
    CHECK(g_changing == 1 && g_changed == 1 && g_last_changed.x == 150 && g_last_changed.cy == 70);

    // WindowFromPoint: hidden windows are not found; a shown one is, at a point inside it only.
    GetWindowRect(h, &r);
    POINT inside = { r.left + 5, r.top + 5 }, outside = { r.right + 50, r.bottom + 50 };
    CHECK(WindowFromPoint(inside) != h);
    ShowWindow(h, SW_SHOWNOACTIVATE);
    GetWindowRect(h, &r);
    inside = (POINT){ r.left + 5, r.top + 5 }; outside = (POINT){ r.right + 50, r.bottom + 50 };
    CHECK(WindowFromPoint(inside) == h);
    CHECK(WindowFromPoint(outside) != h);
    EnableWindow(h, FALSE);
    CHECK(WindowFromPoint(inside) != h);                                                 // nor disabled ones
    EnableWindow(h, TRUE);
    DestroyWindow(h);
    CHECK(WindowFromPoint(inside) != h);
}

// ---- suspending a thread that is busy in the runtime (it posts completion packets and sets events) ---------------------
static volatile LONG g_stop_busy, g_busy_rounds;
static DWORD WINAPI busy_in_runtime(void *port) {
    HANDLE e = CreateEventA(NULL, TRUE, FALSE, NULL);
    while (!g_stop_busy) {
        InterlockedIncrement(&g_busy_rounds);
        PostQueuedCompletionStatus((HANDLE)port, 1, 2, NULL);
        SetEvent(e); ResetEvent(e);
    }
    CloseHandle(e);
    return 0;
}
static HANDLE g_busy;
static volatile LONG g_samples;
static DWORD WINAPI sampler(void *arg) {
    (void)arg;
    DWORD id = GetThreadId(g_busy);
    for (int i = 0; i < 2000; i++) {
        if (SuspendThread(g_busy) == (DWORD)-1) return 1;
        CONTEXT c; c.ContextFlags = CONTEXT_FULL;
        BOOL got = GetThreadContext(g_busy, &c);
        BOOL boost = FALSE;
        GetThreadPriorityBoost(g_busy, &boost);                     // (what Chromium's profiler calls meanwhile)
        if (GetThreadId(g_busy) != id) return 4;
        if (!got) { printf("GetThreadContext: %lu\n", GetLastError()); return 5; }
        if (!c.Rip) return 2;
        if (ResumeThread(g_busy) != 1) return 3;
        InterlockedIncrement(&g_samples);
    }
    return 0;
}
static void test_suspend_busy(void) {
    HANDLE port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    CHECK(port != NULL);
    g_busy = CreateThread(NULL, 0, busy_in_runtime, port, 0, NULL);
    while (g_busy_rounds < 10) Sleep(1);                             // (running in the runtime's calls)
    HANDLE s = CreateThread(NULL, 0, sampler, NULL, 0, NULL);
    CHECK(g_busy && s);
    if (!g_busy || !s) return;
    DWORD r = WaitForSingleObject(s, 30000), code = 9;
    if (r != WAIT_OBJECT_0) { printf("check failed: sampling a busy thread stuck after %ld samples\n", g_samples); fflush(stdout); ExitProcess(1); }
    GetExitCodeThread(s, &code);
    if (code) printf("sampler: code %lu after %ld samples\n", code, g_samples);
    CHECK(code == 0 && g_samples == 2000);
    g_stop_busy = 1;
    CHECK(WaitForSingleObject(g_busy, 10000) == WAIT_OBJECT_0);
    CloseHandle(s); CloseHandle(g_busy); CloseHandle(port);
}

int main(void) {
    test_suspend_busy();
    test_tls();
    test_windows();
    if (failures) { printf("steam_client_ui: %d checks failed\n", failures); return 1; }
    printf("steam_client_ui: all checks passed\n");
    return 0;
}
