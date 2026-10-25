// GetExtendedTcpTable: a loopback connection made here is listed with this process's ID on both ends, the listener
// as LISTEN; the size is asked first; the listener and connection classes and bOrder behave as documented
// (src/tcptable.c). The Steam client checks its WebUI connections this way.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <stdio.h>
#include <stdlib.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static MIB_TCPTABLE_OWNER_PID *table(TCP_TABLE_CLASS cls, BOOL order) {
    DWORD size = 0;
    DWORD r = GetExtendedTcpTable(NULL, &size, order, AF_INET, cls, 0);
    CHECK(r == ERROR_INSUFFICIENT_BUFFER && size >= 4, "size query: %lu, size %lu", r, size);
    size += 10 * sizeof(MIB_TCPROW_OWNER_PID);                     // (connections may come and go meanwhile)
    MIB_TCPTABLE_OWNER_PID *t = malloc(size);
    r = GetExtendedTcpTable(t, &size, order, AF_INET, cls, 0);
    CHECK(r == NO_ERROR, "table class %d: %lu", cls, r);
    return t;
}
static const MIB_TCPROW_OWNER_PID *find(const MIB_TCPTABLE_OWNER_PID *t, u_short lport, u_short rport) {
    for (DWORD i = 0; i < t->dwNumEntries; i++)
        if (t->table[i].dwLocalPort == lport && t->table[i].dwRemotePort == rport && t->table[i].dwLocalAddr == htonl(INADDR_LOOPBACK))
            return &t->table[i];
    return NULL;
}

int main(void) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    int len = sizeof a;
    CHECK(!bind(l, (struct sockaddr *)&a, sizeof a) && !listen(l, 4) && !getsockname(l, (struct sockaddr *)&a, &len), "listen");
    SOCKET c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    CHECK(!connect(c, (struct sockaddr *)&a, sizeof a), "connect");
    SOCKET s = accept(l, NULL, NULL);
    CHECK(s != INVALID_SOCKET, "accept");
    struct sockaddr_in ca; len = sizeof ca;
    getsockname(c, (struct sockaddr *)&ca, &len);
    u_short lp = a.sin_port, cp = ca.sin_port;                     // (network order, as the table has them)
    DWORD me = GetCurrentProcessId();

    MIB_TCPTABLE_OWNER_PID *all = table(TCP_TABLE_OWNER_PID_ALL, TRUE);
    const MIB_TCPROW_OWNER_PID *client = find(all, cp, lp), *server = find(all, lp, cp), *listener = find(all, lp, 0);
    CHECK(client && client->dwOwningPid == me && client->dwState == MIB_TCP_STATE_ESTAB && client->dwRemoteAddr == htonl(INADDR_LOOPBACK),
          "client end: %p pid %lu state %lu", (void *)client, client ? client->dwOwningPid : 0, client ? client->dwState : 0);
    CHECK(server && server->dwOwningPid == me && server->dwState == MIB_TCP_STATE_ESTAB, "server end");
    CHECK(listener && listener->dwOwningPid == me && listener->dwState == MIB_TCP_STATE_LISTEN && !listener->dwRemoteAddr, "listener");
    for (DWORD i = 1; i < all->dwNumEntries; i++) {                // bOrder: local address, then local port
        const MIB_TCPROW_OWNER_PID *p = &all->table[i - 1], *q = &all->table[i];
        CHECK(ntohl(p->dwLocalAddr) < ntohl(q->dwLocalAddr) ||
              (p->dwLocalAddr == q->dwLocalAddr && ntohs((u_short)p->dwLocalPort) <= ntohs((u_short)q->dwLocalPort)), "order at %lu", i);
    }
    MIB_TCPTABLE_OWNER_PID *conns = table(TCP_TABLE_OWNER_PID_CONNECTIONS, FALSE), *listeners = table(TCP_TABLE_OWNER_PID_LISTENER, FALSE);
    CHECK(find(conns, cp, lp) && !find(conns, lp, 0), "connections only");
    CHECK(find(listeners, lp, 0) && !find(listeners, cp, lp), "listeners only");
    for (DWORD i = 0; i < listeners->dwNumEntries; i++) CHECK(listeners->table[i].dwState == MIB_TCP_STATE_LISTEN, "listener state");

    // The basic table: the same rows without the process.
    DWORD size = 0;
    CHECK(GetExtendedTcpTable(NULL, &size, FALSE, AF_INET, TCP_TABLE_BASIC_ALL, 0) == ERROR_INSUFFICIENT_BUFFER, "basic size");
    size += 10 * sizeof(MIB_TCPROW);
    MIB_TCPTABLE *basic = malloc(size);
    CHECK(GetExtendedTcpTable(basic, &size, FALSE, AF_INET, TCP_TABLE_BASIC_ALL, 0) == NO_ERROR, "basic table");
    int seen = 0;
    for (DWORD i = 0; i < basic->dwNumEntries; i++) seen |= basic->table[i].dwLocalPort == cp && basic->table[i].dwRemotePort == lp;
    CHECK(seen, "basic row");
    CHECK(GetExtendedTcpTable(basic, &size, FALSE, 7 /* not a family */, TCP_TABLE_OWNER_PID_ALL, 0) == ERROR_INVALID_PARAMETER, "family");
    CHECK(GetExtendedTcpTable(NULL, NULL, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0) == ERROR_INVALID_PARAMETER, "no size");

    // Closed: the connection's rows go (TIME_WAIT at most, never ESTAB with this process).
    closesocket(c); closesocket(s); closesocket(l);
    MIB_TCPTABLE_OWNER_PID *after = table(TCP_TABLE_OWNER_PID_ALL, FALSE);
    const MIB_TCPROW_OWNER_PID *gone = find(after, cp, lp);
    CHECK(!gone || gone->dwState != MIB_TCP_STATE_ESTAB, "closed connection still established");
    CHECK(!find(after, lp, 0), "closed listener still listed");
    printf(fails ? "tcptable: %d failures\n" : "tcptable: ok\n", fails);
    return fails != 0;
}
