// The session around the windows (user32, shcore): window stations and desktops, power notifications, DPI awareness.
//
// One interactive session: the window station WinSta0 and its desktop Default, where every thread starts and which is
// the input desktop. Programs may make more (Chromium's sandbox creates a window station and a desktop for its child
// processes and passes their names on); they exist here as named objects with handles, nothing more: all windows are
// on the Mac's one screen, whichever desktop their thread is on. Each object goes when its last handle closes, as on
// Windows (WinSta0 and Default stay).
//
// The process is an ordinary user's (system.c's token): it may not name a window station it creates.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "vx.h"
#include "ui.h"

#define ERROR_FILE_NOT_FOUND       2
#define ERROR_ACCESS_DENIED        5
#define ERROR_INVALID_HANDLE       6
#define ERROR_NOT_ENOUGH_MEMORY    8
#define ERROR_INVALID_PARAMETER    87
#define ERROR_INSUFFICIENT_BUFFER  122
#define ERROR_INVALID_NAME         123
#define ERROR_BUSY                 170
#define ERROR_ALREADY_EXISTS       183
#define ERROR_INVALID_WINDOW_HANDLE 1400

// ---- window stations and desktops --------------------------------------------------------------------------------------
typedef struct uobj uobj;
struct uobj {
    int desktop;                    // 0: a window station
    char *name;                     // UTF-8
    uobj *station;                  // a desktop's window station (which it holds a reference on)
    DWORD flags;                    // USEROBJECTFLAGS.dwFlags: WSF_VISIBLE, DF_ALLOWOTHERACCOUNTHOOK
    int refs;                       // its handles' data, its desktops, one for WinSta0 and Default
    uobj *next;
};
typedef struct { uobj *o; BOOL inherit; } uhandle;      // the data of one handle (and of its duplicates)

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static uobj *g_objects;
static uobj g_winsta0 = { 0, "WinSta0", NULL, 1 /* WSF_VISIBLE */, 1, NULL };
static uobj g_default = { 1, "Default", &g_winsta0, 1 /* DF_ALLOWOTHERACCOUNTHOOK */, 1, NULL };
static HANDLE g_process_station;    // SetProcessWindowStation's (first: a handle to WinSta0)
static HANDLE g_initial_desktop;    // every thread's until it calls SetThreadDesktop
static __thread HANDLE t_desktop;
static struct { DWORD tid; HANDLE desk; } g_thread_desktops[64];   // what GetThreadDesktop tells of other threads

// Releases a reference; the last one unlinks and frees the object (never WinSta0 or Default). Called with g_lock.
static void release(uobj *o) {
    if (--o->refs) return;
    for (uobj **p = &g_objects; *p; p = &(*p)->next) if (*p == o) { *p = o->next; break; }
    if (o->station) release(o->station);
    free(o->name);
    free(o);
}
// The handle's data is destroyed with its last handle (sync.c calls this with its own lock held: never the other
// way round, so vx_handle_* is never called with g_lock held).
static void handle_destroy(void *data) {
    uhandle *u = data;
    pthread_mutex_lock(&g_lock);
    release(u->o);
    pthread_mutex_unlock(&g_lock);
    free(u);
}
// A new handle to o, which the caller holds a reference on for it.
static HANDLE new_handle(uobj *o, BOOL inherit) {
    uhandle *u = calloc(1, sizeof *u);
    u->o = o; u->inherit = inherit;
    HANDLE h = vx_handle_new(o->desktop ? VX_HANDLE_DESKTOP : VX_HANDLE_WINSTA, u, handle_destroy);
    if (!h) { pthread_mutex_lock(&g_lock); release(o); pthread_mutex_unlock(&g_lock); free(u); vx_set_last_error(ERROR_NOT_ENOUGH_MEMORY); }
    return h;
}
static uhandle *handle_of(HANDLE h, int desktop) { return vx_handle_data(h, desktop ? VX_HANDLE_DESKTOP : VX_HANDLE_WINSTA); }

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static void make_initial(void) {
    g_winsta0.next = &g_default; g_objects = &g_winsta0;
    g_winsta0.refs++; g_process_station = new_handle(&g_winsta0, 0);
    g_default.refs++; g_initial_desktop = new_handle(&g_default, 0);
}
static void initial(void) { pthread_once(&g_once, make_initial); }

static uobj *find(int desktop, const char *name, uobj *station) {         // with g_lock
    for (uobj *o = g_objects; o; o = o->next)
        if (o->desktop == desktop && o->station == station && !strcasecmp(o->name, name)) return o;
    return NULL;
}
static int valid_name(const char *name) { return name && *name && !strchr(name, '\\') && strlen(name) < 260; }
static uobj *current_station(void) {
    initial();
    uhandle *u = handle_of(g_process_station, 0);
    return u ? u->o : &g_winsta0;
}
static HANDLE thread_desktop(void) { initial(); return t_desktop ? t_desktop : g_initial_desktop; }

// Creates the named object or opens it ("create_only": it must not exist yet). NULL with the last error set.
static HANDLE create(int desktop, const char *name, uobj *station, DWORD flags, int create_only, BOOL inherit) {
    pthread_mutex_lock(&g_lock);
    uobj *o = find(desktop, name, station);
    int existed = o != NULL;
    if (existed && create_only) { pthread_mutex_unlock(&g_lock); vx_set_last_error(ERROR_ALREADY_EXISTS); return NULL; }
    if (!o) {
        o = calloc(1, sizeof *o);
        o->desktop = desktop; o->name = strdup(name); o->station = station; o->flags = flags;
        if (station) station->refs++;
        o->next = g_objects; g_objects = o;
    }
    o->refs++;
    pthread_mutex_unlock(&g_lock);
    HANDLE h = new_handle(o, inherit);
    if (h) vx_set_last_error(existed ? ERROR_ALREADY_EXISTS : 0);
    return h;
}
static HANDLE open_named(int desktop, const char *name, uobj *station, BOOL inherit) {
    if (!name) { vx_set_last_error(ERROR_INVALID_PARAMETER); return NULL; }
    pthread_mutex_lock(&g_lock);
    uobj *o = find(desktop, name, station);
    if (o) o->refs++;
    pthread_mutex_unlock(&g_lock);
    if (!o) { vx_set_last_error(ERROR_FILE_NOT_FOUND); return NULL; }
    return new_handle(o, inherit);
}
// SECURITY_ATTRIBUTES (x64): nLength, lpSecurityDescriptor, bInheritHandle.
static BOOL inherit_of(const uint8_t *sa) { return sa && *(const BOOL *)(sa + 16); }

// CreateWindowStation: only an administrator may name it; without a name it is the logon session's
// ("Service-0x<high>-<low>$", the session's LUID: 0:0 for system.c's token).
static WINAPI HANDLE CreateWindowStationA(const char *name, DWORD flags, DWORD access, const uint8_t *sa) {
    (void)access;
    if (name && *name) { vx_set_last_error(ERROR_ACCESS_DENIED); return NULL; }
    initial();
    return create(0, "Service-0x0-0$", NULL, 0, flags & 1 /* CWF_CREATE_ONLY */, inherit_of(sa));
}
static WINAPI HANDLE CreateWindowStationW(const WCHAR *name, DWORD flags, DWORD access, const uint8_t *sa) {
    if (name && *name) { vx_set_last_error(ERROR_ACCESS_DENIED); return NULL; }
    return CreateWindowStationA(NULL, flags, access, sa);
}
static WINAPI HANDLE OpenWindowStationA(const char *name, BOOL inherit, DWORD access) { (void)access; initial(); return open_named(0, name, NULL, inherit); }
static WINAPI HANDLE OpenWindowStationW(const WCHAR *name, BOOL inherit, DWORD access) {
    char *n = vx_utf8_dup(name);
    HANDLE h = OpenWindowStationA(n, inherit, access);
    free(n);
    return h;
}
// The handle GetProcessWindowStation returns belongs to the system: programs do not close it.
static WINAPI HANDLE GetProcessWindowStation(void) { initial(); return g_process_station; }
static WINAPI BOOL SetProcessWindowStation(HANDLE h) {
    if (!handle_of(h, 0)) { vx_set_last_error(ERROR_INVALID_HANDLE); return 0; }
    initial();
    g_process_station = h;
    return 1;
}
static WINAPI BOOL CloseWindowStation(HANDLE h) {
    if (!handle_of(h, 0)) { vx_set_last_error(ERROR_INVALID_HANDLE); return 0; }
    if (h == g_process_station) { vx_set_last_error(ERROR_BUSY); return 0; }     // the process's own: it stays
    return vx_handle_close(h);
}

// Desktops are made and opened in the process's window station.
static WINAPI HANDLE CreateDesktopA(const char *name, const char *device, void *mode, DWORD flags, DWORD access, const uint8_t *sa) {
    (void)access;
    if (device || mode) { vx_set_last_error(ERROR_INVALID_PARAMETER); return NULL; }
    if (!valid_name(name)) { vx_set_last_error(ERROR_INVALID_NAME); return NULL; }
    return create(1, name, current_station(), flags & 1 /* DF_ALLOWOTHERACCOUNTHOOK */, 0, inherit_of(sa));
}
static WINAPI HANDLE CreateDesktopW(const WCHAR *name, const WCHAR *device, void *mode, DWORD flags, DWORD access, const uint8_t *sa) {
    if (device || mode) { vx_set_last_error(ERROR_INVALID_PARAMETER); return NULL; }
    char *n = vx_utf8_dup(name);
    HANDLE h = CreateDesktopA(n, NULL, NULL, flags, access, sa);
    free(n);
    return h;
}
static WINAPI HANDLE CreateDesktopExW(const WCHAR *name, const WCHAR *device, void *mode, DWORD flags, DWORD access, const uint8_t *sa, uint32_t heap_kb, void *reserved) {
    (void)heap_kb; (void)reserved;
    return CreateDesktopW(name, device, mode, flags, access, sa);
}
static WINAPI HANDLE OpenDesktopA(const char *name, DWORD flags, BOOL inherit, DWORD access) { (void)flags; (void)access; return open_named(1, name, current_station(), inherit); }
static WINAPI HANDLE OpenDesktopW(const WCHAR *name, DWORD flags, BOOL inherit, DWORD access) {
    char *n = vx_utf8_dup(name);
    HANDLE h = OpenDesktopA(n, flags, inherit, access);
    free(n);
    return h;
}
// The desktop receiving the user's input: WinSta0's Default (a new handle the caller closes). Its name matters:
// SteamVR's compositor shows its "display locked" colour unless the input desktop is Default.
static WINAPI HANDLE OpenInputDesktop(DWORD flags, BOOL inherit, DWORD access) {
    (void)flags; (void)access;
    initial();
    pthread_mutex_lock(&g_lock);
    g_default.refs++;
    pthread_mutex_unlock(&g_lock);
    return new_handle(&g_default, inherit);
}
static WINAPI HANDLE GetThreadDesktop(DWORD tid) {
    if (!tid) { vx_set_last_error(ERROR_INVALID_PARAMETER); return NULL; }
    if (tid == vx_current_tid()) return thread_desktop();
    HANDLE h = NULL;
    pthread_mutex_lock(&g_lock);
    for (size_t i = 0; i < sizeof g_thread_desktops / sizeof *g_thread_desktops; i++)
        if (g_thread_desktops[i].tid == tid) h = g_thread_desktops[i].desk;
    pthread_mutex_unlock(&g_lock);
    initial();
    return h ? h : g_initial_desktop;
}
// A thread's entry goes when it ends (a pthread key's destructor, with the thread's id as its value).
static pthread_key_t g_desk_key;
static pthread_once_t g_desk_key_once = PTHREAD_ONCE_INIT;
static void forget_thread(void *value) {
    DWORD tid = (DWORD)(uintptr_t)value;
    pthread_mutex_lock(&g_lock);
    for (size_t i = 0; i < sizeof g_thread_desktops / sizeof *g_thread_desktops; i++)
        if (g_thread_desktops[i].tid == tid) g_thread_desktops[i].tid = 0;
    pthread_mutex_unlock(&g_lock);
}
static void make_desk_key(void) { pthread_key_create(&g_desk_key, forget_thread); }
static WINAPI BOOL count_window(HANDLE hwnd, LPARAM n) { (void)hwnd; ++*(int *)(intptr_t)n; return 0; }
// A thread that has windows stays on its desktop (Windows: also one with hooks, which this does not track).
static WINAPI BOOL SetThreadDesktop(HANDLE h) {
    uhandle *u = handle_of(h, 1), *cur = handle_of(thread_desktop(), 1);
    if (!u) { vx_set_last_error(ERROR_INVALID_HANDLE); return 0; }
    if (!cur || u->o != cur->o) {
        BOOL (WINAPI *each)(DWORD, void *, LPARAM) = (BOOL (WINAPI *)(DWORD, void *, LPARAM))vx_find_export("user32.dll", "EnumThreadWindows");
        int windows = 0;
        if (each) each(vx_current_tid(), (void *)count_window, (LPARAM)(intptr_t)&windows);
        if (windows) { vx_set_last_error(ERROR_BUSY); return 0; }
    }
    t_desktop = h;
    DWORD tid = vx_current_tid();
    pthread_mutex_lock(&g_lock);
    size_t i = 0, n = sizeof g_thread_desktops / sizeof *g_thread_desktops, free_slot = n;
    for (; i < n && g_thread_desktops[i].tid != tid; i++) if (!g_thread_desktops[i].tid && free_slot == n) free_slot = i;
    if (i == n) i = free_slot;
    if (i < n) { g_thread_desktops[i].tid = tid; g_thread_desktops[i].desk = h; }
    pthread_mutex_unlock(&g_lock);
    pthread_once(&g_desk_key_once, make_desk_key);
    pthread_setspecific(g_desk_key, (void *)(uintptr_t)tid);
    return 1;
}
// A desktop a thread of this process is on stays open.
static WINAPI BOOL CloseDesktop(HANDLE h) {
    if (!handle_of(h, 1)) { vx_set_last_error(ERROR_INVALID_HANDLE); return 0; }
    int busy = h == g_initial_desktop || h == t_desktop;
    pthread_mutex_lock(&g_lock);
    for (size_t i = 0; i < sizeof g_thread_desktops / sizeof *g_thread_desktops; i++)
        if (g_thread_desktops[i].tid && g_thread_desktops[i].desk == h) busy = 1;
    pthread_mutex_unlock(&g_lock);
    if (busy) { vx_set_last_error(ERROR_BUSY); return 0; }
    return vx_handle_close(h);
}
static WINAPI BOOL SwitchDesktop(HANDLE h) {
    uhandle *u = handle_of(h, 1);
    if (!u) { vx_set_last_error(ERROR_INVALID_HANDLE); return 0; }
    if (u->o != &g_default) { vx_set_last_error(ERROR_ACCESS_DENIED); return 0; }   // the Mac shows only the one
    return 1;
}

// GetUserObjectInformation: UOI_FLAGS (USEROBJECTFLAGS: fInherit, fReserved, dwFlags), UOI_NAME, UOI_TYPE,
// UOI_USER_SID (none: no user's SID is associated here), UOI_HEAPSIZE (a desktop's heap in KB), UOI_IO (whether it
// receives the user's input).
static BOOL user_object_info(HANDLE h, int index, void *out, DWORD n, DWORD *need, int wide) {
    uhandle *u = handle_of(h, 0);
    if (!u) u = handle_of(h, 1);
    if (!u) { vx_set_last_error(ERROR_INVALID_HANDLE); return 0; }
    uobj *o = u->o;
    DWORD value, size = 4;
    switch (index) {
    case 1:                                                       // UOI_FLAGS
        if (need) *need = 12;
        if (!out || n < 12) { vx_set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
        ((DWORD *)out)[0] = (DWORD)u->inherit; ((DWORD *)out)[1] = 0; ((DWORD *)out)[2] = o->flags;
        return 1;
    case 2: case 3: {                                             // UOI_NAME, UOI_TYPE
        const char *text = index == 2 ? o->name : o->desktop ? "Desktop" : "WindowStation";
        size_t len = wide ? vx_utf8_to_utf16(text, strlen(text), NULL, 0) : strlen(text);
        DWORD bytes = (DWORD)(len + 1) * (wide ? 2 : 1);
        if (need) *need = bytes;
        if (!out || n < bytes) { vx_set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
        if (wide) vx_return_string_w(text, out, n / 2); else vx_return_string_a(text, out, n);
        return 1;
    }
    case 4: if (need) *need = 0; return 1;                       // UOI_USER_SID: none
    case 5:                                                       // UOI_HEAPSIZE
        if (!o->desktop) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
        value = 20480;
        break;
    case 6: value = o == &g_default || o == &g_winsta0; break;   // UOI_IO
    default: vx_set_last_error(ERROR_INVALID_PARAMETER); return 0;
    }
    if (need) *need = size;
    if (!out || n < size) { vx_set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    *(DWORD *)out = value;
    return 1;
}
static WINAPI BOOL GetUserObjectInformationW(HANDLE h, int index, void *out, DWORD n, DWORD *need) { return user_object_info(h, index, out, n, need, 1); }
static WINAPI BOOL GetUserObjectInformationA(HANDLE h, int index, void *out, DWORD n, DWORD *need) { return user_object_info(h, index, out, n, need, 0); }
// SetUserObjectInformation: an object's flags and its handle's inheritance; and on the current process's
// pseudo-handle, whether exceptions in timer callbacks are swallowed (UOI_TIMERPROC_EXCEPTION_SUPPRESSION: kept, as
// this runtime's timers never swallowed them).
static BOOL g_timerproc_suppression = 1;
static WINAPI BOOL SetUserObjectInformationW(HANDLE h, int index, const void *in, DWORD n) {
    if (index == 7 && (intptr_t)h == -1) {                        // UOI_TIMERPROC_EXCEPTION_SUPPRESSION
        if (!in || n < 4) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
        g_timerproc_suppression = *(const BOOL *)in;
        return 1;
    }
    uhandle *u = handle_of(h, 0);
    if (!u) u = handle_of(h, 1);
    if (!u) { vx_set_last_error(ERROR_INVALID_HANDLE); return 0; }
    if (index != 1 || !in || n < 12) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    u->inherit = ((const DWORD *)in)[0] != 0;
    pthread_mutex_lock(&g_lock);
    u->o->flags = ((const DWORD *)in)[2];
    pthread_mutex_unlock(&g_lock);
    return 1;
}
static WINAPI BOOL SetUserObjectInformationA(HANDLE h, int index, const void *in, DWORD n) { return SetUserObjectInformationW(h, index, in, n); }

// ---- power notifications -----------------------------------------------------------------------------------------------
// Registrations are kept and can be undone; none is ever notified (the Mac's sleep and wake are not passed on yet).
typedef struct { int used, settings; HANDLE recipient; DWORD flags; uint8_t guid[16]; } power_note;
static power_note g_notes[64];
static HANDLE add_note(int settings, HANDLE recipient, DWORD flags, const void *guid) {
    pthread_mutex_lock(&g_lock);
    power_note *p = NULL;
    for (size_t i = 0; i < sizeof g_notes / sizeof *g_notes && !p; i++) if (!g_notes[i].used) p = &g_notes[i];
    if (p) { p->used = 1; p->settings = settings; p->recipient = recipient; p->flags = flags; if (guid) memcpy(p->guid, guid, 16); }
    pthread_mutex_unlock(&g_lock);
    if (!p) vx_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
    return p;
}
static BOOL remove_note(HANDLE h, int settings) {
    power_note *p = h;
    BOOL ok = p >= g_notes && p < g_notes + sizeof g_notes / sizeof *g_notes && (((char *)p - (char *)g_notes) % sizeof *p) == 0;
    pthread_mutex_lock(&g_lock);
    ok = ok && p->used && p->settings == settings;
    if (ok) p->used = 0;
    pthread_mutex_unlock(&g_lock);
    if (!ok) vx_set_last_error(ERROR_INVALID_HANDLE);
    return ok;
}
// DEVICE_NOTIFY_WINDOW_HANDLE (0): recipient is a window; DEVICE_NOTIFY_CALLBACK (2): a DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS
// (callback, context).
static WINAPI HANDLE RegisterSuspendResumeNotification(HANDLE recipient, DWORD flags) {
    if (!recipient || (flags != 0 && flags != 2) || (flags == 2 && !*(void *const *)recipient)) { vx_set_last_error(ERROR_INVALID_PARAMETER); return NULL; }
    return add_note(0, recipient, flags, NULL);
}
static WINAPI BOOL UnregisterSuspendResumeNotification(HANDLE h) { return remove_note(h, 0); }
// DEVICE_NOTIFY_WINDOW_HANDLE (0) or DEVICE_NOTIFY_SERVICE_HANDLE (1), and the power setting's GUID.
static WINAPI HANDLE RegisterPowerSettingNotification(HANDLE recipient, const void *guid, DWORD flags) {
    if (!recipient || !guid || flags > 1) { vx_set_last_error(ERROR_INVALID_PARAMETER); return NULL; }
    return add_note(1, recipient, flags, guid);
}
static WINAPI BOOL UnregisterPowerSettingNotification(HANDLE h) { return remove_note(h, 1); }

// ---- DPI awareness ----------------------------------------------------------------------------------------------------
// The Mac's points are the program's pixels: every monitor and window is at 96 DPI (user32.c) whatever the awareness.
// The awareness is still kept as on Windows: set once per process (setting it again fails), and per thread on top.
// Contexts are the DPI_AWARENESS_CONTEXT values: -1 unaware, -2 system aware, -3 per monitor, -4 per monitor v2,
// -5 unaware with GDI scaling; awareness (DPI_AWARENESS, PROCESS_DPI_AWARENESS): 0 unaware, 1 system, 2 per monitor.
static intptr_t g_process_dpi;                 // 0 until set (unaware)
static __thread intptr_t t_thread_dpi;         // 0: the process's
static int dpi_valid(intptr_t c) { return c <= -1 && c >= -5; }
static int dpi_awareness(intptr_t c) { return c == -2 ? 1 : c == -3 || c == -4 ? 2 : dpi_valid(c) ? 0 : -1; }
static intptr_t process_dpi(void) { intptr_t c = __atomic_load_n(&g_process_dpi, __ATOMIC_ACQUIRE); return c ? c : -1; }
static intptr_t thread_dpi(void) { return t_thread_dpi ? t_thread_dpi : process_dpi(); }
static BOOL set_process_dpi(intptr_t c) { intptr_t unset = 0; return __atomic_compare_exchange_n(&g_process_dpi, &unset, c, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE); }

static WINAPI BOOL SetProcessDpiAwarenessContext(HANDLE c) {
    if (!dpi_valid((intptr_t)c)) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    if (!set_process_dpi((intptr_t)c)) { vx_set_last_error(ERROR_ACCESS_DENIED); return 0; }
    return 1;
}
static WINAPI BOOL SetProcessDPIAware(void) { set_process_dpi(-2); return 1; }
static WINAPI BOOL IsProcessDPIAware(void) { return dpi_awareness(process_dpi()) != 0; }
static WINAPI HANDLE GetThreadDpiAwarenessContext(void) { return (HANDLE)thread_dpi(); }
static WINAPI HANDLE SetThreadDpiAwarenessContext(HANDLE c) {
    if (!dpi_valid((intptr_t)c)) { vx_set_last_error(ERROR_INVALID_PARAMETER); return NULL; }
    intptr_t old = thread_dpi();
    t_thread_dpi = (intptr_t)c;
    return (HANDLE)old;
}
static WINAPI HANDLE GetWindowDpiAwarenessContext(HANDLE hwnd) {
    if (!vx_window_get(hwnd)) { vx_set_last_error(ERROR_INVALID_WINDOW_HANDLE); return NULL; }
    return (HANDLE)process_dpi();
}
static WINAPI HANDLE GetDpiAwarenessContextForProcess(HANDLE process) {
    // Another process's is not known here: the default (unaware) for it.
    return (HANDLE)(!process || (intptr_t)process == -1 ? process_dpi() : -1);
}
static WINAPI int GetAwarenessFromDpiAwarenessContext(HANDLE c) { return dpi_awareness((intptr_t)c); }
static WINAPI BOOL AreDpiAwarenessContextsEqual(HANDLE a, HANDLE b) { return dpi_valid((intptr_t)a) && a == b; }
static WINAPI BOOL IsValidDpiAwarenessContext(HANDLE c) { return dpi_valid((intptr_t)c); }
static WINAPI uint32_t GetDpiFromDpiAwarenessContext(HANDLE c) { int a = dpi_awareness((intptr_t)c); return a == 0 || a == 1 ? 96 : 0; }
static WINAPI BOOL EnableNonClientDpiScaling(HANDLE hwnd) {
    if (!vx_window_get(hwnd)) { vx_set_last_error(ERROR_INVALID_WINDOW_HANDLE); return 0; }
    return 1;
}

// shcore (and api-ms-win-shcore-scaling): HRESULTs.
#define S_OK 0
#define E_INVALIDARG ((LONG)0x80070057)
#define E_ACCESSDENIED ((LONG)0x80070005)
static WINAPI LONG SetProcessDpiAwareness(int awareness) {
    static const intptr_t context[] = { -1, -2, -3 };
    if (awareness < 0 || awareness > 2) return E_INVALIDARG;
    return set_process_dpi(context[awareness]) ? S_OK : E_ACCESSDENIED;
}
static WINAPI LONG GetProcessDpiAwareness(HANDLE process, int *awareness) {
    if (!awareness) return E_INVALIDARG;
    *awareness = dpi_awareness((intptr_t)GetDpiAwarenessContextForProcess(process));
    return S_OK;
}
// MDT_EFFECTIVE_DPI (0), MDT_ANGULAR_DPI (1), MDT_RAW_DPI (2): 96, the Mac's points being the pixels.
static WINAPI LONG GetDpiForMonitor(HANDLE monitor, int type, uint32_t *x, uint32_t *y) {
    if (!monitor || type < 0 || type > 2 || !x || !y) return E_INVALIDARG;
    *x = *y = 96;
    return S_OK;
}
static WINAPI LONG GetScaleFactorForMonitor(HANDLE monitor, int *scale) {
    if (!monitor || !scale) return E_INVALIDARG;
    *scale = 100;                                                  // SCALE_100_PERCENT
    return S_OK;
}

// SetCoalescableTimer: SetTimer, without the tolerance (timers here are never coalesced).
static WINAPI uint64_t SetCoalescableTimer(HANDLE hwnd, uint64_t id, uint32_t ms, void *proc, uint32_t tolerance) {
    (void)tolerance;
    static uint64_t (WINAPI *set_timer)(HANDLE, uint64_t, uint32_t, void *);
    if (!set_timer) set_timer = (uint64_t (WINAPI *)(HANDLE, uint64_t, uint32_t, void *))vx_find_export("user32.dll", "SetTimer");
    return set_timer(hwnd, id, ms, proc);
}

static const vx_export user32[] = {
    VX_EXPORT(CreateWindowStationA), VX_EXPORT(CreateWindowStationW), VX_EXPORT(OpenWindowStationA), VX_EXPORT(OpenWindowStationW),
    VX_EXPORT(GetProcessWindowStation), VX_EXPORT(SetProcessWindowStation), VX_EXPORT(CloseWindowStation),
    VX_EXPORT(CreateDesktopA), VX_EXPORT(CreateDesktopW), VX_EXPORT(CreateDesktopExW), VX_EXPORT(OpenDesktopA), VX_EXPORT(OpenDesktopW),
    VX_EXPORT(OpenInputDesktop), VX_EXPORT(GetThreadDesktop), VX_EXPORT(SetThreadDesktop), VX_EXPORT(CloseDesktop), VX_EXPORT(SwitchDesktop),
    VX_EXPORT(GetUserObjectInformationW), VX_EXPORT(GetUserObjectInformationA), VX_EXPORT(SetUserObjectInformationW),
    VX_EXPORT(SetUserObjectInformationA),
    VX_EXPORT(RegisterSuspendResumeNotification), VX_EXPORT(UnregisterSuspendResumeNotification),
    VX_EXPORT(RegisterPowerSettingNotification), VX_EXPORT(UnregisterPowerSettingNotification),
    VX_EXPORT(SetProcessDpiAwarenessContext), VX_EXPORT(SetProcessDPIAware), VX_EXPORT(IsProcessDPIAware),
    VX_EXPORT(GetThreadDpiAwarenessContext), VX_EXPORT(SetThreadDpiAwarenessContext), VX_EXPORT(GetWindowDpiAwarenessContext),
    VX_EXPORT(GetDpiAwarenessContextForProcess), VX_EXPORT(GetAwarenessFromDpiAwarenessContext), VX_EXPORT(AreDpiAwarenessContextsEqual),
    VX_EXPORT(IsValidDpiAwarenessContext), VX_EXPORT(GetDpiFromDpiAwarenessContext), VX_EXPORT(EnableNonClientDpiScaling),
    VX_EXPORT(SetCoalescableTimer),
};
static const vx_export shcore[] = {
    VX_EXPORT(SetProcessDpiAwareness), VX_EXPORT(GetProcessDpiAwareness), VX_EXPORT(GetDpiForMonitor), VX_EXPORT(GetScaleFactorForMonitor),
};
void vx_winsta_init(void) {
    vx_register_module("user32.dll", user32, sizeof user32 / sizeof user32[0]);
    vx_register_module("shcore.dll", shcore, sizeof shcore / sizeof shcore[0]);
}

#ifdef __aarch64__
#include "thunks/winsta.c.inc"
#endif
