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
    printf("threads: counter=%ld interlocked=%ld semaphores=%d condvar=%lu main_tls=%d -> %s\n",
           counter, interlocked, sems, wcode, per_thread,
           ok && counter == 40000 && interlocked == 40000 && sems == 4 && wcode == 77 && per_thread == 7 ? "ok" : "FAILED");
    return 0;
}
