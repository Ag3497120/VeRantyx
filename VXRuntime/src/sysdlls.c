// Small system DLLs that Chromium (the Windows Steam client's steamwebhelper/libcef.dll) links, answered the way a
// Windows machine without the matching services answers, from Microsoft's documentation of each function:
//   userenv       environment blocks for new processes, AppContainer profiles (registered only: no process can be
//                 started inside one, because UpdateProcThreadAttribute in src/extras.c refuses the AppContainer
//                 attributes with ERROR_NOT_SUPPORTED, so such a launch fails instead of running unconfined),
//                 group-policy sections and notifications (policy never changes)
//   winspool.drv  a machine with no printers (and no default printer)
//   dhcpcsvc      the options the Mac's own DHCP client received, for the one adapter iphlpapi reports
//   secur32       the account's name; no LSA and no security packages, so nothing is ever authenticated here
//   oleaut32      the functions libcef.dll imports by ordinal and the runtime did not have yet
#include "vx.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wctype.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <sys/socket.h>
#include <CommonCrypto/CommonDigest.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include <SystemConfiguration/SCDynamicStoreCopyDHCPInfo.h>

typedef int32_t HRESULT;
#define S_OK 0
#define E_INVALIDARG ((HRESULT)0x80070057)
#define HRESULT_FROM_WIN32(e) ((e) ? (HRESULT)(0x80070000u | ((DWORD)(e) & 0xffff)) : S_OK)
enum { ERROR_FILE_NOT_FOUND = 2, ERROR_INVALID_HANDLE = 6, ERROR_INVALID_PARAMETER = 87, ERROR_INVALID_LEVEL = 124,
       ERROR_ALREADY_EXISTS = 183, ERROR_MORE_DATA = 234, ERROR_NONE_MAPPED = 1332, ERROR_TIMEOUT = 1460,
       ERROR_INVALID_PRINTER_NAME = 1801 };

// A pointer this runtime stores where the program reads it: the program's view of the address (ARM64 build with
// the vCPU: its memory below 4 GB sits at an offset on the host).
#ifdef __aarch64__
#define PROGRAM_ADDRESS(p) vx_unlow((uint64_t)(uintptr_t)(p))
#else
#define PROGRAM_ADDRESS(p) ((uint64_t)(uintptr_t)(p))
#endif

// The runtime's own Windows functions these build on (looked up once at start-up).
static BOOL (WINAPI *rt_GetUserNameW)(WCHAR *, DWORD *);
static BOOL (WINAPI *rt_GetComputerNameW)(WCHAR *, DWORD *);
static WCHAR *(WINAPI *rt_GetEnvironmentStringsW)(void);
static BOOL (WINAPI *rt_FreeEnvironmentStringsW)(WCHAR *);
static HANDLE (WINAPI *rt_CreateMutexW)(void *, BOOL, const WCHAR *);
static DWORD (WINAPI *rt_WaitForSingleObject)(HANDLE, DWORD);
static BOOL (WINAPI *rt_ReleaseMutex)(HANDLE);
static BOOL (WINAPI *rt_CloseHandle)(HANDLE);
static LONG (WINAPI *rt_RegCreateKeyExW)(void *, const WCHAR *, DWORD, WCHAR *, DWORD, DWORD, void *, void **, DWORD *);
static LONG (WINAPI *rt_RegSetValueExW)(void *, const WCHAR *, DWORD, DWORD, const uint8_t *, DWORD);
static LONG (WINAPI *rt_RegDeleteKeyW)(void *, const WCHAR *);
static LONG (WINAPI *rt_RegCloseKey)(void *);
static int (WINAPI *rt_CompareStringW)(DWORD, DWORD, const WCHAR *, int, const WCHAR *, int);

// The user's and the machine's names as kernel32/advapi32 report them (UTF-8).
static void user_and_computer(char *user, size_t ucap, char *computer, size_t ccap) {
    WCHAR w[256]; DWORD n = 256;
    snprintf(user, ucap, "%s", "vxrt");
    if (rt_GetUserNameW && rt_GetUserNameW(w, &n)) vx_utf16_to_utf8(w, vx_wcslen(w) + 1, user, ucap);
    n = 256; snprintf(computer, ccap, "%s", "VXRT");
    if (rt_GetComputerNameW && rt_GetComputerNameW(w, &n)) vx_utf16_to_utf8(w, vx_wcslen(w) + 1, computer, ccap);
}

// ---- userenv: environment blocks --------------------------------------------------------------------------------------
// CreateEnvironmentBlock: the variables a new process of the user would get, as one Unicode block ("NAME=value" strings,
// then an empty one) sorted by name without case, as Windows requires of environment blocks. With bInherit, the calling
// process's variables come first; the system's variables are set over them, and with a token (the one user this
// runtime has) the user's profile variables too. Values are the ones this runtime gives for the same places
// (shell folders, GetTempPath, GetComputerName, GetUserName).
typedef struct { WCHAR *text; size_t name; } env_var;                 // "NAME=value", the length of NAME
typedef struct { env_var *v; size_t n, cap; } env_list;
static int env_name_cmp(const WCHAR *a, size_t na, const WCHAR *b, size_t nb) {
    for (size_t i = 0; i < na && i < nb; i++) {
        wint_t x = towupper(a[i]), y = towupper(b[i]);
        if (x != y) return x < y ? -1 : 1;
    }
    return na == nb ? 0 : na < nb ? -1 : 1;
}
static env_var *env_find(env_list *l, const WCHAR *name, size_t n) {
    for (size_t i = 0; i < l->n; i++) if (!env_name_cmp(l->v[i].text, l->v[i].name, name, n)) return &l->v[i];
    return NULL;
}
static void env_put(env_list *l, WCHAR *text, int replace) {          // takes text
    size_t name = 0;
    while (text[name] && (text[name] != '=' || !name)) name++;
    env_var *old = env_find(l, text, name);
    if (old && !replace) { free(text); return; }
    if (old) { free(old->text); old->text = text; old->name = name; return; }
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 64; l->v = realloc(l->v, l->cap * sizeof *l->v); }
    l->v[l->n++] = (env_var){ text, name };
}
static void env_set(env_list *l, const char *name, const char *value, int replace) {
    char line[1024];
    snprintf(line, sizeof line, "%s=%s", name, value);
    env_put(l, vx_utf16_dup(line), replace);
}
static int env_sort(const void *a, const void *b) {
    const env_var *x = a, *y = b;
    return env_name_cmp(x->text, x->name, y->text, y->name);
}
static WINAPI BOOL CreateEnvironmentBlock(void **out, HANDLE token, BOOL inherit) {
    if (!out) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    env_list l = { 0 };
    if (inherit && rt_GetEnvironmentStringsW) {
        WCHAR *block = rt_GetEnvironmentStringsW();
        for (WCHAR *p = block; p && *p; p += vx_wcslen(p) + 1) {
            if (*p == '=') continue;                                  // the per-drive current directories stay with the process
            size_t n = vx_wcslen(p) + 1;
            WCHAR *copy = malloc(n * sizeof(WCHAR));
            memcpy(copy, p, n * sizeof(WCHAR));
            env_put(&l, copy, 1);
        }
        if (block && rt_FreeEnvironmentStringsW) rt_FreeEnvironmentStringsW(block);
    }
    char user[256], computer[256], cpus[16], home[512];
    user_and_computer(user, sizeof user, computer, sizeof computer);
    snprintf(cpus, sizeof cpus, "%ld", sysconf(_SC_NPROCESSORS_ONLN));
    const char *windows = vx_special_folder_path(0x24), *common = vx_special_folder_path(0x23);
    const char *system[][2] = {
        { "SystemRoot", windows }, { "windir", windows }, { "SystemDrive", "C:" }, { "ProgramData", common },
        { "ALLUSERSPROFILE", common }, { "ProgramFiles", vx_special_folder_path(0x26) }, { "ProgramW6432", vx_special_folder_path(0x26) },
        { "ProgramFiles(x86)", vx_special_folder_path(0x2a) }, { "PUBLIC", "C:\\users\\Public" },
        { "ComSpec", "C:\\windows\\system32\\cmd.exe" }, { "OS", "Windows_NT" }, { "PROCESSOR_ARCHITECTURE", "AMD64" },
        { "NUMBER_OF_PROCESSORS", cpus }, { "COMPUTERNAME", computer }, { "PATHEXT", ".COM;.EXE;.BAT;.CMD" },
        { "TEMP", "C:\\windows\\temp" }, { "TMP", "C:\\windows\\temp" },
    };
    for (size_t i = 0; i < sizeof system / sizeof system[0]; i++) env_set(&l, system[i][0], system[i][1], 1);
    env_set(&l, "Path", "C:\\windows\\system32;C:\\windows", 0);      // (an inherited Path stays)
    if (token) {
        const char *profile = vx_special_folder_path(0x28);
        snprintf(home, sizeof home, "%s", profile + 2);               // without the drive
        const char *user_vars[][2] = {
            { "USERPROFILE", profile }, { "APPDATA", vx_special_folder_path(0x1a) }, { "LOCALAPPDATA", vx_special_folder_path(0x1c) },
            { "HOMEDRIVE", "C:" }, { "HOMEPATH", home }, { "USERNAME", user }, { "USERDOMAIN", computer },
        };
        for (size_t i = 0; i < sizeof user_vars / sizeof user_vars[0]; i++) env_set(&l, user_vars[i][0], user_vars[i][1], 1);
    }
    qsort(l.v, l.n, sizeof *l.v, env_sort);
    size_t total = 1;
    for (size_t i = 0; i < l.n; i++) total += vx_wcslen(l.v[i].text) + 1;
    WCHAR *block = vx_heap_alloc(total * sizeof(WCHAR)), *p = block;
    for (size_t i = 0; i < l.n; i++) {
        size_t n = vx_wcslen(l.v[i].text) + 1;
        memcpy(p, l.v[i].text, n * sizeof(WCHAR));
        p += n;
        free(l.v[i].text);
    }
    *p = 0;
    free(l.v);
    *out = block;
    return 1;
}
static WINAPI BOOL DestroyEnvironmentBlock(void *block) {
    if (!block) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    free(block);
    return 1;
}

// ---- userenv: AppContainer profiles ----------------------------------------------------------------------------------
// The profile is registered where Windows keeps the mapping (HKCU\...\AppContainer\Mappings\<SID>: Moniker, DisplayName,
// Description), so a second CreateAppContainerProfile reports that it exists, as on Windows. The SID is
// S-1-15-2-x1..x7 (SECURITY_APP_PACKAGE_AUTHORITY, SECURITY_APP_PACKAGE_BASE_RID, 7 more RIDs) derived from the name
// alone: the RIDs are the first 28 bytes of SHA-256 of the lowercased name in UTF-16LE, which is what Windows is
// reported to use (not compared with a Windows machine here). Names: up to 64 characters of [-_. A-Za-z0-9].
// A profile and its SID are only names: a process asking to be started inside the container fails when the attribute
// is set (UpdateProcThreadAttribute, src/extras.c), since nothing here could confine it.
#define TYPE_HKEY_CURRENT_USER ((void *)(uintptr_t)0x80000001u)
static size_t container_name_length(const WCHAR *name) {
    if (!name) return 0;
    size_t n = 0;
    for (; name[n]; n++) {
        WCHAR c = name[n];
        if (n >= 64 || !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ' '))
            return 0;
    }
    return n;
}
static uint8_t *container_sid(const WCHAR *name, size_t n, char *text, size_t cap) {
    uint8_t lower[128], digest[CC_SHA256_DIGEST_LENGTH];
    for (size_t i = 0; i < n; i++) {
        WCHAR c = name[i] >= 'A' && name[i] <= 'Z' ? (WCHAR)(name[i] + 32) : name[i];
        lower[2 * i] = (uint8_t)c; lower[2 * i + 1] = (uint8_t)(c >> 8);
    }
    CC_SHA256(lower, (CC_LONG)(2 * n), digest);
    uint8_t *sid = vx_heap_zalloc(8 + 4 * 8);                         // freed with FreeSid
    sid[0] = 1; sid[1] = 8; sid[7] = 15;                               // revision, 8 sub-authorities, authority 15
    uint32_t rid[8] = { 2 };
    for (int i = 0; i < 7; i++) rid[i + 1] = (uint32_t)digest[4 * i] | (uint32_t)digest[4 * i + 1] << 8 | (uint32_t)digest[4 * i + 2] << 16 | (uint32_t)digest[4 * i + 3] << 24;
    memcpy(sid + 8, rid, sizeof rid);
    int len = snprintf(text, cap, "S-1-15");
    for (int i = 0; i < 8; i++) len += snprintf(text + len, cap - (size_t)len, "-%u", rid[i]);
    return sid;
}
static WCHAR *mapping_key(const char *sid_text) {
    char path[256];
    snprintf(path, sizeof path, "Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppContainer\\Mappings\\%s", sid_text);
    return vx_utf16_dup(path);
}
static int text_fits(const WCHAR *s, size_t max) { return s && vx_wcslen(s) <= max; }
static WINAPI HRESULT CreateAppContainerProfile(const WCHAR *name, const WCHAR *display, const WCHAR *description,
                                                const void *capabilities, DWORD capability_count, void **sid_out) {
    size_t n = container_name_length(name);
    if (!n || !sid_out || !text_fits(display, 512) || !text_fits(description, 2048) || (capability_count && !capabilities)) return E_INVALIDARG;
    *sid_out = NULL;
    if (!rt_RegCreateKeyExW || !rt_RegSetValueExW || !rt_RegCloseKey) return HRESULT_FROM_WIN32(50);
    char text[128];
    uint8_t *sid = container_sid(name, n, text, sizeof text);
    WCHAR *path = mapping_key(text);
    void *key = NULL; DWORD disposition = 0;
    LONG r = rt_RegCreateKeyExW(TYPE_HKEY_CURRENT_USER, path, 0, NULL, 0, 0xf003f /* KEY_ALL_ACCESS */, NULL, &key, &disposition);
    free(path);
    if (r) { free(sid); return HRESULT_FROM_WIN32(r); }
    if (disposition == 2) { rt_RegCloseKey(key); free(sid); return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS); }   // REG_OPENED_EXISTING_KEY
    static const WCHAR moniker[] = { 'M', 'o', 'n', 'i', 'k', 'e', 'r', 0 }, shown[] = { 'D', 'i', 's', 'p', 'l', 'a', 'y', 'N', 'a', 'm', 'e', 0 },
                       about[] = { 'D', 'e', 's', 'c', 'r', 'i', 'p', 't', 'i', 'o', 'n', 0 };
    rt_RegSetValueExW(key, moniker, 0, 1 /* REG_SZ */, (const uint8_t *)name, (DWORD)(2 * (n + 1)));
    rt_RegSetValueExW(key, shown, 0, 1, (const uint8_t *)display, (DWORD)(2 * (vx_wcslen(display) + 1)));
    rt_RegSetValueExW(key, about, 0, 1, (const uint8_t *)description, (DWORD)(2 * (vx_wcslen(description) + 1)));
    rt_RegCloseKey(key);
    *sid_out = sid;
    return S_OK;
}
static WINAPI HRESULT DeriveAppContainerSidFromAppContainerName(const WCHAR *name, void **sid_out) {
    size_t n = container_name_length(name);
    if (!n || !sid_out) return E_INVALIDARG;
    char text[128];
    *sid_out = container_sid(name, n, text, sizeof text);
    return S_OK;
}
static WINAPI HRESULT DeleteAppContainerProfile(const WCHAR *name) {
    size_t n = container_name_length(name);
    if (!n) return E_INVALIDARG;
    if (!rt_RegDeleteKeyW) return HRESULT_FROM_WIN32(50);
    char text[128];
    free(container_sid(name, n, text, sizeof text));
    WCHAR *path = mapping_key(text);
    LONG r = rt_RegDeleteKeyW(TYPE_HKEY_CURRENT_USER, path);
    free(path);
    return HRESULT_FROM_WIN32(r);
}

// ---- userenv: group policy -------------------------------------------------------------------------------------------
// A policy section is a mutex shared by this runtime's processes (machine and user policy each have one): while a
// program holds it, nothing else enters. Policy is never applied here, so a registered event is never set.
static pthread_mutex_t g_policy_lock = PTHREAD_MUTEX_INITIALIZER;
static HANDLE g_sections[16], g_gp_events[16];
static int remember(HANDLE *set, HANDLE h) {
    for (int i = 0; i < 16; i++) if (!set[i]) { set[i] = h; return 1; }
    return 0;
}
static int forget(HANDLE *set, HANDLE h) {
    for (int i = 0; i < 16; i++) if (h && set[i] == h) { set[i] = NULL; return 1; }
    return 0;
}
static WINAPI HANDLE EnterCriticalPolicySection(BOOL machine) {
    if (!rt_CreateMutexW || !rt_WaitForSingleObject) { vx_set_last_error(50); return NULL; }
    WCHAR *name = vx_utf16_dup(machine ? "Global\\vxrt.userenv.policy.machine" : "Global\\vxrt.userenv.policy.user");
    HANDLE m = rt_CreateMutexW(NULL, 0, name);
    free(name);
    if (!m) return NULL;
    DWORD r = rt_WaitForSingleObject(m, 600000);                       // Windows waits up to 10 minutes
    if (r != 0 && r != 0x80) { rt_CloseHandle(m); vx_set_last_error(r == 0x102 ? ERROR_TIMEOUT : vx_get_last_error()); return NULL; }
    pthread_mutex_lock(&g_policy_lock);
    int kept = remember(g_sections, m);
    pthread_mutex_unlock(&g_policy_lock);
    if (!kept) { rt_ReleaseMutex(m); rt_CloseHandle(m); vx_set_last_error(8); return NULL; }   // ERROR_NOT_ENOUGH_MEMORY
    return m;
}
static WINAPI BOOL LeaveCriticalPolicySection(HANDLE section) {
    pthread_mutex_lock(&g_policy_lock);
    int ours = forget(g_sections, section);
    pthread_mutex_unlock(&g_policy_lock);
    if (!ours) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    BOOL ok = rt_ReleaseMutex(section);
    rt_CloseHandle(section);
    return ok;
}
static WINAPI BOOL RegisterGPNotification(HANDLE event, BOOL machine) {
    (void)machine;
    if (!event) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    pthread_mutex_lock(&g_policy_lock);
    int kept = remember(g_gp_events, event);
    pthread_mutex_unlock(&g_policy_lock);
    if (!kept) vx_set_last_error(8);
    return kept;
}
static WINAPI BOOL UnregisterGPNotification(HANDLE event) {
    pthread_mutex_lock(&g_policy_lock);
    int found = forget(g_gp_events, event);
    pthread_mutex_unlock(&g_policy_lock);
    if (!found) vx_set_last_error(ERROR_INVALID_PARAMETER);
    return found;
}

// ---- winspool.drv: no printers -----------------------------------------------------------------------------------------
// Enumerations succeed with nothing in them; a printer name never opens, so no printer handle is ever valid.
static BOOL printers_none(DWORD level, DWORD *needed, DWORD *returned) {
    if (!needed || !returned) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    if (level != 1 && level != 2 && level != 4 && level != 5) { vx_set_last_error(ERROR_INVALID_LEVEL); return 0; }
    *needed = 0; *returned = 0;
    return 1;
}
static WINAPI BOOL EnumPrintersW(DWORD flags, WCHAR *name, DWORD level, uint8_t *buf, DWORD size, DWORD *needed, DWORD *returned) {
    (void)flags; (void)name; (void)buf; (void)size; return printers_none(level, needed, returned);
}
static WINAPI BOOL EnumPrintersA(DWORD flags, char *name, DWORD level, uint8_t *buf, DWORD size, DWORD *needed, DWORD *returned) {
    (void)flags; (void)name; (void)buf; (void)size; return printers_none(level, needed, returned);
}
static WINAPI BOOL OpenPrinterW(const WCHAR *name, HANDLE *printer, void *defaults) {
    (void)name; (void)defaults;
    if (printer) *printer = NULL;
    vx_set_last_error(ERROR_INVALID_PRINTER_NAME);
    return 0;
}
static WINAPI BOOL OpenPrinterA(const char *name, HANDLE *printer, void *defaults) { (void)name; return OpenPrinterW(NULL, printer, defaults); }
static WINAPI BOOL GetPrinterW(HANDLE printer, DWORD level, uint8_t *buf, DWORD size, DWORD *needed) {
    (void)printer; (void)level; (void)buf; (void)size; (void)needed;
    vx_set_last_error(ERROR_INVALID_HANDLE);
    return 0;
}
static WINAPI BOOL GetPrinterA(HANDLE printer, DWORD level, uint8_t *buf, DWORD size, DWORD *needed) { return GetPrinterW(printer, level, buf, size, needed); }
static WINAPI BOOL GetPrinterDriverW(HANDLE printer, WCHAR *environment, DWORD level, uint8_t *buf, DWORD size, DWORD *needed) {
    (void)environment; return GetPrinterW(printer, level, buf, size, needed);
}
static WINAPI BOOL GetPrinterDriverA(HANDLE printer, char *environment, DWORD level, uint8_t *buf, DWORD size, DWORD *needed) {
    (void)environment; return GetPrinterW(printer, level, buf, size, needed);
}
static WINAPI BOOL ClosePrinter(HANDLE printer) { (void)printer; vx_set_last_error(ERROR_INVALID_HANDLE); return 0; }
// Less than zero is DocumentProperties' failure.
static WINAPI LONG DocumentPropertiesW(HANDLE wnd, HANDLE printer, WCHAR *device, void *out, void *in, DWORD mode) {
    (void)wnd; (void)printer; (void)device; (void)out; (void)in; (void)mode;
    vx_set_last_error(ERROR_INVALID_HANDLE);
    return -1;
}
static WINAPI LONG DocumentPropertiesA(HANDLE wnd, HANDLE printer, char *device, void *out, void *in, DWORD mode) {
    (void)device; return DocumentPropertiesW(wnd, printer, NULL, out, in, mode);
}
static WINAPI int DeviceCapabilitiesW(const WCHAR *device, const WCHAR *port, uint16_t capability, WCHAR *out, const void *mode) {
    (void)device; (void)port; (void)capability; (void)out; (void)mode;
    vx_set_last_error(ERROR_INVALID_PRINTER_NAME);
    return -1;
}
static WINAPI int DeviceCapabilitiesA(const char *device, const char *port, uint16_t capability, char *out, const void *mode) {
    (void)device; (void)port; (void)out; return DeviceCapabilitiesW(NULL, NULL, capability, NULL, mode);
}
// No default printer: FALSE with ERROR_FILE_NOT_FOUND, as documented.
static WINAPI BOOL GetDefaultPrinterW(WCHAR *buf, DWORD *size) {
    (void)buf;
    vx_set_last_error(size ? ERROR_FILE_NOT_FOUND : ERROR_INVALID_PARAMETER);
    return 0;
}
static WINAPI BOOL GetDefaultPrinterA(char *buf, DWORD *size) { (void)buf; return GetDefaultPrinterW(NULL, size); }

// ---- dhcpcsvc: the Mac's DHCP options ----------------------------------------------------------------------------------
// The adapter is the one GetAdaptersAddresses reports (extras.c), named "{7A0B4C2D-3E1F-4A5B-9C8D-<its MAC address>}";
// its options are the ones macOS's DHCP client received for that interface (SCDynamicStoreCopyDHCPInfo). An option
// the server did not send comes back with no data, as on Windows. An adapter name that is not that adapter, or an
// interface that is not configured by DHCP, is ERROR_INVALID_PARAMETER, the documented code for an AdapterName the
// function cannot use.
typedef struct { DWORD flags, option; BOOL vendor; DWORD pad; uint64_t data; DWORD size, pad2; } dhcp_param;   // DHCPCAPI_PARAMS
typedef struct { DWORD count, pad; uint64_t params; } dhcp_param_array;                                      // DHCPCAPI_PARAMS_ARRAY
_Static_assert(sizeof(dhcp_param) == 32 && sizeof(dhcp_param_array) == 16, "DHCPCAPI layouts");
static int adapter_interface(const WCHAR *adapter, char *ifname, size_t cap) {
    char name[64];
    if (!adapter || vx_wcslen(adapter) != 38) return 0;
    vx_utf16_to_utf8(adapter, 39, name, sizeof name);
    unsigned mac[6];
    if (strncasecmp(name, "{7A0B4C2D-3E1F-4A5B-9C8D-", 25) || name[37] != '}' ||
        sscanf(name + 25, "%2x%2x%2x%2x%2x%2x", &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) != 6) return 0;
    struct ifaddrs *list = NULL;
    if (getifaddrs(&list)) return 0;
    int found = 0;
    for (struct ifaddrs *a = list; a && !found; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_LINK || ((struct sockaddr_dl *)a->ifa_addr)->sdl_alen != 6) continue;
        const uint8_t *m = (const uint8_t *)LLADDR((struct sockaddr_dl *)a->ifa_addr);
        found = 1;
        for (int i = 0; i < 6; i++) if (m[i] != mac[i]) found = 0;
        if (found) snprintf(ifname, cap, "%s", a->ifa_name);
    }
    freeifaddrs(list);
    return found;
}
static CFDictionaryRef dhcp_info(const char *ifname) {          // the DHCP info of the network service on that interface
    SCDynamicStoreRef store = SCDynamicStoreCreate(NULL, CFSTR("vxrt dhcpcsvc"), NULL, NULL);
    if (!store) return NULL;
    CFDictionaryRef info = NULL;
    CFStringRef want = CFStringCreateWithCString(NULL, ifname, kCFStringEncodingUTF8);
    CFArrayRef keys = SCDynamicStoreCopyKeyList(store, CFSTR("State:/Network/Service/[^/]+/IPv4"));
    for (CFIndex i = 0; keys && want && !info && i < CFArrayGetCount(keys); i++) {
        CFStringRef key = CFArrayGetValueAtIndex(keys, i);
        CFPropertyListRef v = SCDynamicStoreCopyValue(store, key);
        CFStringRef iface = v && CFGetTypeID(v) == CFDictionaryGetTypeID() ? CFDictionaryGetValue(v, CFSTR("InterfaceName")) : NULL;
        if (iface && CFGetTypeID(iface) == CFStringGetTypeID() && CFStringCompare(iface, want, 0) == kCFCompareEqualTo) {
            CFArrayRef parts = CFStringCreateArrayBySeparatingStrings(NULL, key, CFSTR("/"));   // State: Network Service <id> IPv4
            if (parts && CFArrayGetCount(parts) == 5) info = SCDynamicStoreCopyDHCPInfo(store, CFArrayGetValueAtIndex(parts, 3));
            if (parts) CFRelease(parts);
        }
        if (v) CFRelease(v);
    }
    if (keys) CFRelease(keys);
    if (want) CFRelease(want);
    CFRelease(store);
    return info;
}
static WINAPI DWORD DhcpCApiInitialize(DWORD *version) { if (version) *version = 2; return 0; }   // DHCPCAPI_VERSION_V2
static WINAPI void DhcpCApiCleanup(void) {}
// The two DHCPCAPI_PARAMS_ARRAYs are passed by value; at 16 bytes, the x64 convention passes each as a pointer to a copy.
static WINAPI DWORD DhcpRequestParams(DWORD flags, void *reserved, WCHAR *adapter, void *class_id, const dhcp_param_array *send,
                                      const dhcp_param_array *recd, uint8_t *buffer, DWORD *size, WCHAR *request_id) {
    (void)flags; (void)class_id; (void)send; (void)request_id;
    char ifname[32];
    if (reserved || !size || !recd || (recd->count && !recd->params) || !adapter_interface(adapter, ifname, sizeof ifname)) return ERROR_INVALID_PARAMETER;
    CFDictionaryRef info = dhcp_info(ifname);
    if (!info) return ERROR_INVALID_PARAMETER;
    dhcp_param *p = VX_LOW((uintptr_t)recd->params);
    DWORD need = 0;
    for (DWORD i = 0; i < recd->count; i++) {
        CFDataRef d = !p[i].vendor && p[i].option < 256 ? DHCPInfoGetOptionData(info, (UInt8)p[i].option) : NULL;
        need += d ? (DWORD)CFDataGetLength(d) : 0;
    }
    if (need > *size || (need && !buffer)) { CFRelease(info); *size = need; return ERROR_MORE_DATA; }
    DWORD used = 0;
    for (DWORD i = 0; i < recd->count; i++) {
        CFDataRef d = !p[i].vendor && p[i].option < 256 ? DHCPInfoGetOptionData(info, (UInt8)p[i].option) : NULL;
        DWORD n = d ? (DWORD)CFDataGetLength(d) : 0;
        p[i].size = n;
        p[i].data = n ? PROGRAM_ADDRESS(buffer + used) : 0;
        if (n) { memcpy(buffer + used, CFDataGetBytePtr(d), n); used += n; }
    }
    CFRelease(info);
    *size = used;
    return 0;
}

// ---- secur32: names, no LSA, no security packages --------------------------------------------------------------------
// GetUserNameEx answers what a local account on a workgroup machine has: NameSamCompatible ("COMPUTER\user", the
// names GetComputerName and GetUserName give). The directory formats belong to domain accounts, and the display name
// is not offered (the Mac account's real name is not handed to Windows programs): ERROR_NONE_MAPPED, documented for
// a name that is not available in the format asked for.
enum { NameUnknown, NameFullyQualifiedDN, NameSamCompatible, NameDisplay, NameUniqueId = 6, NameCanonical, NameUserPrincipal,
       NameCanonicalEx, NameServicePrincipal, NameDnsDomain = 12, NameGivenName, NameSurname };
static int user_name_ex(int format, char *out, size_t cap) {      // 0 with the last error set
    char user[256], computer[256];
    switch (format) {
    case NameSamCompatible:
        user_and_computer(user, sizeof user, computer, sizeof computer);
        snprintf(out, cap, "%s\\%s", computer, user);
        return 1;
    case NameDisplay: case NameFullyQualifiedDN: case NameUniqueId: case NameCanonical: case NameUserPrincipal: case NameCanonicalEx:
    case NameServicePrincipal: case NameDnsDomain: case NameGivenName: case NameSurname:
        break;
    default:
        vx_set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    vx_set_last_error(ERROR_NONE_MAPPED);
    return 0;
}
// On success the size is the characters copied, without the terminator; when the buffer is short, ERROR_MORE_DATA
// and the size needed, with it.
static WINAPI uint8_t GetUserNameExW(int format, WCHAR *buf, DWORD *size) {   // (BOOLEAN)
    char name[1024];
    if (!size) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    if (!user_name_ex(format, name, sizeof name)) return 0;
    DWORD n = (DWORD)vx_utf8_to_utf16(name, strlen(name), NULL, 0);
    if (!buf || *size < n + 1) { *size = n + 1; vx_set_last_error(ERROR_MORE_DATA); return 0; }
    vx_utf8_to_utf16(name, strlen(name), buf, n); buf[n] = 0;
    *size = n;
    return 1;
}
static WINAPI uint8_t GetUserNameExA(int format, char *buf, DWORD *size) {
    char name[1024];
    if (!size) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    if (!user_name_ex(format, name, sizeof name)) return 0;
    DWORD n = (DWORD)strlen(name);
    if (!buf || *size < n + 1) { *size = n + 1; vx_set_last_error(ERROR_MORE_DATA); return 0; }
    memcpy(buf, name, n + 1);
    *size = n;
    return 1;
}
// There is no Local Security Authority to connect to (STATUS_INVALID_SERVER_STATE, "the LSA server is disabled"), so
// no LSA handle exists: logons and package calls report an invalid handle and never succeed.
#define STATUS_INVALID_HANDLE ((LONG)0xC0000008)
#define STATUS_INVALID_PARAMETER ((LONG)0xC000000D)
#define STATUS_INVALID_SERVER_STATE ((LONG)0xC00000DC)
static WINAPI LONG LsaConnectUntrusted(HANDLE *lsa) {
    if (!lsa) return STATUS_INVALID_PARAMETER;
    *lsa = NULL;
    return STATUS_INVALID_SERVER_STATE;
}
static WINAPI LONG LsaRegisterLogonProcess(const void *name, HANDLE *lsa, DWORD *mode) {
    (void)name; if (mode) *mode = 0;
    return LsaConnectUntrusted(lsa);
}
static WINAPI LONG LsaDeregisterLogonProcess(HANDLE lsa) { (void)lsa; return STATUS_INVALID_HANDLE; }
static WINAPI LONG LsaLookupAuthenticationPackage(HANDLE lsa, const void *name, DWORD *package) {
    (void)lsa; (void)name; if (package) *package = 0; return STATUS_INVALID_HANDLE;
}
static WINAPI LONG LsaLogonUser(HANDLE lsa, const void *origin, int type, DWORD package, const void *auth, DWORD auth_size,
                                const void *groups, const void *source, void **profile, DWORD *profile_size, void *logon_id,
                                HANDLE *token, void *quotas, LONG *sub_status) {
    (void)lsa; (void)origin; (void)type; (void)package; (void)auth; (void)auth_size; (void)groups; (void)source; (void)logon_id; (void)quotas;
    if (profile) *profile = NULL;
    if (profile_size) *profile_size = 0;
    if (token) *token = NULL;
    if (sub_status) *sub_status = 0;
    return STATUS_INVALID_HANDLE;
}
static WINAPI LONG LsaCallAuthenticationPackage(HANDLE lsa, DWORD package, const void *in, DWORD in_size, void **out, DWORD *out_size, LONG *status) {
    (void)lsa; (void)package; (void)in; (void)in_size;
    if (out) *out = NULL;
    if (out_size) *out_size = 0;
    if (status) *status = 0;
    return STATUS_INVALID_HANDLE;
}
// Nothing here hands out LSA buffers, so only NULL is a buffer that can be "freed".
static WINAPI LONG LsaFreeReturnBuffer(void *buffer) { return buffer ? STATUS_INVALID_PARAMETER : 0; }
// No security package exists (AcquireCredentialsHandle reports SEC_E_UNSUPPORTED_FUNCTION, client.c): every lookup is
// SEC_E_SECPKG_NOT_FOUND, so Negotiate, NTLM and Kerberos are reported unavailable rather than half there.
#define SEC_E_SECPKG_NOT_FOUND ((LONG)0x80090305)
static WINAPI LONG QuerySecurityPackageInfoW(const WCHAR *package, void **info) {
    (void)package; if (info) *info = NULL; return SEC_E_SECPKG_NOT_FOUND;
}
static WINAPI LONG QuerySecurityPackageInfoA(const char *package, void **info) {
    (void)package; if (info) *info = NULL; return SEC_E_SECPKG_NOT_FOUND;
}
// EnumerateSecurityPackages: there are none.
static WINAPI LONG EnumerateSecurityPackagesW(DWORD *count, void **info) { if (count) *count = 0; if (info) *info = NULL; return 0; }
static WINAPI LONG EnumerateSecurityPackagesA(DWORD *count, void **info) { if (count) *count = 0; if (info) *info = NULL; return 0; }
static WINAPI LONG sspi_table_unsupported(void) { return (LONG)0x80090302; }   // SEC_E_UNSUPPORTED_FUNCTION
// InitSecurityInterface: the SecurityFunctionTable (sspi.h) of this DLL's own functions, the way libcurl's Schannel
// backend reaches SSPI; with no packages, acquiring credentials fails and the caller reports its connection failed.
// The table's slots in order, from dwVersion's 8 bytes on; NULL names are reserved slots.
static const char *const sspi_slots[] = {
    "EnumerateSecurityPackages%s", "QueryCredentialsAttributes%s", "AcquireCredentialsHandle%s", "FreeCredentialsHandle", NULL,
    "InitializeSecurityContext%s", "AcceptSecurityContext", "CompleteAuthToken", "DeleteSecurityContext", "ApplyControlToken",
    "QueryContextAttributes%s", "ImpersonateSecurityContext", "RevertSecurityContext", "MakeSignature", "VerifySignature",
    "FreeContextBuffer", "QuerySecurityPackageInfo%s", NULL, NULL, "ExportSecurityContext", "ImportSecurityContext%s",
    "AddCredentials%s", NULL, "QuerySecurityContextToken", "EncryptMessage", "DecryptMessage", "SetContextAttributes%s",
    "SetCredentialsAttributes%s", "ChangeAccountPassword%s", "QueryContextAttributesEx%s", "QueryCredentialsAttributesEx%s",
};
static void *g_sspi_tables[2][1 + sizeof sspi_slots / sizeof sspi_slots[0]];
static void sspi_fill(void) {
    for (int w = 0; w < 2; w++) {
        g_sspi_tables[w][0] = (void *)(uintptr_t)4;                   // dwVersion: SECURITY_SUPPORT_PROVIDER_INTERFACE_VERSION_4
        for (size_t i = 0; i < sizeof sspi_slots / sizeof sspi_slots[0]; i++) {
            if (!sspi_slots[i]) continue;
            char name[64];
            snprintf(name, sizeof name, sspi_slots[i], w ? "W" : "A");
            void *fn = vx_find_export("secur32.dll", name);
            g_sspi_tables[w][1 + i] = fn ? fn : (void *)sspi_table_unsupported;
        }
    }
}
static void *sspi_table(int wide) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, sspi_fill);
    return g_sspi_tables[wide];
}
static WINAPI void *InitSecurityInterfaceW(void) { return sspi_table(1); }
static WINAPI void *InitSecurityInterfaceA(void) { return sspi_table(0); }

// ---- oleaut32: what libcef.dll imports by ordinal -------------------------------------------------------------------
// The ordinals (OLEAUT32's export numbers, a fixed ABI) and their functions: 77 SafeArrayGetVartype, 161 LoadTypeLib,
// 162 LoadRegTypeLib, 185 VariantTimeToSystemTime, 277 VarUI4FromStr, 314 VarBstrCmp. libcef's import table lists them
// in the order of their names (as Microsoft's linker sorts them), and each call site passes the documented arguments
// of that function.
#define DISP_E_TYPEMISMATCH ((HRESULT)0x80020005)
#define DISP_E_OVERFLOW ((HRESULT)0x8002000A)
#define TYPE_E_LIBNOTREGISTERED ((HRESULT)0x8002801D)
#define TYPE_E_CANTLOADLIBRARY ((HRESULT)0x80029C4A)
// Type libraries are not read by this runtime (and none is registered: there is no RegisterTypeLib here), so loading
// one fails the way a missing one does; callers (ATL's IDispatch helpers) report the failure through IDispatch.
static WINAPI HRESULT LoadTypeLib(const WCHAR *file, void **lib) {
    if (!file || !lib) return E_INVALIDARG;
    *lib = NULL;
    return TYPE_E_CANTLOADLIBRARY;
}
static WINAPI HRESULT LoadRegTypeLib(const void *guid, uint16_t major, uint16_t minor, DWORD lcid, void **lib) {
    (void)major; (void)minor; (void)lcid;
    if (!guid || !lib) return E_INVALIDARG;
    *lib = NULL;
    return TYPE_E_LIBNOTREGISTERED;
}
// VarBstrCmp: VARCMP_LT 0, VARCMP_EQ 1, VARCMP_GT 2, compared as CompareString compares with that locale and those
// flags. A NULL BSTR is an empty string (the BSTR convention), so VARCMP_NULL never comes back.
static WINAPI HRESULT VarBstrCmp(const WCHAR *left, const WCHAR *right, DWORD lcid, DWORD flags) {
    static const WCHAR empty[1] = { 0 };
    int nl = left ? (int)(*(const uint32_t *)((const uint8_t *)left - 4) / 2) : 0;
    int nr = right ? (int)(*(const uint32_t *)((const uint8_t *)right - 4) / 2) : 0;
    int r = rt_CompareStringW ? rt_CompareStringW(lcid, flags, left ? left : empty, nl, right ? right : empty, nr) : 0;
    if (!r) return E_INVALIDARG;
    return r - 1;                                                      // CSTR_LESS_THAN 1, CSTR_EQUAL 2, CSTR_GREATER_THAN 3
}
// VarUI4FromStr: a number in text (spaces around it, a sign, "," between thousands, a fraction, an exponent, or "&H" hex
// and "&O" octal) rounded to an unsigned 32-bit integer, halves to even as variant conversions round. Too large or
// negative: DISP_E_OVERFLOW; not a number: DISP_E_TYPEMISMATCH. The separators are the English ones ("." and ",").
static int is_space_w(WCHAR c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f' || c == 0xa0; }
static WINAPI HRESULT VarUI4FromStr(const WCHAR *s, DWORD lcid, DWORD flags, DWORD *out) {
    (void)lcid; (void)flags;
    if (!s || !out) return E_INVALIDARG;
    while (is_space_w(*s)) s++;
    int negative = 0;
    if (*s == '+' || *s == '-') negative = *s++ == '-';
    uint64_t value = 0;
    int any = 0;
    if (*s == '&' && (s[1] == 'h' || s[1] == 'H' || s[1] == 'o' || s[1] == 'O')) {
        int hex = s[1] == 'h' || s[1] == 'H';
        for (s += 2;; s++) {
            int d = *s >= '0' && *s <= '9' ? *s - '0' : hex && *s >= 'a' && *s <= 'f' ? *s - 'a' + 10 : hex && *s >= 'A' && *s <= 'F' ? *s - 'A' + 10 : -1;
            if (d < 0 || (!hex && d > 7)) break;
            value = value * (hex ? 16 : 8) + (uint64_t)d; any = 1;
            if (value > 0xffffffffu) value = 0x100000000ull;           // (stays too large)
        }
    } else {
        // The significant digits (from the first non-zero one) and where the point falls among them: the value is
        // 0.d1d2d3... * 10^whole. Digits past those kept only matter as "something non-zero follows".
        char digits[64]; int n = 0, whole = 0, point = 0, seen = 0, tail = 0;
        for (;; s++) {
            if (*s >= '0' && *s <= '9') {
                seen = 1;
                if (!n && *s == '0') { if (point) whole--; continue; }  // a leading zero: after the point it scales
                if (n < (int)sizeof digits) digits[n++] = (char)*s; else if (*s != '0') tail = 1;
                if (!point) whole++;
                continue;
            }
            if (*s == ',' && !point && seen) continue;
            if (*s == '.' && !point) { point = 1; continue; }
            break;
        }
        if (!seen) return DISP_E_TYPEMISMATCH;
        any = 1;
        if ((*s == 'e' || *s == 'E') && ((s[1] >= '0' && s[1] <= '9') || ((s[1] == '+' || s[1] == '-') && s[2] >= '0' && s[2] <= '9'))) {
            int sign = 1, e = 0; s++;
            if (*s == '+' || *s == '-') sign = *s++ == '-' ? -1 : 1;
            while (*s >= '0' && *s <= '9') { if (e < 100000) e = e * 10 + (*s - '0'); s++; }
            whole += sign * e;
        }
        if (n && whole > 10) value = 0x100000000ull;                   // 11 digits or more
        else if (n && whole >= 0) {
            for (int i = 0; i < whole; i++) value = value * 10 + (uint64_t)(i < n ? digits[i] - '0' : 0);
            int first = whole < n ? digits[whole] - '0' : 0, rest = tail;
            for (int i = whole + 1; i < n; i++) if (digits[i] != '0') rest = 1;
            if (first > 5 || (first == 5 && (rest || (value & 1)))) value++;
        }                                                              // (whole < 0: below 0.1, rounds to 0)
    }
    while (is_space_w(*s)) s++;
    if (!any || *s) return DISP_E_TYPEMISMATCH;
    if (value > 0xffffffffu || (negative && value)) return DISP_E_OVERFLOW;
    *out = (DWORD)value;
    return S_OK;
}
// Variant dates: days since 30 December 1899 (proleptic Gregorian); the fraction is the time of day, also for
// negative dates (-1.25 is 29 December 1899, 06:00). Valid from 1 January 100 to 31 December 9999. Conversions
// to SYSTEMTIME round to the nearest second (milliseconds are 0).
typedef struct { uint16_t year, month, day_of_week, day, hour, minute, second, milliseconds; } SYSTEMTIME;
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {     // days since 1970-01-01
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400), doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}
static void civil_from_days(int64_t z, int64_t *y, unsigned *m, unsigned *d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097), yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int64_t)yoe + era * 400 + (*m <= 2);
}
enum { OLE_EPOCH = -25569 };                                           // 1899-12-30 in days since 1970-01-01
static WINAPI int VariantTimeToSystemTime(double vtime, SYSTEMTIME *st) {
    if (!st || !(vtime >= -657434.0 && vtime < 2958466.0)) return 0;
    double whole = trunc(vtime);
    int64_t day = (int64_t)whole, secs = (int64_t)llround(fabs(vtime - whole) * 86400.0);
    if (secs >= 86400) { secs -= 86400; day++; }
    int64_t unix_day = day + OLE_EPOCH, y; unsigned m, d;
    civil_from_days(unix_day, &y, &m, &d);
    if (y < 100 || y > 9999) return 0;
    st->year = (uint16_t)y; st->month = (uint16_t)m; st->day = (uint16_t)d;
    st->day_of_week = (uint16_t)(((unix_day % 7) + 7 + 4) % 7);       // 1970-01-01 was a Thursday
    st->hour = (uint16_t)(secs / 3600); st->minute = (uint16_t)(secs / 60 % 60); st->second = (uint16_t)(secs % 60); st->milliseconds = 0;
    return 1;
}
static WINAPI int SystemTimeToVariantTime(const SYSTEMTIME *st, double *vtime) {
    static const uint8_t month_days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (!st || !vtime || st->year < 100 || st->year > 9999 || st->month < 1 || st->month > 12 || st->hour > 23 || st->minute > 59 || st->second > 59) return 0;
    int leap = (st->year % 4 == 0 && st->year % 100) || st->year % 400 == 0;
    if (st->day < 1 || st->day > month_days[st->month - 1] + (st->month == 2 && leap)) return 0;
    double day = (double)(days_from_civil(st->year, st->month, st->day) - OLE_EPOCH);
    double time = (st->hour * 3600 + st->minute * 60 + st->second) / 86400.0;
    *vtime = day >= 0 ? day + time : day - time;
    return 1;
}

// ---- registration -------------------------------------------------------------------------------------------------------
static const vx_export userenv[] = {
    VX_EXPORT(CreateEnvironmentBlock), VX_EXPORT(DestroyEnvironmentBlock), VX_EXPORT(CreateAppContainerProfile),
    VX_EXPORT(DeriveAppContainerSidFromAppContainerName), VX_EXPORT(DeleteAppContainerProfile), VX_EXPORT(EnterCriticalPolicySection),
    VX_EXPORT(LeaveCriticalPolicySection), VX_EXPORT(RegisterGPNotification), VX_EXPORT(UnregisterGPNotification),
};
static const vx_export winspool[] = {
    VX_EXPORT(EnumPrintersW), VX_EXPORT(EnumPrintersA), VX_EXPORT(OpenPrinterW), VX_EXPORT(OpenPrinterA), VX_EXPORT(GetPrinterW),
    VX_EXPORT(GetPrinterA), VX_EXPORT(GetPrinterDriverW), VX_EXPORT(GetPrinterDriverA), VX_EXPORT(ClosePrinter),
    VX_EXPORT(DocumentPropertiesW), VX_EXPORT(DocumentPropertiesA), VX_EXPORT(DeviceCapabilitiesW), VX_EXPORT(DeviceCapabilitiesA),
    VX_EXPORT(GetDefaultPrinterW), VX_EXPORT(GetDefaultPrinterA),
    { "#203", (void *)GetDefaultPrinterW },        // libcef.dll imports it by ordinal (a buffer of 260 characters, a size)
};
static const vx_export dhcpcsvc[] = { VX_EXPORT(DhcpCApiInitialize), VX_EXPORT(DhcpCApiCleanup), VX_EXPORT(DhcpRequestParams) };
static const vx_export secur32[] = {
    VX_EXPORT(GetUserNameExW), VX_EXPORT(GetUserNameExA), VX_EXPORT(LsaConnectUntrusted), VX_EXPORT(LsaRegisterLogonProcess),
    VX_EXPORT(LsaDeregisterLogonProcess), VX_EXPORT(LsaLookupAuthenticationPackage), VX_EXPORT(LsaLogonUser),
    VX_EXPORT(LsaCallAuthenticationPackage), VX_EXPORT(LsaFreeReturnBuffer), VX_EXPORT(QuerySecurityPackageInfoW),
    VX_EXPORT(QuerySecurityPackageInfoA), VX_EXPORT(EnumerateSecurityPackagesW), VX_EXPORT(EnumerateSecurityPackagesA),
    VX_EXPORT(InitSecurityInterfaceW), VX_EXPORT(InitSecurityInterfaceA),
};
static vx_export oleaut32[] = {
    { "#77", NULL },                               // SafeArrayGetVartype (com.c), filled in below
    VX_EXPORT(LoadTypeLib), { "#161", (void *)LoadTypeLib }, VX_EXPORT(LoadRegTypeLib), { "#162", (void *)LoadRegTypeLib },
    VX_EXPORT(VariantTimeToSystemTime), { "#185", (void *)VariantTimeToSystemTime }, VX_EXPORT(SystemTimeToVariantTime),
    VX_EXPORT(VarUI4FromStr), { "#277", (void *)VarUI4FromStr }, VX_EXPORT(VarBstrCmp), { "#314", (void *)VarBstrCmp },
};

#define RT(dll, fn) (rt_##fn = (__typeof__(rt_##fn))vx_find_export(dll, #fn))
void vx_sysdlls_init(void) {                       // (after kernel32, advapi32 and COM have registered theirs)
    RT("advapi32.dll", GetUserNameW); RT("kernel32.dll", GetComputerNameW);
    RT("kernel32.dll", GetEnvironmentStringsW); RT("kernel32.dll", FreeEnvironmentStringsW); RT("kernel32.dll", CreateMutexW);
    RT("kernel32.dll", WaitForSingleObject); RT("kernel32.dll", ReleaseMutex); RT("kernel32.dll", CloseHandle);
    RT("advapi32.dll", RegCreateKeyExW); RT("advapi32.dll", RegSetValueExW); RT("advapi32.dll", RegDeleteKeyW);
    RT("advapi32.dll", RegCloseKey); RT("kernel32.dll", CompareStringW);
    oleaut32[0].fn = vx_find_export("oleaut32.dll", "SafeArrayGetVartype");
    vx_register_module("userenv.dll", userenv, sizeof userenv / sizeof userenv[0]);
    vx_register_module("winspool.drv", winspool, sizeof winspool / sizeof winspool[0]);
    vx_register_module("dhcpcsvc.dll", dhcpcsvc, sizeof dhcpcsvc / sizeof dhcpcsvc[0]);
    vx_register_module("secur32.dll", secur32, sizeof secur32 / sizeof secur32[0]);
    vx_register_module("sspicli.dll", secur32, sizeof secur32 / sizeof secur32[0]);   // (secur32 forwards these to sspicli)
    vx_register_module("oleaut32.dll", oleaut32, sizeof oleaut32 / sizeof oleaut32[0]);
}

#ifdef __aarch64__
#include "thunks/sysdlls.c.inc"
#endif
