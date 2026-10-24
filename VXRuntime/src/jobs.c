// Job objects: processes grouped so that a program can watch them and end them together (Chromium puts its child
// processes in jobs; Steam watches the processes it starts).
//
// A job holds the processes assigned to it, each known by its pid and start time (so that a pid macOS gives to another
// process later is never taken for a member), and, unless the job lets them break away silently, the processes they
// start after joining, found through their parent ids when the job is looked at (vxrt children are spawned by their
// parent). TerminateJobObject ends them, and so does JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE when the job's last handle
// closes: by CloseHandle, or because this process ends (Windows closes every handle of a process that ends). An
// orderly end (ExitProcess, exit, an unhandled exception) ends the members at once (vx_jobs_process_exit); for any end
// (TerminateProcess from another process is SIGKILL here, a crash in this runtime) a small watchdog process, started
// the first time such a job has a member, is told the members and ends them when this process has gone.
//
// Limits: only what can really be applied is accepted. KILL_ON_JOB_CLOSE and SILENT_BREAKAWAY_OK are applied;
// DIE_ON_UNHANDLED_EXCEPTION always holds here (an unhandled exception ends a vxrt process, no error box is shown);
// BREAKAWAY_OK is allowed (a member's children stay in the job all the same, never fewer than asked). Limits that
// restrict what members may do or use (ACTIVE_PROCESS, PROCESS_TIME, JOB_TIME, PROCESS_MEMORY, JOB_MEMORY, and any UI
// restriction) cannot be applied - macOS has no such per-group limits and no member's process creation passes through
// here - so SetInformationJobObject refuses them with ERROR_NOT_SUPPORTED instead of reporting a sandbox that does not
// hold (Chromium's sandboxed processes need --no-sandbox here). Scheduling hints (working set, affinity, priority and
// scheduling class, CPU rate) are kept and reported back but not applied, which is said in the log once per job.
// A completion port associated with the job gets JOB_OBJECT_MSG_NEW_PROCESS, JOB_OBJECT_MSG_EXIT_PROCESS and
// JOB_OBJECT_MSG_ACTIVE_PROCESS_ZERO from a thread that looks at such jobs five times a second. Jobs live in this
// process: another process cannot open one by name.
#include <errno.h>
#include <fcntl.h>
#include <libproc.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/proc_info.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#include "vx.h"

int vx_process_info(HANDLE h, DWORD *code);   // src/sync.c

#define LIMIT_SILENT_BREAKAWAY_OK 0x1000u
#define LIMIT_KILL_ON_JOB_CLOSE   0x2000u
// PROCESS_TIME 0x2, JOB_TIME 0x4, ACTIVE_PROCESS 0x8, PROCESS_MEMORY 0x100, JOB_MEMORY 0x200: not applicable, refused
#define LIMITS_REFUSED            0x30eu
// WORKINGSET 0x1, AFFINITY 0x10, PRIORITY_CLASS 0x20, PRESERVE_JOB_TIME 0x40, SCHEDULING_CLASS 0x80, SUBSET_AFFINITY 0x4000
#define LIMITS_HINTS              0x40f1u
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
    int zero_due;                    // a process joined since the last ACTIVE_PROCESS_ZERO (or the port's association)
    int hints_told;                  // the log has said that its scheduling hints (1), its CPU rate (2) are not applied
    uint64_t watched;                // (in the watchdog: the job's id in the process that holds it)
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
static void watch_member(job *j, const member *m);
static member *add_member(job *j, int pid, uint64_t start, uint64_t joined) {
    if (j->nmembers == j->cmembers) { j->cmembers = j->cmembers ? j->cmembers * 2 : 8; j->members = realloc(j->members, (size_t)j->cmembers * sizeof *j->members); }
    member *m = &j->members[j->nmembers++];
    *m = (member){ pid, 1, start, joined, 0, j->port ? 0 : 1 };
    j->total++;
    if (j->port) j->zero_due = 1;          // (however soon it ends, the job's count goes to zero after it)
    if (limit_flags(j) & LIMIT_KILL_ON_JOB_CLOSE) watch_member(j, m);
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

// ---- the watchdog: KILL_ON_JOB_CLOSE however this process ends ----------------------------------------------------------
// Windows closes the handles of a process that ends, however it ends, and a kill-on-close job then ends its processes.
// A process ended by SIGKILL (TerminateProcess here) or by a crash in this runtime runs nothing more, so another process
// does it: "vxrt --vxrt-job-watchdog" (src/loader.c's main), started the first time a kill-on-close job has a member.
// It is told each member of such jobs (pid, start time, when it joined, the job's flags) and when a job is forgotten
// (closed, or no longer kill-on-close), over a socket only this process holds (never inherited: close-on-exec, and
// its other end only in the watchdog); when that socket reads end-of-file this process has gone, and the watchdog ends
// the members still running with the same start time and the processes they started (as refresh() finds them), then
// exits. It leaves its first process at once, so that it is no member's descendant (launchd is its parent).
typedef struct { uint64_t job, start, joined; uint32_t op, pid, flags, unused; } watch_record;
enum { WATCH_MEMBER = 1, WATCH_FORGET = 2, WATCH_HOLDER = 3 };
static int g_watch_fd = -1, g_watch_starts, g_in_watchdog;

static void *reap_watchdog(void *pid) {      // (its first process exits as soon as it has left)
    while (waitpid((pid_t)(intptr_t)pid, NULL, 0) < 0 && errno == EINTR) {}
    return NULL;
}
static int watch_send(const watch_record *r);
static int watch_start(void) {
    if (g_watch_starts >= 3) return 0;          // (a watchdog that cannot start or keeps going away: not retried forever)
    g_watch_starts++;
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) return 0;
    fcntl(sv[0], F_SETFD, FD_CLOEXEC); fcntl(sv[1], F_SETFD, FD_CLOEXEC);
    int on = 1, size = 256 << 10;
    struct timeval timeout = { 2, 0 };          // (never blocked for long, with the handle table locked, by a stuck watchdog)
    setsockopt(sv[0], SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
    setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &size, sizeof size);
    setsockopt(sv[0], SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
    if (sv[1] <= 3) { int d = fcntl(sv[1], F_DUPFD_CLOEXEC, 4); close(sv[1]); sv[1] = d; }   // (not where the file actions put others)
    char self[1024]; uint32_t len = sizeof self;
    char *argv[] = { self, "--vxrt-job-watchdog", NULL };
    extern char **environ;
    posix_spawn_file_actions_t fa; posix_spawnattr_t attr;
    posix_spawn_file_actions_init(&fa); posix_spawnattr_init(&attr);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addinherit_np(&fa, 2);                 // (its log line goes where this process's go)
    posix_spawn_file_actions_adddup2(&fa, sv[1], 3);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);   // nothing else of this process's
    pid_t pid = 0;
    int r = sv[1] < 0 ? EBADF : _NSGetExecutablePath(self, &len) ? ENAMETOOLONG : posix_spawn(&pid, self, &fa, &attr, argv, environ);
    posix_spawn_file_actions_destroy(&fa); posix_spawnattr_destroy(&attr);
    if (sv[1] >= 0) close(sv[1]);
    if (r) { close(sv[0]); vx_log("jobs: cannot start the watchdog (%s): KILL_ON_JOB_CLOSE holds only if this process ends in order", strerror(r)); return 0; }
    pthread_t t; pthread_attr_t a;
    pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED); pthread_attr_setstacksize(&a, 64 << 10);
    pthread_create(&t, &a, reap_watchdog, (void *)(intptr_t)pid);
    pthread_attr_destroy(&a);
    g_watch_fd = sv[0];
    watch_record hello = { 0, 0, 0, WATCH_HOLDER, (uint32_t)getpid(), 0, 0 };   // (for its log line)
    watch_send(&hello);
    for (job *j = g_jobs; j; j = j->next)        // what a watchdog started again has not been told
        if (limit_flags(j) & LIMIT_KILL_ON_JOB_CLOSE)
            for (int i = 0; i < j->nmembers; i++) if (j->members[i].alive) watch_member(j, &j->members[i]);
    return g_watch_fd >= 0;
}
static int watch_send(const watch_record *r) {
    if (g_watch_fd < 0 && !watch_start()) return 0;
    for (size_t done = 0; done < sizeof *r;) {
        ssize_t k = write(g_watch_fd, (const char *)r + done, sizeof *r - done);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) {
            vx_log("jobs: the watchdog stopped listening (%s)", k < 0 ? strerror(errno) : "closed");
            close(g_watch_fd); g_watch_fd = -1;
            return 0;
        }
        done += (size_t)k;
    }
    return 1;
}
static void watch_member(job *j, const member *m) {
    if (g_in_watchdog) return;
    watch_record r = { (uint64_t)(uintptr_t)j, m->start, m->joined, WATCH_MEMBER, (uint32_t)m->pid, limit_flags(j), 0 };
    watch_send(&r);
}
static void watch_forget(job *j) {
    if (g_in_watchdog || g_watch_fd < 0) return;
    watch_record r = { (uint64_t)(uintptr_t)j, 0, 0, WATCH_FORGET, 0, 0, 0 };
    watch_send(&r);
}
// The job's flags changed: the watchdog forgets it, and is told its members again if it is (still) kill-on-close.
static void watch_job(job *j) {
    watch_forget(j);
    if (limit_flags(j) & LIMIT_KILL_ON_JOB_CLOSE)
        for (int i = 0; i < j->nmembers; i++) if (j->members[i].alive) watch_member(j, &j->members[i]);
}

int vx_jobs_watchdog(void) {
    int holder = 0;
    if (fork() > 0) _exit(0);                    // the child carries on (or this process, if fork failed)
    g_in_watchdog = 1;
    watch_record r;
    size_t have = 0;
    for (;;) {
        ssize_t k = read(3, (char *)&r + have, sizeof r - have);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) break;                       // (end of file: the process that holds the jobs has gone)
        if ((have += (size_t)k) < sizeof r) continue;
        have = 0;
        if (r.op == WATCH_HOLDER) { holder = (int)r.pid; continue; }
        job *j = NULL;
        for (job *k2 = g_jobs; k2 && !j; k2 = k2->next) if (k2->watched == r.job) j = k2;
        if (r.op == WATCH_FORGET) { if (j) free_job(j); continue; }
        if (r.op != WATCH_MEMBER) continue;
        if (!j) { j = calloc(1, sizeof *j); j->watched = r.job; j->next = g_jobs; g_jobs = j; }
        *(DWORD *)(j->limits + 16) = r.flags;
        member *m = member_of(j, (int)r.pid);
        if (!m || m->start != r.start) add_member(j, (int)r.pid, r.start, r.joined);
    }
    DWORD ended = 0;
    for (job *j = g_jobs; j; j = j->next) { end_members(j); ended += j->terminated; }
    if (ended) vx_log("jobs: process %d has ended: %u processes of its KILL_ON_JOB_CLOSE jobs ended with it", holder, (unsigned)ended);
    return 0;
}

// This process is ending (ExitProcess, exit, an unhandled exception): as Windows closes its handles, its kill-on-close
// jobs end their processes now. The jobs stay locked: nothing joins them any more.
void vx_jobs_process_exit(void) {
    static int once;
    if (__atomic_exchange_n(&once, 1, __ATOMIC_ACQ_REL)) return;
    int locked = 0;
    for (int i = 0; i < 100 && !(locked = !pthread_mutex_trylock(&g_jobs_lock)); i++) usleep(1000);
    if (!locked) return;                         // (held by a thread that stopped with it: the watchdog does this)
    for (job *j = g_jobs; j; j = j->next) if (limit_flags(j) & LIMIT_KILL_ON_JOB_CLOSE) end_members(j);
}

// A job's handle closed (with the handle table locked): the last one ends the job, and its processes if it says so.
static void job_handle_closed(void *data) {
    pthread_mutex_lock(&g_jobs_lock);
    job *j = data;
    if (live(j) && --j->handles <= 0) {
        int self = 0;
        if (limit_flags(j) & LIMIT_KILL_ON_JOB_CLOSE) { self = end_members(j); watch_forget(j); }
        free_job(j);
        pthread_mutex_unlock(&g_jobs_lock);
        if (self) {                                                // (KILL_ON_JOB_CLOSE with this process in the job)
            vx_jobs_process_exit();
            fflush(stdout); fflush(stderr); _exit(0);
        }
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
            // Every member is counted; messages go out while there is room (one place kept for this job's ZERO), the
            // rest next time, and ZERO only after all of them: it says the job has no process left.
            int active = 0, complete = 1;
            for (int i = 0; i < j->nmembers; i++) {
                member *m = &j->members[i];
                if (m->alive) { active++; m->cpu = cpu_time(m->pid); }
                int need = !m->announced + (!m->alive && m->announced < 2);
                if (!need) continue;
                if (nposts + need >= (int)(sizeof posts / sizeof posts[0])) { complete = 0; continue; }
                if (!m->announced) { posts[nposts++] = (typeof(posts[0])){ j->port, j->key, MSG_NEW_PROCESS, (uintptr_t)m->pid }; m->announced = 1; }
                if (!m->alive && m->announced == 1) { posts[nposts++] = (typeof(posts[0])){ j->port, j->key, MSG_EXIT_PROCESS, (uintptr_t)m->pid }; m->announced = 2; }
            }
            if (!active && complete && j->zero_due && nposts < (int)(sizeof posts / sizeof posts[0])) {
                posts[nposts++] = (typeof(posts[0])){ j->port, j->key, MSG_ACTIVE_PROCESS_ZERO, 0 };
                j->zero_due = 0;
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
    DWORD error = 0, refused = 0, hints = 0;
    int start_watcher = 0;
    pthread_mutex_lock(&g_jobs_lock);
    if (!live(j)) error = 6;
    else switch (cls) {
    case 2: case 9: {                                  // JobObjectBasicLimitInformation, JobObjectExtendedLimitInformation
        DWORD size = cls == 2 ? 64 : 144, before = limit_flags(j);
        if (n < size) error = 24;
        else if (v[4] & ~(cls == 2 ? 0xffu : 0x7fffu)) error = 87;
        else if ((refused = v[4] & LIMITS_REFUSED)) error = 50;          // ERROR_NOT_SUPPORTED: they would not hold
        else {
            memcpy(j->limits, info, size);
            hints = v[4] & LIMITS_HINTS;
            if ((before ^ v[4]) & (LIMIT_KILL_ON_JOB_CLOSE | LIMIT_SILENT_BREAKAWAY_OK)) watch_job(j);
        }
        break;
    }
    case 4:                                            // JobObjectBasicUIRestrictions: none can be applied
        if (n < 4) error = 24; else if (v[0] & ~0xffu) error = 87; else if ((refused = v[0])) error = 50; else j->ui = 0;
        break;
    case 6:                                            // JobObjectEndOfJobTimeInformation (no job time limit is accepted)
        if (n < 4) error = 24; else if (v[0] > 1) error = 87; else j->end_of_job = v[0];
        break;
    case 7:                                            // JobObjectAssociateCompletionPortInformation (once per job)
        if (j->port) error = 87;
        else {
            j->port = port; j->key = ((uint64_t *)info)[0]; port = NULL;
            j->zero_due = 0;
            for (int i = 0; i < j->nmembers; i++) {     // (members already there are not announced; their end counts)
                j->members[i].announced = 1;
                if (j->members[i].alive) j->zero_due = 1;
            }
            static int watching;
            start_watcher = !watching++;
        }
        break;
    case 15:                                           // JobObjectCpuRateControlInformation
        if (n < 8) error = 24; else if (v[0] & ~0x1fu) error = 87; else { j->cpu_rate[0] = v[0]; j->cpu_rate[1] = v[1]; hints = v[0] & 1; }
        break;
    default: error = 87;
    }
    int kind = cls == 15 ? 2 : 1, tell_hints = hints && !(j->hints_told & kind);
    if (tell_hints) j->hints_told |= kind;
    pthread_mutex_unlock(&g_jobs_lock);
    if (port) { BOOL (WINAPI *close_handle)(HANDLE) = (BOOL (WINAPI *)(HANDLE))vx_find_export("kernel32.dll", "CloseHandle"); close_handle(port); }
    if (start_watcher) {
        pthread_t t; pthread_attr_t a;
        pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED); pthread_attr_setstacksize(&a, 256 << 10);
        pthread_create(&t, &a, watch_jobs, NULL);
        pthread_attr_destroy(&a);
    }
    // (always: a program whose sandbox or limits are refused may stop, and this says why)
    if (refused) vx_log("job %p: %s %#x refused (ERROR_NOT_SUPPORTED): vxrt cannot apply them", (void *)j, cls == 4 ? "UI restrictions" : "limits", (unsigned)refused);
    if (tell_hints && cls == 15) vx_log("job %p: CPU rate control kept but not applied", (void *)j);
    else if (tell_hints) vx_log("job %p: limits %#x (working set, affinity, priority or scheduling class) kept but not applied", (void *)j, (unsigned)hints);
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
