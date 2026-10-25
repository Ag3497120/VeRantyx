// iphlpapi's GetExtendedTcpTable: the Mac's TCP endpoints with the process that owns each, as Microsoft documents the
// tables (MIB_TCPTABLE, MIB_TCPTABLE_OWNER_PID, MIB_TCPTABLE_OWNER_MODULE and their IPv6 forms).
//
// The Steam client accepts a connection to its WebUI transport only from the process it expects: it looks the
// connection's remote port up in this table and compares the owning process ID. Process IDs under vxrt are the Mac's,
// so the table is built from what macOS reports for each process the user may inspect (libproc): every TCP socket
// once (a socket inherited by several processes is listed with the first that holds it), its addresses and ports in
// network byte order, its state as a MIB_TCP_STATE.
#include <arpa/inet.h>
#include <errno.h>
#include <libproc.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/proc_info.h>
#include <unistd.h>
#include "vx.h"

enum { TABLE_BASIC_LISTENER, TABLE_BASIC_CONNECTIONS, TABLE_BASIC_ALL, TABLE_OWNER_PID_LISTENER, TABLE_OWNER_PID_CONNECTIONS,
       TABLE_OWNER_PID_ALL, TABLE_OWNER_MODULE_LISTENER, TABLE_OWNER_MODULE_CONNECTIONS, TABLE_OWNER_MODULE_ALL };
enum { MIB_TCP_STATE_CLOSED = 1, MIB_TCP_STATE_LISTEN, MIB_TCP_STATE_SYN_SENT, MIB_TCP_STATE_SYN_RCVD, MIB_TCP_STATE_ESTAB,
       MIB_TCP_STATE_FIN_WAIT1, MIB_TCP_STATE_FIN_WAIT2, MIB_TCP_STATE_CLOSE_WAIT, MIB_TCP_STATE_CLOSING, MIB_TCP_STATE_LAST_ACK,
       MIB_TCP_STATE_TIME_WAIT };
#define WIN_AF_INET 2
#define WIN_AF_INET6 23

typedef struct {
    uint64_t so;                                   // the kernel's socket, to list each once
    int v6;
    uint8_t laddr[16], raddr[16];                  // network order (IPv4 in the first 4 bytes)
    uint32_t lscope, rscope;
    uint16_t lport, rport;                         // network order
    uint32_t state, pid;
} tcp_entry;

static uint32_t mib_state(int s) {                 // macOS's TSI_S_* to MIB_TCP_STATE
    switch (s) {
    case TSI_S_LISTEN: return MIB_TCP_STATE_LISTEN;
    case TSI_S_SYN_SENT: return MIB_TCP_STATE_SYN_SENT;
    case TSI_S_SYN_RECEIVED: return MIB_TCP_STATE_SYN_RCVD;
    case TSI_S_ESTABLISHED: return MIB_TCP_STATE_ESTAB;
    case TSI_S__CLOSE_WAIT: return MIB_TCP_STATE_CLOSE_WAIT;
    case TSI_S_FIN_WAIT_1: return MIB_TCP_STATE_FIN_WAIT1;
    case TSI_S_CLOSING: return MIB_TCP_STATE_CLOSING;
    case TSI_S_LAST_ACK: return MIB_TCP_STATE_LAST_ACK;
    case TSI_S_FIN_WAIT_2: return MIB_TCP_STATE_FIN_WAIT2;
    case TSI_S_TIME_WAIT: return MIB_TCP_STATE_TIME_WAIT;
    default: return MIB_TCP_STATE_CLOSED;
    }
}

// The TCP sockets of every process the user may inspect, of one family (AF_INET or AF_INET6 as Windows numbers them).
static tcp_entry *collect(int family, int *count) {
    int n = 0, cap = 64;
    tcp_entry *out = malloc(cap * sizeof *out);
    int npid = proc_listallpids(NULL, 0);
    if (npid <= 0) { *count = 0; return out; }
    pid_t *pids = malloc((size_t)(npid + 64) * sizeof *pids);
    npid = proc_listallpids(pids, (npid + 64) * (int)sizeof *pids);
    for (int p = 0; p < npid; p++) {
        int bytes = proc_pidinfo(pids[p], PROC_PIDLISTFDS, 0, NULL, 0);
        if (bytes <= 0) continue;                                  // (not ours to inspect, or gone)
        struct proc_fdinfo *fds = malloc((size_t)bytes);
        bytes = proc_pidinfo(pids[p], PROC_PIDLISTFDS, 0, fds, bytes);
        for (int f = 0; f < bytes / (int)sizeof *fds; f++) {
            if (fds[f].proc_fdtype != PROX_FDTYPE_SOCKET) continue;
            struct socket_fdinfo si;
            if (proc_pidfdinfo(pids[p], fds[f].proc_fd, PROC_PIDFDSOCKETINFO, &si, sizeof si) != (int)sizeof si) continue;
            if (si.psi.soi_kind != SOCKINFO_TCP) continue;
            const struct in_sockinfo *in = &si.psi.soi_proto.pri_tcp.tcpsi_ini;
            int v4 = (in->insi_vflag & INI_IPV4) != 0;            // (a dual-stack socket used for IPv4 is listed as IPv4)
            if (v4 != (family == WIN_AF_INET)) continue;
            int seen = 0;
            for (int k = 0; k < n && !seen; k++) seen = out[k].so == si.psi.soi_so;
            if (seen) continue;
            if (n == cap) out = realloc(out, (size_t)(cap *= 2) * sizeof *out);
            tcp_entry *e = &out[n++];
            memset(e, 0, sizeof *e);
            e->so = si.psi.soi_so; e->v6 = !v4;
            if (v4) {
                memcpy(e->laddr, &in->insi_laddr.ina_46.i46a_addr4, 4);
                memcpy(e->raddr, &in->insi_faddr.ina_46.i46a_addr4, 4);
            } else {
                memcpy(e->laddr, &in->insi_laddr.ina_6, 16);
                memcpy(e->raddr, &in->insi_faddr.ina_6, 16);
                // (a link-local address carries its interface in the second 16-bit word inside the kernel)
                if (e->laddr[0] == 0xfe && (e->laddr[1] & 0xc0) == 0x80) { e->lscope = (uint32_t)(e->laddr[2] << 8 | e->laddr[3]); e->laddr[2] = e->laddr[3] = 0; }
                if (e->raddr[0] == 0xfe && (e->raddr[1] & 0xc0) == 0x80) { e->rscope = (uint32_t)(e->raddr[2] << 8 | e->raddr[3]); e->raddr[2] = e->raddr[3] = 0; }
            }
            e->lport = (uint16_t)in->insi_lport;                   // (already network order)
            e->rport = (uint16_t)in->insi_fport;
            e->state = mib_state(si.psi.soi_proto.pri_tcp.tcpsi_state);
            e->pid = (uint32_t)pids[p];
        }
        free(fds);
    }
    free(pids);
    *count = n;
    return out;
}

// bOrder: by local address, local port, remote address, remote port (each as a number, so in host order).
static int compare(const void *a, const void *b) {
    const tcp_entry *x = a, *y = b;
    int c = memcmp(x->laddr, y->laddr, 16);                       // (network order compares as the number)
    if (c) return c;
    int lp = (int)ntohs(x->lport) - (int)ntohs(y->lport);
    if (lp) return lp;
    c = memcmp(x->raddr, y->raddr, 16);
    if (c) return c;
    return (int)ntohs(x->rport) - (int)ntohs(y->rport);
}

// Fills `table` (rows of the class's layout after the entry count) with the entries the class asks for.
DWORD vx_tcp_table(void *table, DWORD *size, BOOL order, DWORD family, int cls) {
    if (!size || (family != WIN_AF_INET && family != WIN_AF_INET6) || cls < TABLE_BASIC_LISTENER || cls > TABLE_OWNER_MODULE_ALL)
        return 87;                                                // ERROR_INVALID_PARAMETER
    int v6 = family == WIN_AF_INET6;
    if (v6 && cls <= TABLE_BASIC_ALL) return 87;                  // (no basic table for IPv6: "not supported" per GetExtendedTcpTable)
    int kind = cls % 3;                                           // 0 listeners, 1 connections, 2 all
    int owner = cls >= TABLE_OWNER_PID_LISTENER, module = cls >= TABLE_OWNER_MODULE_LISTENER;
    size_t row = !v6 ? (module ? 160 : owner ? 24 : 20) : (module ? 192 : 56);
    size_t head = module ? 8 : 4;                                 // (the module rows hold 64-bit fields)
    int n = 0;
    tcp_entry *all = collect((int)family, &n), *rows = malloc((size_t)(n + 1) * sizeof *rows);
    int m = 0;
    for (int i = 0; i < n; i++) {
        int listening = all[i].state == MIB_TCP_STATE_LISTEN;
        if (kind == 0 && !listening) continue;
        if (kind == 1 && listening) continue;
        rows[m++] = all[i];
    }
    free(all);
    if (order) qsort(rows, (size_t)m, sizeof *rows, compare);
    DWORD need = (DWORD)(head + (size_t)m * row);
    if (!table || *size < need) { *size = need; free(rows); return 122; }   // ERROR_INSUFFICIENT_BUFFER
    uint8_t *t = table;
    memset(t, 0, need);
    *(DWORD *)t = (DWORD)m;
    for (int i = 0; i < m; i++) {
        uint8_t *r = t + head + (size_t)i * row;
        const tcp_entry *e = &rows[i];
        DWORD lport = e->lport, rport = e->rport;                 // (the port in the low 16 bits, network order)
        if (!v6) {
            DWORD v[6] = { e->state, 0, lport, 0, rport, e->pid };
            memcpy(&v[1], e->laddr, 4);
            memcpy(&v[3], e->raddr, 4);
            memcpy(r, v, owner ? 24 : 20);
        } else {
            memcpy(r, e->laddr, 16); memcpy(r + 16, &e->lscope, 4); memcpy(r + 20, &lport, 4);
            memcpy(r + 24, e->raddr, 16); memcpy(r + 40, &e->rscope, 4); memcpy(r + 44, &rport, 4);
            memcpy(r + 48, &e->state, 4); memcpy(r + 52, &e->pid, 4);
        }
        // (module rows: liCreateTimestamp and OwningModuleInfo stay zero - GetOwnerModuleFromTcpEntry is not offered)
    }
    *size = need;
    free(rows);
    return 0;
}

static WINAPI DWORD GetExtendedTcpTable(void *table, DWORD *size, BOOL order, DWORD family, int cls, DWORD reserved) {
    (void)reserved;
    return vx_tcp_table(table, size, order, family, cls);
}
// GetTcpTable and GetTcp6Table: the basic IPv4 table, and the IPv6 one with its state first (MIB_TCP6ROW).
static WINAPI DWORD GetTcpTable(void *table, DWORD *size, BOOL order) { return vx_tcp_table(table, size, order, WIN_AF_INET, TABLE_BASIC_ALL); }
static WINAPI DWORD GetTcpTable2(void *table, DWORD *size, BOOL order) {
    // MIB_TCPROW2: the owner-PID row followed by dwOffloadState (TcpConnectionOffloadStateInHost = 0).
    DWORD inner = 0, r = vx_tcp_table(NULL, &inner, order, WIN_AF_INET, TABLE_OWNER_PID_ALL);
    if (r != 122) return r;
    uint8_t *tmp = malloc(inner);
    r = vx_tcp_table(tmp, &inner, order, WIN_AF_INET, TABLE_OWNER_PID_ALL);
    if (r) { free(tmp); return r; }
    DWORD n = *(DWORD *)tmp, need = 4 + n * 28;
    if (!size) { free(tmp); return 87; }
    if (!table || *size < need) { *size = need; free(tmp); return 122; }
    uint8_t *t = table;
    memset(t, 0, need);
    *(DWORD *)t = n;
    for (DWORD i = 0; i < n; i++) memcpy(t + 4 + i * 28, tmp + 4 + i * 24, 24);
    *size = need;
    free(tmp);
    return 0;
}

static const vx_export iphlpapi[] = { VX_EXPORT(GetExtendedTcpTable), VX_EXPORT(GetTcpTable), VX_EXPORT(GetTcpTable2) };
void vx_tcptable_init(void) { vx_register_module("iphlpapi.dll", iphlpapi, sizeof iphlpapi / sizeof iphlpapi[0]); }

#ifdef __aarch64__
#include "thunks/tcptable.c.inc"
#endif
