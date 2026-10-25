// Thread-pool callback instances (src/system.c), as Microsoft documents them: SetEventWhenCallbackReturns,
// ReleaseSemaphoreWhenCallbackReturns, ReleaseMutexWhenCallbackReturns, LeaveCriticalSectionWhenCallbackReturns and
// FreeLibraryWhenCallbackReturns act once the callback has returned (not when called); CallbackMayRunLong returns TRUE
// and other work still runs while that callback blocks; DisassociateCurrentThreadFromCallback lets
// WaitForThreadpoolWorkCallbacks return while the callback still runs. No Man's Sky calls CallbackMayRunLong.
#include <windows.h>
#include <stdio.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("tp_callbacks: FAILED %s (line %d)\n", #c, __LINE__); failures++; } } while (0)

static HANDLE ev, sem, gate, mutex;
static CRITICAL_SECTION cs;
static volatile LONG early_event, early_sem, long_ok, quick_runs, disassociated, mutex_owned;

static VOID CALLBACK actions(PTP_CALLBACK_INSTANCE in, PVOID ctx, PTP_WORK w) {
    (void)ctx; (void)w;
    SetEventWhenCallbackReturns(in, ev);
    ReleaseSemaphoreWhenCallbackReturns(in, sem, 2);
    EnterCriticalSection(&cs);
    LeaveCriticalSectionWhenCallbackReturns(in, &cs);
    mutex_owned = WaitForSingleObject(mutex, 0) == WAIT_OBJECT_0;
    ReleaseMutexWhenCallbackReturns(in, mutex);
    Sleep(50);
    early_event = WaitForSingleObject(ev, 0) == WAIT_OBJECT_0;       // not yet: only once this returns
    early_sem = WaitForSingleObject(sem, 0) == WAIT_OBJECT_0;
}
static VOID CALLBACK long_one(PTP_CALLBACK_INSTANCE in, PVOID ctx, PTP_WORK w) {
    (void)ctx; (void)w;
    long_ok = CallbackMayRunLong(in);
    WaitForSingleObject(gate, 5000);
}
static VOID CALLBACK quick(PTP_CALLBACK_INSTANCE in, PVOID ctx, PTP_WORK w) { (void)in; (void)ctx; (void)w; InterlockedIncrement(&quick_runs); }
static VOID CALLBACK detach(PTP_CALLBACK_INSTANCE in, PVOID ctx, PTP_WORK w) {
    (void)ctx; (void)w;
    DisassociateCurrentThreadFromCallback(in);
    InterlockedExchange(&disassociated, 1);
    WaitForSingleObject(gate, 5000);
}
static VOID CALLBACK simple(PTP_CALLBACK_INSTANCE in, PVOID ctx) { SetEventWhenCallbackReturns(in, (HANDLE)ctx); }

int main(void) {
    ev = CreateEventA(NULL, TRUE, FALSE, NULL);
    sem = CreateSemaphoreA(NULL, 0, 10, NULL);
    gate = CreateEventA(NULL, TRUE, FALSE, NULL);
    mutex = CreateMutexA(NULL, FALSE, NULL);
    InitializeCriticalSection(&cs);

    // 1. The actions happen after the callback returns.
    PTP_WORK w = CreateThreadpoolWork(actions, NULL, NULL);
    CHECK(w != NULL);
    SubmitThreadpoolWork(w);
    WaitForThreadpoolWorkCallbacks(w, FALSE);
    CHECK(!early_event); CHECK(!early_sem); CHECK(mutex_owned);
    CHECK(WaitForSingleObject(ev, 2000) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(sem, 0) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(sem, 0) == WAIT_OBJECT_0);             // released by 2
    CHECK(WaitForSingleObject(sem, 0) == WAIT_TIMEOUT);
    CHECK(TryEnterCriticalSection(&cs)); LeaveCriticalSection(&cs);    // left by the pool
    CHECK(WaitForSingleObject(mutex, 0) == WAIT_OBJECT_0); ReleaseMutex(mutex);   // released by the worker
    CloseThreadpoolWork(w);

    // 2. CallbackMayRunLong: TRUE, and many quick items still run while the long ones block.
    PTP_WORK longs[16], q = CreateThreadpoolWork(quick, NULL, NULL);
    for (int i = 0; i < 16; i++) { longs[i] = CreateThreadpoolWork(long_one, NULL, NULL); SubmitThreadpoolWork(longs[i]); }
    Sleep(200);
    for (int i = 0; i < 8; i++) SubmitThreadpoolWork(q);
    for (int i = 0; i < 300 && quick_runs < 8; i++) Sleep(10);
    CHECK(long_ok);
    CHECK(quick_runs == 8);
    SetEvent(gate);
    for (int i = 0; i < 16; i++) { WaitForThreadpoolWorkCallbacks(longs[i], FALSE); CloseThreadpoolWork(longs[i]); }
    WaitForThreadpoolWorkCallbacks(q, FALSE); CloseThreadpoolWork(q);
    ResetEvent(gate);

    // 3. DisassociateCurrentThreadFromCallback: the wait for callbacks returns while this one still runs.
    PTP_WORK d = CreateThreadpoolWork(detach, NULL, NULL);
    SubmitThreadpoolWork(d);
    for (int i = 0; i < 300 && !disassociated; i++) Sleep(10);
    DWORD t0 = GetTickCount();
    WaitForThreadpoolWorkCallbacks(d, FALSE);
    CHECK(GetTickCount() - t0 < 2000);
    CloseThreadpoolWork(d);
    SetEvent(gate);
    Sleep(50);

    // 4. A simple callback (TrySubmitThreadpoolCallback) gets an instance too.
    HANDLE e2 = CreateEventA(NULL, TRUE, FALSE, NULL);
    CHECK(TrySubmitThreadpoolCallback(simple, e2, NULL));
    CHECK(WaitForSingleObject(e2, 2000) == WAIT_OBJECT_0);

    if (failures) return 1;
    printf("tp_callbacks: ok\n");
    return 0;
}
