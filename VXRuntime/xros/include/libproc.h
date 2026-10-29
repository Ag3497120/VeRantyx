// visionOS (xros/README.md): an app is one process and may not look at others, and the SDK has no <libproc.h>. These
// stand-ins answer as if there were nothing to see: no other processes, no file descriptor or socket details, no
// resource usage. The structures carry only the fields the runtime reads.
#pragma once
#include <stdint.h>
#include <sys/types.h>
#include <sys/resource.h>
#include <errno.h>
#define PROC_PIDTBSDINFO 3
#define PROC_PIDLISTFDS 1
#define PROC_PIDFDSOCKETINFO 3
#define PROX_FDTYPE_SOCKET 2
#define PROC_PIDPATHINFO_MAXSIZE 4096
struct proc_bsdinfo { uint32_t pbi_flags, pbi_status, pbi_xstatus, pbi_pid, pbi_ppid; uint64_t pbi_start_tvsec, pbi_start_tvusec; char pbi_comm[16], pbi_name[32]; };
struct proc_fdinfo { int32_t proc_fd; uint32_t proc_fdtype; };
struct in_sockinfo { int insi_fport, insi_lport; uint64_t insi_gencnt; uint32_t insi_flags, insi_flow; uint8_t insi_vflag, insi_ip_ttl; uint32_t rfu_1;
    union { struct { uint32_t i46a_pad32[3]; uint32_t i46a_addr4; } ina_46; uint8_t ina_6[16]; } insi_faddr, insi_laddr; };
struct tcp_sockinfo { struct in_sockinfo tcpsi_ini; int tcpsi_state; };
struct socket_fdinfo { struct { struct { uint64_t so_; } soi_so_pad; uint64_t soi_so; int soi_kind; int soi_family, soi_protocol, soi_type;
    union { struct in_sockinfo pri_in; struct tcp_sockinfo pri_tcp; } soi_proto; } psi; };
enum { SOCKINFO_IN = 1, SOCKINFO_TCP = 2 };
#define INI_IPV4 0x1
#define INI_IPV6 0x2
static inline int proc_listallpids(void *buf, int size) { (void)buf; (void)size; return 0; }
static inline int proc_pidinfo(int pid, int flavor, uint64_t arg, void *buf, int size) { (void)pid; (void)flavor; (void)arg; (void)buf; (void)size; errno = ESRCH; return 0; }
static inline int proc_pidfdinfo(int pid, int fd, int flavor, void *buf, int size) { (void)pid; (void)fd; (void)flavor; (void)buf; (void)size; errno = ESRCH; return 0; }
static inline int proc_pidpath(int pid, void *buf, uint32_t size) { (void)pid; if (size) ((char *)buf)[0] = 0; errno = ESRCH; return 0; }
static inline int proc_name(int pid, void *buf, uint32_t size) { (void)pid; if (size) ((char *)buf)[0] = 0; return 0; }
static inline int proc_pid_rusage(int pid, int flavor, void *buf) { (void)pid; (void)flavor; (void)buf; errno = ENOTSUP; return -1; }
enum { TSI_S_CLOSED, TSI_S_LISTEN, TSI_S_SYN_SENT, TSI_S_SYN_RECEIVED, TSI_S_ESTABLISHED, TSI_S__CLOSE_WAIT, TSI_S_FIN_WAIT_1,
       TSI_S_CLOSING, TSI_S_LAST_ACK, TSI_S_FIN_WAIT_2, TSI_S_TIME_WAIT };
