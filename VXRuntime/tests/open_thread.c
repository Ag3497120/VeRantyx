// OpenThread opens any running thread of the process by its ID, whether or not a handle to it is open (the main
// thread, which no CreateThread made; a thread whose CreateThread handle was closed at once), and fails with
// ERROR_INVALID_PARAMETER for an ID no thread has (one that ended with no handle left open).
// Valve's tier0 checks the threads it keeps data for this way: Steam's main thread could not be opened, and tier0
// freed its data (the coroutine manager) while it ran.
#include <windows.h>
#include <stdio.h>

static DWORD g_main_tid;
static HANDLE g_go;
static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_fail = 1; } } while (0)

// From another thread: the main thread can be opened and is running.
static DWORD WINAPI open_main(void *arg) {
    (void)arg;
    HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, g_main_tid);
    CHECK(h != NULL, "OpenThread(main thread %lu) failed, error %lu", g_main_tid, GetLastError());
    if (h) {
        DWORD code = 0;
        CHECK(GetExitCodeThread(h, &code) && code == STILL_ACTIVE, "main thread exit code %lu", code);
        CHECK(GetThreadId(h) == g_main_tid, "GetThreadId %lu", GetThreadId(h));
        CloseHandle(h);
    }
    return 0;
}

// A thread whose creator closed its handle at once: it waits until told to end.
static DWORD WINAPI waiter(void *arg) {
    (void)arg;
    WaitForSingleObject(g_go, INFINITE);
    return 7;
}

int main(void) {
    g_main_tid = GetCurrentThreadId();
    HANDLE t = CreateThread(NULL, 0, open_main, NULL, 0, NULL);
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);

    g_go = CreateEventA(NULL, TRUE, FALSE, NULL);
    DWORD tid = 0;
    t = CreateThread(NULL, 0, waiter, NULL, 0, &tid);
    CHECK(t != NULL, "CreateThread failed");
    CloseHandle(t);                                       // no handle to it is left open
    HANDLE h = OpenThread(THREAD_QUERY_INFORMATION | SYNCHRONIZE, FALSE, tid);
    CHECK(h != NULL, "OpenThread(running thread %lu, its handle closed) failed, error %lu", tid, GetLastError());
    DWORD code = 0;
    if (h) CHECK(GetExitCodeThread(h, &code) && code == STILL_ACTIVE, "running thread exit code %lu", code);
    SetEvent(g_go);
    if (h) {
        CHECK(WaitForSingleObject(h, 5000) == WAIT_OBJECT_0, "the opened thread's handle was not signalled when it ended");
        CHECK(GetExitCodeThread(h, &code) && code == 7, "ended thread exit code %lu", code);
        CloseHandle(h);
    }
    // Ended, and no handle open: its ID names no thread any more.
    HANDLE again = NULL;
    for (int i = 0; i < 200; i++) {
        again = OpenThread(THREAD_QUERY_INFORMATION, FALSE, tid);
        if (!again) break;
        CloseHandle(again);
        Sleep(10);
    }
    CHECK(again == NULL && GetLastError() == ERROR_INVALID_PARAMETER, "OpenThread(ended thread) gave %p, error %lu", again, GetLastError());

    // An ID that no thread has.
    SetLastError(0);
    h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, 0xfffffff0u);
    CHECK(h == NULL && GetLastError() == ERROR_INVALID_PARAMETER, "OpenThread(made-up ID) gave %p, error %lu", h, GetLastError());

    // The calling thread itself.
    h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, GetCurrentThreadId());
    CHECK(h != NULL, "OpenThread(itself) failed, error %lu", GetLastError());
    if (h) CloseHandle(h);

    printf(g_fail ? "open_thread: FAILED\n" : "open_thread: ok\n");
    return g_fail;
}
