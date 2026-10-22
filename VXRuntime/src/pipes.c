// Named pipes (\\.\pipe\NAME) between threads and between vxrt processes, on Unix domain sockets.
//
// A pipe name is a socket in /tmp/vxrt-<uid>/ (short: socket paths are limited to 104 bytes), named by a hash of
// the C: root and the pipe name, so separate roots do not meet. A server's instances of one name share the
// listening socket: ConnectNamedPipe accepts one client per instance. Every write goes as a frame (a 4-byte
// length, then the bytes); message-mode reads return one message (ERROR_MORE_DATA and the rest later when the
// buffer is small), byte-mode reads take what the frames hold. With FILE_FLAG_OVERLAPPED, ConnectNamedPipe and
// ReadFile complete in the background (the OVERLAPPED's event is set, a completion port gets its packet).
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include "vx.h"


#define ERROR_FILE_NOT_FOUND 2
#define ERROR_INVALID_HANDLE 6
#define ERROR_BROKEN_PIPE 109
#define ERROR_SEM_TIMEOUT 121
#define ERROR_PIPE_BUSY 231
#define ERROR_NO_DATA 232
#define ERROR_PIPE_NOT_CONNECTED 233
#define ERROR_MORE_DATA 234
#define ERROR_PIPE_CONNECTED 535
#define ERROR_PIPE_LISTENING 536
#define ERROR_IO_PENDING 997
#define STATUS_PENDING 0x103
#define PIPE_TYPE_MESSAGE 4
#define PIPE_READMODE_MESSAGE 2
#define PIPE_NOWAIT 1
#define FILE_FLAG_OVERLAPPED 0x40000000u

typedef struct listener { char path[128]; int fd, refs; struct listener *next; } listener;
typedef struct {
    int server;
    listener *ls;                    // server: the shared listening socket
    int fd;                          // the connection, -1 while there is none
    DWORD mode;                      // PIPE_TYPE_* | PIPE_READMODE_* | PIPE_NOWAIT
    int overlapped;
    uint8_t *rest; size_t rest_len, rest_off;   // the unread part of a message
    pthread_mutex_t lock;            // reads
} vx_pipe;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static listener *g_listeners;
// VXRT_TRACE_PIPES=1: every frame written and every read, with its first bytes.
static int trace_on(void) {
    static int on = -1;
    if (on < 0) on = getenv("VXRT_TRACE_PIPES") && atoi(getenv("VXRT_TRACE_PIPES"));
    return on;
}
static void trace_bytes(const char *what, vx_pipe *p, const void *buf, size_t n, DWORD err) {
    if (!trace_on()) return;
    char hex[3 * 16 + 1] = "";
    for (size_t i = 0; i < n && i < 16; i++) snprintf(hex + 3 * i, 4, "%02x ", ((const uint8_t *)buf)[i]);
    vx_log("pipe %s fd %d %s: %zu bytes, error %u: %s", p->server ? "server" : "client", p->fd, what, n, err, hex);
}

// The socket path of a pipe name ("\\.\pipe\NAME", any case and slashes).
static int pipe_path(const char *name, char *out, size_t cap) {
    if (strncasecmp(name, "\\\\.\\pipe\\", 9) && strncasecmp(name, "//./pipe/", 9)) return 0;
    uint64_t h = 1469598103934665603ull;
    for (const char *c = vx_root(); *c; c++) h = (h ^ (uint8_t)*c) * 1099511628211ull;
    h = (h ^ 0xff) * 1099511628211ull;
    for (const char *c = name + 9; *c; c++) h = (h ^ (uint8_t)(*c >= 'A' && *c <= 'Z' ? *c + 32 : *c)) * 1099511628211ull;
    char dir[64];
    snprintf(dir, sizeof dir, "/tmp/vxrt-%u", (unsigned)getuid());
    mkdir(dir, 0700);
    snprintf(out, cap, "%s/%016llx", dir, (unsigned long long)h);
    return 1;
}
int vx_is_pipe_name(const char *name) { return !strncasecmp(name, "\\\\.\\pipe\\", 9) || !strncasecmp(name, "//./pipe/", 9); }

static vx_pipe *pipe_of(HANDLE h) { return vx_handle_data(h, VX_HANDLE_PIPE); }
static void pipe_destroy(void *p) {
    vx_pipe *pp = p;
    if (pp->fd >= 0) close(pp->fd);
    if (pp->ls) {
        pthread_mutex_lock(&g_lock);
        if (--pp->ls->refs == 0) {
            close(pp->ls->fd);
            unlink(pp->ls->path);
            for (listener **l = &g_listeners; *l; l = &(*l)->next) if (*l == pp->ls) { *l = pp->ls->next; break; }
            free(pp->ls);
        }
        pthread_mutex_unlock(&g_lock);
    }
    free(pp->rest);
    pthread_mutex_destroy(&pp->lock);
    free(pp);
}
static vx_pipe *new_pipe(int server, DWORD mode, int overlapped) {
    vx_pipe *p = calloc(1, sizeof *p);
    p->server = server; p->fd = -1; p->mode = mode; p->overlapped = overlapped;
    pthread_mutex_init(&p->lock, NULL);
    return p;
}

// ---- the server ---------------------------------------------------------------------------------------------------------------
static HANDLE create_named_pipe(const char *name, DWORD open_mode, DWORD pipe_mode) {
    char path[128];
    if (!pipe_path(name, path, sizeof path)) { vx_set_last_error(123 /* ERROR_INVALID_NAME */); return (HANDLE)(intptr_t)-1; }
    pthread_mutex_lock(&g_lock);
    listener *ls = g_listeners;
    while (ls && strcmp(ls->path, path)) ls = ls->next;
    if (!ls) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un a = { .sun_family = AF_UNIX };
        snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
        unlink(path);                                              // a socket left by a process that ended
        if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof a) || listen(fd, 64)) {
            if (fd >= 0) close(fd);
            pthread_mutex_unlock(&g_lock);
            vx_set_last_error(5);
            return (HANDLE)(intptr_t)-1;
        }
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        ls = calloc(1, sizeof *ls);
        snprintf(ls->path, sizeof ls->path, "%s", path);
        ls->fd = fd;
        ls->next = g_listeners; g_listeners = ls;
    }
    ls->refs++;
    pthread_mutex_unlock(&g_lock);
    vx_pipe *p = new_pipe(1, pipe_mode, (open_mode & FILE_FLAG_OVERLAPPED) != 0);
    p->ls = ls;
    if (vx_trace || trace_on()) vx_log("CreateNamedPipe %s open %#x mode %#x", name, (unsigned)open_mode, (unsigned)pipe_mode);
    return vx_handle_new(VX_HANDLE_PIPE, p, pipe_destroy);
}
static WINAPI HANDLE CreateNamedPipeA(const char *name, DWORD open_mode, DWORD pipe_mode, DWORD max, DWORD out, DWORD in, DWORD ms, void *sa) {
    (void)max; (void)out; (void)in; (void)ms; (void)sa;
    return create_named_pipe(name, open_mode, pipe_mode);
}
static WINAPI HANDLE CreateNamedPipeW(const WCHAR *name, DWORD open_mode, DWORD pipe_mode, DWORD max, DWORD out, DWORD in, DWORD ms, void *sa) {
    (void)max; (void)out; (void)in; (void)ms; (void)sa;
    char *a = vx_utf8_dup(name);
    HANDLE h = create_named_pipe(a, open_mode, pipe_mode);
    free(a);
    return h;
}
static int accept_client(vx_pipe *p) {
    int fd;
    if (p->mode & PIPE_NOWAIT) {                                   // only a client that is already waiting
        struct pollfd w = { p->ls->fd, POLLIN, 0 };
        if (poll(&w, 1, 0) != 1) return 0;
    }
    do fd = accept(p->ls->fd, NULL, NULL); while (fd < 0 && errno == EINTR);
    if (fd < 0) return 0;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    p->fd = fd;
    return 1;
}
typedef struct { HANDLE h; vx_pipe *p; void *ov; void *buf; DWORD n; } async_op;
static void *connect_in_background(void *arg) {
    async_op *op = arg;
    int ok = accept_client(op->p);
    vx_io_completed(op->h, op->ov, 0, ok ? 0 : ERROR_BROKEN_PIPE);
    free(op);
    return NULL;
}
// An operation that completes later: like Windows, its OVERLAPPED's event is reset when it starts.
static void start_background(void *(*fn)(void *), async_op *op) {
    *(uint64_t *)op->ov = STATUS_PENDING;
    HANDLE event = (HANDLE)((uintptr_t)*(HANDLE *)((uint8_t *)op->ov + 24) & ~(uintptr_t)1);
    if (event) {
        BOOL (WINAPI *reset)(HANDLE) = (BOOL (WINAPI *)(HANDLE))vx_find_export("kernel32.dll", "ResetEvent");
        reset(event);
    }
    pthread_t t; pthread_attr_t a;
    pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED); pthread_attr_setstacksize(&a, 256 << 10);
    pthread_create(&t, &a, fn, op);
    pthread_attr_destroy(&a);
}
static WINAPI BOOL ConnectNamedPipe(HANDLE h, void *ov) {
    vx_pipe *p = pipe_of(h);
    if (trace_on()) vx_log("ConnectNamedPipe %p ov %p (overlapped %d, fd %d)", h, ov, p ? p->overlapped : -1, p ? p->fd : -1);
    if (!p || !p->server) { vx_set_last_error(ERROR_INVALID_HANDLE); return 0; }
    if (p->fd >= 0) { vx_set_last_error(ERROR_PIPE_CONNECTED); return 0; }
    if (p->overlapped && ov) {
        async_op *op = calloc(1, sizeof *op);
        op->h = h; op->p = p; op->ov = ov;
        start_background(connect_in_background, op);
        vx_set_last_error(ERROR_IO_PENDING);
        return 0;
    }
    // A non-blocking instance (PIPE_NOWAIT, as SteamVR's are) is polled: "listening" until a client comes, then
    // "connected".
    if (p->mode & PIPE_NOWAIT) { vx_set_last_error(accept_client(p) ? ERROR_PIPE_CONNECTED : ERROR_PIPE_LISTENING); return 0; }
    if (!accept_client(p)) { vx_set_last_error(ERROR_BROKEN_PIPE); return 0; }
    return 1;
}
static WINAPI BOOL DisconnectNamedPipe(HANDLE h) {
    vx_pipe *p = pipe_of(h);
    if (!p || !p->server) { vx_set_last_error(ERROR_INVALID_HANDLE); return 0; }
    if (p->fd >= 0) { close(p->fd); p->fd = -1; }
    free(p->rest); p->rest = NULL; p->rest_len = p->rest_off = 0;
    return 1;
}

// ---- the client --------------------------------------------------------------------------------------------------------------------
HANDLE vx_pipe_open(const char *name, DWORD flags) {
    char path[128];
    if (!pipe_path(name, path, sizeof path)) { vx_set_last_error(ERROR_FILE_NOT_FOUND); return (HANDLE)(intptr_t)-1; }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof a)) {
        int e = errno;
        if (fd >= 0) close(fd);
        vx_set_last_error(e == ECONNREFUSED || e == ENOENT ? ERROR_FILE_NOT_FOUND : ERROR_PIPE_BUSY);
        return (HANDLE)(intptr_t)-1;
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    vx_pipe *p = new_pipe(0, 0, (flags & FILE_FLAG_OVERLAPPED) != 0);
    p->fd = fd;
    if (vx_trace) vx_log("connected to pipe %s", name);
    vx_set_last_error(0);
    return vx_handle_new(VX_HANDLE_PIPE, p, pipe_destroy);
}
// WaitNamedPipe: until a server has created the pipe (its socket exists).
static BOOL wait_named_pipe(const char *name, DWORD ms) {
    char path[128];
    if (!pipe_path(name, path, sizeof path)) { vx_set_last_error(ERROR_FILE_NOT_FOUND); return 0; }
    struct timespec start; clock_gettime(CLOCK_MONOTONIC, &start);
    if (ms == 0) ms = 50;                                          // NMPWAIT_USE_DEFAULT_WAIT
    for (;;) {
        struct stat st;
        if (!stat(path, &st) && S_ISSOCK(st.st_mode)) return 1;
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t elapsed = (uint64_t)(now.tv_sec - start.tv_sec) * 1000 + (uint64_t)((now.tv_nsec - start.tv_nsec) / 1000000);
        if (ms != 0xffffffffu && elapsed >= ms) { vx_set_last_error(ERROR_SEM_TIMEOUT); return 0; }
        usleep(2000);
    }
}
static WINAPI BOOL WaitNamedPipeA(const char *name, DWORD ms) { return wait_named_pipe(name, ms); }
static WINAPI BOOL WaitNamedPipeW(const WCHAR *name, DWORD ms) { char *a = vx_utf8_dup(name); BOOL r = wait_named_pipe(a, ms); free(a); return r; }

// ---- reading and writing ----------------------------------------------------------------------------------------------------------
static int read_all(int fd, void *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = recv(fd, (uint8_t *)buf + got, n - got, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return 0;
        got += (size_t)r;
    }
    return 1;
}
// One read: from the rest of a message, else the next frame. Returns the bytes given and the error (0, MORE_DATA,
// BROKEN_PIPE).
static DWORD pipe_read(vx_pipe *p, void *buf, DWORD n, DWORD *err) {
    pthread_mutex_lock(&p->lock);
    *err = 0;
    if (p->fd < 0) { pthread_mutex_unlock(&p->lock); *err = p->server ? ERROR_PIPE_LISTENING : ERROR_PIPE_NOT_CONNECTED; return 0; }
    if (!p->rest || p->rest_off == p->rest_len) {
        uint32_t len;
        if (p->mode & PIPE_NOWAIT) {                               // nothing there: no data now (or the other end went)
            ssize_t r;
            do r = recv(p->fd, &len, 4, MSG_PEEK | MSG_DONTWAIT); while (r < 0 && errno == EINTR);
            if (r <= 0) { pthread_mutex_unlock(&p->lock); *err = r == 0 || errno != EAGAIN ? ERROR_BROKEN_PIPE : ERROR_NO_DATA; return 0; }
        }
        free(p->rest); p->rest = NULL; p->rest_len = p->rest_off = 0;
        if (!read_all(p->fd, &len, 4)) { pthread_mutex_unlock(&p->lock); *err = ERROR_BROKEN_PIPE; return 0; }
        p->rest = malloc(len ? len : 1);
        if (len && !read_all(p->fd, p->rest, len)) { pthread_mutex_unlock(&p->lock); *err = ERROR_BROKEN_PIPE; return 0; }
        p->rest_len = len;
    }
    size_t left = p->rest_len - p->rest_off, take = left < n ? left : n;
    memcpy(buf, p->rest + p->rest_off, take);
    p->rest_off += take;
    if (take < left && (p->mode & PIPE_READMODE_MESSAGE)) *err = ERROR_MORE_DATA;
    trace_bytes("read", p, buf, take, *err);
    pthread_mutex_unlock(&p->lock);
    return (DWORD)take;
}
static void *read_in_background(void *arg) {
    async_op *op = arg;
    DWORD err, got = pipe_read(op->p, op->buf, op->n, &err);
    vx_io_completed(op->h, op->ov, got, err);
    free(op);
    return NULL;
}
// ReadFile on a pipe handle (files.c asks first): 1 if handled, with the result in *ok.
int vx_pipe_read(HANDLE h, void *buf, DWORD n, DWORD *got, void *ov, BOOL *ok) {
    vx_pipe *p = pipe_of(h);
    if (!p) return 0;
    if (p->overlapped && ov) {
        async_op *op = calloc(1, sizeof *op);
        op->h = h; op->p = p; op->ov = ov; op->buf = buf; op->n = n;
        if (got) *got = 0;
        start_background(read_in_background, op);
        vx_set_last_error(ERROR_IO_PENDING);
        *ok = 0;
        return 1;
    }
    DWORD err, r = pipe_read(p, buf, n, &err);
    if (got) *got = r;
    if (ov) vx_io_completed(h, ov, r, err);
    if (err) vx_set_last_error(err);
    *ok = !err;
    return 1;
}
int vx_pipe_write(HANDLE h, const void *buf, DWORD n, DWORD *written, void *ov, BOOL *ok) {
    vx_pipe *p = pipe_of(h);
    if (!p) return 0;
    if (p->fd < 0) { vx_set_last_error(p->server ? ERROR_PIPE_LISTENING : ERROR_NO_DATA); if (written) *written = 0; *ok = 0; return 1; }
    uint32_t len = n;
    trace_bytes("write", p, buf, n, 0);
    struct iovec iov[2] = { { &len, 4 }, { (void *)buf, n } };
    size_t total = 4 + (size_t)n, sent = 0;
    while (sent < total) {
        ssize_t r = writev(p->fd, iov, 2);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) { vx_set_last_error(errno == EPIPE ? ERROR_NO_DATA : ERROR_BROKEN_PIPE); if (written) *written = 0; *ok = 0; return 1; }
        sent += (size_t)r;
        size_t skip = (size_t)r;                                       // move past what went out
        for (int i = 0; i < 2 && skip; i++) {
            size_t d = skip < iov[i].iov_len ? skip : iov[i].iov_len;
            iov[i].iov_base = (uint8_t *)iov[i].iov_base + d; iov[i].iov_len -= d; skip -= d;
        }
    }
    if (written) *written = n;
    if (ov) vx_io_completed(h, ov, n, 0);
    *ok = 1;
    return 1;
}
// PeekNamedPipe on a pipe handle: 0 not a pipe, 1 peeked, -1 failed (the last error set). An empty pipe whose other
// end has gone fails with BROKEN_PIPE, as on Windows: SteamVR's server learns that a client went this way, and kept
// The Lab's first connection (closed at its VR_Shutdown) until its next VR_Init, then took it for a legacy-input app.
int vx_pipe_peek(HANDLE h, void *buf, DWORD n, DWORD *got, DWORD *avail, DWORD *left) {
    vx_pipe *p = pipe_of(h);
    if (!p) return 0;
    pthread_mutex_lock(&p->lock);
    DWORD have = 0, msg = 0;
    int gone = 0;
    if (p->rest && p->rest_off < p->rest_len) {
        have = msg = (DWORD)(p->rest_len - p->rest_off);
        if (buf && n) memcpy(buf, p->rest + p->rest_off, have < n ? have : n);
    } else if (p->fd >= 0) {
        uint32_t len;
        int pending = 0;
        ioctl(p->fd, FIONREAD, &pending);
        if (pending >= 4 && recv(p->fd, &len, 4, MSG_PEEK | MSG_DONTWAIT) == 4) { have = (DWORD)(pending - 4); msg = len; }
        else if (!pending) {
            char c;
            ssize_t r;
            do r = recv(p->fd, &c, 1, MSG_PEEK | MSG_DONTWAIT); while (r < 0 && errno == EINTR);
            gone = r == 0 || (r < 0 && errno != EAGAIN);
        }
    }
    pthread_mutex_unlock(&p->lock);
    if (gone) { vx_set_last_error(ERROR_BROKEN_PIPE); return -1; }
    if (got) *got = buf ? (have < n ? have : n) : 0;
    if (avail) *avail = have;
    if (left) *left = (p->mode & PIPE_READMODE_MESSAGE) && msg > n ? msg - n : 0;
    return 1;
}
int vx_is_pipe(HANDLE h) { return pipe_of(h) != NULL; }

// ---- state ------------------------------------------------------------------------------------------------------------------------
static WINAPI BOOL SetNamedPipeHandleState(HANDLE h, DWORD *mode, DWORD *max, DWORD *timeout) {
    (void)max; (void)timeout;
    vx_pipe *p = pipe_of(h);
    if (!p) { vx_set_last_error(ERROR_INVALID_HANDLE); return 0; }
    if (mode) p->mode = (p->mode & ~(DWORD)(PIPE_READMODE_MESSAGE | PIPE_NOWAIT)) | (*mode & (PIPE_READMODE_MESSAGE | PIPE_NOWAIT));
    return 1;
}
static WINAPI BOOL GetNamedPipeHandleStateA(HANDLE h, DWORD *state, DWORD *instances, DWORD *max, DWORD *timeout, char *user, DWORD n) {
    (void)max; (void)timeout; (void)user; (void)n;
    vx_pipe *p = pipe_of(h);
    if (!p) { vx_set_last_error(ERROR_INVALID_HANDLE); return 0; }
    if (state) *state = p->mode & (PIPE_READMODE_MESSAGE | PIPE_NOWAIT);
    if (instances) *instances = 1;
    return 1;
}
static WINAPI BOOL GetNamedPipeInfo(HANDLE h, DWORD *flags, DWORD *out, DWORD *in, DWORD *max) {
    vx_pipe *p = pipe_of(h);
    if (!p) { vx_set_last_error(ERROR_INVALID_HANDLE); return 0; }
    if (flags) *flags = (p->server ? 1 : 0) | (p->mode & PIPE_TYPE_MESSAGE);
    if (out) *out = 65536; if (in) *in = 65536; if (max) *max = 255;
    return 1;
}
// The process at the other end (the Mac's process ID is the Windows one here).
static WINAPI BOOL GetNamedPipeClientProcessId(HANDLE h, DWORD *pid) {
    vx_pipe *p = pipe_of(h);
    pid_t peer = 0; socklen_t len = sizeof peer;
    if (!p || p->fd < 0 || getsockopt(p->fd, SOL_LOCAL, LOCAL_PEERPID, &peer, &len)) { vx_set_last_error(ERROR_INVALID_HANDLE); return 0; }
    *pid = (DWORD)(p->server ? peer : getpid());
    return 1;
}
static WINAPI BOOL GetNamedPipeServerProcessId(HANDLE h, DWORD *pid) {
    vx_pipe *p = pipe_of(h);
    pid_t peer = 0; socklen_t len = sizeof peer;
    if (!p || p->fd < 0 || getsockopt(p->fd, SOL_LOCAL, LOCAL_PEERPID, &peer, &len)) { vx_set_last_error(ERROR_INVALID_HANDLE); return 0; }
    *pid = (DWORD)(p->server ? getpid() : peer);
    return 1;
}
static WINAPI BOOL TransactNamedPipe(HANDLE h, const void *in, DWORD in_n, void *out, DWORD out_n, DWORD *got, void *ov) {
    BOOL ok;
    DWORD written;
    if (!vx_pipe_write(h, in, in_n, &written, NULL, &ok) || !ok) return 0;
    vx_pipe *p = pipe_of(h);
    DWORD err, r = pipe_read(p, out, out_n, &err);
    if (got) *got = r;
    if (ov) vx_io_completed(h, ov, r, err);
    if (err) { vx_set_last_error(err); return 0; }
    return 1;
}
static WINAPI BOOL CallNamedPipeA(const char *name, const void *in, DWORD in_n, void *out, DWORD out_n, DWORD *got, DWORD ms) {
    if (!wait_named_pipe(name, ms)) return 0;
    HANDLE h = vx_pipe_open(name, 0);
    if (h == (HANDLE)(intptr_t)-1) return 0;
    vx_pipe *p = pipe_of(h);
    p->mode |= PIPE_READMODE_MESSAGE;
    BOOL r = TransactNamedPipe(h, in, in_n, out, out_n, got, NULL);
    vx_handle_close(h);
    return r;
}

static const vx_export k32[] = {
    VX_EXPORT(CreateNamedPipeA), VX_EXPORT(CreateNamedPipeW), VX_EXPORT(ConnectNamedPipe), VX_EXPORT(DisconnectNamedPipe),
    VX_EXPORT(WaitNamedPipeA), VX_EXPORT(WaitNamedPipeW), VX_EXPORT(SetNamedPipeHandleState), VX_EXPORT(GetNamedPipeHandleStateA),
    { "GetNamedPipeHandleStateW", (void *)GetNamedPipeHandleStateA }, VX_EXPORT(GetNamedPipeInfo), VX_EXPORT(TransactNamedPipe),
    VX_EXPORT(CallNamedPipeA), VX_EXPORT(GetNamedPipeClientProcessId), VX_EXPORT(GetNamedPipeServerProcessId),
};
void vx_pipes_init(void) { vx_register_module("kernel32.dll", k32, sizeof k32 / sizeof k32[0]); }

#ifdef __aarch64__
#include "thunks/pipes.c.inc"
#endif
