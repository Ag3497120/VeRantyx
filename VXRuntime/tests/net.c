// Milestone 4b: Winsock. TCP over loopback (listen, connect from a thread, accept, send/recv, select),
// UDP (sendto/recvfrom), name lookup, socket options and error codes.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <string.h>
static int failures;
#define CHECK(c) do { if (!(c)) { printf("check failed (line %d): %s (WSA error %d)\n", __LINE__, #c, WSAGetLastError()); failures++; } } while (0)
static unsigned short port;

static DWORD WINAPI client(void *p) {
    (void)p;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(s, (struct sockaddr *)&a, sizeof a)) return 1;
    send(s, "ping", 4, 0);
    char buf[8] = { 0 };
    int n = recv(s, buf, sizeof buf, 0);
    closesocket(s);
    return n == 4 && !memcmp(buf, "pong", 4) ? 0 : 2;
}

int main(void) {
    WSADATA wsa;
    CHECK(WSAStartup(MAKEWORD(2, 2), &wsa) == 0 && wsa.wVersion == MAKEWORD(2, 2));
    SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    CHECK(l != INVALID_SOCKET);
    BOOL on = TRUE;
    CHECK(setsockopt(l, SOL_SOCKET, SO_REUSEADDR, (char *)&on, sizeof on) == 0);
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
    CHECK(bind(l, (struct sockaddr *)&a, sizeof a) == 0);
    CHECK(listen(l, 4) == 0);
    int len = sizeof a;
    CHECK(getsockname(l, (struct sockaddr *)&a, &len) == 0 && a.sin_family == AF_INET);
    port = ntohs(a.sin_port);
    HANDLE t = CreateThread(NULL, 0, client, NULL, 0, NULL);
    fd_set rs; FD_ZERO(&rs); FD_SET(l, &rs);
    struct timeval tv = { 5, 0 };
    CHECK(select(0, &rs, NULL, NULL, &tv) == 1 && FD_ISSET(l, &rs));
    struct sockaddr_in peer; int plen = sizeof peer;
    SOCKET c = accept(l, (struct sockaddr *)&peer, &plen);
    CHECK(c != INVALID_SOCKET && peer.sin_family == AF_INET && peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK));
    char buf[8] = { 0 };
    CHECK(recv(c, buf, 4, MSG_WAITALL) == 4 && !memcmp(buf, "ping", 4));
    CHECK(send(c, "pong", 4, 0) == 4);
    WaitForSingleObject(t, 5000);
    DWORD rc = 9; GetExitCodeThread(t, &rc);
    CHECK(rc == 0);
    // A non-blocking receive with nothing left: WSAEWOULDBLOCK, then the orderly close shows as 0 bytes.
    u_long nb = 1;
    CHECK(ioctlsocket(c, FIONBIO, &nb) == 0);
    Sleep(50);
    int n = recv(c, buf, sizeof buf, 0);
    CHECK(n == 0 || (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK));
    closesocket(c); closesocket(l);

    // UDP.
    SOCKET u1 = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP), u2 = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in ua = { 0 }; ua.sin_family = AF_INET; ua.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(u1, (struct sockaddr *)&ua, sizeof ua) == 0);
    len = sizeof ua; getsockname(u1, (struct sockaddr *)&ua, &len);
    CHECK(sendto(u2, "datagram", 8, 0, (struct sockaddr *)&ua, sizeof ua) == 8);
    struct sockaddr_in from; int flen = sizeof from; char dg[16] = { 0 };
    DWORD timeout = 2000;
    setsockopt(u1, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof timeout);
    CHECK(recvfrom(u1, dg, sizeof dg, 0, (struct sockaddr *)&from, &flen) == 8 && !strcmp(dg, "datagram") && from.sin_family == AF_INET);
    closesocket(u1); closesocket(u2);

    // Names.
    struct addrinfo hints = { 0 }, *res = NULL;
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    CHECK(getaddrinfo("localhost", "80", &hints, &res) == 0 && res);
    int fams = 0;
    for (struct addrinfo *p = res; p; p = p->ai_next) fams |= p->ai_family == AF_INET ? 1 : p->ai_family == AF_INET6 ? 2 : 4;
    CHECK(fams && !(fams & 4));
    if (res) { CHECK(((struct sockaddr_in *)res->ai_addr)->sin_port == htons(80)); freeaddrinfo(res); }
    char host[256];
    CHECK(gethostname(host, sizeof host) == 0 && host[0]);
    struct hostent *he = gethostbyname("localhost");
    CHECK(he && he->h_addrtype == AF_INET && he->h_length == 4);
    // Errors: connecting to a closed port.
    SOCKET r = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in closed = { 0 }; closed.sin_family = AF_INET; closed.sin_addr.s_addr = htonl(INADDR_LOOPBACK); closed.sin_port = htons(1);
    CHECK(connect(r, (struct sockaddr *)&closed, sizeof closed) == SOCKET_ERROR && WSAGetLastError() == WSAECONNREFUSED);
    closesocket(r);
    WSACleanup();
    printf("net: tcp, udp, select, names, errors: %d failed checks -> %s\n", failures, failures ? "FAILED" : "ok");
    return failures;
}
