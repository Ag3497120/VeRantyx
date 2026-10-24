// iphlpapi's change notifications (src/iphlp.c) when a change completes a request while another thread cancels it or
// starts a new request on the same OVERLAPPED. A real change cannot be made on demand without privileges, so this is
// a Mac program, not a Windows one: it includes iphlp.c and stands in for the runtime functions it calls, delivers
// changes itself (ip_changed) and makes writing a completion slow. On Windows the completion of a request is written
// before CancelIPChangeNotify can answer "not found": from then on the program may free the OVERLAPPED and close its
// event. Every value here is made up; no routing socket is opened.
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>
#include "../src/iphlp.c"                                       // (its static functions and state)

static int failures;
#define CHECK(c) do { if (!(c)) { printf("check failed (line %d): %s\n", __LINE__, #c); failures++; } } while (0)

// ---- what iphlp.c calls in the runtime
static DWORD g_last_error;
static int g_completing;                                            // completions begun (vx_io_completed entered)
static int g_slow;                                                  // each completion takes 200 ms to be written
void vx_io_completed(HANDLE file, void *ov, DWORD bytes, DWORD status) {
    (void)file;
    __atomic_add_fetch(&g_completing, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&g_slow, __ATOMIC_SEQ_CST)) usleep(200 * 1000);
    uint64_t *o = ov;
    o[1] = bytes;
    __atomic_store_n(&o[0], (uint64_t)status, __ATOMIC_RELEASE);
}
void vx_set_last_error(DWORD e) { g_last_error = e; }
HANDLE vx_handle_new(int kind, void *data, void (*destroy)(void *)) { (void)kind; (void)data; (void)destroy; return (HANDLE)(uintptr_t)0x10ab0; }
void *vx_find_export(const char *dll, const char *name) { (void)dll; (void)name; return NULL; }   // (no ResetEvent: no events here)
void vx_register_module(const char *dll, const vx_export *exports, size_t count) { (void)dll; (void)exports; (void)count; }
void vx_log(const char *fmt, ...) { (void)fmt; }
size_t vx_utf8_to_utf16(const char *s, size_t n, WCHAR *out, size_t cap) { (void)s; (void)n; if (cap) out[0] = 0; return 0; }
size_t vx_wcslen(const WCHAR *w) { size_t n = 0; while (w[n]) n++; return n; }

static uint64_t internal_of(const uint64_t *ov) { return __atomic_load_n(&ov[0], __ATOMIC_ACQUIRE); }

static void *deliver_address_change(void *arg) { (void)arg; ip_changed(0); return NULL; }
static int g_sync_returned;
static void *wait_for_address_change(void *arg) {
    (void)arg;
    if (NotifyAddrChange(NULL, NULL) == NO_ERROR) __atomic_store_n(&g_sync_returned, 1, __ATOMIC_SEQ_CST);
    return NULL;
}
// A change delivered on another thread; returns once its completion has begun (and, being slow, not yet been written).
static pthread_t change_in_progress(void) {
    int before = __atomic_load_n(&g_completing, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_slow, 1, __ATOMIC_SEQ_CST);
    pthread_t t;
    pthread_create(&t, NULL, deliver_address_change, NULL);
    while (__atomic_load_n(&g_completing, __ATOMIC_SEQ_CST) == before) usleep(1000);
    return t;
}

int main(void) {
    g_ip_file = vx_handle_new(VX_HANDLE_IPNOTIFY, NULL, NULL);    // the monitor counts as running: changes come from here
    g_ip_monitor = 1;
    HANDLE file = NULL;
    uint64_t ov[4];                                                 // OVERLAPPED: Internal, InternalHigh, offsets, hEvent (none)

    // ---- a change completes the address request, not the route one; the route request is cancelled
    uint64_t route_ov[4] = { 0 };
    memset(ov, 0, sizeof ov);
    CHECK(NotifyAddrChange(&file, ov) == ERROR_IO_PENDING && file == g_ip_file && internal_of(ov) == STATUS_PENDING);
    CHECK(NotifyRouteChange(&file, route_ov) == ERROR_IO_PENDING && internal_of(route_ov) == STATUS_PENDING);
    ip_changed(0);
    CHECK(internal_of(ov) == NO_ERROR && internal_of(route_ov) == STATUS_PENDING);
    CHECK(!CancelIPChangeNotify(ov) && g_last_error == ERROR_NOT_FOUND);
    CHECK(CancelIPChangeNotify(route_ov) && internal_of(route_ov) == ERROR_OPERATION_ABORTED);

    // ---- cancelled while the change completes it: "not found" only once the completion is written, so the caller
    // may free the OVERLAPPED then (written later, it would land in freed memory and signal whatever event is there)
    memset(ov, 0, sizeof ov);
    CHECK(NotifyAddrChange(&file, ov) == ERROR_IO_PENDING);
    pthread_t t = change_in_progress();
    BOOL cancelled = CancelIPChangeNotify(ov);
    uint64_t at_return = internal_of(ov);
    CHECK(!cancelled && g_last_error == ERROR_NOT_FOUND);
    CHECK(at_return == NO_ERROR);                                   // (STATUS_PENDING: the completion was still to come)
    pthread_join(t, NULL);
    CHECK(internal_of(ov) == NO_ERROR);

    // ---- a new request on the same OVERLAPPED while the change completes the previous one: the new one stays pending
    // (the change that ended the previous request is not the new request's), and it is listed, so it can be cancelled
    __atomic_store_n(&g_slow, 0, __ATOMIC_SEQ_CST);
    memset(ov, 0, sizeof ov);
    CHECK(NotifyAddrChange(&file, ov) == ERROR_IO_PENDING);
    t = change_in_progress();
    CHECK(NotifyAddrChange(&file, ov) == ERROR_IO_PENDING);
    pthread_join(t, NULL);
    CHECK(internal_of(ov) == STATUS_PENDING);
    __atomic_store_n(&g_slow, 0, __ATOMIC_SEQ_CST);
    CHECK(CancelIPChangeNotify(ov) && internal_of(ov) == ERROR_OPERATION_ABORTED);
    CHECK(!CancelIPChangeNotify(ov));

    // ---- the synchronous form (no OVERLAPPED) returns at the next change of its kind, not at a route change
    pthread_t waiter;
    pthread_create(&waiter, NULL, wait_for_address_change, NULL);
    usleep(50 * 1000);
    ip_changed(1);
    usleep(50 * 1000);
    CHECK(!__atomic_load_n(&g_sync_returned, __ATOMIC_SEQ_CST));
    for (int i = 0; i < 200 && !__atomic_load_n(&g_sync_returned, __ATOMIC_SEQ_CST); i++) { ip_changed(0); usleep(10 * 1000); }
    CHECK(__atomic_load_n(&g_sync_returned, __ATOMIC_SEQ_CST));    // (repeated: the waiter may not have been waiting yet)
    pthread_join(waiter, NULL);

    if (failures) { printf("iphlp_notify: %d checks failed\n", failures); return 1; }
    puts("iphlp_notify: completion before \"not found\", a new request on the same OVERLAPPED kept pending: ok");
    return 0;
}
