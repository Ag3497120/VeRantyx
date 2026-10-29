// The PE loader: maps Windows x86-64 images (the program and the DLLs it uses) into this process, relocates them,
// binds their imports to other images or to this runtime's own implementations, sets up the TEB/PEB and TLS,
// runs DLL initialization, and calls the program's entry point.
//
//   vxrt program.exe [args...]
//
// A DLL name resolves to this runtime's implementation when one is registered (kernel32, ucrtbase, advapi32,
// ...); otherwise to a PE file, searched in the program's directory, C:\windows\system32 and the current
// directory. Real DLLs (a game's steam_api64.dll, openvr_api.dll, msvcp140.dll) therefore load as they are.
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctype.h>
#include <pthread.h>
#include "vx.h"
#include "pe.h"
#ifdef __aarch64__
#include "../translator/aot.h"
#include "arm64.h"
#endif
#include "dotnet_host.h"
#include <sys/resource.h>
#include <limits.h>
#include <mach-o/getsect.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/ldsyms.h>

vx_process vx_proc;
int vx_trace;

// One write per line (processes share the terminal). A process started by another vxrt process names itself.
void vx_log(const char *fmt, ...) {
    char line[2048];
    int n;
    if (getenv("VXRT_CHILD") && vx_proc.argv && vx_proc.argv[0]) {
        const char *b = vx_proc.argv[0];
        for (const char *c = b; *c; c++) if (*c == '\\' || *c == '/') b = c + 1;
        n = snprintf(line, sizeof line, "[vxrt %.40s:%d] ", b, (int)getpid());
    } else n = snprintf(line, sizeof line, "[vxrt] ");
    static int timed = -1;                       // VXRT_LOG_TIME=1: the time of day (the same clock in every process)
    if (timed < 0) timed = getenv("VXRT_LOG_TIME") != NULL;
    if (timed) {
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        long day = (long)(ts.tv_sec % 86400);
        n += snprintf(line + n, sizeof line - (size_t)n, "%02ld:%02ld:%02ld.%03ld ", day / 3600, day / 60 % 60, day % 60, ts.tv_nsec / 1000000);
    }
    va_list ap; va_start(ap, fmt);
    n += vsnprintf(line + n, sizeof line - (size_t)n - 1, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof line - 2) n = (int)sizeof line - 2;
    line[n++] = '\n';
    fflush(stderr);
    write(2, line, (size_t)n);
}

// ---- built-in module registry ------------------------------------------------------------------------------------
typedef struct { const char *dll; const vx_export *exports; size_t count; } vx_module;
static vx_module g_modules[512];
static size_t g_module_count;

void vx_register_module(const char *dll, const vx_export *exports, size_t count) {
    if (g_module_count < sizeof g_modules / sizeof g_modules[0]) g_modules[g_module_count++] = (vx_module){ dll, exports, count };
    else vx_log("too many export tables: %s is left out", dll);
}

// API sets resolve to the DLL that implements them. The "downlevel" sets (api-ms-win-downlevel-<dll>-l1-1-0, which
// programs built for Windows 7 and later link) are named after the DLL they stand for.
static const char *canonical_dll(const char *dll) {
    if (!strncasecmp(dll, "api-ms-win-downlevel-", 21)) {
        static const char *const downlevel[] = { "advapi32.dll", "kernel32.dll", "normaliz.dll", "ole32.dll", "oleaut32.dll",
                                                 "shell32.dll", "shlwapi.dll", "user32.dll", "version.dll" };
        for (size_t i = 0; i < sizeof downlevel / sizeof *downlevel; i++) {
            size_t n = strlen(downlevel[i]) - 4;
            if (!strncasecmp(dll + 21, downlevel[i], n) && dll[21 + n] == '-') return downlevel[i];
        }
    }
    if (!strncasecmp(dll, "api-ms-win-crt-", 15)) return "ucrtbase.dll";
    if (!strncasecmp(dll, "api-ms-win-core-version-", 24)) return "version.dll";
    if (!strncasecmp(dll, "api-ms-win-devices-config-", 26)) return "cfgmgr32.dll";
    if (!strncasecmp(dll, "api-ms-win-core-", 16)) return "kernel32.dll";
    if (!strncasecmp(dll, "api-ms-win-security-", 20) || !strncasecmp(dll, "api-ms-win-eventing-", 20)) return "advapi32.dll";
    if (!strncasecmp(dll, "api-ms-win-ntuser-", 18) || !strncasecmp(dll, "ext-ms-win-ntuser-", 18)) return "user32.dll";
    if (!strncasecmp(dll, "api-ms-win-shcore-", 18)) return "shcore.dll";
    if (!strncasecmp(dll, "api-ms-win-service-", 19)) return "sechost.dll";
    if (!strncasecmp(dll, "api-ms-win-power-", 17)) return "powrprof.dll";
    if (!strncasecmp(dll, "api-ms-win-", 11)) return "kernelbase.dll";
    return dll;
}

// Compares DLL names, ignoring case and a missing ".dll".
static int same_dll(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    if (la > 4 && !strcasecmp(a + la - 4, ".dll")) la -= 4;
    if (lb > 4 && !strcasecmp(b + lb - 4, ".dll")) lb -= 4;
    return la == lb && !strncasecmp(a, b, la);
}

static int is_builtin(const char *dll) {
    dll = canonical_dll(dll);
    for (size_t m = 0; m < g_module_count; m++) if (same_dll(g_modules[m].dll, dll)) return 1;
    return 0;
}
int vx_is_builtin_dll(const char *dll) { return is_builtin(dll); }

void *vx_find_export(const char *dll, const char *name) {
    dll = canonical_dll(dll);
    for (size_t m = 0; m < g_module_count; m++) {
        if (!same_dll(g_modules[m].dll, dll)) continue;
        for (size_t i = 0; i < g_modules[m].count; i++)
            if (!strcmp(g_modules[m].exports[i].name, name)) return g_modules[m].exports[i].fn;
    }
    return NULL;
}

// A built-in DLL's module handle is a small PE image made for it (Microsoft's PE format): headers and an export
// directory naming its functions, each exported as a jump to the function. Programs and libraries that read a
// module's exports themselves rather than through GetProcAddress (NVIDIA Streamline's sl.interposer.dll walks
// dxgi.dll's to find CreateDXGIFactory1) find them there. GetProcAddress still gives the functions themselves.
// Data exports (variables) are left out: an export is an RVA into the image, and only code gets a jump.
// (ARM64 build: the stubs are x86 code the engine translates; their jumps reach the functions' thunks.)
typedef struct { char name[64]; uint8_t *image; } builtin_module;
static builtin_module g_builtins[256];
static int g_builtin_count;
typedef struct { const char *name; void *fn; size_t order; } builtin_export;
static int export_order(const void *a, const void *b) {
    const builtin_export *x = a, *y = b;
    int c = strcmp(x->name, y->name);
    return c ? c : (x->order > y->order) - (x->order < y->order);
}
static void put16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }
static uint8_t *make_builtin_image(const char *given) {
    const char *dll = canonical_dll(given);
    size_t n = 0, cap = 0;
    builtin_export *e = NULL;
    unsigned long text_size = 0;
    uint8_t *text = getsegmentdata(&_mh_execute_header, "__TEXT", &text_size);
    for (size_t m = 0; m < g_module_count; m++) {
        if (!same_dll(g_modules[m].dll, dll)) continue;
        for (size_t i = 0; i < g_modules[m].count; i++) {
            const vx_export *x = &g_modules[m].exports[i];
            if (!x->name || x->name[0] == '#' || (uint8_t *)x->fn < text || (uint8_t *)x->fn >= text + text_size) continue;
#ifdef __aarch64__
            if (!vx_thunk_find((uint64_t)(uintptr_t)x->fn)) continue;   // (the stub's jump reaches the function's thunk)
#endif
            if (n == cap) { cap = cap ? cap * 2 : 256; builtin_export *g = realloc(e, cap * sizeof *e); if (!g) break; e = g; }
            e[n] = (builtin_export){ x->name, x->fn, n }; n++;
        }
    }
    if (n) qsort(e, n, sizeof *e, export_order);
    size_t k = 0;                                                   // one per name: the first registered, as vx_find_export
    for (size_t i = 0; i < n; i++) if (!k || strcmp(e[k - 1].name, e[i].name)) e[k++] = e[i];
    n = k;
    size_t names = strlen(given) + 1;
    for (size_t i = 0; i < n; i++) names += strlen(e[i].name) + 1;
    const uint32_t dir = 0x1000, funcs = dir + 40, name_rvas = funcs + 4 * (uint32_t)n, ords = name_rvas + 4 * (uint32_t)n;
    const uint32_t strings = ords + 2 * (uint32_t)n, dir_end = strings + (uint32_t)names;
    const uint32_t stubs = (dir_end + 15) & ~15u, size = (stubs + 16 * (uint32_t)n + 0xfff) & ~0xfffu;
    uint8_t *img = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (img == MAP_FAILED) { free(e); return NULL; }
    img[0] = 'M'; img[1] = 'Z'; put32(img + 0x3c, 0x40);
    uint8_t *pe = img + 0x40, *opt = pe + 24, *sec = opt + 0xF0;
    memcpy(pe, "PE\0\0", 4);
    put16(pe + 4, 0x8664); put16(pe + 6, 1); put16(pe + 20, 0xF0); put16(pe + 22, 0x2022);   // AMD64, 1 section, DLL
    put16(opt, 0x20b); put32(opt + 20, 0x1000); put64(opt + 24, (uint64_t)(uintptr_t)img);
    put32(opt + 32, 0x1000); put32(opt + 36, 0x200); put16(opt + 40, 10); put16(opt + 48, 10);
    put32(opt + 56, size); put32(opt + 60, 0x1000); put16(opt + 68, 3 /* console */); put16(opt + 70, 0x160);
    put32(opt + 108, 16); put32(opt + 112, dir); put32(opt + 116, dir_end - dir);   // DataDirectory[0]: exports
    memcpy(sec, ".text", 5); put32(sec + 8, size - 0x1000); put32(sec + 12, 0x1000); put32(sec + 36, 0x60000020);
    uint8_t *d = img + dir;
    char *str = (char *)img + strings;
    memcpy(str, given, strlen(given) + 1);
    put32(d + 12, strings); put32(d + 16, 1); put32(d + 20, (uint32_t)n); put32(d + 24, (uint32_t)n);
    put32(d + 28, funcs); put32(d + 32, name_rvas); put32(d + 36, ords);
    str += strlen(given) + 1;
    for (size_t i = 0; i < n; i++) {
        uint8_t *stub = img + stubs + 16 * i;                       // jmp qword [rip+0], then the address
        stub[0] = 0xff; stub[1] = 0x25; put32(stub + 2, 0); put64(stub + 6, (uint64_t)(uintptr_t)e[i].fn); stub[14] = stub[15] = 0xcc;
        put32(img + funcs + 4 * i, stubs + 16 * (uint32_t)i);
        put32(img + name_rvas + 4 * i, (uint32_t)((uint8_t *)str - img));
        put16(img + ords + 2 * i, (uint16_t)i);
        size_t l = strlen(e[i].name) + 1;
        memcpy(str, e[i].name, l); str += l;
    }
    free(e);
#ifdef __aarch64__
    mprotect(img, size, PROT_READ);                                 // (x86 code: translated, its pages not executable)
#else
    mprotect(img, size, PROT_READ | PROT_EXEC);
#endif
    return img;
}
static void *builtin_handle(const char *name) {
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    void *h = NULL;
    int n = __atomic_load_n(&g_builtin_count, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n && !h; i++) if (same_dll(g_builtins[i].name, name)) h = g_builtins[i].image;
    if (!h && n < 256) {
        snprintf(g_builtins[n].name, sizeof g_builtins[n].name, "%s", name);
        g_builtins[n].image = make_builtin_image(g_builtins[n].name);
        if ((h = g_builtins[n].image)) __atomic_store_n(&g_builtin_count, n + 1, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&lock);
    return h;
}
#ifdef __aarch64__
// A built-in module's export stub at rip (jmp qword [rip+0]): its target (the function's thunk), for the recompiled
// code's runner, which follows it rather than having the engine translate it (translator/aot.c: no JIT on visionOS)
static uint32_t rd32(const uint8_t *p);
static uint64_t rd64(const uint8_t *p);
int vx_builtin_stub(uint64_t rip, uint64_t *target) {
    int n = __atomic_load_n(&g_builtin_count, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++) {
        const uint8_t *img = g_builtins[i].image;
        if (!img || rip < (uint64_t)(uintptr_t)img || rip - (uint64_t)(uintptr_t)img >= rd32(img + 0x40 + 24 + 56)) continue;
        const uint8_t *p = (const uint8_t *)(uintptr_t)rip;
        if (rip + 14 > (uint64_t)(uintptr_t)img + rd32(img + 0x40 + 24 + 56) || p[0] != 0xff || p[1] != 0x25) return 0;
        *target = rd64(p + 6 + (int32_t)rd32(p + 2));
        return 1;
    }
    return 0;
}
#endif
// The DLL name of a built-in module's handle, NULL for anything else.
static const char *builtin_name(const void *h) {
    int n = __atomic_load_n(&g_builtin_count, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++) if (h && g_builtins[i].image == h) return g_builtins[i].name;
    return NULL;
}
static int is_builtin_handle(const void *h) { return builtin_name(h) != NULL; }

#ifdef __aarch64__
#define VX_PROT_CODE (PROT_READ | PROT_WRITE)      // (x86 code: read by the translator, never run as it is)
__thread size_t vx_x86_stack_size;
#else
#define VX_PROT_CODE (PROT_READ | PROT_WRITE | PROT_EXEC)
#endif
// ---- unimplemented imports ------------------------------------------------------------------------------------
// Each missing import gets a small stub that names itself when called:
//   mov rcx, "dll!function" ; mov rax, vx_unimplemented ; jmp rax
static WINAPI void vx_unimplemented(const char *what) {
    vx_log("called an unimplemented function: %s", what);
    fflush(stderr);
    _exit(3);
}

static void *make_stub(const char *dll, const char *name) {
    static uint8_t *page; static size_t used;
    if (!page || used + 32 > 16384) {
        page = mmap(NULL, 16384, VX_PROT_CODE, MAP_PRIVATE | MAP_ANON, -1, 0);
        used = 0;
    }
    char *label = malloc(strlen(dll) + strlen(name) + 2);
    sprintf(label, "%s!%s", dll, name);
    uint8_t *s = page + used; used += 32;
    s[0] = 0x48; s[1] = 0xb9; memcpy(s + 2, &label, 8);                       // mov rcx, imm64
    void *target = (void *)vx_unimplemented;
    s[10] = 0x48; s[11] = 0xb8; memcpy(s + 12, &target, 8);                  // mov rax, imm64
    s[20] = 0xff; s[21] = 0xe0;                                               // jmp rax
    return s;
}

// ---- PE structures (the parts used) ----------------------------------------------------------------------------
#pragma pack(push, 1)
typedef struct { uint32_t va, size; } pe_dir;
typedef struct {
    char name[8]; uint32_t vsize, va, raw_size, raw_ptr, reloc_ptr, line_ptr; uint16_t nreloc, nline; uint32_t flags;
} pe_section;
#pragma pack(pop)

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

// ---- loaded images ----------------------------------------------------------------------------------------------
#define MAX_IMAGES 256
static vx_image *g_images[MAX_IMAGES];
static int g_image_count;
static pthread_mutex_t g_loader_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;   // Windows' loader lock
static DWORD g_next_tls_index;
#define MAX_TLS_MODULES 128

// Lock-free: images are only ever appended (and never unmapped), so a reader sees a consistent prefix. An unloaded
// image is still found by address: a thread or a function pointer may still be in it.
const vx_image *vx_image_at(uint64_t pc) {
    int n = __atomic_load_n(&g_image_count, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++) {
        const vx_image *m = g_images[i];
        if (pc >= (uint64_t)m->base && pc < (uint64_t)m->base + m->size) return m;
    }
    return NULL;
}
// The loaded images, in load order (psapi's EnumProcessModules).
int vx_image_list(const vx_image **out, int cap) {
    int n = __atomic_load_n(&g_image_count, __ATOMIC_ACQUIRE), k = 0;
    for (int i = 0; i < n; i++) if (!g_images[i]->unloaded) { if (k < cap) out[k] = g_images[i]; k++; }
    return k;
}
static vx_image *image_by_base(const void *base) {
    for (int i = 0; i < g_image_count; i++) if (g_images[i]->base == base && !g_images[i]->unloaded) return g_images[i];
    return NULL;
}
static vx_image *image_by_name(const char *name) {
    const char *b = strrchr(name, '\\'); b = b ? b + 1 : name;
    const char *s = strrchr(b, '/'); b = s ? s + 1 : b;
    for (int i = 0; i < g_image_count; i++) if (same_dll(g_images[i]->name, b) && !g_images[i]->unloaded) return g_images[i];
    return NULL;
}

// ---- TEB / PEB / TLS -------------------------------------------------------------------------------------------
static uint8_t *g_peb;
static pthread_key_t g_teb_key;
static uint8_t **g_tebs;                 // every attached thread's TEB, so a DLL loaded later reaches them all
static int g_teb_count, g_teb_cap;
static pthread_mutex_t g_tebs_lock = PTHREAD_MUTEX_INITIALIZER;

// macOS keeps gs pointed at the pthread's TSD array and does not let us move it. Windows code reads the TEB at
// gs:0x30 and the TLS array at gs:0x58, so those two TSD slots are set to the TEB's values on every thread, as
// Wine did on macOS before version 8. Native code (this runtime, macOS, Metal, D3DMetal) keeps working on the
// same thread with no switching.
// gs:0x60 (the PEB, which compilers read directly as NtCurrentPeb()) is macOS's TSD slot 12, where libc keeps
// the buffer of the non-reentrant localtime(). Program threads point it at the PEB; nothing here calls
// localtime() (only localtime_r), so the slot is never used as a buffer while the PEB is in it.
static void set_gs_slots(uint8_t *teb) {
#ifdef __aarch64__
    (void)teb;                                     // (ARM64: translated code's gs is the TEB, x86_ctx.gs_base)
#else
    uint64_t self = (uint64_t)teb, tls = *(uint64_t *)(teb + TEB_TLS_POINTER), peb = (uint64_t)g_peb;
    __asm__ volatile("movq %0, %%gs:0x30" :: "r"(self));
    __asm__ volatile("movq %0, %%gs:0x58" :: "r"(tls));
    __asm__ volatile("movq %0, %%gs:0x60" :: "r"(peb));
#endif
}

// A fresh copy of an image's TLS template (static __declspec(thread) data).
static void *tls_block(const vx_image *m) {
    const uint8_t *td = m->base + m->tls_rva;
    uint64_t start = rd64(td), end = rd64(td + 8);
    uint32_t zero_fill = rd32(td + 32);
    size_t size = (end - start) + zero_fill;
    uint8_t *block = calloc(1, size ? size : 1);
    memcpy(block, (void *)start, end - start);
    return block;
}

uint8_t *vx_teb(void) {
    uint8_t *teb = pthread_getspecific(g_teb_key);
    if (!teb) { vx_thread_attach(); teb = pthread_getspecific(g_teb_key); }
    return teb;
}

void vx_signal_stack(void);   // seh.c
void vx_thread_attach(void) {
    vx_signal_stack();
    uint8_t *teb = calloc(1, TEB_SIZE);
#ifdef __aarch64__
    // The program's code runs on a stack of its own (this thread's stays the runtime's), below a guard page.
    size_t size = vx_x86_stack_size ? vx_x86_stack_size : (2u << 20);   // (as CreateThread's least: 2 MB)
    uint8_t *lo = mmap(NULL, size + 16384, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    mprotect(lo, 16384, PROT_NONE);
    *(void **)(teb + TEB_STACK_BASE) = lo + 16384 + size;
    *(void **)(teb + TEB_STACK_LIMIT) = lo + 16384;
#else
    pthread_t self = pthread_self();
    uint8_t *stack_top = pthread_get_stackaddr_np(self);
    *(void **)(teb + TEB_STACK_BASE) = stack_top;
    *(void **)(teb + TEB_STACK_LIMIT) = stack_top - pthread_get_stacksize_np(self);
#endif
    *(void **)(teb + TEB_SELF) = teb;
    *(uint64_t *)(teb + TEB_CLIENT_ID) = (uint64_t)getpid();
    *(uint64_t *)(teb + TEB_CLIENT_ID + 8) = vx_current_tid();
    *(void **)(teb + TEB_PEB) = g_peb;
    void **tls = calloc(MAX_TLS_MODULES, sizeof(void *));
    *(void ***)(teb + TEB_TLS_POINTER) = tls;
    pthread_mutex_lock(&g_loader_lock);
    for (int i = 0; i < g_image_count; i++)
        if (g_images[i]->tls_index != ~0u && !g_images[i]->unloaded) tls[g_images[i]->tls_index] = tls_block(g_images[i]);
    pthread_mutex_unlock(&g_loader_lock);
    pthread_mutex_lock(&g_tebs_lock);
    if (g_teb_count == g_teb_cap) { g_teb_cap = g_teb_cap ? g_teb_cap * 2 : 64; g_tebs = realloc(g_tebs, g_teb_cap * sizeof *g_tebs); }
    g_tebs[g_teb_count++] = teb;
    pthread_mutex_unlock(&g_tebs_lock);
    pthread_setspecific(g_teb_key, teb);
    set_gs_slots(teb);
    vx_thread_live(1);                                         // (OpenThread finds it by its ID from now on: sync.c)
}

void vx_thread_detach(void) {
    uint8_t *teb = pthread_getspecific(g_teb_key);
    if (!teb) return;
    vx_thread_live(0);
    pthread_mutex_lock(&g_tebs_lock);
    for (int i = 0; i < g_teb_count; i++) if (g_tebs[i] == teb) { g_tebs[i] = g_tebs[--g_teb_count]; break; }
    pthread_mutex_unlock(&g_tebs_lock);
    void **tls = *(void ***)(teb + TEB_TLS_POINTER);
    for (int i = 0; i < MAX_TLS_MODULES; i++) free(tls[i]);
    free(tls);
    free(*(void **)(teb + TEB_TLS_EXPANSION));                // (TlsSetValue's slots past the 64th)
    pthread_setspecific(g_teb_key, NULL);
#ifdef __aarch64__
    void vx_arm64_thread_exit(void);
    vx_arm64_thread_exit();
    uint8_t *lo = *(uint8_t **)(teb + TEB_STACK_LIMIT), *hi = *(uint8_t **)(teb + TEB_STACK_BASE);
    munmap(lo - 16384, (size_t)(hi - lo) + 16384);
#else
    // The PEB leaves TSD slot 12 before the thread ends: libc frees whatever localtime's slot holds.
    __asm__ volatile("movq $0, %%gs:0x60" ::: "memory");
#endif
    free(teb);
}

void vx_set_last_error(DWORD e) { *(DWORD *)(vx_teb() + TEB_LAST_ERROR) = e; }
DWORD vx_get_last_error(void) { return *(DWORD *)(vx_teb() + TEB_LAST_ERROR); }

typedef BOOL (WINAPI *dll_entry)(void *, DWORD, void *);
static void tls_callbacks(const vx_image *m, DWORD reason) {
    if (!m->tls_rva) return;
    uint64_t callbacks = rd64(m->base + m->tls_rva + 24);
    for (dll_entry *cb = (dll_entry *)callbacks; cb && *cb; cb++) (*cb)(m->base, reason, NULL);
}

// Thread attach (2) / detach (3): every image's TLS callbacks and every initialized DLL's entry point, in load
// order for attach and reverse order for detach, under the loader lock.
void vx_notify_thread(DWORD reason) {
    pthread_mutex_lock(&g_loader_lock);
    for (int k = 0; k < g_image_count; k++) {
        vx_image *m = g_images[reason == 3 ? g_image_count - 1 - k : k];
        if (m->unloaded) continue;
        tls_callbacks(m, reason);
        if (m->is_dll && m->initialized && !m->no_thread_calls && m->entry_rva)
            ((dll_entry)(m->base + m->entry_rva))(m->base, reason, NULL);
    }
    pthread_mutex_unlock(&g_loader_lock);
}

// ---- exports ----------------------------------------------------------------------------------------------------------
static void *resolve(const char *dll, const char *name, int ordinal, int depth);

// An image's export by name or (name == NULL) by ordinal; forwarders ("other.Function") are followed.
static void *pe_export(const vx_image *m, const char *name, int ordinal, int depth) {
    if (!m->export_rva) return NULL;
    const uint8_t *ed = m->base + m->export_rva;
    uint32_t ord_base = rd32(ed + 16), nfuncs = rd32(ed + 20), nnames = rd32(ed + 24);
    const uint32_t *funcs = (const uint32_t *)(m->base + rd32(ed + 28));
    const uint32_t *names = (const uint32_t *)(m->base + rd32(ed + 32));
    const uint16_t *ords = (const uint16_t *)(m->base + rd32(ed + 36));
    uint32_t index = ~0u;
    if (name) {
        uint32_t lo = 0, hi = nnames;                            // the name table is sorted
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            int c = strcmp(name, (const char *)m->base + names[mid]);
            if (!c) { index = ords[mid]; break; }
            if (c < 0) hi = mid; else lo = mid + 1;
        }
    } else {
        index = (uint32_t)ordinal - ord_base;
    }
    if (index >= nfuncs || !funcs[index]) return NULL;
    uint32_t rva = funcs[index];
    if (rva >= m->export_rva && rva < m->export_rva + m->export_size) {
        const char *fwd = (const char *)m->base + rva, *dot = strrchr(fwd, '.');
        if (!dot || depth > 8) return NULL;
        char dll[256];
        snprintf(dll, sizeof dll, "%.*s.dll", (int)(dot - fwd), fwd);
        return dot[1] == '#' ? resolve(dll, NULL, atoi(dot + 2), depth + 1) : resolve(dll, dot + 1, 0, depth + 1);
    }
    return m->base + rva;
}

// ---- loading ------------------------------------------------------------------------------------------------------------
static char g_exe_dir[4096];
static char g_altered_dir[4096];   // LoadLibraryEx(LOAD_WITH_ALTERED_SEARCH_PATH): the DLL's own directory, searched first
static char g_dll_dir[4096];       // SetDllDirectory: searched after the program's directory (Unity's Plugins folder)
static DWORD g_search_flags;       // the LOAD_LIBRARY_SEARCH_* flags of the LoadLibraryEx in progress (0: the process's)

// Finds a DLL file: (the altered search directory,) the program's directory, (the SetDllDirectory directory,)
// C:\windows\system32, the current directory. With LOAD_LIBRARY_SEARCH_* flags (LoadLibraryEx's, else
// SetDefaultDllDirectories', src/kernelx.c) only what they name: the program's directory (0x200), the SetDllDirectory
// and AddDllDirectory directories (0x400), system32 and VXRT_DLL_PATH (0x800), all three (0x1000); never the current one.
// Wine's own DLLs (marked "Wine builtin DLL" after the DOS header) need Wine underneath: they are passed over.
static int wine_builtin(const char *path) {
    char head[0x60] = { 0 };
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(head, 1, sizeof head, f);
    fclose(f);
    return n == sizeof head && !memcmp(head + 0x40, "Wine builtin DLL", 16);
}
// VXRT_DLL_FIRST: a Mac directory whose DLLs are loaded in place of the program's own of the same file name, however it
// names them (by name or by full path). The launch scripts put the frame-capture proxy there as openvr_api.dll (with
// the game's own copied beside it as openvr_api_valve.dll), so a game's folder is never changed.
static int preferred_dll(const char *name, char *out, size_t cap) {
    const char *dir = getenv("VXRT_DLL_FIRST");
    if (!dir || !*dir) return 0;
    const char *base = name;
    for (const char *c = name; *c; c++) if (*c == '\\' || *c == '/') base = c + 1;
    char file[256];
    snprintf(file, sizeof file, "%s%s", base, strchr(base, '.') ? "" : ".dll");
    for (char *c = file; *c; c++) if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');   // (the files there: lower case)
    snprintf(out, cap, "%s/%s", dir, file);
    struct stat st;
    return !stat(out, &st) && S_ISREG(st.st_mode);
}
static int search_dll(const char *name, char *out, size_t cap) {
    if (preferred_dll(name, out, cap)) return 1;
    char file[256];
    snprintf(file, sizeof file, "%s%s", name, strchr(name, '.') ? "" : ".dll");
    DWORD flags = g_search_flags ? g_search_flags : vx_dll_default_search();
    if (flags & 0x1000) flags |= 0xe00;                          // LOAD_LIBRARY_SEARCH_DEFAULT_DIRS
    int app = !flags || (flags & 0x200), user = !flags || (flags & 0x400), sys = !flags || (flags & 0x800);
    char dirs[5][4096] = { { 0 } };
    snprintf(dirs[0], sizeof dirs[0], "%s", g_altered_dir);
    if (app) snprintf(dirs[1], sizeof dirs[1], "%s", g_exe_dir);
    if (user) snprintf(dirs[2], sizeof dirs[2], "%s", g_dll_dir);
    if (sys) snprintf(dirs[3], sizeof dirs[3], "%s/drive_c/windows/system32", vx_root());
    char cwd[4096];
    vx_get_cwd(cwd, sizeof cwd);
    if (flags || !vx_unix_path(cwd, dirs[4], sizeof dirs[4])) dirs[4][0] = 0;
    for (int i = 0; i < 5; i++) {
        char dir[4096];
        for (int k = 0; i == 3 && (flags & 0x400) && vx_dll_user_directory(k, dir, sizeof dir); k++) {   // AddDllDirectory's
            struct stat st;
            snprintf(out, cap, "%s/%s", dir, file);
            if (!stat(out, &st) && S_ISREG(st.st_mode)) return 1;
        }
        if (!dirs[i][0]) continue;
        snprintf(out, cap, "%s/%s", dirs[i], file);
        char shadow[4096];
        if (vx_overlay_file(out, shadow, sizeof shadow)) { snprintf(out, cap, "%s", shadow); return 1; }   // (VXRT_OVERLAY's copy first)
        struct stat st;
        if (!stat(out, &st) && S_ISREG(st.st_mode)) return 1;
    }
    // VXRT_DLL_PATH: more Mac directories (colon-separated) holding the user's own Windows DLLs, for example the
    // Visual C++ runtime a Windows installation already has.
    const char *extra = sys ? getenv("VXRT_DLL_PATH") : NULL;
    while (extra && *extra) {
        const char *colon = strchr(extra, ':');
        size_t n = colon ? (size_t)(colon - extra) : strlen(extra);
        snprintf(out, cap, "%.*s/%s", (int)n, extra, file);
        struct stat st;
        if (n && !stat(out, &st) && S_ISREG(st.st_mode) && !wine_builtin(out)) return 1;
        extra = colon ? colon + 1 : NULL;
    }
    return 0;
}

static vx_image *load_pe(const char *path, int is_main);
static int init_dll(vx_image *m);

static vx_image *load_dll(const char *name) {
    vx_image *m = image_by_name(name);
    if (m) return m;
    char path[4096];
    if (!search_dll(name, path, sizeof path)) return NULL;
    return load_pe(path, 0);
}

// What an import of dll!name (or dll!#ordinal) binds to: a loaded or loadable PE image's export, else this
// runtime's implementation, else NULL.
static void *resolve(const char *dll, const char *name, int ordinal, int depth) {
    vx_image *m = image_by_name(dll);
    if (m) return pe_export(m, name, ordinal, depth);
    if (is_builtin(dll)) {
        if (name) return vx_find_export(dll, name);
        char by_number[16];                                       // built-ins list ordinals as "#n"
        snprintf(by_number, sizeof by_number, "#%d", ordinal);
        return vx_find_export(dll, by_number);
    }
    m = load_dll(dll);
    if (m) m->pinned = 1;                                         // (a forwarder's target)
    return m && init_dll(m) ? pe_export(m, name, ordinal, depth) : NULL;
}

// Runs a DLL's initialization: TLS callbacks, then its entry point with DLL_PROCESS_ATTACH.
static int init_dll(vx_image *m) {
    if (m->initialized || !m->is_dll) return 1;
    m->initialized = 1;
    tls_callbacks(m, 1);
    if (m->entry_rva && !((dll_entry)(m->base + m->entry_rva))(m->base, 1 /* DLL_PROCESS_ATTACH */, NULL)) {
        vx_log("%s: DllMain failed", m->name);
        return 0;
    }
    return 1;
}

static vx_image *load_pe(const char *path, int is_main) {
    FILE *f = fopen(path, "rb");
    if (!f) { vx_log("cannot open %s: %s", path, strerror(errno)); vx_set_last_error(2); return NULL; }
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    if (len < 0) { fclose(f); vx_log("cannot size %s", path); vx_set_last_error(193); return NULL; }
    uint8_t *file = malloc(len);
    if (!file || fread(file, 1, (size_t)len, f) != (size_t)len) { fclose(f); free(file); return NULL; }
    fclose(f);
    vx_pe_info pe;
    if (!vx_pe_inspect(file, (size_t)len, &pe)) {
        vx_log("%s: invalid or unsupported PE image", path); free(file); vx_set_last_error(193); return NULL;
    }
    if (pe.has_cli) {
        vx_log("%s: managed %s image (machine 0x%04x, CLR metadata %s); CLR hosting is not implemented",
               path, pe.optional_magic == 0x10b ? "PE32" : "PE32+", pe.machine,
               pe.metadata_version[0] ? pe.metadata_version : "version unknown");
        free(file); vx_set_last_error(193 /* ERROR_BAD_EXE_FORMAT */); return NULL;
    }
    if (pe.machine != 0x8664 || pe.optional_magic != 0x20b) {
        if (pe.machine == 0x014c && pe.optional_magic == 0x10b)
            vx_log("%s: native 32-bit x86 PE32 image; 32-bit native execution is not implemented", path);
        else
            vx_log("%s: unsupported PE machine 0x%04x / optional-header format 0x%04x",
                   path, pe.machine, pe.optional_magic);
        free(file); vx_set_last_error(193 /* ERROR_BAD_EXE_FORMAT */); return NULL;
    }
    uint16_t nsections = pe.section_count, characteristics = pe.characteristics;
    size_t section_table_offset = pe.section_table_offset;
    uint64_t pref_base = pe.image_base;
    uint32_t image_size = pe.image_size, headers_size = pe.headers_size, entry_rva = pe.entry_rva;
    pe_dir dirs[16];
    for (uint32_t i = 0; i < 16; i++) dirs[i] = (pe_dir){ pe.directories[i].rva, pe.directories[i].size };

    // A DLL loaded again after its last FreeLibrary (SDL's libusb-1.0.dll in Steam: loaded, found wanting, freed, a few
    // times a second) gets a fresh copy where the last one was, and that one's place in the table: the table would
    // otherwise fill up ("too many images" within a minute) and the memory grow with every load. On Windows the
    // unloaded copy is unmapped; here the same file's pages come back at the same addresses, so code of it still
    // running on another thread finds the same instructions.
    int reuse = -1;
    if (!is_main) {
        char *real = realpath(path, NULL);
        for (int i = 0; real && i < g_image_count; i++) {
            const vx_image *o = g_images[i];
            if (o->unloaded && !o->pinned && o->is_dll && o->size == image_size && o->unix_path && !strcmp(o->unix_path, real)) { reuse = i; break; }
        }
        free(real);
    }
    uint64_t want = pref_base;
#ifdef __aarch64__
    // VXRT_AOT_DIR: a module made for this file at another base (a DLL whose preferred base is in macOS's shared cache,
    // at 0x180000000) is used where it was made for: the file is mapped there when that is free (translator/aot.c)
    if (getenv("VXRT_AOT_DIR") && reuse < 0) {
        uint64_t b = xa_module_base(getenv("VXRT_AOT_DIR"), path, xa_hash(file, (uint64_t)len));
        if (b && b != pref_base) {
            // (exactly there when free: mmap takes a hint in some ranges as none)
            mach_vm_address_t a = b;
            if (mach_vm_allocate(mach_task_self(), &a, image_size, VM_FLAGS_FIXED) == KERN_SUCCESS) {
                want = b;                                        // (reserved: the mapping below replaces it)
            } else vx_log("aot: %s: its module's base %#llx is taken: the module is not used", path, (unsigned long long)b);
        }
    }
#endif
    // Map at the preferred base when free, anywhere otherwise (then relocate).
    uint8_t *base = reuse >= 0 ? mmap(g_images[reuse]->base, image_size, VX_PROT_CODE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0)
                               : mmap((void *)want, image_size, VX_PROT_CODE, MAP_PRIVATE | MAP_ANON | (want != pref_base ? MAP_FIXED : 0), -1, 0);
    if (base == MAP_FAILED) { vx_log("cannot map %u bytes", image_size); free(file); return NULL; }
    memcpy(base, file, headers_size);
    const pe_section *sec = (const pe_section *)(file + section_table_offset);
    for (uint16_t i = 0; i < nsections; i++) {
        uint32_t n = sec[i].raw_size < sec[i].vsize || !sec[i].vsize ? sec[i].raw_size : sec[i].vsize;
        if (n) memcpy(base + sec[i].va, file + sec[i].raw_ptr, n);
    }
#ifdef __aarch64__
    // VXRT_AOT_DIR: statically recompiled code for this file, if there is some (translator/aot.c, RECOMP.md): a module
    // named after the file and its bytes' hash, made for it at this base
    uint64_t aot_hash = getenv("VXRT_AOT_DIR") ? xa_hash(file, (uint64_t)len) : 0;
#endif
    free(file);

    int64_t delta = (int64_t)((uint64_t)base - pref_base);
    if (delta) {
        // (no relocation directory: nothing to fix up, as Windows takes it, unless the file says its relocations
        // were stripped; resource-only DLLs such as ICU's data DLL are built so)
        if (!dirs[5].size && (characteristics & 0x0001)) { vx_log("%s: loaded away from its base and has no relocations", path); return NULL; }
        for (uint8_t *b = base + dirs[5].va; b < base + dirs[5].va + dirs[5].size;) {
            uint32_t page = rd32(b), block = rd32(b + 4);
            if (!block) break;
            for (uint32_t k = 8; k < block; k += 2) {
                uint16_t e = rd16(b + k);
                if ((e >> 12) == 10) { uint64_t *p = (uint64_t *)(base + page + (e & 0xfff)); *p += delta; }
                else if ((e >> 12) == 3) { uint32_t *p = (uint32_t *)(base + page + (e & 0xfff)); *p += (uint32_t)delta; }
                else if ((e >> 12) != 0) { vx_log("%s: relocation type %u not handled", path, e >> 12); return NULL; }
            }
            b += block;
        }
    }

    vx_image *m = calloc(1, sizeof *m);
    const char *slash = strrchr(path, '/');
    snprintf(m->name, sizeof m->name, "%s", slash ? slash + 1 : path);
    m->unix_path = realpath(path, NULL);
    if (!m->unix_path) m->unix_path = strdup(path);
    char win[4096];
    vx_windows_path(m->unix_path, win, sizeof win);
    m->windows_path = strdup(win);
    m->base = base;
    m->size = image_size;
    m->entry_rva = entry_rva;                     // (read before the file's copy was freed)
    m->export_rva = dirs[0].va; m->export_size = dirs[0].size;
    m->pdata_rva = dirs[3].va; m->pdata_size = dirs[3].size;
    m->tls_rva = dirs[9].size ? dirs[9].va : 0;
    m->tls_index = ~0u;
    m->is_dll = (characteristics & 0x2000) != 0;
    m->refs = 1;
    if (is_main) {
        vx_proc.image_base = base;
        vx_proc.image_size = image_size;
        *(void **)(g_peb + PEB_IMAGE_BASE) = base;
    }
    DWORD reused_tls = ~0u;
    if (reuse >= 0) {
        reused_tls = g_images[reuse]->tls_index;
        // (the old entry itself is kept, not freed: a thread unwinding through that code may still hold it)
        __atomic_store_n(&g_images[reuse], m, __ATOMIC_RELEASE);
    } else {
        if (g_image_count == MAX_IMAGES) { vx_log("too many images"); return NULL; }
        g_images[g_image_count] = m;
        __atomic_store_n(&g_image_count, g_image_count + 1, __ATOMIC_RELEASE);   // visible before imports load (cycles)
    }
    if (vx_trace) vx_log("%s mapped at %p%s", m->name, base, reuse >= 0 ? " (again, where its unloaded copy was)" : "");
#ifdef __aarch64__
    if (aot_hash) {
        char lower[sizeof m->name], mod[4096];
        size_t k = 0;
        for (; m->name[k] && k + 1 < sizeof lower; k++) lower[k] = (char)tolower((unsigned char)m->name[k]);
        lower[k] = 0;
        snprintf(mod, sizeof mod, "%s/%s-%016llx.dylib", getenv("VXRT_AOT_DIR"), lower, (unsigned long long)aot_hash);
        if (!access(mod, R_OK)) xa_load(mod, (uint64_t)(uintptr_t)base, image_size, aot_hash);
        else {
            // (a large image's module comes in parts: <name>-<hash>.p<k>.dylib)
            int parts = 0;
            for (;; parts++) {
                snprintf(mod, sizeof mod, "%s/%s-%016llx.p%d.dylib", getenv("VXRT_AOT_DIR"), lower, (unsigned long long)aot_hash, parts);
                if (access(mod, R_OK) || xa_load(mod, (uint64_t)(uintptr_t)base, image_size, aot_hash)) break;
            }
            if (parts) xa_link(aot_hash);
            else if (vx_trace) vx_log("aot: no module %s", mod);
        }
    }
#endif

    // TLS: an index for this image (its unloaded copy's, when it had one) and a block on every thread that exists
    // already.
    if (m->tls_rva) {
        if (reused_tls == ~0u && g_next_tls_index >= MAX_TLS_MODULES) { vx_log("%s: too many images with TLS", m->name); return NULL; }
        m->tls_index = reused_tls != ~0u ? reused_tls : g_next_tls_index++;
        *(uint32_t *)rd64(base + m->tls_rva + 16) = m->tls_index;
        pthread_mutex_lock(&g_tebs_lock);
        for (int i = 0; i < g_teb_count; i++) {
            void **slot = &(*(void ***)(g_tebs[i] + TEB_TLS_POINTER))[m->tls_index];
            free(*slot);
            *slot = tls_block(m);
        }
        pthread_mutex_unlock(&g_tebs_lock);
    }

    // Imports. A dependency that is a PE file is loaded (and initialized) first.
    int missing = 0;
    if (dirs[1].size) {
        for (const uint8_t *d = base + dirs[1].va; rd32(d + 12); d += 20) {
            const char *dll = (const char *)base + rd32(d + 12);
            uint64_t *lookup = (uint64_t *)(base + (rd32(d) ? rd32(d) : rd32(d + 16)));
            uint64_t *iat = (uint64_t *)(base + rd32(d + 16));
            if (!is_builtin(dll) && !image_by_name(dll) && !load_dll(dll)) vx_log("%s: DLL %s not found", m->name, dll);
            vx_image *dep = image_by_name(dll);
            if (dep) dep->pinned = 1;
            if (dep && dep != m && !init_dll(dep)) return NULL;
            for (size_t i = 0; lookup[i]; i++) {
                const char *name = NULL;
                int ordinal = 0;
                char label[16];
                if (lookup[i] >> 63) { ordinal = (int)(lookup[i] & 0xffff); snprintf(label, sizeof label, "#%d", ordinal); }
                else name = (const char *)base + (lookup[i] & 0x7fffffff) + 2;
                void *fn = resolve(dll, name, ordinal, 0);
                if (fn && !image_by_name(dll)) fn = vx_trace_wrap(dll, name ? name : label, fn);   // VXRT_TRACE_CALLS
                if (!fn) { fn = make_stub(dll, name ? name : label); missing++; if (vx_trace) vx_log("missing %s!%s", dll, name ? name : label); }
                iat[i] = (uint64_t)fn;
            }
        }
    }
    if (missing) vx_log("%s: %d imported functions are not implemented yet (stubs stop the program if called)", m->name, missing);
    return m;
}

// ---- module API (kernel32) -----------------------------------------------------------------------------------------------
#define LOAD_WITH_ALTERED_SEARCH_PATH 0x8
#define LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR 0x100            // (its dependencies: the DLL's own directory first, too)
static void *load_library_searching(const char *name, DWORD flags);
// The call's search flags hold while it loads the DLL and what it imports (LoadLibrary from a DllMain meanwhile: its own).
// LOAD_LIBRARY_SEARCH_SYSTEM32_NO_FORWARDER (0x4000) searches system32 as LOAD_LIBRARY_SEARCH_SYSTEM32 does (never the
// standard search). LoadLibraryEx refuses LOAD_WITH_ALTERED_SEARCH_PATH with any LOAD_LIBRARY_SEARCH_* flag, and
// LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR for a name that is not a fully qualified path (ERROR_INVALID_PARAMETER).
static int fully_qualified(const char *name) {
    int sep0 = name[0] == '\\' || name[0] == '/', sep1 = name[0] && (name[1] == '\\' || name[1] == '/');
    int letter = (name[0] | 0x20) >= 'a' && (name[0] | 0x20) <= 'z';
    return (sep0 && sep1) || (letter && name[1] == ':' && (name[2] == '\\' || name[2] == '/'));   // (UNC, or drive and root)
}
static void *load_library(const char *name, DWORD flags) {
    if (!name || ((flags & LOAD_WITH_ALTERED_SEARCH_PATH) && (flags & 0x5f00)) ||
        ((flags & LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR) && !fully_qualified(name))) {
        vx_set_last_error(87);
        return NULL;
    }
    pthread_mutex_lock(&g_loader_lock);
    DWORD saved = g_search_flags;
    g_search_flags = (flags & 0x1f00) | (flags & 0x4000 ? 0x800 : 0);
    void *h = load_library_searching(name, flags);
    g_search_flags = saved;
    pthread_mutex_unlock(&g_loader_lock);
    return h;
}
static void *load_library_searching(const char *name, DWORD flags) {
    if (vx_trace) vx_log("LoadLibrary %s", name);
    pthread_mutex_lock(&g_loader_lock);
    vx_image *m = NULL;
    void *h = NULL;
    char preferred[4096];
    if ((strchr(name, '\\') || strchr(name, '/')) && preferred_dll(name, preferred, sizeof preferred)) {
        const char *base = name;                                     // (a full path to a DLL that VXRT_DLL_FIRST replaces)
        for (const char *c = name; *c; c++) if (*c == '\\' || *c == '/') base = c + 1;
        if (!(m = image_by_name(base))) m = load_pe(preferred, 0);
        if (m) { m->refs++; if (!init_dll(m)) m = NULL; else h = m->base; }
        pthread_mutex_unlock(&g_loader_lock);
        return h;
    }
    if (strchr(name, '\\') || strchr(name, '/')) {
        char path[4096], saved[4096];
        if (vx_unix_path(name, path, sizeof path)) {
            m = image_by_name(name);
            snprintf(saved, sizeof saved, "%s", g_altered_dir);
            if (flags & (LOAD_WITH_ALTERED_SEARCH_PATH | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR)) {
                snprintf(g_altered_dir, sizeof g_altered_dir, "%s", path);
                char *slash = strrchr(g_altered_dir, '/');
                if (slash) *slash = 0;
            }
            struct stat st;
            const char *base = strrchr(name, '\\') ? strrchr(name, '\\') + 1 : strrchr(name, '/') ? strrchr(name, '/') + 1 : name;
            if (!m && stat(path, &st) && is_builtin(base)) {          // a system DLL this runtime provides, by its full path
                snprintf(g_altered_dir, sizeof g_altered_dir, "%s", saved);
                pthread_mutex_unlock(&g_loader_lock);
                return builtin_handle(base);
            }
            if (!m && stat(path, &st)) vx_set_last_error(126 /* ERROR_MOD_NOT_FOUND */);   // quietly, like Windows
            else if (!m) m = load_pe(path, 0);
            if (m) { m->refs++; if (!init_dll(m)) m = NULL; else h = m->base; }
            snprintf(g_altered_dir, sizeof g_altered_dir, "%s", saved);
            pthread_mutex_unlock(&g_loader_lock);
            return h;
        }
    } else if (!(m = image_by_name(name)) && is_builtin(name)) {
        h = builtin_handle(name);
    } else if (!m) {
        m = load_dll(name);
        if (!m) vx_set_last_error(126 /* ERROR_MOD_NOT_FOUND */);
    }
    if (m) {
        m->refs++;
        if (!init_dll(m)) m = NULL;
        else h = m->base;
    }
    pthread_mutex_unlock(&g_loader_lock);
    return h;
}
static WINAPI void *LoadLibraryA(const char *name) { return load_library(name, 0); }
static WINAPI void *LoadLibraryW(const WCHAR *name) { char *a = vx_utf8_dup(name); void *h = load_library(a, 0); free(a); return h; }
static WINAPI void *LoadLibraryExA(const char *name, HANDLE file, DWORD flags) { (void)file; return load_library(name, flags); }
static WINAPI void *LoadLibraryExW(const WCHAR *name, HANDLE file, DWORD flags) {
    (void)file; char *a = vx_utf8_dup(name); void *h = load_library(a, flags); free(a); return h;
}
static BOOL set_dll_directory(const char *dir) {
    char path[4096] = { 0 };
    if (dir && *dir && !vx_unix_path(dir, path, sizeof path)) return 0;
    pthread_mutex_lock(&g_loader_lock);
    snprintf(g_dll_dir, sizeof g_dll_dir, "%s", path);
    pthread_mutex_unlock(&g_loader_lock);
    return 1;
}
static WINAPI BOOL SetDllDirectoryA(const char *dir) { return set_dll_directory(dir); }
static WINAPI BOOL SetDllDirectoryW(const WCHAR *dir) {
    char *a = dir ? vx_utf8_dup(dir) : NULL; BOOL ok = set_dll_directory(a); free(a); return ok;
}
// The last FreeLibrary of a DLL only LoadLibrary brought in detaches it (DLL_PROCESS_DETACH) and forgets it, so the
// next LoadLibrary maps and initializes a fresh copy, as on Windows: openvr_api.dll unloads vrclient_x64.dll in
// VR_Shutdown, and vrclient asserts if its statics survive into the next VR_Init (Unity initializes OpenVR twice).
// The image stays mapped: code from it may still be running on other threads.
static WINAPI BOOL FreeLibrary(void *h) {
    pthread_mutex_lock(&g_loader_lock);
    vx_image *m = image_by_base(h);
    if (m && m->refs > 1 && --m->refs == 1 && m->is_dll && !m->pinned) {
        if (vx_trace) vx_log("FreeLibrary: %s unloaded", m->name);
        if (m->initialized && m->entry_rva) ((dll_entry)(m->base + m->entry_rva))(m->base, 0 /* DLL_PROCESS_DETACH */, NULL);
        tls_callbacks(m, 0);
        m->unloaded = 1;
    }
    pthread_mutex_unlock(&g_loader_lock);
    return 1;
}
static WINAPI BOOL DisableThreadLibraryCalls(void *h) {
    vx_image *m = image_by_base(h);
    if (m) m->no_thread_calls = 1;
    return m != NULL;
}

static void *module_handle(const char *name) {
    if (!name) return vx_proc.image_base;
    pthread_mutex_lock(&g_loader_lock);
    vx_image *m = image_by_name(name);
    void *h = m ? m->base : is_builtin(name) ? builtin_handle(name) : NULL;
    pthread_mutex_unlock(&g_loader_lock);
    if (!h) vx_set_last_error(126);
    return h;
}
static WINAPI void *GetModuleHandleA(const char *name) { return module_handle(name); }
static WINAPI void *GetModuleHandleW(const WCHAR *name) { char *a = vx_utf8_dup(name); void *h = module_handle(a); free(a); return h; }
#define GET_MODULE_HANDLE_EX_FLAG_PIN 1
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 2
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 4
// Like LoadLibrary, it holds a reference unless asked not to (DLLs pin themselves this way: the CRT, vrclient).
static WINAPI BOOL GetModuleHandleExA(DWORD flags, const char *name, void **out) {
    if (flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) {
        const vx_image *m = vx_image_at((uint64_t)name);
        *out = m && !m->unloaded ? m->base : NULL;
    } else {
        *out = module_handle(name);
    }
    if (!*out) { vx_set_last_error(126); return 0; }
    pthread_mutex_lock(&g_loader_lock);
    vx_image *m = image_by_base(*out);
    if (m && (flags & GET_MODULE_HANDLE_EX_FLAG_PIN)) m->pinned = 1;
    else if (m && !(flags & GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT)) m->refs++;
    pthread_mutex_unlock(&g_loader_lock);
    return 1;
}
static WINAPI BOOL GetModuleHandleExW(DWORD flags, const WCHAR *name, void **out) {
    if (flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) return GetModuleHandleExA(flags, (const char *)name, out);
    char *a = vx_utf8_dup(name);
    BOOL r = GetModuleHandleExA(flags, a, out);
    free(a);
    return r;
}


// ---- VXRT_TRACE_OPENVR=1: what a program asks of OpenVR's compositor ------------------------------------------------------
// openvr_api.dll's VR_GetGenericInterface is wrapped: each interface asked for is logged with its error, and on an
// IVRCompositor the first methods every version has kept (WaitGetPoses 2, Submit 5, PostPresentHandoff 7) are
// patched in its vtable to log their results: the first 20 calls, then every error and every 1000th call.
typedef int (WINAPI *ovr_wait_fn)(void *, void *, uint32_t, void *, uint32_t);
typedef int (WINAPI *ovr_submit_fn)(void *, int, const void *, const float *, int);
typedef void (WINAPI *ovr_handoff_fn)(void *);
static void *(WINAPI *g_ovr_get_interface)(const char *, int *);
static ovr_wait_fn g_ovr_wait; static ovr_submit_fn g_ovr_submit; static ovr_handoff_fn g_ovr_handoff;
static int ovr_note(unsigned *count, int error) { unsigned n = __sync_add_and_fetch(count, 1); return n <= 20 || error || n % 1000 == 0 ? (int)n : 0; }

#if defined(__x86_64__) && !defined(VX_ABI_SCAN)   // (x86 only: tools/vx_abi_scan.py makes no ARM64 thunks for these)
// The compositor's other methods (8 to 39, those that point into vrclient) are counted by thunks that only bump a
// counter and jump on, whatever their arguments; the counts are logged with every 200th WaitGetPoses.
static volatile int g_ovr_counts[40];
static void *g_ovr_orig[40];
__attribute__((naked)) static void ovr_count_8(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovr_counts[8]), "m"(g_ovr_orig[8])); }
__attribute__((naked)) static void ovr_count_9(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovr_counts[9]), "m"(g_ovr_orig[9])); }
__attribute__((naked)) static void ovr_count_10(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovr_counts[10]), "m"(g_ovr_orig[10])); }
__attribute__((naked)) static void ovr_count_11(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovr_counts[11]), "m"(g_ovr_orig[11])); }
__attribute__((naked)) static void ovr_count_12(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovr_counts[12]), "m"(g_ovr_orig[12])); }
__attribute__((naked)) static void ovr_count_13(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovr_counts[13]), "m"(g_ovr_orig[13])); }
__attribute__((naked)) static void ovr_count_14(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovr_counts[14]), "m"(g_ovr_orig[14])); }
__attribute__((naked)) static void ovr_count_15(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovr_counts[15]), "m"(g_ovr_orig[15])); }
__attribute__((naked)) static void ovr_count_16(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovr_counts[16]), "m"(g_ovr_orig[16])); }
__attribute__((naked)) static void ovr_count_17(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovr_counts[17]), "m"(g_ovr_orig[17])); }
__attribute__((naked)) static void ovr_count_18(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovr_counts[18]), "m"(g_ovr_orig[18])); }
__attribute__((naked)) static void ovr_count_19(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovr_counts[19]), "m"(g_ovr_orig[19])); }
__attribute__((naked)) static void ovr_count_20(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovr_counts[20]), "m"(g_ovr_orig[20])); }
__attribute__((naked)) static void ovr_count_21(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovr_counts[21]), "m"(g_ovr_orig[21])); }
__attribute__((naked)) static void ovr_count_22(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovr_counts[22]), "m"(g_ovr_orig[22])); }
__attribute__((naked)) static void ovr_count_23(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovr_counts[23]), "m"(g_ovr_orig[23])); }
__attribute__((naked)) static void ovr_count_24(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovr_counts[24]), "m"(g_ovr_orig[24])); }
__attribute__((naked)) static void ovr_count_25(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovr_counts[25]), "m"(g_ovr_orig[25])); }
__attribute__((naked)) static void ovr_count_26(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovr_counts[26]), "m"(g_ovr_orig[26])); }
__attribute__((naked)) static void ovr_count_27(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovr_counts[27]), "m"(g_ovr_orig[27])); }
__attribute__((naked)) static void ovr_count_28(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovr_counts[28]), "m"(g_ovr_orig[28])); }
__attribute__((naked)) static void ovr_count_29(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovr_counts[29]), "m"(g_ovr_orig[29])); }
__attribute__((naked)) static void ovr_count_30(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovr_counts[30]), "m"(g_ovr_orig[30])); }
__attribute__((naked)) static void ovr_count_31(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovr_counts[31]), "m"(g_ovr_orig[31])); }
__attribute__((naked)) static void ovr_count_32(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovr_counts[32]), "m"(g_ovr_orig[32])); }
__attribute__((naked)) static void ovr_count_33(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovr_counts[33]), "m"(g_ovr_orig[33])); }
__attribute__((naked)) static void ovr_count_34(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovr_counts[34]), "m"(g_ovr_orig[34])); }
__attribute__((naked)) static void ovr_count_35(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovr_counts[35]), "m"(g_ovr_orig[35])); }
__attribute__((naked)) static void ovr_count_36(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovr_counts[36]), "m"(g_ovr_orig[36])); }
__attribute__((naked)) static void ovr_count_37(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovr_counts[37]), "m"(g_ovr_orig[37])); }
__attribute__((naked)) static void ovr_count_38(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovr_counts[38]), "m"(g_ovr_orig[38])); }
__attribute__((naked)) static void ovr_count_39(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovr_counts[39]), "m"(g_ovr_orig[39])); }
static void (*const g_ovr_counters[40])(void) = { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, ovr_count_8, ovr_count_9, ovr_count_10, ovr_count_11, ovr_count_12, ovr_count_13, ovr_count_14, ovr_count_15, ovr_count_16, ovr_count_17, ovr_count_18, ovr_count_19, ovr_count_20, ovr_count_21, ovr_count_22, ovr_count_23, ovr_count_24, ovr_count_25, ovr_count_26, ovr_count_27, ovr_count_28, ovr_count_29, ovr_count_30, ovr_count_31, ovr_count_32, ovr_count_33, ovr_count_34, ovr_count_35, ovr_count_36, ovr_count_37, ovr_count_38, ovr_count_39 };
static WINAPI int ovr_skip_skybox(void *this, const void *textures, uint32_t count) {
    (void)this; (void)textures;
    vx_log("openvr: SetSkyboxOverride with %u textures skipped (VXRT_OPENVR_NO_SKYBOX)", count);
    return 0;
}
static void ovr_patch_rest(void **vtbl, void *any_method) {
    const vx_image *owner = vx_image_at((uint64_t)(uintptr_t)any_method);
    for (int i = 8; i < 40 && owner; i++) {
        if (vx_image_at((uint64_t)(uintptr_t)vtbl[i]) != owner) break;      // past the vtable's end
        g_ovr_orig[i] = vtbl[i]; vtbl[i] = (void *)g_ovr_counters[i];
    }
    if (getenv("VXRT_OPENVR_NO_SKYBOX") && g_ovr_orig[12]) vtbl[12] = (void *)ovr_skip_skybox;   // (014: SetSkyboxOverride)
}

#if defined(__x86_64__) && !defined(VX_ABI_SCAN)
// IVROverlay: FindOverlay (0) and CreateOverlay (1) are logged with their keys; the rest (2 to 89) are counted.
static volatile int g_ovl_counts[90];
static void *g_ovl_orig[90];
__attribute__((naked)) static void ovl_count_2(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[2]), "m"(g_ovl_orig[2])); }
__attribute__((naked)) static void ovl_count_3(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[3]), "m"(g_ovl_orig[3])); }
__attribute__((naked)) static void ovl_count_4(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[4]), "m"(g_ovl_orig[4])); }
__attribute__((naked)) static void ovl_count_5(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[5]), "m"(g_ovl_orig[5])); }
__attribute__((naked)) static void ovl_count_6(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[6]), "m"(g_ovl_orig[6])); }
__attribute__((naked)) static void ovl_count_7(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[7]), "m"(g_ovl_orig[7])); }
__attribute__((naked)) static void ovl_count_8(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[8]), "m"(g_ovl_orig[8])); }
__attribute__((naked)) static void ovl_count_9(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[9]), "m"(g_ovl_orig[9])); }
__attribute__((naked)) static void ovl_count_10(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[10]), "m"(g_ovl_orig[10])); }
__attribute__((naked)) static void ovl_count_11(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[11]), "m"(g_ovl_orig[11])); }
__attribute__((naked)) static void ovl_count_12(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[12]), "m"(g_ovl_orig[12])); }
__attribute__((naked)) static void ovl_count_13(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[13]), "m"(g_ovl_orig[13])); }
__attribute__((naked)) static void ovl_count_14(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[14]), "m"(g_ovl_orig[14])); }
__attribute__((naked)) static void ovl_count_15(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[15]), "m"(g_ovl_orig[15])); }
__attribute__((naked)) static void ovl_count_16(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[16]), "m"(g_ovl_orig[16])); }
__attribute__((naked)) static void ovl_count_17(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[17]), "m"(g_ovl_orig[17])); }
__attribute__((naked)) static void ovl_count_18(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[18]), "m"(g_ovl_orig[18])); }
__attribute__((naked)) static void ovl_count_19(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[19]), "m"(g_ovl_orig[19])); }
__attribute__((naked)) static void ovl_count_20(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[20]), "m"(g_ovl_orig[20])); }
__attribute__((naked)) static void ovl_count_21(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[21]), "m"(g_ovl_orig[21])); }
__attribute__((naked)) static void ovl_count_22(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[22]), "m"(g_ovl_orig[22])); }
__attribute__((naked)) static void ovl_count_23(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[23]), "m"(g_ovl_orig[23])); }
__attribute__((naked)) static void ovl_count_24(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[24]), "m"(g_ovl_orig[24])); }
__attribute__((naked)) static void ovl_count_25(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[25]), "m"(g_ovl_orig[25])); }
__attribute__((naked)) static void ovl_count_26(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[26]), "m"(g_ovl_orig[26])); }
__attribute__((naked)) static void ovl_count_27(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[27]), "m"(g_ovl_orig[27])); }
__attribute__((naked)) static void ovl_count_28(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[28]), "m"(g_ovl_orig[28])); }
__attribute__((naked)) static void ovl_count_29(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[29]), "m"(g_ovl_orig[29])); }
__attribute__((naked)) static void ovl_count_30(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[30]), "m"(g_ovl_orig[30])); }
__attribute__((naked)) static void ovl_count_31(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[31]), "m"(g_ovl_orig[31])); }
__attribute__((naked)) static void ovl_count_32(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[32]), "m"(g_ovl_orig[32])); }
__attribute__((naked)) static void ovl_count_33(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[33]), "m"(g_ovl_orig[33])); }
__attribute__((naked)) static void ovl_count_34(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[34]), "m"(g_ovl_orig[34])); }
__attribute__((naked)) static void ovl_count_35(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[35]), "m"(g_ovl_orig[35])); }
__attribute__((naked)) static void ovl_count_36(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[36]), "m"(g_ovl_orig[36])); }
__attribute__((naked)) static void ovl_count_37(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[37]), "m"(g_ovl_orig[37])); }
__attribute__((naked)) static void ovl_count_38(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[38]), "m"(g_ovl_orig[38])); }
__attribute__((naked)) static void ovl_count_39(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[39]), "m"(g_ovl_orig[39])); }
__attribute__((naked)) static void ovl_count_40(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[40]), "m"(g_ovl_orig[40])); }
__attribute__((naked)) static void ovl_count_41(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[41]), "m"(g_ovl_orig[41])); }
__attribute__((naked)) static void ovl_count_42(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[42]), "m"(g_ovl_orig[42])); }
__attribute__((naked)) static void ovl_count_43(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[43]), "m"(g_ovl_orig[43])); }
__attribute__((naked)) static void ovl_count_44(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[44]), "m"(g_ovl_orig[44])); }
__attribute__((naked)) static void ovl_count_45(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[45]), "m"(g_ovl_orig[45])); }
__attribute__((naked)) static void ovl_count_46(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[46]), "m"(g_ovl_orig[46])); }
__attribute__((naked)) static void ovl_count_47(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[47]), "m"(g_ovl_orig[47])); }
__attribute__((naked)) static void ovl_count_48(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[48]), "m"(g_ovl_orig[48])); }
__attribute__((naked)) static void ovl_count_49(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[49]), "m"(g_ovl_orig[49])); }
__attribute__((naked)) static void ovl_count_50(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[50]), "m"(g_ovl_orig[50])); }
__attribute__((naked)) static void ovl_count_51(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[51]), "m"(g_ovl_orig[51])); }
__attribute__((naked)) static void ovl_count_52(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[52]), "m"(g_ovl_orig[52])); }
__attribute__((naked)) static void ovl_count_53(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[53]), "m"(g_ovl_orig[53])); }
__attribute__((naked)) static void ovl_count_54(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[54]), "m"(g_ovl_orig[54])); }
__attribute__((naked)) static void ovl_count_55(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[55]), "m"(g_ovl_orig[55])); }
__attribute__((naked)) static void ovl_count_56(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[56]), "m"(g_ovl_orig[56])); }
__attribute__((naked)) static void ovl_count_57(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[57]), "m"(g_ovl_orig[57])); }
__attribute__((naked)) static void ovl_count_58(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[58]), "m"(g_ovl_orig[58])); }
__attribute__((naked)) static void ovl_count_59(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[59]), "m"(g_ovl_orig[59])); }
__attribute__((naked)) static void ovl_count_60(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[60]), "m"(g_ovl_orig[60])); }
__attribute__((naked)) static void ovl_count_61(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[61]), "m"(g_ovl_orig[61])); }
__attribute__((naked)) static void ovl_count_62(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[62]), "m"(g_ovl_orig[62])); }
__attribute__((naked)) static void ovl_count_63(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[63]), "m"(g_ovl_orig[63])); }
__attribute__((naked)) static void ovl_count_64(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[64]), "m"(g_ovl_orig[64])); }
__attribute__((naked)) static void ovl_count_65(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[65]), "m"(g_ovl_orig[65])); }
__attribute__((naked)) static void ovl_count_66(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[66]), "m"(g_ovl_orig[66])); }
__attribute__((naked)) static void ovl_count_67(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[67]), "m"(g_ovl_orig[67])); }
__attribute__((naked)) static void ovl_count_68(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[68]), "m"(g_ovl_orig[68])); }
__attribute__((naked)) static void ovl_count_69(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[69]), "m"(g_ovl_orig[69])); }
__attribute__((naked)) static void ovl_count_70(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[70]), "m"(g_ovl_orig[70])); }
__attribute__((naked)) static void ovl_count_71(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[71]), "m"(g_ovl_orig[71])); }
__attribute__((naked)) static void ovl_count_72(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[72]), "m"(g_ovl_orig[72])); }
__attribute__((naked)) static void ovl_count_73(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[73]), "m"(g_ovl_orig[73])); }
__attribute__((naked)) static void ovl_count_74(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[74]), "m"(g_ovl_orig[74])); }
__attribute__((naked)) static void ovl_count_75(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[75]), "m"(g_ovl_orig[75])); }
__attribute__((naked)) static void ovl_count_76(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[76]), "m"(g_ovl_orig[76])); }
__attribute__((naked)) static void ovl_count_77(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[77]), "m"(g_ovl_orig[77])); }
__attribute__((naked)) static void ovl_count_78(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[78]), "m"(g_ovl_orig[78])); }
__attribute__((naked)) static void ovl_count_79(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[79]), "m"(g_ovl_orig[79])); }
__attribute__((naked)) static void ovl_count_80(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[80]), "m"(g_ovl_orig[80])); }
__attribute__((naked)) static void ovl_count_81(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[81]), "m"(g_ovl_orig[81])); }
__attribute__((naked)) static void ovl_count_82(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[82]), "m"(g_ovl_orig[82])); }
__attribute__((naked)) static void ovl_count_83(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[83]), "m"(g_ovl_orig[83])); }
__attribute__((naked)) static void ovl_count_84(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[84]), "m"(g_ovl_orig[84])); }
__attribute__((naked)) static void ovl_count_85(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[85]), "m"(g_ovl_orig[85])); }
__attribute__((naked)) static void ovl_count_86(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[86]), "m"(g_ovl_orig[86])); }
__attribute__((naked)) static void ovl_count_87(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[87]), "m"(g_ovl_orig[87])); }
__attribute__((naked)) static void ovl_count_88(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[88]), "m"(g_ovl_orig[88])); }
__attribute__((naked)) static void ovl_count_89(void) { __asm__ volatile("lock incl %0\n\tjmp *%1" :: "m"(g_ovl_counts[89]), "m"(g_ovl_orig[89])); }
static void (*const g_ovl_counters[90])(void) = { NULL, NULL, ovl_count_2, ovl_count_3, ovl_count_4, ovl_count_5, ovl_count_6, ovl_count_7, ovl_count_8, ovl_count_9, ovl_count_10, ovl_count_11, ovl_count_12, ovl_count_13, ovl_count_14, ovl_count_15, ovl_count_16, ovl_count_17, ovl_count_18, ovl_count_19, ovl_count_20, ovl_count_21, ovl_count_22, ovl_count_23, ovl_count_24, ovl_count_25, ovl_count_26, ovl_count_27, ovl_count_28, ovl_count_29, ovl_count_30, ovl_count_31, ovl_count_32, ovl_count_33, ovl_count_34, ovl_count_35, ovl_count_36, ovl_count_37, ovl_count_38, ovl_count_39, ovl_count_40, ovl_count_41, ovl_count_42, ovl_count_43, ovl_count_44, ovl_count_45, ovl_count_46, ovl_count_47, ovl_count_48, ovl_count_49, ovl_count_50, ovl_count_51, ovl_count_52, ovl_count_53, ovl_count_54, ovl_count_55, ovl_count_56, ovl_count_57, ovl_count_58, ovl_count_59, ovl_count_60, ovl_count_61, ovl_count_62, ovl_count_63, ovl_count_64, ovl_count_65, ovl_count_66, ovl_count_67, ovl_count_68, ovl_count_69, ovl_count_70, ovl_count_71, ovl_count_72, ovl_count_73, ovl_count_74, ovl_count_75, ovl_count_76, ovl_count_77, ovl_count_78, ovl_count_79, ovl_count_80, ovl_count_81, ovl_count_82, ovl_count_83, ovl_count_84, ovl_count_85, ovl_count_86, ovl_count_87, ovl_count_88, ovl_count_89 };
static WINAPI int ovl_find(void *this, const char *key, uint64_t *handle) {
    int r = ((int (WINAPI *)(void *, const char *, uint64_t *))g_ovl_orig[0])(this, key, handle);
    vx_log("openvr: FindOverlay(%s) = %d, handle %#llx", key ? key : "?", r, handle ? (unsigned long long)*handle : 0ull);
    return r;
}
static WINAPI int ovl_create(void *this, const char *key, const char *name, uint64_t *handle) {
    int r = ((int (WINAPI *)(void *, const char *, const char *, uint64_t *))g_ovl_orig[1])(this, key, name, handle);
    vx_log("openvr: CreateOverlay(%s, \"%s\") = %d, handle %#llx", key ? key : "?", name ? name : "?", r, handle ? (unsigned long long)*handle : 0ull);
    return r;
}
static void ovl_patch(void **vtbl) {
    const vx_image *owner = vx_image_at((uint64_t)(uintptr_t)vtbl[0]);
    long page = sysconf(_SC_PAGESIZE);
    uintptr_t start = (uintptr_t)vtbl & ~(uintptr_t)(page - 1), end = ((uintptr_t)(vtbl + 90) + page - 1) & ~(uintptr_t)(page - 1);
    if (!owner || mprotect((void *)start, end - start, PROT_READ | PROT_WRITE)) { vx_log("openvr: cannot patch the overlay vtable"); return; }
    g_ovl_orig[0] = vtbl[0]; g_ovl_orig[1] = vtbl[1];
    vtbl[0] = (void *)ovl_find; vtbl[1] = (void *)ovl_create;
    int i = 2;
    for (; i < 90 && vx_image_at((uint64_t)(uintptr_t)vtbl[i]) == owner; i++) { g_ovl_orig[i] = vtbl[i]; vtbl[i] = (void *)g_ovl_counters[i]; }
    vx_log("openvr: tracing the overlay interface (%d methods)", i);
}
static void ovl_log_counts(void) {
    char text[900]; size_t len = 0;
    for (int i = 2; i < 90; i++) if (g_ovl_counts[i]) len += (size_t)snprintf(text + len, sizeof text - len, " [%d]=%d", i, g_ovl_counts[i]);
    vx_log("openvr: overlay methods called:%s", len ? text : " none");
}
#else
static void ovl_patch(void **vtbl) { (void)vtbl; }
static void ovl_log_counts(void) {}
#endif
static void *g_ovr_this;
static void ovr_log_counts(void) {
    char text[600]; size_t len = 0;
    for (int i = 8; i < 40; i++) if (g_ovr_counts[i]) len += (size_t)snprintf(text + len, sizeof text - len, " [%d]=%d", i, g_ovr_counts[i]);
    vx_log("openvr: other compositor methods called:%s", len ? text : " none");
    ovl_log_counts();
    // IVRCompositor_014 (SetSkyboxOverride is 12): GetCurrentSceneFocusProcess 18, GetLastFrameRenderer 19, CanRenderScene 20
    if (g_ovr_this && g_ovr_orig[18] && g_ovr_orig[19] && g_ovr_orig[20]) {
        uint32_t focus = ((uint32_t (WINAPI *)(void *))g_ovr_orig[18])(g_ovr_this);
        uint32_t last = ((uint32_t (WINAPI *)(void *))g_ovr_orig[19])(g_ovr_this);
        int can = ((uint8_t (WINAPI *)(void *))g_ovr_orig[20])(g_ovr_this);
        vx_log("openvr: scene focus process %u, last frame renderer %u, can render scene %d (this process %d)", focus, last, can, (int)getpid());
    }
    // GetFrameTiming (8): the struct's size is the version; the first size the client takes is the one it knows.
    if (g_ovr_this && g_ovr_orig[8]) {
        static uint32_t known;
        uint32_t timing[128];
        for (uint32_t size = known ? known : 400; size >= 16; size -= 4) {           // the largest size it takes
            memset(timing, 0, sizeof timing); timing[0] = size;
            if (((uint8_t (WINAPI *)(void *, void *, uint32_t))g_ovr_orig[8])(g_ovr_this, timing, 0)) { known = size; break; }
            if (known) break;
        }
        if (known) {
            char text[1400]; size_t len = 0;
            for (uint32_t i = 0; i < known / 4 && i < 60; i++) {
                float f; memcpy(&f, &timing[i], 4);
                if (i < 4 || timing[i] < 0x1000000) len += (size_t)snprintf(text + len, sizeof text - len, " %u", timing[i]);
                else len += (size_t)snprintf(text + len, sizeof text - len, " %.2f", f);
            }
            vx_log("openvr: frame timing (%u bytes):%s", known, text);
        } else vx_log("openvr: GetFrameTiming takes no size from 16 to 400");
    }
}
#else
static void ovr_patch_rest(void **vtbl, void *any_method) { (void)vtbl; (void)any_method; }
static void ovr_log_counts(void) {}
static void ovl_patch(void **vtbl) { (void)vtbl; }
#endif
static WINAPI int ovr_wait(void *this, void *render, uint32_t nrender, void *game, uint32_t ngame) {
    static unsigned count;
    int r = g_ovr_wait(this, render, nrender, game, ngame);
#if defined(__x86_64__) && !defined(VX_ABI_SCAN)
    g_ovr_this = this;
#endif
    int n = ovr_note(&count, r);
    if (n) vx_log("openvr: WaitGetPoses #%d (%u render poses, %u game poses) = %d", n, nrender, ngame, r);
    if ((count <= 3 || count % 100 == 0) && render && nrender) {   // TrackedDevicePose_t: a 3x4 matrix, velocities, result, valid
        const float *m = render; const uint8_t *b = render;
        vx_log("openvr: HMD render pose #%u: position %.3f %.3f %.3f, row0 %.3f %.3f %.3f, result %d, valid %d, connected %d",
               count, m[3], m[7], m[11], m[0], m[1], m[2], *(const int *)(b + 72), b[76], b[77]);
    }
    if (count % 200 == 0) ovr_log_counts();
    return r;
}
static WINAPI int ovr_submit(void *this, int eye, const void *texture, const float *bounds, int flags) {
    static unsigned count;
    int r = g_ovr_submit(this, eye, texture, bounds, flags);
    int n = ovr_note(&count, r);
    if (n) {
        const int *t = texture ? (const int *)((const uint8_t *)texture + 8) : NULL;   // Texture_t: handle, type, colour space
        vx_log("openvr: Submit #%d eye %d texture %p (type %d, colour space %d) bounds %s%.3f %.3f %.3f %.3f flags %#x = %d", n, eye,
               texture ? *(void *const *)texture : NULL, t ? t[0] : -1, t ? t[1] : -1, bounds ? "" : "(none) ",
               bounds ? bounds[0] : 0, bounds ? bounds[1] : 0, bounds ? bounds[2] : 0, bounds ? bounds[3] : 0, flags, r);
    }
    return r;
}
static WINAPI void ovr_handoff(void *this) {
    static unsigned count;
    int n = ovr_note(&count, 0);
    if (n) vx_log("openvr: PostPresentHandoff #%d", n);
    g_ovr_handoff(this);
}
static WINAPI void *ovr_get_interface(const char *version, int *error) {
    void *r = g_ovr_get_interface(version, error);
    vx_log("openvr: VR_GetGenericInterface(%s) = %p, error %d", version ? version : "?", r, error ? *error : 0);
    static int overlay_patched;
    if (r && version && !strncmp(version, "IVROverlay_", 11) && !overlay_patched) { overlay_patched = 1; ovl_patch(*(void ***)r); }
    if (r && version && !strncmp(version, "IVRCompositor_", 14) && !g_ovr_submit) {
        void **vtbl = *(void ***)r;
        long page = sysconf(_SC_PAGESIZE);
        uintptr_t start = (uintptr_t)vtbl & ~(uintptr_t)(page - 1), end = ((uintptr_t)(vtbl + 40) + page - 1) & ~(uintptr_t)(page - 1);
        if (!mprotect((void *)start, end - start, PROT_READ | PROT_WRITE)) {
            g_ovr_wait = (ovr_wait_fn)vtbl[2]; g_ovr_submit = (ovr_submit_fn)vtbl[5]; g_ovr_handoff = (ovr_handoff_fn)vtbl[7];
            ovr_patch_rest(vtbl, vtbl[0]);
            vtbl[2] = (void *)ovr_wait; vtbl[5] = (void *)ovr_submit; vtbl[7] = (void *)ovr_handoff;
            vx_log("openvr: tracing %s's WaitGetPoses, Submit and PostPresentHandoff", version);
        } else vx_log("openvr: cannot patch %s's vtable (%s)", version, strerror(errno));
    }
    return r;
}
static WINAPI void *GetProcAddress(void *module, const char *name) {
    int ordinal = (uintptr_t)name < 0x10000 ? (int)(uintptr_t)name : 0;
    if (!module) module = vx_proc.image_base;             // NULL is the program itself
    pthread_mutex_lock(&g_loader_lock);
    vx_image *m = image_by_base(module);
    int builtin = !m && is_builtin_handle(module);
    char by_number[16];
    snprintf(by_number, sizeof by_number, "#%d", ordinal);
    void *fn = m ? pe_export(m, ordinal ? NULL : name, ordinal, 0) : builtin ? vx_find_export(builtin_name(module), ordinal ? by_number : name) : NULL;
    pthread_mutex_unlock(&g_loader_lock);
    if (fn && builtin) fn = vx_trace_wrap(builtin_name(module), ordinal ? by_number : name, fn);
    // only the first module asked is wrapped: with the capture proxy that is the proxy, which itself then asks the
    // real openvr_api for the same export; wrapping that too would send the game's calls past the proxy
    if (fn && m && !ordinal && !strcmp(name, "VR_GetGenericInterface") && getenv("VXRT_TRACE_OPENVR")
        && (!g_ovr_get_interface || g_ovr_get_interface == fn)) {
        g_ovr_get_interface = (void *(WINAPI *)(const char *, int *))fn;
        fn = (void *)ovr_get_interface;
    }
    if (!fn) vx_set_last_error(m || builtin ? 127 /* ERROR_PROC_NOT_FOUND */ : 6 /* ERROR_INVALID_HANDLE */);
    // a miss on one of the runtime's own modules is logged always: a program that calls the result unchecked
    // crashes at address 0, and the name here is what it wanted
    if (!fn && (vx_trace || builtin)) vx_log("GetProcAddress(%s, %s) failed", m ? m->name : builtin ? builtin_name(module) : "?", ordinal ? by_number : name);
    return fn;
}

static const char *module_path(void *module, char *tmp, size_t cap) {
    vx_image *m = image_by_base(module ? module : vx_proc.image_base);
    if (m) return m->windows_path;
    if (!is_builtin_handle(module)) { vx_set_last_error(126); snprintf(tmp, cap, "%s", ""); return tmp; }
    snprintf(tmp, cap, "C:\\windows\\system32\\%s", builtin_name(module));
    return tmp;
}
// GetModuleFileName truncates (and still terminates) when the buffer is too small.
static WINAPI DWORD GetModuleFileNameA(void *module, char *buf, DWORD size) {
    char tmp[300];
    const char *p = module_path(module, tmp, sizeof tmp);
    DWORD n = (DWORD)strlen(p);
    if (!size) return 0;
    if (n >= size) { memcpy(buf, p, size - 1); buf[size - 1] = 0; vx_set_last_error(122); return size; }
    memcpy(buf, p, n + 1);
    return n;
}
static WINAPI DWORD GetModuleFileNameW(void *module, WCHAR *buf, DWORD size) {
    char tmp[300];
    const char *p = module_path(module, tmp, sizeof tmp);
    if (!size) return 0;
    size_t n = vx_utf8_to_utf16(p, strlen(p), buf, size);
    if (n >= size) { buf[size - 1] = 0; vx_set_last_error(122); return size; }
    buf[n] = 0;
    return (DWORD)n;
}

static const vx_export loader_k32[] = {
    VX_EXPORT(LoadLibraryA), VX_EXPORT(LoadLibraryW), VX_EXPORT(LoadLibraryExA), VX_EXPORT(LoadLibraryExW),
    VX_EXPORT(FreeLibrary), VX_EXPORT(DisableThreadLibraryCalls), VX_EXPORT(SetDllDirectoryA), VX_EXPORT(SetDllDirectoryW),
    VX_EXPORT(GetModuleHandleA), VX_EXPORT(GetModuleHandleW), VX_EXPORT(GetModuleHandleExA), VX_EXPORT(GetModuleHandleExW),
    VX_EXPORT(GetProcAddress), VX_EXPORT(GetModuleFileNameA), VX_EXPORT(GetModuleFileNameW),
};

// ---- main ------------------------------------------------------------------------------------------------------
int vx_ui_run_main(int (*program)(void *), void *arg, size_t stack);

// The program's main thread: loads it (DLL initializers run here, on a Windows thread) and calls its entry.
static size_t g_main_stack;
static int run_program(void *arg) {
    const char *exe = arg;
#ifdef __aarch64__
    vx_x86_stack_size = g_main_stack;
#endif
    vx_thread_attach();
    int managed_rc = vx_dotnet_run_file(exe);
    if (managed_rc >= 0) return managed_rc;
    pthread_mutex_lock(&g_loader_lock);
    vx_image *m = load_pe(exe, 1);
    pthread_mutex_unlock(&g_loader_lock);
    if (!m) return 1;
    if (vx_trace) vx_log("%s: entry %#x", m->name, m->entry_rva);
    tls_callbacks(m, 1 /* DLL_PROCESS_ATTACH */);
    typedef DWORD (WINAPI *entry_fn)(void);
    DWORD rc = ((entry_fn)(m->base + m->entry_rva))();
    fflush(stdout);
    return (int)rc;
}
// The PEB's process parameters (RTL_USER_PROCESS_PARAMETERS), which statically linked C runtimes read directly:
// flags, standard handles, the current directory, image path, command line and environment as UNICODE_STRINGs
// (length, maximum length, buffer), and the window-show flags; and the PEB's Windows version (10.0.19045) and
// processor count.
typedef struct { uint16_t len, max; uint32_t pad; WCHAR *buf; } unicode_string;
static void set_unicode(uint8_t *at, const char *s) {
    unicode_string *u = (unicode_string *)at;
    u->buf = vx_utf16_dup(s);
    u->len = (uint16_t)(vx_wcslen(u->buf) * 2); u->max = (uint16_t)(u->len + 2);
}
static void make_process_parameters(const char *exe) {
    uint8_t *pp = calloc(1, 0x440);
    *(DWORD *)pp = *(DWORD *)(pp + 4) = 0x440;                 // MaximumLength, Length
    *(DWORD *)(pp + 8) = 1;                                     // RTL_USER_PROC_PARAMS_NORMALIZED
    *(HANDLE *)(pp + 0x20) = (HANDLE)(intptr_t)-10;             // standard input, output, error
    *(HANDLE *)(pp + 0x28) = (HANDLE)(intptr_t)-11;
    *(HANDLE *)(pp + 0x30) = (HANDLE)(intptr_t)-12;
    char cwd[1024], win[1024];
    vx_get_cwd(cwd, sizeof cwd);
    size_t n = strlen(cwd);
    if (n && cwd[n - 1] != '\\' && n + 1 < sizeof cwd) { cwd[n] = '\\'; cwd[n + 1] = 0; }
    set_unicode(pp + 0x38, cwd);                                // CurrentDirectory.DosPath
    vx_windows_path(exe, win, sizeof win);
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", win);
    char *slash = strrchr(dir, '\\');
    if (slash) slash[1] = 0;
    set_unicode(pp + 0x50, dir);                                // DllPath
    set_unicode(pp + 0x60, win);                                // ImagePathName
    unicode_string *cmd = (unicode_string *)(pp + 0x70);       // CommandLine
    cmd->buf = vx_proc.command_line_w;
    size_t cl = vx_wcslen(cmd->buf) * 2; cmd->len = (uint16_t)(cl > 0xfffc ? 0xfffc : cl); cmd->max = (uint16_t)(cmd->len + 2);
    WCHAR *(WINAPI *env)(void) = (WCHAR *(WINAPI *)(void))vx_find_export("kernel32.dll", "GetEnvironmentStringsW");
    *(WCHAR **)(pp + 0x80) = env ? env() : NULL;                // Environment
    *(DWORD *)(pp + 0xa4) = 1;                                  // WindowFlags... ShowWindowFlags: SW_SHOWNORMAL
    set_unicode(pp + 0xb0, win);                                // WindowTitle
    set_unicode(pp + 0xc0, "WinSta0\\Default");                // DesktopInfo
    *(void **)(g_peb + 0x20) = pp;
    *(DWORD *)(g_peb + 0xb8) = (DWORD)sysconf(_SC_NPROCESSORS_ONLN);   // NumberOfProcessors
    *(DWORD *)(g_peb + 0x118) = 10; *(DWORD *)(g_peb + 0x11c) = 0;     // OSMajorVersion, OSMinorVersion
    *(uint16_t *)(g_peb + 0x120) = 19045; *(DWORD *)(g_peb + 0x124) = 0; *(DWORD *)(g_peb + 0x128) = 2;   // build, CSD, VER_PLATFORM_WIN32_NT
}

int main(int argc, char **argv, char **envp) {
    if (argc < 2) { fprintf(stderr, "usage: vxrt program.exe [args...]\n"); return 2; }
    if (argc == 2 && !strcmp(argv[1], "--vxrt-job-watchdog")) return vx_jobs_watchdog();   // (src/jobs.c)
    vx_program_env(argv[1]);                     // (first: the settings it gives are read below and in every init)
    vx_trace = getenv("VXRT_TRACE") && *getenv("VXRT_TRACE") == '1';
    // Windows programs keep thousands of handles open; macOS gives a process 256 descriptors (Google Earth VR ran
    // out while reading its tours): as many as the system allows.
    struct rlimit files;
    if (!getrlimit(RLIMIT_NOFILE, &files) && files.rlim_cur < 65536) {
        files.rlim_cur = files.rlim_max < OPEN_MAX ? files.rlim_max : OPEN_MAX;
        setrlimit(RLIMIT_NOFILE, &files);
    }
#ifdef __aarch64__
    vx_arm64_init();                             // (the engine, before any x86 code)
#endif
    vx_wininet_init();                           // (before desktop.c's stand-ins for the same functions)
    vx_winhttp_init();
    vx_directinput_init();
    vx_msxml_init();
    vx_security_init();                          // (advapi32's security objects: before the tables that follow)
    vx_kernelx_init();                           // (before kernel32.c, desktop.c, client.c, extras.c: replaces stand-ins there)
    vx_tray_init();                              // (before user32.c, desktop.c, services.c, client.c, extras.c: icons and menus there are stand-ins)
    vx_kernel32_init();
    vx_system_init();
    vx_region_init();                            // (before desktop.c and client.c: replaces their region stand-ins)
    vx_version_init();
    vx_tcptable_init();                          // (before client.c: replaces its GetExtendedTcpTable stand-in)
    vx_winprops_init();                          // (shell32's window property stores, propsys, the virtual desktop manager)
    vx_a11y_init();                              // (oleacc, UIAutomationCore, uxtheme)
    vx_mfplat_init();                            // (Media Foundation, DXVA2: not started)
    vx_imagehlp_init();
    vx_spellcheck_init();
    vx_present_init();                           // (before desktop.c, client.c, user32.c, gdi.c, dwmapi.c: replaces stand-ins there)
    vx_ime_init();                               // (before desktop.c, services.c, client.c: imm32 stand-ins there)
    vx_clipboard_init();                         // (before desktop.c, client.c, user32.c)
    vx_inherit_init();
    vx_desktop_init();
    vx_winsta_init();
    vx_scm_init();
    vx_services_init();
    vx_crypt32_init();                           // (before client.c's and extras.c's stand-ins for the same functions)
    vx_dpapi_init();
    vx_client_init();
    vx_steambridge_init();
    vx_pipes_init();
    vx_register_module("kernel32.dll", loader_k32, sizeof loader_k32 / sizeof loader_k32[0]);
    vx_register_module("kernelbase.dll", loader_k32, sizeof loader_k32 / sizeof loader_k32[0]);
    vx_ucrt_init();
    vx_sync_init();
    vx_seh_init();
    vx_text_init();
    vx_files_init();
    vx_registry_init();
    vx_user32_init();
    vx_gdi_init();
    vx_opengl_init();
    vx_net_init();
    vx_mm_init();
    vx_midi_init();
    vx_iphlp_init();
    vx_winsock_nsp_init();
    vx_winhttp_proxy_init();
    vx_com_init();
    vx_input_init();
    vx_audio_init();
    vx_xaudio2_init();
    vx_dwrite_init();
    vx_d3d11_init();
    vx_dxgi_init();
    vx_d3d12_init();
    vx_d3d11on12_init();
    vx_vulkan_init();
    vx_d3d9_init();
    vx_vcruntime_init();
    vx_dotnet_init();
    vx_sysdlls_init();
    vx_dbghelp_init();
    vx_osinfo_init();
    vx_pointer_init();
    vx_regnotify_init();                         // (after sync.c's DuplicateHandle and CloseHandle)
    vx_section_init();
    vx_shlpath_init();
    vx_wevtapi_init();
    vx_ktime_init();
    vx_power_init();
    vx_dwmapi_init();
    vx_winmisc_init();
    vx_sysparams_init();                         // (after user32.c's GetSystemMetricsForDpi)
    vx_extras_init();                            // (last: the tables above come first)
    // VXRT_CONTROL_FOR=<program.exe>: only that program takes the socket; others (a launcher's watchdog) pass it on
    const char *control_for = getenv("VXRT_CONTROL_FOR");
    const char *base = strrchr(argv[1], '/') ? strrchr(argv[1], '/') + 1 : argv[1];
    if (getenv("VXRT_CONTROL") && (!control_for || !strcasecmp(base, control_for))) vx_control_start(getenv("VXRT_CONTROL"));

    // vxrt --list-exports: every function the runtime implements, "dll<TAB>name" per line (tools/vxrt_doctor.py
    // compares a game's imports with it, without running the game).
    if (!strcmp(argv[1], "--list-exports")) {
        for (size_t m = 0; m < g_module_count; m++)
            for (size_t i = 0; i < g_modules[m].count; i++)
                if (g_modules[m].exports[i].name) printf("%s\t%s\n", g_modules[m].dll, g_modules[m].exports[i].name);
        return 0;
    }

    char *added = vx_extra_args(&argc, &argv);     // (VXRT_EXTRA_ARGS: a launcher's arguments for this program, launchargs.c)
    void vx_handle_server_start(void);
    vx_handle_server_start();                      // (handles other processes take from this one, or give it: handlexfer.c)
    vx_proc.argc = argc - 1;
    vx_proc.argv = argv + 1;
    vx_profile_start();                            // (after argv: VXRT_PROFILE_FOR names the program)
    // A program another one started (CreateProcess, kernel32.c) has the command line that one gave, word for word, as on
    // Windows (VXRT_COMMAND_LINE, taken out of this process's environment): its own name as the starter wrote it (a
    // Windows path, not where vxrt found the file), its quotes and backslashes untouched. Steam starting itself again
    // after an update passed "C:\...\steam.exe" and got the Mac path back. VXRT_EXTRA_ARGS's go after the name.
    const char *given = getenv("VXRT_COMMAND_LINE");
    if (given) {
        const char *p = given;
        if (*p == '"') { p++; while (*p && *p != '"') p++; if (*p) p++; }
        else while (*p && *p != ' ' && *p != '\t') p++;
        size_t head = (size_t)(p - given);
        vx_proc.command_line = malloc(strlen(given) + (added ? strlen(added) + 1 : 0) + 1);
        memcpy(vx_proc.command_line, given, head);
        vx_proc.command_line[head] = 0;
        if (added) { strcat(vx_proc.command_line, " "); strcat(vx_proc.command_line, added); }
        strcat(vx_proc.command_line, p);
        unsetenv("VXRT_COMMAND_LINE");
        int k = 0;                                 // (and out of the environment block the program sees)
        for (int i = 0; envp[i]; i++) if (strncmp(envp[i], "VXRT_COMMAND_LINE=", 18)) envp[k++] = envp[i];
        envp[k] = NULL;
    } else {
        size_t len = 1;
        for (int i = 1; i < argc; i++) len += strlen(argv[i]) + 3;
        vx_proc.command_line = calloc(1, len);
        for (int i = 1; i < argc; i++) {
            int quote = strchr(argv[i], ' ') != NULL;
            if (i > 1) strcat(vx_proc.command_line, " ");
            if (quote) strcat(vx_proc.command_line, "\"");
            strcat(vx_proc.command_line, argv[i]);
            if (quote) strcat(vx_proc.command_line, "\"");
        }
    }
    free(added);
    vx_proc.envp = envp;
    vx_proc.command_line_w = vx_utf16_dup(vx_proc.command_line);

    g_peb = calloc(1, PEB_SIZE);
    pthread_key_create(&g_teb_key, NULL);
    vx_seh_install_signals();
    static char *exe;
    exe = realpath(argv[1], NULL);
    if (!exe) { vx_log("cannot open %s: %s", argv[1], strerror(errno)); return 1; }
    make_process_parameters(exe);
    snprintf(g_exe_dir, sizeof g_exe_dir, "%s", exe);
    *strrchr(g_exe_dir, '/') = 0;
    // The program runs on a thread of its own with the stack its header asks for (8 MB at least); the macOS
    // main thread stays free for AppKit (see cocoa.m).
    uint64_t reserve = 0;
    FILE *f = fopen(exe, "rb");
    if (f) {
        uint8_t hdr[0x400];
        size_t n = fread(hdr, 1, sizeof hdr, f);
        fclose(f);
        uint32_t nt = n >= 0x40 ? rd32(hdr + 0x3c) : 0;
        if (nt && nt + 24 + 80 <= n) reserve = rd64(hdr + nt + 24 + 72);   // SizeOfStackReserve
    }
    g_main_stack = reserve > (8u << 20) ? (size_t)reserve : (8u << 20);
    return vx_ui_run_main(run_program, exe, g_main_stack);
}

#ifdef __aarch64__
#include "thunks/loader.c.inc"
#endif
