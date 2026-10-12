// Threads and kernel objects: threads, events, mutexes, semaphores, waits, critical sections, SRW locks,
// condition variables, one-time initialization.
//
// Kernel objects live in this process (there is no server): a handle indexes a table of reference-counted
// objects, and every state change wakes all waiters through one condition variable; each waiter then re-checks
// what it waits for. Simple and correct; per-object wait queues are a later optimisation.
#include <errno.h>
#include <fcntl.h>
#include <os/os_sync_wait_on_address.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sched.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "vx.h"

#define INFINITE      0xffffffffu
#define WAIT_TIMEOUT  0x102u
#define WAIT_FAILED   0xffffffffu
#define STILL_ACTIVE  259u
#define CREATE_SUSPENDED 4u

enum { OBJ_THREAD = 1, OBJ_EVENT, OBJ_MUTEX, OBJ_SEMAPHORE, OBJ_OTHER, OBJ_PORT, OBJ_PROCESS };
typedef struct {
    int type, refs;
    int done, started; DWORD suspended, exit_code, tid; pthread_t pthread;     // thread
    DWORD (WINAPI *start)(void *); void *arg;
    int manual, signaled;                                                     // event
    DWORD owner, recursion;                                                   // mutex
    LONG count, max;                                                          // semaphore
    int kind; void *data; void (*destroy)(void *);                            // other modules' objects
    char *name;                                                               // named events, mutexes, semaphores
    struct shared_state *sh;                                                  // a named object's state, shared between processes
    struct packet *packets, **packets_tail; int closed;                       // I/O completion port
} vx_object;
typedef struct packet { DWORD bytes, status; uint64_t key; void *overlapped; struct packet *next; } packet;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_changed = PTHREAD_COND_INITIALIZER;
#define HANDLE_BASE 0x100
#define MAX_HANDLES 8192             // handles 0x100..0x80fc; file handles start at 0x10000
static vx_object *g_handles[MAX_HANDLES];
static vx_object g_never;            // what the pseudo handles of the current process/thread wait on

DWORD vx_current_tid(void) { uint64_t t; pthread_threadid_np(NULL, &t); return (DWORD)t; }

// All of the following run with g_lock held.
static HANDLE new_handle(vx_object *o) {
    for (int i = 0; i < MAX_HANDLES; i++)
        if (!g_handles[i]) { g_handles[i] = o; o->refs++; return (HANDLE)(intptr_t)(HANDLE_BASE + i * 4); }
    return NULL;
}
static int handle_index(HANDLE h) {
    intptr_t v = (intptr_t)h - HANDLE_BASE;
    return v >= 0 && v % 4 == 0 && v / 4 < MAX_HANDLES ? (int)(v / 4) : -1;
}
#define INJECT_BASE 0x20000
#define INJECT_COUNT 4096
static vx_object *g_injected[INJECT_COUNT];    // handles other processes gave this one (see DuplicateHandle)
static vx_object *injected(HANDLE h);
static vx_object *lookup(HANDLE h) {
    int i = handle_index(h);
    if (i >= 0) return g_handles[i];
    return injected(h);
}
// ---- named objects between processes -----------------------------------------------------------------------------
// A named event, mutex or semaphore keeps its state in POSIX shared memory named after the object's type and name
// ("Local\\" and "Global\\" dropped), so every vxrt process opening the name shares it. A spin lock guards the
// state; `seq` changes with every change, and waiters in other processes sleep on it (os_sync_wait_on_address,
// shared) or poll it. `refs` counts the processes' objects; the last one removes the name.
typedef struct shared_state {
    volatile uint32_t magic, spin, seq;
    int32_t type, manual, signaled, count, max;
    volatile int32_t refs;
    uint32_t owner, recursion;
} shared_state;
#define SHARED_MAGIC 0x56584f42u
static void shared_name(const char *name, int type, char *out, size_t cap) {
    if (!strncasecmp(name, "Local\\", 6)) name += 6;
    else if (!strncasecmp(name, "Global\\", 7)) name += 7;
    uint64_t h = 1469598103934665603ull ^ (uint64_t)type;
    for (const char *c = name; *c; c++) h = (h ^ (uint8_t)*c) * 1099511628211ull;
    snprintf(out, cap, "/vxo.%016llx", (unsigned long long)h);
}
// Who holds a name: a registry of process IDs beside each named object or mapping ("/vxr.<hash>"). A name whose
// holders have all ended (killed, crashed) is gone, as on Windows, even though its shared memory is still there.
typedef struct { volatile uint32_t spin; int32_t pids[1000]; } name_registry;
static name_registry *registry_open(const char *object_shm) {
    char reg[40];
    snprintf(reg, sizeof reg, "/vxr.%s", object_shm + 1);
    int fd = shm_open(reg, O_RDWR | O_CREAT, 0600);
    if (fd < 0) return NULL;
    struct stat st;
    if (!fstat(fd, &st) && st.st_size < (off_t)sizeof(name_registry)) ftruncate(fd, sizeof(name_registry));
    name_registry *r = mmap(NULL, sizeof(name_registry), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    return r == MAP_FAILED ? NULL : r;
}
// Joins a name; returns how many holds of it there were (this process's earlier ones included) by live processes.
int vx_name_join(const char *object_shm) {
    name_registry *r = registry_open(object_shm);
    if (!r) return 0;
    while (__atomic_exchange_n(&r->spin, 1, __ATOMIC_ACQUIRE)) sched_yield();
    int others = 0, me = getpid(), free_slot = -1;
    for (int i = 0; i < 1000; i++) {
        if (r->pids[i] && r->pids[i] != me && kill(r->pids[i], 0) && errno == ESRCH) r->pids[i] = 0;   // ended
        if (!r->pids[i]) { if (free_slot < 0) free_slot = i; }
        else others++;
    }
    if (free_slot >= 0) r->pids[free_slot] = me;
    __atomic_store_n(&r->spin, 0, __ATOMIC_RELEASE);
    munmap(r, sizeof *r);
    return others;
}
// Leaves a name; returns 1 when no process holds it any more (the caller removes the object).
int vx_name_leave(const char *object_shm) {
    name_registry *r = registry_open(object_shm);
    if (!r) return 1;
    while (__atomic_exchange_n(&r->spin, 1, __ATOMIC_ACQUIRE)) sched_yield();
    int me = getpid(), left = 0, removed = 0;
    for (int i = 0; i < 1000; i++) {
        if (r->pids[i] == me && !removed) { r->pids[i] = 0; removed = 1; continue; }
        if (r->pids[i] && (r->pids[i] == me || !kill(r->pids[i], 0) || errno != ESRCH)) left++;
    }
    __atomic_store_n(&r->spin, 0, __ATOMIC_RELEASE);
    munmap(r, sizeof *r);
    if (!left) { char reg[40]; snprintf(reg, sizeof reg, "/vxr.%s", object_shm + 1); shm_unlink(reg); }
    return !left;
}
// Attaches to the state of a name: created if `create` (and *existed says whether a live process holds it), else
// only opened.
static shared_state *shared_attach(const char *name, int type, int create, int *existed) {
    char shm[40];
    shared_name(name, type, shm, sizeof shm);
    *existed = 0;
    int others = vx_name_join(shm);
    if (!others) shm_unlink(shm);                                  // left behind by processes that ended
    if (!others && !create) { vx_name_leave(shm); return NULL; }
    int fd = shm_open(shm, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) { fd = shm_open(shm, O_RDWR, 0600); if (fd >= 0) *existed = 1; }
    if (fd < 0) { vx_name_leave(shm); return NULL; }
    if (!*existed) ftruncate(fd, 4096);
    shared_state *sh = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (sh == MAP_FAILED) { vx_name_leave(shm); return NULL; }
    if (*existed) for (int i = 0; i < 100000 && __atomic_load_n(&sh->magic, __ATOMIC_ACQUIRE) != SHARED_MAGIC; i++) sched_yield();
    return sh;
}
static void shared_detach(shared_state *sh, const char *name, int type) {
    char shm[40];
    shared_name(name, type, shm, sizeof shm);
    if (vx_name_leave(shm)) shm_unlink(shm);
    munmap(sh, 4096);
}
static void spin_lock(shared_state *sh) { while (__atomic_exchange_n(&sh->spin, 1, __ATOMIC_ACQUIRE)) sched_yield(); }
static void spin_unlock(shared_state *sh) { __atomic_store_n(&sh->spin, 0, __ATOMIC_RELEASE); }
static void shared_changed(shared_state *sh) {
    __atomic_add_fetch(&sh->seq, 1, __ATOMIC_ACQ_REL);
    if (__builtin_available(macOS 14.4, *)) os_sync_wake_by_address_all((void *)&sh->seq, 4, OS_SYNC_WAKE_BY_ADDRESS_SHARED);
}

// Named objects of this process, so a second Create/Open with the same name finds the first.
static vx_object *g_named[1024];
static int g_named_count;
static vx_object *find_named(const char *name, int type) {
    for (int i = 0; i < g_named_count; i++) if (g_named[i]->type == type && !strcmp(g_named[i]->name, name)) return g_named[i];
    return NULL;
}

static void unref(vx_object *o) {
    if (o->type == OBJ_PORT && o->refs == 1) { o->closed = 1; pthread_cond_broadcast(&g_changed); }
    if (--o->refs) return;
    while (o->packets) { packet *p = o->packets; o->packets = p->next; free(p); }
    if (o->destroy) o->destroy(o->data);
    if (o->name) {
        for (int i = 0; i < g_named_count; i++) if (g_named[i] == o) { g_named[i] = g_named[--g_named_count]; break; }
        if (o->sh) shared_detach(o->sh, o->name, o->type);
        free(o->name);
    }
    free(o);
}
static vx_object *new_object(int type) { vx_object *o = calloc(1, sizeof *o); o->type = type; return o; }

// ---- threads -----------------------------------------------------------------------------------------------------
static void fls_thread_exit(void);
static __thread vx_object *t_self;          // this thread's object (created on demand for threads not made here)
static __thread jmp_buf *t_exit_jump;       // where ExitThread leaves the thread's start routine
static __thread DWORD t_exit_code;

static vx_object *self_object(void) {
    if (!t_self) {
        t_self = new_object(OBJ_THREAD);
        t_self->refs = 1; t_self->started = 1; t_self->tid = vx_current_tid(); t_self->pthread = pthread_self();
    }
    return t_self;
}

static void *thread_main(void *p) {
    vx_object *t = p;
    t_self = t;
    vx_thread_attach();
    pthread_mutex_lock(&g_lock);              // also waits until CreateThread has filled in the object
    while (t->suspended) pthread_cond_wait(&g_changed, &g_lock);
    t->started = 1;
    pthread_mutex_unlock(&g_lock);
    vx_notify_thread(2 /* DLL_THREAD_ATTACH */);
    jmp_buf exit_jump;
    t_exit_jump = &exit_jump;
    DWORD code;
    if (!setjmp(exit_jump)) code = t->start(t->arg);
    else code = t_exit_code;
    t_exit_jump = NULL;
    vx_notify_thread(3 /* DLL_THREAD_DETACH */);
    fls_thread_exit();
    vx_thread_detach();
    pthread_mutex_lock(&g_lock);
    t->done = 1; t->exit_code = code;
    t_self = NULL;
    unref(t);
    pthread_cond_broadcast(&g_changed);
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

static WINAPI HANDLE CreateThread(void *sa, size_t stack, DWORD (WINAPI *start)(void *), void *arg, DWORD flags, DWORD *tid) {
    (void)sa;
    vx_object *t = new_object(OBJ_THREAD);
    t->refs = 1;                               // held by the running thread
    t->start = start; t->arg = arg;
    t->suspended = flags & CREATE_SUSPENDED ? 1 : 0;
    // The thread's stack also carries this runtime's frames (exception dispatch keeps CONTEXTs on it): 2 MB at least.
    size_t size = stack > (2u << 20) ? stack : (2u << 20);
    size = (size + 16383) & ~(size_t)16383;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, size);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_mutex_lock(&g_lock);
    HANDLE h = new_handle(t);
    if (!h || pthread_create(&t->pthread, &a, thread_main, t)) {
        if (h) g_handles[handle_index(h)] = NULL;
        pthread_mutex_unlock(&g_lock);
        free(t); pthread_attr_destroy(&a);
        vx_set_last_error(8 /* ERROR_NOT_ENOUGH_MEMORY */);
        return NULL;
    }
    uint64_t id; pthread_threadid_np(t->pthread, &id);
    t->tid = (DWORD)id;
    pthread_mutex_unlock(&g_lock);
    pthread_attr_destroy(&a);
    if (tid) *tid = t->tid;
    return h;
}

static WINAPI void ExitThread(DWORD code) {
    if (t_exit_jump) { t_exit_code = code; longjmp(*t_exit_jump, 1); }
    fflush(stdout);
    __asm__ volatile("movq $0, %%gs:0x60" ::: "memory");     // the PEB out of libc's localtime slot (see loader.c)
    pthread_exit(NULL);                        // the main thread: the process lives on while other threads run
}

static WINAPI uintptr_t _beginthreadex(void *sa, unsigned stack, DWORD (WINAPI *start)(void *), void *arg, unsigned flags, unsigned *tid) {
    DWORD id = 0;
    HANDLE h = CreateThread(sa, stack, start, arg, flags, &id);
    if (tid) *tid = id;
    return (uintptr_t)h;
}
static WINAPI void _endthreadex(unsigned code) { ExitThread(code); }

static vx_object *thread_object(HANDLE h) {       // with g_lock held
    if ((intptr_t)h == -2) return self_object();
    vx_object *o = lookup(h);
    return o && o->type == OBJ_THREAD ? o : NULL;
}

static WINAPI BOOL GetExitCodeThread(HANDLE h, DWORD *code) {
    pthread_mutex_lock(&g_lock);
    vx_object *t = thread_object(h);
    if (t) *code = t->done ? t->exit_code : STILL_ACTIVE;
    pthread_mutex_unlock(&g_lock);
    if (!t) vx_set_last_error(6 /* ERROR_INVALID_HANDLE */);
    return t != NULL;
}
static WINAPI DWORD GetThreadId(HANDLE h) {
    pthread_mutex_lock(&g_lock);
    vx_object *t = thread_object(h);
    DWORD id = t ? t->tid : 0;
    pthread_mutex_unlock(&g_lock);
    return id;
}
static WINAPI DWORD ResumeThread(HANDLE h) {
    pthread_mutex_lock(&g_lock);
    vx_object *t = thread_object(h);
    DWORD prev = t ? t->suspended : (DWORD)-1;
    if (t && t->suspended) t->suspended--;
    pthread_cond_broadcast(&g_changed);
    pthread_mutex_unlock(&g_lock);
    return prev;
}
// Only a thread that has not started yet can be suspended for now (macOS can suspend a Mach thread, but its
// state while stopped in this runtime's code is not a Windows context yet).
static WINAPI DWORD SuspendThread(HANDLE h) {
    pthread_mutex_lock(&g_lock);
    vx_object *t = thread_object(h);
    DWORD prev = (DWORD)-1;
    if (t && !t->started) prev = t->suspended++;
    pthread_mutex_unlock(&g_lock);
    if (prev == (DWORD)-1) vx_set_last_error(50 /* ERROR_NOT_SUPPORTED */);
    return prev;
}
static WINAPI BOOL GetThreadContext(HANDLE h, void *ctx) { (void)h; (void)ctx; vx_set_last_error(50); return 0; }
static WINAPI BOOL SetThreadContext(HANDLE h, const void *ctx) { (void)h; (void)ctx; vx_set_last_error(50); return 0; }
static WINAPI int GetThreadPriority(HANDLE h) { (void)h; return 0; }
static WINAPI BOOL SetThreadPriority(HANDLE h, int p) { (void)h; (void)p; return 1; }
static WINAPI LONG SetThreadDescription(HANDLE h, const WCHAR *d) { (void)h; (void)d; return 0; }
static WINAPI BOOL SwitchToThread(void) { sched_yield(); return 1; }

static uint64_t cpu_mask(void) { long n = sysconf(_SC_NPROCESSORS_ONLN); return n >= 64 ? ~0ull : (1ull << n) - 1; }
static WINAPI BOOL GetProcessAffinityMask(HANDLE p, uint64_t *pm, uint64_t *sm) { (void)p; *pm = *sm = cpu_mask(); return 1; }
static WINAPI BOOL SetProcessAffinityMask(HANDLE p, uint64_t m) { (void)p; (void)m; return 1; }
static WINAPI uint64_t SetThreadAffinityMask(HANDLE h, uint64_t m) { (void)h; (void)m; return cpu_mask(); }

static WINAPI HANDLE OpenThread(DWORD access, BOOL inherit, DWORD tid) {
    (void)access; (void)inherit;
    pthread_mutex_lock(&g_lock);
    vx_object *t = NULL;
    for (int i = 0; i < MAX_HANDLES && !t; i++)
        if (g_handles[i] && g_handles[i]->type == OBJ_THREAD && g_handles[i]->tid == tid) t = g_handles[i];
    if (!t && tid == vx_current_tid()) t = self_object();
    HANDLE h = t ? new_handle(t) : NULL;
    pthread_mutex_unlock(&g_lock);
    if (!t) vx_set_last_error(87);
    return h;
}
// Killing a thread at an arbitrary point cannot be done safely on macOS (it may hold locks of the system's
// libraries); programs see the call fail.
static WINAPI BOOL TerminateThread(HANDLE h, DWORD code) { (void)h; (void)code; vx_log("TerminateThread is not supported"); vx_set_last_error(5); return 0; }

// ---- processes ------------------------------------------------------------------------------------------------------
// Another process (a child this process started, or any running one opened by its ID): the object is signalled
// when it ends. A thread waits for a child (its exit code), or polls for another process every 100 ms.
typedef struct { vx_object *o; pid_t pid; int child; } process_watch;
static void *watch_process(void *arg) {
    process_watch *w = arg;
    int status = 0;
    DWORD code = 0;
    if (w->child) {
        while (waitpid(w->pid, &status, 0) < 0 && errno == EINTR) {}
        code = WIFEXITED(status) ? (DWORD)WEXITSTATUS(status) : WIFSIGNALED(status) ? 0xC0000005u : 1;
        if (vx_trace) vx_log("process %d ended: %s %d", (int)w->pid, WIFSIGNALED(status) ? "signal" : "exit code", WIFSIGNALED(status) ? WTERMSIG(status) : WEXITSTATUS(status));
    } else {
        while (!kill(w->pid, 0) || errno != ESRCH) usleep(100000);
    }
    pthread_mutex_lock(&g_lock);
    w->o->done = 1; w->o->exit_code = code;
    unref(w->o);
    pthread_cond_broadcast(&g_changed);
    pthread_mutex_unlock(&g_lock);
    free(w);
    return NULL;
}
// A handle to a process (two, when `second` is asked for: the "thread" of a new process waits the same way).
HANDLE vx_process_handle(int pid, int child, HANDLE *second) {
    vx_object *o = new_object(OBJ_PROCESS);
    o->tid = (DWORD)pid;
    pthread_mutex_lock(&g_lock);
    HANDLE h = new_handle(o);
    if (second) *second = new_handle(o);
    o->refs++;                                                    // the watcher's
    pthread_mutex_unlock(&g_lock);
    process_watch *w = malloc(sizeof *w);
    w->o = o; w->pid = pid; w->child = child;
    pthread_t t; pthread_attr_t a;
    pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED); pthread_attr_setstacksize(&a, 128 << 10);
    pthread_create(&t, &a, watch_process, w);
    pthread_attr_destroy(&a);
    return h;
}
// The process ID and exit code behind a process handle (-1 for none).
int vx_process_info(HANDLE h, DWORD *code) {
    pthread_mutex_lock(&g_lock);
    vx_object *o = lookup(h);
    int pid = o && o->type == OBJ_PROCESS ? (int)o->tid : -1;
    if (pid >= 0 && code) *code = o->done ? o->exit_code : STILL_ACTIVE;
    pthread_mutex_unlock(&g_lock);
    return pid;
}
static WINAPI HANDLE OpenProcess(DWORD access, BOOL inherit, DWORD pid) {
    (void)access; (void)inherit;
    if (pid == (DWORD)getpid()) return (HANDLE)(intptr_t)-1;
    if (!pid || kill((pid_t)pid, 0)) { vx_set_last_error(87 /* ERROR_INVALID_PARAMETER */); return NULL; }
    return vx_process_handle((int)pid, 0, NULL);
}

// ---- events, mutexes, semaphores ----------------------------------------------------------------------------------
// Makes a handle for a new object, or for the existing object of that name (ERROR_ALREADY_EXISTS).
static HANDLE create_named(vx_object *o, const char *name) {
    pthread_mutex_lock(&g_lock);
    vx_object *existing = name && *name ? find_named(name, o->type) : NULL;
    int shared_existed = 0;
    if (existing) { free(o); o = existing; }
    else if (name && *name && g_named_count < 1024) {
        o->name = strdup(name);
        o->sh = shared_attach(name, o->type, 1, &shared_existed);
        if (o->sh && !shared_existed) {                             // the first: this object's state goes in
            o->sh->type = o->type; o->sh->manual = o->manual; o->sh->signaled = o->signaled;
            o->sh->count = o->count; o->sh->max = o->max; o->sh->owner = o->owner; o->sh->recursion = o->recursion;
            __atomic_store_n(&o->sh->magic, SHARED_MAGIC, __ATOMIC_RELEASE);
        }
        g_named[g_named_count++] = o;
    }
    HANDLE h = new_handle(o);
    pthread_mutex_unlock(&g_lock);
    if (!h) { if (!existing) free(o); vx_set_last_error(4 /* ERROR_TOO_MANY_OPEN_FILES */); return NULL; }
    vx_set_last_error(existing || shared_existed ? 183 /* ERROR_ALREADY_EXISTS */ : 0);
    return h;
}
static HANDLE open_named(const char *name, int type) {
    pthread_mutex_lock(&g_lock);
    vx_object *o = name ? find_named(name, type) : NULL;
    if (!o && name && *name && g_named_count < 1024) {             // another process's
        int existed;
        shared_state *sh = shared_attach(name, type, 0, &existed);
        if (sh) {
            o = new_object(type);
            o->name = strdup(name); o->sh = sh; o->manual = sh->manual;
            g_named[g_named_count++] = o;
        }
    }
    HANDLE h = o ? new_handle(o) : NULL;
    pthread_mutex_unlock(&g_lock);
    if (!o) vx_set_last_error(2 /* ERROR_FILE_NOT_FOUND */);
    return h;
}
static HANDLE open_named_w(const WCHAR *name, int type) { char *a = vx_utf8_dup(name); HANDLE h = open_named(a, type); free(a); return h; }
static HANDLE create_event(BOOL manual, BOOL initial, const char *name) {
    vx_object *o = new_object(OBJ_EVENT); o->manual = manual; o->signaled = initial; return create_named(o, name);
}
static HANDLE create_event_w(BOOL manual, BOOL initial, const WCHAR *name) { char *a = vx_utf8_dup(name); HANDLE h = create_event(manual, initial, a); free(a); return h; }
static WINAPI HANDLE CreateEventA(void *sa, BOOL manual, BOOL initial, const char *name) { (void)sa; return create_event(manual, initial, name); }
static WINAPI HANDLE CreateEventW(void *sa, BOOL manual, BOOL initial, const WCHAR *name) { (void)sa; return create_event_w(manual, initial, name); }
static WINAPI HANDLE CreateEventExA(void *sa, const char *name, DWORD flags, DWORD access) {
    (void)sa; (void)access; return create_event(flags & 1, (flags & 2) != 0, name);
}
static WINAPI HANDLE CreateEventExW(void *sa, const WCHAR *name, DWORD flags, DWORD access) {
    (void)sa; (void)access;
    return create_event_w(flags & 1 /* CREATE_EVENT_MANUAL_RESET */, (flags & 2) != 0 /* CREATE_EVENT_INITIAL_SET */, name);
}
static WINAPI HANDLE OpenEventA(DWORD access, BOOL inherit, const char *name) { (void)access; (void)inherit; return open_named(name, OBJ_EVENT); }
static WINAPI HANDLE OpenEventW(DWORD access, BOOL inherit, const WCHAR *name) { (void)access; (void)inherit; return open_named_w(name, OBJ_EVENT); }
static HANDLE create_mutex(BOOL owned, const char *name) {
    vx_object *o = new_object(OBJ_MUTEX);
    if (owned) { o->owner = vx_current_tid(); o->recursion = 1; }
    HANDLE h = create_named(o, name);
    // Opening an existing mutex never takes ownership of it.
    return h;
}
static WINAPI HANDLE CreateMutexA(void *sa, BOOL owned, const char *name) { (void)sa; return create_mutex(owned, name); }
static WINAPI HANDLE CreateMutexW(void *sa, BOOL owned, const WCHAR *name) { (void)sa; char *a = vx_utf8_dup(name); HANDLE h = create_mutex(owned, a); free(a); return h; }
static WINAPI HANDLE CreateMutexExW(void *sa, const WCHAR *name, DWORD flags, DWORD access) {
    (void)sa; (void)access; char *a = vx_utf8_dup(name); HANDLE h = create_mutex(flags & 1 /* CREATE_MUTEX_INITIAL_OWNER */, a); free(a); return h;
}
static WINAPI HANDLE OpenMutexA(DWORD access, BOOL inherit, const char *name) { (void)access; (void)inherit; return open_named(name, OBJ_MUTEX); }
static WINAPI HANDLE OpenMutexW(DWORD access, BOOL inherit, const WCHAR *name) { (void)access; (void)inherit; return open_named_w(name, OBJ_MUTEX); }
static HANDLE create_semaphore(LONG initial, LONG max, const char *name) {
    if (max <= 0 || initial < 0 || initial > max) { vx_set_last_error(87); return NULL; }
    vx_object *o = new_object(OBJ_SEMAPHORE); o->count = initial; o->max = max; return create_named(o, name);
}
static WINAPI HANDLE CreateSemaphoreA(void *sa, LONG initial, LONG max, const char *name) { (void)sa; return create_semaphore(initial, max, name); }
static WINAPI HANDLE CreateSemaphoreW(void *sa, LONG initial, LONG max, const WCHAR *name) {
    (void)sa; char *a = vx_utf8_dup(name); HANDLE h = create_semaphore(initial, max, a); free(a); return h;
}
static WINAPI HANDLE CreateSemaphoreExW(void *sa, LONG initial, LONG max, const WCHAR *name, DWORD flags, DWORD access) {
    (void)flags; (void)access; return CreateSemaphoreW(sa, initial, max, name);
}
static WINAPI HANDLE OpenSemaphoreA(DWORD access, BOOL inherit, const char *name) { (void)access; (void)inherit; return open_named(name, OBJ_SEMAPHORE); }
static WINAPI HANDLE OpenSemaphoreW(DWORD access, BOOL inherit, const WCHAR *name) { (void)access; (void)inherit; return open_named_w(name, OBJ_SEMAPHORE); }

// Runs `change` on the object of type `type` behind h, with the lock held, and wakes the waiters.
static BOOL modify(HANDLE h, int type, int (*change)(vx_object *, intptr_t), intptr_t arg) {
    pthread_mutex_lock(&g_lock);
    vx_object *o = lookup(h);
    int r = o && o->type == type ? change(o, arg) : 6 /* ERROR_INVALID_HANDLE */;
    if (!r) pthread_cond_broadcast(&g_changed);
    pthread_mutex_unlock(&g_lock);
    if (r) vx_set_last_error((DWORD)r);
    return r == 0;
}
static int set_event(vx_object *o, intptr_t v) {
    if (o->sh) { spin_lock(o->sh); o->sh->signaled = (int)v; spin_unlock(o->sh); shared_changed(o->sh); return 0; }
    o->signaled = (int)v;
    return 0;
}
static WINAPI BOOL SetEvent(HANDLE h) { return modify(h, OBJ_EVENT, set_event, 1); }
// For threads that are not Windows threads (Core Audio's): signals an event without touching a TEB.
void vx_set_event(HANDLE h) {
    pthread_mutex_lock(&g_lock);
    vx_object *o = lookup(h);
    if (o && o->type == OBJ_EVENT) { set_event(o, 1); pthread_cond_broadcast(&g_changed); }
    pthread_mutex_unlock(&g_lock);
}
static WINAPI BOOL ResetEvent(HANDLE h) { return modify(h, OBJ_EVENT, set_event, 0); }
static int release_mutex(vx_object *o, intptr_t tid) {
    if (o->sh) {
        spin_lock(o->sh);
        int err = o->sh->owner != (DWORD)tid ? 288 : 0;
        if (!err && --o->sh->recursion == 0) o->sh->owner = 0;
        spin_unlock(o->sh);
        if (!err) shared_changed(o->sh);
        return err;
    }
    if (o->owner != (DWORD)tid) return 288;     // ERROR_NOT_OWNER
    if (--o->recursion == 0) o->owner = 0;
    return 0;
}
static WINAPI BOOL ReleaseMutex(HANDLE h) { return modify(h, OBJ_MUTEX, release_mutex, vx_current_tid()); }

static WINAPI BOOL ReleaseSemaphore(HANDLE h, LONG n, LONG *previous) {
    pthread_mutex_lock(&g_lock);
    vx_object *o = lookup(h);
    DWORD err = !o || o->type != OBJ_SEMAPHORE ? 6 : 0;
    if (!err && o->sh) {
        spin_lock(o->sh);
        err = n <= 0 || o->sh->count + n > o->sh->max ? 298 : 0;
        if (!err) { if (previous) *previous = o->sh->count; o->sh->count += n; }
        spin_unlock(o->sh);
        if (!err) { shared_changed(o->sh); pthread_cond_broadcast(&g_changed); }
    } else if (!err) {
        err = n <= 0 || o->count + n > o->max ? 298 /* ERROR_TOO_MANY_POSTS */ : 0;
        if (!err) {
            if (previous) *previous = o->count;
            o->count += n;
            pthread_cond_broadcast(&g_changed);
        }
    }
    pthread_mutex_unlock(&g_lock);
    if (err) vx_set_last_error(err);
    return !err;
}

// ---- waiting ---------------------------------------------------------------------------------------------------------------
static int shared_signaled(const shared_state *sh, int type, DWORD tid) {
    switch (type) {
    case OBJ_EVENT:     return sh->signaled;
    case OBJ_MUTEX:     return !sh->owner || sh->owner == tid;
    case OBJ_SEMAPHORE: return sh->count > 0;
    }
    return 0;
}
static void shared_take(shared_state *sh, int type, DWORD tid) {
    if (type == OBJ_EVENT && !sh->manual) sh->signaled = 0;
    else if (type == OBJ_MUTEX) { sh->owner = tid; sh->recursion++; }
    else if (type == OBJ_SEMAPHORE) sh->count--;
}
static int is_signaled(const vx_object *o, DWORD tid) {
    if (o->sh) return shared_signaled(o->sh, o->type, tid);         // a hint: try_acquire decides
    switch (o->type) {
    case OBJ_THREAD: case OBJ_PROCESS: return o->done;
    case OBJ_EVENT:     return o->signaled;
    case OBJ_MUTEX:     return !o->owner || o->owner == tid;
    case OBJ_SEMAPHORE: return o->count > 0;
    }
    return 0;
}
// Takes the object if it is signalled (atomically for a shared object, which another process may take first).
static int try_acquire(vx_object *o, DWORD tid);
static void acquire(vx_object *o, DWORD tid) {
    if (o->sh) { try_acquire(o, tid); return; }
    if (o->type == OBJ_EVENT && !o->manual) o->signaled = 0;
    else if (o->type == OBJ_MUTEX) { o->owner = tid; o->recursion++; }
    else if (o->type == OBJ_SEMAPHORE) o->count--;
}

static void deadline_after(struct timespec *ts, DWORD ms) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000;
    if (ts->tv_nsec >= 1000000000) { ts->tv_sec++; ts->tv_nsec -= 1000000000; }
}

static int try_acquire(vx_object *o, DWORD tid) {
    if (!o->sh) { if (!is_signaled(o, tid)) return 0; acquire(o, tid); return 1; }
    spin_lock(o->sh);
    int ok = shared_signaled(o->sh, o->type, tid);
    if (ok) shared_take(o->sh, o->type, tid);
    spin_unlock(o->sh);
    if (ok) shared_changed(o->sh);
    return ok;
}
static WINAPI DWORD WaitForMultipleObjects(DWORD n, const HANDLE *hs, BOOL all, DWORD ms) {
    if (!n || n > 64) { vx_set_last_error(87); return WAIT_FAILED; }
    DWORD tid = vx_current_tid();
    struct timespec deadline;
    if (ms != INFINITE) deadline_after(&deadline, ms);
    vx_object *objs[64];
    pthread_mutex_lock(&g_lock);
    int shared = 0;
    for (DWORD i = 0; i < n; i++) {
        objs[i] = lookup(hs[i]);
        if (!objs[i] && ((intptr_t)hs[i] == -1 || (intptr_t)hs[i] == -2)) objs[i] = &g_never;
        if (!objs[i]) { pthread_mutex_unlock(&g_lock); vx_set_last_error(6); return WAIT_FAILED; }
        if (objs[i]->sh) shared = 1;
    }
    DWORD result;
    for (;;) {
        uint32_t seen = n == 1 && objs[0]->sh ? __atomic_load_n(&objs[0]->sh->seq, __ATOMIC_ACQUIRE) : 0;
        if (all) {
            DWORD i = 0;
            while (i < n && is_signaled(objs[i], tid)) i++;
            if (i == n) {
                for (i = 0; i < n && try_acquire(objs[i], tid); i++) {}
                if (i == n) { result = 0; break; }
                // Another process took one first: put back what was taken (events and semaphores), wait again.
                while (i--) {
                    vx_object *o = objs[i];
                    if (o->type == OBJ_SEMAPHORE) { if (o->sh) { spin_lock(o->sh); o->sh->count++; spin_unlock(o->sh); } else o->count++; }
                    else if (o->type == OBJ_EVENT) set_event(o, 1);
                    else if (o->type == OBJ_MUTEX) release_mutex(o, tid);
                }
            }
        } else {
            DWORD i = 0;
            while (i < n && !(is_signaled(objs[i], tid) && try_acquire(objs[i], tid))) i++;
            if (i < n) { result = i; break; }
        }
        if (ms == 0) { result = WAIT_TIMEOUT; break; }
        struct timespec now; clock_gettime(CLOCK_REALTIME, &now);
        if (ms != INFINITE && (now.tv_sec > deadline.tv_sec || (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec))) { result = WAIT_TIMEOUT; break; }
        for (DWORD k = 0; k < n && !shared; k++) if (objs[k]->sh) shared = 1;   // made shareable while waiting
        if (!shared) {
            if (ms == INFINITE) pthread_cond_wait(&g_changed, &g_lock);
            else if (pthread_cond_timedwait(&g_changed, &g_lock, &deadline) == ETIMEDOUT) { result = WAIT_TIMEOUT; break; }
            continue;
        }
        // Another process may change a shared object: one shared object is waited on directly (up to 50 ms at a
        // time); several, or a mix, are looked at again every millisecond.
        int slept = 0;
        if (n == 1) {
            if (__builtin_available(macOS 14.4, *)) {
                uint64_t left_ns = 50000000ull;
                if (ms != INFINITE) {
                    int64_t d = (int64_t)(deadline.tv_sec - now.tv_sec) * 1000000000ll + (deadline.tv_nsec - now.tv_nsec);
                    if (d < (int64_t)left_ns) left_ns = d > 0 ? (uint64_t)d : 1;
                }
                pthread_mutex_unlock(&g_lock);
                os_sync_wait_on_address_with_timeout((void *)&objs[0]->sh->seq, seen, 4, OS_SYNC_WAIT_ON_ADDRESS_SHARED,
                                                     OS_CLOCK_MACH_ABSOLUTE_TIME, left_ns);
                pthread_mutex_lock(&g_lock);
                slept = 1;
            }
        }
        if (!slept) {
            struct timespec slice = now;
            slice.tv_nsec += 1000000;
            if (slice.tv_nsec >= 1000000000) { slice.tv_sec++; slice.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&g_changed, &g_lock, &slice);
        }
    }
    pthread_mutex_unlock(&g_lock);
    return result;
}
static WINAPI DWORD WaitForMultipleObjectsEx(DWORD n, const HANDLE *hs, BOOL all, DWORD ms, BOOL alertable) {
    (void)alertable; return WaitForMultipleObjects(n, hs, all, ms);
}
static WINAPI DWORD WaitForSingleObject(HANDLE h, DWORD ms) { return WaitForMultipleObjects(1, &h, 0, ms); }
static WINAPI DWORD WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL alertable) { (void)alertable; return WaitForMultipleObjects(1, &h, 0, ms); }
static WINAPI DWORD SleepEx(DWORD ms, BOOL alertable) {
    (void)alertable;
    if (ms) usleep((useconds_t)ms * 1000); else sched_yield();
    return 0;
}

// ---- handles -----------------------------------------------------------------------------------------------------------------
static WINAPI BOOL CloseHandle(HANDLE h) {
    intptr_t v = (intptr_t)h;
    if (v == -1 || v == -2) return 1;
    pthread_mutex_lock(&g_lock);
    int i = handle_index(h);
    vx_object *o = i >= 0 ? g_handles[i] : NULL;
    if (o) { g_handles[i] = NULL; unref(o); }
    intptr_t k = ((intptr_t)h - INJECT_BASE) / 4;
    if (!o && (intptr_t)h >= INJECT_BASE && k < INJECT_COUNT && !(((intptr_t)h - INJECT_BASE) % 4) && g_injected[k]) {
        o = g_injected[k]; g_injected[k] = NULL; unref(o);
    }
    pthread_mutex_unlock(&g_lock);
    if (o) return 1;
    int fd = vx_handle_fd(h);
    if (fd > 2) { close(fd); return 1; }
    if (fd >= 0) return 1;
    vx_set_last_error(6);
    return 0;
}

BOOL vx_handle_close(HANDLE h) { return CloseHandle(h); }

HANDLE vx_handle_new(int kind, void *data, void (*destroy)(void *)) {
    vx_object *o = new_object(OBJ_OTHER);
    o->kind = kind; o->data = data; o->destroy = destroy;
    pthread_mutex_lock(&g_lock);
    HANDLE h = new_handle(o);
    pthread_mutex_unlock(&g_lock);
    if (!h) free(o);
    return h;
}
void *vx_handle_data(HANDLE h, int kind) {
    pthread_mutex_lock(&g_lock);
    vx_object *o = lookup(h);
    void *d = o && o->type == OBJ_OTHER && o->kind == kind ? o->data : NULL;
    pthread_mutex_unlock(&g_lock);
    return d;
}

// ---- handles given to other processes ------------------------------------------------------------------------------------
// DuplicateHandle into another process: the object becomes shareable (an unnamed event, mutex or semaphore gets a
// hidden name and moves its state to shared memory; a mapping is shared memory already), and an entry goes into
// the other process's inbox ("/vxi.<pid>"): the handle value given back (0x20000 + 4 * entry) means that entry
// there, and the other process makes it its object when it first uses the value.
typedef struct { int32_t kind; int32_t type; uint64_t size; char name[48]; } inbox_entry;   // kind: 1 named object, 2 mapping, 3 process
typedef struct { volatile uint32_t spin, next; inbox_entry e[INJECT_COUNT]; } inbox;
static inbox *inbox_open(int pid) {
    char name[40];
    snprintf(name, sizeof name, "/vxi.%d", pid);
    int fd = shm_open(name, O_RDWR | O_CREAT, 0600);
    if (fd < 0) return NULL;
    struct stat st;
    if (!fstat(fd, &st) && st.st_size < (off_t)sizeof(inbox)) ftruncate(fd, sizeof(inbox));
    inbox *b = mmap(NULL, sizeof(inbox), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    return b == MAP_FAILED ? NULL : b;
}
static inbox *g_my_inbox;
void *vx_mapping_from_shm(const char *shm, uint64_t size, void (**destroy)(void *));   // files.c
int vx_mapping_shm(HANDLE h, char *shm, size_t cap, uint64_t *size);                  // files.c
static vx_object *injected(HANDLE h) {                           // with g_lock held
    intptr_t v = (intptr_t)h - INJECT_BASE;
    if (v < 0 || v % 4 || v / 4 >= INJECT_COUNT) return NULL;
    int k = (int)(v / 4);
    if (g_injected[k]) return g_injected[k];
    if (!g_my_inbox) g_my_inbox = inbox_open(getpid());
    if (!g_my_inbox) return NULL;
    inbox_entry e = g_my_inbox->e[k];
    vx_object *o = NULL;
    if (e.kind == 1) {
        o = find_named(e.name, e.type);
        if (!o) {
            int existed;
            shared_state *sh = shared_attach(e.name, e.type, 0, &existed);
            if (!sh) return NULL;
            o = new_object(e.type);
            o->name = strdup(e.name); o->sh = sh; o->manual = sh->manual;
            if (g_named_count < 1024) g_named[g_named_count++] = o;
        }
    } else if (e.kind == 2) {
        void (*destroy)(void *) = NULL;
        void *data = vx_mapping_from_shm(e.name, e.size, &destroy);
        if (!data) return NULL;
        o = new_object(OBJ_OTHER);
        o->kind = VX_HANDLE_MAPPING; o->data = data; o->destroy = destroy;
    } else if (e.kind == 3) {
        pthread_mutex_unlock(&g_lock);
        HANDLE ph = vx_process_handle((int)e.size, 0, NULL);
        pthread_mutex_lock(&g_lock);
        int i = handle_index(ph);
        o = i >= 0 ? g_handles[i] : NULL;
        if (o) { o->refs++; g_handles[i] = NULL; unref(o); }      // keep the object, drop the extra handle
    }
    if (!o) return NULL;
    o->refs++;
    g_injected[k] = o;
    if (vx_trace) vx_log("handle %#lx from another process: %s", (long)(intptr_t)h, e.kind == 2 ? "a mapping" : e.kind == 3 ? "a process" : e.name);
    return o;
}
static unsigned g_hidden_names;
// Makes a local event, mutex or semaphore shareable (with g_lock held).
static int make_shareable(vx_object *o) {
    if (o->sh) return 1;
    if (o->type != OBJ_EVENT && o->type != OBJ_MUTEX && o->type != OBJ_SEMAPHORE) return 0;
    char name[48];
    snprintf(name, sizeof name, "__vxdup.%d.%u", (int)getpid(), ++g_hidden_names);
    int existed;
    shared_state *sh = shared_attach(name, o->type, 1, &existed);
    if (!sh) return 0;
    sh->type = o->type; sh->manual = o->manual; sh->signaled = o->signaled; sh->count = o->count; sh->max = o->max;
    sh->owner = o->owner; sh->recursion = o->recursion;
    __atomic_store_n(&sh->magic, SHARED_MAGIC, __ATOMIC_RELEASE);
    free(o->name);
    o->name = strdup(name); o->sh = sh;
    if (g_named_count < 1024) g_named[g_named_count++] = o;
    pthread_cond_broadcast(&g_changed);                           // waiters look at the shared state from now on
    return 1;
}
static HANDLE give_to_process(int pid, HANDLE src) {
    inbox_entry e = { 0 };
    pthread_mutex_lock(&g_lock);
    vx_object *o = lookup(src);
    if (o && (o->type == OBJ_EVENT || o->type == OBJ_MUTEX || o->type == OBJ_SEMAPHORE) && make_shareable(o)) {
        e.kind = 1; e.type = o->type; snprintf(e.name, sizeof e.name, "%s", o->name);
    } else if (o && o->type == OBJ_PROCESS) {
        e.kind = 3; e.size = o->tid;
    } else if (o && o->type == OBJ_OTHER && o->kind == VX_HANDLE_MAPPING) {
        pthread_mutex_unlock(&g_lock);
        if (!vx_mapping_shm(src, e.name, sizeof e.name, &e.size)) { vx_set_last_error(50); return NULL; }
        e.kind = 2;
        pthread_mutex_lock(&g_lock);
    } else if ((intptr_t)src == -1) {
        e.kind = 3; e.size = (uint64_t)getpid();
    }
    pthread_mutex_unlock(&g_lock);
    if (!e.kind) { vx_log("DuplicateHandle: handle %p cannot go to another process yet", src); vx_set_last_error(50); return NULL; }
    if (e.kind == 1) {                                             // held for the other process until it takes it
        char shm[40];
        shared_name(e.name, e.type, shm, sizeof shm);
        vx_name_join(shm);
    }
    inbox *b = inbox_open(pid);
    if (!b) { vx_set_last_error(8); return NULL; }
    while (__atomic_exchange_n(&b->spin, 1, __ATOMIC_ACQUIRE)) sched_yield();
    uint32_t k = b->next++ % INJECT_COUNT;
    b->e[k] = e;
    __atomic_store_n(&b->spin, 0, __ATOMIC_RELEASE);
    munmap(b, sizeof *b);
    if (vx_trace) vx_log("DuplicateHandle: %p to process %d as %#x", src, pid, INJECT_BASE + 4 * k);
    return (HANDLE)(intptr_t)(INJECT_BASE + 4 * k);
}
int vx_process_info(HANDLE h, DWORD *code);
static WINAPI BOOL DuplicateHandle(HANDLE src_process, HANDLE src, HANDLE dst_process, HANDLE *dst, DWORD access, BOOL inherit, DWORD options) {
    (void)access; (void)inherit;
    int src_pid = (intptr_t)src_process == -1 ? getpid() : vx_process_info(src_process, NULL);
    int dst_pid = (intptr_t)dst_process == -1 ? getpid() : vx_process_info(dst_process, NULL);
    if (src_pid != getpid()) { vx_log("DuplicateHandle from another process is not supported yet"); vx_set_last_error(50); return 0; }
    if (dst_pid > 0 && dst_pid != getpid()) {
        HANDLE h = give_to_process(dst_pid, src);
        if (!h) return 0;
        *dst = h;
        if (options & 1 /* DUPLICATE_CLOSE_SOURCE */) CloseHandle(src);
        return 1;
    }
    if ((intptr_t)src == -1) { *dst = src; return 1; }
    pthread_mutex_lock(&g_lock);
    vx_object *o = (intptr_t)src == -2 ? self_object() : lookup(src);
    HANDLE h = o ? new_handle(o) : NULL;
    pthread_mutex_unlock(&g_lock);
    if (!o) {
        int fd = vx_handle_fd(src);
        if (fd < 0) { vx_set_last_error(6); return 0; }
        h = (HANDLE)(intptr_t)(0x10000 + dup(fd));
    }
    *dst = h;
    if (options & 1 /* DUPLICATE_CLOSE_SOURCE */) CloseHandle(src);
    return h != NULL;
}

// ---- critical sections and SRW locks -------------------------------------------------------------------------------------
// A CRITICAL_SECTION (40 bytes, in the program's memory) holds a pointer to a recursive pthread mutex.
static pthread_mutex_t *cs_mutex(void *cs) {
    pthread_mutex_t **slot = (pthread_mutex_t **)cs;
    if (!*slot) {
        pthread_mutex_t *m = malloc(sizeof *m);
        pthread_mutexattr_t a; pthread_mutexattr_init(&a); pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(m, &a); pthread_mutexattr_destroy(&a);
        if (!__sync_bool_compare_and_swap(slot, NULL, m)) { pthread_mutex_destroy(m); free(m); }
    }
    return *slot;
}
static WINAPI void InitializeCriticalSection(void *cs) { memset(cs, 0, 40); cs_mutex(cs); }
static WINAPI BOOL InitializeCriticalSectionAndSpinCount(void *cs, DWORD spin) { (void)spin; InitializeCriticalSection(cs); return 1; }
static WINAPI BOOL InitializeCriticalSectionEx(void *cs, DWORD spin, DWORD flags) { (void)spin; (void)flags; InitializeCriticalSection(cs); return 1; }
static WINAPI void EnterCriticalSection(void *cs) { pthread_mutex_lock(cs_mutex(cs)); }
static WINAPI BOOL TryEnterCriticalSection(void *cs) { return pthread_mutex_trylock(cs_mutex(cs)) == 0; }
static WINAPI void LeaveCriticalSection(void *cs) { pthread_mutex_unlock(cs_mutex(cs)); }
static WINAPI void DeleteCriticalSection(void *cs) {
    pthread_mutex_t **slot = (pthread_mutex_t **)cs;
    if (*slot) { pthread_mutex_destroy(*slot); free(*slot); *slot = NULL; }
}
// SRW lock: one pointer, holding a pthread rwlock.
static pthread_rwlock_t *srw(void *l) {
    pthread_rwlock_t **slot = (pthread_rwlock_t **)l;
    if (!*slot) {
        pthread_rwlock_t *r = malloc(sizeof *r); pthread_rwlock_init(r, NULL);
        if (!__sync_bool_compare_and_swap(slot, NULL, r)) { pthread_rwlock_destroy(r); free(r); }
    }
    return *slot;
}
static WINAPI void InitializeSRWLock(void *l) { *(void **)l = NULL; }
static WINAPI void AcquireSRWLockExclusive(void *l) { pthread_rwlock_wrlock(srw(l)); }
static WINAPI void ReleaseSRWLockExclusive(void *l) { pthread_rwlock_unlock(srw(l)); }
static WINAPI void AcquireSRWLockShared(void *l) { pthread_rwlock_rdlock(srw(l)); }
static WINAPI void ReleaseSRWLockShared(void *l) { pthread_rwlock_unlock(srw(l)); }
static WINAPI BOOL TryAcquireSRWLockExclusive(void *l) { return pthread_rwlock_trywrlock(srw(l)) == 0; }
static WINAPI BOOL TryAcquireSRWLockShared(void *l) { return pthread_rwlock_tryrdlock(srw(l)) == 0; }

// ---- condition variables ------------------------------------------------------------------------------------------------------
// A CONDITION_VARIABLE (one pointer) holds a wake counter. A sleeper reads it while still holding its lock, so a
// wake that comes after the caller checked its predicate always changes the counter and is never missed. Every
// wake wakes all sleepers (Windows allows spurious wakeups).
static pthread_mutex_t g_cv_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv_changed = PTHREAD_COND_INITIALIZER;

static int cv_sleep(uint64_t *cv, uint64_t seen, DWORD ms) {
    struct timespec deadline;
    if (ms != INFINITE) deadline_after(&deadline, ms);
    int woken = 1;
    pthread_mutex_lock(&g_cv_lock);
    while (__atomic_load_n(cv, __ATOMIC_ACQUIRE) == seen) {
        if (ms == INFINITE) pthread_cond_wait(&g_cv_changed, &g_cv_lock);
        else if (ms == 0 || pthread_cond_timedwait(&g_cv_changed, &g_cv_lock, &deadline) == ETIMEDOUT) { woken = 0; break; }
    }
    pthread_mutex_unlock(&g_cv_lock);
    if (!woken) vx_set_last_error(1460 /* ERROR_TIMEOUT */);
    return woken;
}
static WINAPI void InitializeConditionVariable(uint64_t *cv) { *cv = 0; }
static WINAPI void WakeAllConditionVariable(uint64_t *cv) {
    pthread_mutex_lock(&g_cv_lock);
    __atomic_add_fetch(cv, 1, __ATOMIC_RELEASE);
    pthread_cond_broadcast(&g_cv_changed);
    pthread_mutex_unlock(&g_cv_lock);
}
static WINAPI void WakeConditionVariable(uint64_t *cv) { WakeAllConditionVariable(cv); }
static WINAPI BOOL SleepConditionVariableCS(uint64_t *cv, void *cs, DWORD ms) {
    uint64_t seen = __atomic_load_n(cv, __ATOMIC_ACQUIRE);
    LeaveCriticalSection(cs);
    int woken = cv_sleep(cv, seen, ms);
    EnterCriticalSection(cs);
    return woken;
}
static WINAPI BOOL SleepConditionVariableSRW(uint64_t *cv, void *l, DWORD ms, DWORD flags) {
    int shared = flags & 1;                    // CONDITION_VARIABLE_LOCKMODE_SHARED
    uint64_t seen = __atomic_load_n(cv, __ATOMIC_ACQUIRE);
    pthread_rwlock_unlock(srw(l));
    int woken = cv_sleep(cv, seen, ms);
    if (shared) AcquireSRWLockShared(l); else AcquireSRWLockExclusive(l);
    return woken;
}

// ---- fiber-local storage --------------------------------------------------------------------------------------------------------
// Without fibers, FLS is thread-local storage whose callbacks run when a thread ends (MSVC's C runtime keeps its
// per-thread data here).
#define MAX_FLS 128
typedef void (WINAPI *fls_callback)(void *);
static fls_callback g_fls_callbacks[MAX_FLS];
static uint8_t g_fls_used[MAX_FLS];
static __thread void *t_fls[MAX_FLS];
static WINAPI DWORD FlsAlloc(fls_callback cb) {
    pthread_mutex_lock(&g_lock);
    DWORD i = 0;
    while (i < MAX_FLS && g_fls_used[i]) i++;
    if (i < MAX_FLS) { g_fls_used[i] = 1; g_fls_callbacks[i] = cb; }
    pthread_mutex_unlock(&g_lock);
    if (i == MAX_FLS) { vx_set_last_error(8); return 0xffffffffu; }   // FLS_OUT_OF_INDEXES
    return i;
}
static WINAPI void *FlsGetValue(DWORD i) { if (i >= MAX_FLS) { vx_set_last_error(87); return NULL; } vx_set_last_error(0); return t_fls[i]; }
static WINAPI BOOL FlsSetValue(DWORD i, void *v) { if (i >= MAX_FLS || !g_fls_used[i]) { vx_set_last_error(87); return 0; } t_fls[i] = v; return 1; }
static WINAPI BOOL FlsFree(DWORD i) {
    if (i >= MAX_FLS || !g_fls_used[i]) { vx_set_last_error(87); return 0; }
    if (t_fls[i] && g_fls_callbacks[i]) g_fls_callbacks[i](t_fls[i]);     // this thread's value only
    t_fls[i] = NULL;
    g_fls_used[i] = 0;
    return 1;
}
static void fls_thread_exit(void) {
    for (int i = 0; i < MAX_FLS; i++)
        if (t_fls[i] && g_fls_used[i] && g_fls_callbacks[i]) { void *v = t_fls[i]; t_fls[i] = NULL; g_fls_callbacks[i](v); }
}

// ---- interlocked singly linked lists -----------------------------------------------------------------------------------------------
// SLIST_HEADER (16 bytes): the first entry, then the depth. A lock keeps it simple; the lists are short-lived.
typedef struct slist_entry { struct slist_entry *next; } slist_entry;
typedef struct { slist_entry *first; uint64_t depth; } slist_header;
static pthread_mutex_t g_slist_lock = PTHREAD_MUTEX_INITIALIZER;
static WINAPI void InitializeSListHead(slist_header *h) { h->first = NULL; h->depth = 0; }
static WINAPI slist_entry *InterlockedPushEntrySList(slist_header *h, slist_entry *e) {
    pthread_mutex_lock(&g_slist_lock);
    slist_entry *old = h->first;
    e->next = old; h->first = e; h->depth++;
    pthread_mutex_unlock(&g_slist_lock);
    return old;
}
static WINAPI slist_entry *InterlockedPopEntrySList(slist_header *h) {
    pthread_mutex_lock(&g_slist_lock);
    slist_entry *e = h->first;
    if (e) { h->first = e->next; h->depth--; }
    pthread_mutex_unlock(&g_slist_lock);
    return e;
}
static WINAPI slist_entry *InterlockedFlushSList(slist_header *h) {
    pthread_mutex_lock(&g_slist_lock);
    slist_entry *e = h->first;
    h->first = NULL; h->depth = 0;
    pthread_mutex_unlock(&g_slist_lock);
    return e;
}
static WINAPI uint16_t QueryDepthSList(slist_header *h) { return (uint16_t)h->depth; }

// ---- I/O completion ports ---------------------------------------------------------------------------------------------------------
// A port is a queue of completion packets. I/O here completes at once, so a read or write with an OVERLAPPED on
// a handle associated with a port posts its packet right away; waiters take packets in order.
typedef struct { HANDLE handle, port; uint64_t key; int skip_on_success; } association;
static association g_assoc[1024];
static int g_assoc_count;

static void post_packet(vx_object *port, DWORD bytes, uint64_t key, void *ov, DWORD status) {   // g_lock held
    packet *p = calloc(1, sizeof *p);
    p->bytes = bytes; p->key = key; p->overlapped = ov; p->status = status;
    if (!port->packets) port->packets_tail = &port->packets;
    *port->packets_tail = p;
    port->packets_tail = &p->next;
    pthread_cond_broadcast(&g_changed);
}

static WINAPI HANDLE CreateIoCompletionPort(HANDLE file, HANDLE existing, uint64_t key, DWORD threads) {
    (void)threads;
    pthread_mutex_lock(&g_lock);
    HANDLE port = existing;
    if (!existing) {
        vx_object *o = new_object(OBJ_PORT);
        port = new_handle(o);
        if (!port) { free(o); pthread_mutex_unlock(&g_lock); vx_set_last_error(4); return NULL; }
    } else if (!lookup(existing) || lookup(existing)->type != OBJ_PORT) {
        pthread_mutex_unlock(&g_lock); vx_set_last_error(87); return NULL;
    }
    if (file != (HANDLE)(intptr_t)-1) {
        int i = 0;
        while (i < g_assoc_count && g_assoc[i].handle != file) i++;
        if (i == g_assoc_count && g_assoc_count < 1024) g_assoc_count++;
        if (i < 1024) g_assoc[i] = (association){ file, port, key, 0 };
    }
    pthread_mutex_unlock(&g_lock);
    return port;
}
#define FILE_SKIP_COMPLETION_PORT_ON_SUCCESS 1
static WINAPI BOOL SetFileCompletionNotificationModes(HANDLE file, uint8_t flags) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_assoc_count; i++) if (g_assoc[i].handle == file) g_assoc[i].skip_on_success = flags & FILE_SKIP_COMPLETION_PORT_ON_SUCCESS;
    pthread_mutex_unlock(&g_lock);
    return 1;
}
// Called by the file and socket functions when an overlapped operation completes. OVERLAPPED: Internal (status),
// InternalHigh (bytes), offsets, hEvent; an event with its low bit set asks for no completion packet.
void vx_io_completed(HANDLE file, void *ov, DWORD bytes, DWORD status) {
    if (!ov) return;
    uint64_t *o = ov;
    o[0] = status; o[1] = bytes;
    HANDLE event = *(HANDLE *)((uint8_t *)ov + 24);
    pthread_mutex_lock(&g_lock);
    vx_object *e = lookup((HANDLE)((uintptr_t)event & ~(uintptr_t)1));
    if (e && e->type == OBJ_EVENT) e->signaled = 1;
    if (!((uintptr_t)event & 1)) {
        for (int i = 0; i < g_assoc_count; i++) {
            if (g_assoc[i].handle != file) continue;
            vx_object *port = lookup(g_assoc[i].port);
            if (port && port->type == OBJ_PORT && !(status == 0 && g_assoc[i].skip_on_success)) post_packet(port, bytes, g_assoc[i].key, ov, status);
            break;
        }
    }
    pthread_cond_broadcast(&g_changed);
    pthread_mutex_unlock(&g_lock);
}
static WINAPI BOOL PostQueuedCompletionStatus(HANDLE port, DWORD bytes, uint64_t key, void *ov) {
    pthread_mutex_lock(&g_lock);
    vx_object *o = lookup(port);
    if (o && o->type == OBJ_PORT) post_packet(o, bytes, key, ov, 0);
    pthread_mutex_unlock(&g_lock);
    if (!o || o->type != OBJ_PORT) { vx_set_last_error(6); return 0; }
    return 1;
}
// Waits for up to `max` packets. Returns how many were taken; *err is set when none were.
static int take_packets(HANDLE port, packet *out, int max, DWORD ms, DWORD *err) {
    struct timespec deadline;
    if (ms != INFINITE) deadline_after(&deadline, ms);
    int n = 0;
    *err = 0;
    pthread_mutex_lock(&g_lock);
    vx_object *o = lookup(port);
    if (!o || o->type != OBJ_PORT) { pthread_mutex_unlock(&g_lock); *err = 6; return 0; }
    o->refs++;                                               // the port stays alive while waited on
    for (;;) {
        while (n < max && o->packets) {
            packet *p = o->packets;
            o->packets = p->next;
            out[n++] = *p;
            free(p);
        }
        if (n) break;
        if (o->closed) { *err = 735; break; }               // ERROR_ABANDONED_WAIT_0
        if (ms == 0) { *err = 258; break; }                  // WAIT_TIMEOUT
        if (ms == INFINITE) pthread_cond_wait(&g_changed, &g_lock);
        else if (pthread_cond_timedwait(&g_changed, &g_lock, &deadline) == ETIMEDOUT) { *err = 258; break; }
    }
    unref(o);
    pthread_mutex_unlock(&g_lock);
    return n;
}
static WINAPI BOOL GetQueuedCompletionStatus(HANDLE port, DWORD *bytes, uint64_t *key, void **ov, DWORD ms) {
    packet p; DWORD err;
    if (!take_packets(port, &p, 1, ms, &err)) { *ov = NULL; vx_set_last_error(err); return 0; }
    *bytes = p.bytes; *key = p.key; *ov = p.overlapped;
    if (p.status) { vx_set_last_error(p.status); return 0; }
    return 1;
}
// OVERLAPPED_ENTRY: completion key, OVERLAPPED, internal, bytes transferred.
typedef struct { uint64_t key; void *overlapped; uint64_t internal; DWORD bytes; } OVERLAPPED_ENTRY;
static WINAPI BOOL GetQueuedCompletionStatusEx(HANDLE port, OVERLAPPED_ENTRY *entries, uint32_t count, uint32_t *removed, DWORD ms, BOOL alertable) {
    (void)alertable;
    packet ps[64]; DWORD err;
    int n = take_packets(port, ps, count < 64 ? (int)count : 64, ms, &err);
    for (int i = 0; i < n; i++) entries[i] = (OVERLAPPED_ENTRY){ ps[i].key, ps[i].overlapped, ps[i].status, ps[i].bytes };
    *removed = (uint32_t)n;
    if (!n) { vx_set_last_error(err); return 0; }
    return 1;
}
// Most overlapped operations complete at once here; those still running (pipes) hold STATUS_PENDING in Internal
// until vx_io_completed fills it in.
#define STATUS_PENDING 0x103
static WINAPI DWORD WaitForSingleObject(HANDLE h, DWORD ms);
static BOOL overlapped_result(HANDLE file, const volatile uint64_t *ov, DWORD *bytes, DWORD ms) {
    (void)file;
    if (__atomic_load_n(&ov[0], __ATOMIC_ACQUIRE) == STATUS_PENDING) {
        if (!ms) { vx_set_last_error(996 /* ERROR_IO_INCOMPLETE */); return 0; }
        HANDLE event = (HANDLE)((uintptr_t)*(HANDLE *)((uint8_t *)ov + 24) & ~(uintptr_t)1);
        struct timespec start; clock_gettime(CLOCK_MONOTONIC, &start);
        while (__atomic_load_n(&ov[0], __ATOMIC_ACQUIRE) == STATUS_PENDING) {
            if (event) WaitForSingleObject(event, 10); else usleep(500);
            struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
            uint64_t elapsed = (uint64_t)(now.tv_sec - start.tv_sec) * 1000 + (uint64_t)((now.tv_nsec - start.tv_nsec) / 1000000);
            if (ms != INFINITE && elapsed >= ms) { vx_set_last_error(WAIT_TIMEOUT); return 0; }
        }
    }
    *bytes = (DWORD)ov[1];
    if (ov[0]) { vx_set_last_error((DWORD)ov[0]); return 0; }
    return 1;
}
static WINAPI BOOL GetOverlappedResult(HANDLE file, const uint64_t *ov, DWORD *bytes, BOOL wait) { return overlapped_result(file, ov, bytes, wait ? INFINITE : 0); }
static WINAPI BOOL GetOverlappedResultEx(HANDLE file, const uint64_t *ov, DWORD *bytes, DWORD ms, BOOL alertable) { (void)alertable; return overlapped_result(file, ov, bytes, ms); }
static WINAPI BOOL CancelIo(HANDLE file) { (void)file; return 1; }
static WINAPI BOOL CancelIoEx(HANDLE file, void *ov) { (void)file; (void)ov; vx_set_last_error(1168 /* ERROR_NOT_FOUND */); return 0; }

// ---- WaitOnAddress -----------------------------------------------------------------------------------------------------------------
// Waiters sleep while the value at the address still equals the compare value; every wake wakes all waiters
// (spurious wakeups are allowed), which then re-check.
static pthread_mutex_t g_addr_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_addr_changed = PTHREAD_COND_INITIALIZER;
static WINAPI BOOL WaitOnAddress(volatile void *addr, const void *cmp, size_t size, DWORD ms) {
    struct timespec deadline;
    if (ms != INFINITE) deadline_after(&deadline, ms);
    BOOL ok = 1;
    pthread_mutex_lock(&g_addr_lock);
    while (!memcmp((const void *)addr, cmp, size)) {
        if (ms == INFINITE) pthread_cond_wait(&g_addr_changed, &g_addr_lock);
        else if (ms == 0 || pthread_cond_timedwait(&g_addr_changed, &g_addr_lock, &deadline) == ETIMEDOUT) { ok = 0; break; }
    }
    pthread_mutex_unlock(&g_addr_lock);
    if (!ok) vx_set_last_error(1460);
    return ok;
}
static WINAPI void WakeByAddressAll(void *addr) {
    (void)addr;
    pthread_mutex_lock(&g_addr_lock);
    pthread_cond_broadcast(&g_addr_changed);
    pthread_mutex_unlock(&g_addr_lock);
}
static WINAPI void WakeByAddressSingle(void *addr) { WakeByAddressAll(addr); }

// ---- one-time initialization ----------------------------------------------------------------------------------------------------
static pthread_mutex_t g_once_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
typedef BOOL (WINAPI *init_once_fn)(void *once, void *param, void **context);
static WINAPI void InitOnceInitialize(uint64_t *once) { *once = 0; }
static WINAPI BOOL InitOnceExecuteOnce(uint64_t *once, init_once_fn fn, void *param, void **context) {
    if (__atomic_load_n(once, __ATOMIC_ACQUIRE) == 2) return 1;
    pthread_mutex_lock(&g_once_lock);
    BOOL ok = 1;
    if (*once != 2) {
        ok = fn(once, param, context);
        if (ok) __atomic_store_n(once, 2, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&g_once_lock);
    return ok;
}

static const vx_export sync_k32[] = {
    VX_EXPORT(CreateThread), VX_EXPORT(ExitThread), VX_EXPORT(GetExitCodeThread), VX_EXPORT(GetThreadId),
    VX_EXPORT(ResumeThread), VX_EXPORT(SuspendThread), VX_EXPORT(GetThreadContext), VX_EXPORT(SetThreadContext),
    VX_EXPORT(GetThreadPriority), VX_EXPORT(SetThreadPriority), VX_EXPORT(SetThreadDescription), VX_EXPORT(SwitchToThread),
    VX_EXPORT(GetProcessAffinityMask), VX_EXPORT(SetProcessAffinityMask), VX_EXPORT(SetThreadAffinityMask), VX_EXPORT(OpenProcess),
    VX_EXPORT(CreateEventA), VX_EXPORT(CreateEventW), VX_EXPORT(CreateEventExA), VX_EXPORT(CreateEventExW),
    VX_EXPORT(OpenEventA), VX_EXPORT(OpenEventW), VX_EXPORT(SetEvent), VX_EXPORT(ResetEvent),
    VX_EXPORT(CreateMutexA), VX_EXPORT(CreateMutexW), VX_EXPORT(CreateMutexExW), VX_EXPORT(OpenMutexA), VX_EXPORT(OpenMutexW),
    VX_EXPORT(ReleaseMutex),
    VX_EXPORT(CreateSemaphoreA), VX_EXPORT(CreateSemaphoreW), VX_EXPORT(CreateSemaphoreExW), VX_EXPORT(OpenSemaphoreA),
    VX_EXPORT(OpenSemaphoreW), VX_EXPORT(ReleaseSemaphore),
    VX_EXPORT(OpenThread), VX_EXPORT(TerminateThread),
    VX_EXPORT(CreateIoCompletionPort), VX_EXPORT(SetFileCompletionNotificationModes), VX_EXPORT(PostQueuedCompletionStatus),
    VX_EXPORT(GetQueuedCompletionStatus), VX_EXPORT(GetQueuedCompletionStatusEx), VX_EXPORT(GetOverlappedResult),
    VX_EXPORT(GetOverlappedResultEx), VX_EXPORT(CancelIo), VX_EXPORT(CancelIoEx),
    VX_EXPORT(WaitOnAddress), VX_EXPORT(WakeByAddressAll), VX_EXPORT(WakeByAddressSingle),
    VX_EXPORT(WaitForSingleObject), VX_EXPORT(WaitForSingleObjectEx), VX_EXPORT(WaitForMultipleObjects),
    VX_EXPORT(WaitForMultipleObjectsEx), VX_EXPORT(SleepEx), VX_EXPORT(CloseHandle), VX_EXPORT(DuplicateHandle),
    VX_EXPORT(InitializeCriticalSection), VX_EXPORT(InitializeCriticalSectionAndSpinCount), VX_EXPORT(InitializeCriticalSectionEx),
    VX_EXPORT(EnterCriticalSection), VX_EXPORT(TryEnterCriticalSection), VX_EXPORT(LeaveCriticalSection), VX_EXPORT(DeleteCriticalSection),
    VX_EXPORT(InitializeSRWLock), VX_EXPORT(AcquireSRWLockExclusive), VX_EXPORT(ReleaseSRWLockExclusive),
    VX_EXPORT(AcquireSRWLockShared), VX_EXPORT(ReleaseSRWLockShared),
    VX_EXPORT(TryAcquireSRWLockExclusive), VX_EXPORT(TryAcquireSRWLockShared),
    VX_EXPORT(InitializeConditionVariable), VX_EXPORT(WakeConditionVariable), VX_EXPORT(WakeAllConditionVariable),
    VX_EXPORT(SleepConditionVariableCS), VX_EXPORT(SleepConditionVariableSRW),
    VX_EXPORT(InitOnceInitialize), VX_EXPORT(InitOnceExecuteOnce),
    VX_EXPORT(FlsAlloc), VX_EXPORT(FlsGetValue), VX_EXPORT(FlsSetValue), VX_EXPORT(FlsFree),
    VX_EXPORT(InitializeSListHead), VX_EXPORT(InterlockedPushEntrySList), VX_EXPORT(InterlockedPopEntrySList),
    VX_EXPORT(InterlockedFlushSList), VX_EXPORT(QueryDepthSList),
};
static const vx_export sync_crt[] = { VX_EXPORT(_beginthreadex), VX_EXPORT(_endthreadex) };

void vx_sync_init(void) {
    vx_register_module("kernel32.dll", sync_k32, sizeof sync_k32 / sizeof sync_k32[0]);
    vx_register_module("kernelbase.dll", sync_k32, sizeof sync_k32 / sizeof sync_k32[0]);
    vx_register_module("ucrtbase.dll", sync_crt, sizeof sync_crt / sizeof sync_crt[0]);
    vx_register_module("msvcrt.dll", sync_crt, sizeof sync_crt / sizeof sync_crt[0]);
}
