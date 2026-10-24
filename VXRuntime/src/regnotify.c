// RegNotifyChangeKeyValue (advapi32): be told when a registry key changes. Chromium (the Windows Steam client's
// steamwebhelper) watches keys this way and stops at a CHECK when the function is missing. From Microsoft's
// documentation of the function:
//   - one notification per call: the next change after it, then the caller asks again;
//   - what counts (dwNotifyFilter): REG_NOTIFY_CHANGE_NAME, a subkey added or deleted; _LAST_SET, a value set or
//     deleted; _ATTRIBUTES and _SECURITY, a key's attributes or security descriptor (neither is ever changed in this
//     registry, src/registry.c, so those never fire); with bWatchSubtree, changes below the key count too;
//   - asynchronous (fAsynchronous): the call returns at once and the event is signalled on the change; otherwise it
//     returns when the change has happened;
//   - the event is also signalled when the key's handle is closed, and when the thread that asked ends, unless it
//     asked with REG_NOTIFY_THREAD_AGNOSTIC (Windows 8 and later).
// Also signalled: when the watched key itself is deleted (asking again then fails with ERROR_KEY_DELETED).
// What changes: whatever this process does through the registry functions. The registry is the process's own copy
// of $VXRT_ROOT/registry.txt (src/registry.c), so other processes' changes are not seen, here or anywhere else.
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "vx.h"

enum { ERROR_SUCCESS = 0, ERROR_INVALID_HANDLE = 6, ERROR_INVALID_PARAMETER = 87, ERROR_KEY_DELETED = 1018 };
enum { REG_NOTIFY_CHANGE_NAME = 1, REG_NOTIFY_CHANGE_ATTRIBUTES = 2, REG_NOTIFY_CHANGE_LAST_SET = 4,
       REG_NOTIFY_CHANGE_SECURITY = 8, REG_NOTIFY_THREAD_AGNOSTIC = 0x10000000 };
#define CHANGED_DELETED 0x80000000u                       // (registry.c's: the key itself was deleted)

const void *vx_reg_node(void *hkey, int *deleted);        // registry.c
int vx_reg_within(const void *node, const void *ancestor);
const char *vx_object_type(HANDLE h);                     // sync.c

typedef struct watch {
    const void *key;                                      // the watched key (registry.c's; never freed)
    void *hkey;                                           // the handle it was asked through
    int subtree;
    DWORD filter;
    HANDLE event;                                         // asynchronous: our own handle to the program's event
    DWORD tid; int agnostic;                              // who asked; whether its end matters
    int done;                                             // synchronous: set when it fired
    struct watch *next;
} watch;
static watch *g_watches;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_fired = PTHREAD_COND_INITIALIZER;
static pthread_key_t g_thread_key;
static pthread_once_t g_thread_once = PTHREAD_ONCE_INIT;
static BOOL (WINAPI *rt_DuplicateHandle)(HANDLE, HANDLE, HANDLE, HANDLE *, DWORD, BOOL, DWORD);
static BOOL (WINAPI *rt_CloseHandle)(HANDLE);

// Takes w off the list (g_lock held) and tells its caller.
static void fire(watch **link) {
    watch *w = *link;
    *link = w->next;
    if (w->event) { vx_set_event(w->event); rt_CloseHandle(w->event); free(w); }
    else { w->done = 1; pthread_cond_broadcast(&g_fired); }    // (a waiting caller's own record, on its stack)
}

// registry.c: `key` changed (registry lock held).
void vx_regnotify_changed(const void *key, DWORD what) {
    pthread_mutex_lock(&g_lock);
    for (watch **link = &g_watches; *link;) {
        watch *w = *link;
        int here = w->key == key, below = !here && w->subtree && vx_reg_within(key, w->key);
        if ((here || below) && (what & w->filter & 0xf)) fire(link);
        else if (here && what == CHANGED_DELETED) fire(link);
        else link = &w->next;
    }
    pthread_mutex_unlock(&g_lock);
}
// registry.c: a key handle is being closed (registry lock held).
void vx_regnotify_closed(const void *hkey) {
    pthread_mutex_lock(&g_lock);
    for (watch **link = &g_watches; *link;) if ((*link)->hkey == hkey) fire(link); else link = &(*link)->next;
    pthread_mutex_unlock(&g_lock);
}
// A thread that asked without REG_NOTIFY_THREAD_AGNOSTIC ends: its notifications are signalled.
static void thread_ended(void *unused) {
    (void)unused;
    DWORD tid = vx_current_tid();
    pthread_mutex_lock(&g_lock);
    for (watch **link = &g_watches; *link;)
        if ((*link)->event && (*link)->tid == tid && !(*link)->agnostic) fire(link); else link = &(*link)->next;
    pthread_mutex_unlock(&g_lock);
}
static void make_thread_key(void) { pthread_key_create(&g_thread_key, thread_ended); }

static WINAPI LONG RegNotifyChangeKeyValue(void *hkey, BOOL subtree, DWORD filter, HANDLE event, BOOL async) {
    if (filter & ~(DWORD)(REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_ATTRIBUTES | REG_NOTIFY_CHANGE_LAST_SET |
                          REG_NOTIFY_CHANGE_SECURITY | REG_NOTIFY_THREAD_AGNOSTIC)) return ERROR_INVALID_PARAMETER;
    int deleted = 0;
    const void *key = vx_reg_node(hkey, &deleted);
    if (!key) return ERROR_INVALID_HANDLE;
    if (deleted) return ERROR_KEY_DELETED;
    watch local = { .key = key, .hkey = hkey, .subtree = subtree != 0, .filter = filter, .tid = vx_current_tid(),
                    .agnostic = (filter & REG_NOTIFY_THREAD_AGNOSTIC) != 0 };
    if (async) {
        if (!event) return ERROR_INVALID_PARAMETER;
        const char *type = vx_object_type(event);
        if (!type || strcmp(type, "Event")) return ERROR_INVALID_HANDLE;
        // Our own handle: the program may close its handle meanwhile, and its number may then name another object.
        if (!rt_DuplicateHandle((HANDLE)(intptr_t)-1, event, (HANDLE)(intptr_t)-1, &local.event, 0, 0, 2 /* DUPLICATE_SAME_ACCESS */))
            return ERROR_INVALID_HANDLE;
        if (!local.agnostic) { pthread_once(&g_thread_once, make_thread_key); pthread_setspecific(g_thread_key, (void *)1); }
        watch *w = malloc(sizeof *w);
        *w = local;
        pthread_mutex_lock(&g_lock);
        w->next = g_watches; g_watches = w;
        pthread_mutex_unlock(&g_lock);
        return ERROR_SUCCESS;
    }
    pthread_mutex_lock(&g_lock);                           // synchronous: wait for the change on this thread
    local.next = g_watches; g_watches = &local;
    while (!local.done) pthread_cond_wait(&g_fired, &g_lock);
    pthread_mutex_unlock(&g_lock);
    return ERROR_SUCCESS;
}

static const vx_export advapi32[] = { VX_EXPORT(RegNotifyChangeKeyValue) };

void vx_regnotify_init(void) {
    rt_DuplicateHandle = (BOOL (WINAPI *)(HANDLE, HANDLE, HANDLE, HANDLE *, DWORD, BOOL, DWORD))vx_find_export("kernel32.dll", "DuplicateHandle");
    rt_CloseHandle = (BOOL (WINAPI *)(HANDLE))vx_find_export("kernel32.dll", "CloseHandle");
    vx_register_module("advapi32.dll", advapi32, 1);
    vx_register_module("kernelbase.dll", advapi32, 1);      // (and api-ms-win-core-registry, which resolves to kernel32)
    vx_register_module("kernel32.dll", advapi32, 1);
}

#ifdef __aarch64__
#include "thunks/regnotify.c.inc"
#endif
