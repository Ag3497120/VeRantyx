// Milestone 2a: threads, waits, events, mutexes, semaphores, condition variables, TLS.
#include <windows.h>
#include <stdio.h>

static CRITICAL_SECTION cs;
static volatile LONG counter, interlocked;
static HANDLE go, sem, mtx;
static DWORD tls_index;
static __thread int per_thread = 7;          // compiler TLS
static CONDITION_VARIABLE cv;
static SRWLOCK lock = SRWLOCK_INIT;
static int ready;

static DWORD WINAPI worker(void *arg) {
    int id = (int)(INT_PTR)arg;
    TlsSetValue(tls_index, (void *)(INT_PTR)(id * 10));
    per_thread = id;
    WaitForSingleObject(go, INFINITE);                      // manual-reset event: all start together
    for (int i = 0; i < 10000; i++) {
        EnterCriticalSection(&cs); counter++; LeaveCriticalSection(&cs);
        InterlockedIncrement(&interlocked);
    }
    WaitForSingleObject(mtx, INFINITE); ReleaseMutex(mtx);
    ReleaseSemaphore(sem, 1, NULL);
    if ((INT_PTR)TlsGetValue(tls_index) != id * 10 || per_thread != id) return 100 + id;
    return id;
}

static DWORD WINAPI waiter(void *arg) {
    AcquireSRWLockExclusive(&lock);
    while (!ready) SleepConditionVariableSRW(&cv, &lock, INFINITE, 0);
    ReleaseSRWLockExclusive(&lock);
    return 77;
}

// Fibers: two fibers and the main one take turns; each keeps its locals, floating point and a deep recursion.
static void *main_fiber, *fibers[2];
static int fiber_log[16], fiber_steps;
static int depth(int n) { volatile char pad[256]; pad[0] = (char)n; return n ? depth(n - 1) + pad[0] - (char)n + 1 : 0; }
static void WINAPI fiber_body(void *param) {
    int me = (int)(INT_PTR)param;
    double x = me * 1.5;
    for (int round = 0; round < 3; round++) {
        fiber_log[fiber_steps++] = me * 10 + round;
        x += depth(500) * 0.001;                                  // 0.5 per round
        SwitchToFiber(me == 1 ? fibers[1] : main_fiber);
    }
    fiber_log[fiber_steps++] = (int)(x * 10);                     // 1: 30, 2: 45
    SwitchToFiber(main_fiber);
}
static int fibers_ok(void) {
    main_fiber = ConvertThreadToFiber(NULL);
    if (!main_fiber || !IsThreadAFiber()) return 0;
    fibers[0] = CreateFiber(0, fiber_body, (void *)1);
    fibers[1] = CreateFiber(64 << 10, fiber_body, (void *)2);
    for (int i = 0; i < 4; i++) SwitchToFiber(fibers[0]);
    SwitchToFiber(fibers[1]);
    // fiber 1 switches to 2, 2 back to main: 10 20 11 21 12 22, then 1 ends (30), then 2 (45)
    static const int want[] = { 10, 20, 11, 21, 12, 22, 30, 45 };
    int ok = fiber_steps == 8;
    for (int i = 0; ok && i < 8; i++) ok = fiber_log[i] == want[i];
    if (!ok) { printf("fibers:"); for (int i = 0; i < fiber_steps; i++) printf(" %d", fiber_log[i]); printf("\n"); }
    DeleteFiber(fibers[0]); DeleteFiber(fibers[1]);
    return ok && ConvertFiberToThread() && !IsThreadAFiber();
}

int main(void) {
    InitializeCriticalSection(&cs);
    tls_index = TlsAlloc();
    go = CreateEventA(NULL, TRUE, FALSE, NULL);
    sem = CreateSemaphoreA(NULL, 0, 8, NULL);
    mtx = CreateMutexA(NULL, FALSE, NULL);
    HANDLE t[4];
    for (int i = 0; i < 4; i++) t[i] = CreateThread(NULL, 0, worker, (void *)(INT_PTR)(i + 1), 0, NULL);
    Sleep(20);
    SetEvent(go);
    DWORD w = WaitForMultipleObjects(4, t, TRUE, 10000);
    int ok = w == WAIT_OBJECT_0;
    for (int i = 0; i < 4; i++) {
        DWORD code = 0; GetExitCodeThread(t[i], &code);
        if (code != (DWORD)(i + 1)) { printf("thread %d exit code %lu\n", i + 1, code); ok = 0; }
        CloseHandle(t[i]);
    }
    int sems = 0;
    while (WaitForSingleObject(sem, 0) == WAIT_OBJECT_0) sems++;
    HANDLE wt = CreateThread(NULL, 0, waiter, NULL, 0, NULL);
    Sleep(20);
    AcquireSRWLockExclusive(&lock); ready = 1; ReleaseSRWLockExclusive(&lock);
    WakeAllConditionVariable(&cv);
    DWORD wcode = 0;
    if (WaitForSingleObject(wt, 5000) != WAIT_OBJECT_0) ok = 0;
    GetExitCodeThread(wt, &wcode);
    int fib = fibers_ok();
    ok = ok && counter == 40000 && interlocked == 40000 && sems == 4 && wcode == 77 && per_thread == 7 && fib;
    printf("threads: counter=%ld interlocked=%ld semaphores=%d condvar=%lu main_tls=%d fibers=%s -> %s\n",
           counter, interlocked, sems, wcode, per_thread, fib ? "ok" : "bad", ok ? "ok" : "FAILED");
    return !ok;
}
