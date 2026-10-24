// user32: window classes, windows, message queues, input state, system metrics.
//
// Every thread that creates a window or asks for messages gets a queue. Posted messages, messages sent from
// other threads, WM_QUIT, WM_PAINT for invalid windows and WM_TIMER are delivered from it in Windows' order:
// sent messages first (handled inside GetMessage/PeekMessage), then posted ones, then quit, paint, timers.
// Messages sent within a thread call the window procedure directly.
#include <ctype.h>
#include <stdarg.h>
#include <errno.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <unistd.h>
#include <ApplicationServices/ApplicationServices.h>
#include "ui.h"

#define CW_USEDEFAULT ((int)0x80000000)
#define HWND_BASE     0x30000
#define MAX_WINDOWS   16384
#define HWND_DESKTOP_VALUE ((HANDLE)(intptr_t)0x10010)

typedef struct { HANDLE hwnd; uint32_t message; WPARAM wparam; LPARAM lparam; DWORD time; POINT pt; DWORD priv; } MSG;
_Static_assert(sizeof(MSG) == 48, "MSG");

WINAPI BOOL vx_ShowWindow(HANDLE hwnd, int cmd);

static DWORD tick_ms(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (DWORD)(mach_absolute_time() * tb.numer / tb.denom / 1000000);
}

// ---- classes -------------------------------------------------------------------------------------------------------
struct vx_class {
    char name[256];
    uint16_t atom;
    int unicode;
    WNDPROC proc;
    uint32_t style;
    int cls_extra, wnd_extra;
    HANDLE instance, icon, cursor, background;
};
static vx_class *g_classes[1024];
static int g_class_count;
static pthread_mutex_t g_class_lock = PTHREAD_MUTEX_INITIALIZER;

// A class name may be an atom (MAKEINTATOM: a value below 0x10000).
static vx_class *find_class(const char *name) {
    pthread_mutex_lock(&g_class_lock);
    vx_class *c = NULL;
    for (int i = 0; i < g_class_count && !c; i++)
        if ((uintptr_t)name < 0x10000 ? g_classes[i]->atom == (uint16_t)(uintptr_t)name : !strcasecmp(g_classes[i]->name, name)) c = g_classes[i];
    pthread_mutex_unlock(&g_class_lock);
    return c;
}

// WNDCLASSEX (x64): cbSize, style, proc, cls/wnd extra, instance, icon, cursor, brush, menu name, class name, small icon.
typedef struct { uint32_t size, style; WNDPROC proc; int cls_extra, wnd_extra; HANDLE instance, icon, cursor, background; const void *menu, *name; HANDLE icon_small; } WNDCLASSEX;
typedef struct { uint32_t style; WNDPROC proc; int cls_extra, wnd_extra; HANDLE instance, icon, cursor, background; const void *menu, *name; } WNDCLASS;

static uint16_t register_class(const char *name, int unicode, uint32_t style, WNDPROC proc, int cls_extra, int wnd_extra,
                               HANDLE instance, HANDLE icon, HANDLE cursor, HANDLE background) {
    if (find_class(name)) { vx_set_last_error(1410 /* ERROR_CLASS_ALREADY_EXISTS */); return 0; }
    vx_class *c = calloc(1, sizeof *c);
    snprintf(c->name, sizeof c->name, "%s", name);
    c->unicode = unicode; c->style = style; c->proc = proc; c->cls_extra = cls_extra; c->wnd_extra = wnd_extra;
    c->instance = instance; c->icon = icon; c->cursor = cursor; c->background = background;
    pthread_mutex_lock(&g_class_lock);
    c->atom = (uint16_t)(0xC000 + g_class_count);
    if (g_class_count < 1024) g_classes[g_class_count++] = c;
    pthread_mutex_unlock(&g_class_lock);
    return c->atom;
}
static WINAPI uint16_t RegisterClassExA(const WNDCLASSEX *wc) {
    return register_class(wc->name, 0, wc->style, wc->proc, wc->cls_extra, wc->wnd_extra, wc->instance, wc->icon, wc->cursor, wc->background);
}
static WINAPI uint16_t RegisterClassExW(const WNDCLASSEX *wc) {
    char *n = vx_utf8_dup(wc->name);
    uint16_t a = register_class(n, 1, wc->style, wc->proc, wc->cls_extra, wc->wnd_extra, wc->instance, wc->icon, wc->cursor, wc->background);
    free(n);
    return a;
}
static WINAPI uint16_t RegisterClassA(const WNDCLASS *wc) {
    return register_class(wc->name, 0, wc->style, wc->proc, wc->cls_extra, wc->wnd_extra, wc->instance, wc->icon, wc->cursor, wc->background);
}
static WINAPI uint16_t RegisterClassW(const WNDCLASS *wc) {
    char *n = vx_utf8_dup(wc->name);
    uint16_t a = register_class(n, 1, wc->style, wc->proc, wc->cls_extra, wc->wnd_extra, wc->instance, wc->icon, wc->cursor, wc->background);
    free(n);
    return a;
}
static WINAPI BOOL UnregisterClassA(const char *name, HANDLE inst) { (void)name; (void)inst; return 1; }
static WINAPI BOOL UnregisterClassW(const WCHAR *name, HANDLE inst) { (void)name; (void)inst; return 1; }
static WINAPI BOOL GetClassInfoExA(HANDLE inst, const char *name, WNDCLASSEX *wc) {
    (void)inst;
    vx_class *c = find_class(name);
    if (!c) { vx_set_last_error(1411 /* ERROR_CLASS_DOES_NOT_EXIST */); return 0; }
    wc->style = c->style; wc->proc = c->proc; wc->cls_extra = c->cls_extra; wc->wnd_extra = c->wnd_extra;
    wc->instance = c->instance; wc->icon = c->icon; wc->cursor = c->cursor; wc->background = c->background;
    wc->menu = NULL; wc->name = name; wc->icon_small = c->icon;
    return c->atom;
}
static WINAPI BOOL GetClassInfoExW(HANDLE inst, const WCHAR *name, WNDCLASSEX *wc) {
    char *n = (uintptr_t)name < 0x10000 ? (char *)name : vx_utf8_dup(name);
    BOOL r = GetClassInfoExA(inst, n, wc);
    if (n != (char *)name) free(n);
    wc->name = name;
    return r;
}

// ---- queues ----------------------------------------------------------------------------------------------------------
typedef struct sent_msg {
    HANDLE hwnd; uint32_t msg; WPARAM wp; LPARAM lp;
    LRESULT result; int done;
    struct sent_msg *next;
} sent_msg;
typedef struct { HANDLE hwnd; uint64_t id; DWORD ms, due; void *proc; } vx_timer;
struct vx_queue {
    DWORD tid;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    MSG *msgs; int head, count, cap;
    sent_msg *sent;
    int quit; WPARAM quit_code;
    vx_timer timers[64]; int ntimers;
    int waiting;                    // its thread waits in GetMessage / WaitMessage / MsgWait... (vx_ui_idle)
    vx_queue *next;
};
static vx_queue *g_queues;
static pthread_mutex_t g_queues_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_sent_done = PTHREAD_COND_INITIALIZER;     // with g_queues_lock: sent messages finishing
static __thread vx_queue *t_queue;
static __thread DWORD t_last_time;

static vx_queue *my_queue(void) {
    if (t_queue) return t_queue;
    vx_queue *q = calloc(1, sizeof *q);
    q->tid = vx_current_tid();
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->cond, NULL);
    pthread_mutex_lock(&g_queues_lock);
    q->next = g_queues; g_queues = q;
    pthread_mutex_unlock(&g_queues_lock);
    return t_queue = q;
}
static vx_queue *queue_of_thread(DWORD tid) {
    pthread_mutex_lock(&g_queues_lock);
    vx_queue *q = g_queues;
    while (q && q->tid != tid) q = q->next;
    pthread_mutex_unlock(&g_queues_lock);
    return q;
}

static void cursor_pos(POINT *p) {
    CGEventRef e = CGEventCreate(NULL);
    CGPoint c = e ? CGEventGetLocation(e) : CGPointZero;
    if (e) CFRelease(e);
    p->x = (LONG)c.x; p->y = (LONG)c.y;
}

static void queue_post(vx_queue *q, HANDLE hwnd, uint32_t msg, WPARAM wp, LPARAM lp) {
    MSG m = { hwnd, msg, wp, lp, tick_ms(), { 0, 0 }, 0 };
    cursor_pos(&m.pt);
    pthread_mutex_lock(&q->lock);
    if (q->count == q->cap) {
        int cap = q->cap ? q->cap * 2 : 64;
        MSG *n = malloc(cap * sizeof *n);
        for (int i = 0; i < q->count; i++) n[i] = q->msgs[(q->head + i) % q->cap];
        free(q->msgs); q->msgs = n; q->cap = cap; q->head = 0;
    }
    q->msgs[(q->head + q->count++) % q->cap] = m;
    pthread_cond_broadcast(&q->cond);
    pthread_mutex_unlock(&q->lock);
}

// ---- windows -----------------------------------------------------------------------------------------------------------
static vx_window *g_windows[MAX_WINDOWS];
static int g_window_next;
static pthread_mutex_t g_win_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
static HANDLE g_focus, g_active, g_capture;

vx_window *vx_window_get(HANDLE hwnd) {
    intptr_t i = ((intptr_t)hwnd - HWND_BASE) / 2;
    if ((intptr_t)hwnd < HWND_BASE || ((intptr_t)hwnd - HWND_BASE) % 2 || i >= MAX_WINDOWS) return NULL;
    vx_window *w = g_windows[i];
    return w && !w->destroyed ? w : NULL;
}

void vx_window_post(HANDLE hwnd, uint32_t msg, WPARAM wp, LPARAM lp) {
    vx_window *w = vx_window_get(hwnd);
    if (w) queue_post(w->queue, hwnd, msg, wp, lp);
}

// To every top-level window, as Windows broadcasts WM_DEVICECHANGE.
void vx_window_post_all(uint32_t msg, WPARAM wp, LPARAM lp) {
    pthread_mutex_lock(&g_win_lock);
    for (int i = 0; i < g_window_next; i++) {
        vx_window *w = g_windows[i];
        if (w && !w->destroyed && !(w->style & WS_CHILD)) queue_post(w->queue, w->hwnd, msg, wp, lp);
    }
    pthread_mutex_unlock(&g_win_lock);
}

static LRESULT call_proc(vx_window *w, uint32_t msg, WPARAM wp, LPARAM lp) { return w->proc(w->hwnd, msg, wp, lp); }

// Handles the messages other threads sent to this thread's windows. Called with q->lock held; drops it while a
// window procedure runs.
static void process_sent(vx_queue *q) {
    while (q->sent) {
        sent_msg *s = q->sent;
        q->sent = s->next;
        pthread_mutex_unlock(&q->lock);
        vx_window *w = vx_window_get(s->hwnd);
        LRESULT r = w ? call_proc(w, s->msg, s->wp, s->lp) : 0;
        pthread_mutex_lock(&g_queues_lock);
        s->result = r; s->done = 1;
        pthread_cond_broadcast(&g_sent_done);
        pthread_mutex_unlock(&g_queues_lock);
        pthread_mutex_lock(&q->lock);
    }
}

LRESULT vx_send_message(HANDLE hwnd, uint32_t msg, WPARAM wp, LPARAM lp) {
    vx_window *w = vx_window_get(hwnd);
    if (!w) { vx_set_last_error(1400 /* ERROR_INVALID_WINDOW_HANDLE */); return 0; }
    if (w->tid == vx_current_tid()) return call_proc(w, msg, wp, lp);
    // Another thread's window: queue it there and wait, still answering messages sent to this thread.
    sent_msg s = { hwnd, msg, wp, lp, 0, 0, NULL };
    vx_queue *q = w->queue;
    pthread_mutex_lock(&q->lock);
    sent_msg **tail = &q->sent;
    while (*tail) tail = &(*tail)->next;
    *tail = &s;
    pthread_cond_broadcast(&q->cond);
    pthread_mutex_unlock(&q->lock);
    for (;;) {
        pthread_mutex_lock(&g_queues_lock);
        if (!s.done) {
            struct timespec ts; struct timeval tv; gettimeofday(&tv, NULL);
            ts.tv_sec = tv.tv_sec; ts.tv_nsec = (long)tv.tv_usec * 1000 + 5000000;
            if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&g_sent_done, &g_queues_lock, &ts);
        }
        int done = s.done;
        pthread_mutex_unlock(&g_queues_lock);
        if (done) return s.result;
        if (t_queue) { pthread_mutex_lock(&t_queue->lock); process_sent(t_queue); pthread_mutex_unlock(&t_queue->lock); }
        if (!vx_window_get(hwnd)) {               // destroyed meanwhile: take the message back if still queued
            pthread_mutex_lock(&q->lock);
            for (sent_msg **p = &q->sent; *p; p = &(*p)->next) if (*p == &s) { *p = s.next; break; }
            pthread_mutex_unlock(&q->lock);
            pthread_mutex_lock(&g_queues_lock); done = s.done; pthread_mutex_unlock(&g_queues_lock);
            if (!done) return 0;
        }
    }
}

static WINAPI LRESULT SendMessageA(HANDLE h, uint32_t m, WPARAM w, LPARAM l) { return vx_send_message(h, m, w, l); }
static WINAPI LRESULT SendMessageW(HANDLE h, uint32_t m, WPARAM w, LPARAM l) { return vx_send_message(h, m, w, l); }
static WINAPI LRESULT SendMessageTimeoutA(HANDLE h, uint32_t m, WPARAM w, LPARAM l, uint32_t flags, uint32_t ms, uint64_t *result) {
    (void)flags; (void)ms;
    LRESULT r = vx_send_message(h, m, w, l);
    if (result) *result = (uint64_t)r;
    return vx_window_get(h) != NULL;
}
static WINAPI LRESULT SendMessageTimeoutW(HANDLE h, uint32_t m, WPARAM w, LPARAM l, uint32_t flags, uint32_t ms, uint64_t *result) {
    return SendMessageTimeoutA(h, m, w, l, flags, ms, result);
}
static WINAPI BOOL PostMessageA(HANDLE h, uint32_t m, WPARAM w, LPARAM l) {
    if (!h) { queue_post(my_queue(), NULL, m, w, l); return 1; }
    vx_window *win = vx_window_get(h);
    if (!win) { vx_set_last_error(1400); return 0; }
    queue_post(win->queue, h, m, w, l);
    return 1;
}
static WINAPI BOOL PostMessageW(HANDLE h, uint32_t m, WPARAM w, LPARAM l) { return PostMessageA(h, m, w, l); }
static WINAPI BOOL SendNotifyMessageA(HANDLE h, uint32_t m, WPARAM w, LPARAM l) {
    vx_window *win = vx_window_get(h);
    if (win && win->tid == vx_current_tid()) { call_proc(win, m, w, l); return 1; }
    return PostMessageA(h, m, w, l);
}
static WINAPI BOOL SendNotifyMessageW(HANDLE h, uint32_t m, WPARAM w, LPARAM l) { return SendNotifyMessageA(h, m, w, l); }
static WINAPI BOOL PostThreadMessageA(DWORD tid, uint32_t m, WPARAM w, LPARAM l) {
    vx_queue *q = tid == vx_current_tid() ? my_queue() : queue_of_thread(tid);
    if (!q) { vx_set_last_error(1444 /* ERROR_INVALID_THREAD_ID */); return 0; }
    queue_post(q, NULL, m, w, l);
    return 1;
}
static WINAPI BOOL PostThreadMessageW(DWORD tid, uint32_t m, WPARAM w, LPARAM l) { return PostThreadMessageA(tid, m, w, l); }
static WINAPI void PostQuitMessage(int code) {
    vx_queue *q = my_queue();
    pthread_mutex_lock(&q->lock);
    q->quit = 1; q->quit_code = (WPARAM)(int64_t)code;
    pthread_cond_broadcast(&q->cond);
    pthread_mutex_unlock(&q->lock);
}

// ---- getting messages ---------------------------------------------------------------------------------------------------
static int matches(const MSG *m, HANDLE hwnd, uint32_t min, uint32_t max) {
    if (hwnd && hwnd != (HANDLE)(intptr_t)-1 && m->hwnd != hwnd) return 0;
    if (hwnd == (HANDLE)(intptr_t)-1 && m->hwnd) return 0;                // -1: thread messages only
    return (!min && !max) || (m->message >= min && m->message <= max);
}
static int in_range(uint32_t msg, uint32_t min, uint32_t max) { return (!min && !max) || (msg >= min && msg <= max); }

// One step of GetMessage/PeekMessage. Returns 1 with *out filled, 0 if nothing is ready. q->lock held.
static int next_message(vx_queue *q, MSG *out, HANDLE hwnd, uint32_t min, uint32_t max, int remove) {
    process_sent(q);
    for (int i = 0; i < q->count; i++) {
        MSG *m = &q->msgs[(q->head + i) % q->cap];
        if (!matches(m, hwnd, min, max)) continue;
        *out = *m;
        if (remove) {
            for (int j = i; j > 0; j--) q->msgs[(q->head + j) % q->cap] = q->msgs[(q->head + j - 1) % q->cap];
            q->head = (q->head + 1) % q->cap;
            q->count--;
        }
        return 1;
    }
    if (q->quit && in_range(WM_QUIT, min, max)) {
        *out = (MSG){ NULL, WM_QUIT, q->quit_code, 0, tick_ms(), { 0, 0 }, 0 };
        if (remove) q->quit = 0;
        return 1;
    }
    if (in_range(WM_PAINT, min, max)) {
        pthread_mutex_lock(&g_win_lock);
        for (int i = 0; i < g_window_next; i++) {
            vx_window *w = g_windows[i];
            if (w && !w->destroyed && w->tid == q->tid && w->invalid && w->visible && (!hwnd || hwnd == w->hwnd)) {
                *out = (MSG){ w->hwnd, WM_PAINT, 0, 0, tick_ms(), { 0, 0 }, 0 };
                pthread_mutex_unlock(&g_win_lock);
                return 1;                                  // stays until the window is validated (BeginPaint)
            }
        }
        pthread_mutex_unlock(&g_win_lock);
    }
    if (in_range(WM_TIMER, min, max)) {
        DWORD now = tick_ms();
        for (int i = 0; i < q->ntimers; i++) {
            vx_timer *t = &q->timers[i];
            if ((int32_t)(now - t->due) < 0 || (hwnd && hwnd != t->hwnd)) continue;
            *out = (MSG){ t->hwnd, WM_TIMER, t->id, (LPARAM)t->proc, now, { 0, 0 }, 0 };
            if (remove) t->due = now + t->ms;
            return 1;
        }
    }
    return 0;
}

// Milliseconds until the next timer is due (or -1 for none).
static int next_timer_wait(vx_queue *q) {
    int best = -1;
    DWORD now = tick_ms();
    for (int i = 0; i < q->ntimers; i++) {
        int d = (int)(int32_t)(q->timers[i].due - now);
        if (d < 0) d = 0;
        if (best < 0 || d < best) best = d;
    }
    return best;
}
static void wait_queue(vx_queue *q, int max_ms) {         // q->lock held
    int ms = next_timer_wait(q);
    if (max_ms >= 0 && (ms < 0 || ms > max_ms)) ms = max_ms;
    if (ms < 0) { pthread_cond_wait(&q->cond, &q->lock); return; }
    struct timeval tv; gettimeofday(&tv, NULL);
    struct timespec ts = { tv.tv_sec + ms / 1000, (long)tv.tv_usec * 1000 + (long)(ms % 1000) * 1000000 };
    if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
    pthread_cond_timedwait(&q->cond, &q->lock, &ts);
}

static WINAPI BOOL GetMessageA(MSG *out, HANDLE hwnd, uint32_t min, uint32_t max) {
    vx_queue *q = my_queue();
    pthread_mutex_lock(&q->lock);
    while (!next_message(q, out, hwnd, min, max, 1)) { q->waiting = 1; wait_queue(q, -1); q->waiting = 0; }
    pthread_mutex_unlock(&q->lock);
    t_last_time = out->time;
    return out->message != WM_QUIT;
}
static WINAPI BOOL GetMessageW(MSG *out, HANDLE hwnd, uint32_t min, uint32_t max) { return GetMessageA(out, hwnd, min, max); }
#define PM_REMOVE 1
static WINAPI BOOL PeekMessageA(MSG *out, HANDLE hwnd, uint32_t min, uint32_t max, uint32_t flags) {
    vx_queue *q = my_queue();
    pthread_mutex_lock(&q->lock);
    int r = next_message(q, out, hwnd, min, max, flags & PM_REMOVE);
    pthread_mutex_unlock(&q->lock);
    if (r) t_last_time = out->time;
    return r;
}
static WINAPI BOOL PeekMessageW(MSG *out, HANDLE hwnd, uint32_t min, uint32_t max, uint32_t flags) { return PeekMessageA(out, hwnd, min, max, flags); }
static WINAPI BOOL WaitMessage(void) {
    vx_queue *q = my_queue();
    MSG m;
    pthread_mutex_lock(&q->lock);
    while (!next_message(q, &m, NULL, 0, 0, 0)) { q->waiting = 1; wait_queue(q, -1); q->waiting = 0; }
    pthread_mutex_unlock(&q->lock);
    return 1;
}
static WINAPI LONG GetMessageTime(void) { return (LONG)t_last_time; }
static WINAPI DWORD GetQueueStatus(uint32_t flags) {
    vx_queue *q = my_queue();
    MSG m;
    pthread_mutex_lock(&q->lock);
    int any = next_message(q, &m, NULL, 0, 0, 0);
    pthread_mutex_unlock(&q->lock);
    return any ? (flags & 0xffff) << 16 | (flags & 0xffff) : 0;
}

typedef DWORD (WINAPI *wait_multiple_fn)(DWORD, const HANDLE *, BOOL, DWORD);
// Waits for kernel objects or input in the queue (QS_* masks are not distinguished yet).
static WINAPI DWORD MsgWaitForMultipleObjectsEx(DWORD n, const HANDLE *hs, DWORD ms, DWORD mask, DWORD flags) {
    (void)mask;
    static wait_multiple_fn wait;
    if (!wait) wait = (wait_multiple_fn)vx_find_export("kernel32.dll", "WaitForMultipleObjects");
    vx_queue *q = my_queue();
    DWORD start = tick_ms();
    for (;;) {
        MSG m;
        pthread_mutex_lock(&q->lock);
        int ready = next_message(q, &m, NULL, 0, 0, 0);
        q->waiting = !ready;
        pthread_mutex_unlock(&q->lock);
        if (ready) return n;                                         // WAIT_OBJECT_0 + n: input is available
        DWORD r = n ? wait(n, hs, flags & 1 /* MWMO_WAITALL */, 0) : 0x102;
        if (r != 0x102) { q->waiting = 0; return r; }
        DWORD elapsed = tick_ms() - start;
        if (ms != 0xffffffffu && elapsed >= ms) { q->waiting = 0; return 0x102; }   // WAIT_TIMEOUT
        pthread_mutex_lock(&q->lock);
        wait_queue(q, 2);
        pthread_mutex_unlock(&q->lock);
    }
}
static WINAPI DWORD MsgWaitForMultipleObjects(DWORD n, const HANDLE *hs, BOOL all, DWORD ms, DWORD mask) {
    return MsgWaitForMultipleObjectsEx(n, hs, ms, mask, all ? 1 : 0);
}

static WINAPI BOOL TranslateMessage(const MSG *m) { return m->message == WM_KEYDOWN || m->message == WM_SYSKEYDOWN; }   // characters arrive as WM_CHAR already
typedef void (WINAPI *TIMERPROC)(HANDLE, uint32_t, uint64_t, DWORD);
static WINAPI LRESULT DispatchMessageA(const MSG *m) {
    if (m->message == WM_TIMER && m->lparam) { ((TIMERPROC)m->lparam)(m->hwnd, WM_TIMER, m->wparam, tick_ms()); return 0; }
    vx_window *w = vx_window_get(m->hwnd);
    return w ? call_proc(w, m->message, m->wparam, m->lparam) : 0;
}
static WINAPI LRESULT DispatchMessageW(const MSG *m) { return DispatchMessageA(m); }

// ---- timers ----------------------------------------------------------------------------------------------------------------
static WINAPI uint64_t SetTimer(HANDLE hwnd, uint64_t id, uint32_t ms, void *proc) {
    vx_window *w = hwnd ? vx_window_get(hwnd) : NULL;
    if (hwnd && !w) { vx_set_last_error(1400); return 0; }
    vx_queue *q = w ? w->queue : my_queue();
    static uint64_t next_id = 0x7000;
    if (!hwnd) id = __sync_fetch_and_add(&next_id, 1);
    if (ms < 10) ms = 10;                                   // USER_TIMER_MINIMUM
    pthread_mutex_lock(&q->lock);
    int i = 0;
    while (i < q->ntimers && !(q->timers[i].hwnd == hwnd && q->timers[i].id == id)) i++;
    if (i == q->ntimers && q->ntimers < 64) q->ntimers++;
    if (i < 64) q->timers[i] = (vx_timer){ hwnd, id, ms, tick_ms() + ms, proc };
    pthread_cond_broadcast(&q->cond);
    pthread_mutex_unlock(&q->lock);
    return i < 64 ? (id ? id : 1) : 0;
}
static BOOL kill_timer(vx_queue *q, HANDLE hwnd, uint64_t id, int all) {
    BOOL found = 0;
    pthread_mutex_lock(&q->lock);
    for (int i = 0; i < q->ntimers;)
        if (q->timers[i].hwnd == hwnd && (all || q->timers[i].id == id)) { q->timers[i] = q->timers[--q->ntimers]; found = 1; }
        else i++;
    pthread_mutex_unlock(&q->lock);
    return found;
}
static WINAPI BOOL KillTimer(HANDLE hwnd, uint64_t id) {
    vx_window *w = hwnd ? vx_window_get(hwnd) : NULL;
    return kill_timer(w ? w->queue : my_queue(), hwnd, id, 0);
}

// ---- creating and destroying windows ------------------------------------------------------------------------------------------
// CREATESTRUCT (x64): create params, instance, menu, parent, cy, cx, y, x, style, name, class, ex style.
typedef struct { void *params; HANDLE instance, menu, parent; int cy, cx, y, x; LONG style; const void *name, *cls; DWORD exstyle; } CREATESTRUCT;

static void set_title(vx_window *w, const char *t) { free(w->title); w->title = strdup(t ? t : ""); }

static HANDLE create_window(DWORD exstyle, const char *class_name, const char *title, DWORD style, int x, int y, int cx, int cy,
                            HANDLE parent, HANDLE menu, HANDLE instance, void *params, const void *raw_title, const void *raw_class) {
    vx_class *c = find_class(class_name);
    if (vx_trace) vx_log("CreateWindow \"%s\" class %s style %#x %dx%d", title ? title : "", (uintptr_t)class_name < 0x10000 ? "(atom)" : class_name, (unsigned)style, cx, cy);
    if (!c) { vx_set_last_error(1407 /* ERROR_CANNOT_FIND_WND_CLASS */); return NULL; }
    vx_window *w = calloc(1, sizeof *w);
    pthread_mutex_init(&w->lock, NULL);
    w->cls = c; w->proc = c->proc; w->unicode = c->unicode;
    w->style = style; w->exstyle = exstyle; w->parent = parent; w->instance = instance;
    w->id = (int64_t)(intptr_t)menu; w->enabled = !(style & WS_DISABLED);
    w->tid = vx_current_tid(); w->queue = my_queue();
    w->extra_size = c->wnd_extra; w->extra = calloc(1, c->wnd_extra > 0 ? c->wnd_extra : 1);
    set_title(w, title);
    RECT screen; vx_virtual_screen(&screen);
    if (cx == CW_USEDEFAULT) { cx = (screen.right - screen.left) * 3 / 4; cy = (screen.bottom - screen.top) * 3 / 4; }
    if (x == CW_USEDEFAULT) { x = y = 0; w->center = !(style & WS_CHILD); }
    w->rect = (RECT){ x, y, x + cx, y + cy };
    pthread_mutex_lock(&g_win_lock);
    if (g_window_next == MAX_WINDOWS) { pthread_mutex_unlock(&g_win_lock); free(w); vx_set_last_error(1158); return NULL; }
    w->hwnd = (HANDLE)(intptr_t)(HWND_BASE + g_window_next * 2);
    g_windows[g_window_next++] = w;
    pthread_mutex_unlock(&g_win_lock);

    if (style & WS_CHILD) {
        w->client_w = cx; w->client_h = cy;
        vx_window_resized(w, cx, cy);
    } else {
        vx_cocoa_create(w);
    }
    CREATESTRUCT cs = { params, instance, menu, parent, cy, cx, y, x, (LONG)style, raw_title, raw_class, exstyle };
    if (!call_proc(w, WM_NCCREATE, 0, (LPARAM)&cs)) { vx_cocoa_destroy(w); w->destroyed = 1; return NULL; }
    if (call_proc(w, WM_CREATE, 0, (LPARAM)&cs) == -1) {
        call_proc(w, WM_NCDESTROY, 0, 0);
        vx_cocoa_destroy(w); w->destroyed = 1;
        return NULL;
    }
    HANDLE hwnd = w->hwnd;
    if (!vx_window_get(hwnd)) return NULL;                       // destroyed by its own WM_CREATE
    call_proc(w, WM_SIZE, 0, MAKELPARAM(w->client_w, w->client_h));
    call_proc(w, WM_MOVE, 0, MAKELPARAM(w->rect.left, w->rect.top + (w->rect.bottom - w->rect.top - w->client_h)));
    if (style & WS_VISIBLE) {
        w->style &= ~WS_VISIBLE;
        vx_ShowWindow(hwnd, (style & WS_MAXIMIZE) ? 3 : (style & WS_MINIMIZE) ? 6 : 5);
    }
    return hwnd;
}
static WINAPI HANDLE CreateWindowExA(DWORD ex, const char *cls, const char *title, DWORD style, int x, int y, int cx, int cy,
                                     HANDLE parent, HANDLE menu, HANDLE inst, void *params) {
    return create_window(ex, cls, title, style, x, y, cx, cy, parent, menu, inst, params, title, cls);
}
static WINAPI HANDLE CreateWindowExW(DWORD ex, const WCHAR *cls, const WCHAR *title, DWORD style, int x, int y, int cx, int cy,
                                     HANDLE parent, HANDLE menu, HANDLE inst, void *params) {
    char *c = (uintptr_t)cls < 0x10000 ? (char *)cls : vx_utf8_dup(cls), *t = vx_utf8_dup(title);
    HANDLE h = create_window(ex, c, t, style, x, y, cx, cy, parent, menu, inst, params, title, cls);
    if (c != (char *)cls) free(c);
    free(t);
    return h;
}

static WINAPI BOOL DestroyWindow(HANDLE hwnd) {
    vx_window *w = vx_window_get(hwnd);
    if (!w) { vx_set_last_error(1400); return 0; }
    if (w->tid != vx_current_tid()) { vx_set_last_error(5); return 0; }   // only the owner thread may destroy it
    w->visible = 0;
    vx_cocoa_show(w, 0);
    call_proc(w, WM_DESTROY, 0, 0);
    // Children go with their parent.
    pthread_mutex_lock(&g_win_lock);
    for (int i = 0; i < g_window_next; i++)
        if (g_windows[i] && !g_windows[i]->destroyed && g_windows[i]->parent == hwnd) { pthread_mutex_unlock(&g_win_lock); DestroyWindow(g_windows[i]->hwnd); pthread_mutex_lock(&g_win_lock); }
    pthread_mutex_unlock(&g_win_lock);
    call_proc(w, WM_NCDESTROY, 0, 0);
    vx_tray_window_destroyed(hwnd);                   // (its notification-area icons go with it: tray.m)
    kill_timer(w->queue, hwnd, 0, 1);
    vx_cocoa_destroy(w);
    w->destroyed = 1;                                 // the slot is never reused, so stale handles stay invalid
    vx_texts_free(&w->texts);
    if (g_focus == hwnd) g_focus = NULL;
    if (g_active == hwnd) g_active = NULL;
    if (g_capture == hwnd) g_capture = NULL;
    return 1;
}

WINAPI BOOL vx_ShowWindow(HANDLE hwnd, int cmd) {
    vx_window *w = vx_window_get(hwnd);
    if (!w) return 0;
    BOOL was = w->visible;
    // Hiding a hidden window changes nothing, and Windows returns at once. Steam hides its hidden windows on every
    // frame of its main loop: going to AppKit's main thread for each made it wait there for window ordering.
    if (cmd == 0 && !was) return 0;
    int show = cmd != 0;
    if (vx_trace && show != was) vx_log("ShowWindow %p \"%s\" %d: %s", hwnd, w->title ? w->title : "", cmd, show ? "shown" : "hidden");
    if (show != was) call_proc(w, WM_SHOWWINDOW, show, 0);
    w->visible = show;
    if (show) { w->invalid = 1; w->erase = 1; }
    if (cmd == 0) w->style &= ~WS_VISIBLE; else w->style |= WS_VISIBLE;
    vx_cocoa_show(w, cmd);
    return was;
}
static WINAPI BOOL ShowWindowAsync(HANDLE hwnd, int cmd) { return vx_ShowWindow(hwnd, cmd); }

// ---- calls from cocoa (main thread) ------------------------------------------------------------------------------------------
// Resizes the client-area bitmap (keeping what fits) and tells the window.
void vx_window_resized(vx_window *w, int cw, int ch) {
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;
    pthread_mutex_lock(&w->lock);
    int changed = cw != w->bw || ch != w->bh;
    if (changed) {
        uint32_t *px = calloc((size_t)cw * ch, 4);
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGContextRef ctx = CGBitmapContextCreate(px, cw, ch, 8, (size_t)cw * 4, cs, kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
        CGColorSpaceRelease(cs);
        if (w->pixels) {
            for (int y = 0; y < ch && y < w->bh; y++) memcpy(px + (size_t)y * cw, w->pixels + (size_t)y * w->bw, (size_t)(cw < w->bw ? cw : w->bw) * 4);
            CGContextRelease(w->ctx);
            free(w->pixels);
        }
        // GDI draws top-down: flip the context once so y grows downwards.
        CGContextTranslateCTM(ctx, 0, ch);
        CGContextScaleCTM(ctx, 1, -1);
        w->pixels = px; w->ctx = ctx; w->bw = cw; w->bh = ch;
    }
    int had = w->client_w || w->client_h;
    w->client_w = cw; w->client_h = ch;
    pthread_mutex_unlock(&w->lock);
    if (changed && had) {
        w->invalid = 1; w->erase = 1;
        vx_window_post(w->hwnd, WM_SIZE, w->minimized ? 1 : 0, MAKELPARAM(cw, ch));
        if (w->queue) { pthread_mutex_lock(&w->queue->lock); pthread_cond_broadcast(&w->queue->cond); pthread_mutex_unlock(&w->queue->lock); }
    }
}
void vx_window_moved(vx_window *w, RECT frame) {
    pthread_mutex_lock(&w->lock);
    int moved = frame.left != w->rect.left || frame.top != w->rect.top;
    w->rect = frame;
    pthread_mutex_unlock(&w->lock);
    if (moved) vx_window_post(w->hwnd, WM_MOVE, 0, MAKELPARAM(frame.left, frame.bottom - w->client_h));
}
// VXRT_STAY_ACTIVE=1: a window that was active stays so when the Mac moves on to another app. A game played in a
// headset is seldom the Mac's frontmost app, and a game told it is in the background stops reading its controllers
// (Elite Dangerous acquired no DirectInput device, read no XInput pad and took no key).
static int stay_active(void) { static int on = -1; if (on < 0) { const char *e = getenv("VXRT_STAY_ACTIVE"); on = e && *e == '1'; } return on; }
static int trace_focus(void) { static int on = -1; if (on < 0) on = getenv("VXRT_TRACE_INPUT") != NULL; return on; }
void vx_window_activated(vx_window *w, int active) {
    if (trace_focus()) vx_log("focus: window %p (%s) %s%s", w->hwnd, w->title ? w->title : "?", active ? "activated" : "deactivated",
                              !active && stay_active() && g_active == w->hwnd ? " (kept active: VXRT_STAY_ACTIVE)" : "");
    if (!active && stay_active() && g_active == w->hwnd) return;
    if (active) { g_active = g_focus = w->hwnd; }
    else if (g_active == w->hwnd) { g_active = NULL; if (g_focus == w->hwnd) g_focus = NULL; }
    vx_window_post(w->hwnd, WM_ACTIVATEAPP, active, 0);
    vx_window_post(w->hwnd, WM_ACTIVATE, active ? 1 : 0, 0);
    vx_window_post(w->hwnd, active ? WM_SETFOCUS : WM_KILLFOCUS, 0, 0);
}
// A window shown without taking the Mac's focus (VXRT_NO_ACTIVATE) is still the game's active one with VXRT_STAY_ACTIVE.
void vx_window_shown_inactive(vx_window *w) { if (stay_active() && !g_active) vx_window_activated(w, 1); }
void vx_window_minimized(vx_window *w, int minimized) {
    w->minimized = minimized;
    vx_window_post(w->hwnd, WM_SIZE, minimized ? 1 /* SIZE_MINIMIZED */ : 0, MAKELPARAM(w->client_w, w->client_h));
    if (!minimized) { w->invalid = 1; vx_window_post(w->hwnd, 0 /* WM_NULL: wake the queue */, 0, 0); }
}
void vx_request_redraw(vx_window *w) { vx_cocoa_redraw(w); }

// ---- keyboard ------------------------------------------------------------------------------------------------------------------
// Mac key codes to Windows virtual keys and set-1 scan codes (0x100 marks an extended key).
static const struct { uint8_t vk; uint16_t scan; } g_keymap[128] = {
    [0x00] = { 'A', 0x1E }, [0x01] = { 'S', 0x1F }, [0x02] = { 'D', 0x20 }, [0x03] = { 'F', 0x21 }, [0x04] = { 'H', 0x23 },
    [0x05] = { 'G', 0x22 }, [0x06] = { 'Z', 0x2C }, [0x07] = { 'X', 0x2D }, [0x08] = { 'C', 0x2E }, [0x09] = { 'V', 0x2F },
    [0x0B] = { 'B', 0x30 }, [0x0C] = { 'Q', 0x10 }, [0x0D] = { 'W', 0x11 }, [0x0E] = { 'E', 0x12 }, [0x0F] = { 'R', 0x13 },
    [0x10] = { 'Y', 0x15 }, [0x11] = { 'T', 0x14 }, [0x12] = { '1', 0x02 }, [0x13] = { '2', 0x03 }, [0x14] = { '3', 0x04 },
    [0x15] = { '4', 0x05 }, [0x16] = { '6', 0x07 }, [0x17] = { '5', 0x06 }, [0x18] = { 0xBB, 0x0D }, [0x19] = { '9', 0x0A },
    [0x1A] = { '7', 0x08 }, [0x1B] = { 0xBD, 0x0C }, [0x1C] = { '8', 0x09 }, [0x1D] = { '0', 0x0B }, [0x1E] = { 0xDD, 0x1B },
    [0x1F] = { 'O', 0x18 }, [0x20] = { 'U', 0x16 }, [0x21] = { 0xDB, 0x1A }, [0x22] = { 'I', 0x17 }, [0x23] = { 'P', 0x19 },
    [0x24] = { 0x0D, 0x1C }, [0x25] = { 'L', 0x26 }, [0x26] = { 'J', 0x24 }, [0x27] = { 0xDE, 0x28 }, [0x28] = { 'K', 0x25 },
    [0x29] = { 0xBA, 0x27 }, [0x2A] = { 0xDC, 0x2B }, [0x2B] = { 0xBC, 0x33 }, [0x2C] = { 0xBF, 0x35 }, [0x2D] = { 'N', 0x31 },
    [0x2E] = { 'M', 0x32 }, [0x2F] = { 0xBE, 0x34 }, [0x30] = { 0x09, 0x0F }, [0x31] = { 0x20, 0x39 }, [0x32] = { 0xC0, 0x29 },
    [0x33] = { 0x08, 0x0E }, [0x35] = { 0x1B, 0x01 }, [0x36] = { 0x5C, 0x15C }, [0x37] = { 0x5B, 0x15B }, [0x38] = { 0x10, 0x2A },
    [0x39] = { 0x14, 0x3A }, [0x3A] = { 0x12, 0x38 }, [0x3B] = { 0x11, 0x1D }, [0x3C] = { 0x10, 0x36 }, [0x3D] = { 0x12, 0x138 },
    [0x3E] = { 0x11, 0x11D }, [0x41] = { 0x6E, 0x53 }, [0x43] = { 0x6A, 0x37 }, [0x45] = { 0x6B, 0x4E }, [0x47] = { 0x90, 0x45 },
    [0x4B] = { 0x6F, 0x135 }, [0x4C] = { 0x0D, 0x11C }, [0x4E] = { 0x6D, 0x4A }, [0x52] = { 0x60, 0x52 }, [0x53] = { 0x61, 0x4F },
    [0x54] = { 0x62, 0x50 }, [0x55] = { 0x63, 0x51 }, [0x56] = { 0x64, 0x4B }, [0x57] = { 0x65, 0x4C }, [0x58] = { 0x66, 0x4D },
    [0x59] = { 0x67, 0x47 }, [0x5B] = { 0x68, 0x48 }, [0x5C] = { 0x69, 0x49 }, [0x60] = { 0x74, 0x3F }, [0x61] = { 0x75, 0x40 },
    [0x62] = { 0x76, 0x41 }, [0x63] = { 0x72, 0x3D }, [0x64] = { 0x77, 0x42 }, [0x65] = { 0x78, 0x43 }, [0x67] = { 0x7A, 0x57 },
    [0x6D] = { 0x79, 0x44 }, [0x6F] = { 0x7B, 0x58 }, [0x72] = { 0x2D, 0x152 }, [0x73] = { 0x24, 0x147 }, [0x74] = { 0x21, 0x149 },
    [0x75] = { 0x2E, 0x153 }, [0x76] = { 0x73, 0x3E }, [0x77] = { 0x23, 0x14F }, [0x78] = { 0x71, 0x3C }, [0x79] = { 0x22, 0x151 },
    [0x7A] = { 0x70, 0x3B }, [0x7B] = { 0x25, 0x14B }, [0x7C] = { 0x27, 0x14D }, [0x7D] = { 0x28, 0x150 }, [0x7E] = { 0x26, 0x148 },
};
static uint8_t g_keys[256];         // 0x80 down, 0x01 toggled
static void raw_key(int scan, int vk, int down);
static void set_key(int vk, int down) {
    if (down && !(g_keys[vk] & 0x80)) g_keys[vk] ^= 0x01;
    g_keys[vk] = (uint8_t)((g_keys[vk] & 0x01) | (down ? 0x80 : 0));
}
void vx_mouse_button(int vk, int down) { set_key(vk, down); }

// DirectInput's keyboard and mouse (directinput.m) read these: the keys by DIK code (the set-1 scan code, 0x80 added
// for an extended key) and the mouse's movement since it last asked.
static volatile uint8_t g_dik[256];
static volatile LONG g_mouse_dx, g_mouse_dy, g_mouse_dz;
int vx_dinput_key_down(int dik) { return g_dik[dik & 0xff] != 0; }
void vx_dinput_mouse_take(LONG *dx, LONG *dy, LONG *dz) {
    *dx = __sync_lock_test_and_set(&g_mouse_dx, 0);
    *dy = __sync_lock_test_and_set(&g_mouse_dy, 0);
    *dz = __sync_lock_test_and_set(&g_mouse_dz, 0);
}
int vx_dinput_mouse_button(int i) {                   // left, right, middle, X1, X2
    static const uint8_t vks[5] = { 0x01, 0x02, 0x04, 0x05, 0x06 };
    return i >= 0 && i < 5 && (g_keys[vks[i]] & 0x80);
}
void vx_key_event(vx_window *w, int code, int down, int repeat, int alt) {
    if (code < 0 || code >= 128 || !g_keymap[code].vk) return;
    int vk = g_keymap[code].vk, scan = g_keymap[code].scan;
    g_dik[(scan & 0x7f) | (scan & 0x100 ? 0x80 : 0)] = down ? 1 : 0;
    set_key(vk, down);
    static const struct { int code, vk; } sides[] = { { 0x38, 0xA0 }, { 0x3C, 0xA1 }, { 0x3B, 0xA2 }, { 0x3E, 0xA3 }, { 0x3A, 0xA4 }, { 0x3D, 0xA5 } };
    for (size_t i = 0; i < sizeof sides / sizeof sides[0]; i++) if (sides[i].code == code) set_key(sides[i].vk, down);
    LPARAM lp = 1 | (LPARAM)(scan & 0xff) << 16 | (scan & 0x100 ? (LPARAM)1 << 24 : 0) | (alt ? (LPARAM)1 << 29 : 0);
    if (!down) lp |= (LPARAM)3 << 30;
    else if (repeat) lp |= (LPARAM)1 << 30;
    int sys = alt || vk == 0x12;
    if (trace_focus() && down) { static volatile int keys; if (__sync_add_and_fetch(&keys, 1) <= 30) vx_log("input: key %#x down to window %p (%s)", vk, w->hwnd, w->title ? w->title : "?"); }
    vx_window_post(w->hwnd, down ? (sys ? WM_SYSKEYDOWN : WM_KEYDOWN) : (sys ? WM_SYSKEYUP : WM_KEYUP), (WPARAM)vk, lp);
    raw_key(scan, vk, down);
}
void vx_char_event(vx_window *w, const uint16_t *chars, int n) {
    for (int i = 0; i < n; i++) vx_window_post(w->hwnd, WM_CHAR, chars[i], 1);
}

static WINAPI int16_t GetKeyState(int vk) { uint8_t k = g_keys[vk & 0xff]; return (int16_t)((k & 0x80 ? 0x8000 : 0) | (k & 0x01)); }
static WINAPI int16_t GetAsyncKeyState(int vk) { return (int16_t)(g_keys[vk & 0xff] & 0x80 ? 0x8000 : 0); }
static WINAPI BOOL GetKeyboardState(uint8_t *keys) { memcpy(keys, g_keys, 256); return 1; }
// MapVirtualKey: 0 VK->scan, 1 scan->VK, 2 VK->char, 3 scan->VK (left/right distinguished).
static WINAPI uint32_t MapVirtualKeyA(uint32_t code, uint32_t type) {
    for (int i = 0; i < 128; i++) {
        if (!g_keymap[i].vk) continue;
        if (type == 0 && g_keymap[i].vk == code) return g_keymap[i].scan & 0xff;
        if ((type == 1 || type == 3) && (g_keymap[i].scan & 0xff) == code && !(g_keymap[i].scan & 0x100)) return g_keymap[i].vk;
    }
    if (type == 2) return code >= 'A' && code <= 'Z' ? code : code >= '0' && code <= '9' ? code : code == 0x20 ? ' ' : 0;
    return 0;
}
static WINAPI uint32_t MapVirtualKeyW(uint32_t code, uint32_t type) { return MapVirtualKeyA(code, type); }
static WINAPI uint32_t MapVirtualKeyExA(uint32_t code, uint32_t type, HANDLE layout) { (void)layout; return MapVirtualKeyA(code, type); }
static WINAPI uint32_t MapVirtualKeyExW(uint32_t code, uint32_t type, HANDLE layout) { (void)layout; return MapVirtualKeyA(code, type); }
static WINAPI HANDLE GetKeyboardLayout(DWORD tid) { (void)tid; return (HANDLE)(intptr_t)0x04090409; }   // US English
static WINAPI int GetKeyboardType(int which) { return which == 0 ? 4 : which == 2 ? 12 : 0; }
static WINAPI int GetKeyNameTextA(LONG lp, char *buf, int n) {
    int scan = (lp >> 16) & 0xff, vk = (int)MapVirtualKeyA((uint32_t)scan, 1);
    char name[16];
    if (vk >= 'A' && vk <= 'Z') snprintf(name, sizeof name, "%c", vk);
    else if (vk >= '0' && vk <= '9') snprintf(name, sizeof name, "%c", vk);
    else snprintf(name, sizeof name, "Key %d", scan);
    return n > 0 ? snprintf(buf, (size_t)n, "%s", name) : 0;
}

// ---- mouse and cursor -------------------------------------------------------------------------------------------------------------
static WINAPI BOOL GetCursorPos(POINT *p) { cursor_pos(p); return 1; }
static WINAPI BOOL SetCursorPos(int x, int y) { CGWarpMouseCursorPosition(CGPointMake(x, y)); CGAssociateMouseAndMouseCursorPosition(1); return 1; }
static int g_cursor_count;
static RECT g_clip; static int g_clipped;
static void update_mouse_mode(void);
static WINAPI int ShowCursor(BOOL show) {
    int before = g_cursor_count;
    g_cursor_count += show ? 1 : -1;
    if (before >= 0 && g_cursor_count < 0) CGDisplayHideCursor(kCGDirectMainDisplay);
    if (before < 0 && g_cursor_count >= 0) CGDisplayShowCursor(kCGDirectMainDisplay);
    update_mouse_mode();
    return g_cursor_count;
}
static HANDLE g_cursor = (HANDLE)(intptr_t)0x10003;
static WINAPI HANDLE SetCursor(HANDLE c) { HANDLE o = g_cursor; g_cursor = c; return o; }
static WINAPI HANDLE GetCursor(void) { return g_cursor; }
// Cursors, icons and images from resources: handles that stand for them (drawing them comes later).
static WINAPI HANDLE LoadCursorA(HANDLE inst, const char *name) { (void)inst; return (HANDLE)(0x10000 | ((uintptr_t)name & 0xffff)); }
static WINAPI HANDLE LoadCursorW(HANDLE inst, const WCHAR *name) { return LoadCursorA(inst, (const char *)name); }
static WINAPI HANDLE LoadIconA(HANDLE inst, const char *name) { (void)inst; return (HANDLE)(0x20000 | ((uintptr_t)name & 0xffff)); }
static WINAPI HANDLE LoadIconW(HANDLE inst, const WCHAR *name) { return LoadIconA(inst, (const char *)name); }
static WINAPI HANDLE LoadImageA(HANDLE inst, const char *name, uint32_t type, int cx, int cy, uint32_t flags) {
    (void)inst; (void)cx; (void)cy; (void)flags; return (HANDLE)((0x30000 + ((uintptr_t)type << 16)) | ((uintptr_t)name & 0xffff));
}
static WINAPI HANDLE LoadImageW(HANDLE inst, const WCHAR *name, uint32_t type, int cx, int cy, uint32_t flags) { return LoadImageA(inst, (const char *)name, type, cx, cy, flags); }
static WINAPI BOOL DestroyCursor(HANDLE c) { (void)c; return 1; }
static WINAPI BOOL DestroyIcon(HANDLE c) { (void)c; return 1; }
static RECT g_clip; static int g_clipped;
// A hidden cursor confined to a rectangle is how games ask for mouse-look: the cursor then stays put while the
// mouse keeps sending movement (raw input and WM_MOUSEMOVE deltas through SetCursorPos re-centering both work).
static void update_mouse_mode(void) { CGAssociateMouseAndMouseCursorPosition(!(g_clipped && g_cursor_count < 0)); }
static WINAPI BOOL ClipCursor(const RECT *r) { g_clipped = r != NULL; if (r) g_clip = *r; update_mouse_mode(); return 1; }
static WINAPI BOOL GetClipCursor(RECT *r) { if (g_clipped) *r = g_clip; else vx_virtual_screen(r); return 1; }
static WINAPI HANDLE SetCapture(HANDLE h) { HANDLE o = g_capture; g_capture = h; return o; }
static WINAPI BOOL ReleaseCapture(void) { g_capture = NULL; return 1; }
static WINAPI HANDLE GetCapture(void) { return g_capture; }
static WINAPI BOOL TrackMouseEvent(void *t) { (void)t; return 1; }
static WINAPI uint32_t GetDoubleClickTime(void) { return 500; }


// ---- raw input ---------------------------------------------------------------------------------------------------------------------
// Programs that register for raw input get WM_INPUT for every mouse movement (unaccelerated deltas, buttons, wheel)
// and key, as RAWINPUT records: a header (type, size, device, wParam) and RAWMOUSE (flags, button flags and data,
// raw buttons, x, y, extra) or RAWKEYBOARD (make code, flags, reserved, virtual key, message, extra).
typedef struct { uint16_t page, usage; DWORD flags; HANDLE target; } RAWINPUTDEVICE;
static WINAPI BOOL ScreenToClient(HANDLE h, POINT *p);
#define RIDEV_REMOVE 0x1
#define RAW_MOUSE_DEVICE ((HANDLE)(intptr_t)0x50001)
#define RAW_KEYBOARD_DEVICE ((HANDLE)(intptr_t)0x50002)
#define RAW_RING 1024
static RAWINPUTDEVICE g_raw_mouse, g_raw_keyboard;
static int g_raw_has_mouse, g_raw_has_keyboard;
static struct { uint8_t data[48]; uint32_t size; } g_raw[RAW_RING];
static uint32_t g_raw_next;
static pthread_mutex_t g_raw_lock = PTHREAD_MUTEX_INITIALIZER;

static WINAPI BOOL RegisterRawInputDevices(const RAWINPUTDEVICE *d, uint32_t n, uint32_t size) {
    if (size != sizeof(RAWINPUTDEVICE)) { vx_set_last_error(87); return 0; }
    for (uint32_t i = 0; i < n; i++) {
        if (d[i].page != 1) continue;                               // generic desktop controls
        int remove = (d[i].flags & RIDEV_REMOVE) != 0;
        if (d[i].usage == 2) { g_raw_mouse = d[i]; g_raw_has_mouse = !remove; }
        if (d[i].usage == 6) { g_raw_keyboard = d[i]; g_raw_has_keyboard = !remove; }
    }
    return 1;
}
static WINAPI uint32_t GetRegisteredRawInputDevices(RAWINPUTDEVICE *d, uint32_t *n, uint32_t size) {
    uint32_t count = (uint32_t)(g_raw_has_mouse + g_raw_has_keyboard);
    if (!d || *n < count) { *n = count; vx_set_last_error(122); return d ? (uint32_t)-1 : 0; }
    uint32_t k = 0;
    if (g_raw_has_mouse) d[k++] = g_raw_mouse;
    if (g_raw_has_keyboard) d[k++] = g_raw_keyboard;
    (void)size;
    return k;
}
// Where WM_INPUT goes: the registered window, else the focused one, else the program's first window.
static HANDLE raw_target(const RAWINPUTDEVICE *reg) {
    if (reg->target && vx_window_get(reg->target)) return reg->target;
    if (g_focus && vx_window_get(g_focus)) return g_focus;
    for (int i = 0; i < g_window_next; i++) if (g_windows[i] && !g_windows[i]->destroyed && !(g_windows[i]->style & WS_CHILD)) return g_windows[i]->hwnd;
    return NULL;
}
static void post_raw(const RAWINPUTDEVICE *reg, const uint8_t *record, uint32_t size) {
    HANDLE target = raw_target(reg);
    if (!target) return;
    pthread_mutex_lock(&g_raw_lock);
    uint32_t i = g_raw_next++ % RAW_RING;
    memcpy(g_raw[i].data, record, size);
    g_raw[i].size = size;
    pthread_mutex_unlock(&g_raw_lock);
    vx_window_post(target, 0x00FF /* WM_INPUT */, 0 /* RIM_INPUT */, (LPARAM)(0x60000 + i));
}
void vx_raw_mouse(int dx, int dy, uint16_t button_flags, int16_t wheel) {
    __sync_fetch_and_add(&g_mouse_dx, dx);
    __sync_fetch_and_add(&g_mouse_dy, dy);
    if (button_flags & 0x0400 /* RI_MOUSE_WHEEL */) __sync_fetch_and_add(&g_mouse_dz, wheel);
    if (!g_raw_has_mouse || (!dx && !dy && !button_flags)) return;
    uint8_t r[48] = { 0 };
    *(DWORD *)r = 0; *(DWORD *)(r + 4) = 48; *(HANDLE *)(r + 8) = RAW_MOUSE_DEVICE;
    *(uint16_t *)(r + 28) = button_flags; *(int16_t *)(r + 30) = wheel;     // RAWMOUSE at 24: flags, padding, button flags and data
    *(LONG *)(r + 36) = dx; *(LONG *)(r + 40) = dy;                          // lLastX, lLastY
    post_raw(&g_raw_mouse, r, 48);
}
static void raw_key(int scan, int vk, int down) {
    if (!g_raw_has_keyboard) return;
    uint8_t r[40] = { 0 };
    *(DWORD *)r = 1; *(DWORD *)(r + 4) = 40; *(HANDLE *)(r + 8) = RAW_KEYBOARD_DEVICE;
    *(uint16_t *)(r + 24) = (uint16_t)(scan & 0xff);
    *(uint16_t *)(r + 26) = (uint16_t)((down ? 0 : 1) | (scan & 0x100 ? 2 : 0));   // RI_KEY_BREAK, RI_KEY_E0
    *(uint16_t *)(r + 30) = (uint16_t)vk;
    *(uint32_t *)(r + 32) = down ? WM_KEYDOWN : WM_KEYUP;
    post_raw(&g_raw_keyboard, r, 40);
}
#define RID_INPUT 0x10000003
#define RID_HEADER 0x10000005
static WINAPI uint32_t GetRawInputData(HANDLE h, uint32_t cmd, void *data, uint32_t *size, uint32_t header_size) {
    uint32_t i = (uint32_t)((uintptr_t)h - 0x60000);
    if (i >= RAW_RING || header_size != 24) { vx_set_last_error(6); return (uint32_t)-1; }
    pthread_mutex_lock(&g_raw_lock);
    uint32_t need = cmd == RID_HEADER ? 24 : g_raw[i].size;
    uint32_t r;
    if (!data) { *size = need; r = 0; }
    else if (*size < need) { vx_set_last_error(122); r = (uint32_t)-1; }
    else { memcpy(data, g_raw[i].data, need); r = need; }
    pthread_mutex_unlock(&g_raw_lock);
    return r;
}
static WINAPI uint32_t GetRawInputBuffer(void *data, uint32_t *size, uint32_t header_size) { (void)data; (void)header_size; *size = 0; return 0; }
static WINAPI LRESULT DefRawInputProc(void *const *raw, int n, uint32_t header_size) { (void)raw; (void)n; (void)header_size; return 0; }
// RAWINPUTDEVICELIST: device, type.
static WINAPI uint32_t GetRawInputDeviceList(uint8_t *list, uint32_t *n, uint32_t size) {
    if (!list) { *n = 2; return 0; }
    if (*n < 2 || size != 16) { *n = 2; vx_set_last_error(122); return (uint32_t)-1; }
    *(HANDLE *)list = RAW_MOUSE_DEVICE; *(DWORD *)(list + 8) = 0;
    *(HANDLE *)(list + 16) = RAW_KEYBOARD_DEVICE; *(DWORD *)(list + 24) = 1;
    return 2;
}
#define RIDI_DEVICENAME 0x20000007
#define RIDI_DEVICEINFO 0x2000000b
static uint32_t device_info(HANDLE dev, uint32_t cmd, void *data, uint32_t *size, int wide) {
    int mouse = dev == RAW_MOUSE_DEVICE;
    if (cmd == RIDI_DEVICENAME) {
        const char *name = mouse ? "\\\\?\\HID#VID_05AC&PID_0001#vxrt-mouse" : "\\\\?\\HID#VID_05AC&PID_0002#vxrt-keyboard";
        uint32_t need = (uint32_t)strlen(name) + 1;
        if (!data || *size < need) { *size = need; return data ? (uint32_t)-1 : 0; }
        if (wide) vx_utf8_to_utf16(name, need, data, need); else memcpy(data, name, need);
        return need;
    }
    if (cmd == RIDI_DEVICEINFO) {
        if (!data || *size < 32) { *size = 32; return data ? (uint32_t)-1 : 0; }
        uint8_t *d = data;
        memset(d, 0, 32);
        *(DWORD *)d = 32; *(DWORD *)(d + 4) = mouse ? 0 : 1;
        if (mouse) { *(DWORD *)(d + 8) = 1; *(DWORD *)(d + 12) = 5; *(DWORD *)(d + 16) = 1000; *(DWORD *)(d + 20) = 1; }
        else { *(DWORD *)(d + 8) = 4; *(DWORD *)(d + 16) = 1; *(DWORD *)(d + 20) = 12; *(DWORD *)(d + 24) = 3; *(DWORD *)(d + 28) = 104; }
        return 32;
    }
    vx_set_last_error(87);
    return (uint32_t)-1;
}
static WINAPI uint32_t GetRawInputDeviceInfoA(HANDLE dev, uint32_t cmd, void *data, uint32_t *size) { return device_info(dev, cmd, data, size, 0); }
static WINAPI uint32_t GetRawInputDeviceInfoW(HANDLE dev, uint32_t cmd, void *data, uint32_t *size) { return device_info(dev, cmd, data, size, 1); }

// SendInput: input as if it came from the devices: raw input, key and button messages to the focused window.
// INPUT: type (0 mouse, 1 keyboard), then MOUSEINPUT (dx, dy, data, flags, time, extra) or KEYBDINPUT
// (virtual key, scan code, flags, time, extra).
static WINAPI uint32_t SendInput(uint32_t n, const uint8_t *inputs, int size) {
    if (size != 40) { vx_set_last_error(87); return 0; }
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *in = inputs + (size_t)i * 40;
        HANDLE target = raw_target(&g_raw_keyboard);
        vx_window *w = vx_window_get(target);
        if (*(const DWORD *)in == 0) {
            LONG dx = *(const LONG *)(in + 8), dy = *(const LONG *)(in + 12);
            DWORD data = *(const DWORD *)(in + 16), flags = *(const DWORD *)(in + 20);
            uint16_t bf = 0;
            static const struct { DWORD in; uint16_t raw; uint32_t msg; int vk, down; } buttons[] = {
                { 0x2, 0x1, WM_LBUTTONDOWN, 1, 1 }, { 0x4, 0x2, WM_LBUTTONUP, 1, 0 }, { 0x8, 0x4, WM_RBUTTONDOWN, 2, 1 },
                { 0x10, 0x8, WM_RBUTTONUP, 2, 0 }, { 0x20, 0x10, WM_MBUTTONDOWN, 4, 1 }, { 0x40, 0x20, WM_MBUTTONUP, 4, 0 } };
            POINT p; cursor_pos(&p);
            if (w) ScreenToClient(w->hwnd, &p);
            for (size_t k = 0; k < sizeof buttons / sizeof buttons[0]; k++) {
                if (!(flags & buttons[k].in)) continue;
                bf |= buttons[k].raw;
                set_key(buttons[k].vk, buttons[k].down);
                if (w) vx_window_post(w->hwnd, buttons[k].msg, 0, MAKELPARAM(p.x, p.y));
            }
            int16_t wheel = 0;
            if (flags & 0x800) { bf |= 0x400; wheel = (int16_t)data; if (w) vx_window_post(w->hwnd, WM_MOUSEWHEEL, (WPARAM)(uint16_t)wheel << 16, 0); }
            if ((flags & 0x1) && !(flags & 0x8000)) {
                vx_raw_mouse(dx, dy, bf, wheel);
                if (w) vx_window_post(w->hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(p.x, p.y));
            } else if (bf) vx_raw_mouse(0, 0, bf, wheel);
        } else if (*(const DWORD *)in == 1) {
            uint16_t vk = *(const uint16_t *)(in + 8);
            DWORD flags = *(const DWORD *)(in + 12);
            int down = !(flags & 0x2);                                         // KEYEVENTF_KEYUP
            int code = -1;
            for (int k = 0; k < 128 && code < 0; k++) if (g_keymap[k].vk == vk) code = k;
            if (code >= 0 && w) {
                vx_key_event(w, code, down, 0, 0);
                if (down && ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9') || vk == ' ')) {
                    uint16_t ch = (uint16_t)(vk >= 'A' && vk <= 'Z' && !(g_keys[0x10] & 0x80) ? vk + 32 : vk);
                    vx_char_event(w, &ch, 1);
                }
            }
        }
    }
    return n;
}

// ---- window state ------------------------------------------------------------------------------------------------------------------
static WINAPI BOOL IsWindow(HANDLE h) { return vx_window_get(h) != NULL; }
static WINAPI BOOL IsWindowVisible(HANDLE h) { vx_window *w = vx_window_get(h); return w && w->visible; }
static WINAPI BOOL IsIconic(HANDLE h) { vx_window *w = vx_window_get(h); return w && w->minimized; }
static WINAPI BOOL IsZoomed(HANDLE h) { (void)h; return 0; }
static WINAPI BOOL IsWindowEnabled(HANDLE h) { vx_window *w = vx_window_get(h); return w && w->enabled; }
static WINAPI BOOL EnableWindow(HANDLE h, BOOL e) { vx_window *w = vx_window_get(h); if (!w) return 0; BOOL was = !w->enabled; w->enabled = e; return was; }
static WINAPI HANDLE GetParent(HANDLE h) { vx_window *w = vx_window_get(h); return w ? w->parent : NULL; }
static WINAPI HANDLE GetAncestor(HANDLE h, uint32_t flags) {
    (void)flags;
    vx_window *w = vx_window_get(h);
    while (w && w->parent && vx_window_get(w->parent)) w = vx_window_get(w->parent);
    return w ? w->hwnd : NULL;
}
static WINAPI HANDLE GetWindow(HANDLE h, uint32_t cmd) { (void)h; (void)cmd; return NULL; }
static WINAPI HANDLE GetDesktopWindow(void) { return HWND_DESKTOP_VALUE; }
static WINAPI HANDLE GetForegroundWindow(void) {
    if (trace_focus()) {
        static volatile int calls; static HANDLE last = (HANDLE)1;
        int n = __sync_add_and_fetch(&calls, 1);
        if (n <= 3 || last != g_active) { last = g_active; vx_log("focus: GetForegroundWindow #%d = %p", n, g_active); }
    }
    return g_active;
}
static WINAPI HANDLE GetActiveWindow(void) { return g_active; }
static WINAPI HANDLE GetFocus(void) { return g_focus; }
static WINAPI BOOL SetForegroundWindow(HANDLE h) { vx_window *w = vx_window_get(h); if (!w) return 0; vx_cocoa_activate(w); return 1; }
static WINAPI HANDLE SetActiveWindow(HANDLE h) { HANDLE o = g_active; SetForegroundWindow(h); return o; }
static WINAPI BOOL BringWindowToTop(HANDLE h) { return SetForegroundWindow(h); }
static WINAPI HANDLE SetFocus(HANDLE h) {
    HANDLE old = g_focus;
    if (old == h) return old;
    vx_window *o = vx_window_get(old), *n = vx_window_get(h);
    g_focus = h;
    if (o) vx_send_message(old, WM_KILLFOCUS, (WPARAM)h, 0);
    if (n) vx_send_message(h, WM_SETFOCUS, (WPARAM)old, 0);
    return old;
}
static WINAPI DWORD GetWindowThreadProcessId(HANDLE h, DWORD *pid) {
    vx_window *w = vx_window_get(h);
    if (pid) *pid = (DWORD)getpid();
    return w ? w->tid : 0;
}

static WINAPI BOOL GetClientRect(HANDLE h, RECT *r) {
    if (h == HWND_DESKTOP_VALUE) { vx_virtual_screen(r); r->right -= r->left; r->bottom -= r->top; r->left = r->top = 0; return 1; }
    vx_window *w = vx_window_get(h);
    if (!w) { vx_set_last_error(1400); return 0; }
    pthread_mutex_lock(&w->lock);
    *r = (RECT){ 0, 0, w->client_w, w->client_h };
    pthread_mutex_unlock(&w->lock);
    return 1;
}
static WINAPI BOOL GetWindowRect(HANDLE h, RECT *r) {
    if (h == HWND_DESKTOP_VALUE) return vx_virtual_screen(r);
    vx_window *w = vx_window_get(h);
    if (!w) { vx_set_last_error(1400); return 0; }
    pthread_mutex_lock(&w->lock);
    *r = w->rect;
    pthread_mutex_unlock(&w->lock);
    return 1;
}
// The client area sits at the bottom of the frame (the title bar is on top, no side borders).
static void client_origin(vx_window *w, POINT *o) {
    pthread_mutex_lock(&w->lock);
    o->x = w->rect.left;
    o->y = w->rect.bottom - w->client_h;
    pthread_mutex_unlock(&w->lock);
}
static WINAPI BOOL ClientToScreen(HANDLE h, POINT *p) {
    vx_window *w = vx_window_get(h);
    if (!w) return 0;
    POINT o; client_origin(w, &o);
    p->x += o.x; p->y += o.y;
    return 1;
}
static WINAPI BOOL ScreenToClient(HANDLE h, POINT *p) {
    vx_window *w = vx_window_get(h);
    if (!w) return 0;
    POINT o; client_origin(w, &o);
    p->x -= o.x; p->y -= o.y;
    return 1;
}
static WINAPI int MapWindowPoints(HANDLE from, HANDLE to, POINT *pts, uint32_t n) {
    POINT a = { 0, 0 }, b = { 0, 0 };
    vx_window *f = vx_window_get(from), *t = vx_window_get(to);
    if (f) client_origin(f, &a);
    if (t) client_origin(t, &b);
    for (uint32_t i = 0; i < n; i++) { pts[i].x += a.x - b.x; pts[i].y += a.y - b.y; }
    return (int)(uint16_t)(a.x - b.x) | (int)(uint16_t)(a.y - b.y) << 16;
}

#define SWP_NOSIZE 0x1
#define SWP_NOMOVE 0x2
#define SWP_NOACTIVATE 0x10
#define SWP_SHOWWINDOW 0x40
#define SWP_HIDEWINDOW 0x80
static WINAPI BOOL SetWindowPos(HANDLE h, HANDLE after, int x, int y, int cx, int cy, uint32_t flags) {
    (void)after;
    vx_window *w = vx_window_get(h);
    if (!w) { vx_set_last_error(1400); return 0; }
    pthread_mutex_lock(&w->lock);
    RECT r = w->rect;
    if (!(flags & SWP_NOMOVE)) { r.right += x - r.left; r.bottom += y - r.top; r.left = x; r.top = y; }
    if (!(flags & SWP_NOSIZE)) { r.right = r.left + cx; r.bottom = r.top + cy; }
    w->rect = r;
    pthread_mutex_unlock(&w->lock);
    if (w->style & WS_CHILD) vx_window_resized(w, r.right - r.left, r.bottom - r.top);
    else vx_cocoa_set_frame(w);
    if (flags & SWP_SHOWWINDOW) vx_ShowWindow(h, flags & SWP_NOACTIVATE ? 8 : 5);
    if (flags & SWP_HIDEWINDOW) vx_ShowWindow(h, 0);
    return 1;
}
static WINAPI BOOL MoveWindow(HANDLE h, int x, int y, int cx, int cy, BOOL repaint) { (void)repaint; return SetWindowPos(h, NULL, x, y, cx, cy, 0); }

// Window rectangles from client rectangles: a title bar on top when there is a caption, no borders.
static WINAPI BOOL AdjustWindowRectEx(RECT *r, DWORD style, BOOL menu, DWORD exstyle) {
    (void)menu; (void)exstyle;
    if ((style & WS_CAPTION) == WS_CAPTION && !(style & WS_CHILD)) r->top -= vx_cocoa_title_height();
    return 1;
}
static WINAPI BOOL AdjustWindowRect(RECT *r, DWORD style, BOOL menu) { return AdjustWindowRectEx(r, style, menu, 0); }
static WINAPI BOOL AdjustWindowRectExForDpi(RECT *r, DWORD style, BOOL menu, DWORD exstyle, uint32_t dpi) { (void)dpi; return AdjustWindowRectEx(r, style, menu, exstyle); }

// ---- window data (Get/SetWindowLong) -------------------------------------------------------------------------------------------
#define GWLP_WNDPROC   (-4)
#define GWLP_HINSTANCE (-6)
#define GWLP_HWNDPARENT (-8)
#define GWLP_ID        (-12)
#define GWL_STYLE      (-16)
#define GWL_EXSTYLE    (-20)
#define GWLP_USERDATA  (-21)
static int64_t get_long(HANDLE h, int index, int size) {
    vx_window *w = vx_window_get(h);
    if (!w) { vx_set_last_error(1400); return 0; }
    switch (index) {
    case GWLP_WNDPROC: return (int64_t)(intptr_t)w->proc;
    case GWLP_HINSTANCE: return (int64_t)(intptr_t)w->instance;
    case GWLP_HWNDPARENT: return (int64_t)(intptr_t)w->parent;
    case GWLP_ID: return w->id;
    case GWL_STYLE: return (int64_t)(int32_t)w->style;
    case GWL_EXSTYLE: return (int64_t)(int32_t)w->exstyle;
    case GWLP_USERDATA: return w->userdata;
    }
    if (index < 0 || index + size > w->extra_size) { vx_set_last_error(1413 /* ERROR_INVALID_INDEX */); return 0; }
    int64_t v = 0;
    memcpy(&v, w->extra + index, (size_t)size);
    return size == 4 ? (int64_t)(int32_t)v : v;
}
static int64_t set_long(HANDLE h, int index, int64_t value, int size) {
    vx_window *w = vx_window_get(h);
    if (!w) { vx_set_last_error(1400); return 0; }
    int64_t old = get_long(h, index, size);
    switch (index) {
    case GWLP_WNDPROC: w->proc = (WNDPROC)(intptr_t)value; return old;
    case GWLP_HINSTANCE: w->instance = (HANDLE)(intptr_t)value; return old;
    case GWLP_HWNDPARENT: w->parent = (HANDLE)(intptr_t)value; return old;
    case GWLP_ID: w->id = value; return old;
    case GWL_STYLE: {
        DWORD before = w->style;
        w->style = (DWORD)value;
        if ((before ^ w->style) & (WS_CAPTION | WS_THICKFRAME | WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX)) vx_cocoa_set_style(w);
        return old;
    }
    case GWL_EXSTYLE: w->exstyle = (DWORD)value; return old;
    case GWLP_USERDATA: w->userdata = value; return old;
    }
    if (index < 0 || index + size > w->extra_size) { vx_set_last_error(1413); return 0; }
    memcpy(w->extra + index, &value, (size_t)size);
    return old;
}
static WINAPI int64_t GetWindowLongPtrA(HANDLE h, int i) { return get_long(h, i, 8); }
static WINAPI int64_t GetWindowLongPtrW(HANDLE h, int i) { return get_long(h, i, 8); }
static WINAPI int64_t SetWindowLongPtrA(HANDLE h, int i, int64_t v) { return set_long(h, i, v, 8); }
static WINAPI int64_t SetWindowLongPtrW(HANDLE h, int i, int64_t v) { return set_long(h, i, v, 8); }
static WINAPI LONG GetWindowLongA(HANDLE h, int i) { return (LONG)get_long(h, i, 4); }
static WINAPI LONG GetWindowLongW(HANDLE h, int i) { return (LONG)get_long(h, i, 4); }
static WINAPI LONG SetWindowLongA(HANDLE h, int i, LONG v) { return (LONG)set_long(h, i, v, 4); }
static WINAPI LONG SetWindowLongW(HANDLE h, int i, LONG v) { return (LONG)set_long(h, i, v, 4); }
static WINAPI LRESULT CallWindowProcA(WNDPROC p, HANDLE h, uint32_t m, WPARAM w, LPARAM l) { return p ? p(h, m, w, l) : 0; }
static WINAPI LRESULT CallWindowProcW(WNDPROC p, HANDLE h, uint32_t m, WPARAM w, LPARAM l) { return p ? p(h, m, w, l) : 0; }
static WINAPI int GetClassNameA(HANDLE h, char *buf, int n) {
    vx_window *w = vx_window_get(h);
    if (!w || n <= 0) return 0;
    return (int)vx_return_string_a(w->cls->name, buf, (DWORD)n) < n ? (int)strlen(buf) : 0;
}
// The class name, cut to the buffer (n - 1 characters and the terminator); the characters copied.
static WINAPI int GetClassNameW(HANDLE h, WCHAR *buf, int n) {
    vx_window *w = vx_window_get(h);
    if (!w) { vx_set_last_error(1400 /* ERROR_INVALID_WINDOW_HANDLE */); return 0; }
    if (!buf || n <= 0) { vx_set_last_error(122 /* ERROR_INSUFFICIENT_BUFFER */); return 0; }
    size_t len = vx_utf8_to_utf16(w->cls->name, strlen(w->cls->name), buf, (size_t)n - 1);
    if (len > (size_t)n - 1) len = (size_t)n - 1;
    buf[len] = 0;
    return (int)len;
}
// A window's class: its GCL_*/GCLP_* fields. Class extra bytes are not kept (none are ever set here: zero, as
// RegisterClass makes them); the small icon and the menu name are not kept either.
static WINAPI uint64_t GetClassLongPtrW(HANDLE h, int index) {
    vx_window *w = vx_window_get(h);
    if (!w) { vx_set_last_error(1400); return 0; }
    vx_class *c = w->cls;
    switch (index) {
    case -10: return (uint64_t)(uintptr_t)c->background;      // GCLP_HBRBACKGROUND
    case -12: return (uint64_t)(uintptr_t)c->cursor;          // GCLP_HCURSOR
    case -14: return (uint64_t)(uintptr_t)c->icon;            // GCLP_HICON
    case -16: return (uint64_t)(uintptr_t)c->instance;        // GCLP_HMODULE
    case -18: return (uint64_t)c->wnd_extra;                  // GCL_CBWNDEXTRA
    case -20: return (uint64_t)c->cls_extra;                  // GCL_CBCLSEXTRA
    case -24: return (uint64_t)(uintptr_t)c->proc;            // GCLP_WNDPROC
    case -26: return c->style;                                // GCL_STYLE
    case -32: return c->atom;                                 // GCW_ATOM
    case -8: case -34: return 0;                              // GCLP_MENUNAME, GCLP_HICONSM
    default:
        if (index >= 0 && index + 8 <= c->cls_extra) return 0;
        vx_set_last_error(1413 /* ERROR_INVALID_INDEX */);
        return 0;
    }
}
static WINAPI DWORD GetClassLongW(HANDLE h, int index) { return (DWORD)GetClassLongPtrW(h, index); }

// ---- titles --------------------------------------------------------------------------------------------------------------------
static WINAPI BOOL SetWindowTextA(HANDLE h, const char *t) {
    vx_window *w = vx_window_get(h);
    if (!w) return 0;
    set_title(w, t);
    vx_cocoa_set_title(w);
    return 1;
}
static WINAPI BOOL SetWindowTextW(HANDLE h, const WCHAR *t) { char *a = vx_utf8_dup(t); BOOL r = SetWindowTextA(h, a); free(a); return r; }
static WINAPI int GetWindowTextA(HANDLE h, char *buf, int n) {
    vx_window *w = vx_window_get(h);
    if (!w || n <= 0) return 0;
    snprintf(buf, (size_t)n, "%s", w->title ? w->title : "");
    return (int)strlen(buf);
}
static WINAPI int GetWindowTextW(HANDLE h, WCHAR *buf, int n) {
    vx_window *w = vx_window_get(h);
    if (!w || n <= 0) return 0;
    const char *t = w->title ? w->title : "";
    size_t len = vx_utf8_to_utf16(t, strlen(t), buf, (size_t)n - 1);
    if (len > (size_t)n - 1) len = (size_t)n - 1;
    buf[len] = 0;
    return (int)len;
}
static WINAPI int GetWindowTextLengthA(HANDLE h) { vx_window *w = vx_window_get(h); return w && w->title ? (int)strlen(w->title) : 0; }
static WINAPI int GetWindowTextLengthW(HANDLE h) { vx_window *w = vx_window_get(h); return w && w->title ? (int)vx_utf8_to_utf16(w->title, strlen(w->title), NULL, 0) : 0; }

static int match_window(vx_window *w, const char *cls, const char *title) {
    if (w->destroyed || (w->style & WS_CHILD)) return 0;
    if (cls && ((uintptr_t)cls < 0x10000 ? w->cls->atom != (uint16_t)(uintptr_t)cls : strcasecmp(w->cls->name, cls))) return 0;
    if (title && strcmp(w->title ? w->title : "", title)) return 0;
    return 1;
}
static WINAPI HANDLE FindWindowA(const char *cls, const char *title) {
    pthread_mutex_lock(&g_win_lock);
    HANDLE h = NULL;
    for (int i = 0; i < g_window_next && !h; i++) if (g_windows[i] && match_window(g_windows[i], cls, title)) h = g_windows[i]->hwnd;
    pthread_mutex_unlock(&g_win_lock);
    return h;
}
static WINAPI HANDLE FindWindowW(const WCHAR *cls, const WCHAR *title) {
    char *c = (uintptr_t)cls < 0x10000 ? (char *)cls : vx_utf8_dup(cls), *t = vx_utf8_dup(title);
    HANDLE h = FindWindowA(c, t);
    if (c != (char *)cls) free(c);
    free(t);
    return h;
}
typedef BOOL (WINAPI *WNDENUMPROC)(HANDLE, LPARAM);
static WINAPI BOOL EnumWindows(WNDENUMPROC fn, LPARAM lp) {
    for (int i = 0; i < g_window_next; i++) {
        vx_window *w = g_windows[i];
        if (w && !w->destroyed && !(w->style & WS_CHILD) && !fn(w->hwnd, lp)) break;
    }
    return 1;
}
static WINAPI BOOL EnumThreadWindows(DWORD tid, WNDENUMPROC fn, LPARAM lp) {
    for (int i = 0; i < g_window_next; i++) {
        vx_window *w = g_windows[i];
        if (w && !w->destroyed && w->tid == tid && !fn(w->hwnd, lp)) break;
    }
    return 1;
}

// ---- painting ------------------------------------------------------------------------------------------------------------------
// PAINTSTRUCT: hdc, fErase, rcPaint, fRestore, fIncUpdate, rgbReserved[32].
typedef struct { HANDLE hdc; BOOL erase; RECT paint; BOOL restore, inc_update; uint8_t reserved[32]; } PAINTSTRUCT;
static WINAPI HANDLE BeginPaint(HANDLE h, PAINTSTRUCT *ps) {
    vx_window *w = vx_window_get(h);
    if (!w) return NULL;
    int erase = w->erase;
    w->invalid = 0; w->erase = 0;
    memset(ps, 0, sizeof *ps);
    ps->hdc = vx_dc_for_window(w);
    ps->paint = (RECT){ 0, 0, w->client_w, w->client_h };
    ps->erase = erase && !vx_send_message(h, WM_ERASEBKGND, (WPARAM)ps->hdc, 0);
    return ps->hdc;
}
static WINAPI BOOL EndPaint(HANDLE h, const PAINTSTRUCT *ps) { (void)h; vx_dc_release(ps->hdc); return 1; }
static WINAPI BOOL InvalidateRect(HANDLE h, const RECT *r, BOOL erase) {
    (void)r;
    if (!h) {
        for (int i = 0; i < g_window_next; i++) if (g_windows[i] && !g_windows[i]->destroyed) InvalidateRect(g_windows[i]->hwnd, NULL, erase);
        return 1;
    }
    vx_window *w = vx_window_get(h);
    if (!w) return 0;
    w->invalid = 1;
    if (erase) w->erase = 1;
    pthread_mutex_lock(&w->queue->lock);
    pthread_cond_broadcast(&w->queue->cond);
    pthread_mutex_unlock(&w->queue->lock);
    return 1;
}
static WINAPI BOOL InvalidateRgn(HANDLE h, HANDLE rgn, BOOL erase) { (void)rgn; return InvalidateRect(h, NULL, erase); }
static WINAPI BOOL ValidateRect(HANDLE h, const RECT *r) { (void)r; vx_window *w = vx_window_get(h); if (w) w->invalid = 0; return w != NULL; }
static WINAPI BOOL GetUpdateRect(HANDLE h, RECT *r, BOOL erase) {
    (void)erase;
    vx_window *w = vx_window_get(h);
    if (!w || !w->invalid) { if (r) *r = (RECT){ 0, 0, 0, 0 }; return 0; }
    if (r) *r = (RECT){ 0, 0, w->client_w, w->client_h };
    return 1;
}
static WINAPI BOOL UpdateWindow(HANDLE h) {
    vx_window *w = vx_window_get(h);
    if (!w) return 0;
    if (w->invalid && w->visible) vx_send_message(h, WM_PAINT, 0, 0);
    return 1;
}
static WINAPI BOOL RedrawWindow(HANDLE h, const RECT *r, HANDLE rgn, uint32_t flags) {
    (void)rgn;
    InvalidateRect(h, r, (flags & 0x4) != 0);       // RDW_ERASE
    if (flags & 0x100) UpdateWindow(h);             // RDW_UPDATENOW
    return 1;
}

// ---- the default window procedure ----------------------------------------------------------------------------------------------
static LRESULT def_window_proc(HANDLE h, uint32_t msg, WPARAM wp, LPARAM lp, int unicode) {
    vx_window *w = vx_window_get(h);
    switch (msg) {
    case WM_NCCREATE: return 1;
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); return 0; }
    case WM_ERASEBKGND: if (w && w->cls->background) { vx_fill_client(w, w->cls->background); return 1; } return 0;
    case WM_SETTEXT:
        if (unicode) SetWindowTextW(h, (const WCHAR *)lp); else SetWindowTextA(h, (const char *)lp);
        return 1;
    case WM_GETTEXT: return unicode ? GetWindowTextW(h, (WCHAR *)lp, (int)wp) : GetWindowTextA(h, (char *)lp, (int)wp);
    case WM_GETTEXTLENGTH: return unicode ? GetWindowTextLengthW(h) : GetWindowTextLengthA(h);
    case WM_NCHITTEST: return 1;                                       // HTCLIENT
    case WM_SYSCOMMAND: if ((wp & 0xfff0) == 0xF060) vx_window_post(h, WM_CLOSE, 0, 0); return 0;   // SC_CLOSE
    case WM_SYSKEYDOWN: if (wp == 0x73 && (lp & (1 << 29))) vx_window_post(h, WM_CLOSE, 0, 0); return 0;   // Alt+F4
    case WM_ACTIVATE: if (wp & 0xffff) SetFocus(h); return 0;
    case 0x0011: case 0x0016: return 1;                               // WM_QUERYENDSESSION, WM_ENDSESSION
    default: return 0;
    }
}
static WINAPI LRESULT DefWindowProcA(HANDLE h, uint32_t m, WPARAM w, LPARAM l) { return def_window_proc(h, m, w, l, 0); }
static WINAPI LRESULT DefWindowProcW(HANDLE h, uint32_t m, WPARAM w, LPARAM l) { return def_window_proc(h, m, w, l, 1); }

// ---- screens and system information -----------------------------------------------------------------------------------------------
int vx_virtual_screen(RECT *r) {
    CGRect b = CGDisplayBounds(CGMainDisplayID());
    *r = (RECT){ 0, 0, (LONG)b.size.width, (LONG)b.size.height };
    return 1;
}
static int refresh_rate(void) {
    CGDisplayModeRef m = CGDisplayCopyDisplayMode(CGMainDisplayID());
    double hz = m ? CGDisplayModeGetRefreshRate(m) : 60;
    if (m) CGDisplayModeRelease(m);
    return hz > 1 ? (int)(hz + 0.5) : 60;
}
static WINAPI int GetSystemMetrics(int i) {
    RECT s; vx_virtual_screen(&s);
    switch (i) {
    case 0: case 16: case 78: return s.right;                 // SM_CXSCREEN, SM_CXFULLSCREEN, SM_CXVIRTUALSCREEN
    case 1: case 79: return s.bottom;                         // SM_CYSCREEN, SM_CYVIRTUALSCREEN
    case 17: return s.bottom - vx_cocoa_title_height();       // SM_CYFULLSCREEN
    case 4: return vx_cocoa_title_height();                   // SM_CYCAPTION
    case 5: case 6: case 32: case 33: return 0;               // borders and frames: none
    case 11: case 12: return 32;                              // SM_CXICON, SM_CYICON
    case 13: case 14: return 32;                              // SM_CXCURSOR, SM_CYCURSOR
    case 19: return 1;                                        // SM_MOUSEPRESENT
    case 23: return 0;                                        // SM_SWAPBUTTON
    case 36: case 37: return 4;                               // SM_CXDOUBLECLK, SM_CYDOUBLECLK
    case 43: return 3;                                        // SM_CMOUSEBUTTONS
    case 49: case 50: return 16;                              // SM_CXSMICON, SM_CYSMICON
    case 75: return 1;                                        // SM_MOUSEWHEELPRESENT
    case 80: return 1;                                        // SM_CMONITORS
    case 76: case 77: return 0;                               // SM_XVIRTUALSCREEN, SM_YVIRTUALSCREEN
    default: return 0;
    }
}
static WINAPI int GetSystemMetricsForDpi(int i, uint32_t dpi) { (void)dpi; return GetSystemMetrics(i); }
#define SPI_GETWORKAREA 0x30
int vx_sysparams(uint32_t action, uint32_t param, void *data, int wide, uint32_t dpi);   // fonts, non-client metrics (sysparams.c)
static WINAPI BOOL SystemParametersInfoA(uint32_t action, uint32_t param, void *data, uint32_t flags) {
    (void)param; (void)flags;
    int r = vx_sysparams(action, param, data, 0, 96);
    if (r >= 0) return r;
    if (action == SPI_GETWORKAREA && data) { vx_virtual_screen(data); ((RECT *)data)->top = 0; return 1; }
    if (action == 0x68 /* SPI_GETWHEELSCROLLLINES */ && data) { *(uint32_t *)data = 3; return 1; }
    if (data && (action & 0xff00) == 0) memset(data, 0, 4);
    return 1;
}
static WINAPI BOOL SystemParametersInfoW(uint32_t a, uint32_t p, void *d, uint32_t f) {
    int r = vx_sysparams(a, p, d, 1, 96);
    return r >= 0 ? r : SystemParametersInfoA(a, p, d, f);
}

// DEVMODE: the A and W layouts differ in the two name fields; the display fields follow them.
static void fill_devmode(uint8_t *dm, int wide) {
    int name = wide ? 64 : 32, form = wide ? 64 : 32;
    int base = name;                                   // dmSpecVersion
    *(uint16_t *)(dm + base + 4) = wide ? 220 : 156;   // dmSize
    *(DWORD *)(dm + base + 8) = 0x00040000 | 0x00080000 | 0x00100000 | 0x00400000;   // BITSPERPEL | PELSWIDTH | PELSHEIGHT | DISPLAYFREQUENCY
    int after_form = base + 4 + 2 + 2 + 4 + 16 + 10 + form;   // dmLogPixels
    RECT s; vx_virtual_screen(&s);
    *(uint16_t *)(dm + after_form) = 96;
    *(DWORD *)(dm + after_form + 2) = 32;              // dmBitsPerPel
    *(DWORD *)(dm + after_form + 6) = (DWORD)s.right;  // dmPelsWidth
    *(DWORD *)(dm + after_form + 10) = (DWORD)s.bottom;
    *(DWORD *)(dm + after_form + 14) = 0;              // dmDisplayFlags
    *(DWORD *)(dm + after_form + 18) = (DWORD)refresh_rate();
}
static WINAPI BOOL EnumDisplaySettingsA(const char *dev, DWORD mode, uint8_t *dm) {
    (void)dev;
    if (mode != 0xffffffffu && mode != 0xfffffffeu && mode != 0) { vx_set_last_error(87); return 0; }
    fill_devmode(dm, 0);
    return 1;
}
static WINAPI BOOL EnumDisplaySettingsW(const WCHAR *dev, DWORD mode, uint8_t *dm) {
    (void)dev;
    if (mode != 0xffffffffu && mode != 0xfffffffeu && mode != 0) { vx_set_last_error(87); return 0; }
    fill_devmode(dm, 1);
    return 1;
}
static WINAPI BOOL EnumDisplaySettingsExA(const char *dev, DWORD mode, uint8_t *dm, DWORD flags) { (void)flags; return EnumDisplaySettingsA(dev, mode, dm); }
static WINAPI BOOL EnumDisplaySettingsExW(const WCHAR *dev, DWORD mode, uint8_t *dm, DWORD flags) { (void)flags; return EnumDisplaySettingsW(dev, mode, dm); }
static WINAPI LONG ChangeDisplaySettingsA(void *dm, DWORD flags) { (void)dm; (void)flags; return 0; }   // DISP_CHANGE_SUCCESSFUL: the mode stays
static WINAPI LONG ChangeDisplaySettingsW(void *dm, DWORD flags) { (void)dm; (void)flags; return 0; }
static WINAPI LONG ChangeDisplaySettingsExA(const char *d, void *dm, HANDLE h, DWORD f, void *p) { (void)d; (void)dm; (void)h; (void)f; (void)p; return 0; }
static WINAPI LONG ChangeDisplaySettingsExW(const WCHAR *d, void *dm, HANDLE h, DWORD f, void *p) { (void)d; (void)dm; (void)h; (void)f; (void)p; return 0; }
// DISPLAY_DEVICE: cb, name[32], string[128], flags, id[128], key[128] (A); the W form doubles the strings.
static WINAPI BOOL EnumDisplayDevicesA(const char *dev, DWORD i, uint8_t *dd, DWORD flags) {
    (void)dev; (void)flags;
    if (i) return 0;
    memset(dd + 4, 0, 32 + 128 + 4 + 128 + 128);
    strcpy((char *)dd + 4, "\\\\.\\DISPLAY1");
    strcpy((char *)dd + 36, "Apple Display (VXRuntime)");
    *(DWORD *)(dd + 164) = 0x1 | 0x4;                   // ATTACHED_TO_DESKTOP | PRIMARY_DEVICE
    return 1;
}
static WINAPI BOOL EnumDisplayDevicesW(const WCHAR *dev, DWORD i, uint8_t *dd, DWORD flags) {
    (void)dev; (void)flags;
    if (i) return 0;
    memset(dd + 4, 0, (32 + 128) * 2 + 4 + 128 * 4);
    vx_utf8_to_utf16("\\\\.\\DISPLAY1", 12, (WCHAR *)(dd + 4), 31);
    vx_utf8_to_utf16("Apple Display (VXRuntime)", 25, (WCHAR *)(dd + 68), 127);
    *(DWORD *)(dd + 324) = 0x1 | 0x4;
    return 1;
}

#define PRIMARY_MONITOR ((HANDLE)(intptr_t)0x10001)
static WINAPI HANDLE MonitorFromWindow(HANDLE h, DWORD f) { (void)h; (void)f; return PRIMARY_MONITOR; }
static WINAPI HANDLE MonitorFromPoint(POINT p, DWORD f) { (void)p; (void)f; return PRIMARY_MONITOR; }
static WINAPI HANDLE MonitorFromRect(const RECT *r, DWORD f) { (void)r; (void)f; return PRIMARY_MONITOR; }
// MONITORINFO(EX): cbSize, rcMonitor, rcWork, dwFlags (+ szDevice[32] in the EX form).
static WINAPI BOOL GetMonitorInfoA(HANDLE m, uint8_t *mi) {
    (void)m;
    DWORD size = *(DWORD *)mi;
    RECT s; vx_virtual_screen(&s);
    memcpy(mi + 4, &s, sizeof s); memcpy(mi + 20, &s, sizeof s);
    *(DWORD *)(mi + 36) = 1;                             // MONITORINFOF_PRIMARY
    if (size >= 72) strcpy((char *)mi + 40, "\\\\.\\DISPLAY1");
    return 1;
}
static WINAPI BOOL GetMonitorInfoW(HANDLE m, uint8_t *mi) {
    (void)m;
    DWORD size = *(DWORD *)mi;
    RECT s; vx_virtual_screen(&s);
    memcpy(mi + 4, &s, sizeof s); memcpy(mi + 20, &s, sizeof s);
    *(DWORD *)(mi + 36) = 1;
    if (size >= 104) vx_utf8_to_utf16("\\\\.\\DISPLAY1\0", 13, (WCHAR *)(mi + 40), 32);
    return 1;
}
typedef BOOL (WINAPI *MONITORENUMPROC)(HANDLE, HANDLE, RECT *, LPARAM);
static WINAPI BOOL EnumDisplayMonitors(HANDLE dc, const RECT *clip, MONITORENUMPROC fn, LPARAM lp) {
    (void)dc; (void)clip;
    RECT s; vx_virtual_screen(&s);
    fn(PRIMARY_MONITOR, NULL, &s, lp);
    return 1;
}
static WINAPI uint32_t GetDpiForWindow(HANDLE h) { (void)h; return 96; }
static WINAPI uint32_t GetDpiForSystem(void) { return 96; }   // (the awareness: winsta.c)

// ---- message boxes, misc ---------------------------------------------------------------------------------------------------------
static WINAPI int MessageBoxA(HANDLE owner, const char *text, const char *caption, uint32_t type) {
    (void)owner;
    vx_log("MessageBox: %s: %s", caption ? caption : "", text ? text : "");
    return vx_cocoa_message_box(text, caption, type);
}
static WINAPI int MessageBoxW(HANDLE owner, const WCHAR *text, const WCHAR *caption, uint32_t type) {
    char *t = vx_utf8_dup(text), *c = vx_utf8_dup(caption);
    int r = MessageBoxA(owner, t, c, type);
    free(t); free(c);
    return r;
}
static WINAPI int MessageBoxExA(HANDLE o, const char *t, const char *c, uint32_t type, uint16_t lang) { (void)lang; return MessageBoxA(o, t, c, type); }
static WINAPI int MessageBoxExW(HANDLE o, const WCHAR *t, const WCHAR *c, uint32_t type, uint16_t lang) { (void)lang; return MessageBoxW(o, t, c, type); }
static WINAPI BOOL MessageBeep(uint32_t type) { (void)type; vx_cocoa_beep(); return 1; }

static uint16_t g_message_atoms;
static char g_message_names[256][64];
static WINAPI uint32_t RegisterWindowMessageA(const char *name) {
    for (int i = 0; i < g_message_atoms; i++) if (!strcasecmp(g_message_names[i], name)) return 0xC000u + 0x400 + i;
    if (g_message_atoms == 256) return 0;
    snprintf(g_message_names[g_message_atoms], 64, "%s", name);
    return 0xC000u + 0x400 + g_message_atoms++;
}
static WINAPI uint32_t RegisterWindowMessageW(const WCHAR *name) { char *a = vx_utf8_dup(name); uint32_t r = RegisterWindowMessageA(a); free(a); return r; }

// Hooks, menus, clipboard and dialogs exist only as far as programs probe for them.
static WINAPI HANDLE SetWindowsHookExA(int id, void *fn, HANDLE mod, DWORD tid) {
    (void)fn; (void)mod;
    if (trace_focus()) vx_log("input: SetWindowsHookEx(%d, thread %u) accepted, not called", id, (unsigned)tid);
    return (HANDLE)(intptr_t)0x1;
}
static WINAPI HANDLE SetWindowsHookExW(int id, void *fn, HANDLE mod, DWORD tid) { return SetWindowsHookExA(id, fn, mod, tid); }
static WINAPI BOOL UnhookWindowsHookEx(HANDLE h) { (void)h; return 1; }
static WINAPI LRESULT CallNextHookEx(HANDLE h, int code, WPARAM w, LPARAM l) { (void)h; (void)code; (void)w; (void)l; return 0; }
static WINAPI HANDLE GetMenu(HANDLE h) { (void)h; return NULL; }
static WINAPI BOOL SetMenu(HANDLE h, HANDLE m) { (void)h; (void)m; return 1; }
static WINAPI BOOL DrawMenuBar(HANDLE h) { (void)h; return 1; }
static WINAPI HANDLE GetSystemMenu(HANDLE h, BOOL revert) { (void)h; (void)revert; return NULL; }
static WINAPI BOOL OpenClipboard(HANDLE h) { (void)h; return 0; }
static WINAPI BOOL CloseClipboard(void) { return 1; }
static WINAPI BOOL IsClipboardFormatAvailable(uint32_t f) { (void)f; return 0; }

// wsprintf: a Windows varargs function; the arguments sit in 8-byte slots, as vx_format expects.
static WINAPI int wvsprintfA(char *buf, const char *fmt, const char *ap) { return vx_format(buf, 1025, fmt, ap); }
static WINAPI int wsprintfA(char *buf, const char *fmt, ...) {
    vx_va_list ap;
    vx_va_start(ap, fmt);
    int n = vx_format(buf, 1025, fmt, ap);
    vx_va_end(ap);
    return n;
}
static WINAPI char *CharUpperA(char *s) { if ((uintptr_t)s < 0x10000) return (char *)(uintptr_t)toupper((int)(uintptr_t)s); for (char *p = s; *p; p++) *p = (char)toupper((unsigned char)*p); return s; }
static WINAPI char *CharLowerA(char *s) { if ((uintptr_t)s < 0x10000) return (char *)(uintptr_t)tolower((int)(uintptr_t)s); for (char *p = s; *p; p++) *p = (char)tolower((unsigned char)*p); return s; }
static WINAPI char *CharNextA(const char *s) { return (char *)(*s ? s + 1 : s); }
static WINAPI BOOL GetWindowPlacement(HANDLE h, uint8_t *wp) {
    vx_window *w = vx_window_get(h);
    if (!w) return 0;
    *(uint32_t *)(wp + 8) = w->minimized ? 2 : 1;               // showCmd
    memcpy(wp + 28, &w->rect, sizeof w->rect);                  // rcNormalPosition
    return 1;
}

// ---- Command Clips (control.c): the windows as text, actions as messages ----------------------------------------------
typedef struct { char *p; size_t n, cap; } sbuf;
static void sb_put(sbuf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) { b->cap = (b->n + n + 1) * 2; b->p = realloc(b->p, b->cap); }
    memcpy(b->p + b->n, s, n); b->n += n; b->p[b->n] = 0;
}
static void sb_printf(sbuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sb_printf(sbuf *b, const char *fmt, ...) {
    char tmp[256]; va_list ap; va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap); va_end(ap);
    sb_put(b, tmp, n < 0 ? 0 : (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1);
}
static void sb_json_str(sbuf *b, const char *s) {
    sb_put(b, "\"", 1);
    for (const unsigned char *c = (const unsigned char *)(s ? s : ""); *c; c++) {
        if (*c == '"' || *c == '\\') { char e[2] = { '\\', (char)*c }; sb_put(b, e, 2); }
        else if (*c < 0x20) sb_printf(b, "\\u%04x", *c);
        else sb_put(b, (const char *)c, 1);
    }
    sb_put(b, "\"", 1);
}
char *vx_ui_state(int texts) {
    sbuf b = { 0 };
    sb_printf(&b, "{\"idle\":%s,\"active\":%lld,\"focus\":%lld,\"windows\":[", vx_ui_idle() ? "true" : "false",
              (long long)(intptr_t)g_active, (long long)(intptr_t)g_focus);
    int first = 1;
    pthread_mutex_lock(&g_win_lock);
    for (int i = 0; i < g_window_next; i++) {
        vx_window *w = g_windows[i];
        if (!w || w->destroyed) continue;
        sb_printf(&b, "%s{\"hwnd\":%lld,\"parent\":%lld,\"class\":", first ? "" : ",", (long long)(intptr_t)w->hwnd, (long long)(intptr_t)w->parent);
        first = 0;
        sb_json_str(&b, w->cls ? w->cls->name : "");
        sb_put(&b, ",\"title\":", 9); sb_json_str(&b, w->title);
        POINT o; client_origin(w, &o);
        sb_printf(&b, ",\"visible\":%s,\"enabled\":%s,\"client\":[%d,%d],\"origin\":[%d,%d]", w->visible ? "true" : "false",
                  w->enabled ? "true" : "false", w->client_w, w->client_h, (int)o.x, (int)o.y);
        if (!texts) { sb_put(&b, "}", 1); continue; }
        sb_put(&b, ",\"texts\":[", 10);
        pthread_mutex_lock(&vx_text_lock);
        for (int k = 0; k < w->texts.n; k++) {
            vx_text *t = &w->texts.v[k];
            sb_printf(&b, "%s{\"text\":", k ? "," : ""); sb_json_str(&b, t->s);
            sb_printf(&b, ",\"box\":[%d,%d,%d,%d]}", (int)t->r.left, (int)t->r.top, (int)(t->r.right - t->r.left), (int)(t->r.bottom - t->r.top));
        }
        pthread_mutex_unlock(&vx_text_lock);
        sb_put(&b, "]}", 2);
    }
    pthread_mutex_unlock(&g_win_lock);
    sb_put(&b, "]}", 2);
    return b.p;
}
// Idle: every thread that owns a window waits for messages with none queued, none sent to it and nothing of its windows
// left to paint. (A program that renders in a loop of PeekMessage is never idle.)
int vx_ui_idle(void) {
    vx_queue *qs[256]; int nq = 0;
    pthread_mutex_lock(&g_queues_lock);
    for (vx_queue *q = g_queues; q && nq < 256; q = q->next) qs[nq++] = q;
    pthread_mutex_unlock(&g_queues_lock);
    int owners = 0;
    for (int k = 0; k < nq; k++) {
        vx_queue *q = qs[k];
        int owns = 0, unpainted = 0;
        pthread_mutex_lock(&g_win_lock);
        for (int i = 0; i < g_window_next; i++) {
            vx_window *w = g_windows[i];
            if (w && !w->destroyed && w->tid == q->tid) { owns = 1; unpainted |= w->invalid && w->visible; }
        }
        pthread_mutex_unlock(&g_win_lock);
        if (!owns) continue;
        owners++;
        pthread_mutex_lock(&q->lock);
        int busy = !q->waiting || q->count || q->sent || q->quit || unpainted;
        pthread_mutex_unlock(&q->lock);
        if (busy) return 0;
    }
    return owners > 0;
}
HANDLE vx_ui_window(HANDLE hwnd) {
    if (hwnd) return vx_window_get(hwnd) ? hwnd : NULL;
    if (g_active && vx_window_get(g_active)) return g_active;
    HANDLE found = NULL;
    pthread_mutex_lock(&g_win_lock);
    for (int i = 0; i < g_window_next && !found; i++) {
        vx_window *w = g_windows[i];
        if (w && !w->destroyed && w->visible && !w->parent) found = w->hwnd;
    }
    pthread_mutex_unlock(&g_win_lock);
    return found;
}
// The first drawn text containing needle, in hwnd or (0) in any window: its window and the middle of its box.
int vx_ui_find_text(HANDLE hwnd, const char *needle, HANDLE *in, POINT *at) {
    int found = 0;
    pthread_mutex_lock(&g_win_lock);
    pthread_mutex_lock(&vx_text_lock);
    for (int i = 0; i < g_window_next && !found; i++) {
        vx_window *w = g_windows[i];
        if (!w || w->destroyed || (hwnd && w->hwnd != hwnd)) continue;
        for (int k = 0; k < w->texts.n && !found; k++)
            if (strstr(w->texts.v[k].s, needle)) {
                RECT r = w->texts.v[k].r;
                *in = w->hwnd; *at = (POINT){ (r.left + r.right) / 2, (r.top + r.bottom) / 2 }; found = 1;
            }
    }
    pthread_mutex_unlock(&vx_text_lock);
    pthread_mutex_unlock(&g_win_lock);
    return found;
}
int vx_ui_snapshot(HANDLE hwnd, const char *png) {
    vx_window *w = vx_window_get(hwnd);
    if (!w) return 0;
    pthread_mutex_lock(&w->lock);
    CGImageRef img = w->ctx ? CGBitmapContextCreateImage(w->ctx) : NULL;
    pthread_mutex_unlock(&w->lock);
    if (!img) return 0;
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)png, (CFIndex)strlen(png), false);
    CGImageDestinationRef d = url ? CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL) : NULL;
    int ok = 0;
    if (d) { CGImageDestinationAddImage(d, img, NULL); ok = CGImageDestinationFinalize(d); CFRelease(d); }
    if (url) CFRelease(url);
    CGImageRelease(img);
    return ok;
}
void vx_ui_click(HANDLE hwnd, int x, int y) {
    vx_window *w = vx_window_get(hwnd);
    if (!w) return;
    LPARAM at = (LPARAM)((uint32_t)(uint16_t)x | (uint32_t)(uint16_t)y << 16);
    queue_post(w->queue, hwnd, WM_MOUSEMOVE, 0, at);
    queue_post(w->queue, hwnd, WM_LBUTTONDOWN, 1 /* MK_LBUTTON */, at);
    queue_post(w->queue, hwnd, WM_LBUTTONUP, 0, at);
}
void vx_ui_key(HANDLE hwnd, int vk) {
    vx_window *w = vx_window_get(hwnd);
    if (!w) return;
    queue_post(w->queue, hwnd, WM_KEYDOWN, (WPARAM)vk, 1);
    queue_post(w->queue, hwnd, WM_KEYUP, (WPARAM)vk, (LPARAM)(1 | 3u << 30));
}
void vx_ui_type(HANDLE hwnd, const char *utf8) {
    vx_window *w = vx_window_get(hwnd);
    if (!w) return;
    size_t n = vx_utf8_to_utf16(utf8, strlen(utf8), NULL, 0);
    WCHAR *u = malloc((n + 1) * sizeof *u);
    vx_utf8_to_utf16(utf8, strlen(utf8), u, n);
    for (size_t i = 0; i < n; i++) queue_post(w->queue, hwnd, WM_CHAR, u[i], 1);
    free(u);
}

static const vx_export user32[] = {
    VX_EXPORT(RegisterClassExA), VX_EXPORT(RegisterClassExW), VX_EXPORT(RegisterClassA), VX_EXPORT(RegisterClassW),
    VX_EXPORT(UnregisterClassA), VX_EXPORT(UnregisterClassW), VX_EXPORT(GetClassInfoExA), VX_EXPORT(GetClassInfoExW),
    VX_EXPORT(CreateWindowExA), VX_EXPORT(CreateWindowExW), VX_EXPORT(DestroyWindow),
    { "ShowWindow", (void *)vx_ShowWindow }, VX_EXPORT(ShowWindowAsync), VX_EXPORT(UpdateWindow),
    VX_EXPORT(SendMessageA), VX_EXPORT(SendMessageW), VX_EXPORT(SendMessageTimeoutA), VX_EXPORT(SendMessageTimeoutW),
    VX_EXPORT(PostMessageA), VX_EXPORT(PostMessageW), VX_EXPORT(SendNotifyMessageA), VX_EXPORT(SendNotifyMessageW),
    VX_EXPORT(PostThreadMessageA), VX_EXPORT(PostThreadMessageW), VX_EXPORT(PostQuitMessage),
    VX_EXPORT(GetMessageA), VX_EXPORT(GetMessageW), VX_EXPORT(PeekMessageA), VX_EXPORT(PeekMessageW), VX_EXPORT(WaitMessage),
    VX_EXPORT(GetMessageTime), VX_EXPORT(GetQueueStatus), VX_EXPORT(MsgWaitForMultipleObjects), VX_EXPORT(MsgWaitForMultipleObjectsEx),
    VX_EXPORT(TranslateMessage), VX_EXPORT(DispatchMessageA), VX_EXPORT(DispatchMessageW),
    VX_EXPORT(SetTimer), VX_EXPORT(KillTimer),
    VX_EXPORT(DefWindowProcA), VX_EXPORT(DefWindowProcW), VX_EXPORT(CallWindowProcA), VX_EXPORT(CallWindowProcW),
    VX_EXPORT(GetWindowLongPtrA), VX_EXPORT(GetWindowLongPtrW), VX_EXPORT(SetWindowLongPtrA), VX_EXPORT(SetWindowLongPtrW),
    VX_EXPORT(GetWindowLongA), VX_EXPORT(GetWindowLongW), VX_EXPORT(SetWindowLongA), VX_EXPORT(SetWindowLongW), VX_EXPORT(GetClassNameA), VX_EXPORT(GetClassNameW), VX_EXPORT(GetClassLongPtrW), VX_EXPORT(GetClassLongW),
    VX_EXPORT(SetWindowTextA), VX_EXPORT(SetWindowTextW), VX_EXPORT(GetWindowTextA), VX_EXPORT(GetWindowTextW),
    VX_EXPORT(GetWindowTextLengthA), VX_EXPORT(GetWindowTextLengthW), VX_EXPORT(FindWindowA), VX_EXPORT(FindWindowW),
    VX_EXPORT(EnumWindows), VX_EXPORT(EnumThreadWindows),
    VX_EXPORT(IsWindow), VX_EXPORT(IsWindowVisible), VX_EXPORT(IsIconic), VX_EXPORT(IsZoomed), VX_EXPORT(IsWindowEnabled),
    VX_EXPORT(EnableWindow), VX_EXPORT(GetParent), VX_EXPORT(GetAncestor), VX_EXPORT(GetWindow), VX_EXPORT(GetDesktopWindow),
    VX_EXPORT(GetForegroundWindow), VX_EXPORT(GetActiveWindow), VX_EXPORT(GetFocus), VX_EXPORT(SetForegroundWindow),
    VX_EXPORT(SetActiveWindow), VX_EXPORT(BringWindowToTop), VX_EXPORT(SetFocus), VX_EXPORT(GetWindowThreadProcessId),
    VX_EXPORT(GetClientRect), VX_EXPORT(GetWindowRect), VX_EXPORT(ClientToScreen), VX_EXPORT(ScreenToClient), VX_EXPORT(MapWindowPoints),
    VX_EXPORT(SetWindowPos), VX_EXPORT(MoveWindow), VX_EXPORT(AdjustWindowRect), VX_EXPORT(AdjustWindowRectEx),
    VX_EXPORT(AdjustWindowRectExForDpi), VX_EXPORT(GetWindowPlacement),
    VX_EXPORT(BeginPaint), VX_EXPORT(EndPaint), VX_EXPORT(InvalidateRect), VX_EXPORT(InvalidateRgn), VX_EXPORT(ValidateRect),
    VX_EXPORT(GetUpdateRect), VX_EXPORT(RedrawWindow),
    VX_EXPORT(GetKeyState), VX_EXPORT(GetAsyncKeyState), VX_EXPORT(GetKeyboardState), VX_EXPORT(MapVirtualKeyA), VX_EXPORT(MapVirtualKeyW),
    VX_EXPORT(MapVirtualKeyExA), VX_EXPORT(MapVirtualKeyExW), VX_EXPORT(GetKeyboardLayout), VX_EXPORT(GetKeyboardType), VX_EXPORT(GetKeyNameTextA),
    VX_EXPORT(GetCursorPos), VX_EXPORT(SetCursorPos), VX_EXPORT(ShowCursor), VX_EXPORT(SetCursor), VX_EXPORT(GetCursor),
    VX_EXPORT(LoadCursorA), VX_EXPORT(LoadCursorW), VX_EXPORT(LoadIconA), VX_EXPORT(LoadIconW), VX_EXPORT(LoadImageA), VX_EXPORT(LoadImageW),
    VX_EXPORT(DestroyCursor), VX_EXPORT(DestroyIcon), VX_EXPORT(ClipCursor), VX_EXPORT(GetClipCursor),
    VX_EXPORT(SetCapture), VX_EXPORT(ReleaseCapture), VX_EXPORT(GetCapture), VX_EXPORT(TrackMouseEvent), VX_EXPORT(GetDoubleClickTime),
    VX_EXPORT(RegisterRawInputDevices), VX_EXPORT(GetRawInputData), VX_EXPORT(GetRegisteredRawInputDevices),
    VX_EXPORT(GetRawInputBuffer), VX_EXPORT(DefRawInputProc), VX_EXPORT(GetRawInputDeviceList), VX_EXPORT(GetRawInputDeviceInfoA),
    VX_EXPORT(GetRawInputDeviceInfoW), VX_EXPORT(SendInput),
    VX_EXPORT(GetSystemMetrics), VX_EXPORT(GetSystemMetricsForDpi), VX_EXPORT(SystemParametersInfoA), VX_EXPORT(SystemParametersInfoW),
    VX_EXPORT(EnumDisplaySettingsA), VX_EXPORT(EnumDisplaySettingsW), VX_EXPORT(EnumDisplaySettingsExA), VX_EXPORT(EnumDisplaySettingsExW),
    VX_EXPORT(ChangeDisplaySettingsA), VX_EXPORT(ChangeDisplaySettingsW), VX_EXPORT(ChangeDisplaySettingsExA), VX_EXPORT(ChangeDisplaySettingsExW),
    VX_EXPORT(EnumDisplayDevicesA), VX_EXPORT(EnumDisplayDevicesW),
    VX_EXPORT(MonitorFromWindow), VX_EXPORT(MonitorFromPoint), VX_EXPORT(MonitorFromRect), VX_EXPORT(GetMonitorInfoA), VX_EXPORT(GetMonitorInfoW),
    VX_EXPORT(EnumDisplayMonitors), VX_EXPORT(GetDpiForWindow), VX_EXPORT(GetDpiForSystem),
    VX_EXPORT(MessageBoxA), VX_EXPORT(MessageBoxW), VX_EXPORT(MessageBoxExA), VX_EXPORT(MessageBoxExW), VX_EXPORT(MessageBeep),
    VX_EXPORT(RegisterWindowMessageA), VX_EXPORT(RegisterWindowMessageW),
    VX_EXPORT(SetWindowsHookExA), VX_EXPORT(SetWindowsHookExW), VX_EXPORT(UnhookWindowsHookEx), VX_EXPORT(CallNextHookEx),
    VX_EXPORT(GetMenu), VX_EXPORT(SetMenu), VX_EXPORT(DrawMenuBar), VX_EXPORT(GetSystemMenu),
    VX_EXPORT(OpenClipboard), VX_EXPORT(CloseClipboard), VX_EXPORT(IsClipboardFormatAvailable),
    VX_EXPORT(wvsprintfA), VX_EXPORT(wsprintfA), VX_EXPORT(CharUpperA), VX_EXPORT(CharLowerA), VX_EXPORT(CharNextA),
};

// The system's control classes. Their windows exist (so dialogs and splash screens that make labels, buttons and
// progress bars get handles) but draw nothing of their own; progress bars keep their range and position.
static WINAPI LRESULT control_proc(HANDLE h, uint32_t m, WPARAM w, LPARAM l) {
    switch (m) {
    case 0x401: case 0x402: case 0x403: case 0x404: case 0x405: case 0x406: case 0x407: case 0x408: case 0x409:   // PBM_*
    case 0x40a: case 0x410:
        return 0;
    case 0x87: return 0x80 | 0x1;                                  // WM_GETDLGCODE: DLGC_STATIC-like, wants arrows
    }
    return def_window_proc(h, m, w, l, 1);
}
static void register_controls(void) {
    static const char *const names[] = {
        "Static", "Button", "Edit", "ListBox", "ComboBox", "ComboLBox", "ScrollBar", "msctls_progress32", "msctls_trackbar32",
        "msctls_updown32", "SysLink", "RichEdit20W", "RichEdit20A", "RICHEDIT50W", "SysListView32", "SysTreeView32",
        "SysTabControl32", "tooltips_class32", "SysHeader32", "msctls_statusbar32", "ToolbarWindow32", "ReBarWindow32",
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        register_class(names[i], 1, 0, control_proc, 0, 64, NULL, NULL, NULL, NULL);
}

void vx_user32_init(void) {
    register_controls();
    vx_register_module("user32.dll", user32, sizeof user32 / sizeof user32[0]);
}

#ifdef __aarch64__
#include "thunks/user32.c.inc"
#endif
