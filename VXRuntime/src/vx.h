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
} vx_process;
extern vx_process vx_proc;

// A loaded PE image: the program or one of its DLLs.
typedef struct {
    char      name[64];                // file name, e.g. "steam_api64.dll"
    char     *unix_path, *windows_path;
    uint8_t  *base;
    size_t    size;
    uint32_t  entry_rva, export_rva, export_size;
    uint32_t  pdata_rva, pdata_size;   // the exception directory (.pdata RUNTIME_FUNCTIONs)
    uint32_t  tls_rva;                 // the TLS directory, 0 if none
    DWORD     tls_index;               // ~0u if none
    int       is_dll, refs, initialized, no_thread_calls;
} vx_image;
const vx_image *vx_image_at(uint64_t pc);    // the image containing an address, or NULL

// TEB layout (the fields Windows code reads directly). Allocated per thread, 8 KB.
enum {
    TEB_EXCEPTION_LIST = 0x00, TEB_STACK_BASE = 0x08, TEB_STACK_LIMIT = 0x10, TEB_SELF = 0x30,
    TEB_CLIENT_ID = 0x40, TEB_TLS_POINTER = 0x58, TEB_PEB = 0x60, TEB_LAST_ERROR = 0x68,
    TEB_TLS_SLOTS = 0x1480, TEB_SIZE = 0x2000,
};
enum { PEB_IMAGE_BASE = 0x10, PEB_PROCESS_HEAP = 0x30, PEB_SIZE = 0x1000 };
uint8_t *vx_teb(void);                 // this thread's TEB (creates it on first use)
void     vx_thread_attach(void);       // TEB + gs slots + TLS for the calling thread
void     vx_thread_detach(void);       // frees the calling thread's TEB and TLS (after its detach notifications)
void     vx_notify_thread(DWORD reason);   // TLS callbacks and DllMain for thread attach (2) / detach (3)
DWORD    vx_current_tid(void);

// Last error lives in the TEB like on Windows.
void  vx_set_last_error(DWORD e);
DWORD vx_get_last_error(void);

// Handles: small table; stdio handles are the pseudo values Windows uses.
int vx_handle_fd(HANDLE h);            // -1 if not a file handle
// Other kernel objects owned by a module (find handles, ...): `destroy` runs when the last handle closes.
enum { VX_HANDLE_FIND = 1, VX_HANDLE_MAPPING };
HANDLE vx_handle_new(int kind, void *data, void (*destroy)(void *));
void  *vx_handle_data(HANDLE h, int kind);    // NULL unless h is a handle of that kind
BOOL   vx_handle_close(HANDLE h);

// Diagnostics (stderr); VXRT_TRACE=1 also logs every implemented call that opts in.
void vx_log(const char *fmt, ...);
extern int vx_trace;

// Module registration entry points (one per source file).
void vx_kernel32_init(void);
void vx_ucrt_init(void);
void vx_sync_init(void);               // threads and kernel objects (sync.c)
void vx_seh_init(void);                // exceptions (seh.c)
void vx_seh_install_signals(void);     // CPU faults become Windows exceptions
void vx_text_init(void);               // code pages, environment (text.c)
void vx_files_init(void);              // files and paths (files.c)
void vx_registry_init(void);           // advapi32 registry (registry.c)

// Text (text.c). Counts are in code units; the converters return the length needed and write at most cap units.
size_t vx_utf8_to_utf16(const char *s, size_t n, WCHAR *out, size_t cap);
size_t vx_utf16_to_utf8(const WCHAR *w, size_t n, char *out, size_t cap);
size_t vx_wcslen(const WCHAR *w);
char  *vx_utf8_dup(const WCHAR *w);    // malloc'd, NULL for NULL
WCHAR *vx_utf16_dup(const char *s);
DWORD  vx_return_string_a(const char *s, char *buf, DWORD cap);    // Windows' "copy out or report the size"
DWORD  vx_return_string_w(const char *s, WCHAR *buf, DWORD cap);

// Paths (files.c): C:\ is $VXRT_ROOT/drive_c, Z:\ is the Mac's root.
const char *vx_root(void);
int   vx_full_path(const char *win, char *out, size_t cap);     // canonical absolute Windows path
int   vx_unix_path(const char *win, char *out, size_t cap);     // 0 (last error set) if unmapped
void  vx_windows_path(const char *unix_path, char *out, size_t cap);
void  vx_get_cwd(char *out, size_t cap);
int   vx_set_cwd(const char *win);
DWORD vx_error_from_errno(int e);

// Formatting with a Windows va_list (a pointer to 8-byte argument slots).
int vx_format(char *out, size_t cap, const char *fmt, const char *ap);
#endif
