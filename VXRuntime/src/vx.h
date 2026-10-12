// VeRantyx runtime (vxrt): runs Windows x86-64 programs on Apple silicon without Wine.
//
// One macOS x86-64 process per Windows process. Rosetta 2 translates the Windows program's code and this
// runtime's code alike, and the Windows API is implemented here as ordinary functions with the Windows calling
// convention (ms_abi), which the program calls directly: no Windows/Unix split, no syscall boundary, no
// server process for in-process objects.
#ifndef VX_H
#define VX_H
#include <stddef.h>
#include <stdint.h>

#define WINAPI __attribute__((ms_abi))

typedef uint32_t DWORD;
typedef int32_t  LONG;
typedef int      BOOL;
typedef void    *HANDLE;
typedef uint16_t WCHAR;

// ---- exports: what a Windows DLL name resolves to ----------------------------------------------------------
typedef struct { const char *name; void *fn; } vx_export;
// Register the exports of one module (case-insensitive name, e.g. "kernel32.dll"). The table must stay alive.
void vx_register_module(const char *dll, const vx_export *exports, size_t count);
void *vx_find_export(const char *dll, const char *name);   // NULL if not implemented
#define VX_EXPORT(fn) { #fn, (void *)fn }

// ---- the process ----------------------------------------------------------------------------------------------
typedef struct {
    uint8_t  *image_base;
    size_t    image_size;
    char    **argv;
    int       argc;
    char    **envp;
    char     *command_line;
    WCHAR    *command_line_w;
    uint32_t  pdata_rva, pdata_size;   // the exception directory (.pdata RUNTIME_FUNCTIONs)
} vx_process;
extern vx_process vx_proc;

// TEB layout (the fields Windows code reads directly). Allocated per thread, 8 KB.
enum {
    TEB_EXCEPTION_LIST = 0x00, TEB_STACK_BASE = 0x08, TEB_STACK_LIMIT = 0x10, TEB_SELF = 0x30,
    TEB_CLIENT_ID = 0x40, TEB_TLS_POINTER = 0x58, TEB_PEB = 0x60, TEB_LAST_ERROR = 0x68,
    TEB_TLS_SLOTS = 0x1480, TEB_SIZE = 0x2000,
};
enum { PEB_IMAGE_BASE = 0x10, PEB_PROCESS_HEAP = 0x30, PEB_SIZE = 0x1000 };
uint8_t *vx_teb(void);                 // this thread's TEB (creates it on first use)
void     vx_thread_attach(void);       // TEB + gs slots + TLS for the calling thread
void     vx_call_tls_callbacks(DWORD reason);   // the image's TLS callbacks (1 process attach, 2/3 thread attach/detach)
DWORD    vx_current_tid(void);

// Last error lives in the TEB like on Windows.
void  vx_set_last_error(DWORD e);
DWORD vx_get_last_error(void);

// Handles: small table; stdio handles are the pseudo values Windows uses.
int vx_handle_fd(HANDLE h);            // -1 if not a file handle

// Diagnostics (stderr); VXRT_TRACE=1 also logs every implemented call that opts in.
void vx_log(const char *fmt, ...);
extern int vx_trace;

// Module registration entry points (one per source file).
void vx_kernel32_init(void);
void vx_ucrt_init(void);
void vx_sync_init(void);               // threads and kernel objects (sync.c)
void vx_seh_init(void);                // exceptions (seh.c)
void vx_seh_install_signals(void);     // CPU faults become Windows exceptions

// Formatting with a Windows va_list (a pointer to 8-byte argument slots).
int vx_format(char *out, size_t cap, const char *fmt, const char *ap);
#endif
