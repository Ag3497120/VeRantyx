// Fibers as job systems use them (src/sync.c): GetCurrentFiber and GetFiberData read inline from gs:0x20 as MSVC
// compiles them (__readgsqword(0x20)), also after the thread's priority was raised; a fiber switching straight to
// another fiber; and several threads taking turns running one shared set of fibers, so that a fiber leaves one
// thread and is resumed on another. No Man's Sky's job threads raise their priority, then switch to the value
// gs:0x20 gives: it was the thread's QoS value (0x10ff), and they faulted at 0x110f.
#include <windows.h>
#include <stdio.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("fiber_pool: FAILED %s (line %d)\n", #c, __LINE__); failures++; } } while (0)

static void *gs_fiber(void) { void *f; __asm__ volatile("movq %%gs:0x20, %0" : "=r"(f)); return f; }   // MSVC's GetCurrentFiber
static void *gs_fiber_data(void) { return *(void **)gs_fiber(); }                                      // MSVC's GetFiberData

// 1. A chain: main -> A -> B -> C -> main, each switching from inside the previous fiber.
static void *main_fiber, *chain[3];
static volatile int chain_seen[3], chain_ok = 1;
static void WINAPI chain_body(void *param) {
    int i = (int)(INT_PTR)param;
    if (gs_fiber() != chain[i] || gs_fiber_data() != param) chain_ok = 0;
    chain_seen[i]++;
    SwitchToFiber(i < 2 ? chain[i + 1] : main_fiber);
    for (;;) SwitchToFiber(main_fiber);
}

// 2. A pool: fibers in a shared queue, run by several threads. A fiber does a step, then switches back to the
// thread that ran it (its "home", set by that thread before the switch), which puts it back in the queue.
enum { THREADS = 4, FIBERS = 12, STEPS = 400 };
static CRITICAL_SECTION lock;
static void *pool[FIBERS], *queue[FIBERS];
static int queue_n;
static void *home[FIBERS];
static volatile LONG steps_done, wrong_current, migrations;
static DWORD last_thread[FIBERS];
static volatile LONG stop;
static void WINAPI pool_body(void *param) {
    int i = (int)(INT_PTR)param;
    volatile double acc = i;                              // (a local that must survive switches and threads)
    for (int step = 0;; step++) {
        if (gs_fiber() != pool[i] || gs_fiber_data() != param || GetCurrentFiber() != pool[i]) InterlockedIncrement(&wrong_current);
        DWORD t = GetCurrentThreadId();
        if (last_thread[i] && last_thread[i] != t) InterlockedIncrement(&migrations);
        last_thread[i] = t;
        acc += 1;
        if (acc != i + step + 1) InterlockedIncrement(&wrong_current);
        InterlockedIncrement(&steps_done);
        SwitchToFiber(home[i]);                           // (the thread queues it again: another may resume it)
    }
}
static DWORD WINAPI worker(void *arg) {
    (void)arg;
    void *me = ConvertThreadToFiber((void *)0x5eed);
    CHECK(me != NULL);
    CHECK(SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST));   // (rewrote gs:0x20 on macOS)
    CHECK(gs_fiber() == me && gs_fiber_data() == (void *)0x5eed);
    while (!stop) {
        void *f = NULL;
        EnterCriticalSection(&lock);
        if (queue_n) {
            int k = (GetCurrentThreadId() + queue_n) % queue_n;   // (not in order, so fibers change threads)
            f = queue[k]; queue[k] = queue[--queue_n];
        }
        LeaveCriticalSection(&lock);
        if (!f) { Sleep(0); continue; }
        for (int i = 0; i < FIBERS; i++) if (pool[i] == f) home[i] = me;
        SwitchToFiber(f);
        if (gs_fiber() != me) InterlockedIncrement(&wrong_current);
        EnterCriticalSection(&lock);
        queue[queue_n++] = f;                             // (once it has left: then it can be resumed elsewhere)
        LeaveCriticalSection(&lock);
        if (steps_done >= STEPS) stop = 1;
    }
    CHECK(ConvertFiberToThread());
    return 0;
}

int main(void) {
    main_fiber = ConvertThreadToFiber((void *)0x1234);
    CHECK(main_fiber != NULL);
    CHECK(gs_fiber() == main_fiber && gs_fiber_data() == (void *)0x1234);
    CHECK(GetCurrentFiber() == main_fiber);
    CHECK(SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL));
    CHECK(gs_fiber() == main_fiber);
    for (int i = 0; i < 3; i++) chain[i] = CreateFiber(0, chain_body, (void *)(INT_PTR)i);
    SwitchToFiber(chain[0]);
    CHECK(gs_fiber() == main_fiber);
    CHECK(chain_ok && chain_seen[0] == 1 && chain_seen[1] == 1 && chain_seen[2] == 1);
    printf("fiber_pool: chain main -> A -> B -> C -> main: %s\n", chain_ok ? "ok" : "wrong current fiber");

    InitializeCriticalSection(&lock);
    for (int i = 0; i < FIBERS; i++) { pool[i] = CreateFiber(64 << 10, pool_body, (void *)(INT_PTR)i); queue[queue_n++] = pool[i]; }
    HANDLE t[THREADS];
    for (int i = 0; i < THREADS; i++) t[i] = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    CHECK(WaitForMultipleObjects(THREADS, t, TRUE, 20000) == WAIT_OBJECT_0);
    CHECK(steps_done >= STEPS);
    CHECK(wrong_current == 0);
    CHECK(migrations > 0);
    printf("fiber_pool: %d fibers on %d threads: %ld steps, %ld moves between threads, %ld wrong\n", FIBERS, THREADS,
           steps_done, migrations, wrong_current);
    for (int i = 0; i < FIBERS; i++) DeleteFiber(pool[i]);
    for (int i = 0; i < 3; i++) DeleteFiber(chain[i]);
    if (failures) return 1;
    printf("fiber_pool: ok\n");
    return 0;
}
