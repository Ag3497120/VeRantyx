// kernel32 and ntdll functions the Windows Steam client and its Chromium (steamwebhelper) import beyond the rest of
// this runtime: serial ports (there are none), memory notifications and hints, threads in this process started
// "remotely", thread and process information, volumes and DOS device names of the C: and Z: drives, DLL directories
// (the loader's search, src/loader.c, asks this file), time zone conversions with a given zone, Windows Error
// Reporting registrations, NtQueryObject, NtWriteFile and NTSTATUS codes (src/ntstatus.c). Job objects: src/jobs.c.
//
// Its tables are registered first (src/loader.c): where an older stand-in elsewhere has the same name (job objects in
// desktop.c, DLL directories in extras.c, volume paths in client.c, SystemTimeToTzSpecificLocalTime in kernel32.c),
// the one here is the one programs get.
#include <ctype.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <fcntl.h>
#include <libproc.h>
#include <mach/mach.h>
#include <mach/vm_page_size.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>
#include "vx.h"

int vx_process_info(HANDLE h, DWORD *code);   // src/sync.c: a process handle's pid, -1 if it is none
int vx_is_pipe(HANDLE h);                     // src/pipes.c

static BOOL fail(DWORD error) { vx_set_last_error(error); return 0; }
// The pid behind a process handle (the current process's pseudo handle included), -1 if it is not one.
static int process_pid(HANDLE p) { return (intptr_t)p == -1 ? (int)getpid() : vx_process_info(p, NULL); }
static BOOL this_process_only(HANDLE p) {        // other processes' memory and threads cannot be reached from here
    int pid = process_pid(p);
    if (pid < 0) return fail(6);                  // ERROR_INVALID_HANDLE
    if (pid != (int)getpid()) return fail(5);     // ERROR_ACCESS_DENIED, as for a handle without the access needed
    return 1;
}
// A pointer this file stores into the program's memory, as the program sees it (the vCPU's memory below 4 GB is the
// host's at an offset, vx.h); and the program's address of memory this file reads.
#ifdef __aarch64__
#define TO_PROGRAM(p) vx_unlow((uint64_t)(uintptr_t)(p))
#else
#define TO_PROGRAM(p) ((uint64_t)(uintptr_t)(p))
#endif
static DWORD thread_id(HANDLE h) {                // 0 if h is not a thread handle (GetCurrentThread's included)
    DWORD (WINAPI *id)(HANDLE) = (DWORD (WINAPI *)(HANDLE))vx_find_export("kernel32.dll", "GetThreadId");
    return id ? id(h) : 0;
}

// ---- serial ports: there are none ------------------------------------------------------------------------------------
// A file, pipe or console handle fails the way such handles do on Windows (the driver under them knows no serial
// requests: ERROR_INVALID_FUNCTION); anything else is not a handle these functions take (ERROR_INVALID_HANDLE).
static BOOL not_a_comm_port(HANDLE h) { return fail(vx_handle_fd(h) >= 0 || vx_is_pipe(h) ? 1 : 6); }
static WINAPI BOOL ClearCommError(HANDLE h, DWORD *errors, void *stat) { (void)errors; (void)stat; return not_a_comm_port(h); }
static WINAPI BOOL EscapeCommFunction(HANDLE h, DWORD func) { (void)func; return not_a_comm_port(h); }
static WINAPI BOOL GetCommModemStatus(HANDLE h, DWORD *status) { (void)status; return not_a_comm_port(h); }
static WINAPI BOOL GetCommState(HANDLE h, void *dcb) { (void)dcb; return not_a_comm_port(h); }
static WINAPI BOOL SetCommState(HANDLE h, void *dcb) { (void)dcb; return not_a_comm_port(h); }
static WINAPI BOOL SetCommTimeouts(HANDLE h, void *timeouts) { (void)timeouts; return not_a_comm_port(h); }
static WINAPI BOOL PurgeComm(HANDLE h, DWORD flags) { (void)flags; return not_a_comm_port(h); }

// ---- memory ------------------------------------------------------------------------------------------------------------
// Memory resource notifications: one manual-reset event per kind, as on Windows (every caller gets a handle to the
// same object), signalled while the condition holds. The condition is macOS's memory pressure: low memory while the
// system is at its warning or critical level, high memory while it is normal. A dispatch source follows the changes.
static HANDLE g_memory_events[2];                 // LowMemoryResourceNotification, HighMemoryResourceNotification
static const void *g_memory_objects[2];
static pthread_mutex_t g_memory_lock = PTHREAD_MUTEX_INITIALIZER;
static int pressure_level(void) {                 // 1 normal, 2 warning, 4 critical
    int level = 1; size_t n = sizeof level;
    if (sysctlbyname("kern.memorystatus_vm_pressure_level", &level, &n, NULL, 0)) level = 1;
    return level;
}
static int memory_condition(int type, int level) { return type == 0 ? level >= 2 : level <= 1; }
static void memory_changed(void *unused) {       // (a dispatch thread: no Windows thread, the event handles are ours)
    (void)unused;
    BOOL (WINAPI *reset)(HANDLE) = (BOOL (WINAPI *)(HANDLE))vx_find_export("kernel32.dll", "ResetEvent");
    int level = pressure_level();
    pthread_mutex_lock(&g_memory_lock);
    for (int t = 0; t < 2; t++) {
        if (!g_memory_events[t]) continue;
        if (memory_condition(t, level)) vx_set_event(g_memory_events[t]);
        else reset(g_memory_events[t]);
    }
    pthread_mutex_unlock(&g_memory_lock);
}
static WINAPI HANDLE CreateMemoryResourceNotification(int type) {
    if (type != 0 && type != 1) { vx_set_last_error(87); return NULL; }
    HANDLE (WINAPI *create)(void *, BOOL, BOOL, const WCHAR *) = (HANDLE (WINAPI *)(void *, BOOL, BOOL, const WCHAR *))vx_find_export("kernel32.dll", "CreateEventW");
    BOOL (WINAPI *dup)(HANDLE, HANDLE, HANDLE, HANDLE *, DWORD, BOOL, DWORD) =
        (BOOL (WINAPI *)(HANDLE, HANDLE, HANDLE, HANDLE *, DWORD, BOOL, DWORD))vx_find_export("kernel32.dll", "DuplicateHandle");
    static int watching;
    pthread_mutex_lock(&g_memory_lock);
    if (!g_memory_events[type]) {
        HANDLE e = create(NULL, 1, memory_condition(type, pressure_level()), NULL);
        if (e && vx_handle_object(e, &g_memory_objects[type], NULL, NULL, NULL, 0, NULL)) g_memory_events[type] = e;
    }
    if (g_memory_events[type] && !watching++) {
        dispatch_source_t s = dispatch_source_create(DISPATCH_SOURCE_TYPE_MEMORYPRESSURE, 0,
            DISPATCH_MEMORYPRESSURE_NORMAL | DISPATCH_MEMORYPRESSURE_WARN | DISPATCH_MEMORYPRESSURE_CRITICAL,
            dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
        dispatch_source_set_event_handler_f(s, memory_changed);
        dispatch_resume(s);
    }
    HANDLE h = NULL;
    if (g_memory_events[type] && !dup((HANDLE)(intptr_t)-1, g_memory_events[type], (HANDLE)(intptr_t)-1, &h, 0, 0, 2 /* DUPLICATE_SAME_ACCESS */)) h = NULL;
    pthread_mutex_unlock(&g_memory_lock);
    if (!h) vx_set_last_error(8);
    return h;
}
static WINAPI BOOL QueryMemoryResourceNotification(HANDLE h, BOOL *state) {
    const void *object = NULL;
    if (!vx_handle_object(h, &object, NULL, NULL, NULL, 0, NULL)) return fail(6);
    for (int t = 0; t < 2; t++)
        if (object && object == g_memory_objects[t]) {
            if (!state) return fail(87);
            *state = memory_condition(t, pressure_level());
            return 1;
        }
    return fail(6);
}

typedef struct { void *base, *alloc_base; DWORD alloc_protect; uint16_t partition, pad; size_t region; DWORD state, protect, type, pad2; } MEMORY_BASIC_INFORMATION;
static size_t query(const void *addr, MEMORY_BASIC_INFORMATION *mbi) {
    size_t (WINAPI *q)(const void *, MEMORY_BASIC_INFORMATION *, size_t) = (size_t (WINAPI *)(const void *, MEMORY_BASIC_INFORMATION *, size_t))vx_find_export("kernel32.dll", "VirtualQuery");
    return q(addr, mbi, sizeof *mbi);
}
// The pages stay committed; their contents may go. Only read-write pages can be discarded (as on Windows); the x86
// build lets macOS take the pages back. (The ARM64 build leaves them: the vCPU maps that memory, src/vm_hooks.c.)
static WINAPI DWORD DiscardVirtualMemory(void *addr, size_t size) {
    if (((uintptr_t)addr & 0xfff) || !size || (size & 0xfff)) return 87;          // ERROR_INVALID_PARAMETER
    for (uintptr_t a = (uintptr_t)addr, end = a + size; a < end; ) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!query((void *)a, &mbi) || mbi.state != 0x1000 /* MEM_COMMIT */) return 487;   // ERROR_INVALID_ADDRESS
        if (mbi.protect != 0x04 /* PAGE_READWRITE */) return 87;
        if (!mbi.region) break;
        a = (a & ~(uintptr_t)0xfff) + mbi.region;     // (the region's size counts from a's page)
    }
#ifndef __aarch64__
    uintptr_t page = (uintptr_t)getpagesize(), from = ((uintptr_t)addr + page - 1) & ~(page - 1), to = ((uintptr_t)addr + size) & ~(page - 1);
    if (to > from) madvise((void *)from, to - from, MADV_FREE);
#endif
    return 0;
}
typedef struct { uint64_t address; size_t bytes; } MEMORY_RANGE_ENTRY;
static WINAPI BOOL PrefetchVirtualMemory(HANDLE p, uintptr_t count, const MEMORY_RANGE_ENTRY *ranges, uint32_t flags) {
    if (flags || (count && !ranges)) return fail(87);
    if (!this_process_only(p)) return 0;
    uintptr_t page = (uintptr_t)getpagesize();
    for (uintptr_t i = 0; i < count; i++) {           // a hint: ranges macOS will not read ahead are left alone
        uintptr_t from = (uintptr_t)VX_LOW(ranges[i].address) & ~(page - 1), to = (uintptr_t)VX_LOW(ranges[i].address) + ranges[i].bytes;
        if (to > from) madvise((void *)from, to - from, MADV_WILLNEED);
    }
    return 1;
}
static WINAPI BOOL VirtualFreeEx(HANDLE p, void *addr, size_t size, DWORD type) {
    if (!this_process_only(p)) return 0;
    BOOL (WINAPI *vfree)(void *, size_t, DWORD) = (BOOL (WINAPI *)(void *, size_t, DWORD))vx_find_export("kernel32.dll", "VirtualFree");
    return vfree(addr, size, type);
}
// PSAPI_WORKING_SET_EX_INFORMATION: an address, then its attributes: Valid (bit 0: in memory), ShareCount (1-3),
// Win32Protection (4-14), Shared (15). Whether a page is in memory comes from mincore.
static WINAPI BOOL K32QueryWorkingSetEx(HANDLE p, void *pv, DWORD cb) {
    if (!this_process_only(p)) return 0;
    if (cb < 16 || !pv) return fail(24);               // ERROR_BAD_LENGTH
    uint64_t *entry = pv;
    uintptr_t page = (uintptr_t)getpagesize();
    for (DWORD i = 0; i < cb / 16; i++, entry += 2) {
        uintptr_t a = (uintptr_t)VX_LOW(entry[0]) & ~(page - 1);
        uint64_t attrs = 0;
        MEMORY_BASIC_INFORMATION mbi;
        char in_core = 0;
        if (query((void *)a, &mbi) && mbi.state == 0x1000) {
            int shared = mbi.type == 0x1000000;        // MEM_IMAGE: the pages of a loaded image can be shared
            if (!mincore((void *)a, page, &in_core) && (in_core & MINCORE_INCORE))
                attrs = 1 | (uint64_t)(shared ? 1 : 0) << 1 | (uint64_t)(mbi.protect & 0x7ff) << 4 | (uint64_t)shared << 15;
            else attrs = (uint64_t)shared << 15;
        }
        entry[1] = attrs;
    }
    return 1;
}
// PERFORMANCE_INFORMATION, in 4 KB pages: commit figures and physical memory as GlobalMemoryStatusEx gives them, the
// file cache and wired (kernel) memory from the Mac's statistics, open files, processes and threads of the system.
typedef struct { DWORD cb; size_t commit_total, commit_limit, commit_peak, physical_total, physical_available, system_cache,
                 kernel_total, kernel_paged, kernel_nonpaged, page_size; DWORD handles, processes, threads; } PERFORMANCE_INFORMATION;
typedef struct { DWORD length, load; uint64_t total_phys, avail_phys, total_page, avail_page, total_virtual, avail_virtual, avail_ext; } MEMORYSTATUSEX;
static WINAPI BOOL K32GetPerformanceInfo(PERFORMANCE_INFORMATION *info, DWORD cb) {
    if (!info || cb < sizeof *info) return fail(24);
    MEMORYSTATUSEX m = { .length = sizeof m };
    BOOL (WINAPI *status)(MEMORYSTATUSEX *) = (BOOL (WINAPI *)(MEMORYSTATUSEX *))vx_find_export("kernel32.dll", "GlobalMemoryStatusEx");
    status(&m);
    const uint64_t page = 4096;
    memset(info, 0, sizeof *info);
    info->cb = sizeof *info;
    info->physical_total = m.total_phys / page;
    info->physical_available = m.avail_phys / page;
    info->commit_limit = m.total_page / page;
    info->commit_total = (m.total_page - m.avail_page) / page;
    static size_t peak;
    if (info->commit_total > __atomic_load_n(&peak, __ATOMIC_RELAXED)) __atomic_store_n(&peak, info->commit_total, __ATOMIC_RELAXED);
    info->commit_peak = __atomic_load_n(&peak, __ATOMIC_RELAXED);
    vm_statistics64_data_t vs; mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vs, &count) == KERN_SUCCESS) {
        // (the counts are in the kernel's pages, 16 KB on Apple silicon also for a process under Rosetta)
        info->system_cache = (size_t)((uint64_t)vs.external_page_count * vm_kernel_page_size / page);
        info->kernel_nonpaged = info->kernel_total = (size_t)((uint64_t)vs.wire_count * vm_kernel_page_size / page);
    }
    info->page_size = page;
    int files = 0; size_t n = sizeof files;
    if (!sysctlbyname("kern.num_files", &files, &n, NULL, 0)) info->handles = (DWORD)files;
    processor_set_name_t set;
    struct processor_set_load_info load; mach_msg_type_number_t load_count = PROCESSOR_SET_LOAD_INFO_COUNT;
    if (processor_set_default(mach_host_self(), &set) == KERN_SUCCESS &&
        processor_set_statistics(set, PROCESSOR_SET_LOAD_INFO, (processor_set_info_t)&load, &load_count) == KERN_SUCCESS) {
        info->processes = (DWORD)load.task_count;
        info->threads = (DWORD)load.thread_count;
    }
    return 1;
}

// ---- threads ---------------------------------------------------------------------------------------------------------
// Threads can be started in this process only: no other process's address space is reachable from here.
static WINAPI HANDLE CreateRemoteThreadEx(HANDLE p, void *sa, size_t stack, void *start, void *param, DWORD flags, void *attributes, DWORD *tid) {
    (void)attributes;                                  // (group affinity and ideal processor: hints macOS decides itself)
    if (!this_process_only(p)) return NULL;
    HANDLE (WINAPI *create)(void *, size_t, void *, void *, DWORD, DWORD *) = (HANDLE (WINAPI *)(void *, size_t, void *, void *, DWORD, DWORD *))vx_find_export("kernel32.dll", "CreateThread");
    return create(sa, stack, start, param, flags, tid);
}
static WINAPI HANDLE CreateRemoteThread(HANDLE p, void *sa, size_t stack, void *start, void *param, DWORD flags, DWORD *tid) {
    return CreateRemoteThreadEx(p, sa, stack, start, param, flags, NULL, tid);
}
// What programs set on threads (by thread id): the priority boost switch (macOS has no such boost), the memory
// priority and power throttling. A thread asking for power throttling (EcoQoS) for itself gets the utility class, the
// one macOS runs on efficiency cores; without it, the class its priority gives (as SetThreadPriority, src/sync.c).
typedef struct { DWORD tid; int boost_disabled; uint32_t memory_priority, throttle_control, throttle_state, dynamic_code; } thread_settings;
static thread_settings *g_thread_settings;
static int g_nthread_settings, g_cthread_settings;
static pthread_mutex_t g_thread_settings_lock = PTHREAD_MUTEX_INITIALIZER;
static const thread_settings g_default_settings = { 0, 0, 5 /* MEMORY_PRIORITY_NORMAL */, 0, 0, 0 };
static const thread_settings *settings_of(DWORD tid) {   // with the lock held (a thread that set nothing: the defaults)
    for (int i = 0; i < g_nthread_settings; i++) if (g_thread_settings[i].tid == tid) return &g_thread_settings[i];
    return &g_default_settings;
}
static thread_settings *settings_for(DWORD tid) {     // with the lock held: to change them
    for (int i = 0; i < g_nthread_settings; i++) if (g_thread_settings[i].tid == tid) return &g_thread_settings[i];
    if (g_nthread_settings == g_cthread_settings) {
        g_cthread_settings = g_cthread_settings ? g_cthread_settings * 2 : 64;
        g_thread_settings = realloc(g_thread_settings, (size_t)g_cthread_settings * sizeof *g_thread_settings);
    }
    thread_settings *s = &g_thread_settings[g_nthread_settings++];
    *s = g_default_settings;
    s->tid = tid;
    return s;
}
static WINAPI BOOL GetThreadPriorityBoost(HANDLE h, BOOL *disabled) {
    DWORD tid = thread_id(h);
    if (!tid) return fail(6);
    if (!disabled) return fail(87);
    pthread_mutex_lock(&g_thread_settings_lock);
    *disabled = settings_of(tid)->boost_disabled;
    pthread_mutex_unlock(&g_thread_settings_lock);
    return 1;
}
static WINAPI BOOL SetThreadPriorityBoost(HANDLE h, BOOL disable) {
    DWORD tid = thread_id(h);
    if (!tid) return fail(6);
    pthread_mutex_lock(&g_thread_settings_lock);
    settings_for(tid)->boost_disabled = disable != 0;
    pthread_mutex_unlock(&g_thread_settings_lock);
    return 1;
}
static qos_class_t qos_for_priority(int p) {
    return p >= 2 ? QOS_CLASS_USER_INTERACTIVE : p >= 0 ? QOS_CLASS_USER_INITIATED : p > -15 ? QOS_CLASS_UTILITY : QOS_CLASS_BACKGROUND;
}
enum { ThreadMemoryPriority, ThreadAbsoluteCpuPriority, ThreadDynamicCodePolicy, ThreadPowerThrottling };
static WINAPI BOOL SetThreadInformation(HANDLE h, int cls, void *info, DWORD size) {
    DWORD tid = thread_id(h);
    if (!tid) return fail(6);
    if (!info) return fail(87);
    const uint32_t *v = info;
    uint32_t throttled = 0;
    pthread_mutex_lock(&g_thread_settings_lock);
    thread_settings *s = settings_for(tid);
    DWORD error = 0;
    switch (cls) {
    case ThreadMemoryPriority:                     // MEMORY_PRIORITY_INFORMATION: 1 (very low) .. 5 (normal)
        if (size != 4) error = 24; else if (v[0] < 1 || v[0] > 5) error = 87; else s->memory_priority = v[0];
        break;
    case ThreadDynamicCodePolicy:                  // THREAD_DYNAMIC_CODE_ALLOW (no code-generation policy here to lift)
        if (size != 4) error = 24; else if (v[0] > 1) error = 87; else s->dynamic_code = v[0];
        break;
    case ThreadPowerThrottling:                    // THREAD_POWER_THROTTLING_STATE: version 1, control and state masks
        if (size != 12) error = 24;
        else if (v[0] != 1 || (v[1] & ~1u) || (v[2] & ~v[1])) error = 87;
        else { s->throttle_control = v[1]; s->throttle_state = v[2]; throttled = v[1] & v[2] & 1; }
        break;
    default: error = 87;                           // (ThreadAbsoluteCpuPriority and later classes: not supported)
    }
    pthread_mutex_unlock(&g_thread_settings_lock);
    if (error) return fail(error);
    if (cls == ThreadPowerThrottling && tid == vx_current_tid()) {
        int (WINAPI *priority)(HANDLE) = (int (WINAPI *)(HANDLE))vx_find_export("kernel32.dll", "GetThreadPriority");
        qos_class_t q = qos_for_priority(priority((HANDLE)(intptr_t)-2));
        if (throttled && q != QOS_CLASS_BACKGROUND) q = QOS_CLASS_UTILITY;
        pthread_set_qos_class_self_np(q, 0);
    }
    return 1;
}
static WINAPI BOOL GetThreadInformation(HANDLE h, int cls, void *info, DWORD size) {
    DWORD tid = thread_id(h);
    if (!tid) return fail(6);
    if (!info) return fail(87);
    uint32_t *v = info;
    DWORD error = 0;
    pthread_mutex_lock(&g_thread_settings_lock);
    const thread_settings *s = settings_of(tid);
    switch (cls) {
    case ThreadMemoryPriority: if (size != 4) error = 24; else v[0] = s->memory_priority; break;
    case ThreadDynamicCodePolicy: if (size != 4) error = 24; else v[0] = s->dynamic_code; break;
    case ThreadPowerThrottling: if (size != 12) error = 24; else { v[0] = 1; v[1] = s->throttle_control; v[2] = s->throttle_state; } break;
    default: error = 87;
    }
    pthread_mutex_unlock(&g_thread_settings_lock);
    return error ? fail(error) : 1;
}
// There is no WOW64 (no 32-bit code runs here): as on a Windows without it, ERROR_INVALID_FUNCTION.
static WINAPI BOOL Wow64GetThreadContext(HANDLE h, void *context) { (void)context; return fail(thread_id(h) ? 1 : 6); }

// ---- processes --------------------------------------------------------------------------------------------------------
// This process's handles: the kernel objects in the handle table and its open descriptors (files, pipes, sockets).
static WINAPI BOOL GetProcessHandleCount(HANDLE p, DWORD *count) {
    if (!this_process_only(p)) return 0;
    if (!count) return fail(87);
    int bytes = proc_pidinfo(getpid(), PROC_PIDLISTFDS, 0, NULL, 0), fds = 0;
    if (bytes > 0) {
        struct proc_fdinfo *list = malloc((size_t)bytes);
        int got = list ? proc_pidinfo(getpid(), PROC_PIDLISTFDS, 0, list, bytes) : 0;
        fds = got > 0 ? got / (int)sizeof *list : 0;
        free(list);
    }
    *count = vx_handle_count() + (DWORD)fds;
    return 1;
}
// Cycles at a nominal 3 GHz of the CPU time used, as QueryThreadCycleTime counts them (src/services.c). Another process's
// CPU time comes in the timebase's ticks (hw.tbfrequency; Rosetta reports a 1:1 timebase to this process).
static WINAPI BOOL QueryProcessCycleTime(HANDLE p, uint64_t *cycles) {
    int pid = process_pid(p);
    if (pid < 0) return fail(6);
    if (!cycles) return fail(87);
    if (pid == (int)getpid()) {
        struct timespec ts; clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
        *cycles = ((uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec) * 3;
        return 1;
    }
    struct rusage_info_v2 ru;
    uint64_t hz = 0; size_t n = sizeof hz;
    if (proc_pid_rusage(pid, RUSAGE_INFO_V2, (rusage_info_t *)&ru) || sysctlbyname("hw.tbfrequency", &hz, &n, NULL, 0) || !hz) return fail(5);
    uint64_t ticks = ru.ri_user_time + ru.ri_system_time;
    *cycles = (uint64_t)((__uint128_t)ticks * 1000000000ull / hz) * 3;
    return 1;
}
// SetProcessInformation: memory priority, power throttling and leap-second handling are hints and are kept for this
// process (no other process's can be changed from here, nor does macOS take them: accepted). The fail-fast-on-commit
// option is not supported (a Windows without it answers the same).
enum { ProcessMemoryPriority, ProcessMemoryExhaustionInfo, ProcessAppMemoryInfo, ProcessInPrivateInfo, ProcessPowerThrottling,
       ProcessLeapSecondInfo = 8 };
static struct { uint32_t memory_priority, throttle_control, throttle_state, leap_flags; } g_process_settings = { 5, 0, 0, 0 };
static WINAPI BOOL SetProcessInformation(HANDLE p, int cls, void *info, DWORD size) {
    int pid = process_pid(p);
    if (pid < 0) return fail(6);
    if (!info) return fail(87);
    const uint32_t *v = info;
    int self = pid == (int)getpid();
    switch (cls) {
    case ProcessMemoryPriority:
        if (size != 4) return fail(24);
        if (v[0] < 1 || v[0] > 5) return fail(87);
        if (self) g_process_settings.memory_priority = v[0];
        return 1;
    case ProcessPowerThrottling:                   // PROCESS_POWER_THROTTLING_STATE: version 1, EXECUTION_SPEED 1, IGNORE_TIMER_RESOLUTION 4
        if (size != 12) return fail(24);
        if (v[0] != 1 || (v[1] & ~5u) || (v[2] & ~v[1])) return fail(87);
        if (self) { g_process_settings.throttle_control = v[1]; g_process_settings.throttle_state = v[2]; }
        return 1;
    case ProcessLeapSecondInfo:                    // PROCESS_LEAP_SECOND_INFO: flags (1: sixty-second minutes), reserved
        if (size != 8) return fail(24);
        if ((v[0] & ~1u) || v[1]) return fail(87);
        if (self) g_process_settings.leap_flags = v[0];   // (the Mac's clock has no leap seconds: nothing changes)
        return 1;
    }
    return fail(87);
}
// Apple silicon starts from its own boot firmware, neither BIOS nor UEFI.
static WINAPI BOOL GetFirmwareType(int *type) {
    if (!type) return fail(87);
    *type = 0;                                         // FirmwareTypeUnknown
    return 1;
}
// The console is the terminal vxrt runs in (when there is one), always a window.
static WINAPI BOOL GetConsoleDisplayMode(DWORD *flags) {
    if (!flags) return fail(87);
    if (!isatty(0) && !isatty(1) && !isatty(2)) return fail(6);
    *flags = 0;
    return 1;
}
// Windows Error Reporting: there is no WER service here to load a registered handler after a crash (as with WER turned
// off). Registrations are kept, up to WER_MAX_REGISTERED_RUNTIME_EXCEPTION_MODULES, so that unregistering answers right.
static struct { WCHAR *dll; void *context; } g_wer[16];
static pthread_mutex_t g_wer_lock = PTHREAD_MUTEX_INITIALIZER;
static int same_wide(const WCHAR *a, const WCHAR *b) {      // ignoring ASCII case
    for (; *a && *b; a++, b++) if ((*a >= 'A' && *a <= 'Z' ? *a + 32 : *a) != (*b >= 'A' && *b <= 'Z' ? *b + 32 : *b)) return 0;
    return *a == *b;
}
static WINAPI LONG WerRegisterRuntimeExceptionModule(const WCHAR *dll, void *context) {
    if (!dll || !*dll) return (LONG)0x80070057;        // E_INVALIDARG
    pthread_mutex_lock(&g_wer_lock);
    int slot = -1;
    for (int i = 0; i < 16 && slot < 0; i++) if (!g_wer[i].dll) slot = i;
    if (slot >= 0) {
        size_t n = vx_wcslen(dll);
        g_wer[slot].dll = malloc((n + 1) * sizeof(WCHAR));
        memcpy(g_wer[slot].dll, dll, (n + 1) * sizeof(WCHAR));
        g_wer[slot].context = context;
    }
    pthread_mutex_unlock(&g_wer_lock);
    return slot >= 0 ? 0 : (LONG)0x8007007a;          // HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER): the limit
}
static WINAPI LONG WerUnregisterRuntimeExceptionModule(const WCHAR *dll, void *context) {
    if (!dll || !*dll) return (LONG)0x80070057;
    LONG r = (LONG)0x80070490;                        // HRESULT_FROM_WIN32(ERROR_NOT_FOUND)
    pthread_mutex_lock(&g_wer_lock);
    for (int i = 0; i < 16; i++)
        if (g_wer[i].dll && g_wer[i].context == context && same_wide(g_wer[i].dll, dll)) { free(g_wer[i].dll); g_wer[i].dll = NULL; r = 0; break; }
    pthread_mutex_unlock(&g_wer_lock);
    return r;
}

// ---- locales ----------------------------------------------------------------------------------------------------------
// The runtime's locales (src/desktop.c) are specific locales Windows ships: LOCALE_WINDOWS | LOCALE_SPECIFICDATA. They are
// all there is, so asking for neutral, supplemental or alternate-sort locales alone enumerates none.
static WINAPI BOOL EnumSystemLocalesEx(BOOL (WINAPI *fn)(WCHAR *, DWORD, intptr_t), DWORD flags, intptr_t param, void *reserved) {
    if (!fn || reserved) return fail(87);
    if (flags & ~0x37u) return fail(1004);             // ERROR_INVALID_FLAGS (LOCALE_REPLACEMENT is not one to ask for)
    if (flags && !(flags & 0x21)) return 1;
    for (int i = 0; vx_locale_at(i); i++) {
        WCHAR name[16];
        size_t n = vx_utf8_to_utf16(vx_locale_at(i)->name, strlen(vx_locale_at(i)->name), name, 15);
        name[n] = 0;
        if (!fn(name, 0x21, param)) break;
    }
    return 1;
}
static WINAPI BOOL IsValidLocaleName(const WCHAR *name) {
    if (!name) return 0;
    char *a = vx_utf8_dup(name);
    BOOL ok = vx_locale_by_name(a) != NULL;
    free(a);
    return ok;
}

// ---- volumes and DOS device names ---------------------------------------------------------------------------------------
// The two drives (GetLogicalDrives): C: (the root's drive_c) and Z: (the Mac's root), each a volume with a GUID path and
// an NT device. C: keeps the GUID GetVolumeNameForVolumeMountPointA has given since Elite Dangerous (src/extras.c).
static const struct { char letter; const char *guid, *device; } g_volumes[] = {
    { 'C', "{3f4e8c2a-9b1d-4c6e-8a7f-1d2e3f4a5b6c}", "\\Device\\HarddiskVolume1" },
    { 'Z', "{3f4e8c2a-9b1d-4c6e-8a7f-1d2e3f4a5b6d}", "\\Device\\HarddiskVolume2" },
};
#define NVOLUMES (int)(sizeof g_volumes / sizeof g_volumes[0])
static const char *drive_device(char letter) {        // "\Device\HarddiskVolumeN" of a drive letter, NULL if unmapped
    for (int i = 0; i < NVOLUMES; i++) if (g_volumes[i].letter == toupper((unsigned char)letter)) return g_volumes[i].device;
    return NULL;
}
static int ascii_out(const char *s, WCHAR *out, DWORD cap) {   // a volume GUID path with the terminator, or 0 if it does not fit
    size_t n = strlen(s);
    if (!out || cap <= n) return 0;
    for (size_t i = 0; i <= n; i++) out[i] = (WCHAR)(unsigned char)s[i];
    return 1;
}
static int volume_path(int i, WCHAR *out, DWORD cap) { char s[64]; snprintf(s, sizeof s, "\\\\?\\Volume%s\\", g_volumes[i].guid); return ascii_out(s, out, cap); }
static WINAPI HANDLE FindFirstVolumeW(WCHAR *name, DWORD cap) {
    if (!volume_path(0, name, cap)) { vx_set_last_error(206); return (HANDLE)(intptr_t)-1; }   // ERROR_FILENAME_EXCED_RANGE
    int *next = malloc(sizeof *next);
    *next = 1;
    HANDLE h = vx_handle_new(VX_HANDLE_VOLUME_SEARCH, next, free);
    if (!h) { free(next); vx_set_last_error(4); return (HANDLE)(intptr_t)-1; }
    return h;
}
static WINAPI BOOL FindNextVolumeW(HANDLE h, WCHAR *name, DWORD cap) {
    int *next = vx_handle_data(h, VX_HANDLE_VOLUME_SEARCH);
    if (!next) return fail(6);
    if (*next >= NVOLUMES) return fail(18);           // ERROR_NO_MORE_FILES
    if (!volume_path(*next, name, cap)) return fail(206);
    ++*next;
    return 1;
}
static WINAPI BOOL FindVolumeClose(HANDLE h) {
    if (!vx_handle_data(h, VX_HANDLE_VOLUME_SEARCH)) return fail(6);
    return vx_handle_close(h);
}
// DOS device names: the drives and their volumes' GUID names, each a link to the volume's device.
static int dos_device(const char *name, char *target, size_t cap) {
    for (int i = 0; i < NVOLUMES; i++) {
        char volume[64];
        snprintf(volume, sizeof volume, "Volume%s", g_volumes[i].guid);
        if ((toupper((unsigned char)name[0]) == g_volumes[i].letter && name[1] == ':' && !name[2]) || !strcasecmp(name, volume)) {
            snprintf(target, cap, "%s", g_volumes[i].device);
            return 1;
        }
    }
    return 0;
}
// The result as a list of strings, each with its terminator, then one more; the count includes them all.
static DWORD dos_devices(const char *name, char *list, size_t cap) {
    size_t n = 0;
    if (name) {
        if (!dos_device(name, list, cap - 1)) { vx_set_last_error(2); return 0; }   // ERROR_FILE_NOT_FOUND
        n = strlen(list) + 1;
    } else {
        for (int i = 0; i < NVOLUMES; i++) n += (size_t)snprintf(list + n, cap - n, "%c:", g_volumes[i].letter) + 1;
        for (int i = 0; i < NVOLUMES; i++) n += (size_t)snprintf(list + n, cap - n, "Volume%s", g_volumes[i].guid) + 1;
    }
    list[n++] = 0;
    return (DWORD)n;
}
static WINAPI DWORD QueryDosDeviceW(const WCHAR *name, WCHAR *out, DWORD cap) {
    char list[512], *a = name ? vx_utf8_dup(name) : NULL;
    DWORD n = dos_devices(a, list, sizeof list);
    free(a);
    if (!n) return 0;
    if (!out || cap < n) { vx_set_last_error(122); return 0; }   // ERROR_INSUFFICIENT_BUFFER
    for (DWORD i = 0; i < n; i++) out[i] = (WCHAR)(unsigned char)list[i];
    return n;
}
static WINAPI DWORD QueryDosDeviceA(const char *name, char *out, DWORD cap) {
    char list[512];
    DWORD n = dos_devices(name, list, sizeof list);
    if (!n) return 0;
    if (!out || cap < n) { vx_set_last_error(122); return 0; }
    memcpy(out, list, n);
    return n;
}
// A volume's mount points: its drive's root ("C:\", then the list's end).
static WINAPI BOOL GetVolumePathNamesForVolumeNameW(const WCHAR *volume, WCHAR *out, DWORD cap, DWORD *need) {
    if (!volume) return fail(87);
    for (int i = 0; i < NVOLUMES; i++) {
        WCHAR path[64];
        volume_path(i, path, 64);
        if (!same_wide(path, volume)) continue;
        if (need) *need = 5;
        if (!out || cap < 5) return fail(234);        // ERROR_MORE_DATA
        out[0] = (WCHAR)g_volumes[i].letter; out[1] = ':'; out[2] = '\\'; out[3] = 0; out[4] = 0;
        return 1;
    }
    size_t n = vx_wcslen(volume);
    return fail(n && volume[n - 1] == '\\' ? 2 : 123); // unknown volume / not a volume GUID path (ERROR_INVALID_NAME)
}

// ---- DLL directories --------------------------------------------------------------------------------------------------
// SetDefaultDllDirectories' flags and AddDllDirectory's directories, which the loader's search uses (src/loader.c):
// with search flags in force only the application directory, the user directories and system32 are searched, as each
// flag says, never the current directory.
static DWORD g_default_search;
typedef struct dll_directory { struct dll_directory *next; char unix_path[]; } dll_directory;
static dll_directory *g_dll_directories;
static pthread_mutex_t g_dll_directories_lock = PTHREAD_MUTEX_INITIALIZER;
DWORD vx_dll_default_search(void) { return __atomic_load_n(&g_default_search, __ATOMIC_ACQUIRE); }
int vx_dll_user_directory(int i, char *out, size_t cap) {
    pthread_mutex_lock(&g_dll_directories_lock);
    dll_directory *d = g_dll_directories;
    while (d && i-- > 0) d = d->next;
    if (d) snprintf(out, cap, "%s", d->unix_path);
    pthread_mutex_unlock(&g_dll_directories_lock);
    return d != NULL;
}
static WINAPI BOOL SetDefaultDllDirectories(DWORD flags) {
    // LOAD_LIBRARY_SEARCH_APPLICATION_DIR 0x200, _USER_DIRS 0x400, _SYSTEM32 0x800, _DEFAULT_DIRS 0x1000
    if (!flags || (flags & ~0x1e00u)) return fail(87);
    __atomic_store_n(&g_default_search, flags, __ATOMIC_RELEASE);
    return 1;
}
static WINAPI void *AddDllDirectory(const WCHAR *directory) {
    if (!directory) { vx_set_last_error(87); return NULL; }
    char *a = vx_utf8_dup(directory), full[4096], unix_path[4096];
    int absolute = a[0] == '\\' || a[0] == '/' || (isalpha((unsigned char)a[0]) && a[1] == ':' && (a[2] == '\\' || a[2] == '/'));
    int ok = absolute && vx_full_path(a, full, sizeof full) && vx_unix_path(full, unix_path, sizeof unix_path);
    free(a);
    if (!ok) { vx_set_last_error(87); return NULL; }
    size_t n = strlen(unix_path);
    while (n > 1 && unix_path[n - 1] == '/') unix_path[--n] = 0;
    dll_directory *d = malloc(sizeof *d + n + 1);
    memcpy(d->unix_path, unix_path, n + 1);
    pthread_mutex_lock(&g_dll_directories_lock);
    d->next = g_dll_directories;
    g_dll_directories = d;
    pthread_mutex_unlock(&g_dll_directories_lock);
    return d;                                          // the cookie
}
static WINAPI BOOL RemoveDllDirectory(void *cookie) {
    pthread_mutex_lock(&g_dll_directories_lock);
    dll_directory **p = &g_dll_directories;
    while (*p && *p != cookie) p = &(*p)->next;
    dll_directory *d = *p;
    if (d) *p = d->next;
    pthread_mutex_unlock(&g_dll_directories_lock);
    if (!d) return fail(87);
    free(d);
    return 1;
}

// ---- time zones -----------------------------------------------------------------------------------------------------------
// A TIME_ZONE_INFORMATION's rules: UTC = local time + Bias + StandardBias, or + DaylightBias between DaylightDate (given in
// standard time) and StandardDate (given in daylight time). A transition date is a weekday's occurrence in a month (wYear
// 0: wDay 1-5, 5 the last) or, with a year, one date. No zone (NULL) or the zone GetTimeZoneInformation describes (which
// carries no transition dates) means the Mac's own zone and its rules.
typedef struct { uint16_t year, month, weekday, day, hour, minute, second, ms; } SYSTEMTIME;
typedef struct { LONG bias; WCHAR std_name[32]; SYSTEMTIME std_date; LONG std_bias; WCHAR dst_name[32]; SYSTEMTIME dst_date; LONG dst_bias; } TIME_ZONE_INFORMATION;
static int days_in_month(int year, int month) {
    static const int days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return month == 2 && ((year % 4 == 0 && year % 100) || year % 400 == 0) ? 29 : days[month - 1];
}
static int valid_time(const SYSTEMTIME *st) {
    return st && st->year >= 1601 && st->year <= 30827 && st->month >= 1 && st->month <= 12 && st->day >= 1 &&
           st->day <= days_in_month(st->year, st->month) && st->hour < 24 && st->minute < 60 && st->second < 60 && st->ms < 1000;
}
static int64_t seconds_of(int year, int month, int day, int hour, int minute, int second) {   // as if UTC
    struct tm tm = { .tm_year = year - 1900, .tm_mon = month - 1, .tm_mday = day, .tm_hour = hour, .tm_min = minute, .tm_sec = second };
    return (int64_t)timegm(&tm);
}
static void time_of(int64_t secs, uint16_t ms, SYSTEMTIME *st) {
    time_t t = (time_t)secs; struct tm tm; gmtime_r(&t, &tm);
    *st = (SYSTEMTIME){ (uint16_t)(tm.tm_year + 1900), (uint16_t)(tm.tm_mon + 1), (uint16_t)tm.tm_wday, (uint16_t)tm.tm_mday,
                        (uint16_t)tm.tm_hour, (uint16_t)tm.tm_min, (uint16_t)tm.tm_sec, ms };
}
static int64_t transition(const SYSTEMTIME *rule, int year) {   // the local wall time of a transition in that year
    if (rule->year) return seconds_of(rule->year, rule->month, rule->day, rule->hour, rule->minute, rule->second);
    int64_t first = seconds_of(year, rule->month, 1, 0, 0, 0);
    time_t t = (time_t)first; struct tm tm; gmtime_r(&t, &tm);
    int day = 1 + (rule->weekday - tm.tm_wday + 7) % 7 + (rule->day - 1) * 7;
    while (day > days_in_month(year, rule->month)) day -= 7;
    return seconds_of(year, rule->month, day, rule->hour, rule->minute, rule->second);
}
static int has_rules(const TIME_ZONE_INFORMATION *tz) { return tz->std_date.month && tz->dst_date.month; }
static int in_range(int64_t t, int64_t start, int64_t end) {    // daylight time from start to end (either hemisphere)
    return start < end ? t >= start && t < end : !(t >= end && t < start);
}
static int mac_zone(const TIME_ZONE_INFORMATION *tz) {
    if (!tz) return 1;
    TIME_ZONE_INFORMATION mine;
    DWORD (WINAPI *get)(TIME_ZONE_INFORMATION *) = (DWORD (WINAPI *)(TIME_ZONE_INFORMATION *))vx_find_export("kernel32.dll", "GetTimeZoneInformation");
    return get && get(&mine) != 0xffffffffu && !memcmp(&mine, tz, sizeof mine);
}
static WINAPI BOOL TzSpecificLocalTimeToSystemTime(const TIME_ZONE_INFORMATION *tz, const SYSTEMTIME *local, SYSTEMTIME *utc) {
    if (!valid_time(local) || !utc) return fail(87);
    int64_t secs;
    if (mac_zone(tz)) {
        struct tm tm = { .tm_year = local->year - 1900, .tm_mon = local->month - 1, .tm_mday = local->day, .tm_hour = local->hour,
                         .tm_min = local->minute, .tm_sec = local->second, .tm_isdst = -1 };
        secs = (int64_t)mktime(&tm);
    } else {
        int64_t t = seconds_of(local->year, local->month, local->day, local->hour, local->minute, local->second);
        int dst = has_rules(tz) && in_range(t, transition(&tz->dst_date, local->year), transition(&tz->std_date, local->year));
        secs = t + 60 * (int64_t)(tz->bias + (dst ? tz->dst_bias : tz->std_bias));
    }
    time_of(secs, local->ms, utc);
    return 1;
}
static WINAPI BOOL SystemTimeToTzSpecificLocalTime(const TIME_ZONE_INFORMATION *tz, const SYSTEMTIME *utc, SYSTEMTIME *local) {
    if (!valid_time(utc) || !local) return fail(87);
    int64_t t = seconds_of(utc->year, utc->month, utc->day, utc->hour, utc->minute, utc->second), secs;
    if (mac_zone(tz)) {
        time_t tt = (time_t)t; struct tm tm; localtime_r(&tt, &tm);
        secs = t + tm.tm_gmtoff;
    } else {
        int64_t standard = t - 60 * (int64_t)(tz->bias + tz->std_bias);
        SYSTEMTIME s; time_of(standard, 0, &s);
        int dst = 0;
        if (has_rules(tz)) {                                    // the transitions in UTC
            int64_t start = transition(&tz->dst_date, s.year) + 60 * (int64_t)(tz->bias + tz->std_bias);
            int64_t end = transition(&tz->std_date, s.year) + 60 * (int64_t)(tz->bias + tz->dst_bias);
            dst = in_range(t, start, end);
        }
        secs = t - 60 * (int64_t)(tz->bias + (dst ? tz->dst_bias : tz->std_bias));
    }
    time_of(secs, utc->ms, local);
    return 1;
}

// ---- ntdll: NTSTATUS, objects, writes ----------------------------------------------------------------------------------------
// RtlNtStatusToDosError keeps the status it converted as the thread's last status (Windows keeps it in the TEB);
// RtlGetLastNtStatus gives it back while the last error is still the one it maps to, else the status the last error
// usually comes from (src/ntstatus.c), so that a failed call reports the same failure both ways.
static __thread uint32_t t_last_status;
static WINAPI DWORD RtlNtStatusToDosError(uint32_t status) { t_last_status = status; return vx_ntstatus_to_win32(status); }
static WINAPI DWORD RtlNtStatusToDosErrorNoTeb(uint32_t status) { return vx_ntstatus_to_win32(status); }
static WINAPI uint32_t RtlGetLastNtStatus(void) {
    DWORD error = vx_get_last_error();
    if (vx_ntstatus_to_win32(t_last_status) == error) return t_last_status;
    return vx_win32_to_ntstatus(error);
}

#define STATUS_INVALID_INFO_CLASS   0xc0000003u
#define STATUS_INFO_LENGTH_MISMATCH 0xc0000004u
#define STATUS_INVALID_HANDLE       0xc0000008u
#define STATUS_INVALID_PARAMETER    0xc000000du
#define STATUS_NOT_SUPPORTED        0xc00000bbu
// What NtQueryObject tells about a handle: the type's name, the object's name in the NT namespace, its access and handles.
typedef struct { const char *type; char name[1100]; DWORD access, handles; } object_facts;
static int describe(HANDLE h, object_facts *f) {
    memset(f, 0, sizeof *f);
    intptr_t v = (intptr_t)h;
    if (v == -1) { f->type = "Process"; f->access = 0x1fffff; f->handles = 1; return 1; }
    int fd = vx_handle_fd(h);
    if (fd >= 0) {                                    // a file: \Device\HarddiskVolumeN\path (QueryDosDevice's devices)
        f->type = "File"; f->access = 0x1f01ff; f->handles = 1;
        char unix_path[1024], win[1100];
        if (fd > 2 && !fcntl(fd, F_GETPATH, unix_path)) {
            vx_windows_path(unix_path, win, sizeof win);
            const char *device = drive_device(win[0]);
            if (device && win[1] == ':') snprintf(f->name, sizeof f->name, "%s%s", device, win + 2);
        }
        return 1;
    }
    const char *type = NULL; int kind = 0; char name[512];
    if (!vx_handle_object(h, NULL, &type, &kind, name, sizeof name, &f->handles)) return 0;
    static const struct { const char *type; DWORD access; } all_access[] = {
        { "Thread", 0x1fffff }, { "Process", 0x1fffff }, { "Event", 0x1f0003 }, { "Mutant", 0x1f0001 },
        { "Semaphore", 0x1f0003 }, { "IoCompletion", 0x1f0003 }, { "File", 0x1f01ff }, { "Section", 0xf001f }, { "Job", 0x1f003f },
    };
    if (!type) type = kind == 1 || kind == 7 || kind == VX_HANDLE_VOLUME_SEARCH ? "File"       // find handles, pipes: files
                    : kind == 2 ? "Section" : kind == VX_HANDLE_JOB ? "Job"
                    : "Unknown";                      // another module's object this file does not know: no Windows type
    f->type = type;
    for (size_t i = 0; i < sizeof all_access / sizeof all_access[0]; i++) if (!strcmp(all_access[i].type, type)) f->access = all_access[i].access;
    if (name[0]) {                                     // named objects live in the session's namespace unless Global\ (sic)
        const char *rest = name;
        int global = !strncasecmp(name, "Global\\", 7);
        if (global || !strncasecmp(name, "Local\\", 6)) rest = strchr(name, '\\') + 1;
        snprintf(f->name, sizeof f->name, "%s\\BaseNamedObjects\\%s", global ? "" : "\\Sessions\\1", rest);
    }
    return 1;
}
typedef struct { uint16_t length, max; uint32_t pad; uint64_t buffer; } UNICODE_STRING;
static void unicode_at(UNICODE_STRING *u, WCHAR *w, const char *text) {   // w: room in the same buffer, after the structure
    size_t n = vx_utf8_to_utf16(text, strlen(text), w, 0x7ffe);
    w[n] = 0;
    u->length = (uint16_t)(n * 2); u->max = (uint16_t)(n * 2 + 2); u->buffer = TO_PROGRAM(w);
}
static WINAPI uint32_t NtQueryObject(HANDLE h, int cls, void *info, uint32_t len, uint32_t *ret) {
    object_facts f;
    if (cls < 0 || cls > 4 || cls == 3) return STATUS_INVALID_INFO_CLASS;   // (3, every type of the system: not kept)
    if (!describe(h, &f)) return STATUS_INVALID_HANDLE;
    uint32_t need;
    size_t chars = vx_utf8_to_utf16(cls == 1 ? f.name : f.type, strlen(cls == 1 ? f.name : f.type), NULL, 0);
    switch (cls) {
    case 0: need = 56; break;                          // PUBLIC_OBJECT_BASIC_INFORMATION
    case 1: need = 16 + (f.name[0] ? (uint32_t)(chars + 1) * 2 : 0); break;   // OBJECT_NAME_INFORMATION (unnamed: empty)
    case 2: need = 104 + (uint32_t)(chars + 1) * 2; break;                    // OBJECT_TYPE_INFORMATION
    default: need = 2; break;                          // OBJECT_HANDLE_FLAG_INFORMATION: inherit, protect from close
    }
    if (ret) *ret = need;
    if (!info || len < need) return STATUS_INFO_LENGTH_MISMATCH;
    memset(info, 0, need);
    uint32_t *words = info;
    switch (cls) {
    case 0: words[1] = f.access; words[2] = f.handles; words[3] = f.handles + 1; break;   // attributes 0, granted access, handles, references
    case 1: if (f.name[0]) unicode_at(info, (WCHAR *)((uint8_t *)info + 16), f.name); break;
    case 2:                                            // the name, the type's objects and handles (this one's), its valid access
        unicode_at(info, (WCHAR *)((uint8_t *)info + 104), f.type);
        words[4] = 1; words[5] = f.handles; words[21] = f.access;
        break;
    }
    return 0;
}

// NtWriteFile: synchronous, at the handle's position, at an offset (which then becomes the position, as for a
// synchronous handle) or at the end (FILE_WRITE_TO_END_OF_FILE). Completion routines (APCs) are not supported.
static WINAPI uint32_t NtWriteFile(HANDLE h, HANDLE event, void *apc, void *apc_context, uint64_t *iosb, const void *buf, uint32_t len,
                                   const int64_t *offset, uint32_t *key) {
    (void)apc_context; (void)key;
    if (apc) return STATUS_NOT_SUPPORTED;
    if (!iosb || (len && !buf)) return STATUS_INVALID_PARAMETER;
    int fd = vx_handle_fd(h);
    uint32_t status = 0;
    uint64_t done = 0;
    if (fd > 2 && offset && *offset != -2 /* FILE_USE_FILE_POINTER_POSITION */) {
        ssize_t r;
        if (*offset == -1) { lseek(fd, 0, SEEK_END); r = write(fd, buf, len); }   // FILE_WRITE_TO_END_OF_FILE
        else if (*offset < 0) return STATUS_INVALID_PARAMETER;
        else if ((r = pwrite(fd, buf, len, (off_t)*offset)) >= 0) lseek(fd, (off_t)(*offset + r), SEEK_SET);
        if (r < 0) status = vx_win32_to_ntstatus(vx_error_from_errno(errno));
        else done = (uint64_t)r;
    } else {
        BOOL (WINAPI *write_file)(HANDLE, const void *, DWORD, DWORD *, void *) = (BOOL (WINAPI *)(HANDLE, const void *, DWORD, DWORD *, void *))vx_find_export("kernel32.dll", "WriteFile");
        DWORD saved = vx_get_last_error(), written = 0;
        if (write_file(h, buf, len, &written, NULL)) done = written;
        else status = vx_win32_to_ntstatus(vx_get_last_error());
        vx_set_last_error(saved);                      // (a native call leaves the last error alone)
    }
    iosb[0] = status; iosb[1] = done;
    if (event && !status) {
        BOOL (WINAPI *set)(HANDLE) = (BOOL (WINAPI *)(HANDLE))vx_find_export("kernel32.dll", "SetEvent");
        set(event);
    }
    return status;
}

// ---- tables ---------------------------------------------------------------------------------------------------------------
static const vx_export kernelx_k32[] = {
    VX_EXPORT(ClearCommError), VX_EXPORT(EscapeCommFunction), VX_EXPORT(GetCommModemStatus), VX_EXPORT(GetCommState),
    VX_EXPORT(SetCommState), VX_EXPORT(SetCommTimeouts), VX_EXPORT(PurgeComm), VX_EXPORT(CreateMemoryResourceNotification),
    VX_EXPORT(QueryMemoryResourceNotification), VX_EXPORT(DiscardVirtualMemory), VX_EXPORT(PrefetchVirtualMemory),
    VX_EXPORT(VirtualFreeEx), VX_EXPORT(K32QueryWorkingSetEx), VX_EXPORT(K32GetPerformanceInfo), VX_EXPORT(CreateRemoteThread),
    VX_EXPORT(CreateRemoteThreadEx), VX_EXPORT(GetThreadPriorityBoost), VX_EXPORT(SetThreadPriorityBoost),
    VX_EXPORT(SetThreadInformation), VX_EXPORT(GetThreadInformation), VX_EXPORT(Wow64GetThreadContext),
    VX_EXPORT(GetProcessHandleCount), VX_EXPORT(QueryProcessCycleTime), VX_EXPORT(SetProcessInformation),
    VX_EXPORT(GetFirmwareType), VX_EXPORT(GetConsoleDisplayMode), VX_EXPORT(WerRegisterRuntimeExceptionModule),
    VX_EXPORT(WerUnregisterRuntimeExceptionModule), VX_EXPORT(EnumSystemLocalesEx), VX_EXPORT(IsValidLocaleName),
    VX_EXPORT(FindFirstVolumeW), VX_EXPORT(FindNextVolumeW), VX_EXPORT(FindVolumeClose), VX_EXPORT(QueryDosDeviceW),
    VX_EXPORT(QueryDosDeviceA), VX_EXPORT(GetVolumePathNamesForVolumeNameW), VX_EXPORT(SetDefaultDllDirectories),
    VX_EXPORT(AddDllDirectory), VX_EXPORT(RemoveDllDirectory), VX_EXPORT(TzSpecificLocalTimeToSystemTime),
    VX_EXPORT(SystemTimeToTzSpecificLocalTime),
};
static const vx_export kernelx_psapi[] = {
    { "QueryWorkingSetEx", (void *)K32QueryWorkingSetEx }, { "GetPerformanceInfo", (void *)K32GetPerformanceInfo },
};
static const vx_export kernelx_ntdll[] = {
    VX_EXPORT(NtQueryObject), { "ZwQueryObject", (void *)NtQueryObject }, VX_EXPORT(NtWriteFile), { "ZwWriteFile", (void *)NtWriteFile },
    VX_EXPORT(RtlGetLastNtStatus), VX_EXPORT(RtlNtStatusToDosError), VX_EXPORT(RtlNtStatusToDosErrorNoTeb),
};
void vx_kernelx_init(void) {
    vx_register_module("kernel32.dll", kernelx_k32, sizeof kernelx_k32 / sizeof kernelx_k32[0]);
    vx_register_module("kernelbase.dll", kernelx_k32, sizeof kernelx_k32 / sizeof kernelx_k32[0]);
    vx_register_module("psapi.dll", kernelx_psapi, sizeof kernelx_psapi / sizeof kernelx_psapi[0]);
    vx_register_module("ntdll.dll", kernelx_ntdll, sizeof kernelx_ntdll / sizeof kernelx_ntdll[0]);
    vx_jobs_init();
}

#ifdef __aarch64__
#include "thunks/kernelx.c.inc"
#endif
