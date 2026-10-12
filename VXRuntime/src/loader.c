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
#include <pthread.h>
#include "vx.h"

vx_process vx_proc;
int vx_trace;

void vx_log(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fputs("[vxrt] ", stderr); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
}

// ---- built-in module registry ------------------------------------------------------------------------------------
typedef struct { const char *dll; const vx_export *exports; size_t count; } vx_module;
static vx_module g_modules[512];
static size_t g_module_count;

void vx_register_module(const char *dll, const vx_export *exports, size_t count) {
    if (g_module_count < sizeof g_modules / sizeof g_modules[0]) g_modules[g_module_count++] = (vx_module){ dll, exports, count };
    else vx_log("too many export tables: %s is left out", dll);
}

// API sets resolve to the DLL that implements them.
static const char *canonical_dll(const char *dll) {
    if (!strncasecmp(dll, "api-ms-win-crt-", 15)) return "ucrtbase.dll";
    if (!strncasecmp(dll, "api-ms-win-core-", 16)) return "kernel32.dll";
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

void *vx_find_export(const char *dll, const char *name) {
    dll = canonical_dll(dll);
    for (size_t m = 0; m < g_module_count; m++) {
        if (!same_dll(g_modules[m].dll, dll)) continue;
        for (size_t i = 0; i < g_modules[m].count; i++)
            if (!strcmp(g_modules[m].exports[i].name, name)) return g_modules[m].exports[i].fn;
    }
    return NULL;
}

// A module handle for a built-in DLL is a pointer to a stored copy of its name.
static char g_builtin_names[256][64];
static int g_builtin_count;
static void *builtin_handle(const char *name) {
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    void *h = NULL;
    for (int i = 0; i < g_builtin_count && !h; i++) if (same_dll(g_builtin_names[i], name)) h = g_builtin_names[i];
    if (!h && g_builtin_count < 256) { snprintf(g_builtin_names[g_builtin_count], 64, "%s", name); h = g_builtin_names[g_builtin_count++]; }
    pthread_mutex_unlock(&lock);
    return h;
}
static int is_builtin_handle(const void *h) {
    return (const char *)h >= g_builtin_names[0] && (const char *)h < g_builtin_names[g_builtin_count] &&
           ((const char *)h - g_builtin_names[0]) % 64 == 0;
}

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
        page = mmap(NULL, 16384, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
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

// Lock-free: images are only ever appended (and never unmapped), so a reader sees a consistent prefix.
const vx_image *vx_image_at(uint64_t pc) {
    int n = __atomic_load_n(&g_image_count, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++) {
        const vx_image *m = g_images[i];
        if (pc >= (uint64_t)m->base && pc < (uint64_t)m->base + m->size) return m;
    }
    return NULL;
}
static vx_image *image_by_base(const void *base) {
    for (int i = 0; i < g_image_count; i++) if (g_images[i]->base == base) return g_images[i];
    return NULL;
}
static vx_image *image_by_name(const char *name) {
    const char *b = strrchr(name, '\\'); b = b ? b + 1 : name;
    const char *s = strrchr(b, '/'); b = s ? s + 1 : b;
    for (int i = 0; i < g_image_count; i++) if (same_dll(g_images[i]->name, b)) return g_images[i];
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
    uint64_t self = (uint64_t)teb, tls = *(uint64_t *)(teb + TEB_TLS_POINTER), peb = (uint64_t)g_peb;
    __asm__ volatile("movq %0, %%gs:0x30" :: "r"(self));
    __asm__ volatile("movq %0, %%gs:0x58" :: "r"(tls));
    __asm__ volatile("movq %0, %%gs:0x60" :: "r"(peb));
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

void vx_thread_attach(void) {
    uint8_t *teb = calloc(1, TEB_SIZE);
    pthread_t self = pthread_self();
    uint8_t *stack_top = pthread_get_stackaddr_np(self);
    *(void **)(teb + TEB_STACK_BASE) = stack_top;
    *(void **)(teb + TEB_STACK_LIMIT) = stack_top - pthread_get_stacksize_np(self);
    *(void **)(teb + TEB_SELF) = teb;
    *(uint64_t *)(teb + TEB_CLIENT_ID) = (uint64_t)getpid();
    *(uint64_t *)(teb + TEB_CLIENT_ID + 8) = vx_current_tid();
    *(void **)(teb + TEB_PEB) = g_peb;
    void **tls = calloc(MAX_TLS_MODULES, sizeof(void *));
    *(void ***)(teb + TEB_TLS_POINTER) = tls;
    pthread_mutex_lock(&g_loader_lock);
    for (int i = 0; i < g_image_count; i++)
        if (g_images[i]->tls_index != ~0u) tls[g_images[i]->tls_index] = tls_block(g_images[i]);
    pthread_mutex_unlock(&g_loader_lock);
    pthread_mutex_lock(&g_tebs_lock);
    if (g_teb_count == g_teb_cap) { g_teb_cap = g_teb_cap ? g_teb_cap * 2 : 64; g_tebs = realloc(g_tebs, g_teb_cap * sizeof *g_tebs); }
    g_tebs[g_teb_count++] = teb;
    pthread_mutex_unlock(&g_tebs_lock);
    pthread_setspecific(g_teb_key, teb);
    set_gs_slots(teb);
}

void vx_thread_detach(void) {
    uint8_t *teb = pthread_getspecific(g_teb_key);
    if (!teb) return;
    pthread_mutex_lock(&g_tebs_lock);
    for (int i = 0; i < g_teb_count; i++) if (g_tebs[i] == teb) { g_tebs[i] = g_tebs[--g_teb_count]; break; }
    pthread_mutex_unlock(&g_tebs_lock);
    void **tls = *(void ***)(teb + TEB_TLS_POINTER);
    for (int i = 0; i < MAX_TLS_MODULES; i++) free(tls[i]);
    free(tls);
    pthread_setspecific(g_teb_key, NULL);
    // The PEB leaves TSD slot 12 before the thread ends: libc frees whatever localtime's slot holds.
    __asm__ volatile("movq $0, %%gs:0x60" ::: "memory");
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

// Finds a DLL file: (the altered search directory,) the program's directory, C:\windows\system32, the current
// directory.
// Wine's own DLLs (marked "Wine builtin DLL" after the DOS header) need Wine underneath: they are passed over.
static int wine_builtin(const char *path) {
    char head[0x60] = { 0 };
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(head, 1, sizeof head, f);
    fclose(f);
    return n == sizeof head && !memcmp(head + 0x40, "Wine builtin DLL", 16);
}
static int search_dll(const char *name, char *out, size_t cap) {
    char file[256];
    snprintf(file, sizeof file, "%s%s", name, strchr(name, '.') ? "" : ".dll");
    char dirs[4][4096];
    snprintf(dirs[0], sizeof dirs[0], "%s", g_altered_dir);
    snprintf(dirs[1], sizeof dirs[1], "%s", g_exe_dir);
    snprintf(dirs[2], sizeof dirs[2], "%s/drive_c/windows/system32", vx_root());
    char cwd[4096];
    vx_get_cwd(cwd, sizeof cwd);
    if (!vx_unix_path(cwd, dirs[3], sizeof dirs[3])) dirs[3][0] = 0;
    for (int i = 0; i < 4; i++) {
        if (!dirs[i][0]) continue;
        snprintf(out, cap, "%s/%s", dirs[i], file);
        struct stat st;
        if (!stat(out, &st) && S_ISREG(st.st_mode)) return 1;
    }
    // VXRT_DLL_PATH: more Mac directories (colon-separated) holding the user's own Windows DLLs, for example the
    // Visual C++ runtime a Windows installation already has.
    const char *extra = getenv("VXRT_DLL_PATH");
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
    uint8_t *file = malloc(len);
    if (fread(file, 1, len, f) != (size_t)len) { fclose(f); free(file); return NULL; }
    fclose(f);
    if (len < 0x200 || file[0] != 'M' || file[1] != 'Z') { vx_log("%s: not an MZ executable", path); free(file); vx_set_last_error(193); return NULL; }
    const uint8_t *nt = file + rd32(file + 0x3c);
    if (memcmp(nt, "PE\0\0", 4) || rd16(nt + 4) != 0x8664 || rd16(nt + 24) != 0x20b) {
        vx_log("%s: not a PE32+ x86-64 image", path); free(file); vx_set_last_error(193 /* ERROR_BAD_EXE_FORMAT */); return NULL;
    }
    uint16_t nsections = rd16(nt + 6), characteristics = rd16(nt + 22), opt_size = rd16(nt + 20);
    const uint8_t *opt = nt + 24;
    uint64_t pref_base = rd64(opt + 24);
    uint32_t image_size = rd32(opt + 56), headers_size = rd32(opt + 60);
    uint32_t ndirs = rd32(opt + 108);
    pe_dir dirs[16];
    for (uint32_t i = 0; i < 16; i++) dirs[i] = i < ndirs ? *(const pe_dir *)(opt + 112 + i * 8) : (pe_dir){0, 0};

    // Map at the preferred base when free, anywhere otherwise (then relocate).
    uint8_t *base = mmap((void *)pref_base, image_size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (base == MAP_FAILED) { vx_log("cannot map %u bytes", image_size); free(file); return NULL; }
    memcpy(base, file, headers_size);
    const pe_section *sec = (const pe_section *)(opt + opt_size);
    for (uint16_t i = 0; i < nsections; i++) {
        uint32_t n = sec[i].raw_size < sec[i].vsize || !sec[i].vsize ? sec[i].raw_size : sec[i].vsize;
        if (n) memcpy(base + sec[i].va, file + sec[i].raw_ptr, n);
    }
    free(file);

    int64_t delta = (int64_t)((uint64_t)base - pref_base);
    if (delta) {
        if (!dirs[5].size) { vx_log("%s: loaded away from its base and has no relocations", path); return NULL; }
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
    m->entry_rva = rd32(opt + 16);
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
    if (g_image_count == MAX_IMAGES) { vx_log("too many images"); return NULL; }
    g_images[g_image_count] = m;
    __atomic_store_n(&g_image_count, g_image_count + 1, __ATOMIC_RELEASE);   // visible before imports load (cycles)
    if (vx_trace) vx_log("%s mapped at %p", m->name, base);

    // TLS: an index for this image and a block on every thread that exists already.
    if (m->tls_rva) {
        m->tls_index = g_next_tls_index++;
        *(uint32_t *)rd64(base + m->tls_rva + 16) = m->tls_index;
        pthread_mutex_lock(&g_tebs_lock);
        for (int i = 0; i < g_teb_count; i++) (*(void ***)(g_tebs[i] + TEB_TLS_POINTER))[m->tls_index] = tls_block(m);
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
            if (dep && dep != m && !init_dll(dep)) return NULL;
            for (size_t i = 0; lookup[i]; i++) {
                const char *name = NULL;
                int ordinal = 0;
                char label[16];
                if (lookup[i] >> 63) { ordinal = (int)(lookup[i] & 0xffff); snprintf(label, sizeof label, "#%d", ordinal); }
                else name = (const char *)base + (lookup[i] & 0x7fffffff) + 2;
                void *fn = resolve(dll, name, ordinal, 0);
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
static void *load_library(const char *name, DWORD flags) {
    pthread_mutex_lock(&g_loader_lock);
    vx_image *m = NULL;
    void *h = NULL;
    if (strchr(name, '\\') || strchr(name, '/')) {
        char path[4096], saved[4096];
        if (vx_unix_path(name, path, sizeof path)) {
            m = image_by_name(name);
            snprintf(saved, sizeof saved, "%s", g_altered_dir);
            if (flags & LOAD_WITH_ALTERED_SEARCH_PATH) {
                snprintf(g_altered_dir, sizeof g_altered_dir, "%s", path);
                char *slash = strrchr(g_altered_dir, '/');
                if (slash) *slash = 0;
            }
            struct stat st;
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
// Images stay mapped: code from them may still be running on other threads, and unloading is rarely needed.
static WINAPI BOOL FreeLibrary(void *h) {
    pthread_mutex_lock(&g_loader_lock);
    vx_image *m = image_by_base(h);
    if (m && m->refs > 1) m->refs--;
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
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 4
static WINAPI BOOL GetModuleHandleExA(DWORD flags, const char *name, void **out) {
    if (flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) {
        const vx_image *m = vx_image_at((uint64_t)name);
        *out = m ? m->base : NULL;
    } else {
        *out = module_handle(name);
    }
    if (!*out) vx_set_last_error(126);
    return *out != NULL;
}
static WINAPI BOOL GetModuleHandleExW(DWORD flags, const WCHAR *name, void **out) {
    if (flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) return GetModuleHandleExA(flags, (const char *)name, out);
    char *a = vx_utf8_dup(name);
    BOOL r = GetModuleHandleExA(flags, a, out);
    free(a);
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
    void *fn = m ? pe_export(m, ordinal ? NULL : name, ordinal, 0) : builtin ? vx_find_export((const char *)module, ordinal ? by_number : name) : NULL;
    pthread_mutex_unlock(&g_loader_lock);
    if (!fn) vx_set_last_error(m || builtin ? 127 /* ERROR_PROC_NOT_FOUND */ : 6 /* ERROR_INVALID_HANDLE */);
    if (!fn && vx_trace) vx_log("GetProcAddress(%s, %s) failed", m ? m->name : builtin ? (const char *)module : "?", ordinal ? by_number : name);
    return fn;
}

static const char *module_path(void *module, char *tmp, size_t cap) {
    vx_image *m = image_by_base(module ? module : vx_proc.image_base);
    if (m) return m->windows_path;
    if (!is_builtin_handle(module)) { vx_set_last_error(126); snprintf(tmp, cap, "%s", ""); return tmp; }
    snprintf(tmp, cap, "C:\\windows\\system32\\%s", (const char *)module);
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
    VX_EXPORT(FreeLibrary), VX_EXPORT(DisableThreadLibraryCalls),
    VX_EXPORT(GetModuleHandleA), VX_EXPORT(GetModuleHandleW), VX_EXPORT(GetModuleHandleExA), VX_EXPORT(GetModuleHandleExW),
    VX_EXPORT(GetProcAddress), VX_EXPORT(GetModuleFileNameA), VX_EXPORT(GetModuleFileNameW),
};

// ---- main ------------------------------------------------------------------------------------------------------
int vx_ui_run_main(int (*program)(void *), void *arg, size_t stack);

// The program's main thread: loads it (DLL initializers run here, on a Windows thread) and calls its entry.
static int run_program(void *arg) {
    const char *exe = arg;
    vx_thread_attach();
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
    vx_trace = getenv("VXRT_TRACE") && *getenv("VXRT_TRACE") == '1';
    vx_kernel32_init();
    vx_system_init();
    vx_desktop_init();
    vx_services_init();
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
    vx_net_init();
    vx_mm_init();
    vx_com_init();
    vx_input_init();
    vx_audio_init();
    vx_xaudio2_init();
    vx_d3d11_init();
    vx_dxgi_init();
    vx_d3d9_init();
    vx_vcruntime_init();

    vx_proc.argc = argc - 1;
    vx_proc.argv = argv + 1;
    vx_proc.envp = envp;
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
    return vx_ui_run_main(run_program, exe, reserve > (8u << 20) ? (size_t)reserve : (8u << 20));
}
