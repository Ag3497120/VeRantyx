// The PE loader: maps a Windows x86-64 .exe into this process, relocates it, binds its imports to this
// runtime's own implementations, sets up the TEB/PEB and TLS, and calls its entry point.
//
//   vxrt program.exe [args...]
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
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

// ---- module registry ---------------------------------------------------------------------------------------
typedef struct { const char *dll; const vx_export *exports; size_t count; } vx_module;
static vx_module g_modules[64];
static size_t g_module_count;

void vx_register_module(const char *dll, const vx_export *exports, size_t count) {
    if (g_module_count < 64) g_modules[g_module_count++] = (vx_module){ dll, exports, count };
}

// api-ms-win-crt-*.dll are API sets: all of them resolve to the UCRT here.
static const char *canonical_dll(const char *dll) {
    if (!strncasecmp(dll, "api-ms-win-crt-", 15)) return "ucrtbase.dll";
    if (!strncasecmp(dll, "api-ms-win-core-", 16)) return "kernel32.dll";
    return dll;
}

void *vx_find_export(const char *dll, const char *name) {
    dll = canonical_dll(dll);
    for (size_t m = 0; m < g_module_count; m++) {
        if (strcasecmp(g_modules[m].dll, dll)) continue;
        for (size_t i = 0; i < g_modules[m].count; i++)
            if (!strcmp(g_modules[m].exports[i].name, name)) return g_modules[m].exports[i].fn;
    }
    return NULL;
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

static uint8_t *load_image(const char *path, uint32_t *entry_rva, pe_dir *dirs) {
    FILE *f = fopen(path, "rb");
    if (!f) { vx_log("cannot open %s: %s", path, strerror(errno)); return NULL; }
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *file = malloc(len);
    if (fread(file, 1, len, f) != (size_t)len) { fclose(f); return NULL; }
    fclose(f);
    if (len < 0x200 || file[0] != 'M' || file[1] != 'Z') { vx_log("%s: not an MZ executable", path); return NULL; }
    const uint8_t *nt = file + rd32(file + 0x3c);
    if (memcmp(nt, "PE\0\0", 4) || rd16(nt + 4) != 0x8664 || rd16(nt + 24) != 0x20b) {
        vx_log("%s: not a PE32+ x86-64 image", path); return NULL;
    }
    uint16_t nsections = rd16(nt + 6), opt_size = rd16(nt + 20);
    const uint8_t *opt = nt + 24;
    *entry_rva = rd32(opt + 16);
    uint64_t pref_base = rd64(opt + 24);
    uint32_t image_size = rd32(opt + 56), headers_size = rd32(opt + 60);
    uint32_t ndirs = rd32(opt + 108);
    for (uint32_t i = 0; i < 16; i++) dirs[i] = i < ndirs ? *(const pe_dir *)(opt + 112 + i * 8) : (pe_dir){0, 0};

    // Map at the preferred base when free, anywhere otherwise (then relocate).
    uint8_t *base = mmap((void *)pref_base, image_size, PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_PRIVATE | MAP_ANON, -1, 0);
    if (base == MAP_FAILED) { vx_log("cannot map %u bytes", image_size); return NULL; }
    memcpy(base, file, headers_size);
    const pe_section *sec = (const pe_section *)(opt + opt_size);
    for (uint16_t i = 0; i < nsections; i++) {
        uint32_t n = sec[i].raw_size < sec[i].vsize || !sec[i].vsize ? sec[i].raw_size : sec[i].vsize;
        if (n) memcpy(base + sec[i].va, file + sec[i].raw_ptr, n);
    }

    int64_t delta = (int64_t)((uint64_t)base - pref_base);
    if (delta) {
        if (!dirs[5].size) { vx_log("%s: loaded away from its base and has no relocations", path); return NULL; }
        for (uint8_t *b = base + dirs[5].va; b < base + dirs[5].va + dirs[5].size;) {
            uint32_t page = rd32(b), block = rd32(b + 4);
            for (uint32_t k = 8; k < block; k += 2) {
                uint16_t e = rd16(b + k);
                if ((e >> 12) == 10) { uint64_t *p = (uint64_t *)(base + page + (e & 0xfff)); *p += delta; }
                else if ((e >> 12) != 0) { vx_log("relocation type %u not handled", e >> 12); return NULL; }
            }
            b += block;
        }
    }

    // Imports.
    int missing = 0;
    if (dirs[1].size) {
        for (const uint8_t *d = base + dirs[1].va; rd32(d + 12); d += 20) {
            const char *dll = (const char *)base + rd32(d + 12);
            uint64_t *lookup = (uint64_t *)(base + (rd32(d) ? rd32(d) : rd32(d + 16)));
            uint64_t *iat = (uint64_t *)(base + rd32(d + 16));
            for (size_t i = 0; lookup[i]; i++) {
                const char *name;
                char ordname[16];
                if (lookup[i] >> 63) { snprintf(ordname, sizeof ordname, "#%u", (unsigned)(lookup[i] & 0xffff)); name = ordname; }
                else name = (const char *)base + (lookup[i] & 0x7fffffff) + 2;
                void *fn = vx_find_export(dll, name);
                if (!fn) { fn = make_stub(dll, name); missing++; if (vx_trace) vx_log("missing %s!%s", dll, name); }
                iat[i] = (uint64_t)fn;
            }
        }
    }
    if (missing) vx_log("%d imported functions are not implemented yet (stubs stop the program if called)", missing);
    vx_proc.image_base = base;
    vx_proc.image_size = image_size;
    free(file);
    return base;
}

// ---- TEB / PEB / TLS -------------------------------------------------------------------------------------------
static uint8_t *g_peb;
static pe_dir g_tls_dir;
static pthread_key_t g_teb_key;

// macOS keeps gs pointed at the pthread's TSD array and does not let us move it. Windows code reads the TEB at
// gs:0x30 and the TLS array at gs:0x58, so those two TSD slots are set to the TEB's values on every thread, as
// Wine did on macOS before version 8. Native code (this runtime, macOS, Metal, D3DMetal) keeps working on the
// same thread with no switching.
static void set_gs_slots(uint8_t *teb) {
    uint64_t self = (uint64_t)teb, tls = *(uint64_t *)(teb + TEB_TLS_POINTER);
    __asm__ volatile("movq %0, %%gs:0x30" :: "r"(self));
    __asm__ volatile("movq %0, %%gs:0x58" :: "r"(tls));
}

static void init_thread_tls(uint8_t *teb) {
    if (!g_tls_dir.size) return;
    const uint8_t *td = vx_proc.image_base + g_tls_dir.va;
    uint64_t start = rd64(td), end = rd64(td + 8), index_va = rd64(td + 16);
    uint32_t zero_fill = rd32(td + 32);
    size_t size = (end - start) + zero_fill;
    uint8_t *block = calloc(1, size ? size : 1);
    memcpy(block, (void *)start, end - start);
    void **array = calloc(64, sizeof(void *));
    array[0] = block;
    *(uint32_t *)index_va = 0;
    *(void ***)(teb + TEB_TLS_POINTER) = array;
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
    uint64_t tid; pthread_threadid_np(NULL, &tid);
    *(uint64_t *)(teb + TEB_CLIENT_ID + 8) = tid;
    *(void **)(teb + TEB_PEB) = g_peb;
    init_thread_tls(teb);
    pthread_setspecific(g_teb_key, teb);
    set_gs_slots(teb);
}

void vx_set_last_error(DWORD e) { *(DWORD *)(vx_teb() + TEB_LAST_ERROR) = e; }
DWORD vx_get_last_error(void) { return *(DWORD *)(vx_teb() + TEB_LAST_ERROR); }

static void call_tls_callbacks(DWORD reason) {
    if (!g_tls_dir.size) return;
    const uint8_t *td = vx_proc.image_base + g_tls_dir.va;
    uint64_t callbacks = rd64(td + 24);
    typedef void (WINAPI *tls_cb)(void *, DWORD, void *);
    for (tls_cb *cb = (tls_cb *)callbacks; cb && *cb; cb++) (*cb)(vx_proc.image_base, reason, NULL);
}

// ---- main ------------------------------------------------------------------------------------------------------
int main(int argc, char **argv, char **envp) {
    if (argc < 2) { fprintf(stderr, "usage: vxrt program.exe [args...]\n"); return 2; }
    vx_trace = getenv("VXRT_TRACE") && *getenv("VXRT_TRACE") == '1';
    vx_kernel32_init();
    vx_ucrt_init();

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
    vx_proc.command_line_w = calloc(len, sizeof(WCHAR));
    for (size_t i = 0; vx_proc.command_line[i]; i++) vx_proc.command_line_w[i] = (uint8_t)vx_proc.command_line[i];

    uint32_t entry_rva; pe_dir dirs[16];
    if (!load_image(argv[1], &entry_rva, dirs)) return 1;
    g_tls_dir = dirs[9];
    g_peb = calloc(1, PEB_SIZE);
    *(void **)(g_peb + PEB_IMAGE_BASE) = vx_proc.image_base;
    pthread_key_create(&g_teb_key, NULL);
    vx_thread_attach();
    if (vx_trace) vx_log("%s mapped at %p, entry %#x", argv[1], vx_proc.image_base, entry_rva);

    call_tls_callbacks(1 /* DLL_PROCESS_ATTACH */);
    typedef DWORD (WINAPI *entry_fn)(void);
    DWORD rc = ((entry_fn)(vx_proc.image_base + entry_rva))();
    fflush(stdout);
    return (int)rc;
}
