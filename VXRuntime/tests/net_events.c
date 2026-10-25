// WSAEventSelect and WSAEnumNetworkEvents as Microsoft documents them, the way Chromium's sockets use them (Steam's
// web helper connects to steamclient's local WebSocket servers this way): a non-blocking connect reports FD_CONNECT
// with its result in iErrorCode[FD_CONNECT_BIT] (a refused one was reported as connected, so Chromium kept a dead
// socket); FD_WRITE with a successful connect; FD_ACCEPT; a socket accepted from a registered listening socket starts
// with the same registration; FD_READ, then FD_CLOSE once the data is read, with 0 for an orderly close and
// WSAECONNRESET for a reset; closesocket ends the registration (a socket opened later with the same number must not set
// the old event).
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("net_events: FAILED %s (line %d, WSA error %d)\n", #c, __LINE__, WSAGetLastError()); failures++; } } while (0)

// Waits for the event, then takes the record: the events seen (0 on timeout), their error codes in *ne.
static long wait_events(SOCKET s, WSAEVENT ev, WSANETWORKEVENTS *ne, DWORD ms) {
    memset(ne, 0, sizeof *ne);
    if (WSAWaitForMultipleEvents(1, &ev, FALSE, ms, FALSE) != WSA_WAIT_EVENT_0) return 0;
    if (WSAEnumNetworkEvents(s, ev, ne)) return -1;
    return ne->lNetworkEvents;
}
// Waits until `want` has been seen (several records may come first), collecting the events and the last error codes.
static long wait_for(SOCKET s, WSAEVENT ev, long want, WSANETWORKEVENTS *all, DWORD ms) {
    memset(all, 0, sizeof *all);
    DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < ms && (all->lNetworkEvents & want) != want) {
        WSANETWORKEVENTS ne;
        long got = wait_events(s, ev, &ne, 100);
        if (got <= 0) continue;
        all->lNetworkEvents |= got;
        for (int k = 0; k < FD_MAX_EVENTS; k++) if (got & (1 << k)) all->iErrorCode[k] = ne.iErrorCode[k];
    }
    return all->lNetworkEvents;
}
static SOCKET listener4(unsigned short *port) {
    SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(l, (struct sockaddr *)&a, sizeof a);
    listen(l, 4);
    int len = sizeof a;
    getsockname(l, (struct sockaddr *)&a, &len);
    *port = ntohs(a.sin_port);
    return l;
}
// Starts a non-blocking connect registered for FD_CONNECT; returns what connect() said (0 or its WSA error).
static int start_connect(SOCKET s, WSAEVENT ev, const struct sockaddr *a, int len, long mask) {
    CHECK(WSAEventSelect(s, ev, mask) == 0);
    return connect(s, a, len) ? WSAGetLastError() : 0;
}

int main(void) {
    WSADATA wsa;
    CHECK(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    WSANETWORKEVENTS ne;

    // 1. Refused, IPv4 and IPv6: FD_CONNECT with WSAECONNREFUSED (or the refusal at once from connect).
    unsigned short port4;
    SOCKET l = listener4(&port4);                                  // (IPv4 only: [::1] at that port is refused)
    SOCKET probe = listener4(&(unsigned short){ 0 });
    struct sockaddr_in closed4 = { 0 };
    int clen = sizeof closed4;
    getsockname(probe, (struct sockaddr *)&closed4, &clen);
    closesocket(probe);                                            // (a port nobody listens on now)
    for (int v6 = 0; v6 < 2; v6++) {
        SOCKET s = socket(v6 ? AF_INET6 : AF_INET, SOCK_STREAM, IPPROTO_TCP);
        WSAEVENT ev = WSACreateEvent();
        struct sockaddr_in6 a6 = { 0 };
        a6.sin6_family = AF_INET6; a6.sin6_addr = in6addr_loopback; a6.sin6_port = htons(port4);
        int r = v6 ? start_connect(s, ev, (struct sockaddr *)&a6, sizeof a6, FD_CONNECT | FD_READ | FD_CLOSE)
                   : start_connect(s, ev, (struct sockaddr *)&closed4, sizeof closed4, FD_CONNECT | FD_READ | FD_CLOSE);
        CHECK(r == WSAEWOULDBLOCK || r == WSAECONNREFUSED);
        if (r == WSAEWOULDBLOCK) {
            long got = wait_for(s, ev, FD_CONNECT, &ne, 3000);
            CHECK(got & FD_CONNECT);
            CHECK(ne.iErrorCode[FD_CONNECT_BIT] == WSAECONNREFUSED);
            if (ne.iErrorCode[FD_CONNECT_BIT] != WSAECONNREFUSED)
                printf("net_events: %s refused connect: FD_CONNECT error %d\n", v6 ? "IPv6" : "IPv4", ne.iErrorCode[FD_CONNECT_BIT]);
            CHECK(!(got & (FD_WRITE | FD_CLOSE)));                 // (a connect that failed: FD_CONNECT alone)
        }
        closesocket(s); WSACloseEvent(ev);
    }

    // 2. Connected: FD_CONNECT with 0 and FD_WRITE; the listening socket's FD_ACCEPT; the accepted socket inherits it.
    WSAEVENT lev = WSACreateEvent();
    CHECK(WSAEventSelect(l, lev, FD_ACCEPT | FD_READ | FD_CLOSE) == 0);
    SOCKET c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    WSAEVENT cev = WSACreateEvent();
    struct sockaddr_in a4 = { 0 };
    a4.sin_family = AF_INET; a4.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a4.sin_port = htons(port4);
    int r = start_connect(c, cev, (struct sockaddr *)&a4, sizeof a4, FD_CONNECT | FD_WRITE | FD_READ | FD_CLOSE);
    CHECK(r == WSAEWOULDBLOCK || r == 0);
    long got = wait_for(c, cev, FD_CONNECT | FD_WRITE, &ne, 3000);
    CHECK((got & (FD_CONNECT | FD_WRITE)) == (FD_CONNECT | FD_WRITE) && ne.iErrorCode[FD_CONNECT_BIT] == 0);
    got = wait_for(l, lev, FD_ACCEPT, &ne, 3000);
    if (got != FD_ACCEPT) printf("net_events: listening socket: events %#lx\n", got);
    CHECK(got == FD_ACCEPT);
    SOCKET acc = accept(l, NULL, NULL);
    CHECK(acc != INVALID_SOCKET);
    CHECK(send(c, "hello", 5, 0) == 5);
    got = wait_for(acc, lev, FD_READ, &ne, 3000);                  // (on the listening socket's event: inherited)
    CHECK(got & FD_READ);
    char buf[16];
    // 3. FD_CLOSE after the data: an orderly close reports 0, and only once the data has been read.
    CHECK(shutdown(c, SD_SEND) == 0);
    WSAEVENT aev = WSACreateEvent();
    CHECK(WSAEventSelect(acc, aev, FD_READ | FD_CLOSE) == 0);
    got = wait_for(acc, aev, FD_READ, &ne, 3000);
    CHECK(got == FD_READ);                                        // (data still there: no FD_CLOSE yet)
    CHECK(recv(acc, buf, sizeof buf, 0) == 5);
    got = wait_for(acc, aev, FD_CLOSE, &ne, 3000);
    CHECK((got & FD_CLOSE) && ne.iErrorCode[FD_CLOSE_BIT] == 0);
    closesocket(acc); closesocket(c);
    WSACloseEvent(aev); WSACloseEvent(cev);

    // 4. A reset connection: FD_CLOSE with WSAECONNRESET (or WSAECONNABORTED).
    c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    CHECK(connect(c, (struct sockaddr *)&a4, sizeof a4) == 0);
    WSAEnumNetworkEvents(l, lev, &ne);
    acc = accept(l, NULL, NULL);
    CHECK(acc != INVALID_SOCKET);
    aev = WSACreateEvent();
    CHECK(WSAEventSelect(acc, aev, FD_READ | FD_CLOSE) == 0);
    struct linger lg = { 1, 0 };
    setsockopt(c, SOL_SOCKET, SO_LINGER, (char *)&lg, sizeof lg);
    closesocket(c);                                                // (linger 0: a reset)
    got = wait_for(acc, aev, FD_CLOSE, &ne, 3000);
    CHECK(got & FD_CLOSE);
    CHECK(ne.iErrorCode[FD_CLOSE_BIT] == WSAECONNRESET || ne.iErrorCode[FD_CLOSE_BIT] == WSAECONNABORTED);

    // 5. closesocket ends the registration: a new socket, likely with the same number, does not set the old event.
    SOCKET old = acc, n = INVALID_SOCKET, spare[16];
    int nspare = 0;
    closesocket(acc);
    WSAResetEvent(aev);
    while (nspare < 16) {                                          // (until one gets the closed socket's number)
        SOCKET t = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (t == old) { n = t; break; }
        spare[nspare++] = t;
    }
    for (int i = 0; i < nspare; i++) closesocket(spare[i]);
    if (n == INVALID_SOCKET) { n = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); printf("net_events: no socket got number %llu again\n", (unsigned long long)old); }
    CHECK(connect(n, (struct sockaddr *)&a4, sizeof a4) == 0);
    CHECK(send(n, "x", 1, 0) == 1);
    SOCKET acc2 = accept(l, NULL, NULL);
    CHECK(send(acc2, "y", 1, 0) == 1);
    CHECK(WSAWaitForMultipleEvents(1, &aev, FALSE, 300, FALSE) == WSA_WAIT_TIMEOUT);
    closesocket(n); closesocket(acc2); closesocket(l);
    WSACloseEvent(aev); WSACloseEvent(lev);
    WSACleanup();
    printf("net_events: FD_CONNECT results, FD_ACCEPT, inherited registration, FD_READ then FD_CLOSE (orderly, reset),"
           " closesocket: %d failed checks -> %s\n", failures, failures ? "FAILED" : "ok");
    return failures != 0;
}
