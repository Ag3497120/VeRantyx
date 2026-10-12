// kernel32: the Windows core API, implemented directly on macOS.
#include <mach/mach_time.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>
#include <malloc/malloc.h>
#include "vx.h"

#define STD_INPUT_HANDLE  ((DWORD)-10)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define STD_ERROR_HANDLE  ((DWORD)-12)

// ---- handles ----------------------------------------------------------------------------------------------------
int vx_handle_fd(HANDLE h) {
    intptr_t v = (intptr_t)h;
    if (v == -10) return 0;
    if (v == -11) return 1;
    if (v == -12) return 2;
    if (v >= 0x10000 && v < 0x10000 + 1024) return (int)(v - 0x10000);   // file handles: 0x10000 + fd
    return -1;
}

static WINAPI HANDLE GetStdHandle(DWORD which) {
    if (which == STD_INPUT_HANDLE || which == STD_OUTPUT_HANDLE || which == STD_ERROR_HANDLE)
        return (HANDLE)(intptr_t)(int32_t)which;
    return (HANDLE)(intptr_t)-1;
}

static WINAPI BOOL WriteFile(HANDLE h, const void *buf, DWORD n, DWORD *written, void *overlapped) {
    (void)overlapped;
    int fd = vx_handle_fd(h);
    if (fd < 0) { vx_set_last_error(6 /* ERROR_INVALID_HANDLE */); return 0; }
    ssize_t r = write(fd, buf, n);
    if (written) *written = r > 0 ? (DWORD)r : 0;
    return r >= 0;
}

static WINAPI BOOL WriteConsoleA(HANDLE h, const void *buf, DWORD n, DWORD *written, void *reserved) {
    return WriteFile(h, buf, n, written, reserved);
}

static WINAPI BOOL WriteConsoleW(HANDLE h, const WCHAR *buf, DWORD n, DWORD *written, void *reserved) {
    (void)reserved;
    char *utf8 = malloc((size_t)n * 3 + 1); size_t k = 0;
    for (DWORD i = 0; i < n; i++) {
        uint32_t c = buf[i];
        if (c < 0x80) utf8[k++] = (char)c;
        else if (c < 0x800) { utf8[k++] = 0xc0 | (c >> 6); utf8[k++] = 0x80 | (c & 0x3f); }
        else { utf8[k++] = 0xe0 | (c >> 12); utf8[k++] = 0x80 | ((c >> 6) & 0x3f); utf8[k++] = 0x80 | (c & 0x3f); }
    }
    DWORD w = 0; BOOL ok = WriteFile(h, utf8, (DWORD)k, &w, NULL);
    if (written) *written = ok ? n : 0;
    free(utf8);
    return ok;
}

static WINAPI void ExitProcess(DWORD code) { fflush(stdout); fflush(stderr); _exit((int)code); }
static WINAPI BOOL TerminateProcess(HANDLE h, DWORD code) { (void)h; ExitProcess(code); return 1; }

// ---- errors, debugging, process info -------------------------------------------------------------------------------
static WINAPI DWORD GetLastError(void) { return vx_get_last_error(); }
static WINAPI void SetLastError(DWORD e) { vx_set_last_error(e); }
static void *g_unhandled_filter;
static WINAPI void *SetUnhandledExceptionFilter(void *f) { void *old = g_unhandled_filter; g_unhandled_filter = f; return old; }
static WINAPI BOOL IsDebuggerPresent(void) { return 0; }
static WINAPI DWORD GetCurrentProcessId(void) { return (DWORD)getpid(); }
static WINAPI DWORD GetCurrentThreadId(void) { uint64_t t; pthread_threadid_np(NULL, &t); return (DWORD)t; }
static WINAPI HANDLE GetCurrentProcess(void) { return (HANDLE)(intptr_t)-1; }
static WINAPI HANDLE GetCurrentThread(void) { return (HANDLE)(intptr_t)-2; }
static WINAPI char *GetCommandLineA(void) { return vx_proc.command_line; }
static WINAPI WCHAR *GetCommandLineW(void) { return vx_proc.command_line_w; }

typedef struct { DWORD cb; void *reserved, *desktop, *title; DWORD x, y, xs, ys, xc, yc, fill, flags; uint16_t show, cb2; void *r2; HANDLE in, out, err; } STARTUPINFO;
static WINAPI void GetStartupInfoA(STARTUPINFO *si) {
    memset(si, 0, sizeof *si); si->cb = sizeof *si;
    si->in = (HANDLE)(intptr_t)-10; si->out = (HANDLE)(intptr_t)-11; si->err = (HANDLE)(intptr_t)-12;
}
static WINAPI void GetStartupInfoW(STARTUPINFO *si) { GetStartupInfoA(si); }

// ---- time -----------------------------------------------------------------------------------------------------------
static WINAPI void Sleep(DWORD ms) { if (ms) usleep((useconds_t)ms * 1000); else sched_yield(); }
static WINAPI BOOL QueryPerformanceFrequency(int64_t *f) { *f = 1000000000; return 1; }
static WINAPI BOOL QueryPerformanceCounter(int64_t *c) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    *c = (int64_t)(mach_absolute_time() * tb.numer / tb.denom);
    return 1;
}
static WINAPI uint64_t GetTickCount64(void) { int64_t c; QueryPerformanceCounter(&c); return (uint64_t)c / 1000000; }
static WINAPI DWORD GetTickCount(void) { return (DWORD)GetTickCount64(); }
static WINAPI void GetSystemTimeAsFileTime(uint64_t *ft) {
    struct timeval tv; gettimeofday(&tv, NULL);
    *ft = ((uint64_t)tv.tv_sec + 11644473600ull) * 10000000ull + (uint64_t)tv.tv_usec * 10;   // 100 ns since 1601
}
static WINAPI void GetSystemTimePreciseAsFileTime(uint64_t *ft) { GetSystemTimeAsFileTime(ft); }

// ---- critical sections and SRW locks ----------------------------------------------------------------------------------
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

// ---- TLS slots (TEB + 0x1480, 64 of them) --------------------------------------------------------------------------------
static uint64_t g_tls_used;
static WINAPI DWORD TlsAlloc(void) {
    for (DWORD i = 0; i < 64; i++)
        if (!(__sync_fetch_and_or(&g_tls_used, 1ull << i) & (1ull << i))) return i;
    return 0xffffffffu;   // TLS_OUT_OF_INDEXES
}
static WINAPI BOOL TlsFree(DWORD i) { if (i >= 64) return 0; __sync_fetch_and_and(&g_tls_used, ~(1ull << i)); return 1; }
static WINAPI void *TlsGetValue(DWORD i) {
    if (i >= 64) { vx_set_last_error(87); return NULL; }
    vx_set_last_error(0);
    return ((void **)(vx_teb() + TEB_TLS_SLOTS))[i];
}
static WINAPI BOOL TlsSetValue(DWORD i, void *v) {
    if (i >= 64) { vx_set_last_error(87); return 0; }
    ((void **)(vx_teb() + TEB_TLS_SLOTS))[i] = v;
    return 1;
}

// ---- memory -----------------------------------------------------------------------------------------------------------
#define PAGE_EXECUTE_READWRITE 0x40
#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define MEM_RELEASE 0x8000
#define MEM_IMAGE 0x1000000
#define MEM_PRIVATE 0x20000

static WINAPI void *VirtualAlloc(void *addr, size_t size, DWORD type, DWORD protect) {
    (void)protect;
    if (addr && (type & MEM_COMMIT) && !(type & MEM_RESERVE)) return addr;   // committing a reserved range
    void *p = mmap(addr, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) { vx_set_last_error(8 /* ERROR_NOT_ENOUGH_MEMORY */); return NULL; }
    return p;
}
static WINAPI BOOL VirtualFree(void *addr, size_t size, DWORD type) {
    if (type & MEM_RELEASE) { (void)size; return 1; }   // sizes are not tracked yet; release leaks the range
    return 1;
}
// Everything is mapped read/write/execute for now, so a protection change always "succeeds".
static WINAPI BOOL VirtualProtect(void *addr, size_t size, DWORD prot, DWORD *old) {
    (void)addr; (void)size; (void)prot;
    if (old) *old = PAGE_EXECUTE_READWRITE;
    return 1;
}
typedef struct { void *base, *alloc_base; DWORD alloc_protect; uint16_t partition, pad; size_t region; DWORD state, protect, type, pad2; } MEMORY_BASIC_INFORMATION;
static WINAPI size_t VirtualQuery(const void *addr, MEMORY_BASIC_INFORMATION *mbi, size_t len) {
    if (len < sizeof *mbi) return 0;
    uintptr_t page = (uintptr_t)addr & ~(uintptr_t)0xfff;
    memset(mbi, 0, sizeof *mbi);
    mbi->base = (void *)page;
    mbi->alloc_protect = mbi->protect = PAGE_EXECUTE_READWRITE;
    mbi->state = MEM_COMMIT;
    uintptr_t ib = (uintptr_t)vx_proc.image_base;
    if (page >= ib && page < ib + vx_proc.image_size) {
        mbi->alloc_base = (void *)ib; mbi->region = ib + vx_proc.image_size - page; mbi->type = MEM_IMAGE;
    } else {
        mbi->alloc_base = (void *)page; mbi->region = 0x1000; mbi->type = MEM_PRIVATE;
    }
    return sizeof *mbi;
}

// Heaps: one process heap, backed by malloc.
#define HEAP_ZERO_MEMORY 0x8
static WINAPI HANDLE GetProcessHeap(void) { return (HANDLE)(intptr_t)0x48454150; }
static WINAPI HANDLE HeapCreate(DWORD f, size_t a, size_t b) { (void)f; (void)a; (void)b; return GetProcessHeap(); }
static WINAPI BOOL HeapDestroy(HANDLE h) { (void)h; return 1; }
static WINAPI void *HeapAlloc(HANDLE h, DWORD flags, size_t n) { (void)h; return flags & HEAP_ZERO_MEMORY ? calloc(1, n ? n : 1) : malloc(n ? n : 1); }
static WINAPI void *HeapReAlloc(HANDLE h, DWORD flags, void *p, size_t n) {
    (void)h;
    size_t old = p ? malloc_size(p) : 0;
    void *q = realloc(p, n ? n : 1);
    if (q && (flags & HEAP_ZERO_MEMORY) && n > old) memset((char *)q + old, 0, n - old);
    return q;
}
static WINAPI BOOL HeapFree(HANDLE h, DWORD f, void *p) { (void)h; (void)f; free(p); return 1; }
static WINAPI size_t HeapSize(HANDLE h, DWORD f, const void *p) { (void)h; (void)f; return malloc_size(p); }

// ---- modules ---------------------------------------------------------------------------------------------------------------
// A module handle for one of this runtime's DLLs is a pointer to its (canonical) name.
static WINAPI void *GetModuleHandleA(const char *name) {
    if (!name) return vx_proc.image_base;
    static char names[32][64]; static int n;
    for (int i = 0; i < n; i++) if (!strcasecmp(names[i], name)) return names[i];
    if (n < 32) { snprintf(names[n], 64, "%s", name); return names[n++]; }
    return NULL;
}
static WINAPI void *GetModuleHandleW(const WCHAR *name) {
    if (!name) return vx_proc.image_base;
    char a[64]; int i = 0;
    for (; name[i] && i < 63; i++) a[i] = (char)name[i];
    a[i] = 0;
    return GetModuleHandleA(a);
}
static WINAPI void *LoadLibraryA(const char *name) { return GetModuleHandleA(name); }
static WINAPI void *LoadLibraryW(const WCHAR *name) { return GetModuleHandleW(name); }
static WINAPI BOOL FreeLibrary(void *m) { (void)m; return 1; }
static WINAPI void *GetProcAddress(void *module, const char *name) {
    if ((uintptr_t)name < 0x10000 || module == vx_proc.image_base) return NULL;   // ordinals and the exe: not yet
    void *fn = vx_find_export((const char *)module, name);
    if (!fn) vx_set_last_error(127 /* ERROR_PROC_NOT_FOUND */);
    return fn;
}
static WINAPI DWORD GetModuleFileNameA(void *m, char *buf, DWORD size) {
    (void)m;
    const char *path = vx_proc.argv[0];
    DWORD n = (DWORD)strlen(path);
    if (n >= size) n = size - 1;
    memcpy(buf, path, n); buf[n] = 0;
    return n;
}

// ---- system info -----------------------------------------------------------------------------------------------------------
typedef struct { uint16_t arch, reserved; DWORD page; void *min_addr, *max_addr; uint64_t mask; DWORD ncpu, type, gran; uint16_t level, rev; } SYSTEM_INFO;
static WINAPI void GetSystemInfo(SYSTEM_INFO *si) {
    memset(si, 0, sizeof *si);
    si->arch = 9;   // PROCESSOR_ARCHITECTURE_AMD64 (the program runs as x86-64 under Rosetta)
    si->page = 4096; si->gran = 65536;
    si->min_addr = (void *)0x10000; si->max_addr = (void *)0x7ffffffeffff;
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    si->ncpu = (DWORD)n; si->mask = n >= 64 ? ~0ull : (1ull << n) - 1;
    si->type = 8664; si->level = 6;
}
static WINAPI void GetNativeSystemInfo(SYSTEM_INFO *si) { GetSystemInfo(si); }

static const vx_export k32[] = {
    VX_EXPORT(GetStdHandle), VX_EXPORT(WriteFile), VX_EXPORT(WriteConsoleA), VX_EXPORT(WriteConsoleW),
    VX_EXPORT(ExitProcess), VX_EXPORT(TerminateProcess),
    VX_EXPORT(GetLastError), VX_EXPORT(SetLastError), VX_EXPORT(SetUnhandledExceptionFilter), VX_EXPORT(IsDebuggerPresent),
    VX_EXPORT(GetCurrentProcessId), VX_EXPORT(GetCurrentThreadId), VX_EXPORT(GetCurrentProcess), VX_EXPORT(GetCurrentThread),
    VX_EXPORT(GetCommandLineA), VX_EXPORT(GetCommandLineW), VX_EXPORT(GetStartupInfoA), VX_EXPORT(GetStartupInfoW),
    VX_EXPORT(Sleep), VX_EXPORT(QueryPerformanceFrequency), VX_EXPORT(QueryPerformanceCounter),
    VX_EXPORT(GetTickCount), VX_EXPORT(GetTickCount64), VX_EXPORT(GetSystemTimeAsFileTime), VX_EXPORT(GetSystemTimePreciseAsFileTime),
    VX_EXPORT(InitializeCriticalSection), VX_EXPORT(InitializeCriticalSectionAndSpinCount), VX_EXPORT(InitializeCriticalSectionEx),
    VX_EXPORT(EnterCriticalSection), VX_EXPORT(TryEnterCriticalSection), VX_EXPORT(LeaveCriticalSection), VX_EXPORT(DeleteCriticalSection),
    VX_EXPORT(InitializeSRWLock), VX_EXPORT(AcquireSRWLockExclusive), VX_EXPORT(ReleaseSRWLockExclusive),
    VX_EXPORT(AcquireSRWLockShared), VX_EXPORT(ReleaseSRWLockShared),
    VX_EXPORT(TlsAlloc), VX_EXPORT(TlsFree), VX_EXPORT(TlsGetValue), VX_EXPORT(TlsSetValue),
    VX_EXPORT(VirtualAlloc), VX_EXPORT(VirtualFree), VX_EXPORT(VirtualProtect), VX_EXPORT(VirtualQuery),
    VX_EXPORT(GetProcessHeap), VX_EXPORT(HeapCreate), VX_EXPORT(HeapDestroy), VX_EXPORT(HeapAlloc), VX_EXPORT(HeapReAlloc),
    VX_EXPORT(HeapFree), VX_EXPORT(HeapSize),
    VX_EXPORT(GetModuleHandleA), VX_EXPORT(GetModuleHandleW), VX_EXPORT(LoadLibraryA), VX_EXPORT(LoadLibraryW),
    VX_EXPORT(FreeLibrary), VX_EXPORT(GetProcAddress), VX_EXPORT(GetModuleFileNameA),
    VX_EXPORT(GetSystemInfo), VX_EXPORT(GetNativeSystemInfo),
};

void vx_kernel32_init(void) {
    vx_register_module("kernel32.dll", k32, sizeof k32 / sizeof k32[0]);
    vx_register_module("kernelbase.dll", k32, sizeof k32 / sizeof k32[0]);
}
