// Job objects: processes grouped so that a program can watch them and end them together (Chromium puts its child
// processes in jobs; Steam watches the processes it starts).
//
// A job holds the processes assigned to it, each known by its pid and start time (so that a pid macOS gives to another
// process later is never taken for a member), and, unless the job lets them break away silently, the processes they
// start after joining, found through their parent ids when the job is looked at (vxrt children are spawned by their
// parent). TerminateJobObject and JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE end them. Other limits (processes, memory, CPU
// time and rate, UI) are kept and reported back but not enforced: macOS has no such per-group limits, and no process
// creation passes through here. A completion port associated with the job gets JOB_OBJECT_MSG_NEW_PROCESS,
// JOB_OBJECT_MSG_EXIT_PROCESS and JOB_OBJECT_MSG_ACTIVE_PROCESS_ZERO from a thread that looks at such jobs five times a
// second. Jobs live in this process: another process cannot open one by name.
#include <libproc.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/proc_info.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>
#include "vx.h"

int vx_process_info(HANDLE h, DWORD *code);   // src/sync.c

#define LIMIT_SILENT_BREAKAWAY_OK 0x1000u
#define LIMIT_KILL_ON_JOB_CLOSE   0x2000u
enum { MSG_ACTIVE_PROCESS_ZERO = 4, MSG_NEW_PROCESS = 6, MSG_EXIT_PROCESS = 7 };

typedef struct {
    int pid, alive;
    uint64_t start, joined;          // microseconds since 1970: when it started, when it joined
    uint64_t cpu;                    // its CPU time last seen (100 ns), counted for the job when it ends
    int announced;                   // 1: NEW_PROCESS posted (or not to be), 2: EXIT_PROCESS too
} member;
typedef struct job {
    struct job *next;
    int handles;                     // handles to it (CreateJobObject of an existing name, OpenJobObject add one)
    char *name;
    member *members; int nmembers, cmembers;
    uint8_t limits[144];             // JOBOBJECT_EXTENDED_LIMIT_INFORMATION (its basic limits first), as set
    DWORD ui, end_of_job, cpu_rate[2], total, terminated;
    uint64_t ended_cpu;              // CPU time of members that have ended (100 ns)
    HANDLE port; uint64_t key;       // the associated completion port (this file's own handle to it) and key
    int active_seen, zero_posted;
} job;
static job *g_jobs;
static pthread_mutex_t g_jobs_lock = PTHREAD_MUTEX_INITIALIZER;
static HANDLE g_ports_to_close[64];  // closed by the watcher: a job's last handle goes with the handle table locked
static int g_nports_to_close;

static BOOL fail(DWORD error) { vx_set_last_error(error); return 0; }
static int process_pid(HANDLE p) { return (intptr_t)p == -1 ? (int)getpid() : vx_process_info(p, NULL); }
static uint64_t now_us(void) { struct timeval tv; gettimeofday(&tv, NULL); return (uint64_t)tv.tv_sec * 1000000 + (uint64_t)tv.tv_usec; }
static DWORD limit_flags(const job *j) { return *(const DWORD *)(j->limits + 16); }
// A running process's start time and parent; 0 if it has ended (a zombie included).
static int process_facts(int pid, uint64_t *start, int *ppid) {
    struct proc_bsdinfo b;
    if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &b, sizeof b) != (int)sizeof b || b.pbi_status == 5 /* SZOMB */) return 0;
    if (start) *start = b.pbi_start_tvsec * 1000000 + b.pbi_start_tvusec;
    if (ppid) *ppid = (int)b.pbi_ppid;
    return 1;
}
static uint64_t cpu_time(int pid) {   // user and system time, 100 ns (the counters are in timebase ticks, hw.tbfrequency)
    static uint64_t hz;
    if (!hz) { size_t n = sizeof hz; if (sysctlbyname("hw.tbfrequency", &hz, &n, NULL, 0) || !hz) hz = 24000000; }
    struct rusage_info_v2 ru;
    if (proc_pid_rusage(pid, RUSAGE_INFO_V2, (rusage_info_t *)&ru)) return 0;
    return (uint64_t)((__uint128_t)(ru.ri_user_time + ru.ri_system_time) * 10000000u / hz);
}

// All of the following run with g_jobs_lock held (and none calls into the handle table: a job's last handle closes with
// that table's lock held).
static int live(const job *j) { for (job *k = g_jobs; k; k = k->next) if (k == j) return 1; return 0; }
static member *member_of(job *j, int pid) {
    for (int i = 0; i < j->nmembers; i++) if (j->members[i].alive && j->members[i].pid == pid) return &j->members[i];
    return NULL;
}
static member *add_member(job *j, int pid, uint64_t start, uint64_t joined) {
    if (j->nmembers == j->cmembers) { j->cmembers = j->cmembers ? j->cmembers * 2 : 8; j->members = realloc(j->members, (size_t)j->cmembers * sizeof *j->members); }
    member *m = &j->members[j->nmembers++];
    *m = (member){ pid, 1, start, joined, 0, j->port ? 0 : 1 };
    j->total++;
    return m;
}
static void ended(job *j, member *m) { m->alive = 0; j->ended_cpu += m->cpu; }
// Members that have ended; then (unless they may break away) the processes members started after they joined.
static void refresh(job *j, int descendants) {
    for (int i = 0; i < j->nmembers; i++) {
        member *m = &j->members[i];
        uint64_t start;
        if (m->alive && (!process_facts(m->pid, &start, NULL) || start != m->start)) ended(j, m);
    }
    if (!descendants || (limit_flags(j) & LIMIT_SILENT_BREAKAWAY_OK)) return;
    int alive = 0;
    for (int i = 0; i < j->nmembers; i++) alive += j->members[i].alive;
    if (!alive) return;
    int cap = proc_listallpids(NULL, 0) + 64;
    if (cap <= 64) return;
    int *pids = malloc((size_t)cap * sizeof *pids);
    int n = proc_listallpids(pids, cap * (int)sizeof *pids);
    for (int added = 1, pass = 0; added && pass < 8; pass++) {   // (children of children found in the pass before)
        added = 0;
        for (int i = 0; i < n; i++) {
            uint64_t start; int ppid;
            if (pids[i] <= 0 || member_of(j, pids[i]) || !process_facts(pids[i], &start, &ppid)) continue;
            member *parent = member_of(j, ppid);
            if (parent && start >= parent->joined) { add_member(j, pids[i], start, start); added = 1; }
        }
    }
    free(pids);
}
// Ends every member; 1 if this process is one (the caller ends it last).
static int end_members(job *j) {
    refresh(j, 1);
    int self = 0;
    for (int i = 0; i < j->nmembers; i++) {
        member *m = &j->members[i];
        uint64_t start;
        if (!m->alive) continue;
        if (m->pid == (int)getpid()) { self = 1; continue; }
        if (process_facts(m->pid, &start, NULL) && start == m->start) { kill(m->pid, SIGKILL); j->terminated++; }
        ended(j, m);
    }
    return self;
}
static void free_job(job *j) {
    for (job **p = &g_jobs; *p; p = &(*p)->next) if (*p == j) { *p = j->next; break; }
    if (j->port && g_nports_to_close < 64) g_ports_to_close[g_nports_to_close++] = j->port;
    free(j->members); free(j->name); free(j);
}

// A job's handle closed (with the handle table locked): the last one ends the job, and its processes if it says so.
static void job_handle_closed(void *data) {
    pthread_mutex_lock(&g_jobs_lock);
    job *j = data;
    if (live(j) && --j->handles <= 0) {
        int self = (limit_flags(j) & LIMIT_KILL_ON_JOB_CLOSE) ? end_members(j) : 0;
        free_job(j);
        pthread_mutex_unlock(&g_jobs_lock);
        if (self) { fflush(stdout); fflush(stderr); _exit(0); }   // (KILL_ON_JOB_CLOSE with this process in the job)
        return;
    }
    pthread_mutex_unlock(&g_jobs_lock);
}
static job *job_of(HANDLE h) { return vx_handle_data(h, VX_HANDLE_JOB); }   // then checked with live() under the lock

static HANDLE open_job(const char *name, int create) {
    pthread_mutex_lock(&g_jobs_lock);
    job *j = NULL;
    if (name && *name) for (job *k = g_jobs; k && !j; k = k->next) if (k->name && !strcasecmp(k->name, name)) j = k;
    int existed = j != NULL;
    if (!j && !create) { pthread_mutex_unlock(&g_jobs_lock); vx_set_last_error(2); return NULL; }   // ERROR_FILE_NOT_FOUND
    if (!j) {
        j = calloc(1, sizeof *j);
        j->name = name && *name ? strdup(name) : NULL;
        j->next = g_jobs; g_jobs = j;
    }
    j->handles++;
    pthread_mutex_unlock(&g_jobs_lock);
    HANDLE h = vx_handle_new(VX_HANDLE_JOB, j, job_handle_closed);
    if (!h) { job_handle_closed(j); vx_set_last_error(4); return NULL; }
    vx_set_last_error(existed && create ? 183 /* ERROR_ALREADY_EXISTS */ : 0);
    return h;
}
static WINAPI HANDLE CreateJobObjectA(void *sa, const char *name) { (void)sa; return open_job(name, 1); }
static WINAPI HANDLE CreateJobObjectW(void *sa, const WCHAR *name) { (void)sa; char *a = vx_utf8_dup(name); HANDLE h = open_job(a, 1); free(a); return h; }
static WINAPI HANDLE OpenJobObjectA(DWORD access, BOOL inherit, const char *name) { (void)access; (void)inherit; return open_job(name, 0); }
static WINAPI HANDLE OpenJobObjectW(DWORD access, BOOL inherit, const WCHAR *name) {
    (void)access; (void)inherit;
    char *a = vx_utf8_dup(name); HANDLE h = open_job(a, 0); free(a); return h;
}

static WINAPI BOOL AssignProcessToJobObject(HANDLE h, HANDLE process) {
    int pid = process_pid(process);
    job *j = job_of(h);
    if (!j || pid < 0) return fail(6);
    uint64_t start;
    if (!process_facts(pid, &start, NULL)) return fail(5);   // it has ended (ERROR_ACCESS_DENIED, as for a process terminating)
    uint64_t joined = now_us();
    pthread_mutex_lock(&g_jobs_lock);
    if (!live(j)) { pthread_mutex_unlock(&g_jobs_lock); return fail(6); }
    refresh(j, 0);
    member *m = member_of(j, pid);
    if (!m || m->start != start) add_member(j, pid, start, joined);
    pthread_mutex_unlock(&g_jobs_lock);
    if (vx_trace) vx_log("job %p: process %d assigned", (void *)j, pid);
    return 1;
}
static WINAPI BOOL TerminateJobObject(HANDLE h, DWORD code) {
    job *j = job_of(h);
    if (!j) return fail(6);
    pthread_mutex_lock(&g_jobs_lock);
    if (!live(j)) { pthread_mutex_unlock(&g_jobs_lock); return fail(6); }
    int self = end_members(j);
    pthread_mutex_unlock(&g_jobs_lock);
    if (vx_trace) vx_log("job %p: terminated (code %u)", (void *)j, code);
    if (self) {
        void (WINAPI *exit_process)(DWORD) = (void (WINAPI *)(DWORD))vx_find_export("kernel32.dll", "ExitProcess");
        exit_process(code);
    }
    return 1;
}

// ---- the watcher: completion port messages ---------------------------------------------------------------------------------
static void *watch_jobs(void *unused) {
    (void)unused;
    BOOL (WINAPI *post)(HANDLE, DWORD, uint64_t, void *) = (BOOL (WINAPI *)(HANDLE, DWORD, uint64_t, void *))vx_find_export("kernel32.dll", "PostQueuedCompletionStatus");
    BOOL (WINAPI *close_handle)(HANDLE) = (BOOL (WINAPI *)(HANDLE))vx_find_export("kernel32.dll", "CloseHandle");
    for (unsigned tick = 0;; tick++) {
        struct { HANDLE port; uint64_t key; DWORD msg; uintptr_t pid; } posts[256];
        HANDLE closing[64];
        int nposts = 0, nclosing = 0;
        pthread_mutex_lock(&g_jobs_lock);
        for (job *j = g_jobs; j; j = j->next) {
            if (!j->port) continue;
            refresh(j, tick % 5 == 0);                 // (new descendants once a second, ended members every time)
            int active = 0;
            for (int i = 0; i < j->nmembers && nposts < 254; i++) {
                member *m = &j->members[i];
                if (m->alive) { active++; m->cpu = cpu_time(m->pid); }
                if (!m->announced) { posts[nposts++] = (typeof(posts[0])){ j->port, j->key, MSG_NEW_PROCESS, (uintptr_t)m->pid }; m->announced = 1; }
                if (!m->alive && m->announced == 1) { posts[nposts++] = (typeof(posts[0])){ j->port, j->key, MSG_EXIT_PROCESS, (uintptr_t)m->pid }; m->announced = 2; }
            }
            if (active) { j->active_seen = 1; j->zero_posted = 0; }
            else if (j->active_seen && !j->zero_posted && nposts < 256) {
                posts[nposts++] = (typeof(posts[0])){ j->port, j->key, MSG_ACTIVE_PROCESS_ZERO, 0 };
                j->zero_posted = 1;
            }
        }
        while (g_nports_to_close && nclosing < 64) closing[nclosing++] = g_ports_to_close[--g_nports_to_close];
        pthread_mutex_unlock(&g_jobs_lock);
        for (int i = 0; i < nposts; i++) post(posts[i].port, posts[i].msg, posts[i].key, (void *)posts[i].pid);
        for (int i = 0; i < nclosing; i++) close_handle(closing[i]);
        usleep(200000);
    }
    return NULL;
}

// JOBOBJECT_BASIC_LIMIT_INFORMATION (64 bytes, LimitFlags at 16) inside JOBOBJECT_EXTENDED_LIMIT_INFORMATION (144),
// JOBOBJECT_ASSOCIATE_COMPLETION_PORT (key, port), JOBOBJECT_CPU_RATE_CONTROL_INFORMATION (flags, rate).
static WINAPI BOOL SetInformationJobObject(HANDLE h, int cls, void *info, DWORD n) {
    job *j = job_of(h);
    if (!j) return fail(6);
    if (!info) return fail(87);
    const DWORD *v = info;
    HANDLE port = NULL;
    if (cls == 7) {                                    // the port: checked and taken outside the job lock
        if (n < 16) return fail(24);
        const char *type = NULL;
        HANDLE given = ((HANDLE *)info)[1];
        if (!vx_handle_object(given, NULL, &type, NULL, NULL, 0, NULL) || !type || strcmp(type, "IoCompletion")) return fail(87);
        BOOL (WINAPI *dup)(HANDLE, HANDLE, HANDLE, HANDLE *, DWORD, BOOL, DWORD) =
            (BOOL (WINAPI *)(HANDLE, HANDLE, HANDLE, HANDLE *, DWORD, BOOL, DWORD))vx_find_export("kernel32.dll", "DuplicateHandle");
        if (!dup((HANDLE)(intptr_t)-1, given, (HANDLE)(intptr_t)-1, &port, 0, 0, 2 /* DUPLICATE_SAME_ACCESS */)) return 0;
    }
    DWORD error = 0;
    int start_watcher = 0;
    pthread_mutex_lock(&g_jobs_lock);
    if (!live(j)) error = 6;
    else switch (cls) {
    case 2:                                            // JobObjectBasicLimitInformation
        if (n < 64) error = 24; else if (v[4] & ~0xffu) error = 87; else memcpy(j->limits, info, 64);
        break;
    case 9:                                            // JobObjectExtendedLimitInformation
        if (n < 144) error = 24; else if (v[4] & ~0x7fffu) error = 87; else memcpy(j->limits, info, 144);
        break;
    case 4:                                            // JobObjectBasicUIRestrictions
        if (n < 4) error = 24; else if (v[0] & ~0xffu) error = 87; else j->ui = v[0];
        break;
    case 6:                                            // JobObjectEndOfJobTimeInformation
        if (n < 4) error = 24; else if (v[0] > 1) error = 87; else j->end_of_job = v[0];
        break;
    case 7:                                            // JobObjectAssociateCompletionPortInformation (once per job)
        if (j->port) error = 87;
        else {
            j->port = port; j->key = ((uint64_t *)info)[0]; port = NULL;
            for (int i = 0; i < j->nmembers; i++) j->members[i].announced = 1;   // (members already there are not announced)
            static int watching;
            start_watcher = !watching++;
        }
        break;
    case 15:                                           // JobObjectCpuRateControlInformation
        if (n < 8) error = 24; else if (v[0] & ~0x1fu) error = 87; else { j->cpu_rate[0] = v[0]; j->cpu_rate[1] = v[1]; }
        break;
    default: error = 87;
    }
    pthread_mutex_unlock(&g_jobs_lock);
    if (port) { BOOL (WINAPI *close_handle)(HANDLE) = (BOOL (WINAPI *)(HANDLE))vx_find_export("kernel32.dll", "CloseHandle"); close_handle(port); }
    if (start_watcher) {
        pthread_t t; pthread_attr_t a;
        pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED); pthread_attr_setstacksize(&a, 256 << 10);
        pthread_create(&t, &a, watch_jobs, NULL);
        pthread_attr_destroy(&a);
    }
    if (vx_trace && !error && cls != 7) vx_log("job %p: information class %d kept (limits are not enforced)", (void *)j, cls);
    return error ? fail(error) : 1;
}
static job *job_of_this_process(void) {               // with g_jobs_lock held
    for (job *j = g_jobs; j; j = j->next) { refresh(j, 0); if (member_of(j, (int)getpid())) return j; }
    return NULL;
}
static WINAPI BOOL QueryInformationJobObject(HANDLE h, int cls, void *info, DWORD n, DWORD *ret) {
    job *given = h ? job_of(h) : NULL;
    if (h && !given) return fail(6);
    DWORD need;
    switch (cls) {
    case 1: need = 48; break; case 2: need = 64; break; case 3: need = 16; break; case 4: need = 4; break;
    case 6: need = 4; break; case 8: need = 96; break; case 9: need = 144; break; case 15: need = 8; break;
    default: return fail(87);
    }
    if (n < need) return fail(24);
    if (!info) return fail(87);
    pthread_mutex_lock(&g_jobs_lock);
    job *j = h ? (live(given) ? given : NULL) : job_of_this_process();
    if (!j) { pthread_mutex_unlock(&g_jobs_lock); return fail(6); }
    refresh(j, 1);
    DWORD error = 0;
    memset(info, 0, cls == 3 ? 8 : need);
    uint32_t *w = info; uint64_t *q = info;
    switch (cls) {
    case 1: case 8: {                                  // accounting: CPU times (100 ns), processes; (8: and no I/O counted)
        uint64_t cpu = j->ended_cpu;
        DWORD active = 0;
        for (int i = 0; i < j->nmembers; i++) if (j->members[i].alive) { active++; j->members[i].cpu = cpu_time(j->members[i].pid); cpu += j->members[i].cpu; }
        q[0] = q[2] = cpu;                             // total and this period's user time (kernel time not told apart)
        w[9] = j->total; w[10] = active; w[11] = j->terminated;
        break;
    }
    case 2: memcpy(info, j->limits, 64); break;
    case 9: memcpy(info, j->limits, 144); break;
    case 4: w[0] = j->ui; break;
    case 6: w[0] = j->end_of_job; break;
    case 15: w[0] = j->cpu_rate[0]; w[1] = j->cpu_rate[1]; break;
    case 3: {                                          // JOBOBJECT_BASIC_PROCESS_ID_LIST: active members, as many ids as fit
        DWORD active = 0, listed = 0, room = (n - 8) / 8;
        for (int i = 0; i < j->nmembers; i++) {
            if (!j->members[i].alive) continue;
            active++;
            if (listed < room) q[1 + listed++] = (uint64_t)j->members[i].pid;
        }
        w[0] = active; w[1] = listed;
        need = 8 + listed * 8;
        if (listed < active) error = 234;              // ERROR_MORE_DATA
        break;
    }
    }
    pthread_mutex_unlock(&g_jobs_lock);
    if (ret) *ret = need;
    return error ? fail(error) : 1;
}
// Whether a process is in the job (job NULL: in any job of this process's).
static WINAPI BOOL IsProcessInJob(HANDLE process, HANDLE h, BOOL *result) {
    int pid = process_pid(process);
    job *given = h ? job_of(h) : NULL;
    if (pid < 0 || (h && !given)) return fail(6);
    if (!result) return fail(87);
    int in = 0;
    pthread_mutex_lock(&g_jobs_lock);
    for (job *j = g_jobs; j && !in; j = j->next) {
        if (given && j != given) continue;
        refresh(j, 1);
        in = member_of(j, pid) != NULL;
    }
    pthread_mutex_unlock(&g_jobs_lock);
    *result = in;
    return 1;
}

static const vx_export jobs_k32[] = {
    VX_EXPORT(CreateJobObjectA), VX_EXPORT(CreateJobObjectW), VX_EXPORT(OpenJobObjectA), VX_EXPORT(OpenJobObjectW),
    VX_EXPORT(AssignProcessToJobObject), VX_EXPORT(TerminateJobObject), VX_EXPORT(SetInformationJobObject),
    VX_EXPORT(QueryInformationJobObject), VX_EXPORT(IsProcessInJob),
};
void vx_jobs_init(void) {
    vx_register_module("kernel32.dll", jobs_k32, sizeof jobs_k32 / sizeof jobs_k32[0]);
    vx_register_module("kernelbase.dll", jobs_k32, sizeof jobs_k32 / sizeof jobs_k32[0]);
}

#ifdef __aarch64__
#include "thunks/jobs.c.inc"
#endif
