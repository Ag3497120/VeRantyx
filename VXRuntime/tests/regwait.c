// Registered waits and timer-queue timers ended with a CompletionEvent (src/system.c), as Microsoft documents
// UnregisterWaitEx and DeleteTimerQueueTimer: an event handle is set once the callbacks running have returned, at once
// when none is; INVALID_HANDLE_VALUE waits for them. Chromium's ObjectWatcher stops watching (after a socket's connect
// or accept event, Steam's web helper) with UnregisterWaitEx(wait, event) and then waits on the event: it was never
// set, and that thread waited forever.
#include <windows.h>
#include <stdio.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("regwait: FAILED %s (line %d, last error %lu)\n", #c, __LINE__, GetLastError()); failures++; } } while (0)

static volatile LONG calls, in_callback;
static HANDLE gate;
static VOID CALLBACK on_signal(PVOID param, BOOLEAN timed_out) {
    (void)timed_out;
    InterlockedIncrement(&calls);
    if (param) {                                         // (holds the callback until the gate opens)
        InterlockedExchange(&in_callback, 1);
        WaitForSingleObject(gate, 5000);
        InterlockedExchange(&in_callback, 0);
    }
}
static VOID CALLBACK on_timer(PVOID param, BOOLEAN fired) { (void)param; (void)fired; InterlockedIncrement(&calls); }
static int wait_calls(LONG n) { for (int i = 0; i < 300 && calls < n; i++) Sleep(10); return calls >= n; }

int main(void) {
    HANDLE ev = CreateEventA(NULL, TRUE, FALSE, NULL), done = CreateEventA(NULL, TRUE, FALSE, NULL), wait = NULL;
    gate = CreateEventA(NULL, TRUE, FALSE, NULL);

    // 1. After the callback has run: the completion event is set at once.
    CHECK(RegisterWaitForSingleObject(&wait, ev, on_signal, NULL, INFINITE, WT_EXECUTEINWAITTHREAD | WT_EXECUTEONLYONCE));
    SetEvent(ev);
    CHECK(wait_calls(1));
    CHECK(UnregisterWaitEx(wait, done));
    CHECK(WaitForSingleObject(done, 2000) == WAIT_OBJECT_0);

    // 2. While the callback runs: the completion event waits for it to return.
    ResetEvent(ev); ResetEvent(done); calls = 0;
    CHECK(RegisterWaitForSingleObject(&wait, ev, on_signal, (PVOID)1, INFINITE, WT_EXECUTEONLYONCE));
    SetEvent(ev);
    for (int i = 0; i < 300 && !in_callback; i++) Sleep(10);
    CHECK(in_callback);
    CHECK(UnregisterWaitEx(wait, done));
    CHECK(WaitForSingleObject(done, 200) == WAIT_TIMEOUT);
    SetEvent(gate);
    CHECK(WaitForSingleObject(done, 5000) == WAIT_OBJECT_0);
    CHECK(!in_callback);

    // 3. INVALID_HANDLE_VALUE: returns once the callbacks have returned; a wait never signalled: at once.
    ResetEvent(ev); ResetEvent(gate); calls = 0;
    CHECK(RegisterWaitForSingleObject(&wait, ev, on_signal, (PVOID)1, INFINITE, WT_EXECUTEONLYONCE));
    SetEvent(ev);
    for (int i = 0; i < 300 && !in_callback; i++) Sleep(10);
    HANDLE t = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)(void *)SetEvent, gate, CREATE_SUSPENDED, NULL);
    ResumeThread(t);                                     // (opens the gate while UnregisterWaitEx waits)
    CHECK(UnregisterWaitEx(wait, INVALID_HANDLE_VALUE));
    CHECK(!in_callback && calls == 1);
    WaitForSingleObject(t, 5000); CloseHandle(t);
    HANDLE never = CreateEventA(NULL, TRUE, FALSE, NULL);
    ResetEvent(done);
    CHECK(RegisterWaitForSingleObject(&wait, never, on_signal, NULL, INFINITE, WT_EXECUTEONLYONCE));
    CHECK(UnregisterWaitEx(wait, done));
    CHECK(WaitForSingleObject(done, 2000) == WAIT_OBJECT_0);

    // 4. A timer-queue timer deleted with a completion event.
    HANDLE timer = NULL; calls = 0; ResetEvent(done);
    CHECK(CreateTimerQueueTimer(&timer, NULL, on_timer, NULL, 10, 0, 0));
    CHECK(wait_calls(1));
    CHECK(DeleteTimerQueueTimer(NULL, timer, done));
    CHECK(WaitForSingleObject(done, 2000) == WAIT_OBJECT_0);

    printf("regwait: UnregisterWaitEx and DeleteTimerQueueTimer completion events (after, during and without callbacks)"
           " -> %s\n", failures ? "FAILED" : "ok");
    return failures != 0;
}
