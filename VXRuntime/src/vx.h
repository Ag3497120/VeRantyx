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

#ifdef __aarch64__
// The ARM64 build (src/arm64.c): the program is x86-64 code the translator runs; it calls these functions through
// thunks generated from their C signatures (tools/vx_thunks.py), so they are ordinary ARM64 functions.
#define WINAPI
void *vx_x86_return_address(void);          // the program's return address while it calls this runtime
#define VX_RETURN_ADDRESS() vx_x86_return_address()
typedef const char *vx_va_list;              // the program's variadic arguments: 8-byte slots on its stack
extern __thread const char *vx_va_slots;     //   (set by the thunk)
#define vx_va_start(ap, last) ((ap) = vx_va_slots)
#define vx_va_end(ap) ((void)(ap))
#else
#define WINAPI __attribute__((ms_abi))
#define VX_RETURN_ADDRESS() __builtin_return_address(0)
typedef __builtin_ms_va_list vx_va_list;
#define vx_va_start(ap, last) __builtin_ms_va_start(ap, last)
#define vx_va_end(ap) __builtin_ms_va_end(ap)
#endif

typedef uint32_t DWORD;
typedef int32_t  LONG;
#ifndef __OBJC__
typedef int      BOOL;              // Objective-C (cocoa.m) has its own BOOL and calls none of the functions using this one
#endif
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
    int       pinned;                  // imported by another image (or the program): never unloaded
    int       unloaded;                // FreeLibrary'd: detached and forgotten by name, still mapped
} vx_image;
const vx_image *vx_image_at(uint64_t pc);    // the image containing an address, or NULL
int vx_image_list(const vx_image **out, int cap);   // the loaded images (all of them is the return value)
int vx_overlay_file(const char *unix_path, char *out, size_t cap);   // files.c: VXRT_OVERLAY's copy of a file, if any
void vx_wininet_init(void);                  // wininet.m: HTTP and HTTPS on NSURLSession
void vx_winhttp_init(void);                  // winhttp.m: synchronous WinHTTP request handles on NSURLSession
void vx_directinput_init(void);              // directinput.m: DirectInput 8 HID joystick/gamepad polling
void vx_msxml_init(void);                    // msxml.c: MSXML DOMDocument through CoCreateInstance
typedef struct { DWORD lcid; const char *name, *english, *abbrev, *iso639, *iso3166; } vx_locale;   // desktop.c
const vx_locale *vx_locale_by_lcid(DWORD lcid);              // NULL when unknown; 0 and the default LCIDs give the user's
const vx_locale *vx_locale_by_name(const char *name);        // "en-GB" (any case); NULL or "" give the user's
const vx_locale *vx_locale_at(int i);                        // the i-th locale there is, NULL past the last
void vx_extras_init(void);                   // extras.c: smaller functions more programs need
void vx_kernelx_init(void);                  // kernelx.c: kernel32/ntdll the Windows Steam client needs (jobs.c, ntstatus.c)
void vx_jobs_init(void);
void vx_jobs_process_exit(void);             // jobs.c: this process ends: its KILL_ON_JOB_CLOSE jobs end their processes
int vx_jobs_watchdog(void);                  // jobs.c: "vxrt --vxrt-job-watchdog" (main), which does that for any end
enum { VX_HANDLE_JOB = 0x4a01, VX_HANDLE_VOLUME_SEARCH = 0x5601 };   // (their handle kinds)
DWORD vx_ntstatus_to_win32(uint32_t status);        // RtlNtStatusToDosError (ntstatus.c)
uint32_t vx_win32_to_ntstatus(DWORD error);         // the status an error usually comes from
DWORD vx_dll_default_search(void);                  // SetDefaultDllDirectories' flags (0: none), for the loader
int vx_dll_user_directory(int i, char *out, size_t cap);   // AddDllDirectory's i-th directory (Unix path), 0 past the last
void vx_opengl_init(void);                   // opengl.c: opengl32.dll on macOS's OpenGL
int vx_gl_describe_pixel_format(void *pfd, uint32_t size);   // (gdi32's pixel-format calls and SwapBuffers)
int vx_gl_swap(void);
void vx_profile_start(void);                 // profile.c: VXRT_PROFILE=file, a sampling profiler
void vx_profile_held(int delta);             // a keyed mutex taken (+1) or released (-1) by this thread
void vx_profile_finish(void);                // writes the report now (ExitProcess)

// TEB layout (the fields Windows code reads directly). Allocated per thread, 8 KB.
enum {
    TEB_EXCEPTION_LIST = 0x00, TEB_STACK_BASE = 0x08, TEB_STACK_LIMIT = 0x10, TEB_SELF = 0x30,
    TEB_CLIENT_ID = 0x40, TEB_TLS_POINTER = 0x58, TEB_PEB = 0x60, TEB_LAST_ERROR = 0x68,
    TEB_TLS_SLOTS = 0x1480, TEB_SIZE = 0x2000,
};
enum { PEB_IMAGE_BASE = 0x10, PEB_PROCESS_HEAP = 0x30, PEB_SIZE = 0x1000 };
uint8_t *vx_teb(void);                 // this thread's TEB (creates it on first use)
void     vx_thread_attach(void);       // TEB + gs slots + TLS for the calling thread
#ifdef __aarch64__
extern __thread size_t vx_x86_stack_size;   // the x86 stack vx_thread_attach makes (0: 1 MB)
void vx_arm64_init(void);
#endif
// The program's heap blocks: in the ARM64 build its small ones come from the range the vCPU guest allocates from too
// (src/vm_hooks.c, whose free/realloc/malloc_size take them).
#ifdef __aarch64__
void *vxvm_alloc(size_t n);
void *vxvm_zalloc(size_t n);
int vxvm_owns(const void *p);
#define vx_heap_alloc(n) vxvm_alloc(n)
#define vx_heap_zalloc(n) vxvm_zalloc(n)
#define vx_heap_owns(p) vxvm_owns(p)
#else
#define vx_heap_alloc(n) malloc((n) ? (n) : 1)
#define vx_heap_zalloc(n) calloc(1, (n) ? (n) : 1)
#define vx_heap_owns(p) 0
#endif
#ifdef __aarch64__
// The program's memory below 4 GB (vCPU only: the host cannot map there): the host has it at vx_low_base + its
// address, and vx_low_map marks the 64 KB granules handed out (vx_virtual_alloc_below), so that only a pointer into
// them is moved (small handles and resource ids are not). In-process, vx_low_base is 0 and nothing moves.
extern uint64_t vx_low_base;
extern uint8_t vx_low_map[1 << 16];
static inline uint64_t vx_low(uint64_t v) { return !(v >> 32) && vx_low_map[v >> 16] ? v + vx_low_base : v; }
static inline uint64_t vx_unlow(uint64_t v) { return vx_low_base && v - vx_low_base < (1ull << 32) ? v - vx_low_base : v; }
static inline void vx_unlow_at(uint64_t p) {                  // (an output the function stored a pointer through)
    if (!vx_low_base || !p) return;
    uint64_t v = *(const uint64_t *)(uintptr_t)p;
    if (v - vx_low_base < (1ull << 32)) *(uint64_t *)(uintptr_t)p = v - vx_low_base;
}

#define VX_LOW(p) ((void *)(uintptr_t)vx_low((uint64_t)(uintptr_t)(p)))
#else
#define VX_LOW(p) ((void *)(p))
#endif
// A new Metal buffer whose memory a Map will hand the program: the vCPU guest must not keep an older page at that
// address (the kernel unmapped a freed buffer's memory, unseen by src/vm_hooks.c, and a new one can come there).
#ifdef __aarch64__
void xv_unmap(uint64_t va, uint64_t len);
#define vx_guest_forget(p, n) xv_unmap((uint64_t)(uintptr_t)(p), (n))
#else
#define vx_guest_forget(p, n) ((void)0)
#endif
void     vx_thread_detach(void);       // frees the calling thread's TEB and TLS (after its detach notifications)
void     vx_notify_thread(DWORD reason);   // TLS callbacks and DllMain for thread attach (2) / detach (3)
DWORD    vx_current_tid(void);

// Last error lives in the TEB like on Windows.
void  vx_set_last_error(DWORD e);
DWORD vx_get_last_error(void);

// Handles: small table; stdio handles are the pseudo values Windows uses.
int vx_handle_fd(HANDLE h);            // -1 if not a file handle
// Other kernel objects owned by a module (find handles, ...): `destroy` runs when the last handle closes.
enum { VX_HANDLE_FIND = 1, VX_HANDLE_MAPPING, VX_HANDLE_PIPE = 7, VX_HANDLE_WINSTA, VX_HANDLE_DESKTOP, VX_HANDLE_SC_MANAGER };
HANDLE vx_handle_new(int kind, void *data, void (*destroy)(void *));
void  *vx_handle_data(HANDLE h, int kind);    // NULL unless h is a handle of that kind
BOOL   vx_handle_close(HANDLE h);
// An overlapped operation on `file` finished: fills the OVERLAPPED, signals its event, queues the completion packet.
void   vx_io_completed(HANDLE file, void *overlapped, DWORD bytes, DWORD status);
void   vx_set_event(HANDLE event);          // SetEvent for threads that are not Windows threads
void   vx_release_semaphore(HANDLE sem, LONG n);   // ReleaseSemaphore for them (a count past the maximum is dropped)
// A handle's object as NtQueryObject describes it (sync.c): see there. vx_handle_count: the handles of the table.
int    vx_handle_object(HANDLE h, const void **id, const char **type, int *kind, char *name, size_t cap, DWORD *handles);
DWORD  vx_handle_count(void);

// Diagnostics (stderr); VXRT_TRACE=1 also logs every implemented call that opts in.
void vx_log(const char *fmt, ...);
void vx_control_start(const char *path);       // control.c: VXRT_CONTROL, the socket Command Clips talk to
extern int vx_trace;

// Module registration entry points (one per source file).
void vx_kernel32_init(void);
void vx_desktop_init(void);            // the rest of the Win32 surface games link (desktop.c)
void vx_pipes_init(void);              // named pipes on Unix domain sockets (pipes.c)
void *vx_trace_wrap(const char *dll, const char *name, void *fn);   // VXRT_TRACE_CALLS (calltrace.c)
void vx_client_init(void);             // what desktop clients use: certificate stores, bcrypt random, comctl32 (client.c)
void vx_crypt32_init(void);            // certificates, stores, signatures by macOS; wintrust that trusts nothing unverified (crypt32.c)
void vx_dpapi_init(void);              // CryptProtectData/Memory with a key of the VXRT_ROOT, AES-256-GCM (dpapi.c)
void vx_steambridge_init(void);
void vx_services_init(void);           // resources, command lines, random numbers, psapi, C runtime extras (services.c)
void vx_system_init(void);             // processor topology, thread pool, shell folders (system.c)
void vx_security_init(void);           // SIDs, ACLs, security descriptors, SDDL, tokens, AccessCheck (security.c)
void vx_ucrt_init(void);
void vx_sync_init(void);               // threads and kernel objects (sync.c)
void vx_seh_init(void);                // exceptions (seh.c)
void vx_seh_install_signals(void);     // CPU faults become Windows exceptions
void vx_text_init(void);               // code pages, environment (text.c)
void vx_files_init(void);              // files and paths (files.c)
void vx_registry_init(void);           // advapi32 registry (registry.c)
void vx_user32_init(void);             // windows and messages (user32.c, cocoa.m)
void vx_winsta_init(void);             // window stations, desktops, power notifications, DPI awareness (winsta.c)
void vx_tray_init(void);               // the notification area as menu-bar status items, menus, icons (tray.m, menus.m, icons.c)
void vx_scm_init(void);                // the service control manager, with no service installed (scm.c)
void vx_gdi_init(void);                // drawing (gdi.c)
void vx_net_init(void);                // ws2_32 (net.c)
void vx_mm_init(void);                 // winmm, imm32 (mm.c)
void vx_midi_init(void);               // winmm without MIDI devices, waveOutPause/Restart (midi.c)
void vx_iphlp_init(void);              // iphlpapi: interface table, IPv4 adapters, change notifications (iphlp.c)
void vx_winsock_nsp_init(void);        // ws2_32's protocol and name-space catalogs, DNS lookups (winsock_nsp.c)
void vx_winhttp_proxy_init(void);      // WinHttpCrackUrl, status callbacks, the proxy resolver (winhttp_proxy.m)
int vx_winhttp_proxy_close(HANDLE h);  // (winhttp.m's WinHttpCloseHandle: 1 when h was a resolver, closed there)
int vx_winhttp_session_async(HANDLE h); // (winhttp.m: 1 for a session opened with WINHTTP_FLAG_ASYNC; the resolver's)
void vx_d3d11_init(void);              // Direct3D 11 on Metal (d3d11.m)
void vx_d3d9_init(void);               // IDirect3D9: adapters and display modes (d3d9.m)
void vx_dxgi_init(void);               // DXGI (dxgi.m)
void vx_d3d11on12_init(void);          // D3D11On12CreateDevice (d3d11on12.m)
void vx_d3d12_init(void);              // Direct3D 12 on Metal (d3d12.m)
void vx_vulkan_init(void);             // Vulkan 1.0 bootstrap; Vulkan GPU objects are added with their Metal backend
void vx_input_init(void);              // XInput (input.m)
void vx_audio_init(void);              // WASAPI on Core Audio (audio.m; after COM)
void vx_xaudio2_init(void);            // XAudio2 2.8/2.9 on Core Audio (xaudio2.m)
void vx_dwrite_init(void);             // DirectWrite on CoreText (dwrite.m)
void vx_com_init(void);                // ole32, oleaut32 (com.c)
void vx_vcruntime_init(void);          // vcruntime140, MSVC C++ exceptions (vcruntime.c; after seh and ucrt)
void vx_sysdlls_init(void);            // userenv, winspool, dhcpcsvc, secur32 names/LSA, oleaut32 ordinals (sysdlls.c; after COM)
void vx_osinfo_init(void);             // shlwapi's IsOS (osinfo.c)
void vx_pointer_init(void);            // touch and pointer input: a Mac with no touch screen or pen (pointer.c)
void vx_regnotify_init(void);          // RegNotifyChangeKeyValue (regnotify.c)
void vx_section_init(void);            // NtQuerySection (section.c)
void vx_wevtapi_init(void);            // the Windows Event Log query API: no log here, empty channels (wevtapi.c)
void vx_ktime_init(void);              // interrupt-time clocks: QueryUnbiasedInterruptTimePrecise, ... (ktime.c)
void vx_power_init(void);              // powrprof: power information and plans of a desktop on AC power (power.c)
void vx_sysparams_init(void);          // SystemParametersInfo's UI fonts and non-client metrics (sysparams.c)
void vx_dwmapi_init(void);             // the Desktop Window Manager API for windows macOS frames (dwmapi.c)
void vx_winmisc_init(void);            // smaller user32 functions: IsChild, DeferWindowPos, GetLastInputInfo, ... (winmisc.c)
void vx_region_init(void);             // GDI regions: rectangles combined, compared, listed (region.c)
void vx_shlpath_init(void);            // shlwapi path strings: PathFindFileName, PathMatchSpec, ... (shlpath.c)
void vx_dbghelp_init(void);            // dbghelp: StackWalk64, symbols from export tables (dbghelp.c; after seh)
void vx_extra_args(int *argc, char ***argv);   // VXRT_EXTRA_ARGS: a launcher's arguments for one program (launchargs.c)

// Text (text.c). Counts are in code units; the converters return the length needed and write at most cap units.
size_t vx_utf8_to_utf16(const char *s, size_t n, WCHAR *out, size_t cap);
size_t vx_utf16_to_utf8(const WCHAR *w, size_t n, char *out, size_t cap);
size_t vx_wcslen(const WCHAR *w);
char  *vx_utf8_dup(const WCHAR *w);    // malloc'd, NULL for NULL
WCHAR *vx_utf16_dup(const char *s);
DWORD  vx_return_string_a(const char *s, char *buf, DWORD cap);    // Windows' "copy out or report the size"
DWORD  vx_return_string_w(const char *s, WCHAR *buf, DWORD cap);
const char *vx_special_folder_path(int csidl);   // shell folder map shared with managed Environment.GetFolderPath

// Paths (files.c): C:\ is $VXRT_ROOT/drive_c, Z:\ is the Mac's root.
const char *vx_root(void);
int   vx_full_path(const char *win, char *out, size_t cap);     // canonical absolute Windows path
int   vx_unix_path(const char *win, char *out, size_t cap);     // 0 (last error set) if unmapped
int   vx_unix_path_write(const char *win, char *out, size_t cap, int keep_contents);   // writes: through VXRT_OVERLAY
void  vx_windows_path(const char *unix_path, char *out, size_t cap);
void  vx_get_cwd(char *out, size_t cap);
int   vx_set_cwd(const char *win);
DWORD vx_error_from_errno(int e);

// Formatting with a Windows va_list (a pointer to 8-byte argument slots).
int vx_format(char *out, size_t cap, const char *fmt, const char *ap);
#endif
