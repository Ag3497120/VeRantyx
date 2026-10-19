// ws2_32 (Winsock) on BSD sockets. A SOCKET is the file descriptor.
//
// What differs between the two and is converted here: sockaddr (BSD has a length byte; AF_INET6 is 23 on
// Windows, 30 on macOS), fd_set (a count and an array of sockets), hostent and addrinfo layouts, socket option
// values (SO_RCVTIMEO in milliseconds, linger), MSG_WAITALL, ioctlsocket, and error codes (WSAE*, 10000 + n).
// Sockets never raise SIGPIPE (SO_NOSIGPIPE).
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include "vx.h"

typedef uint64_t SOCKET;
#define INVALID_SOCKET (~(SOCKET)0)
#define SOCKET_ERROR (-1)
#define WIN_AF_INET6 23

static int wsa_error(int e) {
    switch (e) {
    case EINTR: return 10004;          case EBADF: return 10038;         case EACCES: return 10013;
    case EFAULT: return 10014;         case EINVAL: return 10022;        case EMFILE: return 10024;
    case EWOULDBLOCK: return 10035;    case EINPROGRESS: return 10035;   // Windows reports a pending connect as WOULDBLOCK
    case EALREADY: return 10037;       case ENOTSOCK: return 10038;      case EDESTADDRREQ: return 10039;
    case EMSGSIZE: return 10040;       case EPROTOTYPE: return 10041;    case ENOPROTOOPT: return 10042;
    case EPROTONOSUPPORT: return 10043; case EOPNOTSUPP: return 10045;   case EAFNOSUPPORT: return 10047;
    case EADDRINUSE: return 10048;     case EADDRNOTAVAIL: return 10049; case ENETDOWN: return 10050;
    case ENETUNREACH: return 10051;    case ECONNABORTED: return 10053;  case ECONNRESET: return 10054;
    case ENOBUFS: return 10055;        case EISCONN: return 10056;       case ENOTCONN: return 10057;
    case ESHUTDOWN: return 10058;      case ETIMEDOUT: return 10060;     case ECONNREFUSED: return 10061;
    case EHOSTUNREACH: return 10065;   case EPIPE: return 10054;
    default: return 10022;
    }
}
// EOPNOTSUPP from a socket call is usually a mismatch in this layer (a flag or option meant for Windows), not the
// program's doing, and a program may retry it without end: the first few are logged with the call that got them.
static int wsa_error_from(const char *call, int e, int detail) {
    static int logged;
    if (e == EOPNOTSUPP && logged < 5) { logged++; vx_log("winsock: %s got EOPNOTSUPP (%#x)", call, detail); }
    return wsa_error(e);
}
static int fail_in(const char *call) { vx_set_last_error((DWORD)wsa_error_from(call, errno, 0)); return SOCKET_ERROR; }
#define fail() fail_in(__func__)
static int to_fd(SOCKET s) { return s > 0x7fffffff ? -1 : (int)s; }

// ---- addresses ------------------------------------------------------------------------------------------------------
static int family_in(int f) { return f == WIN_AF_INET6 ? AF_INET6 : f; }
static int family_out(int f) { return f == AF_INET6 ? WIN_AF_INET6 : f; }

// Windows sockaddr (16-bit family first) -> BSD sockaddr.
static socklen_t addr_in(const void *win, int len, struct sockaddr_storage *out) {
    memset(out, 0, sizeof *out);
    if (!win || len < 2) return 0;
    if ((size_t)len > sizeof *out) len = sizeof *out;
    memcpy(out, win, (size_t)len);
    int family = family_in(*(const uint16_t *)win);
    out->ss_family = (sa_family_t)family;
    out->ss_len = (uint8_t)(family == AF_INET6 ? sizeof(struct sockaddr_in6) : family == AF_INET ? sizeof(struct sockaddr_in) : len);
    return out->ss_len;
}
// BSD sockaddr -> Windows; *len in/out like Windows (the size needed when the buffer is too small).
static int addr_out(const struct sockaddr *a, socklen_t alen, void *win, int *len) {
    if (!win || !len) return 0;
    uint8_t buf[sizeof(struct sockaddr_storage)];
    memcpy(buf, a, alen);
    *(uint16_t *)buf = (uint16_t)family_out(a->sa_family);
    int need = (int)alen;
    if (*len < need) { *len = need; vx_set_last_error(10014); return -1; }
    memcpy(win, buf, (size_t)need);
    *len = need;
    return 0;
}

// ---- startup ------------------------------------------------------------------------------------------------------------
// WSADATA (x64): version, high version, max sockets, max UDP datagram, vendor info, description[257], status[129].
static WINAPI int WSAStartup(uint16_t version, uint8_t *data) {
    if (data) {
        memset(data, 0, 408);
        *(uint16_t *)data = version < 0x0202 ? version : 0x0202;
        *(uint16_t *)(data + 2) = 0x0202;
        snprintf((char *)data + 16, 257, "VXRuntime sockets (BSD)");
        snprintf((char *)data + 16 + 257, 129, "Running");
    }
    return 0;
}
static WINAPI int WSACleanup(void) { return 0; }
static WINAPI int WSAGetLastError(void) { return (int)vx_get_last_error(); }
static WINAPI void WSASetLastError(int e) { vx_set_last_error((DWORD)e); }

// ---- sockets -----------------------------------------------------------------------------------------------------------------
static WINAPI SOCKET ws_socket(int af, int type, int protocol) {
    int fd = socket(family_in(af), type, protocol);
    if (fd < 0) { fail(); return INVALID_SOCKET; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    return (SOCKET)fd;
}
static WINAPI SOCKET WSASocketA(int af, int type, int protocol, void *info, uint32_t group, DWORD flags) { (void)info; (void)group; (void)flags; return ws_socket(af, type, protocol); }
static WINAPI SOCKET WSASocketW(int af, int type, int protocol, void *info, uint32_t group, DWORD flags) { (void)info; (void)group; (void)flags; return ws_socket(af, type, protocol); }
static WINAPI int closesocket(SOCKET s) { return close(to_fd(s)) ? fail() : 0; }
static WINAPI int ws_bind(SOCKET s, const void *addr, int len) {
    struct sockaddr_storage a; socklen_t l = addr_in(addr, len, &a);
    return bind(to_fd(s), (struct sockaddr *)&a, l) ? fail() : 0;
}
static WINAPI int ws_connect(SOCKET s, const void *addr, int len) {
    struct sockaddr_storage a; socklen_t l = addr_in(addr, len, &a);
    return connect(to_fd(s), (struct sockaddr *)&a, l) ? fail() : 0;
}
static WINAPI int ws_listen(SOCKET s, int backlog) { return listen(to_fd(s), backlog) ? fail() : 0; }
static WINAPI SOCKET ws_accept(SOCKET s, void *addr, int *len) {
    struct sockaddr_storage a; socklen_t l = sizeof a;
    int fd = accept(to_fd(s), (struct sockaddr *)&a, &l);
    if (fd < 0) { fail(); return INVALID_SOCKET; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    if (addr && len) addr_out((struct sockaddr *)&a, l, addr, len);
    return (SOCKET)fd;
}
static int flags_in(int f) { return (f & ~8) | ((f & 8) ? MSG_WAITALL : 0); }   // MSG_WAITALL is 8 on Windows
static WINAPI int ws_send(SOCKET s, const char *buf, int len, int flags) {
    ssize_t n = send(to_fd(s), buf, (size_t)len, flags_in(flags));
    return n < 0 ? fail() : (int)n;
}
static WINAPI int ws_recv(SOCKET s, char *buf, int len, int flags) {
    ssize_t n = recv(to_fd(s), buf, (size_t)len, flags_in(flags));
    return n < 0 ? fail() : (int)n;
}
static WINAPI int ws_sendto(SOCKET s, const char *buf, int len, int flags, const void *to, int tolen) {
    struct sockaddr_storage a; socklen_t l = addr_in(to, tolen, &a);
    ssize_t n = sendto(to_fd(s), buf, (size_t)len, flags_in(flags), to ? (struct sockaddr *)&a : NULL, to ? l : 0);
    return n < 0 ? fail() : (int)n;
}
static WINAPI int ws_recvfrom(SOCKET s, char *buf, int len, int flags, void *from, int *fromlen) {
    struct sockaddr_storage a; socklen_t l = sizeof a;
    ssize_t n = recvfrom(to_fd(s), buf, (size_t)len, flags_in(flags), (struct sockaddr *)&a, &l);
    if (n < 0) return fail();
    if (from && fromlen) addr_out((struct sockaddr *)&a, l, from, fromlen);
    return (int)n;
}
static WINAPI int ws_shutdown(SOCKET s, int how) { return shutdown(to_fd(s), how) ? fail() : 0; }   // SD_* = SHUT_*
static WINAPI int ws_getsockname(SOCKET s, void *addr, int *len) {
    struct sockaddr_storage a; socklen_t l = sizeof a;
    if (getsockname(to_fd(s), (struct sockaddr *)&a, &l)) return fail();
    return addr_out((struct sockaddr *)&a, l, addr, len) ? SOCKET_ERROR : 0;
}
static WINAPI int ws_getpeername(SOCKET s, void *addr, int *len) {
    struct sockaddr_storage a; socklen_t l = sizeof a;
    if (getpeername(to_fd(s), (struct sockaddr *)&a, &l)) return fail();
    return addr_out((struct sockaddr *)&a, l, addr, len) ? SOCKET_ERROR : 0;
}

// WSABUF arrays (length, pointer). Sends complete at once; an overlapped receive with nothing to read yet waits in
// a background thread and completes the OVERLAPPED (its event, the socket's completion port) when data comes.
// Completion routines are not supported.
// A program answered WSAEOPNOTSUPP usually retries: the first few answers from each place are logged so the loop can be found.
static void not_supported(const char *what, unsigned detail) {
    static int logged[8]; int slot = (int)(((uintptr_t)what >> 4) & 7);
    if (logged[slot] < 3) { logged[slot]++; vx_log("winsock: %s is not supported (%#x)", what, detail); }
    vx_set_last_error(10045);
}
typedef struct { uint32_t len; char *buf; } WSABUF;
#define WSA_IO_PENDING 997
static int load_iov(struct iovec *iov, const WSABUF *bufs, DWORD n) {
    if (n > 64) n = 64;
    for (DWORD i = 0; i < n; i++) iov[i] = (struct iovec){ bufs[i].buf, bufs[i].len };
    return (int)n;
}
// Completion routines: a WSARecv/WSASend given one is told its result by a call on the issuing thread's next
// alertable wait (SleepEx, WSAWaitForMultipleEvents with fAlertable, WaitFor*Ex), as Windows does with an APC; the
// OVERLAPPED's hEvent is then the program's own field and is left alone.
typedef struct { void (WINAPI *routine)(DWORD, DWORD, void *, DWORD); DWORD error, bytes; void *ov; } completion_call;
static WINAPI void completion_apc(uintptr_t data) {
    completion_call *c = (completion_call *)data;
    c->routine(c->error, c->bytes, c->ov, 0);
    free(c);
}
static HANDLE issuing_thread(void) {                               // (a real handle: the pseudo handle means the caller)
    static HANDLE (WINAPI *open_thread)(DWORD, BOOL, DWORD);
    static DWORD (WINAPI *current_tid)(void);
    if (!open_thread) {
        open_thread = (HANDLE (WINAPI *)(DWORD, BOOL, DWORD))vx_find_export("kernel32.dll", "OpenThread");
        current_tid = (DWORD (WINAPI *)(void))vx_find_export("kernel32.dll", "GetCurrentThreadId");
    }
    return open_thread(0x1FFFFF, 0, current_tid());
}
static void complete_with_routine(HANDLE thread, void *cr, void *ov, DWORD bytes, DWORD error) {
    static DWORD (WINAPI *queue)(void (WINAPI *)(uintptr_t), HANDLE, uintptr_t);
    static BOOL (WINAPI *close)(HANDLE);
    if (!queue) {
        queue = (DWORD (WINAPI *)(void (WINAPI *)(uintptr_t), HANDLE, uintptr_t))vx_find_export("kernel32.dll", "QueueUserAPC");
        close = (BOOL (WINAPI *)(HANDLE))vx_find_export("kernel32.dll", "CloseHandle");
    }
    uint64_t *o = ov;
    if (o) { o[0] = error; o[1] = bytes; }                         // Internal, InternalHigh as WSAGetOverlappedResult reads them
    completion_call *c = malloc(sizeof *c);
    *c = (completion_call){ (void (WINAPI *)(DWORD, DWORD, void *, DWORD))cr, error, bytes, ov };
    if (!queue(completion_apc, thread, (uintptr_t)c)) free(c);
    close(thread);
}
static int send_msg(SOCKET s, const WSABUF *bufs, DWORD n, DWORD *sent, DWORD flags, const void *to, int tolen, void *ov, void *cr) {
    struct iovec iov[64];
    struct sockaddr_storage a; socklen_t l = to ? addr_in(to, tolen, &a) : 0;
    struct msghdr m = { to ? &a : NULL, l, iov, load_iov(iov, bufs, n), NULL, 0, 0 };
    ssize_t r = sendmsg(to_fd(s), &m, flags_in((int)flags));
    if (r < 0) { int e = fail(); if (ov && !cr) vx_io_completed((HANDLE)s, ov, 0, vx_get_last_error()); return e; }
    if (sent) *sent = (DWORD)r;
    if (cr) complete_with_routine(issuing_thread(), cr, ov, (DWORD)r, 0);
    else if (ov) vx_io_completed((HANDLE)s, ov, (DWORD)r, 0);
    return 0;
}
typedef struct { SOCKET s; struct iovec iov[64]; int n; void *ov; void *from; int *fromlen; int flags; void *cr; HANDLE thread; } pending_recv;
static ssize_t recv_now(SOCKET s, struct iovec *iov, int n, int flags, void *from, int *fromlen) {
    struct sockaddr_storage a; socklen_t l = sizeof a;
    struct msghdr m = { from ? &a : NULL, from ? l : 0, iov, n, NULL, 0, 0 };
    ssize_t r = recvmsg(to_fd(s), &m, flags);
    if (r >= 0 && from && fromlen) addr_out((struct sockaddr *)&a, m.msg_namelen, from, fromlen);
    return r;
}
static void *recv_in_background(void *arg) {
    pending_recv *p = arg;
    struct pollfd w = { to_fd(p->s), POLLIN, 0 };
    ssize_t r = -1;
    DWORD err = 995;                                               // ERROR_OPERATION_ABORTED: the socket was closed
    for (;;) {
        int k = poll(&w, 1, 1000);
        if (k < 0 && errno == EINTR) continue;
        if (k < 0 || (w.revents & POLLNVAL)) break;
        if (k == 0) { if (fcntl(w.fd, F_GETFD) < 0) break; continue; }
        r = recv_now(p->s, p->iov, p->n, p->flags | MSG_DONTWAIT, p->from, p->fromlen);
        if (r < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        err = r < 0 ? (DWORD)wsa_error_from("an overlapped receive", errno, p->flags) : 0;
        break;
    }
    if (p->cr) complete_with_routine(p->thread, p->cr, p->ov, r > 0 ? (DWORD)r : 0, err);
    else vx_io_completed((HANDLE)p->s, p->ov, r > 0 ? (DWORD)r : 0, err);
    free(p);
    return NULL;
}
static int recv_msg(SOCKET s, WSABUF *bufs, DWORD n, DWORD *got, DWORD *flags, void *from, int *fromlen, void *ov, void *cr) {
    struct iovec iov[64];
    int count = load_iov(iov, bufs, n), f = flags ? flags_in((int)*flags) : 0;
    if (!ov) {
        ssize_t r = recv_now(s, iov, count, f, from, fromlen);
        if (r < 0) return fail();
        if (got) *got = (DWORD)r;
        if (flags) *flags = 0;
        return 0;
    }
    ssize_t r = recv_now(s, iov, count, f | MSG_DONTWAIT, from, fromlen);
    if (r >= 0) {
        if (got) *got = (DWORD)r;
        if (flags) *flags = 0;
        if (cr) complete_with_routine(issuing_thread(), cr, ov, (DWORD)r, 0);
        else vx_io_completed((HANDLE)s, ov, (DWORD)r, 0);
        return 0;
    }
    if (errno != EAGAIN) { int e = fail(); if (!cr) vx_io_completed((HANDLE)s, ov, 0, vx_get_last_error()); return e; }
    pending_recv *p = calloc(1, sizeof *p);
    p->s = s; memcpy(p->iov, iov, sizeof iov); p->n = count; p->ov = ov; p->from = from; p->fromlen = fromlen; p->flags = f;
    p->cr = cr; p->thread = cr ? issuing_thread() : NULL;
    *(uint64_t *)ov = 0x103;                                       // STATUS_PENDING; the event is reset as the I/O starts
    HANDLE event = cr ? NULL : (HANDLE)((uintptr_t)*(HANDLE *)((uint8_t *)ov + 24) & ~(uintptr_t)1);
    if (event) {
        BOOL (WINAPI *reset)(HANDLE) = (BOOL (WINAPI *)(HANDLE))vx_find_export("kernel32.dll", "ResetEvent");
        reset(event);
    }
    pthread_t t; pthread_attr_t at;
    pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED); pthread_attr_setstacksize(&at, 128 << 10);
    pthread_create(&t, &at, recv_in_background, p);
    pthread_attr_destroy(&at);
    vx_set_last_error(WSA_IO_PENDING);
    return SOCKET_ERROR;
}
static WINAPI int WSASend(SOCKET s, WSABUF *bufs, DWORD n, DWORD *sent, DWORD flags, void *ov, void *cr) {
    return send_msg(s, bufs, n, sent, flags, NULL, 0, ov, cr);
}
static WINAPI int WSASendTo(SOCKET s, WSABUF *bufs, DWORD n, DWORD *sent, DWORD flags, const void *to, int tolen, void *ov, void *cr) {
    return send_msg(s, bufs, n, sent, flags, to, tolen, ov, cr);
}
static WINAPI int WSARecv(SOCKET s, WSABUF *bufs, DWORD n, DWORD *got, DWORD *flags, void *ov, void *cr) {
    return recv_msg(s, bufs, n, got, flags, NULL, NULL, ov, cr);
}
static WINAPI int WSARecvFrom(SOCKET s, WSABUF *bufs, DWORD n, DWORD *got, DWORD *flags, void *from, int *fromlen, void *ov, void *cr) {
    return recv_msg(s, bufs, n, got, flags, from, fromlen, ov, cr);
}
// AcceptEx: a background thread accepts the next connection and puts it under the accept socket's number (dup2);
// the local and remote addresses go into the buffer after the first data bytes, one slot each (local_len,
// remote_len), where GetAcceptExSockaddrs (wsock32 #1142) finds them.
typedef struct { SOCKET listen, accept; uint8_t *buf; DWORD data, local_len, remote_len; void *ov; } pending_accept;
static void *accept_in_background(void *arg) {
    pending_accept *p = arg;
    int fd;
    for (;;) {                                                     // a non-blocking listener: wait until a client comes
        fd = accept(to_fd(p->listen), NULL, NULL);
        if (fd >= 0 || (errno != EINTR && errno != EAGAIN)) break;
        struct pollfd w = { to_fd(p->listen), POLLIN, 0 };
        if (poll(&w, 1, 1000) < 0 && errno != EINTR) break;
        if (w.revents & POLLNVAL) { errno = EBADF; break; }
    }
    DWORD got = 0, err = 0;
    if (fd < 0) err = (DWORD)wsa_error(errno);
    else {
        dup2(fd, to_fd(p->accept));
        close(fd);
        fd = to_fd(p->accept);
        int fl = fcntl(fd, F_GETFL); fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
        if (p->data) { ssize_t n = recv(fd, p->buf, p->data, 0); got = n > 0 ? (DWORD)n : 0; }
        struct sockaddr_storage a; socklen_t l = sizeof a; int len;
        if (!getsockname(fd, (struct sockaddr *)&a, &l)) { len = (int)p->local_len; addr_out((struct sockaddr *)&a, l, p->buf + p->data, &len); }
        l = sizeof a;
        if (!getpeername(fd, (struct sockaddr *)&a, &l)) { len = (int)p->remote_len; addr_out((struct sockaddr *)&a, l, p->buf + p->data + p->local_len, &len); }
    }
    vx_io_completed((HANDLE)p->listen, p->ov, got, err);
    free(p);
    return NULL;
}
static WINAPI BOOL AcceptEx(SOCKET listen_s, SOCKET accept_s, void *buf, DWORD data, DWORD local_len, DWORD remote_len, DWORD *received, void *ov) {
    if (!ov) { vx_set_last_error(87); return 0; }
    pending_accept *p = calloc(1, sizeof *p);
    *p = (pending_accept){ listen_s, accept_s, buf, data, local_len, remote_len, ov };
    if (received) *received = 0;
    *(uint64_t *)ov = 0x103;
    HANDLE event = (HANDLE)((uintptr_t)*(HANDLE *)((uint8_t *)ov + 24) & ~(uintptr_t)1);
    if (event) {
        BOOL (WINAPI *reset)(HANDLE) = (BOOL (WINAPI *)(HANDLE))vx_find_export("kernel32.dll", "ResetEvent");
        reset(event);
    }
    pthread_t t; pthread_attr_t at;
    pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED); pthread_attr_setstacksize(&at, 128 << 10);
    pthread_create(&t, &at, accept_in_background, p);
    pthread_attr_destroy(&at);
    vx_set_last_error(WSA_IO_PENDING);
    return 0;
}
static WINAPI void GetAcceptExSockaddrs(void *buf, DWORD data, DWORD local_len, DWORD remote_len, void **local, int *llen, void **remote, int *rlen) {
    uint8_t *b = buf;
    int lf = ((uint16_t *)(b + data))[0], rf = ((uint16_t *)(b + data + local_len))[0];
    if (local) *local = b + data;
    if (llen) *llen = lf == WIN_AF_INET6 ? 28 : 16;
    if (remote) *remote = b + data + local_len;
    if (rlen) *rlen = rf == WIN_AF_INET6 ? 28 : 16;
    (void)remote_len;
}

// ---- options and modes -------------------------------------------------------------------------------------------------------
#define WIN_SOL_SOCKET 0xffff
static WINAPI int ws_setsockopt(SOCKET s, int level, int opt, const char *val, int len) {
    int fd = to_fd(s);
    if (level == WIN_SOL_SOCKET) {
        if (opt == 0x1005 || opt == 0x1006) {                    // SO_SNDTIMEO / SO_RCVTIMEO: a DWORD of milliseconds
            DWORD ms = len >= 4 ? *(const DWORD *)val : 0;
            struct timeval tv = { ms / 1000, (int)(ms % 1000) * 1000 };
            return setsockopt(fd, SOL_SOCKET, opt == 0x1005 ? SO_SNDTIMEO : SO_RCVTIMEO, &tv, sizeof tv) ? fail() : 0;
        }
        if (opt == 0x80) {                                        // SO_LINGER: two u_shorts
            struct linger l = { ((const uint16_t *)val)[0], ((const uint16_t *)val)[1] };
            return setsockopt(fd, SOL_SOCKET, SO_LINGER, &l, sizeof l) ? fail() : 0;
        }
        if (opt == (int)~0x4u) return 0;                          // SO_EXCLUSIVEADDRUSE: the default on macOS
        if (opt == 0x7010) return 0;                              // SO_UPDATE_ACCEPT_CONTEXT
        int v = len >= 4 ? *(const int *)val : len >= 1 ? *(const uint8_t *)val : 0;
        if (opt == SO_SNDBUF || opt == SO_RCVBUF) {               // macOS caps buffers (kern.ipc.maxsockbuf): take the largest it allows
            if (v <= 0) return 0;                                 // 0 on Windows: send from the program's buffers; macOS keeps its own
            int asked = v;
            while (setsockopt(fd, SOL_SOCKET, opt, &v, sizeof v)) {
                if (errno != ENOBUFS && errno != EINVAL) return fail();
                if ((v /= 2) < 4096) return fail();
            }
            if (vx_trace && v != asked) vx_log("socket buffer %d asked, %d given", asked, v);
            return 0;
        }
        return setsockopt(fd, SOL_SOCKET, opt, &v, sizeof v) ? fail() : 0;   // the other numbers match
    }
    int v = len >= 4 ? *(const int *)val : len >= 1 ? *(const uint8_t *)val : 0;
    if (level == IPPROTO_IP && (opt == 12 || opt == 13)) return setsockopt(fd, level, opt, val, (socklen_t)len) ? fail() : 0;   // ip_mreq
    if (level == IPPROTO_IPV6 && opt == 27) return setsockopt(fd, level, IPV6_V6ONLY, &v, sizeof v) ? fail() : 0;
    return setsockopt(fd, level, opt, &v, sizeof v) ? fail() : 0;
}
static WINAPI int ws_getsockopt(SOCKET s, int level, int opt, char *val, int *len) {
    int fd = to_fd(s);
    if (level == WIN_SOL_SOCKET) level = SOL_SOCKET;
    if (level == SOL_SOCKET && (opt == 0x1005 || opt == 0x1006)) {
        struct timeval tv; socklen_t l = sizeof tv;
        if (getsockopt(fd, SOL_SOCKET, opt == 0x1005 ? SO_SNDTIMEO : SO_RCVTIMEO, &tv, &l)) return fail();
        *(DWORD *)val = (DWORD)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
        *len = 4;
        return 0;
    }
    int v = 0; socklen_t l = sizeof v;
    if (getsockopt(fd, level, opt, &v, &l)) return fail();
    if (level == SOL_SOCKET && opt == SO_ERROR && v) v = wsa_error(v);
    if (*len >= 4) { *(int *)val = v; *len = 4; } else if (*len >= 1) { *val = (char)v; *len = 1; }
    return 0;
}
static WINAPI int ioctlsocket(SOCKET s, LONG cmd, uint32_t *arg) {
    int fd = to_fd(s);
    if ((uint32_t)cmd == 0x8004667eu) {                           // FIONBIO
        int fl = fcntl(fd, F_GETFL);
        return fcntl(fd, F_SETFL, *arg ? fl | O_NONBLOCK : fl & ~O_NONBLOCK) ? fail() : 0;
    }
    if ((uint32_t)cmd == 0x4004667fu) {                           // FIONREAD
        int n = 0;
        if (ioctl(fd, FIONREAD, &n)) return fail();
        *arg = (uint32_t)n;
        return 0;
    }
    not_supported("this ioctlsocket command", (unsigned)cmd);
    return SOCKET_ERROR;
}
// Extension functions, by GUID: ConnectEx (a connection that completes like overlapped I/O) and DisconnectEx.
typedef struct { SOCKET s; struct sockaddr_storage a; socklen_t l; const char *send; DWORD send_len; void *ov; } pending_connect;
static void *connect_in_background(void *arg) {
    pending_connect *p = arg;
    int fd = to_fd(p->s), r, err = 0;
    do r = connect(fd, (struct sockaddr *)&p->a, p->l); while (r < 0 && errno == EINTR);
    if (r < 0 && (errno == EINPROGRESS || errno == EALREADY)) {    // a non-blocking socket: wait until it is writable
        struct pollfd w = { fd, POLLOUT, 0 };
        while (poll(&w, 1, -1) < 0 && errno == EINTR) {}
        socklen_t el = sizeof err;
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
    } else if (r < 0) err = errno;
    DWORD sent = 0;
    if (!err && p->send && p->send_len) {
        ssize_t n = send(fd, p->send, p->send_len, 0);
        if (n < 0) err = errno; else sent = (DWORD)n;
    }
    vx_io_completed((HANDLE)p->s, p->ov, sent, err ? (DWORD)wsa_error(err) : 0);
    free(p);
    return NULL;
}
static WINAPI BOOL ConnectEx(SOCKET s, const void *name, int len, const char *send, DWORD send_len, DWORD *sent, void *ov) {
    if (!ov) { vx_set_last_error(87); return 0; }
    pending_connect *p = calloc(1, sizeof *p);
    p->s = s; p->l = addr_in(name, len, &p->a); p->send = send; p->send_len = send_len; p->ov = ov;
    if (sent) *sent = 0;
    *(uint64_t *)ov = 0x103;
    HANDLE event = (HANDLE)((uintptr_t)*(HANDLE *)((uint8_t *)ov + 24) & ~(uintptr_t)1);
    if (event) {
        BOOL (WINAPI *reset)(HANDLE) = (BOOL (WINAPI *)(HANDLE))vx_find_export("kernel32.dll", "ResetEvent");
        reset(event);
    }
    pthread_t t; pthread_attr_t at;
    pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED); pthread_attr_setstacksize(&at, 128 << 10);
    pthread_create(&t, &at, connect_in_background, p);
    pthread_attr_destroy(&at);
    vx_set_last_error(WSA_IO_PENDING);
    return 0;
}
static WINAPI BOOL DisconnectEx(SOCKET s, void *ov, DWORD flags, DWORD reserved) {
    (void)flags; (void)reserved;
    shutdown(to_fd(s), SHUT_RDWR);
    if (ov) vx_io_completed((HANDLE)s, ov, 0, 0);
    return 1;
}
static void *extension_function(const uint8_t *guid) {
    uint32_t d1; memcpy(&d1, guid, 4);
    if (d1 == 0x25a207b9) return (void *)ConnectEx;
    if (d1 == 0x7fda2e11) return (void *)DisconnectEx;
    if (d1 == 0xb5367df2) return (void *)GetAcceptExSockaddrs;
    if (d1 == 0xb5367df1) return (void *)AcceptEx;
    return NULL;
}
#define SIO_GET_EXTENSION_FUNCTION_POINTER 0xC8000006u
#define SIO_UDP_CONNRESET 0x9800000Cu
#define SIO_KEEPALIVE_VALS 0x98000004u
static WINAPI int WSAIoctl(SOCKET s, DWORD code, void *in, DWORD in_len, void *out, DWORD out_len, DWORD *got, void *ov, void *cr) {
    (void)in_len; (void)out_len; (void)cr;
    if (got) *got = 0;
    int r = SOCKET_ERROR;
    if (code == 0x8004667eu || code == 0x4004667fu) {                // FIONBIO, FIONREAD
        r = ioctlsocket(s, (LONG)code, code == 0x8004667eu ? (uint32_t *)in : (uint32_t *)out);
        if (!r && code == 0x4004667fu && got) *got = 4;
    } else if (code == SIO_GET_EXTENSION_FUNCTION_POINTER && in && in_len >= 16 && out && out_len >= 8 && extension_function(in)) {
        *(void **)out = extension_function(in);
        if (got) *got = 8;
        r = 0;
    } else if (code == SIO_UDP_CONNRESET) r = 0;                       // macOS does not report ICMP resets on UDP reads
    else if (code == SIO_KEEPALIVE_VALS) {
        int on = in ? *(uint32_t *)in != 0 : 1;
        setsockopt(to_fd(s), SOL_SOCKET, SO_KEEPALIVE, &on, sizeof on);   // best effort (not for every socket type)
        r = 0;
    } else {
        not_supported("this WSAIoctl code", (unsigned)code);
    }
    if (!r && ov) vx_io_completed((HANDLE)s, ov, 0, 0);
    return r;
}

// ---- WSAEventSelect: network events as event objects ---------------------------------------------------------------------
// One thread polls the registered sockets every few milliseconds and sets their events; FD_WRITE is reported when a
// socket becomes writable (not while it stays so), the others while their condition holds.
#define FD_READ_BIT 1
#define FD_WRITE_BIT 2
#define FD_ACCEPT_BIT 8
#define FD_CONNECT_BIT 16
#define FD_CLOSE_BIT 32
typedef struct { SOCKET s; HANDLE event; LONG mask, pending; int writable, closed; } event_select;
static event_select g_selects[256];
static int g_select_count;
static pthread_mutex_t g_select_lock = PTHREAD_MUTEX_INITIALIZER;
static BOOL (WINAPI *g_set_event)(HANDLE), (WINAPI *g_reset_event)(HANDLE);
static void *select_poller(void *arg) {
    (void)arg;
    for (;;) {
        struct pollfd fds[256]; int n;
        pthread_mutex_lock(&g_select_lock);
        n = g_select_count;
        for (int i = 0; i < n; i++) {
            fds[i].fd = to_fd(g_selects[i].s);
            fds[i].events = (short)(((g_selects[i].mask & (FD_READ_BIT | FD_ACCEPT_BIT | FD_CLOSE_BIT)) ? POLLIN : 0) |
                                    ((g_selects[i].mask & (FD_WRITE_BIT | FD_CONNECT_BIT)) ? POLLOUT : 0));
            fds[i].revents = 0;
        }
        pthread_mutex_unlock(&g_select_lock);
        if (n) poll(fds, (nfds_t)n, 0);
        pthread_mutex_lock(&g_select_lock);
        for (int i = 0; i < n && i < g_select_count; i++) {
            event_select *e = &g_selects[i];
            if (to_fd(e->s) != fds[i].fd) continue;
            LONG add = 0;
            if (fds[i].revents & (POLLIN | POLLHUP)) {
                char c;
                ssize_t r = (e->mask & FD_ACCEPT_BIT) ? 1 : recv(fds[i].fd, &c, 1, MSG_PEEK | MSG_DONTWAIT);
                if (r == 0 || (fds[i].revents & POLLHUP)) { if (!e->closed) add |= FD_CLOSE_BIT; e->closed = 1; }
                else if (r > 0) add |= e->mask & (FD_READ_BIT | FD_ACCEPT_BIT);
            }
            int w = (fds[i].revents & POLLOUT) != 0;
            if (w && !e->writable) add |= FD_WRITE_BIT | FD_CONNECT_BIT;
            e->writable = w;
            add &= e->mask;
            if (add & ~e->pending) { e->pending |= add; g_set_event(e->event); }
        }
        pthread_mutex_unlock(&g_select_lock);
        usleep(2000);
    }
    return NULL;
}
static WINAPI int WSAEventSelect(SOCKET s, HANDLE event, LONG mask) {
    if (!g_set_event) {
        g_set_event = (BOOL (WINAPI *)(HANDLE))vx_find_export("kernel32.dll", "SetEvent");
        g_reset_event = (BOOL (WINAPI *)(HANDLE))vx_find_export("kernel32.dll", "ResetEvent");
    }
    int fd = to_fd(s);
    int fl = fcntl(fd, F_GETFL); fcntl(fd, F_SETFL, fl | O_NONBLOCK);      // as on Windows
    pthread_mutex_lock(&g_select_lock);
    int i = 0;
    while (i < g_select_count && g_selects[i].s != s) i++;
    if (!event || !mask) { if (i < g_select_count) g_selects[i] = g_selects[--g_select_count]; }
    else if (i < 256) {
        if (i == g_select_count) g_select_count++;
        g_selects[i] = (event_select){ s, event, mask, 0, 0, 0 };
    }
    pthread_mutex_unlock(&g_select_lock);
    static int started;
    if (!__sync_lock_test_and_set(&started, 1)) { pthread_t t; pthread_create(&t, NULL, select_poller, NULL); pthread_detach(t); }
    return 0;
}
typedef struct { LONG events; int errors[10]; } WSANETWORKEVENTS;
static WINAPI int WSAEnumNetworkEvents(SOCKET s, HANDLE event, WSANETWORKEVENTS *out) {
    memset(out, 0, sizeof *out);
    pthread_mutex_lock(&g_select_lock);
    for (int i = 0; i < g_select_count; i++) if (g_selects[i].s == s) { out->events = g_selects[i].pending; g_selects[i].pending = 0; break; }
    pthread_mutex_unlock(&g_select_lock);
    if (event && g_reset_event) g_reset_event(event);
    return 0;
}
static WINAPI HANDLE WSACreateEvent(void) {
    HANDLE (WINAPI *create)(void *, BOOL, BOOL, const char *) = (HANDLE (WINAPI *)(void *, BOOL, BOOL, const char *))vx_find_export("kernel32.dll", "CreateEventA");
    return create(NULL, 1, 0, NULL);                              // manual reset, as WSA events are
}
static WINAPI BOOL WSACloseEvent(HANDLE e) { BOOL (WINAPI *f)(HANDLE) = (BOOL (WINAPI *)(HANDLE))vx_find_export("kernel32.dll", "CloseHandle"); return f(e); }
static WINAPI BOOL WSASetEvent(HANDLE e) { BOOL (WINAPI *f)(HANDLE) = (BOOL (WINAPI *)(HANDLE))vx_find_export("kernel32.dll", "SetEvent"); return f(e); }
static WINAPI BOOL WSAResetEvent(HANDLE e) { BOOL (WINAPI *f)(HANDLE) = (BOOL (WINAPI *)(HANDLE))vx_find_export("kernel32.dll", "ResetEvent"); return f(e); }
static WINAPI DWORD WSAWaitForMultipleEvents(DWORD n, const HANDLE *events, BOOL all, DWORD ms, BOOL alertable) {
    static DWORD (WINAPI *wait)(DWORD, const HANDLE *, BOOL, DWORD, BOOL);
    if (!wait) wait = (DWORD (WINAPI *)(DWORD, const HANDLE *, BOOL, DWORD, BOOL))vx_find_export("kernel32.dll", "WaitForMultipleObjectsEx");
    return wait(n, events, all, ms, alertable);                   // WSA_WAIT_EVENT_0 / _TIMEOUT / _IO_COMPLETION match
}

// fd_set: a count, then up to 64 sockets.
typedef struct { uint32_t count; SOCKET fds[64]; } WIN_FD_SET;
static void set_in(const WIN_FD_SET *w, fd_set *f, int *maxfd) {
    FD_ZERO(f);
    if (!w) return;
    for (uint32_t i = 0; i < w->count && i < 64; i++) {
        int fd = to_fd(w->fds[i]);
        if (fd >= 0 && fd < FD_SETSIZE) { FD_SET(fd, f); if (fd > *maxfd) *maxfd = fd; }
    }
}
static void set_out(WIN_FD_SET *w, const fd_set *f) {
    if (!w) return;
    uint32_t k = 0;
    for (uint32_t i = 0; i < w->count && i < 64; i++) { int fd = to_fd(w->fds[i]); if (fd >= 0 && FD_ISSET(fd, f)) w->fds[k++] = w->fds[i]; }
    w->count = k;
}
static WINAPI int ws_select(int nfds, WIN_FD_SET *r, WIN_FD_SET *w, WIN_FD_SET *e, const int32_t *timeout) {
    (void)nfds;
    fd_set rf, wf, ef; int maxfd = -1;
    set_in(r, &rf, &maxfd); set_in(w, &wf, &maxfd); set_in(e, &ef, &maxfd);
    struct timeval tv, *tvp = NULL;
    if (timeout) { tv.tv_sec = timeout[0]; tv.tv_usec = timeout[1]; tvp = &tv; }      // Windows timeval: two 32-bit longs
    int n = select(maxfd + 1, &rf, &wf, &ef, tvp);
    if (n < 0) return fail();
    set_out(r, &rf); set_out(w, &wf); set_out(e, &ef);
    return n;
}
static WINAPI int __WSAFDIsSet(SOCKET s, WIN_FD_SET *set) {
    for (uint32_t i = 0; i < set->count && i < 64; i++) if (set->fds[i] == s) return 1;
    return 0;
}

// ---- names and byte order ---------------------------------------------------------------------------------------------------
static WINAPI uint16_t ws_htons(uint16_t v) { return htons(v); }
static WINAPI uint16_t ws_ntohs(uint16_t v) { return ntohs(v); }
static WINAPI uint32_t ws_htonl(uint32_t v) { return htonl(v); }
static WINAPI uint32_t ws_ntohl(uint32_t v) { return ntohl(v); }
static WINAPI uint32_t ws_inet_addr(const char *s) { return inet_addr(s); }
static WINAPI char *ws_inet_ntoa(uint32_t a) { struct in_addr in = { a }; static __thread char buf[16]; snprintf(buf, sizeof buf, "%s", inet_ntoa(in)); return buf; }
static WINAPI int ws_inet_pton(int af, const char *src, void *dst) { return inet_pton(family_in(af), src, dst); }
static WINAPI const char *ws_inet_ntop(int af, const void *src, char *dst, size_t len) { return inet_ntop(family_in(af), src, dst, (socklen_t)len); }
static WINAPI int ws_gethostname(char *name, int len) { return gethostname(name, (size_t)len) ? fail() : 0; }

// hostent (x64): name, aliases, address type (short), length (short), address list. One per thread, like Windows.
typedef struct { char *name; char **aliases; int16_t addrtype, length; char **addr_list; } WIN_HOSTENT;
static __thread WIN_HOSTENT t_host;
static __thread char *t_host_addrs[17], t_host_data[16][16], t_host_name[256], *t_host_alias[1];
static WINAPI WIN_HOSTENT *ws_gethostbyname(const char *name) {
    struct addrinfo hints = { 0 }, *res = NULL;
    hints.ai_family = AF_INET;
    if (getaddrinfo(name, NULL, &hints, &res) || !res) { vx_set_last_error(11001 /* WSAHOST_NOT_FOUND */); return NULL; }
    int n = 0;
    for (struct addrinfo *a = res; a && n < 16; a = a->ai_next) {
        memcpy(t_host_data[n], &((struct sockaddr_in *)a->ai_addr)->sin_addr, 4);
        t_host_addrs[n] = t_host_data[n]; n++;
    }
    t_host_addrs[n] = NULL;
    snprintf(t_host_name, sizeof t_host_name, "%s", res->ai_canonname ? res->ai_canonname : name);
    freeaddrinfo(res);
    t_host_alias[0] = NULL;
    t_host = (WIN_HOSTENT){ t_host_name, t_host_alias, 2, 4, t_host_addrs };
    return &t_host;
}

// addrinfo has the same field order on both systems, but the address inside it differs: lists are rebuilt.
typedef struct win_addrinfo { int flags, family, socktype, protocol; size_t addrlen; char *canonname; void *addr; struct win_addrinfo *next; } WIN_ADDRINFO;
static WIN_ADDRINFO *convert_list(struct addrinfo *res) {
    WIN_ADDRINFO *head = NULL, **tail = &head;
    for (struct addrinfo *a = res; a; a = a->ai_next) {
        WIN_ADDRINFO *w = calloc(1, sizeof *w + sizeof(struct sockaddr_storage));
        w->flags = a->ai_flags; w->family = family_out(a->ai_family); w->socktype = a->ai_socktype; w->protocol = a->ai_protocol;
        w->addr = w + 1;
        int len = sizeof(struct sockaddr_storage);
        addr_out(a->ai_addr, a->ai_addrlen, w->addr, &len);
        w->addrlen = (size_t)len;
        w->canonname = a->ai_canonname ? strdup(a->ai_canonname) : NULL;
        *tail = w; tail = &w->next;
    }
    return head;
}
static WINAPI int ws_getaddrinfo(const char *node, const char *service, const WIN_ADDRINFO *hints, WIN_ADDRINFO **out) {
    struct addrinfo h = { 0 }, *res = NULL;
    if (hints) { h.ai_flags = hints->flags & (AI_PASSIVE | AI_CANONNAME | AI_NUMERICHOST | AI_ADDRCONFIG); h.ai_family = family_in(hints->family); h.ai_socktype = hints->socktype; h.ai_protocol = hints->protocol; }
    int r = getaddrinfo(node, service, &h, &res);
    if (r) {
        int e = r == EAI_NONAME ? 11001 : r == EAI_AGAIN ? 11002 : r == EAI_FAMILY ? 10047 : r == EAI_SERVICE ? 10109 : 11003;
        vx_set_last_error((DWORD)e);
        return e;
    }
    *out = convert_list(res);
    freeaddrinfo(res);
    return 0;
}
static WINAPI void ws_freeaddrinfo(WIN_ADDRINFO *a) {
    while (a) { WIN_ADDRINFO *n = a->next; free(a->canonname); free(a); a = n; }
}
static WINAPI int GetAddrInfoW(const WCHAR *node, const WCHAR *service, const WIN_ADDRINFO *hints, WIN_ADDRINFO **out) {
    char *n = vx_utf8_dup(node), *s = vx_utf8_dup(service);
    int r = ws_getaddrinfo(n, s, hints, out);   // the W form's canonical names stay UTF-8 (rarely read)
    free(n); free(s);
    return r;
}
static WINAPI int ws_getnameinfo(const void *addr, int len, char *host, DWORD hlen, char *serv, DWORD slen, int flags) {
    struct sockaddr_storage a; socklen_t l = addr_in(addr, len, &a);
    int r = getnameinfo((struct sockaddr *)&a, l, host, hlen, serv, slen, flags);
    return r ? 11001 : 0;
}

#define NET(name, fn) { name, (void *)fn }
static const vx_export ws2[] = {
    VX_EXPORT(WSAStartup), VX_EXPORT(WSACleanup), VX_EXPORT(WSAGetLastError), VX_EXPORT(WSASetLastError),
    NET("socket", ws_socket), VX_EXPORT(WSASocketA), VX_EXPORT(WSASocketW), VX_EXPORT(closesocket), NET("bind", ws_bind),
    NET("connect", ws_connect), NET("listen", ws_listen), NET("accept", ws_accept), NET("send", ws_send), NET("recv", ws_recv),
    NET("sendto", ws_sendto), NET("recvfrom", ws_recvfrom), NET("shutdown", ws_shutdown), NET("getsockname", ws_getsockname),
    NET("getpeername", ws_getpeername), VX_EXPORT(WSASend), VX_EXPORT(WSARecv), VX_EXPORT(WSASendTo), VX_EXPORT(WSARecvFrom),
    VX_EXPORT(WSAIoctl), VX_EXPORT(GetAcceptExSockaddrs), NET("#1142", GetAcceptExSockaddrs), VX_EXPORT(AcceptEx), NET("#1141", AcceptEx),
    VX_EXPORT(WSAEventSelect), VX_EXPORT(WSAEnumNetworkEvents), VX_EXPORT(WSACreateEvent), VX_EXPORT(WSACloseEvent), VX_EXPORT(WSASetEvent),
    VX_EXPORT(WSAResetEvent), VX_EXPORT(WSAWaitForMultipleEvents), NET("setsockopt", ws_setsockopt),
    NET("getsockopt", ws_getsockopt), VX_EXPORT(ioctlsocket), NET("select", ws_select), VX_EXPORT(__WSAFDIsSet),
    NET("htons", ws_htons), NET("ntohs", ws_ntohs), NET("htonl", ws_htonl), NET("ntohl", ws_ntohl),
    NET("inet_addr", ws_inet_addr), NET("inet_ntoa", ws_inet_ntoa), NET("inet_pton", ws_inet_pton), NET("inet_ntop", ws_inet_ntop),
    NET("gethostname", ws_gethostname), NET("gethostbyname", ws_gethostbyname), NET("getaddrinfo", ws_getaddrinfo),
    NET("freeaddrinfo", ws_freeaddrinfo), VX_EXPORT(GetAddrInfoW), { "FreeAddrInfoW", (void *)ws_freeaddrinfo },
    NET("getnameinfo", ws_getnameinfo),
    // Ordinals, for programs that import ws2_32 by number.
    NET("#1", ws_accept), NET("#2", ws_bind), NET("#3", closesocket), NET("#4", ws_connect), NET("#5", ws_getpeername),
    NET("#6", ws_getsockname), NET("#7", ws_getsockopt), NET("#8", ws_htonl), NET("#9", ws_htons), NET("#10", ioctlsocket),
    NET("#11", ws_inet_addr), NET("#12", ws_inet_ntoa), NET("#13", ws_listen), NET("#14", ws_ntohl), NET("#15", ws_ntohs),
    NET("#16", ws_recv), NET("#17", ws_recvfrom), NET("#18", ws_select), NET("#19", ws_send), NET("#20", ws_sendto),
    NET("#21", ws_setsockopt), NET("#22", ws_shutdown), NET("#23", ws_socket), NET("#52", ws_gethostbyname),
    NET("#57", ws_gethostname), NET("#111", WSAGetLastError), NET("#112", WSASetLastError), NET("#115", WSAStartup),
    NET("#116", WSACleanup), NET("#151", __WSAFDIsSet),
};

void vx_net_init(void) {
    vx_register_module("ws2_32.dll", ws2, sizeof ws2 / sizeof ws2[0]);
    vx_register_module("wsock32.dll", ws2, sizeof ws2 / sizeof ws2[0]);
    vx_register_module("mswsock.dll", ws2, sizeof ws2 / sizeof ws2[0]);
}

#ifdef __aarch64__
#include "thunks/net.c.inc"
#endif
