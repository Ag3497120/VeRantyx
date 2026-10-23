// ws2_32's catalogs: the transport protocols (WSAEnumProtocols, WSCEnumProtocols, WSCGetProviderPath), the DNS name
// space (WSAEnumNameSpaceProviders, WSALookupServiceBegin/Next/End on 127.0.0.1 and localhost, WSASetService) and
// WSADuplicateSocket, which refuses (sockets are not shared between processes here).
#define _WIN32_WINNT 0x0A00                                      // (Windows 10: the Vista and Windows 8 declarations)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <ws2spi.h>
#include <svcguid.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("check failed (line %d): %s\n", __LINE__, #c); failures++; } } while (0)

static int lookup(GUID *service, const WCHAR *name, int family, int protocol, DWORD flags, WSAQUERYSETW **out) {
    WSAQUERYSETW query;
    AFPROTOCOLS protocols = { family, protocol };
    memset(&query, 0, sizeof query);
    query.dwSize = sizeof query;
    query.lpServiceClassId = service;
    query.lpszServiceInstanceName = (WCHAR *)name;
    query.dwNameSpace = NS_DNS;
    if (family) { query.dwNumberOfProtocols = 1; query.lpafpProtocols = &protocols; }
    HANDLE h = NULL;
    if (WSALookupServiceBeginW(&query, flags, &h)) return WSAGetLastError();
    DWORD size = 8;
    char small[8];
    CHECK(WSALookupServiceNextW(h, flags, &size, (WSAQUERYSETW *)small) == SOCKET_ERROR && WSAGetLastError() == WSAEFAULT && size > sizeof query);
    *out = malloc(size);
    CHECK(WSALookupServiceNextW(h, flags, &size, *out) == 0);
    DWORD more = size;
    char again[512];
    CHECK(WSALookupServiceNextW(h, flags, &more, (WSAQUERYSETW *)again) == SOCKET_ERROR && WSAGetLastError() == WSA_E_NO_MORE);
    CHECK(WSALookupServiceEnd(h) == 0);
    CHECK(WSALookupServiceEnd(h) == SOCKET_ERROR && WSAGetLastError() == WSA_INVALID_HANDLE);
    return 0;
}

int main(void) {
    WSADATA wsa;
    CHECK(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);

    // ---- transport protocols
    DWORD length = 0;
    CHECK(WSAEnumProtocolsW(NULL, NULL, &length) == SOCKET_ERROR && WSAGetLastError() == WSAENOBUFS && length == 4 * sizeof(WSAPROTOCOL_INFOW));
    WSAPROTOCOL_INFOW protocols[8];
    length = sizeof protocols;
    int n = WSAEnumProtocolsW(NULL, protocols, &length);
    CHECK(n == 4);
    int tcp4 = -1;
    for (int i = 0; i < n; i++) {
        WSAPROTOCOL_INFOW *p = &protocols[i];
        CHECK(p->ProtocolChain.ChainLen == BASE_PROTOCOL);
        CHECK(!memcmp(&p->ProviderId, &protocols[0].ProviderId, sizeof(GUID)));
        CHECK(i == 0 || p->dwCatalogEntryId != protocols[i - 1].dwCatalogEntryId);
        CHECK(p->iAddressFamily == AF_INET || p->iAddressFamily == AF_INET6);
        CHECK(p->iMaxSockAddr == (p->iAddressFamily == AF_INET ? (int)sizeof(struct sockaddr_in) : (int)sizeof(struct sockaddr_in6)));
        CHECK(p->iSocketType == SOCK_STREAM ? p->iProtocol == IPPROTO_TCP && (p->dwServiceFlags1 & XP1_GUARANTEED_DELIVERY)
                                            : p->iProtocol == IPPROTO_UDP && (p->dwServiceFlags1 & XP1_CONNECTIONLESS));
        CHECK(!(p->dwServiceFlags1 & XP1_IFS_HANDLES));                // (descriptors here are not file handles)
        CHECK(p->szProtocol[0] != 0);
        if (p->iAddressFamily == AF_INET && p->iSocketType == SOCK_STREAM) tcp4 = i;
    }
    CHECK(tcp4 >= 0);
    INT udp_only[] = { IPPROTO_UDP, 0 };
    length = sizeof protocols;
    CHECK(WSAEnumProtocolsW(udp_only, protocols + 4, &length) == 2 && protocols[4].iProtocol == IPPROTO_UDP && protocols[5].iProtocol == IPPROTO_UDP);
    WSAPROTOCOL_INFOA narrow[4];
    length = sizeof narrow;
    CHECK(WSAEnumProtocolsA(NULL, narrow, &length) == 4 && length == 4 * sizeof(WSAPROTOCOL_INFOA));
    char name[256];
    WideCharToMultiByte(CP_ACP, 0, protocols[0].szProtocol, -1, name, sizeof name, NULL, NULL);
    CHECK(!strcmp(narrow[0].szProtocol, name));

    // the SPI's view: the same catalog, errors through lpErrno; the provider's DLL, unexpanded
    int error = 0;
    length = 0;
    CHECK(WSCEnumProtocols(NULL, NULL, &length, &error) == SOCKET_ERROR && error == WSAENOBUFS && length == 4 * sizeof(WSAPROTOCOL_INFOW));
    length = sizeof protocols;
    CHECK(WSCEnumProtocols(NULL, protocols, &length, &error) == 4);
    WCHAR path[MAX_PATH];
    int path_length = MAX_PATH;
    CHECK(WSCGetProviderPath(&protocols[0].ProviderId, path, &path_length, &error) == 0 && !wcscmp(path, L"%SystemRoot%\\system32\\mswsock.dll"));
    path_length = 4;
    CHECK(WSCGetProviderPath(&protocols[0].ProviderId, path, &path_length, &error) == SOCKET_ERROR && error == WSAEFAULT && path_length > 4);
    GUID made_up = { 0x12345678, 0x1234, 0x1234, { 1, 2, 3, 4, 5, 6, 7, 8 } };
    path_length = MAX_PATH;
    CHECK(WSCGetProviderPath(&made_up, path, &path_length, &error) == SOCKET_ERROR && error == WSAEINVAL);

    // ---- WSADuplicateSocket: refused, as an invalid request; not a socket is not a socket
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    WSAPROTOCOL_INFOW shared;
    CHECK(s != INVALID_SOCKET);
    CHECK(WSADuplicateSocketW(s, GetCurrentProcessId(), &shared) == SOCKET_ERROR && WSAGetLastError() == WSAEINVAL);
    CHECK(WSADuplicateSocketW(s, GetCurrentProcessId(), NULL) == SOCKET_ERROR && WSAGetLastError() == WSAEFAULT);
    CHECK(WSADuplicateSocketW((SOCKET)0x7ffffff0, GetCurrentProcessId(), &shared) == SOCKET_ERROR && WSAGetLastError() == WSAENOTSOCK);
    closesocket(s);

    // ---- name spaces: DNS only
    length = 0;
    CHECK(WSAEnumNameSpaceProvidersW(&length, NULL) == SOCKET_ERROR && WSAGetLastError() == WSAEFAULT && length > sizeof(WSANAMESPACE_INFOW));
    WSANAMESPACE_INFOW *spaces = malloc(length);
    CHECK(WSAEnumNameSpaceProvidersW(&length, spaces) == 1);
    CHECK(spaces->dwNameSpace == NS_DNS && spaces->fActive && spaces->lpszIdentifier && spaces->lpszIdentifier[0]);

    GUID by_string = SVCID_INET_HOSTADDRBYINETSTRING, by_name = SVCID_INET_HOSTADDRBYNAME, hostname = SVCID_HOSTNAME;
    WSAQUERYSETW *result = NULL;
    CHECK(lookup(&by_string, L"127.0.0.1", AF_INET, IPPROTO_TCP, LUP_RETURN_NAME | LUP_RETURN_ADDR | LUP_RETURN_TYPE, &result) == 0);
    if (result) {
        CHECK(result->dwNameSpace == NS_DNS && result->lpNSProviderId && !memcmp(result->lpNSProviderId, &spaces->NSProviderId, sizeof(GUID)));
        CHECK(result->lpServiceClassId && !memcmp(result->lpServiceClassId, &by_string, sizeof(GUID)));
        CHECK(result->lpszServiceInstanceName && !wcscmp(result->lpszServiceInstanceName, L"127.0.0.1"));
        CHECK(result->dwNumberOfCsAddrs == 1 && result->lpcsaBuffer);
        CSADDR_INFO *a = result->lpcsaBuffer;
        struct sockaddr_in *remote = (struct sockaddr_in *)a->RemoteAddr.lpSockaddr;
        CHECK(a->iSocketType == SOCK_STREAM && a->iProtocol == IPPROTO_TCP && a->RemoteAddr.iSockaddrLength == sizeof *remote);
        CHECK(remote->sin_family == AF_INET && remote->sin_addr.s_addr == htonl(INADDR_LOOPBACK));
        CHECK(a->LocalAddr.lpSockaddr && a->LocalAddr.lpSockaddr->sa_family == AF_INET);
        free(result);
        result = NULL;
    }
    // only what is asked for comes back: the addresses, as UDP
    CHECK(lookup(&by_string, L"127.0.0.1", AF_INET, IPPROTO_UDP, LUP_RETURN_ADDR, &result) == 0);
    if (result) {
        CHECK(!result->lpszServiceInstanceName && !result->lpServiceClassId && result->dwNumberOfCsAddrs == 1);
        CHECK(result->lpcsaBuffer && result->lpcsaBuffer->iSocketType == SOCK_DGRAM && result->lpcsaBuffer->iProtocol == IPPROTO_UDP);
        free(result);
        result = NULL;
    }
    // a name from the hosts file (no network): at least one loopback address
    CHECK(lookup(&by_name, L"localhost", 0, 0, LUP_RETURN_ADDR, &result) == 0);
    if (result) {
        CHECK(result->dwNumberOfCsAddrs >= 1);
        for (DWORD i = 0; i < result->dwNumberOfCsAddrs; i++) {
            SOCKADDR *sa = result->lpcsaBuffer[i].RemoteAddr.lpSockaddr;
            CHECK(sa->sa_family == AF_INET ? ((struct sockaddr_in *)sa)->sin_addr.s_addr == htonl(INADDR_LOOPBACK)
                                           : sa->sa_family == AF_INET6 && IN6_IS_ADDR_LOOPBACK(&((struct sockaddr_in6 *)sa)->sin6_addr));
        }
        free(result);
        result = NULL;
    }
    CHECK(lookup(&by_string, L"not-a-number.invalid", 0, 0, LUP_RETURN_ADDR, &result) != 0);

    // what has no provider: another name space, another service class, containers, missing pieces
    WSAQUERYSETW query;
    memset(&query, 0, sizeof query);
    query.dwSize = sizeof query;
    query.lpServiceClassId = &hostname;
    query.dwNameSpace = NS_NLA;
    HANDLE h = (HANDLE)1;
    CHECK(WSALookupServiceBeginW(&query, LUP_RETURN_ALL, &h) == SOCKET_ERROR && WSAGetLastError() == WSASERVICE_NOT_FOUND);
    query.dwNameSpace = NS_DNS;
    query.lpServiceClassId = &made_up;
    CHECK(WSALookupServiceBeginW(&query, LUP_RETURN_ALL, &h) == SOCKET_ERROR && WSAGetLastError() == WSASERVICE_NOT_FOUND);
    query.lpServiceClassId = &hostname;
    CHECK(WSALookupServiceBeginW(&query, LUP_CONTAINERS, &h) == SOCKET_ERROR && WSAGetLastError() == WSANO_DATA);
    query.lpServiceClassId = NULL;
    CHECK(WSALookupServiceBeginW(&query, LUP_RETURN_ALL, &h) == SOCKET_ERROR && WSAGetLastError() == WSAEINVAL);
    DWORD size = 64;
    char buffer[64];
    CHECK(WSALookupServiceNextW((HANDLE)0x1234, 0, &size, (WSAQUERYSETW *)buffer) == SOCKET_ERROR && WSAGetLastError() == WSA_INVALID_HANDLE);

    // registration: a no-op in DNS, nothing to delete; NLA has no provider
    query.lpServiceClassId = &hostname;
    query.lpszServiceInstanceName = L"made-up.test";
    CHECK(WSASetServiceW(&query, RNRSERVICE_REGISTER, 0) == 0);
    CHECK(WSASetServiceW(&query, RNRSERVICE_DEREGISTER, 0) == 0);
    CHECK(WSASetServiceW(&query, RNRSERVICE_DELETE, 0) == SOCKET_ERROR && WSAGetLastError() == WSASERVICE_NOT_FOUND);
    query.dwNameSpace = NS_NLA;
    CHECK(WSASetServiceW(&query, RNRSERVICE_REGISTER, 0) == SOCKET_ERROR && WSAGetLastError() == WSAEINVAL);
    free(spaces);

    WSACleanup();
    if (failures) { printf("winsock_nsp: %d checks failed\n", failures); return 1; }
    puts("winsock_nsp: protocol catalog, provider path, DNS name space lookups, registration, duplicate refused: ok");
    return 0;
}
