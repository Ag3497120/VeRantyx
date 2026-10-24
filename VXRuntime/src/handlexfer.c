// Handles between processes that need the other process's help (DuplicateHandle, src/sync.c): a file descriptor
// (anonymous pipes, files: handles 0x10000 + fd) going to another process, and any handle taken *from* another
// process (DuplicateHandle with a source process that is not this one). Steam's process pipes do both: the web helper
// gives steam.exe its pipe ends, steam.exe takes handles out of its children ("Failed DuplicateHandle call in
// ConnectPipe: 50" before, after which steam.exe went down through its crash handler).
//
// Every VXRuntime process answers such requests on a Unix socket of its own, <user temp dir>/vxh/<pid>, in a folder
// only this user can enter (0700, owned by the user; anything else there is refused: no server, the requests fail
// closed). A request comes from a process of the same user only (the socket's peer credentials). The answer to "give
// me handle h" is the descriptor itself (SCM_RIGHTS) for a file handle, else the value of a handle the server put in
// the asking process's inbox (sync.c's way for events, mutexes, semaphores, mappings and processes); "take this
// descriptor" makes it a file handle of the server's process and answers its value.
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include "vx.h"

#define ERROR_ACCESS_DENIED  5
#define ERROR_INVALID_HANDLE 6
#define ERROR_NOT_SUPPORTED  50

enum { OP_GIVE = 1, OP_TAKE = 2 };
typedef struct { uint32_t magic, op; uint64_t handle; int32_t close_source, from_pid; } xfer_request;
typedef struct { uint32_t magic; int32_t kind; uint64_t handle; uint32_t error; } xfer_reply;   // kind: 0 failed, 1 a handle value, 4 a descriptor
#define XFER_MAGIC 0x76786866u                                   // 'vxhf'

HANDLE vx_give_to_process(int pid, HANDLE src);                   // sync.c (the inbox)
int vx_handle_fd(HANDLE h);                                       // kernel32.c

// The folder: made 0700 if it is not there; used only if it is a real folder of this user that no one else may enter.
static int socket_dir(char *out, size_t cap) {
    char tmp[512];
    size_t n = confstr(_CS_DARWIN_USER_TEMP_DIR, tmp, sizeof tmp);
    if (!n || n > sizeof tmp) return 0;
    if (snprintf(out, cap, "%svxh", tmp) >= (int)cap) return 0;
    mkdir(out, 0700);
    struct stat st;
    if (lstat(out, &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 077)) return 0;
    return 1;
}
static int socket_path(int pid, struct sockaddr_un *a) {
    char dir[512];
    if (!socket_dir(dir, sizeof dir)) return 0;
    memset(a, 0, sizeof *a);
    a->sun_family = AF_UNIX;
    return snprintf(a->sun_path, sizeof a->sun_path, "%s/%d", dir, pid) < (int)sizeof a->sun_path;
}

static int send_all(int s, const void *p, size_t n) {
    for (const char *c = p; n;) { ssize_t k = send(s, c, n, 0); if (k <= 0) { if (k < 0 && errno == EINTR) continue; return 0; } c += k; n -= (size_t)k; }
    return 1;
}
static int recv_all(int s, void *p, size_t n) {
    for (char *c = p; n;) { ssize_t k = recv(s, c, n, 0); if (k <= 0) { if (k < 0 && errno == EINTR) continue; return 0; } c += k; n -= (size_t)k; }
    return 1;
}
// One message with a descriptor along (SCM_RIGHTS); fd < 0: none.
static int send_with_fd(int s, const void *p, size_t n, int fd) {
    struct iovec iov = { (void *)p, n };
    union { struct cmsghdr h; char buf[CMSG_SPACE(sizeof(int))]; } c;
    struct msghdr m = { 0 };
    m.msg_iov = &iov; m.msg_iovlen = 1;
    if (fd >= 0) {
        memset(&c, 0, sizeof c);
        m.msg_control = c.buf; m.msg_controllen = sizeof c.buf;
        struct cmsghdr *h = CMSG_FIRSTHDR(&m);
        h->cmsg_level = SOL_SOCKET; h->cmsg_type = SCM_RIGHTS; h->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(h), &fd, sizeof fd);
    }
    for (;;) { ssize_t k = sendmsg(s, &m, 0); if (k == (ssize_t)n) return 1; if (k < 0 && errno == EINTR) continue; return 0; }
}
static int recv_with_fd(int s, void *p, size_t n, int *fd) {
    struct iovec iov = { p, n };
    union { struct cmsghdr h; char buf[CMSG_SPACE(sizeof(int))]; } c;
    struct msghdr m = { 0 };
    m.msg_iov = &iov; m.msg_iovlen = 1; m.msg_control = c.buf; m.msg_controllen = sizeof c.buf;
    *fd = -1;
    ssize_t k;
    do k = recvmsg(s, &m, MSG_WAITALL); while (k < 0 && errno == EINTR);
    if (k != (ssize_t)n) return 0;
    for (struct cmsghdr *h = CMSG_FIRSTHDR(&m); h; h = CMSG_NXTHDR(&m, h))
        if (h->cmsg_level == SOL_SOCKET && h->cmsg_type == SCM_RIGHTS && h->cmsg_len >= CMSG_LEN(sizeof(int))) memcpy(fd, CMSG_DATA(h), sizeof *fd);
    return 1;
}
// A file handle for a descriptor this process now has (0x10000 + fd, as files.c makes them).
static HANDLE fd_handle(int fd) {
    if (fd < 0 || fd >= 0x10000) { if (fd >= 0) close(fd); return NULL; }
    return (HANDLE)(intptr_t)(0x10000 + fd);
}

// ---- the server ------------------------------------------------------------------------------------------------------
static void serve_one(int c) {
    uid_t uid; gid_t gid;
    xfer_request q;
    xfer_reply r = { XFER_MAGIC, 0, 0, ERROR_NOT_SUPPORTED };
    if (getpeereid(c, &uid, &gid) || uid != getuid()) return;    // (fail closed: another user's process)
    int got_fd = -1;
    if (!recv_with_fd(c, &q, sizeof q, &got_fd) || q.magic != XFER_MAGIC) { if (got_fd >= 0) close(got_fd); return; }
    if (q.op == OP_GIVE) {
        HANDLE h = (HANDLE)(intptr_t)q.handle;
        int fd = vx_handle_fd(h);
        if (fd >= 0) {
            r.kind = 4; r.error = 0;
            send_with_fd(c, &r, sizeof r, fd);
            if (q.close_source && fd > 2) close(fd);
            return;
        }
        HANDLE given = q.from_pid > 0 ? vx_give_to_process(q.from_pid, h) : NULL;
        if (given) {
            r.kind = 1; r.handle = (uint64_t)(intptr_t)given; r.error = 0;
            if (q.close_source) vx_handle_close(h);
        } else r.error = vx_get_last_error() ? vx_get_last_error() : ERROR_INVALID_HANDLE;
        send_all(c, &r, sizeof r);
    } else if (q.op == OP_TAKE) {
        HANDLE h = fd_handle(got_fd);
        got_fd = -1;
        if (h) { r.kind = 1; r.handle = (uint64_t)(intptr_t)h; r.error = 0; }
        send_all(c, &r, sizeof r);
    }
    if (got_fd >= 0) close(got_fd);
}
static char g_server_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
static void remove_server_socket(void) { if (*g_server_path) unlink(g_server_path); }
static void *server(void *arg) {
    int s = (int)(intptr_t)arg;
    for (;;) {
        int c = accept(s, NULL, NULL);
        if (c < 0) { if (errno == EINTR || errno == ECONNABORTED) continue; break; }
        struct timeval tv = { 5, 0 };                          // (a client that says nothing is not waited for)
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        serve_one(c);
        close(c);
    }
    return NULL;
}
void vx_handle_server_start(void) {
    struct sockaddr_un a;
    if (!socket_path(getpid(), &a)) return;
    unlink(a.sun_path);                                           // (a socket left by an earlier process with this pid)
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) return;
    fcntl(s, F_SETFD, FD_CLOEXEC);
    mode_t old = umask(077);
    int ok = !bind(s, (struct sockaddr *)&a, sizeof a) && !listen(s, 16);
    umask(old);
    if (!ok) { close(s); return; }
    snprintf(g_server_path, sizeof g_server_path, "%s", a.sun_path);
    atexit(remove_server_socket);
    pthread_t t; pthread_attr_t at;
    pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED); pthread_attr_setstacksize(&at, 256 << 10);
    if (pthread_create(&t, &at, server, (void *)(intptr_t)s)) { close(s); unlink(g_server_path); *g_server_path = 0; }
    pthread_attr_destroy(&at);
}

// ---- asking another process ----------------------------------------------------------------------------------------
static int connect_to(int pid) {
    struct sockaddr_un a;
    if (!socket_path(pid, &a)) return -1;
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) return -1;
    fcntl(s, F_SETFD, FD_CLOEXEC);
    struct timeval tv = { 5, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (connect(s, (struct sockaddr *)&a, sizeof a)) { close(s); return -1; }
    uid_t uid; gid_t gid;                                          // (the server is this user's too)
    if (getpeereid(s, &uid, &gid) || uid != getuid()) { close(s); return -1; }
    return s;
}
// A handle of process pid made one of this process (DuplicateHandle from another process). NULL with the last error set
// when that process does not answer (not a VXRuntime process, or gone) or has no such handle.
HANDLE vx_handle_pull(int pid, HANDLE remote, int close_source) {
    int s = connect_to(pid);
    if (s < 0) { vx_set_last_error(ERROR_NOT_SUPPORTED); return NULL; }
    xfer_request q = { XFER_MAGIC, OP_GIVE, (uint64_t)(intptr_t)remote, close_source != 0, (int32_t)getpid() };
    xfer_reply r;
    int fd = -1;
    HANDLE h = NULL;
    if (send_all(s, &q, sizeof q) && recv_with_fd(s, &r, sizeof r, &fd) && r.magic == XFER_MAGIC) {
        if (r.kind == 4 && fd >= 0) { h = fd_handle(fd); fd = -1; }
        else if (r.kind == 1) h = (HANDLE)(intptr_t)r.handle;
        if (!h) vx_set_last_error(r.error ? r.error : ERROR_INVALID_HANDLE);
    } else vx_set_last_error(ERROR_NOT_SUPPORTED);
    if (fd >= 0) close(fd);
    close(s);
    return h;
}
// A descriptor of this process given to process pid: the value of the file handle it has there.
HANDLE vx_handle_push_fd(int pid, int fd) {
    int s = connect_to(pid);
    if (s < 0) { vx_set_last_error(ERROR_NOT_SUPPORTED); return NULL; }
    xfer_request q = { XFER_MAGIC, OP_TAKE, 0, 0, (int32_t)getpid() };
    xfer_reply r;
    HANDLE h = NULL;
    if (send_with_fd(s, &q, sizeof q, fd) && recv_all(s, &r, sizeof r) && r.magic == XFER_MAGIC && r.kind == 1) h = (HANDLE)(intptr_t)r.handle;
    else vx_set_last_error(ERROR_NOT_SUPPORTED);
    close(s);
    return h;
}
