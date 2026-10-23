// dbghelp: the stack walks and symbol lookups programs do for their own crash and debug reports (Chromium's
// base::debug::StackTrace), in the calling process. Written from Microsoft's documentation of DbgHelp.
//
// StackWalk64 walks x64 frames with the program's own unwind data through the runtime's RtlLookupFunctionEntry and
// RtlVirtualUnwind (seh.c), the way x64 Windows walks them. Symbols come from the PE export tables of the loaded
// images (what DbgHelp offers without a PDB: the nearest export below an address); there is no line information.
// A walk ends at a frame whose code is neither in a loaded image nor in a registered function table: the program
// was called from this runtime's own native code there (a thread's start, a window procedure, a callback).
#include "vx.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>

enum { ERROR_INVALID_HANDLE = 6, ERROR_INVALID_PARAMETER = 87, ERROR_INSUFFICIENT_BUFFER = 122, ERROR_MOD_NOT_FOUND = 126,
       ERROR_INVALID_ADDRESS = 487 };
#define IMAGE_FILE_MACHINE_AMD64 0x8664

// Layouts (DbgHelp.h / winnt.h; offsets as MinGW-w64's headers give them).
typedef struct { DWORD begin, end, unwind; } RUNTIME_FUNCTION;
typedef struct __attribute__((aligned(16))) {
    uint8_t  head[0x78];
    uint64_t gpr[16];                 // Rax Rcx Rdx Rbx Rsp Rbp Rsi Rdi R8..R15
    uint64_t rip;
    uint8_t  rest[0x4d0 - 0x100];
} CONTEXT;
_Static_assert(sizeof(CONTEXT) == 0x4d0 && __builtin_offsetof(CONTEXT, rip) == 0xf8, "CONTEXT layout");
enum { RSP = 4, RBP = 5 };
typedef struct { uint64_t offset; uint16_t segment; DWORD mode; } ADDRESS64;   // mode 3: AddrModeFlat
typedef struct {
    ADDRESS64 pc, ret, frame, stack, bstore;
    uint64_t func_table_entry;
    uint64_t params[4];
    BOOL far_, virtual_;
    uint64_t reserved[3];
    uint8_t kd_help[112];
} STACKFRAME64;
_Static_assert(sizeof(ADDRESS64) == 16 && sizeof(STACKFRAME64) == 264 && __builtin_offsetof(STACKFRAME64, reserved) == 128, "STACKFRAME64 layout");
typedef struct {
    DWORD size_of_struct, type_index;
    uint64_t reserved[2];
    DWORD index, size;
    uint64_t mod_base;
    DWORD flags;
    uint64_t value, address;
    DWORD reg, scope, tag, name_len, max_name_len;
    char name[1];
} SYMBOL_INFO;
_Static_assert(__builtin_offsetof(SYMBOL_INFO, name) == 84 && __builtin_offsetof(SYMBOL_INFO, value) == 48, "SYMBOL_INFO layout");
#define SYMFLAG_EXPORT 0x200
#define SYM_TAG_PUBLIC_SYMBOL 10                  // SymTagPublicSymbol

#ifdef __aarch64__
#define PROGRAM_ADDRESS(p) vx_unlow((uint64_t)(uintptr_t)(p))
#else
#define PROGRAM_ADDRESS(p) ((uint64_t)(uintptr_t)(p))
#endif

static RUNTIME_FUNCTION *(WINAPI *rt_RtlLookupFunctionEntry)(uint64_t, uint64_t *, void *);
static void *(WINAPI *rt_RtlVirtualUnwind)(DWORD, uint64_t, uint64_t, RUNTIME_FUNCTION *, CONTEXT *, void **, uint64_t *, void *);
static DWORD (WINAPI *rt_GetProcessId)(HANDLE);

// ---- sessions: SymInitialize .. SymCleanup, per process handle -------------------------------------------------------
// Options are global (SymSetOptions has no process); each initialized process has its search path.
typedef struct { HANDLE process; char *path; } session;
static session g_sessions[16];
static DWORD g_options = 0x2 | 0x4;               // SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS, DbgHelp's defaults
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static session *find_session(HANDLE process) {    // (g_lock held)
    for (int i = 0; i < 16; i++) if (g_sessions[i].path && g_sessions[i].process == process) return &g_sessions[i];
    return NULL;
}
static int is_this_process(HANDLE process) {
    if ((intptr_t)process == -1) return 1;
    return rt_GetProcessId && rt_GetProcessId(process) == (DWORD)getpid();
}
// The default search path: the current directory, then _NT_SYMBOL_PATH and _NT_ALTERNATE_SYMBOL_PATH when set.
static char *default_path(void) {
    const char *a = getenv("_NT_SYMBOL_PATH"), *b = getenv("_NT_ALTERNATE_SYMBOL_PATH");
    size_t n = 2 + (a ? strlen(a) + 1 : 0) + (b ? strlen(b) + 1 : 0);
    char *p = malloc(n);
    snprintf(p, n, ".%s%s%s%s", a ? ";" : "", a ? a : "", b ? ";" : "", b ? b : "");
    return p;
}
static BOOL initialize(HANDLE process, char *path) {   // takes path (NULL: the default)
    if (!path || !*path) { free(path); path = default_path(); }
    pthread_mutex_lock(&g_lock);
    int ok = 0;
    if (!find_session(process))
        for (int i = 0; i < 16 && !ok; i++) if (!g_sessions[i].path) { g_sessions[i] = (session){ process, path }; ok = 1; }
    pthread_mutex_unlock(&g_lock);
    if (!ok) { free(path); vx_set_last_error(ERROR_INVALID_PARAMETER); }   // (already initialized: DbgHelp says the same)
    return ok;
}
static WINAPI BOOL SymInitialize(HANDLE process, const char *path, BOOL invade) {
    (void)invade;                                  // (the loaded images are always known)
    return initialize(process, path ? strdup(path) : NULL);
}
static WINAPI BOOL SymInitializeW(HANDLE process, const WCHAR *path, BOOL invade) {
    (void)invade;
    return initialize(process, vx_utf8_dup(path));
}
static WINAPI BOOL SymCleanup(HANDLE process) {
    pthread_mutex_lock(&g_lock);
    session *s = find_session(process);
    if (s) { free(s->path); s->path = NULL; s->process = NULL; }
    pthread_mutex_unlock(&g_lock);
    if (!s) vx_set_last_error(ERROR_INVALID_HANDLE);
    return s != NULL;
}
static int initialized(HANDLE process) {
    pthread_mutex_lock(&g_lock);
    int ok = find_session(process) != NULL;
    pthread_mutex_unlock(&g_lock);
    if (!ok) vx_set_last_error(ERROR_INVALID_HANDLE);
    return ok;
}
static WINAPI DWORD SymSetOptions(DWORD options) { g_options = options; return g_options; }
static WINAPI DWORD SymGetOptions(void) { return g_options; }
static BOOL set_path(HANDLE process, char *path) {     // takes path
    if (!path || !*path) { free(path); path = default_path(); }
    pthread_mutex_lock(&g_lock);
    session *s = find_session(process);
    if (s) { free(s->path); s->path = path; }
    pthread_mutex_unlock(&g_lock);
    if (!s) { free(path); vx_set_last_error(ERROR_INVALID_HANDLE); }
    return s != NULL;
}
static WINAPI BOOL SymSetSearchPathW(HANDLE process, const WCHAR *path) { return set_path(process, vx_utf8_dup(path)); }
static WINAPI BOOL SymSetSearchPath(HANDLE process, const char *path) { return set_path(process, path ? strdup(path) : NULL); }
// The path with its terminator must fit the buffer, else ERROR_INSUFFICIENT_BUFFER.
static BOOL get_path(HANDLE process, void *out, DWORD cap, int wide) {
    pthread_mutex_lock(&g_lock);
    session *s = find_session(process);
    char *path = s ? strdup(s->path) : NULL;
    pthread_mutex_unlock(&g_lock);
    if (!path) { vx_set_last_error(ERROR_INVALID_HANDLE); return 0; }
    size_t need = wide ? vx_utf8_to_utf16(path, strlen(path), NULL, 0) + 1 : strlen(path) + 1;
    BOOL ok = out && need <= cap;
    if (ok && wide) { vx_utf8_to_utf16(path, strlen(path), out, need - 1); ((WCHAR *)out)[need - 1] = 0; }
    else if (ok) memcpy(out, path, need);
    else vx_set_last_error(ERROR_INSUFFICIENT_BUFFER);
    free(path);
    return ok;
}
static WINAPI BOOL SymGetSearchPathW(HANDLE process, WCHAR *out, DWORD cap) { return get_path(process, out, cap, 1); }
static WINAPI BOOL SymGetSearchPath(HANDLE process, char *out, DWORD cap) { return get_path(process, out, cap, 0); }

// ---- modules and function tables ---------------------------------------------------------------------------------------
static WINAPI uint64_t SymGetModuleBase64(HANDLE process, uint64_t address) {
    if (!is_this_process(process)) { vx_set_last_error(ERROR_INVALID_HANDLE); return 0; }
    const vx_image *m = vx_image_at(address);
    if (!m) { vx_set_last_error(ERROR_MOD_NOT_FOUND); return 0; }
    return PROGRAM_ADDRESS(m->base);
}
// x64: the RUNTIME_FUNCTION of the function containing the address (images and registered dynamic tables alike).
static WINAPI void *SymFunctionTableAccess64(HANDLE process, uint64_t address) {
    if (!is_this_process(process)) { vx_set_last_error(ERROR_INVALID_HANDLE); return NULL; }
    uint64_t base = 0;
    RUNTIME_FUNCTION *fn = rt_RtlLookupFunctionEntry ? rt_RtlLookupFunctionEntry(address, &base, NULL) : NULL;
    if (!fn) vx_set_last_error(ERROR_INVALID_ADDRESS);
    return fn;
}

// ---- symbols from export tables ------------------------------------------------------------------------------------------
// The named export at or below the address in its image (forwarders, whose RVA points into the export directory,
// are not code). Returns its RVA and name, or 0.
static int nearest_export(const vx_image *m, uint32_t rva, uint32_t *at, const char **name) {
    if (!m->export_rva || !m->export_size) return 0;
    const uint8_t *dir = m->base + m->export_rva;
    uint32_t names, functions_rva, names_rva, ordinals_rva, nfunctions;
    memcpy(&nfunctions, dir + 20, 4); memcpy(&names, dir + 24, 4);
    memcpy(&functions_rva, dir + 28, 4); memcpy(&names_rva, dir + 32, 4); memcpy(&ordinals_rva, dir + 36, 4);
    const uint32_t *functions = (const uint32_t *)(m->base + functions_rva), *name_list = (const uint32_t *)(m->base + names_rva);
    const uint16_t *ordinals = (const uint16_t *)(m->base + ordinals_rva);
    int found = 0;
    for (uint32_t i = 0; i < names; i++) {
        if (ordinals[i] >= nfunctions) continue;
        uint32_t f = functions[ordinals[i]];
        if (!f || f > rva || (f >= m->export_rva && f < m->export_rva + m->export_size)) continue;
        if (!found || f > *at) { *at = f; *name = (const char *)(m->base + name_list[i]); found = 1; }
    }
    return found;
}
static WINAPI BOOL SymFromAddr(HANDLE process, uint64_t address, uint64_t *displacement, SYMBOL_INFO *symbol) {
    if (!initialized(process)) return 0;
    if (!is_this_process(process)) { vx_set_last_error(ERROR_INVALID_HANDLE); return 0; }   // (only this process is read)
    if (!symbol) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    const vx_image *m = vx_image_at(address);
    if (!m) { vx_set_last_error(ERROR_MOD_NOT_FOUND); return 0; }
    uint32_t rva = (uint32_t)(address - (uint64_t)(uintptr_t)m->base), at = 0;
    const char *name = NULL;
    if (!nearest_export(m, rva, &at, &name)) { vx_set_last_error(ERROR_INVALID_ADDRESS); return 0; }
    DWORD max = symbol->max_name_len;
    memset(symbol, 0, __builtin_offsetof(SYMBOL_INFO, name));
    symbol->size_of_struct = 88;
    symbol->max_name_len = max;
    symbol->mod_base = PROGRAM_ADDRESS(m->base);
    symbol->address = symbol->mod_base + at;
    symbol->flags = SYMFLAG_EXPORT;
    symbol->tag = SYM_TAG_PUBLIC_SYMBOL;
    uint64_t base = 0;                             // its size, when the export starts a function of the unwind data
    RUNTIME_FUNCTION *fn = rt_RtlLookupFunctionEntry ? rt_RtlLookupFunctionEntry((uint64_t)(uintptr_t)m->base + at, &base, NULL) : NULL;
    if (fn && fn->begin == at) symbol->size = fn->end - fn->begin;
    size_t len = strlen(name);
    symbol->name_len = (DWORD)len;
    if (max) {
        size_t n = len < max - 1 ? len : max - 1;
        memcpy(symbol->name, name, n); symbol->name[n] = 0;
    }
    if (displacement) *displacement = rva - at;
    return 1;
}
// No PDBs are read, so no address has line information.
static WINAPI BOOL SymGetLineFromAddr64(HANDLE process, uint64_t address, DWORD *displacement, void *line) {
    (void)address; (void)displacement; (void)line;
    if (!initialized(process)) return 0;
    vx_set_last_error(ERROR_INVALID_ADDRESS);
    return 0;
}

// ---- StackWalk64 -------------------------------------------------------------------------------------------------------
// The first call reports the context's own frame; each later call unwinds the context one frame and reports that one
// (the STACKFRAME64's reserved words remember which frame the context is at). AddrReturn is where the reported frame
// returns to. Memory is read through the kernel (mach_vm_read_overwrite), so a bad stack pointer ends the walk instead
// of faulting. Only this process is walked, so the routines passed in are not called: the runtime knows its function
// tables (the images' and those registered with RtlAddFunctionTable and its kin) and reads its memory directly.
static int read_u64(uint64_t address, uint64_t *out) {
    mach_vm_size_t got = 0;
    return mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(uintptr_t)VX_LOW(address), 8, (mach_vm_address_t)(uintptr_t)out, &got) == KERN_SUCCESS && got == 8;
}
// Moves c to its caller. 0 at the end of the stack (or where the frames are not the program's).
static int unwind_once(CONTEXT *c, RUNTIME_FUNCTION **entry) {
    uint64_t pc = c->rip, sp = c->gpr[RSP], base = 0, ret;
    if (!pc || !read_u64(sp, &ret)) return 0;
    RUNTIME_FUNCTION *fn = rt_RtlLookupFunctionEntry ? rt_RtlLookupFunctionEntry(pc, &base, NULL) : NULL;
    if (entry) *entry = fn;
    if (fn) {
        void *data = NULL; uint64_t frame = 0;
        rt_RtlVirtualUnwind(0, base, pc, fn, c, &data, &frame, NULL);
    } else if (vx_image_at(pc)) {                  // a leaf function: the return address is on top of the stack
        c->rip = ret;
        c->gpr[RSP] = sp + 8;
    } else return 0;
    return c->gpr[RSP] > sp && c->rip;             // (frames only move up the stack)
}
#define WALK_MARK 0x5658525453544b31ull            // "VXRTSTK1": this STACKFRAME64 continues a walk
static WINAPI BOOL StackWalk64(DWORD machine, HANDLE process, HANDLE thread, STACKFRAME64 *frame, void *context,
                               void *read_memory, void *function_table_access, void *get_module_base, void *translate_address) {
    (void)thread; (void)read_memory; (void)function_table_access; (void)get_module_base; (void)translate_address;
    if (machine != IMAGE_FILE_MACHINE_AMD64 || !frame || !context) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    if (!is_this_process(process)) { vx_set_last_error(ERROR_INVALID_HANDLE); return 0; }
    CONTEXT *c = context;
    if (frame->reserved[0] == WALK_MARK && frame->reserved[1] == c->rip && frame->reserved[2] == c->gpr[RSP]) {
        if (!unwind_once(c, NULL)) return 0;      // the next frame
    } else if (!c->rip) return 0;
    CONTEXT caller = *c;
    RUNTIME_FUNCTION *fn = NULL;
    int more = unwind_once(&caller, &fn);
    memset(frame, 0, __builtin_offsetof(STACKFRAME64, kd_help));
    frame->pc = (ADDRESS64){ c->rip, 0, 3 };
    frame->ret = (ADDRESS64){ more ? caller.rip : 0, 0, 3 };
    frame->frame = (ADDRESS64){ c->gpr[RBP], 0, 3 };
    frame->stack = (ADDRESS64){ c->gpr[RSP], 0, 3 };
    frame->func_table_entry = fn ? PROGRAM_ADDRESS(fn) : 0;
    frame->reserved[0] = WALK_MARK; frame->reserved[1] = c->rip; frame->reserved[2] = c->gpr[RSP];
    return 1;
}

static const vx_export dbghelp[] = {
    VX_EXPORT(SymInitialize), VX_EXPORT(SymInitializeW), VX_EXPORT(SymCleanup), VX_EXPORT(SymSetOptions), VX_EXPORT(SymGetOptions),
    VX_EXPORT(SymSetSearchPathW), VX_EXPORT(SymSetSearchPath), VX_EXPORT(SymGetSearchPathW), VX_EXPORT(SymGetSearchPath),
    VX_EXPORT(SymGetModuleBase64), VX_EXPORT(SymFunctionTableAccess64), VX_EXPORT(SymFromAddr), VX_EXPORT(SymGetLineFromAddr64),
    VX_EXPORT(StackWalk64),
};

void vx_dbghelp_init(void) {                       // (after seh.c and kernel32 have registered theirs)
    rt_RtlLookupFunctionEntry = (__typeof__(rt_RtlLookupFunctionEntry))vx_find_export("kernel32.dll", "RtlLookupFunctionEntry");
    rt_RtlVirtualUnwind = (__typeof__(rt_RtlVirtualUnwind))vx_find_export("kernel32.dll", "RtlVirtualUnwind");
    rt_GetProcessId = (__typeof__(rt_GetProcessId))vx_find_export("kernel32.dll", "GetProcessId");
    vx_register_module("dbghelp.dll", dbghelp, sizeof dbghelp / sizeof dbghelp[0]);
}

#ifdef __aarch64__
#include "thunks/dbghelp.c.inc"
#endif
