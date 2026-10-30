// steamclient64.dll on the Mac Steam client: Windows games talk to Steam through steam_api64.dll, which loads
// steamclient64.dll from the running Steam and asks it for interfaces (CreateInterface) and callbacks
// (Steam_BGetCallback). Here that DLL is built in and forwards to the Mac client's own steamclient.dylib (its
// x86-64 slice runs in this process like the rest of the runtime), so a game uses the Steam session the user is
// signed in to on the Mac: ownership, the user, networking and all are Steam's own answers.
//
// The interfaces are C++ objects. Windows code calls their methods with Microsoft's convention (ms_abi) through
// MSVC vtables; the Mac objects expect the System V convention. Each interface handed to Windows is a proxy whose
// vtable holds static stubs, one per method index, that call the same index on the Mac object. Arguments are
// passed through as integers (most Steam methods take integers and pointers only); methods that return a class by
// value (CSteamID), take floats, or take Windows function pointers are listed and converted. Interfaces returned
// for a version string ("SteamUser023", ...) are wrapped in turn.
//
// Opt-in: VXRT_STEAM_BRIDGE=1 (with Steam running on the Mac). VXRT_TRACE_STEAM=1 logs every method the first
// time it is called. VXRT_STEAM_CLIENT=path picks another steamclient.dylib.
#include "vx.h"
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SYSV __attribute__((sysv_abi))
typedef uint64_t u64;
typedef SYSV u64 (*sysv_fn)(u64, u64, u64, u64, u64, u64, u64, u64, u64, u64);

static void *g_lib;
static int g_trace;
int vx_steambridge_enabled(void);

static void *mac_lib(void) {
    static int tried;
    if (tried) return g_lib;
    tried = 1;
    const char *path = getenv("VXRT_STEAM_CLIENT");
    char buf[1024];
    if (!path) {
        snprintf(buf, sizeof buf, "%s/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/steamclient.dylib", getenv("HOME") ? getenv("HOME") : "");
        path = buf;
    }
    struct sigaction before[32];
    for (int i = 1; i < 32; i++) sigaction(i, NULL, &before[i]);
    g_lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    for (int i = 1; i < 32; i++) if (i != SIGKILL && i != SIGSTOP) {   // the client's own crash handlers would take the runtime's signals
        struct sigaction now;
        sigaction(i, NULL, &now);
        if (now.sa_sigaction != before[i].sa_sigaction) { vx_log("Steam bridge: steamclient replaced the handler of signal %d; restored", i); sigaction(i, &before[i], NULL); }
    }
    if (!g_lib) vx_log("Steam bridge: cannot load %s, so the program runs without Steam (its sign-in through Steam fails): %s", path, dlerror());
    else if (g_trace) vx_log("Steam bridge: loaded %s", path);
    return g_lib;
}

// Reads memory that may not be mapped (an argument that is perhaps a pointer), without faulting.
static int peek(const void *p, void *out, size_t n) {
    mach_vm_size_t got = 0;
    return (uintptr_t)p >= 0x10000 && mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(uintptr_t)p, n, (mach_vm_address_t)(uintptr_t)out, &got) == KERN_SUCCESS && got == n;
}
// An interface version string ("SteamUser023", "STEAMAPPS_INTERFACE_VERSION008"), or NULL.
static const char *version_string(u64 arg, char *out, size_t cap) {
    if (!peek((const void *)arg, out, cap - 1)) {
        size_t n = 0;                                   // near the end of a mapping: byte by byte
        while (n < cap - 1 && peek((const char *)arg + n, out + n, 1) && out[n]) n++;
        if (n == 0) return NULL;
    }
    out[cap - 1] = 0;
    size_t len = strnlen(out, cap - 1);
    if (len < 8 || len > 60 || out[len] != 0) return NULL;
    if (strncmp(out, "Steam", 5) && strncmp(out, "STEAM", 5)) return NULL;
    if (out[len - 1] < '0' || out[len - 1] > '9' || out[len - 2] < '0' || out[len - 2] > '9') return NULL;
    for (size_t i = 0; i < len; i++) if (!(out[i] == '_' || (out[i] >= '0' && out[i] <= '9') || ((out[i] | 32) >= 'a' && (out[i] | 32) <= 'z'))) return NULL;
    return out;
}

// ---- proxies --------------------------------------------------------------------------------------------------
#define METHODS 192
typedef struct { void **vtbl; void *mac; char name[56]; uint8_t called[METHODS]; } proxy;
static void *g_vtbl[METHODS];
static proxy *g_proxies[512];
static int g_nproxies;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static proxy *wrap(void *mac, const char *name) {
    if (!mac) return NULL;
    pthread_mutex_lock(&g_lock);
    proxy *p = NULL;
    for (int i = 0; i < g_nproxies && !p; i++) if (g_proxies[i]->mac == mac && !strcmp(g_proxies[i]->name, name)) p = g_proxies[i];
    if (!p && g_nproxies < 512) {
        p = calloc(1, sizeof *p);
        p->vtbl = g_vtbl; p->mac = mac;
        snprintf(p->name, sizeof p->name, "%s", name);
        g_proxies[g_nproxies++] = p;
        if (g_trace) vx_log("Steam bridge: interface %s", name);
    }
    pthread_mutex_unlock(&g_lock);
    return p;
}

// Windows function pointers handed to the Mac client (warning hooks, "is this callback registered" checks) go
// through these System V trampolines.
// The client may call them on threads of its own: those get a TEB, and the Windows code runs on a 2 MB stack (the
// client's threads have 512 KB, less than Windows code expects).
typedef WINAPI u64 (*ms_fn)(u64, u64, u64, u64);
static ms_fn g_tramp_target[8];
uint64_t vx_call_on_stack(uint64_t (*fn)(void *), void *arg, void *stack_top);   // context.S
typedef struct { ms_fn fn; u64 a, b, c, d; } windows_call;
static uint64_t run_call(void *p) { windows_call *w = p; return w->fn(w->a, w->b, w->c, w->d); }
static u64 call_windows(ms_fn fn, u64 a, u64 b, u64 c, u64 d) {
#ifdef __aarch64__
    return fn(a, b, c, d);                             // (ARM64: any thread gets an x86 stack and TEB, src/arm64.c)
#endif
    uint8_t *teb = NULL;
#ifndef __aarch64__
    __asm__ volatile("movq %%gs:0x30, %0" : "=r"(teb));
#endif
    if (teb) return fn(a, b, c, d);                    // a thread of the program
    static __thread uint8_t *stack;
    if (!stack) {
        vx_teb();                                      // made on first use
        stack = malloc(2 << 20);
    }
    windows_call w = { fn, a, b, c, d };
    return vx_call_on_stack(run_call, &w, stack + (2 << 20));
}
#define TRAMP(i) static SYSV u64 tramp##i(u64 a, u64 b, u64 c, u64 d) { return call_windows(g_tramp_target[i], a, b, c, d); }
TRAMP(0) TRAMP(1) TRAMP(2) TRAMP(3) TRAMP(4) TRAMP(5) TRAMP(6) TRAMP(7)
static void *const g_tramps[8] = { (void *)tramp0, (void *)tramp1, (void *)tramp2, (void *)tramp3, (void *)tramp4, (void *)tramp5, (void *)tramp6, (void *)tramp7 };
static u64 sysv_callback(u64 fn) {
    pthread_mutex_lock(&g_lock);
    int i = 0;
    while (i < 8 && g_tramp_target[i] && (u64)(uintptr_t)g_tramp_target[i] != fn) i++;
    if (i < 8) g_tramp_target[i] = (ms_fn)(uintptr_t)fn;
    pthread_mutex_unlock(&g_lock);
    if (i == 8) { vx_log("Steam bridge: too many callbacks"); return 0; }
    return (u64)(uintptr_t)g_tramps[i];
}
// The start of a function in a loaded Windows image (its .pdata says so): a function pointer handed to the
// client. Other code addresses among the "arguments" are return addresses left on the stack past the real ones.
static int in_windows_code(u64 v) {
    const vx_image *m = vx_image_at(v);
    if (!m || !m->pdata_size) return 0;
    const uint32_t *fn = (const uint32_t *)(m->base + m->pdata_rva);
    size_t lo = 0, hi = m->pdata_size / 12;
    uint32_t rva = (uint32_t)(v - (u64)(uintptr_t)m->base);
    while (lo < hi) { size_t mid = (lo + hi) / 2; if (fn[3 * mid] < rva) lo = mid + 1; else hi = mid; }
    return lo < m->pdata_size / 12 && fn[3 * lo] == rva;      // (a .pdata entry begins only in code)
}

// Methods that need more than passing integers through, by interface (version prefix) and vtable index.
enum { PLAIN, RET_CLASS8 /* returns an 8-byte class (CSteamID): MSVC passes a hidden result pointer */ };
static const struct { const char *iface; int index, kind; } g_special[] = {
    { "SteamUser0", 2, RET_CLASS8 },              // GetSteamID
};
static int special(const proxy *p, int k) {
    for (size_t i = 0; i < sizeof g_special / sizeof g_special[0]; i++)
        if (g_special[i].index == k && !strncmp(p->name, g_special[i].iface, strlen(g_special[i].iface))) return g_special[i].kind;
    return PLAIN;
}

// SteamNetworkingMessage_t carries the client's functions: m_pfnFreeData at 176 and m_pfnRelease at 184, which
// Windows code calls (msg->Release()). Messages handed to Windows get a Windows-callable release; messages handed
// back (SendMessages) get the client's again, and a Windows m_pfnFreeData becomes a trampoline.
#define MSG_FREE 176
#define MSG_RELEASE 184
static void *g_mac_release;
static WINAPI void release_message(uint8_t *msg) { ((SYSV void (*)(void *))g_mac_release)(msg); }
static void message_to_windows(uint8_t *msg) {
    void **rel = (void **)(msg + MSG_RELEASE);
    if (!msg || *rel == (void *)release_message) return;
    if (g_mac_release && *rel != g_mac_release) vx_log("Steam bridge: a message with another release function %p", *rel);
    g_mac_release = *rel;
    *rel = (void *)release_message;
}
static void message_to_mac(uint8_t *msg) {
    if (!msg) return;
    void **rel = (void **)(msg + MSG_RELEASE), **free_fn = (void **)(msg + MSG_FREE);
    if (*rel == (void *)release_message) *rel = g_mac_release;
    if (*free_fn && in_windows_code((u64)(uintptr_t)*free_fn)) *free_fn = (void *)(uintptr_t)sysv_callback((u64)(uintptr_t)*free_fn);
}

static u64 call_method(proxy *p, int k, u64 *a) {
    void **mvt = *(void ***)p->mac;
    sysv_fn fn = (sysv_fn)mvt[k];
    int kind = special(p, k);
    int first = !p->called[k];
    p->called[k] = 1;
    // Windows code pointers among the arguments become System V trampolines.
    u64 args[9];
    memcpy(args, a, sizeof args);
    for (int i = 0; i < 9; i++) if (in_windows_code(args[i])) {
        if (g_trace) vx_log("Steam bridge: %s method %d argument %d is Windows code %#llx", p->name, k, i, (unsigned long long)args[i]);
        args[i] = sysv_callback(args[i]);
    }
    int sockets = !strncmp(p->name, "SteamNetworkingSockets", 22), utils = !strncmp(p->name, "SteamNetworkingUtils", 20);
    if (sockets && k == 12)                            // SendMessages(count, messages, results)
        for (int i = 0; i < (int)a[0]; i++) message_to_mac(((uint8_t **)(uintptr_t)a[1])[i]);
    u64 r;
    if (kind == RET_CLASS8) {
        u64 v = fn((u64)(uintptr_t)p->mac, args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], 0);
        memcpy((void *)(uintptr_t)a[0], &v, 8);
        r = a[0];
    } else r = fn((u64)(uintptr_t)p->mac, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8]);
    if (sockets && (k == 14 || k == 27))               // ReceiveMessagesOn{Connection,PollGroup}(handle, messages, max)
        for (int i = 0; i < (int)r; i++) message_to_windows(((uint8_t **)(uintptr_t)a[1])[i]);
    if (utils && k == 0 && r) message_to_windows((uint8_t *)(uintptr_t)r);   // AllocateMessage
    // ISteamClient's GetISteam...(user, pipe, version) and GetISteamUtils(pipe, version): the result is that
    // interface. (Only the arguments these take are looked at: registers past them hold leftovers.)
    char ver[64];
    const char *name = NULL;
    if (!strncmp(p->name, "SteamClient", 11))
        for (int i = 1; i < 3 && !name && r; i++) name = version_string(a[i], ver, sizeof ver);
    if (name && r) r = (u64)(uintptr_t)wrap((void *)(uintptr_t)r, name);
    // A program whose Steam sign-in fails says little about why (the Frontier launcher shows "this Frontier account does
    // not own Elite Dangerous", issue #10): no pipe or no user from the Mac client is said in the log, always.
    if (!strncmp(p->name, "SteamClient", 11) && !r && (k == 0 || k == 2)) {
        static int said[3];
        if (!said[k]++) vx_log("Steam bridge: Mac Steam %s; the program runs without Steam (its sign-in through Steam fails)",
                              k == 0 ? "is not reachable (CreateSteamPipe failed: is Steam for Mac running?)"
                                     : "has no signed-in user (ConnectToGlobalUser failed: is Steam for Mac signed in, and not still starting?)");
    }
    if (g_trace > 2) { Dl_info di; if (dladdr((void *)fn, &di)) vx_log("Steam bridge: %s method %d is %s+%#lx", p->name, k, di.dli_fname, (unsigned long)((char *)fn - (char *)di.dli_fbase)); }
    if (g_trace && (first || g_trace > 1)) vx_log("Steam bridge: %s method %d (%#llx, %#llx, %#llx, %#llx) = %#llx%s%s", p->name, k,
                                 (unsigned long long)a[0], (unsigned long long)a[1], (unsigned long long)a[2], (unsigned long long)a[3],
                                 (unsigned long long)r, name ? " -> " : "", name ? name : "");
    return r;
}
#define STUB(k) static WINAPI u64 stub##k(proxy *p, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7, u64 a8, u64 a9) { \
    u64 a[9] = { a1, a2, a3, a4, a5, a6, a7, a8, a9 }; return call_method(p, k, a); }
STUB(0) STUB(1) STUB(2) STUB(3) STUB(4) STUB(5) STUB(6) STUB(7)
STUB(8) STUB(9) STUB(10) STUB(11) STUB(12) STUB(13) STUB(14) STUB(15)
STUB(16) STUB(17) STUB(18) STUB(19) STUB(20) STUB(21) STUB(22) STUB(23)
STUB(24) STUB(25) STUB(26) STUB(27) STUB(28) STUB(29) STUB(30) STUB(31)
STUB(32) STUB(33) STUB(34) STUB(35) STUB(36) STUB(37) STUB(38) STUB(39)
STUB(40) STUB(41) STUB(42) STUB(43) STUB(44) STUB(45) STUB(46) STUB(47)
STUB(48) STUB(49) STUB(50) STUB(51) STUB(52) STUB(53) STUB(54) STUB(55)
STUB(56) STUB(57) STUB(58) STUB(59) STUB(60) STUB(61) STUB(62) STUB(63)
STUB(64) STUB(65) STUB(66) STUB(67) STUB(68) STUB(69) STUB(70) STUB(71)
STUB(72) STUB(73) STUB(74) STUB(75) STUB(76) STUB(77) STUB(78) STUB(79)
STUB(80) STUB(81) STUB(82) STUB(83) STUB(84) STUB(85) STUB(86) STUB(87)
STUB(88) STUB(89) STUB(90) STUB(91) STUB(92) STUB(93) STUB(94) STUB(95)
STUB(96) STUB(97) STUB(98) STUB(99) STUB(100) STUB(101) STUB(102) STUB(103)
STUB(104) STUB(105) STUB(106) STUB(107) STUB(108) STUB(109) STUB(110) STUB(111)
STUB(112) STUB(113) STUB(114) STUB(115) STUB(116) STUB(117) STUB(118) STUB(119)
STUB(120) STUB(121) STUB(122) STUB(123) STUB(124) STUB(125) STUB(126) STUB(127)
STUB(128) STUB(129) STUB(130) STUB(131) STUB(132) STUB(133) STUB(134) STUB(135)
STUB(136) STUB(137) STUB(138) STUB(139) STUB(140) STUB(141) STUB(142) STUB(143)
STUB(144) STUB(145) STUB(146) STUB(147) STUB(148) STUB(149) STUB(150) STUB(151)
STUB(152) STUB(153) STUB(154) STUB(155) STUB(156) STUB(157) STUB(158) STUB(159)
STUB(160) STUB(161) STUB(162) STUB(163) STUB(164) STUB(165) STUB(166) STUB(167)
STUB(168) STUB(169) STUB(170) STUB(171) STUB(172) STUB(173) STUB(174) STUB(175)
STUB(176) STUB(177) STUB(178) STUB(179) STUB(180) STUB(181) STUB(182) STUB(183)
STUB(184) STUB(185) STUB(186) STUB(187) STUB(188) STUB(189) STUB(190) STUB(191)
static void *const g_stubs[METHODS] = {
    (void *)stub0, (void *)stub1, (void *)stub2, (void *)stub3, (void *)stub4, (void *)stub5, (void *)stub6, (void *)stub7,
    (void *)stub8, (void *)stub9, (void *)stub10, (void *)stub11, (void *)stub12, (void *)stub13, (void *)stub14, (void *)stub15,
    (void *)stub16, (void *)stub17, (void *)stub18, (void *)stub19, (void *)stub20, (void *)stub21, (void *)stub22, (void *)stub23,
    (void *)stub24, (void *)stub25, (void *)stub26, (void *)stub27, (void *)stub28, (void *)stub29, (void *)stub30, (void *)stub31,
    (void *)stub32, (void *)stub33, (void *)stub34, (void *)stub35, (void *)stub36, (void *)stub37, (void *)stub38, (void *)stub39,
    (void *)stub40, (void *)stub41, (void *)stub42, (void *)stub43, (void *)stub44, (void *)stub45, (void *)stub46, (void *)stub47,
    (void *)stub48, (void *)stub49, (void *)stub50, (void *)stub51, (void *)stub52, (void *)stub53, (void *)stub54, (void *)stub55,
    (void *)stub56, (void *)stub57, (void *)stub58, (void *)stub59, (void *)stub60, (void *)stub61, (void *)stub62, (void *)stub63,
    (void *)stub64, (void *)stub65, (void *)stub66, (void *)stub67, (void *)stub68, (void *)stub69, (void *)stub70, (void *)stub71,
    (void *)stub72, (void *)stub73, (void *)stub74, (void *)stub75, (void *)stub76, (void *)stub77, (void *)stub78, (void *)stub79,
    (void *)stub80, (void *)stub81, (void *)stub82, (void *)stub83, (void *)stub84, (void *)stub85, (void *)stub86, (void *)stub87,
    (void *)stub88, (void *)stub89, (void *)stub90, (void *)stub91, (void *)stub92, (void *)stub93, (void *)stub94, (void *)stub95,
    (void *)stub96, (void *)stub97, (void *)stub98, (void *)stub99, (void *)stub100, (void *)stub101, (void *)stub102, (void *)stub103,
    (void *)stub104, (void *)stub105, (void *)stub106, (void *)stub107, (void *)stub108, (void *)stub109, (void *)stub110, (void *)stub111,
    (void *)stub112, (void *)stub113, (void *)stub114, (void *)stub115, (void *)stub116, (void *)stub117, (void *)stub118, (void *)stub119,
    (void *)stub120, (void *)stub121, (void *)stub122, (void *)stub123, (void *)stub124, (void *)stub125, (void *)stub126, (void *)stub127,
    (void *)stub128, (void *)stub129, (void *)stub130, (void *)stub131, (void *)stub132, (void *)stub133, (void *)stub134, (void *)stub135,
    (void *)stub136, (void *)stub137, (void *)stub138, (void *)stub139, (void *)stub140, (void *)stub141, (void *)stub142, (void *)stub143,
    (void *)stub144, (void *)stub145, (void *)stub146, (void *)stub147, (void *)stub148, (void *)stub149, (void *)stub150, (void *)stub151,
    (void *)stub152, (void *)stub153, (void *)stub154, (void *)stub155, (void *)stub156, (void *)stub157, (void *)stub158, (void *)stub159,
    (void *)stub160, (void *)stub161, (void *)stub162, (void *)stub163, (void *)stub164, (void *)stub165, (void *)stub166, (void *)stub167,
    (void *)stub168, (void *)stub169, (void *)stub170, (void *)stub171, (void *)stub172, (void *)stub173, (void *)stub174, (void *)stub175,
    (void *)stub176, (void *)stub177, (void *)stub178, (void *)stub179, (void *)stub180, (void *)stub181, (void *)stub182, (void *)stub183,
    (void *)stub184, (void *)stub185, (void *)stub186, (void *)stub187, (void *)stub188, (void *)stub189, (void *)stub190, (void *)stub191,
};

// ---- the DLL's exports ----------------------------------------------------------------------------------------
static WINAPI void *CreateInterface(const char *version, int *code) {
    void *lib = mac_lib();
    SYSV void *(*create)(const char *, int *) = lib ? dlsym(lib, "CreateInterface") : NULL;
    void *mac = create ? create(version, code) : NULL;
    if (g_trace) vx_log("Steam bridge: CreateInterface(%s) = %p", version, mac);
    return mac ? wrap(mac, version) : NULL;
}
// The flat functions take integers and pointers only.
static void trace_flat(const char *name, int *count, u64 a, u64 b, u64 r) {
    if (!g_trace || ++*count > 3) return;
    char s[64];
    const char *text = version_string(a, s, sizeof s);
    vx_log("Steam bridge: %s(%#llx%s%s%s, %#llx) = %#llx", name, (unsigned long long)a, text ? " \"" : "", text ? text : "", text ? "\"" : "",
           (unsigned long long)b, (unsigned long long)r);
}
#define FLAT(name) static WINAPI u64 name(u64 a, u64 b, u64 c, u64 d, u64 e, u64 f, u64 g, u64 h) { \
    static sysv_fn fn; static int count; if (!fn && mac_lib()) fn = (sysv_fn)dlsym(g_lib, #name); \
    u64 r = fn ? fn(a, b, c, d, e, f, g, h, 0, 0) : 0; trace_flat(#name, &count, a, b, r); return r; }
FLAT(Steam_BConnected) FLAT(Steam_BLoggedOn) FLAT(Steam_BReleaseSteamPipe) FLAT(Steam_ConnectToGlobalUser)
FLAT(Steam_CreateGlobalUser) FLAT(Steam_CreateLocalUser) FLAT(Steam_CreateSteamPipe) FLAT(Steam_FreeLastCallback) FLAT(Steam_GSBLoggedOn)
FLAT(Steam_GSBSecure) FLAT(Steam_GSGetSteam2GetEncryptionKeyToSendToNewClient) FLAT(Steam_GSGetSteamID) FLAT(Steam_GSLogOff) FLAT(Steam_GSLogOn)
FLAT(Steam_GSRemoveUserConnect) FLAT(Steam_GSSendSteam2UserConnect) FLAT(Steam_GSSendSteam3UserConnect) FLAT(Steam_GSSendUserDisconnect)
FLAT(Steam_GSSendUserStatusResponse) FLAT(Steam_GSSetServerType) FLAT(Steam_GSSetSpawnCount) FLAT(Steam_GSUpdateStatus) FLAT(Steam_GetAPICallResult)
FLAT(Steam_GetGSHandle) FLAT(Steam_InitiateGameConnection) FLAT(Steam_LogOff) FLAT(Steam_LogOn)
FLAT(Steam_ReleaseThreadLocalMemory) FLAT(Steam_ReleaseUser) FLAT(Steam_SetLocalIPBinding)
FLAT(Steam_TerminateGameConnection)
// Callbacks whose layout differs: the client packs structures to 4 bytes, Windows to 8, so a structure with an
// 8-byte member after an odd number of 4-byte ones moves. CallbackMsg_t itself: user, id, data pointer, size.
typedef struct { int user, id; uint8_t *data; int size; } callback_msg;
static __thread uint8_t t_converted[4096];
static WINAPI BOOL Steam_BGetCallback(int pipe, callback_msg *msg) {
    static sysv_fn fn;
    if (!fn && mac_lib()) fn = (sysv_fn)dlsym(g_lib, "Steam_BGetCallback");
    BOOL got = fn ? (BOOL)fn((u64)pipe, (u64)(uintptr_t)msg, 0, 0, 0, 0, 0, 0, 0, 0) : 0;
    if (!got) return 0;
    if (msg->id == 1221 && msg->size >= 4 + 696 && msg->size <= 2048) {   // SteamNetConnectionStatusChangedCallback_t
        memset(t_converted, 0, 712);
        memcpy(t_converted, msg->data, 4);              // m_hConn
        memcpy(t_converted + 8, msg->data + 4, 696);    // m_info: 8-aligned on Windows
        memcpy(t_converted + 704, msg->data + 700, 4);  // m_eOldState
        msg->data = t_converted; msg->size = 712;
    }
    if (g_trace) { static uint8_t seen[8192]; if (msg->id >= 0 && msg->id < 8192 * 8 && !(seen[msg->id >> 3] & (1 << (msg->id & 7)))) {
        seen[msg->id >> 3] |= (uint8_t)(1 << (msg->id & 7)); vx_log("Steam bridge: callback %d (%d bytes)", msg->id, msg->size); } }
    return 1;
}
// The Mac client has neither of these: an interface version is known when the client carries its name (the
// interface itself still comes from the client, or NULL).
static int known_version(const char *version) {
    static char *text;
    static size_t size;
    if (!text) {
        Dl_info info;
        void *create = mac_lib() ? dlsym(g_lib, "CreateInterface") : NULL;
        FILE *f = create && dladdr(create, &info) ? fopen(info.dli_fname, "rb") : NULL;
        if (f) {
            fseek(f, 0, SEEK_END); size = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
            text = malloc(size + 1);
            size = fread(text, 1, size, f);
            fclose(f);
        }
        if (!text) return 1;
    }
    size_t n = strlen(version);
    for (const char *p = text; (p = memmem(p, size - (size_t)(p - text), version, n + 1)); p++)
        if (p == text || !p[-1]) return 1;              // the whole string, not the tail of a longer one
    return 0;
}
static WINAPI BOOL Steam_IsKnownInterface(const char *version) {
    int k = version && known_version(version);
    if (g_trace) vx_log("Steam bridge: Steam_IsKnownInterface(%s) = %d", version ? version : "(null)", k);
    return k;
}
static WINAPI void Steam_NotifyMissingInterface(int pipe, const char *version) {
    (void)pipe; vx_log("Steam bridge: the Mac Steam client lacks interface %s", version ? version : "(null)");
}
// Crash reporting stays with this runtime (the Mac client's handler would take the signals the runtime needs).
static WINAPI u64 Breakpad_Ignored(void) { return 0; }

static const vx_export g_exports[] = {
    VX_EXPORT(CreateInterface),
    VX_EXPORT(Steam_BConnected), VX_EXPORT(Steam_BGetCallback), VX_EXPORT(Steam_BLoggedOn), VX_EXPORT(Steam_BReleaseSteamPipe),
    VX_EXPORT(Steam_ConnectToGlobalUser), VX_EXPORT(Steam_CreateGlobalUser), VX_EXPORT(Steam_CreateLocalUser), VX_EXPORT(Steam_CreateSteamPipe),
    VX_EXPORT(Steam_FreeLastCallback), VX_EXPORT(Steam_GSBLoggedOn), VX_EXPORT(Steam_GSBSecure),
    VX_EXPORT(Steam_GSGetSteam2GetEncryptionKeyToSendToNewClient), VX_EXPORT(Steam_GSGetSteamID), VX_EXPORT(Steam_GSLogOff),
    VX_EXPORT(Steam_GSLogOn), VX_EXPORT(Steam_GSRemoveUserConnect), VX_EXPORT(Steam_GSSendSteam2UserConnect),
    VX_EXPORT(Steam_GSSendSteam3UserConnect), VX_EXPORT(Steam_GSSendUserDisconnect), VX_EXPORT(Steam_GSSendUserStatusResponse),
    VX_EXPORT(Steam_GSSetServerType), VX_EXPORT(Steam_GSSetSpawnCount), VX_EXPORT(Steam_GSUpdateStatus), VX_EXPORT(Steam_GetAPICallResult),
    VX_EXPORT(Steam_GetGSHandle), VX_EXPORT(Steam_InitiateGameConnection), VX_EXPORT(Steam_IsKnownInterface), VX_EXPORT(Steam_LogOff),
    VX_EXPORT(Steam_LogOn), VX_EXPORT(Steam_NotifyMissingInterface), VX_EXPORT(Steam_ReleaseThreadLocalMemory), VX_EXPORT(Steam_ReleaseUser),
    VX_EXPORT(Steam_SetLocalIPBinding), VX_EXPORT(Steam_TerminateGameConnection),
    { "Breakpad_SteamMiniDumpInit", (void *)Breakpad_Ignored }, { "Breakpad_SteamSendMiniDump", (void *)Breakpad_Ignored },
    { "Breakpad_SteamSetAppID", (void *)Breakpad_Ignored }, { "Breakpad_SteamSetSteamID", (void *)Breakpad_Ignored },
    { "Breakpad_SteamWriteMiniDumpSetComment", (void *)Breakpad_Ignored },
    { "Breakpad_SteamWriteMiniDumpUsingExceptionInfoWithBuildId", (void *)Breakpad_Ignored },
};

// What steam_api64.dll looks for before it loads steamclient64.dll: Steam's process (this one stands in: it is
// running) and the client DLL's path, under HKEY_CURRENT_USER\Software\Valve\Steam\ActiveProcess.
void vx_reg_put(const char *path, const char *name, DWORD type, const void *data, DWORD size);   // registry.c
void vx_steam_registry(void) {
    if (!vx_steambridge_enabled()) return;
    DWORD pid = (DWORD)getpid(), user = 1;
    vx_reg_put("HKEY_CURRENT_USER\\Software\\Valve\\Steam\\ActiveProcess", "pid", 4, &pid, 4);
    vx_reg_put("HKEY_CURRENT_USER\\Software\\Valve\\Steam\\ActiveProcess", "ActiveUser", 4, &user, 4);
    static const char dll[] = "C:\\windows\\system32\\steamclient64.dll", dir[] = "C:\\windows\\system32";
    WCHAR w[64];
    for (size_t i = 0; i < sizeof dll; i++) w[i] = (WCHAR)dll[i];
    vx_reg_put("HKEY_CURRENT_USER\\Software\\Valve\\Steam\\ActiveProcess", "SteamClientDll64", 1, w, (DWORD)sizeof dll * 2);
    for (size_t i = 0; i < sizeof dir; i++) w[i] = (WCHAR)dir[i];
    vx_reg_put("HKEY_CURRENT_USER\\Software\\Valve\\Steam", "SteamPath", 1, w, (DWORD)sizeof dir * 2);
}
int vx_steambridge_enabled(void) {
    static int on = -1;
    if (on < 0) on = getenv("VXRT_STEAM_BRIDGE") && atoi(getenv("VXRT_STEAM_BRIDGE"));
    return on;
}

// "vxrt --vxrt-steam-check": whether a game started now would get Steam, asked of the same steamclient.dylib (and the
// same slice) the bridge loads, the way steam_api does: a pipe to the running client, its signed-in user, and whether
// that user is online (a game's sign-in through Steam needs Steam's servers: Elite's Frontier launcher shows "does not
// own Elite Dangerous" without them, issue #10). Then both are let go. Nothing of Steam's files is read or written.
// One word on stdout and the exit status: ready 0, unavailable 2 (no client library), not-running 3 (no pipe),
// not-signed-in 4 (no user), offline 5 (a user, not connected to Steam).
int vx_steam_check(void) {
    if (!mac_lib()) { puts("unavailable"); return 2; }
    typedef SYSV int (*pipe_fn)(void);
    typedef SYSV int (*user_fn)(int);
    typedef SYSV int (*logged_fn)(int, int);
    typedef SYSV void (*release_user_fn)(int, int);
    typedef SYSV int (*release_pipe_fn)(int);
    pipe_fn create = (pipe_fn)dlsym(g_lib, "Steam_CreateSteamPipe");
    user_fn connect = (user_fn)dlsym(g_lib, "Steam_ConnectToGlobalUser");
    logged_fn logged = (logged_fn)dlsym(g_lib, "Steam_BLoggedOn");
    release_user_fn release_user = (release_user_fn)dlsym(g_lib, "Steam_ReleaseUser");
    release_pipe_fn release_pipe = (release_pipe_fn)dlsym(g_lib, "Steam_BReleaseSteamPipe");
    if (!create || !connect || !logged || !release_user || !release_pipe) { puts("unavailable"); return 2; }
    int pipe = create();
    if (!pipe) { puts("not-running"); return 3; }
    int user = connect(pipe), status = 0;
    if (!user) { puts("not-signed-in"); status = 4; }
    else if (!logged(user, pipe)) { puts("offline"); status = 5; }
    else puts("ready");
    if (user) release_user(pipe, user);
    release_pipe(pipe);
    fflush(stdout);
    return status;
}

void vx_steambridge_init(void) {
    for (int i = 0; i < METHODS; i++) g_vtbl[i] = g_stubs[i];
    g_trace = getenv("VXRT_TRACE_STEAM") ? atoi(getenv("VXRT_TRACE_STEAM")) : 0;   // 2: every call
    if (vx_steambridge_enabled()) vx_register_module("steamclient64.dll", g_exports, sizeof g_exports / sizeof g_exports[0]);
}

#ifdef __aarch64__
#include "thunks/steambridge.c.inc"
#endif
