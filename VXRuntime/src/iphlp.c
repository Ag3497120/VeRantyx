// iphlpapi, the rest Steam's Chromium (steamwebhelper) imports: the Mac's interfaces as MIB-II rows (GetIfTable2,
// GetIfEntry2, FreeMibTable), its IPv4 adapters (GetInterfaceInfo), change notifications from the kernel's routing
// socket (NotifyAddrChange, NotifyRouteChange, CancelIPChangeNotify), and DHCP leases, which stay the Mac's own
// (IpReleaseAddress, IpRenewAddress). Every value comes from the system (sysctl NET_RT_IFLIST2, getifaddrs, the
// interface's media status, SystemConfiguration's interface names); what macOS does not know is left zero.
//
// Interface GUIDs and LUIDs follow extras.c's GetAdaptersAddresses, so the same adapter has the same identity in
// both: {7A0B4C2D-3E1F-4A5B-9C8D-<MAC>} and (IfType << 48 | index << 24).
#include "vx.h"
#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_media.h>
#include <net/if_types.h>
#if VX_XROS
// visionOS (VISIONOS_PORT.md): no <net/route.h> and no SCNetworkInterface; interfaces keep their BSD names
#define RTM_IFINFO2 0x12
#define SCNetworkInterfaceCopyAll() ((CFArrayRef)NULL)
#define SCNetworkInterfaceGetLocalizedDisplayName(ni) ((void)(ni), (CFStringRef)NULL)
#define SCNetworkInterfaceGetBSDName(ni) ((void)(ni), (CFStringRef)NULL)
#else
#include <net/route.h>
#endif
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/sysctl.h>
#include <unistd.h>

enum {
    NO_ERROR = 0, ERROR_FILE_NOT_FOUND = 2, ERROR_NOT_ENOUGH_MEMORY = 8, ERROR_NOT_SUPPORTED = 50,
    ERROR_INVALID_PARAMETER = 87, ERROR_INSUFFICIENT_BUFFER = 122, ERROR_NO_DATA = 232,
    ERROR_OPERATION_ABORTED = 995, ERROR_IO_PENDING = 997, ERROR_NOT_FOUND = 1168,
};
#define STATUS_PENDING 0x103

// ---- the Mac's interfaces ---------------------------------------------------------------------------------------------
typedef struct {
    char name[IFNAMSIZ];
    uint32_t index, flags, mac_len, snd_drops;
    uint8_t mac[32];
    struct if_data64 data;
    int has_ipv4;
    int media_valid, media_active, media_type;      // SIOCGIFMEDIA: the link state where the driver reports one
} host_if;

// Every interface the kernel lists (NET_RT_IFLIST2: an RTM_IFINFO2 message, then the interface's link address).
static int host_interfaces(host_if **out) {
    *out = NULL;
    int mib[6] = { CTL_NET, PF_ROUTE, 0, 0, NET_RT_IFLIST2, 0 };
    char *buf = NULL;
    size_t len = 0;
    int read = 0;
    for (int attempt = 0; attempt < 4 && !read; attempt++) {       // (the list can grow between the two calls)
        if (sysctl(mib, 6, NULL, &len, NULL, 0) < 0) break;
        len += 4096;
        free(buf);
        buf = malloc(len);
        if (!buf) break;
        read = !sysctl(mib, 6, buf, &len, NULL, 0);
        if (!read && errno != ENOMEM) break;
    }
    if (!read) { free(buf); return -1; }
    int count = 0;
    for (size_t at = 0; at + sizeof(struct if_msghdr) <= len; ) {
        const struct if_msghdr *m = (const struct if_msghdr *)(buf + at);
        if (!m->ifm_msglen) break;
        if (m->ifm_type == RTM_IFINFO2) count++;
        at += m->ifm_msglen;
    }
    host_if *list = calloc((size_t)count + 1, sizeof *list);
    if (!list) { free(buf); return -1; }
    int n = 0;
    for (size_t at = 0; at + sizeof(struct if_msghdr) <= len && n < count; ) {
        const struct if_msghdr *m = (const struct if_msghdr *)(buf + at);
        if (!m->ifm_msglen) break;
        if (m->ifm_type == RTM_IFINFO2 && m->ifm_msglen >= sizeof(struct if_msghdr2) + sizeof(struct sockaddr_dl) - 12) {
            const struct if_msghdr2 *m2 = (const struct if_msghdr2 *)m;
            const struct sockaddr_dl *dl = (const struct sockaddr_dl *)(m2 + 1);
            host_if *h = &list[n++];
            size_t name_len = dl->sdl_nlen < IFNAMSIZ - 1 ? dl->sdl_nlen : IFNAMSIZ - 1;
            memcpy(h->name, dl->sdl_data, name_len);
            h->index = m2->ifm_index;
            h->flags = (uint32_t)m2->ifm_flags;
            h->data = m2->ifm_data;
            h->snd_drops = (uint32_t)m2->ifm_snd_drops;
            h->mac_len = dl->sdl_alen <= sizeof h->mac ? dl->sdl_alen : 0;
            memcpy(h->mac, LLADDR(dl), h->mac_len);
        }
        at += m->ifm_msglen;
    }
    free(buf);
    struct ifaddrs *addrs = NULL;
    if (!getifaddrs(&addrs)) {
        for (struct ifaddrs *a = addrs; a; a = a->ifa_next)
            if (a->ifa_addr && a->ifa_addr->sa_family == AF_INET)
                for (int i = 0; i < n; i++) if (!strcmp(list[i].name, a->ifa_name)) list[i].has_ipv4 = 1;
        freeifaddrs(addrs);
    }
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    for (int i = 0; s >= 0 && i < n; i++) {
        struct ifmediareq media;
        memset(&media, 0, sizeof media);
        snprintf(media.ifm_name, sizeof media.ifm_name, "%s", list[i].name);
        if (!ioctl(s, SIOCGIFMEDIA, &media) && (media.ifm_status & IFM_AVALID)) {
            list[i].media_valid = 1;
            list[i].media_active = (media.ifm_status & IFM_ACTIVE) != 0;
            list[i].media_type = IFM_TYPE(media.ifm_active ? media.ifm_active : media.ifm_current);
        }
    }
    if (s >= 0) close(s);
    *out = list;
    return n;
}

// The user-visible name of an interface ("Wi-Fi", "Thunderbolt Bridge"), from SystemConfiguration; the BSD name
// ("en0") when it has none.
static void interface_display_name(CFArrayRef sc, const char *bsd, char *out, size_t cap) {
    snprintf(out, cap, "%s", bsd);
    if (!sc) return;
    for (CFIndex i = 0; i < CFArrayGetCount(sc); i++) {
        SCNetworkInterfaceRef ni = (SCNetworkInterfaceRef)CFArrayGetValueAtIndex(sc, i);
        CFStringRef name = SCNetworkInterfaceGetBSDName(ni);
        char bsd_name[IFNAMSIZ];
        if (!name || !CFStringGetCString(name, bsd_name, sizeof bsd_name, kCFStringEncodingUTF8) || strcmp(bsd_name, bsd)) continue;
        CFStringRef display = SCNetworkInterfaceGetLocalizedDisplayName(ni);
        if (display) CFStringGetCString(display, out, (CFIndex)cap, kCFStringEncodingUTF8);
        return;
    }
}

// IANA interface type, as the MIB_IF_ROW2 documentation lists the supported ones; anything else is "other".
static uint32_t windows_if_type(const host_if *h) {
    switch (h->data.ifi_type) {
    case IFT_ETHER: case IFT_BRIDGE: return 6;                      // (macOS types Wi-Fi as Ethernet too)
    case IFT_LOOP: return 24;
    case IFT_PPP: return 23;
    case IFT_IEEE1394: return 144;
    case IFT_GIF: case IFT_STF: return 131;                         // IF_TYPE_TUNNEL
    case IFT_CELLULAR: return 243;                                  // IF_TYPE_WWANPP
    default: return 1;
    }
}
static uint64_t interface_luid(const host_if *h) { return ((uint64_t)windows_if_type(h) << 48) | ((uint64_t)(h->index & 0xffffff) << 24); }
static void interface_guid(const host_if *h, uint8_t guid[16]) {
    static const uint8_t base[10] = { 0x2d, 0x4c, 0x0b, 0x7a, 0x1f, 0x3e, 0x5b, 0x4a, 0x9c, 0x8d };   // {7A0B4C2D-3E1F-4A5B-9C8D-
    memcpy(guid, base, 10);
    if (h->mac_len == 6) memcpy(guid + 10, h->mac, 6);
    else { guid[10] = guid[11] = 0xff; guid[12] = (uint8_t)(h->index >> 24); guid[13] = (uint8_t)(h->index >> 16); guid[14] = (uint8_t)(h->index >> 8); guid[15] = (uint8_t)h->index; }
}

// ---- GetIfTable2, GetIfEntry2, FreeMibTable ----------------------------------------------------------------------------------
// MIB_IF_ROW2 (netioapi.h; 1352 bytes on x64, the layout of the MinGW-w64 header).
typedef struct {
    uint64_t InterfaceLuid;
    uint32_t InterfaceIndex;
    uint8_t InterfaceGuid[16];
    WCHAR Alias[257], Description[257];
    uint32_t PhysicalAddressLength;
    uint8_t PhysicalAddress[32], PermanentPhysicalAddress[32];
    uint32_t Mtu, Type, TunnelType, MediaType, PhysicalMediumType, AccessType, DirectionType;
    uint8_t InterfaceAndOperStatusFlags;
    uint32_t OperStatus, AdminStatus, MediaConnectState;
    uint8_t NetworkGuid[16];
    uint32_t ConnectionType;
    uint64_t TransmitLinkSpeed, ReceiveLinkSpeed;
    uint64_t InOctets, InUcastPkts, InNUcastPkts, InDiscards, InErrors, InUnknownProtos, InUcastOctets, InMulticastOctets, InBroadcastOctets;
    uint64_t OutOctets, OutUcastPkts, OutNUcastPkts, OutDiscards, OutErrors, OutUcastOctets, OutMulticastOctets, OutBroadcastOctets, OutQLen;
} mib_if_row2;
_Static_assert(sizeof(mib_if_row2) == 1352, "MIB_IF_ROW2");
_Static_assert(__builtin_offsetof(mib_if_row2, Mtu) == 1124 && __builtin_offsetof(mib_if_row2, OperStatus) == 1156, "MIB_IF_ROW2");
_Static_assert(__builtin_offsetof(mib_if_row2, ConnectionType) == 1184 && __builtin_offsetof(mib_if_row2, TransmitLinkSpeed) == 1192, "MIB_IF_ROW2");

static void fill_row(mib_if_row2 *r, const host_if *h, CFArrayRef sc) {
    memset(r, 0, sizeof *r);
    uint32_t type = windows_if_type(h);
    r->InterfaceLuid = interface_luid(h);
    r->InterfaceIndex = h->index;
    interface_guid(h, r->InterfaceGuid);
    char display[256];
    interface_display_name(sc, h->name, display, sizeof display);
    vx_utf8_to_utf16(display, strlen(display) + 1, r->Alias, 257);
    r->Alias[256] = 0;
    vx_utf8_to_utf16(h->name, strlen(h->name) + 1, r->Description, 257);
    r->PhysicalAddressLength = h->mac_len;
    memcpy(r->PhysicalAddress, h->mac, h->mac_len);
    memcpy(r->PermanentPhysicalAddress, h->mac, h->mac_len);        // (the burned-in address is not known here)
    r->Mtu = h->data.ifi_mtu;
    r->Type = type;
    r->TunnelType = h->data.ifi_type == IFT_STF ? 11 : type == 131 ? 1 : 0;   // 6to4, other, none
    r->MediaType = type == 24 ? 17 : type == 131 ? 15 : type == 23 ? 3 : type == 144 ? 13 : type == 243 ? 9 : 0;   // NDIS_MEDIUM
    r->PhysicalMediumType = !h->media_valid ? 0 : h->media_type == IFM_IEEE80211 ? 9 : h->media_type == IFM_ETHER ? 14 : 0;
    r->AccessType = (h->flags & IFF_LOOPBACK) ? 1 : (h->flags & IFF_POINTOPOINT) ? 3 : 2;   // loopback, point-to-point, broadcast
    int connected = h->media_valid ? h->media_active : (h->flags & IFF_UP) && (h->flags & IFF_RUNNING);
    int hardware = h->media_valid && !strncmp(h->name, "en", 2);
    r->InterfaceAndOperStatusFlags = (uint8_t)((hardware ? 0x1 | 0x4 : 0) | (connected ? 0 : 0x10));   // hardware, connector, not connected
    r->AdminStatus = (h->flags & IFF_UP) ? 1 : 2;
    r->OperStatus = (h->flags & IFF_UP) && connected ? 1 : 2;
    r->MediaConnectState = connected ? 1 : 2;
    r->ConnectionType = 1;                                          // dedicated
    r->TransmitLinkSpeed = r->ReceiveLinkSpeed = h->data.ifi_baudrate;
    // macOS counts packets and octets per interface, not octets per cast type: those stay zero
    r->InOctets = h->data.ifi_ibytes;
    r->InNUcastPkts = h->data.ifi_imcasts;
    r->InUcastPkts = h->data.ifi_ipackets > h->data.ifi_imcasts ? h->data.ifi_ipackets - h->data.ifi_imcasts : 0;
    r->InDiscards = h->data.ifi_iqdrops;
    r->InErrors = h->data.ifi_ierrors;
    r->InUnknownProtos = h->data.ifi_noproto;
    r->OutOctets = h->data.ifi_obytes;
    r->OutNUcastPkts = h->data.ifi_omcasts;
    r->OutUcastPkts = h->data.ifi_opackets > h->data.ifi_omcasts ? h->data.ifi_opackets - h->data.ifi_omcasts : 0;
    r->OutDiscards = h->snd_drops;
    r->OutErrors = h->data.ifi_oerrors;
}

// The table is one block: NumEntries, padding to 8, the rows. FreeMibTable frees it.
static WINAPI DWORD GetIfTable2(void **table) {
    if (!table) return ERROR_INVALID_PARAMETER;
    *table = NULL;
    host_if *list;
    int n = host_interfaces(&list);
    if (n < 0) return ERROR_NOT_ENOUGH_MEMORY;
    uint8_t *t = vx_heap_zalloc(8 + (size_t)n * sizeof(mib_if_row2));
    if (!t) { free(list); return ERROR_NOT_ENOUGH_MEMORY; }
    CFArrayRef sc = SCNetworkInterfaceCopyAll();
    *(uint32_t *)t = (uint32_t)n;
    for (int i = 0; i < n; i++) fill_row((mib_if_row2 *)(t + 8) + i, &list[i], sc);
    if (sc) CFRelease(sc);
    free(list);
    *table = t;
    return NO_ERROR;
}
// The row of the interface named by InterfaceLuid, or by InterfaceIndex when the LUID is zero.
static WINAPI DWORD GetIfEntry2(mib_if_row2 *row) {
    if (!row || (!row->InterfaceLuid && !row->InterfaceIndex)) return ERROR_INVALID_PARAMETER;
    host_if *list;
    int n = host_interfaces(&list);
    if (n < 0) return ERROR_NOT_ENOUGH_MEMORY;
    DWORD r = ERROR_FILE_NOT_FOUND;
    for (int i = 0; i < n; i++) {
        if (row->InterfaceLuid ? interface_luid(&list[i]) != row->InterfaceLuid : list[i].index != row->InterfaceIndex) continue;
        CFArrayRef sc = SCNetworkInterfaceCopyAll();
        fill_row(row, &list[i], sc);
        if (sc) CFRelease(sc);
        r = NO_ERROR;
        break;
    }
    free(list);
    return r;
}
static WINAPI void FreeMibTable(void *memory) { free(memory); }

// ---- GetInterfaceInfo, IpReleaseAddress, IpRenewAddress --------------------------------------------------------------------
// IP_INTERFACE_INFO: NumAdapters, then IP_ADAPTER_INDEX_MAP { Index; WCHAR Name[128] } (260 bytes) per adapter, at
// least one. The adapters with IPv4 that are up, without the loopback interface (as the documentation says).
enum { ADAPTER_MAP = 260 };
static void adapter_name(const host_if *h, WCHAR *out) {
    uint8_t g[16];
    interface_guid(h, g);
    char text[80];
    snprintf(text, sizeof text, "\\DEVICE\\TCPIP_{%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
             g[3], g[2], g[1], g[0], g[5], g[4], g[7], g[6], g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
    vx_utf8_to_utf16(text, strlen(text) + 1, out, 128);
}
static int ipv4_adapter(const host_if *h) { return h->has_ipv4 && (h->flags & IFF_UP) && !(h->flags & IFF_LOOPBACK); }
static WINAPI DWORD GetInterfaceInfo(uint8_t *table, uint32_t *size) {
    if (!size) return ERROR_INVALID_PARAMETER;
    host_if *list;
    int n = host_interfaces(&list);
    if (n < 0) return ERROR_NOT_ENOUGH_MEMORY;
    int adapters = 0;
    for (int i = 0; i < n; i++) adapters += ipv4_adapter(&list[i]);
    uint32_t need = 4 + ADAPTER_MAP * (uint32_t)(adapters ? adapters : 1);
    if (!table || *size < need) { *size = need; free(list); return ERROR_INSUFFICIENT_BUFFER; }
    memset(table, 0, need);
    *(int32_t *)table = adapters;
    uint8_t *map = table + 4;
    for (int i = 0; i < n; i++) {
        if (!ipv4_adapter(&list[i])) continue;
        *(uint32_t *)map = list[i].index;
        adapter_name(&list[i], (WCHAR *)(map + 4));
        map += ADAPTER_MAP;
    }
    free(list);
    return adapters ? NO_ERROR : ERROR_NO_DATA;
}
// The Mac's DHCP client owns its leases; a Windows program does not release or renew them. An adapter that is not one
// of GetInterfaceInfo's is an invalid parameter, as documented; for one that is, the operation is not supported here.
static DWORD dhcp_lease(const uint8_t *adapter, const char *what) {
    if (!adapter) return ERROR_INVALID_PARAMETER;
    host_if *list;
    int n = host_interfaces(&list);
    if (n < 0) return ERROR_NOT_ENOUGH_MEMORY;
    int known = 0;
    for (int i = 0; i < n && !known; i++) {
        if (!ipv4_adapter(&list[i])) continue;
        WCHAR name[128];
        adapter_name(&list[i], name);
        known = !memcmp(name, adapter + 4, (vx_wcslen(name) + 1) * sizeof(WCHAR));
    }
    free(list);
    if (!known) return ERROR_INVALID_PARAMETER;
    vx_log("iphlpapi: %s of a DHCP lease refused (the Mac's network settings are not changed by Windows programs)", what);
    return ERROR_NOT_SUPPORTED;
}
static WINAPI DWORD IpReleaseAddress(const uint8_t *adapter) { return dhcp_lease(adapter, "release"); }
static WINAPI DWORD IpRenewAddress(const uint8_t *adapter) { return dhcp_lease(adapter, "renewal"); }

// ---- NotifyAddrChange, NotifyRouteChange, CancelIPChangeNotify ---------------------------------------------------------------
// One thread reads the kernel's routing socket (route(4)): an IPv4 address added to or removed from an interface
// completes the pending NotifyAddrChange requests, an IPv4 route added, removed or changed the NotifyRouteChange ones.
// A request is an OVERLAPPED that completes like a file's (its event signalled, GetOverlappedResult answers), or a
// call without one that waits for the next change.
//
// A request's completion (Internal, InternalHigh, its event) is written with g_ip_lock held, by the routing thread and
// by CancelIPChangeNotify alike, in the same step that takes it off the list: a request is either listed (pending) or
// complete, never in between. So when CancelIPChangeNotify finds nothing, the completion is already written and the
// program may free the OVERLAPPED and close its event, as on Windows; and a new request on the same OVERLAPPED cannot
// be completed by the change that ended the one before. (vx_io_completed takes only sync.c's lock and runs no program
// code, so g_ip_lock, then that lock, is safe; notify_change's ResetEvent takes them in the same order.)
enum { VX_HANDLE_IPNOTIFY = 0x4950 };
typedef struct ip_wait { void *ov; int route; struct ip_wait *next; } ip_wait;
static pthread_mutex_t g_ip_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_ip_changed = PTHREAD_COND_INITIALIZER;
static ip_wait *g_ip_waits;
static unsigned g_ip_generation[2];                                 // address changes, route changes
static HANDLE g_ip_file;                                            // the "file handle" the requests are made on
static int g_ip_monitor;                                            // 1 running, -1 the routing socket could not open

#if !VX_XROS   // (visionOS: no routing socket in the SDK, VISIONOS_PORT.md: no change notifications there)
static void ip_changed(int route) {
    pthread_mutex_lock(&g_ip_lock);
    g_ip_generation[route]++;
    for (ip_wait **link = &g_ip_waits; *link; ) {
        ip_wait *w = *link;
        if (w->route != route) { link = &w->next; continue; }
        *link = w->next;
        vx_io_completed(g_ip_file, w->ov, 0, NO_ERROR);           // (still under g_ip_lock: see above)
        free(w);
    }
    pthread_cond_broadcast(&g_ip_changed);
    pthread_mutex_unlock(&g_ip_lock);
}
// A routing message's addresses follow its header, each rounded up to 4 bytes (route(4)); the one of `which` (RTAX_*).
static const struct sockaddr *message_address(const uint8_t *addresses, const uint8_t *end, int present, int which) {
    for (int i = 0; i < RTAX_MAX && addresses < end; i++) {
        if (!(present & (1 << i))) continue;
        const struct sockaddr *sa = (const struct sockaddr *)addresses;
        if (i == which) return addresses + sizeof sa->sa_family + 1 <= end ? sa : NULL;
        addresses += sa->sa_len ? (size_t)(1 + ((sa->sa_len - 1) | 3)) : 4;
    }
    return NULL;
}
static void *ip_monitor(void *arg) {
    int s = (int)(intptr_t)arg;
    uint8_t buf[2048] __attribute__((aligned(8)));
    for (;;) {
        ssize_t n = read(s, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        const struct rt_msghdr *m = (const struct rt_msghdr *)buf;
        if ((size_t)n < sizeof m->rtm_msglen + 2 || m->rtm_version != RTM_VERSION) continue;
        if ((m->rtm_type == RTM_NEWADDR || m->rtm_type == RTM_DELADDR) && (size_t)n >= sizeof(struct ifa_msghdr)) {
            const struct ifa_msghdr *a = (const struct ifa_msghdr *)buf;
            const struct sockaddr *sa = message_address(buf + sizeof *a, buf + n, a->ifam_addrs, RTAX_IFA);
            if (sa && sa->sa_family == AF_INET) ip_changed(0);
        } else if ((m->rtm_type == RTM_ADD || m->rtm_type == RTM_DELETE || m->rtm_type == RTM_CHANGE) && (size_t)n >= sizeof *m &&
                   !(m->rtm_flags & (RTF_LLINFO | RTF_WASCLONED))) {   // (ARP entries and per-destination clones are not routes on Windows)
            const struct sockaddr *sa = message_address(buf + sizeof *m, buf + n, m->rtm_addrs, RTAX_DST);
            if (sa && sa->sa_family == AF_INET) ip_changed(1);
        }
    }
    close(s);
    return NULL;
}
#endif
static int ip_monitor_start(void) {                                 // with g_ip_lock held
    if (g_ip_monitor) return g_ip_monitor > 0;
    if (!g_ip_file) g_ip_file = vx_handle_new(VX_HANDLE_IPNOTIFY, NULL, NULL);
#if VX_XROS
    int s = -1; errno = ENOTSUP;
    void *(*ip_monitor)(void *) = NULL;
#else
    int s = socket(PF_ROUTE, SOCK_RAW, AF_UNSPEC);
#endif
    if (s >= 0) fcntl(s, F_SETFD, FD_CLOEXEC);                     // (not for the programs this one starts)
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (s < 0 || !g_ip_file || pthread_create(&thread, &attr, ip_monitor, (void *)(intptr_t)s)) {
        if (s >= 0) close(s);
        vx_log("iphlpapi: no routing socket (errno %d): change notifications are not available", errno);
        g_ip_monitor = -1;
    } else g_ip_monitor = 1;
    pthread_attr_destroy(&attr);
    return g_ip_monitor > 0;
}
static DWORD notify_change(HANDLE *handle, void *ov, int route) {
    if (!handle != !ov) return ERROR_INVALID_PARAMETER;
    pthread_mutex_lock(&g_ip_lock);
    if (!ip_monitor_start()) { pthread_mutex_unlock(&g_ip_lock); return ERROR_NOT_SUPPORTED; }
    if (!ov) {                                                      // synchronous: until the next change
        unsigned seen = g_ip_generation[route];
        while (g_ip_generation[route] == seen) pthread_cond_wait(&g_ip_changed, &g_ip_lock);
        pthread_mutex_unlock(&g_ip_lock);
        return NO_ERROR;
    }
    for (ip_wait *w = g_ip_waits; w; w = w->next)
        if (w->ov == ov) { pthread_mutex_unlock(&g_ip_lock); return ERROR_INVALID_PARAMETER; }   // already pending
    ip_wait *w = calloc(1, sizeof *w);
    if (!w) { pthread_mutex_unlock(&g_ip_lock); return ERROR_NOT_ENOUGH_MEMORY; }
    // OVERLAPPED: Internal, InternalHigh, offsets, hEvent (+24). Pending until it completes; its event is reset now,
    // as a starting I/O request resets it.
    uint64_t *o = ov;
    __atomic_store_n(&o[0], (uint64_t)STATUS_PENDING, __ATOMIC_RELEASE);
    o[1] = 0;
    HANDLE event = (HANDLE)((uintptr_t)*(HANDLE *)((uint8_t *)ov + 24) & ~(uintptr_t)1);
    if (event) {
        static BOOL (WINAPI *reset)(HANDLE);
        if (!reset) reset = (BOOL (WINAPI *)(HANDLE))vx_find_export("kernel32.dll", "ResetEvent");
        if (reset) reset(event);
    }
    w->ov = ov; w->route = route; w->next = g_ip_waits; g_ip_waits = w;
    *handle = g_ip_file;
    pthread_mutex_unlock(&g_ip_lock);
    return ERROR_IO_PENDING;
}
static WINAPI DWORD NotifyAddrChange(HANDLE *handle, void *overlapped) { return notify_change(handle, overlapped, 0); }
static WINAPI DWORD NotifyRouteChange(HANDLE *handle, void *overlapped) { return notify_change(handle, overlapped, 1); }
// The request ends as a cancelled I/O does: ERROR_OPERATION_ABORTED, its event signalled. One that is not listed has
// already completed (its completion written), or never was a request: ERROR_NOT_FOUND.
static WINAPI BOOL CancelIPChangeNotify(void *overlapped) {
    pthread_mutex_lock(&g_ip_lock);
    ip_wait *found = NULL, **link = &g_ip_waits;
    for (; *link; link = &(*link)->next)
        if ((*link)->ov == overlapped) { found = *link; *link = found->next; break; }
    if (found) vx_io_completed(g_ip_file, overlapped, 0, ERROR_OPERATION_ABORTED);   // (under g_ip_lock: see above)
    pthread_mutex_unlock(&g_ip_lock);
    if (!found) { vx_set_last_error(ERROR_NOT_FOUND); return 0; }
    free(found);
    return 1;
}

static const vx_export iphlpapi[] = {
    VX_EXPORT(GetIfTable2), VX_EXPORT(GetIfEntry2), VX_EXPORT(FreeMibTable), VX_EXPORT(GetInterfaceInfo),
    VX_EXPORT(IpReleaseAddress), VX_EXPORT(IpRenewAddress), VX_EXPORT(NotifyAddrChange), VX_EXPORT(NotifyRouteChange),
    VX_EXPORT(CancelIPChangeNotify),
};
void vx_iphlp_init(void) {
    vx_register_module("iphlpapi.dll", iphlpapi, sizeof iphlpapi / sizeof iphlpapi[0]);
}

#ifdef __aarch64__
#include "thunks/iphlp.c.inc"
#endif
