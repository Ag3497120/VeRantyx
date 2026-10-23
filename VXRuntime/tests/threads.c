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

// SuspendThread / GetThreadContext / ResumeThread on a running thread (a garbage collector's stop-the-world), and
// QueueUserAPC into an alertable wait.
static volatile LONG spins, spinning = 1;
static DWORD WINAPI spinner(void *arg) { (void)arg; while (spinning) InterlockedIncrement(&spins); return 5; }
static volatile LONG apc_ran;
static void WINAPI apc_fn(ULONG_PTR data) { apc_ran = (LONG)data; }
static DWORD WINAPI sleeper(void *arg) { (void)arg; DWORD r = SleepEx(INFINITE, TRUE); return r == WAIT_IO_COMPLETION && apc_ran == 42 ? 9 : 1; }
static int suspend_ok(void) {
    HANDLE t = CreateThread(NULL, 0, spinner, NULL, 0, NULL);
    while (spins < 1000) Sleep(1);
    int ok = SuspendThread(t) == 0;
    LONG a = spins; Sleep(30); LONG b = spins;             // stopped: no progress
    CONTEXT c; memset(&c, 0, sizeof c); c.ContextFlags = CONTEXT_FULL;
    ok = ok && GetThreadContext(t, &c) && c.Rsp && c.Rip && a == b;
    // (stopped in its loop: the program's own registers, not the runtime's)
    int in_loop = c.Rip >= (DWORD64)(uintptr_t)spinner && c.Rip < (DWORD64)(uintptr_t)spinner + 64;
    if (!in_loop) printf("suspended thread: rip %#llx, spinner at %p\n", (unsigned long long)c.Rip, (void *)spinner);
    ok = ok && in_loop;
    ok = ok && ResumeThread(t) == 1;
    Sleep(30);
    ok = ok && spins > b;                                    // running again
    spinning = 0;
    DWORD code = 0;
    ok = ok && WaitForSingleObject(t, 5000) == WAIT_OBJECT_0 && GetExitCodeThread(t, &code) && code == 5;
    CloseHandle(t);
    HANDLE s = CreateThread(NULL, 0, sleeper, NULL, 0, NULL);
    Sleep(20);
    ok = ok && QueueUserAPC(apc_fn, s, 42);
    ok = ok && WaitForSingleObject(s, 5000) == WAIT_OBJECT_0 && GetExitCodeThread(s, &code) && code == 9;
    CloseHandle(s);
    return ok;
}
// A thread stopped in the middle of pushes and pops (which the translator writes at offsets from one register and
// applies to RSP once, xlat.c's stack runs): GetThreadContext's Rsp must be the one of the instruction at Rip.
extern char pushloop[], pl_push1[], pl_push2[], pl_push3[], pl_pop1[], pl_pop2[], pl_pop3[], pl_cmp[], pl_end[];
extern DWORD64 pl_base, pl_count;
__asm__(".globl pushloop, pl_push1, pl_push2, pl_push3, pl_pop1, pl_pop2, pl_pop3, pl_cmp, pl_end, pl_base, pl_count\n"
        ".data\npl_base: .quad 0\npl_count: .quad 0\n.text\n"
        "pushloop: movq %rsp, pl_base(%rip)\n"
        "pl_push1: pushq %rbx\npl_push2: pushq %rsi\npl_push3: pushq %rdi\n"
        "pl_pop1: popq %rdi\npl_pop2: popq %rsi\npl_pop3: popq %rbx\n"
        "pl_cmp: incq pl_count(%rip)\n cmpl $0, (%rcx)\n je pl_push1\npl_end: ret\n");
static volatile LONG pl_stop;
static int stack_run_inside;
static DWORD WINAPI pusher(void *arg) { (void)arg; ((void (*)(volatile LONG *))(void *)pushloop)(&pl_stop); return 3; }
static int stack_run_ok(void) {
    HANDLE t = CreateThread(NULL, 0, pusher, NULL, 0, NULL);
    while (!*(volatile DWORD64 *)&pl_base) Sleep(1);
    const char *at[] = { pl_push1, pl_push2, pl_push3, pl_pop1, pl_pop2, pl_pop3, pl_cmp, pl_end };
    static const int depth[] = { 0, 8, 16, 24, 16, 8, 0 };
    int seen[8] = { 0 }, wrong = 0, inside = 0;
    DWORD t0 = GetTickCount();
    for (int k = 0; k < 2000; k++) {
        if (SuspendThread(t) != 0) { wrong++; break; }
        CONTEXT c; memset(&c, 0, sizeof c); c.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(t, &c)) {
            for (int i = 0; i < 7; i++)
                if (c.Rip >= (DWORD64)(uintptr_t)at[i] && c.Rip < (DWORD64)(uintptr_t)at[i + 1]) {
                    seen[i]++; inside += i >= 1 && i <= 5;
                    if (c.Rsp != pl_base - (DWORD64)depth[i]) {
                        if (wrong < 3) printf("stack run: rip at step %d, rsp %#llx, want %#llx\n", i, (unsigned long long)c.Rsp, (unsigned long long)(pl_base - depth[i]));
                        wrong++;
                    }
                }
        }
        DWORD64 before = *(volatile DWORD64 *)&pl_count;
        ResumeThread(t);
        for (int w = 0; w < 1000000 && *(volatile DWORD64 *)&pl_count < before + 1 + (DWORD64)(k % 7); w++) {}   // (it runs a little)
    }
    if (getenv("PL_DEBUG")) fprintf(stderr, "stack run: 2000 stops in %lu ms (%d %d %d %d %d %d %d)\n", (unsigned long)(GetTickCount() - t0), seen[0], seen[1], seen[2], seen[3], seen[4], seen[5], seen[6]);
    pl_stop = 1;
    DWORD code = 0;
    int ok = WaitForSingleObject(t, 5000) == WAIT_OBJECT_0 && GetExitCodeThread(t, &code) && code == 3;
    CloseHandle(t);
    stack_run_inside = inside;                               // (the x86 build under Rosetta stops between blocks: none)
    return ok && !wrong;
}

// The lowest page of this thread's stack found as the Boehm GC finds it (walking down from the TEB's stack base while
// pages are PAGE_READWRITE and not guard pages) must be below the current stack pointer.
static int stack_walk_ok(void) {
    char *base = (char *)((NT_TIB *)NtCurrentTeb())->StackBase, *bottom = base;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(base - 1, &mbi, sizeof mbi) || !(mbi.Protect & PAGE_READWRITE)) return 0;
    do {
        bottom = (char *)mbi.BaseAddress;
        if (!VirtualQuery(bottom - 1, &mbi, sizeof mbi)) break;
    } while ((mbi.Protect & PAGE_READWRITE) && !(mbi.Protect & PAGE_GUARD));
    volatile char here = 0;
    return bottom <= (char *)&here && (char *)&here < base;
}
// Locks in a structure packed to 4 bytes (SteamVR's vrserver has such critical sections): none of them may need
// 8-byte alignment.
#pragma pack(push, 4)
static struct { int pad; CRITICAL_SECTION cs; SRWLOCK srw; CONDITION_VARIABLE cv; } packed __attribute__((aligned(8)));   // (cs: 4 bytes off)
#pragma pack(pop)
static volatile LONG packed_cs_count, packed_srw_count;
static DWORD WINAPI packed_worker(void *arg) {
    (void)arg;
    for (int i = 0; i < 2000; i++) {
        EnterCriticalSection(&packed.cs); EnterCriticalSection(&packed.cs); packed_cs_count++; LeaveCriticalSection(&packed.cs); LeaveCriticalSection(&packed.cs);
        AcquireSRWLockExclusive(&packed.srw); packed_srw_count++; ReleaseSRWLockExclusive(&packed.srw);
        AcquireSRWLockShared(&packed.srw); ReleaseSRWLockShared(&packed.srw);
    }
    return 0;
}
static int packed_ok(void) {
    InitializeCriticalSection(&packed.cs); InitializeSRWLock(&packed.srw); InitializeConditionVariable(&packed.cv);
    HANDLE t[3];
    for (int i = 0; i < 3; i++) t[i] = CreateThread(NULL, 0, packed_worker, NULL, 0, NULL);
    WaitForMultipleObjects(3, t, TRUE, 10000);
    for (int i = 0; i < 3; i++) CloseHandle(t[i]);
    EnterCriticalSection(&packed.cs);
    WakeAllConditionVariable(&packed.cv);
    BOOL woke = SleepConditionVariableCS(&packed.cv, &packed.cs, 1);   // (nobody wakes it: times out)
    LeaveCriticalSection(&packed.cs);
    DeleteCriticalSection(&packed.cs);
    return packed_cs_count == 6000 && packed_srw_count == 6000 && !woke;
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
    int fib = fibers_ok(), sus = suspend_ok(), walk = stack_walk_ok(), pk = packed_ok(), run = stack_run_ok();
    // Closing a pseudo-handle (process, thread, and the token ones Chromium closes: -4..-6) is a no-op that succeeds.
    for (INT_PTR h = -1; h >= -6; h--) if (!CloseHandle((HANDLE)h)) { printf("CloseHandle(%d) failed\n", (int)h); ok = 0; }
    ok = ok && counter == 40000 && interlocked == 40000 && sems == 4 && wcode == 77 && per_thread == 7 && fib && sus && walk && pk && run;
    printf("threads: counter=%ld interlocked=%ld semaphores=%d condvar=%lu main_tls=%d fibers=%s suspend+apc=%s stack=%s packed locks=%s stack runs=%s (%d stops inside) -> %s\n",
           counter, interlocked, sems, wcode, per_thread, fib ? "ok" : "bad", sus ? "ok" : "bad", walk ? "ok" : "bad", pk ? "ok" : "bad", run ? "ok" : "bad", stack_run_inside, ok ? "ok" : "FAILED");
    return !ok;
}
