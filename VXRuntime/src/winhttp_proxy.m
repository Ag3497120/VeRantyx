// winhttp.dll beyond winhttp.m's synchronous requests: WinHttpCrackUrl, status callbacks (WinHttpSetStatusCallback)
// and the asynchronous proxy resolver of Windows 8 (WinHttpCreateProxyResolver, WinHttpGetProxyForUrlEx,
// WinHttpGetProxyResult, WinHttpFreeProxyResult), which Steam's Chromium imports.
//
// The resolver answers from macOS: a PAC file (WINHTTP_AUTOPROXY_CONFIG_URL, or the Mac's own with ALLOW_AUTOCONFIG)
// is downloaded and run by CFNetwork, the static settings (ALLOW_STATIC) are the Mac's network proxy settings, and
// auto-detection (WPAD) finds a PAC file only where CFNetwork reports one for the URL. The answer is the PAC's list:
// proxies and DIRECT entries, in order.
//
// WinHTTP wants the resolver's session opened with WINHTTP_FLAG_ASYNC; winhttp.m opens synchronous sessions only (it
// refuses the flag), and the resolver is asynchronous by itself, so any session is taken.
#import <Foundation/Foundation.h>
#import <CFNetwork/CFNetwork.h>
#import <SystemConfiguration/SystemConfiguration.h>
#include "vx.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef int32_t WINBOOL;
enum {
    ERROR_INVALID_HANDLE = 6, ERROR_NOT_ENOUGH_MEMORY = 8, ERROR_INVALID_PARAMETER = 87, ERROR_INSUFFICIENT_BUFFER = 122,
    ERROR_IO_PENDING = 997, ERROR_WINHTTP_INVALID_URL = 12005, ERROR_WINHTTP_UNRECOGNIZED_SCHEME = 12006,
    ERROR_WINHTTP_OPERATION_CANCELLED = 12017, ERROR_WINHTTP_INCORRECT_HANDLE_TYPE = 12018,
    ERROR_WINHTTP_INCORRECT_HANDLE_STATE = 12019, ERROR_WINHTTP_BAD_AUTO_PROXY_SCRIPT = 12166,
    ERROR_WINHTTP_UNABLE_TO_DOWNLOAD_SCRIPT = 12167, ERROR_WINHTTP_AUTO_PROXY_SERVICE_ERROR = 12178,
    ERROR_WINHTTP_AUTODETECTION_FAILED = 12180,
};
enum { INTERNET_SCHEME_HTTP = 1, INTERNET_SCHEME_HTTPS = 2, INTERNET_SCHEME_SOCKS = 4 };
enum { WH_SESSION = 1, WH_REQUEST = 3 };                            // winhttp.m's handle kinds (their first word)

// ---- WinHttpCrackUrl ------------------------------------------------------------------------------------------------------
typedef struct {
    DWORD dwStructSize; WCHAR *lpszScheme; DWORD dwSchemeLength; int32_t nScheme;
    WCHAR *lpszHostName; DWORD dwHostNameLength; uint16_t nPort;
    WCHAR *lpszUserName; DWORD dwUserNameLength; WCHAR *lpszPassword; DWORD dwPasswordLength;
    WCHAR *lpszUrlPath; DWORD dwUrlPathLength; WCHAR *lpszExtraInfo; DWORD dwExtraInfoLength;
} url_components;
_Static_assert(sizeof(url_components) == 104 && __builtin_offsetof(url_components, lpszUrlPath) == 72, "URL_COMPONENTS");
#define ICU_ESCAPE 0x80000000u
#define ICU_DECODE 0x10000000u
#define ICU_REJECT_USERPWD 0x00004000u

static int is_alpha(WCHAR c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static int same_text(const WCHAR *w, size_t n, const char *a) {          // ASCII, ignoring case
    if (strlen(a) != n) return 0;
    for (size_t i = 0; i < n; i++) if ((w[i] | 0x20) != a[i]) return 0;
    return 1;
}
static int hex_value(WCHAR c) { return c >= '0' && c <= '9' ? c - '0' : (c | 0x20) >= 'a' && (c | 0x20) <= 'f' ? (c | 0x20) - 'a' + 10 : -1; }

// A component copied into the caller's buffer, with %xx decoded (ICU_DECODE: each to the character of that code,
// UTF-8 is not decoded) and/or characters escaped (ICU_ESCAPE: controls, space, DEL and non-ASCII, the last as their
// UTF-8 bytes). Returns the length it has (without the terminator), whether or not it fitted.
static size_t transform(const WCHAR *in, size_t n, DWORD flags, WCHAR *out, size_t cap) {
    size_t k = 0;
#define PUT(c) do { if (k < cap) out[k] = (WCHAR)(c); k++; } while (0)
    for (size_t i = 0; i < n; i++) {
        uint32_t c = in[i];
        if ((flags & ICU_DECODE) && c == '%' && i + 2 < n && hex_value(in[i + 1]) >= 0 && hex_value(in[i + 2]) >= 0) {
            c = (uint32_t)(hex_value(in[i + 1]) * 16 + hex_value(in[i + 2]));
            i += 2;
        }
        if ((flags & ICU_ESCAPE) && (c <= 0x20 || c >= 0x7f)) {
            uint8_t bytes[4];
            int nb = 0;
            if (c >= 0xd800 && c < 0xdc00 && i + 1 < n && in[i + 1] >= 0xdc00 && in[i + 1] < 0xe000) { c = 0x10000 + ((c - 0xd800) << 10) + (in[i + 1] - 0xdc00); i++; }
            if (c < 0x80) bytes[nb++] = (uint8_t)c;
            else if (c < 0x800) { bytes[nb++] = (uint8_t)(0xc0 | c >> 6); bytes[nb++] = (uint8_t)(0x80 | (c & 0x3f)); }
            else if (c < 0x10000) { bytes[nb++] = (uint8_t)(0xe0 | c >> 12); bytes[nb++] = (uint8_t)(0x80 | (c >> 6 & 0x3f)); bytes[nb++] = (uint8_t)(0x80 | (c & 0x3f)); }
            else { bytes[nb++] = (uint8_t)(0xf0 | c >> 18); bytes[nb++] = (uint8_t)(0x80 | (c >> 12 & 0x3f)); bytes[nb++] = (uint8_t)(0x80 | (c >> 6 & 0x3f)); bytes[nb++] = (uint8_t)(0x80 | (c & 0x3f)); }
            for (int b = 0; b < nb; b++) { PUT('%'); PUT("0123456789ABCDEF"[bytes[b] >> 4]); PUT("0123456789ABCDEF"[bytes[b] & 15]); }
        } else PUT(c);
    }
#undef PUT
    return k;
}
// One component, as URL_COMPONENTS asks for it: (NULL, 0) not at all; (NULL, nonzero) a pointer into the URL and the
// length; (buffer, size) a copy, its length without the terminator back in the length (ERROR_INSUFFICIENT_BUFFER when
// it does not fit, the size needed then in the length).
static DWORD give_component(const WCHAR *url, const WCHAR *start, size_t n, DWORD flags, int transformable, WCHAR **ptr, DWORD *len) {
    if (!*len) return 0;
    if (!*ptr) { *ptr = start ? (WCHAR *)start : NULL; *len = start ? (DWORD)n : 0; (void)url; return 0; }
    DWORD f = transformable ? flags : 0;
    size_t need = start ? transform(start, n, f, NULL, 0) : 0;
    if (need + 1 > *len) { *len = (DWORD)need + 1; return ERROR_INSUFFICIENT_BUFFER; }
    if (start) transform(start, n, f, *ptr, need);
    (*ptr)[need] = 0;
    *len = (DWORD)need;
    return 0;
}
static WINAPI WINBOOL WinHttpCrackUrl(const WCHAR *url, DWORD length, DWORD flags, url_components *parts) {
    if (!url || !parts || parts->dwStructSize != sizeof(url_components)) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    size_t n = length ? length : vx_wcslen(url);
    // scheme ":" "//" authority, then the path, then the extra information ("?..." and/or "#...")
    size_t colon = 0;
    while (colon < n && url[colon] != ':' && (is_alpha(url[colon]) || (colon && ((url[colon] >= '0' && url[colon] <= '9') ||
           url[colon] == '+' || url[colon] == '-' || url[colon] == '.')))) colon++;
    if (!colon || colon >= n || url[colon] != ':') { vx_set_last_error(ERROR_WINHTTP_UNRECOGNIZED_SCHEME); return 0; }
    int scheme = same_text(url, colon, "http") ? INTERNET_SCHEME_HTTP : same_text(url, colon, "https") ? INTERNET_SCHEME_HTTPS : 0;
    if (!scheme) { vx_set_last_error(ERROR_WINHTTP_UNRECOGNIZED_SCHEME); return 0; }
    if (colon + 3 > n || url[colon + 1] != '/' || url[colon + 2] != '/') { vx_set_last_error(ERROR_WINHTTP_INVALID_URL); return 0; }
    size_t authority = colon + 3, end = authority;
    while (end < n && url[end] != '/' && url[end] != '?' && url[end] != '#') end++;
    size_t at = end;                                                // the last '@' of the authority ends the user information
    for (size_t i = authority; i < end; i++) if (url[i] == '@') at = i;
    const WCHAR *user = NULL, *password = NULL;
    size_t user_n = 0, password_n = 0, host = authority;
    if (at < end) {
        user = url + authority;
        size_t i = authority;
        while (i < at && url[i] != ':') i++;
        user_n = i - authority;
        if (i < at) { password = url + i + 1; password_n = at - i - 1; }
        host = at + 1;
        if (flags & ICU_REJECT_USERPWD) { vx_set_last_error(ERROR_WINHTTP_INVALID_URL); return 0; }
    }
    size_t host_end = host;
    if (host < end && url[host] == '[') {                           // an IPv6 literal, brackets included (RFC 3986's host)
        while (host_end < end && url[host_end] != ']') host_end++;
        if (host_end >= end) { vx_set_last_error(ERROR_WINHTTP_INVALID_URL); return 0; }
        host_end++;
    } else while (host_end < end && url[host_end] != ':') host_end++;
    if (host_end == host) { vx_set_last_error(ERROR_WINHTTP_INVALID_URL); return 0; }
    uint32_t port = scheme == INTERNET_SCHEME_HTTPS ? 443 : 80;
    if (host_end < end) {
        if (url[host_end] != ':') { vx_set_last_error(ERROR_WINHTTP_INVALID_URL); return 0; }
        if (host_end + 1 < end) {
            port = 0;
            for (size_t i = host_end + 1; i < end; i++) {
                if (url[i] < '0' || url[i] > '9' || (port = port * 10 + (url[i] - '0')) > 65535) { vx_set_last_error(ERROR_WINHTTP_INVALID_URL); return 0; }
            }
        }
    }
    size_t extra = end;
    while (extra < n && url[extra] != '?' && url[extra] != '#') extra++;
    parts->nScheme = scheme;
    parts->nPort = (uint16_t)port;
    // without an extra-information component asked for, the path carries the query and fragment (documented)
    int extra_wanted = parts->lpszExtraInfo || parts->dwExtraInfoLength;
    DWORD e = 0, r;
    if ((r = give_component(url, url, colon, flags, 0, &parts->lpszScheme, &parts->dwSchemeLength))) e = r;
    if ((r = give_component(url, url + host, host_end - host, flags, 1, &parts->lpszHostName, &parts->dwHostNameLength))) e = r;
    if ((r = give_component(url, user, user_n, flags, 1, &parts->lpszUserName, &parts->dwUserNameLength))) e = r;
    if ((r = give_component(url, password, password_n, flags, 1, &parts->lpszPassword, &parts->dwPasswordLength))) e = r;
    if ((r = give_component(url, url + end, (extra_wanted ? extra : n) - end, flags, 1, &parts->lpszUrlPath, &parts->dwUrlPathLength))) e = r;
    if ((r = give_component(url, url + extra, n - extra, flags, 1, &parts->lpszExtraInfo, &parts->dwExtraInfoLength))) e = r;
    if (e) { vx_set_last_error(e); return 0; }
    return 1;
}

// ---- status callbacks and resolver handles ----------------------------------------------------------------------------------
typedef void (WINAPI *status_callback)(HANDLE, uintptr_t, DWORD, void *, DWORD);
#define WINHTTP_INVALID_STATUS_CALLBACK ((status_callback)(intptr_t)-1)
enum {
    STATUS_HANDLE_CLOSING = 0x800, STATUS_REQUEST_ERROR = 0x200000, STATUS_GETPROXYFORURL_COMPLETE = 0x1000000,
    API_GET_PROXY_FOR_URL = 6,
};
// The callbacks of winhttp.m's handles (it keeps none itself): set here, handed to a resolver made from a session,
// and told of the handle's closing (vx_winhttp_proxy_close, from its WinHttpCloseHandle).
typedef struct { HANDLE handle; status_callback callback; DWORD flags; } callback_slot;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static callback_slot *g_slots;
static int g_slot_count, g_slot_cap;

enum { RESOLVER_KIND = 0x56585252 };                                // "RRXV": not one of winhttp.m's kinds
typedef struct { int proxy, bypass, scheme; uint16_t port; char host[256]; } proxy_entry;
typedef struct resolver {
    uint32_t kind;                                                  // (first, as winhttp.m's handles: it reads this word)
    struct resolver *next;
    status_callback callback;
    DWORD flags;
    int busy, closed, has_result, entry_count;
    proxy_entry *entries;
} resolver;
static resolver *g_resolvers;
static void **g_results;                                            // the entry arrays WinHttpGetProxyResult gave out
static int g_result_count, g_result_cap;

static resolver *find_resolver(HANDLE h) {                          // with g_lock held
    for (resolver *r = g_resolvers; r; r = r->next) if (r == h) return r;
    return NULL;
}
static int winhttp_handle(HANDLE h) {                               // one of winhttp.m's session/connection/request handles
    if (!h || find_resolver(h)) return 0;
    uint32_t kind = *(const uint32_t *)h;
    return kind >= WH_SESSION && kind <= WH_REQUEST;
}
static callback_slot *find_slot(HANDLE h) {                         // with g_lock held
    for (int i = 0; i < g_slot_count; i++) if (g_slots[i].handle == h) return &g_slots[i];
    return NULL;
}

static WINAPI status_callback WinHttpSetStatusCallback(HANDLE h, status_callback callback, DWORD flags, uintptr_t reserved) {
    (void)reserved;
    if (!h) { vx_set_last_error(ERROR_INVALID_HANDLE); return WINHTTP_INVALID_STATUS_CALLBACK; }
    pthread_mutex_lock(&g_lock);
    status_callback previous = NULL;
    resolver *r = find_resolver(h);
    if (r) {
        previous = r->callback;
        r->callback = callback; r->flags = flags;
    } else if (winhttp_handle(h)) {
        callback_slot *s = find_slot(h);
        if (s) previous = s->callback;
        if (!s && callback) {
            if (g_slot_count == g_slot_cap) {
                int cap = g_slot_cap ? g_slot_cap * 2 : 16;
                callback_slot *grown = realloc(g_slots, (size_t)cap * sizeof *grown);
                if (!grown) { pthread_mutex_unlock(&g_lock); vx_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return WINHTTP_INVALID_STATUS_CALLBACK; }
                g_slots = grown; g_slot_cap = cap;
            }
            s = &g_slots[g_slot_count++];
            s->handle = h;
        }
        if (s && callback) { s->callback = callback; s->flags = flags; }
        else if (s) *s = g_slots[--g_slot_count];                  // removed
    } else {
        pthread_mutex_unlock(&g_lock);
        vx_set_last_error(ERROR_WINHTTP_INCORRECT_HANDLE_TYPE);
        return WINHTTP_INVALID_STATUS_CALLBACK;
    }
    pthread_mutex_unlock(&g_lock);
    return previous;
}

// The resolver's callback, if it asked for this status (read now: the program may change it meanwhile).
static void notify(resolver *r, uintptr_t context, DWORD status, void *info, DWORD info_length) {
    pthread_mutex_lock(&g_lock);
    status_callback callback = r->flags & status ? r->callback : NULL;
    pthread_mutex_unlock(&g_lock);
    if (callback) callback((HANDLE)r, context, status, info, info_length);
}
static void closing(resolver *r) {                                  // the last notification; the resolver goes
    HANDLE handle = r;
    notify(r, 0, STATUS_HANDLE_CLOSING, &handle, sizeof handle);
    free(r->entries);
    free(r);
}

static WINAPI DWORD WinHttpCreateProxyResolver(HANDLE session, HANDLE *out) {
    if (!out) return ERROR_INVALID_PARAMETER;
    *out = NULL;
    if (!session) return ERROR_INVALID_HANDLE;
    pthread_mutex_lock(&g_lock);
    if (!winhttp_handle(session) || *(const uint32_t *)session != WH_SESSION) { pthread_mutex_unlock(&g_lock); return ERROR_WINHTTP_INCORRECT_HANDLE_TYPE; }
    resolver *r = calloc(1, sizeof *r);
    if (!r) { pthread_mutex_unlock(&g_lock); return ERROR_NOT_ENOUGH_MEMORY; }
    r->kind = RESOLVER_KIND;
    callback_slot *s = find_slot(session);                          // (set on the session before, as documented)
    if (s) { r->callback = s->callback; r->flags = s->flags; }
    r->next = g_resolvers; g_resolvers = r;
    pthread_mutex_unlock(&g_lock);
    *out = r;
    return 0;
}

// ---- resolving ----------------------------------------------------------------------------------------------------------------
typedef struct { int done; CFArrayRef list; CFErrorRef error; } pac_state;
static void pac_finished(void *client, CFArrayRef list, CFErrorRef error) {
    pac_state *s = client;
    s->done = 1;
    if (error) s->error = (CFErrorRef)CFRetain(error);
    else if (list) s->list = (CFArrayRef)CFRetain(list);
}
// Downloads and runs a PAC file on this thread's run loop (up to 30 s; sooner when the resolver is closed).
static NSArray *run_pac(NSURL *pac, NSURL *target, resolver *r, DWORD *error) {
    pac_state state = { 0 };
    CFStreamClientContext context = { 0, &state, NULL, NULL, NULL };
    CFRunLoopSourceRef source = CFNetworkExecuteProxyAutoConfigurationURL((__bridge CFURLRef)pac, (__bridge CFURLRef)target, pac_finished, &context);
    if (!source) { *error = ERROR_WINHTTP_UNABLE_TO_DOWNLOAD_SCRIPT; return nil; }
    CFRunLoopAddSource(CFRunLoopGetCurrent(), source, kCFRunLoopDefaultMode);
    struct timespec start; clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        pthread_mutex_lock(&g_lock);
        int closed = r->closed;
        pthread_mutex_unlock(&g_lock);
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        if (state.done || closed || now.tv_sec - start.tv_sec >= 30) break;
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.25, true);
    }
    CFRunLoopSourceInvalidate(source);
    CFRelease(source);
    if (state.list) return CFBridgingRelease(state.list);
    if (state.error) {
        // a script that does not run is a bad script; anything else kept the script from arriving
        *error = CFErrorGetCode(state.error) == kCFErrorPACFileError ? ERROR_WINHTTP_BAD_AUTO_PROXY_SCRIPT : ERROR_WINHTTP_UNABLE_TO_DOWNLOAD_SCRIPT;
        CFRelease(state.error);
    } else *error = ERROR_WINHTTP_UNABLE_TO_DOWNLOAD_SCRIPT;        // (timed out, or closed meanwhile)
    return nil;
}
// The first PAC location in a CFNetwork answer, if any.
static NSURL *pac_location(NSArray *list) {
    for (NSDictionary *proxy in list) {
        if (![proxy isKindOfClass:[NSDictionary class]]) continue;
        if ([proxy[(__bridge NSString *)kCFProxyTypeKey] isEqual:(__bridge NSString *)kCFProxyTypeAutoConfigurationURL]) {
            id url = proxy[(__bridge NSString *)kCFProxyAutoConfigurationURLKey];
            if ([url isKindOfClass:[NSURL class]]) return url;
            if ([url isKindOfClass:[NSString class]]) return [NSURL URLWithString:url];
        }
    }
    return nil;
}
static int to_entries(NSArray *list, int bypass, proxy_entry **out) {
    proxy_entry *entries = calloc(list.count + 1, sizeof *entries);
    int n = 0;
    for (NSDictionary *proxy in list) {
        if (!entries || ![proxy isKindOfClass:[NSDictionary class]]) continue;
        NSString *type = proxy[(__bridge NSString *)kCFProxyTypeKey];
        proxy_entry *e = &entries[n];
        if ([type isEqual:(__bridge NSString *)kCFProxyTypeNone]) { e->bypass = bypass; n++; continue; }
        int socks = [type isEqual:(__bridge NSString *)kCFProxyTypeSOCKS];
        if (!socks && ![type isEqual:(__bridge NSString *)kCFProxyTypeHTTP] && ![type isEqual:(__bridge NSString *)kCFProxyTypeHTTPS]) continue;
        NSString *host = proxy[(__bridge NSString *)kCFProxyHostNameKey];
        NSNumber *port = proxy[(__bridge NSString *)kCFProxyPortNumberKey];
        if (![host isKindOfClass:[NSString class]] || !host.length) continue;
        e->proxy = 1;
        e->scheme = socks ? INTERNET_SCHEME_SOCKS : INTERNET_SCHEME_HTTP;   // (a PAC "PROXY" is an HTTP proxy, for https URLs too)
        e->port = [port isKindOfClass:[NSNumber class]] && port.intValue > 0 && port.intValue <= 65535 ? (uint16_t)port.intValue : socks ? 1080 : 80;
        snprintf(e->host, sizeof e->host, "%s", host.UTF8String);
        n++;
    }
    *out = entries;
    return entries ? n : -1;
}
enum { AUTOPROXY_AUTO_DETECT = 0x1, AUTOPROXY_CONFIG_URL = 0x2, AUTOPROXY_ALLOW_AUTOCONFIG = 0x100, AUTOPROXY_ALLOW_STATIC = 0x200 };
static DWORD resolve(resolver *r, NSURL *target, DWORD flags, NSURL *config, proxy_entry **entries, int *count) {
    DWORD error = ERROR_WINHTTP_AUTO_PROXY_SERVICE_ERROR;
    NSArray *list = nil;
    int bypass = 0;
    NSDictionary *system = CFBridgingRelease(SCDynamicStoreCopyProxies(NULL));
    if (![system isKindOfClass:[NSDictionary class]]) system = @{};
    if (flags & AUTOPROXY_AUTO_DETECT) {
        NSDictionary *detect = @{ (__bridge NSString *)kCFNetworkProxiesProxyAutoDiscoveryEnable: @1 };
        NSURL *found = pac_location(CFBridgingRelease(CFNetworkCopyProxiesForURL((__bridge CFURLRef)target, (__bridge CFDictionaryRef)detect)));
        if (found) list = run_pac(found, target, r, &error);
        if (!list) error = ERROR_WINHTTP_AUTODETECTION_FAILED;
    }
    if (!list && config) list = run_pac(config, target, r, &error);
    if (!list && (flags & AUTOPROXY_ALLOW_AUTOCONFIG) && [system[(__bridge NSString *)kSCPropNetProxiesProxyAutoConfigEnable] intValue]) {
        id text = system[(__bridge NSString *)kSCPropNetProxiesProxyAutoConfigURLString];
        NSURL *pac = [text isKindOfClass:[NSString class]] ? [NSURL URLWithString:text] : nil;
        if (pac) list = run_pac(pac, target, r, &error);
    }
    if (!list && (flags & AUTOPROXY_ALLOW_STATIC)) {
        NSMutableDictionary *fixed = [system mutableCopy];          // the static part of the Mac's settings
        [fixed removeObjectsForKeys:@[ (__bridge NSString *)kSCPropNetProxiesProxyAutoConfigEnable,
                                       (__bridge NSString *)kSCPropNetProxiesProxyAutoConfigURLString,
                                       (__bridge NSString *)kSCPropNetProxiesProxyAutoDiscoveryEnable ]];
        list = CFBridgingRelease(CFNetworkCopyProxiesForURL((__bridge CFURLRef)target, (__bridge CFDictionaryRef)fixed));
        // DIRECT although a proxy is set: the host is one of the exceptions
        bypass = [fixed[(__bridge NSString *)kSCPropNetProxiesHTTPEnable] intValue] || [fixed[(__bridge NSString *)kSCPropNetProxiesHTTPSEnable] intValue] ||
                 [fixed[(__bridge NSString *)kSCPropNetProxiesSOCKSEnable] intValue];
    }
    if (!list) return error;
    int n = to_entries(list, bypass, entries);
    if (n < 0) return ERROR_NOT_ENOUGH_MEMORY;
    if (!n) { free(*entries); *entries = NULL; return ERROR_WINHTTP_AUTO_PROXY_SERVICE_ERROR; }
    *count = n;
    return 0;
}
typedef struct { resolver *r; NSURL *target, *config; DWORD flags; uintptr_t context; } resolve_job;
static void *resolve_thread(void *arg) {
    resolve_job *job = arg;
    resolver *r = job->r;
    proxy_entry *entries = NULL;
    int count = 0;
    DWORD error;
    @autoreleasepool { error = resolve(r, job->target, job->flags, job->config, &entries, &count); }
    pthread_mutex_lock(&g_lock);
    int cancelled = r->closed;
    if (!cancelled && !error) { free(r->entries); r->entries = entries; r->entry_count = count; r->has_result = 1; entries = NULL; }
    pthread_mutex_unlock(&g_lock);
    free(entries);
    vx_thread_attach();
    struct { uintptr_t result; DWORD error; } async_result = { API_GET_PROXY_FOR_URL, cancelled ? ERROR_WINHTTP_OPERATION_CANCELLED : error };
    if (!cancelled && !error) notify(r, job->context, STATUS_GETPROXYFORURL_COMPLETE, NULL, 0);
    else notify(r, job->context, STATUS_REQUEST_ERROR, &async_result, sizeof async_result);
    pthread_mutex_lock(&g_lock);
    r->busy = 0;
    int closed = r->closed;                                         // (closed meanwhile, the callback included)
    pthread_mutex_unlock(&g_lock);
    if (closed) closing(r);
    vx_thread_detach();
    job->target = nil; job->config = nil;
    free(job);
    return NULL;
}
typedef struct { DWORD flags, auto_detect_flags; const WCHAR *auto_config_url; void *reserved; DWORD reserved_value; WINBOOL auto_logon; } autoproxy_options;
_Static_assert(sizeof(autoproxy_options) == 32, "WINHTTP_AUTOPROXY_OPTIONS");
static NSURL *url_from(const WCHAR *w) {
    NSString *text = [NSString stringWithCharacters:(const unichar *)w length:vx_wcslen(w)];
    NSURL *url = [NSURL URLWithString:text];
    return url.scheme.length && url.host.length ? url : nil;
}
static WINAPI DWORD WinHttpGetProxyForUrlEx(HANDLE handle, const WCHAR *url, const autoproxy_options *options, uintptr_t context) {
    if (!handle) return ERROR_INVALID_HANDLE;
    if (!url || !options) return ERROR_INVALID_PARAMETER;
    DWORD flags = options->flags;
    if (options->reserved || options->reserved_value ||
        !(flags & (AUTOPROXY_AUTO_DETECT | AUTOPROXY_CONFIG_URL | AUTOPROXY_ALLOW_AUTOCONFIG | AUTOPROXY_ALLOW_STATIC)) ||
        ((flags & AUTOPROXY_AUTO_DETECT) ? !options->auto_detect_flags || (options->auto_detect_flags & ~3u) : options->auto_detect_flags != 0) ||
        ((flags & AUTOPROXY_CONFIG_URL) ? !options->auto_config_url : options->auto_config_url != NULL))
        return ERROR_INVALID_PARAMETER;
    NSURL *target = nil, *config = nil;
    int pac_scheme = 1;
    @autoreleasepool {                                              // (a Windows thread has no pool of its own)
        target = url_from(url);
        if (flags & AUTOPROXY_CONFIG_URL) {
            config = url_from(options->auto_config_url);
            NSString *scheme = config.scheme.lowercaseString;
            pac_scheme = config && ([scheme isEqualToString:@"http"] || [scheme isEqualToString:@"https"]);
        }
    }
    if (!pac_scheme) return ERROR_WINHTTP_UNRECOGNIZED_SCHEME;
    if (!target) return ERROR_WINHTTP_INVALID_URL;
    resolve_job *job = calloc(1, sizeof *job);
    if (!job) return ERROR_NOT_ENOUGH_MEMORY;
    job->target = target; job->config = config; job->flags = flags; job->context = context;
    pthread_mutex_lock(&g_lock);
    resolver *r = find_resolver(handle);
    DWORD e = !r ? ERROR_WINHTTP_INCORRECT_HANDLE_TYPE : r->busy ? ERROR_WINHTTP_INCORRECT_HANDLE_STATE : 0;
    if (!e) { r->busy = 1; r->has_result = 0; job->r = r; }
    pthread_mutex_unlock(&g_lock);
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 4 << 20);
    if (!e && pthread_create(&thread, &attr, resolve_thread, job)) {
        pthread_mutex_lock(&g_lock);
        r->busy = 0;
        int closed = r->closed;
        pthread_mutex_unlock(&g_lock);
        if (closed) closing(r);
        e = ERROR_NOT_ENOUGH_MEMORY;
    }
    pthread_attr_destroy(&attr);
    if (e) { job->target = nil; job->config = nil; free(job); return e; }
    return ERROR_IO_PENDING;                                       // always asynchronous (documented)
}

// WINHTTP_PROXY_RESULT { cEntries; pEntries } with WINHTTP_PROXY_RESULT_ENTRY { fProxy, fBypass, ProxyScheme,
// pwszProxy, ProxyPort } (32 bytes): the entries and their host names in one block, freed by WinHttpFreeProxyResult.
static WINAPI DWORD WinHttpGetProxyResult(HANDLE handle, uint8_t *result) {
    if (!handle) return ERROR_INVALID_HANDLE;
    pthread_mutex_lock(&g_lock);
    resolver *r = find_resolver(handle);
    DWORD e = !r ? ERROR_WINHTTP_INCORRECT_HANDLE_TYPE : !result ? ERROR_INVALID_PARAMETER : !r->has_result ? ERROR_WINHTTP_INCORRECT_HANDLE_STATE : 0;
    if (e) { pthread_mutex_unlock(&g_lock); return e; }
    size_t size = (size_t)r->entry_count * 32;
    for (int i = 0; i < r->entry_count; i++) if (r->entries[i].proxy) size += (strlen(r->entries[i].host) + 1) * sizeof(WCHAR);
    uint8_t *block = vx_heap_zalloc(size);
    if (block && g_result_count == g_result_cap) {
        int cap = g_result_cap ? g_result_cap * 2 : 16;
        void **grown = realloc(g_results, (size_t)cap * sizeof *grown);
        if (grown) { g_results = grown; g_result_cap = cap; }
    }
    if (!block || g_result_count == g_result_cap) { pthread_mutex_unlock(&g_lock); free(block); return ERROR_NOT_ENOUGH_MEMORY; }
    g_results[g_result_count++] = block;
    WCHAR *text = (WCHAR *)(block + (size_t)r->entry_count * 32);
    for (int i = 0; i < r->entry_count; i++) {
        uint8_t *entry = block + 32 * (size_t)i;
        const proxy_entry *p = &r->entries[i];
        *(int32_t *)entry = p->proxy; *(int32_t *)(entry + 4) = p->bypass; *(int32_t *)(entry + 8) = p->scheme;
        if (p->proxy) {
            size_t chars = strlen(p->host) + 1;
            vx_utf8_to_utf16(p->host, chars, text, chars);
            *(WCHAR **)(entry + 16) = text;
            text += chars;
        }
        *(uint16_t *)(entry + 24) = p->port;
    }
    *(DWORD *)result = (DWORD)r->entry_count;
    *(uint8_t **)(result + 8) = block;
    pthread_mutex_unlock(&g_lock);
    return 0;
}
static WINAPI void WinHttpFreeProxyResult(uint8_t *result) {
    if (!result) return;
    void *block = *(void **)(result + 8);
    pthread_mutex_lock(&g_lock);
    int found = 0;
    for (int i = 0; i < g_result_count && !found; i++)
        if (g_results[i] == block) { g_results[i] = g_results[--g_result_count]; found = 1; }
    pthread_mutex_unlock(&g_lock);
    if (!found) return;                                             // (not ours: left alone)
    free(block);
    memset(result, 0, 16);
}

// From winhttp.m's WinHttpCloseHandle, before it looks at the handle: a resolver is closed here (1); for its own
// handles the callback slot goes, after the documented last notification, WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING (0).
int vx_winhttp_proxy_close(HANDLE h) {
    if (!h) return 0;
    pthread_mutex_lock(&g_lock);
    resolver *r = find_resolver(h);
    if (r) {
        resolver **link = &g_resolvers;
        while (*link != r) link = &(*link)->next;
        *link = r->next;
        r->closed = 1;
        int busy = r->busy;
        pthread_mutex_unlock(&g_lock);
        if (!busy) closing(r);                                      // (else its resolution ends with it)
        return 1;
    }
    callback_slot slot = { 0 };
    callback_slot *s = find_slot(h);
    if (s) { slot = *s; *s = g_slots[--g_slot_count]; }
    pthread_mutex_unlock(&g_lock);
    if (slot.callback && (slot.flags & STATUS_HANDLE_CLOSING)) {
        HANDLE handle = h;
        slot.callback(h, 0, STATUS_HANDLE_CLOSING, &handle, sizeof handle);
    }
    return 0;
}

static const vx_export g_winhttp_proxy[] = {
    VX_EXPORT(WinHttpCrackUrl), VX_EXPORT(WinHttpSetStatusCallback), VX_EXPORT(WinHttpCreateProxyResolver),
    VX_EXPORT(WinHttpGetProxyForUrlEx), VX_EXPORT(WinHttpGetProxyResult), VX_EXPORT(WinHttpFreeProxyResult),
};
void vx_winhttp_proxy_init(void) {
    vx_register_module("winhttp.dll", g_winhttp_proxy, sizeof g_winhttp_proxy / sizeof g_winhttp_proxy[0]);
}

#ifdef __aarch64__
#include "thunks/winhttp_proxy.m.inc"
#endif
