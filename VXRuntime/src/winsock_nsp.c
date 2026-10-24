// ws2_32's catalogs: the transport protocols (WSAEnumProtocols, WSCEnumProtocols, WSCGetProviderPath) and the name
// spaces (WSAEnumNameSpaceProviders, WSALookupServiceBegin/Next/End, WSASetService), plus WSADuplicateSocket.
//
// The transport provider is this runtime's own (net.c: BSD sockets; mswsock.dll is its DLL here), so it carries its
// own provider GUID, not Microsoft's: TCP and UDP over IPv4 and IPv6, base protocols (chain length 1). Its socket
// descriptors are not file handles (ReadFile does not take them), so XP1_IFS_HANDLES is not claimed.
//
// One name-space provider: DNS (NS_DNS), answered by the Mac's resolver (getaddrinfo), for the host-name service
// classes (SVCID_HOSTNAME, SVCID_INET_HOSTADDRBYNAME, SVCID_INET_HOSTADDRBYINETSTRING). Registration is a no-op in the
// DNS name space, as the WSASetService documentation says. Other name spaces (NLA, ...) have no provider here.
#include "vx.h"
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define SOCKET_ERROR (-1)
enum {
    WSA_INVALID_HANDLE = 6, WSA_NOT_ENOUGH_MEMORY = 8, WSAEFAULT = 10014, WSAEINVAL = 10022, WSAENOTSOCK = 10038,
    WSAENOBUFS = 10055, WSA_E_NO_MORE = 10110, WSASERVICE_NOT_FOUND = 10108, WSATRY_AGAIN = 11002, WSANO_DATA = 11004,
};
enum { NS_ALL = 0, NS_DNS = 12 };
enum { LUP_CONTAINERS = 0x2, LUP_RETURN_NAME = 0x10, LUP_RETURN_TYPE = 0x20, LUP_RETURN_ADDR = 0x100,
       LUP_FLUSHPREVIOUS = 0x2000 };                                // (winsock2.h's values; the web page swaps FLUSHCACHE/FLUSHPREVIOUS)
enum { WIN_AF_INET = 2, WIN_AF_INET6 = 23 };
typedef struct { uint32_t d1; uint16_t d2, d3; uint8_t d4[8]; } guid;

static int same_guid(const guid *a, const guid *b) { return !memcmp(a, b, sizeof *a); }

// ---- the transport catalog -------------------------------------------------------------------------------------------------
// WSAPROTOCOL_INFOW (628 bytes) / WSAPROTOCOL_INFOA (372): the same fields, then the name in WCHARs or chars.
typedef struct {
    DWORD ServiceFlags1, ServiceFlags2, ServiceFlags3, ServiceFlags4, ProviderFlags;
    guid ProviderId;
    DWORD CatalogEntryId;
    int32_t ChainLen;
    DWORD ChainEntries[7];
    int32_t Version, AddressFamily, MaxSockAddr, MinSockAddr, SocketType, Protocol, ProtocolMaxOffset, NetworkByteOrder, SecurityScheme;
    DWORD MessageSize, ProviderReserved;
} protocol_fields;
_Static_assert(sizeof(protocol_fields) == 116, "WSAPROTOCOL_INFO up to szProtocol");
enum { PROTOCOL_INFO_W = 116 + 256 * 2, PROTOCOL_INFO_A = 116 + 256 };

// {5658B5D0-7C1E-4B8A-9E2D-56585254534B}: this runtime's BSD-socket provider
static const guid g_provider = { 0x5658b5d0, 0x7c1e, 0x4b8a, { 0x9e, 0x2d, 0x56, 0x58, 0x52, 0x54, 0x53, 0x4b } };
enum {
    XP1_CONNECTIONLESS = 0x1, XP1_GUARANTEED_DELIVERY = 0x2, XP1_GUARANTEED_ORDER = 0x4, XP1_MESSAGE_ORIENTED = 0x8,
    XP1_GRACEFUL_CLOSE = 0x20, XP1_EXPEDITED_DATA = 0x40, XP1_SUPPORT_BROADCAST = 0x200, XP1_SUPPORT_MULTIPOINT = 0x400,
    PFL_MATCHES_PROTOCOL_ZERO = 0x8,
};
typedef struct { int family, type, protocol, max_message; const char *name; } catalog_entry;
static const catalog_entry g_catalog[] = {
    { WIN_AF_INET, 1, 6, 0, "VXRuntime [TCP/IP]" },
    { WIN_AF_INET, 2, 17, 65507, "VXRuntime [UDP/IP]" },            // (the largest IPv4 UDP payload)
    { WIN_AF_INET6, 1, 6, 0, "VXRuntime [TCP/IPv6]" },
    { WIN_AF_INET6, 2, 17, 65527, "VXRuntime [UDP/IPv6]" },
};
enum { CATALOG = sizeof g_catalog / sizeof g_catalog[0], CATALOG_FIRST_ID = 1001 };

static void protocol_info(int i, uint8_t *out, int wide) {
    const catalog_entry *e = &g_catalog[i];
    protocol_fields f;
    memset(&f, 0, sizeof f);
    f.ServiceFlags1 = e->type == 1 ? XP1_GUARANTEED_DELIVERY | XP1_GUARANTEED_ORDER | XP1_GRACEFUL_CLOSE | XP1_EXPEDITED_DATA
                                   : XP1_CONNECTIONLESS | XP1_MESSAGE_ORIENTED | XP1_SUPPORT_BROADCAST | XP1_SUPPORT_MULTIPOINT;
    f.ProviderFlags = PFL_MATCHES_PROTOCOL_ZERO;
    f.ProviderId = g_provider;
    f.CatalogEntryId = (DWORD)(CATALOG_FIRST_ID + i);
    f.ChainLen = 1;                                                 // BASE_PROTOCOL
    f.ChainEntries[0] = f.CatalogEntryId;
    f.Version = 2;
    f.AddressFamily = e->family;
    f.MaxSockAddr = f.MinSockAddr = e->family == WIN_AF_INET ? 16 : 28;
    f.SocketType = e->type;
    f.Protocol = e->protocol;
    f.MessageSize = (DWORD)e->max_message;
    memset(out, 0, wide ? PROTOCOL_INFO_W : PROTOCOL_INFO_A);
    memcpy(out, &f, sizeof f);
    if (wide) vx_utf8_to_utf16(e->name, strlen(e->name) + 1, (WCHAR *)(out + sizeof f), 256);
    else memcpy(out + sizeof f, e->name, strlen(e->name) + 1);
}
// The entries whose protocol is in the zero-terminated list (all of them without one); returns the count, or -1 with
// *error set (the buffer too small: the size needed in *length).
static int enum_protocols(const int32_t *protocols, uint8_t *buffer, DWORD *length, int wide, int *error) {
    int chosen[CATALOG], n = 0;
    for (int i = 0; i < CATALOG; i++) {
        int take = !protocols;
        for (const int32_t *p = protocols; p && *p && !take; p++) take = *p == g_catalog[i].protocol;
        if (take) chosen[n++] = i;
    }
    if (!length) { *error = WSAEFAULT; return -1; }
    DWORD size = wide ? PROTOCOL_INFO_W : PROTOCOL_INFO_A, need = size * (DWORD)n;
    if (!buffer || *length < need) { *length = need; *error = WSAENOBUFS; return -1; }
    for (int k = 0; k < n; k++) protocol_info(chosen[k], buffer + size * (DWORD)k, wide);
    *length = need;
    return n;
}
static WINAPI int WSAEnumProtocolsW(const int32_t *protocols, uint8_t *buffer, DWORD *length) {
    int error = 0, n = enum_protocols(protocols, buffer, length, 1, &error);
    if (n < 0) vx_set_last_error((DWORD)error);
    return n < 0 ? SOCKET_ERROR : n;
}
static WINAPI int WSAEnumProtocolsA(const int32_t *protocols, uint8_t *buffer, DWORD *length) {
    int error = 0, n = enum_protocols(protocols, buffer, length, 0, &error);
    if (n < 0) vx_set_last_error((DWORD)error);
    return n < 0 ? SOCKET_ERROR : n;
}
// The SPI's view: the same catalog (nothing here is hidden or layered), the error through lpErrno.
static WINAPI int WSCEnumProtocols(const int32_t *protocols, uint8_t *buffer, DWORD *length, int *error) {
    int e = 0, n = enum_protocols(protocols, buffer, length, 1, &e);
    if (n < 0 && error) *error = e;
    return n < 0 ? SOCKET_ERROR : n;
}
// The provider DLL's path, environment strings not expanded (as documented).
static WINAPI int WSCGetProviderPath(const guid *provider, WCHAR *path, int *length, int *error) {
    static const char dll[] = "%SystemRoot%\\system32\\mswsock.dll";
    int need = (int)sizeof dll;
    int e = 0;
    if (!provider || !same_guid(provider, &g_provider)) e = WSAEINVAL;
    else if (!path || !length || *length < need) { if (length) *length = need; e = WSAEFAULT; }
    if (e) { if (error) *error = e; return SOCKET_ERROR; }
    vx_utf8_to_utf16(dll, sizeof dll, path, (size_t)*length);
    *length = need;
    return 0;
}

// ---- WSADuplicateSocket ----------------------------------------------------------------------------------------------------
// A descriptor here is the process's own file descriptor; handing it to another process would need the descriptor
// passed between the two runtimes, which they do not do yet. The request fails as an invalid one rather than give
// the target process a protocol structure it cannot use.
static int duplicate_socket(uint64_t s, DWORD pid, void *info, const char *name) {
    int type = 0;
    socklen_t length = sizeof type;
    if (s > 0x7fffffff || getsockopt((int)s, SOL_SOCKET, SO_TYPE, &type, &length)) { vx_set_last_error(WSAENOTSOCK); return SOCKET_ERROR; }
    if (!info) { vx_set_last_error(WSAEFAULT); return SOCKET_ERROR; }
    static int logged;
    if (!logged++) vx_log("%s: socket %llu for process %u: sockets are not shared between processes here", name, (unsigned long long)s, pid);
    vx_set_last_error(WSAEINVAL);
    return SOCKET_ERROR;
}
static WINAPI int WSADuplicateSocketW(uint64_t s, DWORD pid, void *info) { return duplicate_socket(s, pid, info, "WSADuplicateSocketW"); }
static WINAPI int WSADuplicateSocketA(uint64_t s, DWORD pid, void *info) { return duplicate_socket(s, pid, info, "WSADuplicateSocketA"); }

// ---- the DNS name space ----------------------------------------------------------------------------------------------------
// {5658B5D1-7C1E-4B8A-9E2D-56584E53444E}: the DNS name-space provider
static const guid g_dns_provider = { 0x5658b5d1, 0x7c1e, 0x4b8a, { 0x9e, 0x2d, 0x56, 0x58, 0x4e, 0x53, 0x44, 0x4e } };
static const char g_dns_identifier[] = "VXRuntime DNS";
// svcguid.h: {0002A80x-0000-0000-C000-000000000046}
static const guid g_svcid_hostname = { 0x0002a800, 0, 0, { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const guid g_svcid_by_inet_string = { 0x0002a801, 0, 0, { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const guid g_svcid_by_name = { 0x0002a803, 0, 0, { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };

// WSANAMESPACE_INFOW (40 bytes): provider GUID, name space, active, version, identifier; the identifier after the array.
static WINAPI int WSAEnumNameSpaceProvidersW(DWORD *length, uint8_t *buffer) {
    DWORD need = 40 + (DWORD)sizeof g_dns_identifier * 2;
    if (!length || !buffer || *length < need) { if (length) *length = need; vx_set_last_error(WSAEFAULT); return SOCKET_ERROR; }
    memset(buffer, 0, need);
    memcpy(buffer, &g_dns_provider, 16);
    *(DWORD *)(buffer + 16) = NS_DNS;
    *(int32_t *)(buffer + 20) = 1;                                  // fActive
    *(DWORD *)(buffer + 24) = 1;                                    // dwVersion
    WCHAR *identifier = (WCHAR *)(buffer + 40);
    vx_utf8_to_utf16(g_dns_identifier, sizeof g_dns_identifier, identifier, sizeof g_dns_identifier);
    *(WCHAR **)(buffer + 32) = identifier;
    return 1;
}

// WSAQUERYSETW (120 bytes on x64)
typedef struct {
    DWORD dwSize; WCHAR *lpszServiceInstanceName; guid *lpServiceClassId; void *lpVersion; WCHAR *lpszComment;
    DWORD dwNameSpace; guid *lpNSProviderId; WCHAR *lpszContext; DWORD dwNumberOfProtocols; int32_t *lpafpProtocols;
    WCHAR *lpszQueryString; DWORD dwNumberOfCsAddrs; uint8_t *lpcsaBuffer; DWORD dwOutputFlags; void *lpBlob;
} query_set;
_Static_assert(sizeof(query_set) == 120 && __builtin_offsetof(query_set, lpcsaBuffer) == 96, "WSAQUERYSETW");

// A lookup holds its one result set: the name and the addresses, found by WSALookupServiceBegin.
typedef struct lookup {
    struct lookup *next;
    DWORD flags;
    guid service;
    char name[256];
    int count, done;
    struct { int family, type, protocol; uint8_t address[28]; } *entries;
} lookup;
static pthread_mutex_t g_lookup_lock = PTHREAD_MUTEX_INITIALIZER;
static lookup *g_lookups;

static int lookup_error(int gai) {
    switch (gai) {
    case EAI_NONAME: return WSASERVICE_NOT_FOUND;
#ifdef EAI_NODATA
    case EAI_NODATA: return WSANO_DATA;
#endif
    case EAI_AGAIN: return WSATRY_AGAIN;
    case EAI_MEMORY: return WSA_NOT_ENOUGH_MEMORY;
    default: return WSANO_DATA;
    }
}
static WINAPI int WSALookupServiceBeginW(const query_set *query, DWORD flags, HANDLE *handle) {
    if (handle) *handle = NULL;
    if (!query || !handle || query->dwSize < sizeof(query_set) || !query->lpServiceClassId) { vx_set_last_error(WSAEINVAL); return SOCKET_ERROR; }
    if (query->lpNSProviderId && !same_guid(query->lpNSProviderId, &g_dns_provider)) { vx_set_last_error(WSAEINVAL); return SOCKET_ERROR; }
    if (!query->lpNSProviderId && query->dwNameSpace != NS_ALL && query->dwNameSpace != NS_DNS) { vx_set_last_error(WSASERVICE_NOT_FOUND); return SOCKET_ERROR; }
    if (flags & LUP_CONTAINERS) { vx_set_last_error(WSANO_DATA); return SOCKET_ERROR; }   // DNS has no containers
    const guid *service = query->lpServiceClassId;
    int by_name = same_guid(service, &g_svcid_by_name), by_string = same_guid(service, &g_svcid_by_inet_string);
    if (!by_name && !by_string && !same_guid(service, &g_svcid_hostname)) { vx_set_last_error(WSASERVICE_NOT_FOUND); return SOCKET_ERROR; }
    char name[256] = "";
    if (query->lpszServiceInstanceName) vx_utf16_to_utf8(query->lpszServiceInstanceName, vx_wcslen(query->lpszServiceInstanceName) + 1, name, sizeof name);
    name[sizeof name - 1] = 0;
    if (!name[0]) {
        if (by_name || by_string) { vx_set_last_error(WSAEINVAL); return SOCKET_ERROR; }
        gethostname(name, sizeof name - 1);                         // SVCID_HOSTNAME without a name: this computer
    }
    // the protocols asked for (AFPROTOCOLS: family, protocol): TCP over any family when none are
    int want_tcp = !query->dwNumberOfProtocols, want_udp = 0, want4 = !query->dwNumberOfProtocols, want6 = want4;
    for (DWORD i = 0; i < query->dwNumberOfProtocols && query->lpafpProtocols; i++) {
        int family = query->lpafpProtocols[2 * i], protocol = query->lpafpProtocols[2 * i + 1];
        if (family == WIN_AF_INET) want4 = 1; else if (family == WIN_AF_INET6) want6 = 1; else continue;
        if (protocol == 17) want_udp = 1; else want_tcp = 1;
    }
    if (!want4 && !want6) { vx_set_last_error(WSANO_DATA); return SOCKET_ERROR; }
    struct addrinfo hints, *found = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = want4 && want6 ? AF_UNSPEC : want4 ? AF_INET : AF_INET6;
    hints.ai_socktype = SOCK_STREAM;                                // (one entry per address; the protocols are added below)
    hints.ai_flags = AI_CANONNAME | (by_string ? AI_NUMERICHOST : 0);
    int gai = getaddrinfo(name, NULL, &hints, &found);
    if (gai) { vx_set_last_error((DWORD)lookup_error(gai)); return SOCKET_ERROR; }
    lookup *l = calloc(1, sizeof *l);
    int addresses = 0;
    for (struct addrinfo *a = found; a; a = a->ai_next) addresses++;
    if (l) l->entries = calloc((size_t)addresses * 2 + 1, sizeof *l->entries);
    if (!l || !l->entries) { free(l); freeaddrinfo(found); vx_set_last_error(WSA_NOT_ENOUGH_MEMORY); return SOCKET_ERROR; }
    l->flags = flags;
    l->service = *service;
    snprintf(l->name, sizeof l->name, "%s", found && found->ai_canonname ? found->ai_canonname : name);
    for (struct addrinfo *a = found; a; a = a->ai_next) {
        if (a->ai_family != AF_INET && a->ai_family != AF_INET6) continue;
        for (int udp = 0; udp < 2; udp++) {
            if (udp ? !want_udp : !want_tcp) continue;
            int k = l->count++;
            l->entries[k].family = a->ai_family == AF_INET ? WIN_AF_INET : WIN_AF_INET6;
            l->entries[k].type = udp ? 2 : 1;
            l->entries[k].protocol = udp ? 17 : 6;
            // a Windows sockaddr: 16-bit family, then as BSD's (port, address; IPv6 flow info and scope too)
            size_t size = a->ai_family == AF_INET ? 16 : 28;
            memcpy(l->entries[k].address, a->ai_addr, size < a->ai_addrlen ? size : a->ai_addrlen);
            *(uint16_t *)l->entries[k].address = (uint16_t)l->entries[k].family;
        }
    }
    freeaddrinfo(found);
    pthread_mutex_lock(&g_lookup_lock);
    l->next = g_lookups; g_lookups = l;
    pthread_mutex_unlock(&g_lookup_lock);
    *handle = l;
    return 0;
}
static lookup *find_lookup(HANDLE h) {                              // with g_lookup_lock held
    for (lookup *l = g_lookups; l; l = l->next) if (l == h) return l;
    return NULL;
}
// The result set, laid out in the caller's buffer: the WSAQUERYSETW, then the service class and provider GUIDs, the
// name, the CSADDR_INFO array (local and remote SOCKET_ADDRESS, socket type, protocol: 40 bytes) and the addresses.
static WINAPI int WSALookupServiceNextW(HANDLE handle, DWORD flags, DWORD *length, uint8_t *buffer) {
    pthread_mutex_lock(&g_lookup_lock);
    lookup *l = find_lookup(handle);
    if (!l) { pthread_mutex_unlock(&g_lookup_lock); vx_set_last_error(WSA_INVALID_HANDLE); return SOCKET_ERROR; }
    if (!length) { pthread_mutex_unlock(&g_lookup_lock); vx_set_last_error(WSAEFAULT); return SOCKET_ERROR; }
    if (l->done) { pthread_mutex_unlock(&g_lookup_lock); vx_set_last_error(WSA_E_NO_MORE); return SOCKET_ERROR; }
    DWORD returned = l->flags & flags;                              // the Begin's restrictions combined with these (documented)
    size_t name_chars = strlen(l->name) + 1;
    size_t need = sizeof(query_set) + 32;
    size_t name_at = need;
    if (returned & LUP_RETURN_NAME) need += name_chars * 2;
    need = (need + 7) & ~(size_t)7;
    size_t csaddr_at = need;
    if (returned & LUP_RETURN_ADDR) need += (size_t)l->count * (40 + 2 * 32);
    if (!buffer || *length < need) {
        if (flags & LUP_FLUSHPREVIOUS) { l->done = 1; pthread_mutex_unlock(&g_lookup_lock); vx_set_last_error(WSA_E_NO_MORE); return SOCKET_ERROR; }
        *length = (DWORD)need;
        pthread_mutex_unlock(&g_lookup_lock);
        vx_set_last_error(WSAEFAULT);
        return SOCKET_ERROR;
    }
    memset(buffer, 0, need);
    query_set *q = (query_set *)buffer;
    q->dwSize = sizeof(query_set);
    q->dwNameSpace = NS_DNS;
    guid *class_id = (guid *)(buffer + sizeof(query_set)), *provider = class_id + 1;
    *provider = g_dns_provider;
    q->lpNSProviderId = provider;
    if (returned & LUP_RETURN_TYPE) { *class_id = l->service; q->lpServiceClassId = class_id; }
    if (returned & LUP_RETURN_NAME) {
        q->lpszServiceInstanceName = (WCHAR *)(buffer + name_at);
        vx_utf8_to_utf16(l->name, name_chars, q->lpszServiceInstanceName, name_chars);
    }
    if (returned & LUP_RETURN_ADDR) {
        uint8_t *csaddr = buffer + csaddr_at, *addresses = csaddr + (size_t)l->count * 40;
        for (int k = 0; k < l->count; k++) {
            uint8_t *entry = csaddr + 40 * k, *local = addresses + 64 * k, *remote = local + 32;
            int size = l->entries[k].family == WIN_AF_INET ? 16 : 28;
            *(uint16_t *)local = (uint16_t)l->entries[k].family;    // the local side: the family's unspecified address
            memcpy(remote, l->entries[k].address, (size_t)size);
            *(uint8_t **)entry = local; *(int32_t *)(entry + 8) = size;
            *(uint8_t **)(entry + 16) = remote; *(int32_t *)(entry + 24) = size;
            *(int32_t *)(entry + 32) = l->entries[k].type; *(int32_t *)(entry + 36) = l->entries[k].protocol;
        }
        q->dwNumberOfCsAddrs = (DWORD)l->count;
        q->lpcsaBuffer = csaddr;
    }
    l->done = 1;
    pthread_mutex_unlock(&g_lookup_lock);
    return 0;
}
static WINAPI int WSALookupServiceEnd(HANDLE handle) {
    pthread_mutex_lock(&g_lookup_lock);
    lookup **link = &g_lookups;
    while (*link && *link != handle) link = &(*link)->next;
    lookup *l = *link;
    if (l) *link = l->next;
    pthread_mutex_unlock(&g_lookup_lock);
    if (!l) { vx_set_last_error(WSA_INVALID_HANDLE); return SOCKET_ERROR; }
    free(l->entries);
    free(l);
    return 0;
}
// RNRSERVICE_REGISTER (0) and RNRSERVICE_DEREGISTER (1) are no-ops in the DNS name space (documented); there is
// nothing registered to delete (RNRSERVICE_DELETE, 2). A name space without a provider here is an invalid one.
static WINAPI int WSASetServiceW(const query_set *info, int operation, DWORD flags) {
    (void)flags;
    if (!info || info->dwSize < sizeof(query_set) || !info->lpServiceClassId || operation < 0 || operation > 2 ||
        (info->lpNSProviderId ? !same_guid(info->lpNSProviderId, &g_dns_provider)
                              : info->dwNameSpace != NS_ALL && info->dwNameSpace != NS_DNS)) {
        vx_set_last_error(WSAEINVAL);
        return SOCKET_ERROR;
    }
    if (operation == 2) { vx_set_last_error(WSASERVICE_NOT_FOUND); return SOCKET_ERROR; }
    return 0;
}

static const vx_export ws2_32[] = {
    VX_EXPORT(WSAEnumProtocolsW), VX_EXPORT(WSAEnumProtocolsA), VX_EXPORT(WSCEnumProtocols), VX_EXPORT(WSCGetProviderPath),
    VX_EXPORT(WSADuplicateSocketW), VX_EXPORT(WSADuplicateSocketA), VX_EXPORT(WSAEnumNameSpaceProvidersW),
    VX_EXPORT(WSALookupServiceBeginW), VX_EXPORT(WSALookupServiceNextW), VX_EXPORT(WSALookupServiceEnd), VX_EXPORT(WSASetServiceW),
};
void vx_winsock_nsp_init(void) {
    vx_register_module("ws2_32.dll", ws2_32, sizeof ws2_32 / sizeof ws2_32[0]);
}

#ifdef __aarch64__
#include "thunks/winsock_nsp.c.inc"
#endif
