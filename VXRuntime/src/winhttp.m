// winhttp.dll: synchronous HTTP(S) requests for Windows programs, on NSURLSession. Handles follow the WinHTTP
// session -> connection -> request shape. Responses are buffered so WinHttpReadData can return them in caller chunks.
//
// A session may be opened with WINHTTP_FLAG_ASYNC: the proxy resolver (winhttp_proxy.m) takes only such a session, as
// documented. Requests in it would have to complete through status callbacks, which this file does not do yet, so
// WinHttpConnect, WinHttpOpenRequest and WinHttpSendRequest refuse an asynchronous handle with ERROR_NOT_SUPPORTED
// rather than run the request synchronously behind the program's back.
#import <Foundation/Foundation.h>
#import <CFNetwork/CFNetwork.h>
#import <SystemConfiguration/SystemConfiguration.h>
#include "vx.h"
#include <stdlib.h>
#include <string.h>

enum { WH_SESSION = 1, WH_CONNECTION, WH_REQUEST };
enum {
    WH_ACCESS_DEFAULT_PROXY = 0,
    WH_ACCESS_NO_PROXY = 1,
    WH_ACCESS_NAMED_PROXY = 3,
    WH_ACCESS_AUTOMATIC_PROXY = 4
};
enum {
    ERROR_WINHTTP_TIMEOUT = 12002,
    ERROR_WINHTTP_INVALID_URL = 12005,
    ERROR_WINHTTP_CANNOT_CONNECT = 12029,
    ERROR_WINHTTP_HEADER_NOT_FOUND = 12150,
    ERROR_INSUFFICIENT_BUFFER = 122,
    ERROR_NOT_ENOUGH_MEMORY = 8
};
#define WINHTTP_FLAG_SECURE 0x00800000u
#define WINHTTP_FLAG_ASYNC 0x10000000u
#define ERROR_NOT_SUPPORTED 50
#define WINHTTP_QUERY_STATUS_CODE 19u
#define WINHTTP_QUERY_STATUS_TEXT 20u
#define WINHTTP_QUERY_RAW_HEADERS 21u
#define WINHTTP_QUERY_RAW_HEADERS_CRLF 22u
#define WINHTTP_QUERY_VERSION 18u
#define WINHTTP_QUERY_CONTENT_TYPE 1u
#define WINHTTP_QUERY_CONTENT_LENGTH 5u
#define WINHTTP_QUERY_DATE 9u
#define WINHTTP_QUERY_EXPIRES 10u
#define WINHTTP_QUERY_LAST_MODIFIED 11u
#define WINHTTP_QUERY_LOCATION 33u
#define WINHTTP_QUERY_SERVER 37u
#define WINHTTP_QUERY_SET_COOKIE 43u
#define WINHTTP_QUERY_CUSTOM 65535u
#define WINHTTP_QUERY_FLAG_NUMBER 0x20000000u
#define WINHTTP_OPTION_CONNECT_TIMEOUT 3u
#define WINHTTP_OPTION_SEND_TIMEOUT 5u
#define WINHTTP_OPTION_RECEIVE_TIMEOUT 6u
#define WINHTTP_OPTION_DISABLE_FEATURE 63u
#define WINHTTP_DISABLE_REDIRECTS 0x2u
#define WINHTTP_OPTION_REDIRECT_POLICY 88u
#define WINHTTP_OPTION_REDIRECT_POLICY_NEVER 0u

typedef struct {
    uint32_t kind;
    char *agent;
    uint32_t access_type;
    char *proxy;
    char *proxy_bypass;
    char *host;
    uint16_t port;
    int secure;
    char *verb;
    char *path;
    char *headers;
    uint8_t *upload;
    size_t upload_size, upload_expected;
    uint8_t *body;
    size_t body_size, body_read;
    NSString *response_headers;
    NSString *status_text;
    uint32_t status;
    uint32_t timeout_ms, connect_timeout_ms, send_timeout_ms;
    int sent, received;
    int no_redirect;                 // WINHTTP_OPTION_REDIRECT_POLICY_NEVER: a 3xx is the response (its Location header included)
    int async;                       // opened with WINHTTP_FLAG_ASYNC (a session), or made under such a session
} winhttp_handle;

// NSURLSession follows redirects on its own; a request that must not (WinHTTP's redirect policy "never", which
// .NET's AllowAutoRedirect = false asks for) runs in a session whose delegate declines them, so the program sees
// the 302 and its Location header, as on Windows.
@interface VXNoRedirectDelegate : NSObject <NSURLSessionTaskDelegate>
@end
@implementation VXNoRedirectDelegate
- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task
        willPerformHTTPRedirection:(NSHTTPURLResponse *)response newRequest:(NSURLRequest *)request
        completionHandler:(void (^)(NSURLRequest *))completionHandler {
    (void)session; (void)task; (void)response; (void)request;
    completionHandler(nil);
}
@end

static char *copy_string(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *copy = malloc(n + 1);
    if (copy) memcpy(copy, s, n + 1);
    return copy;
}

static NSString *string_a(const char *s) {
    return s ? [NSString stringWithUTF8String:s] : nil;
}

static NSString *string_w(const WCHAR *s) {
    if (!s) return nil;
    size_t n = 0;
    while (s[n]) n++;
    return [[NSString alloc] initWithCharacters:(const unichar *)s length:n];
}

static char *utf8_copy(NSString *s) {
    return s ? copy_string(s.UTF8String) : NULL;
}

typedef struct {
    DWORD access_type;
    WCHAR *proxy;
    WCHAR *proxy_bypass;
} winhttp_proxy_info;

typedef struct {
    BOOL auto_detect;
    WCHAR *auto_config_url;
    WCHAR *proxy;
    WCHAR *proxy_bypass;
} winhttp_ie_proxy_config;

static WCHAR *global_wide_copy(NSString *value) {
    if (!value) return NULL;
    NSUInteger length = value.length;
    if (length > SIZE_MAX / sizeof(WCHAR) - 1) return NULL;
    WCHAR *copy = vx_heap_alloc((length + 1) * sizeof(WCHAR));
    if (!copy) return NULL;
    [value getCharacters:(unichar *)copy range:NSMakeRange(0, length)];
    copy[length] = 0;
    return copy;
}

static BOOL proxy_setting_enabled(NSDictionary *settings, CFStringRef key) {
    id value = settings[(__bridge NSString *)key];
    return [value respondsToSelector:@selector(boolValue)] && [value boolValue];
}

static NSString *proxy_endpoint(NSDictionary *settings, CFStringRef host_key, CFStringRef port_key) {
    NSString *host = settings[(__bridge NSString *)host_key];
    if (![host isKindOfClass:[NSString class]] || !host.length) return nil;
    NSNumber *port = settings[(__bridge NSString *)port_key];
    NSString *address = host;
    if ([host containsString:@":"] && ![host hasPrefix:@"["]) address = [NSString stringWithFormat:@"[%@]", host];
    if ([port isKindOfClass:[NSNumber class]] && port.intValue > 0 && port.intValue <= UINT16_MAX)
        return [NSString stringWithFormat:@"%@:%d", address, port.intValue];
    return address;
}

static void free_ie_proxy_config(winhttp_ie_proxy_config *config) {
    free(config->auto_config_url);
    free(config->proxy);
    free(config->proxy_bypass);
    memset(config, 0, sizeof *config);
}

static NSArray<NSString *> *proxy_tokens(NSString *list) {
    NSCharacterSet *separators = [NSCharacterSet characterSetWithCharactersInString:@"; \t\r\n"];
    NSMutableArray<NSString *> *tokens = [NSMutableArray array];
    for (NSString *token in [list componentsSeparatedByCharactersInSet:separators]) {
        NSString *trimmed = [token stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
        if (trimmed.length) [tokens addObject:trimmed];
    }
    return tokens;
}

static BOOL parse_proxy_endpoint(NSString *text, NSString **host_out, NSNumber **port_out) {
    if (!text.length) return NO;
    NSString *url_text = [text containsString:@"://"] ? text : [@"http://" stringByAppendingString:text];
    NSURLComponents *components = [NSURLComponents componentsWithString:url_text];
    NSString *path = components.path;
    if (!components.host.length || components.user.length || components.password.length ||
        components.query.length || components.fragment.length || (path.length && ![path isEqualToString:@"/"]))
        return NO;
    NSNumber *port = components.port ?: @80;
    if (port.intValue < 1 || port.intValue > UINT16_MAX) return NO;
    if (host_out) *host_out = components.host;
    if (port_out) *port_out = port;
    return YES;
}

static BOOL valid_proxy_list(NSString *list) {
    if (!list.length) return NO;
    NSArray<NSString *> *tokens = proxy_tokens(list);
    if (!tokens.count) return NO;
    for (NSString *token in tokens) {
        NSRange equals = [token rangeOfString:@"="];
        NSString *endpoint = token;
        if (equals.location != NSNotFound) {
            NSString *target_scheme = [[token substringToIndex:equals.location] lowercaseString];
            if (![target_scheme isEqualToString:@"http"] && ![target_scheme isEqualToString:@"https"])
                return NO;
            endpoint = [token substringFromIndex:equals.location + 1];
        }
        if (!parse_proxy_endpoint(endpoint, NULL, NULL)) return NO;
    }
    return YES;
}

static NSString *proxy_endpoint_for_scheme(NSString *list, NSString *scheme) {
    NSString *generic = nil;
    NSString *specific = nil;
    for (NSString *token in proxy_tokens(list)) {
        NSRange equals = [token rangeOfString:@"="];
        if (equals.location == NSNotFound) {
            if (!generic) generic = token;
            continue;
        }
        NSString *target_scheme = [[token substringToIndex:equals.location] lowercaseString];
        if ([target_scheme isEqualToString:scheme]) specific = [token substringFromIndex:equals.location + 1];
    }
    return specific ?: generic;
}

static BOOL proxy_bypass_matches(NSString *list, NSString *host) {
    if (!list.length || !host.length) return NO;
    for (NSString *pattern in proxy_tokens(list)) {
        if ([pattern caseInsensitiveCompare:@"<local>"] == NSOrderedSame &&
            [host rangeOfString:@"."].location == NSNotFound) return YES;
        NSString *expression = [NSRegularExpression escapedPatternForString:pattern];
        expression = [expression stringByReplacingOccurrencesOfString:@"\\*" withString:@".*"];
        expression = [expression stringByReplacingOccurrencesOfString:@"\\?" withString:@"."];
        NSRegularExpression *regex = [NSRegularExpression regularExpressionWithPattern:
            [NSString stringWithFormat:@"^(?:%@)$", expression] options:NSRegularExpressionCaseInsensitive error:nil];
        if ([regex firstMatchInString:host options:0 range:NSMakeRange(0, host.length)]) return YES;
    }
    return NO;
}

static NSDictionary *direct_proxy_settings(void) {
    return @{ (__bridge NSString *)kCFNetworkProxiesHTTPEnable: @NO,
              (__bridge NSString *)kCFNetworkProxiesHTTPSEnable: @NO,
              (__bridge NSString *)kCFNetworkProxiesSOCKSEnable: @NO };
}

static NSDictionary *proxy_settings_for_request(const winhttp_handle *handle, NSURL *url) {
    if (handle->access_type == WH_ACCESS_AUTOMATIC_PROXY) return nil;
    if (handle->access_type != WH_ACCESS_NAMED_PROXY ||
        proxy_bypass_matches(string_a(handle->proxy_bypass), url.host)) return direct_proxy_settings();

    NSString *scheme = [url.scheme.lowercaseString isEqualToString:@"https"] ? @"https" : @"http";
    NSString *endpoint = proxy_endpoint_for_scheme(string_a(handle->proxy), scheme);
    NSString *proxy_host = nil;
    NSNumber *proxy_port = nil;
    if (!parse_proxy_endpoint(endpoint, &proxy_host, &proxy_port)) return direct_proxy_settings();
    if ([scheme isEqualToString:@"https"])
        return @{ (__bridge NSString *)kCFNetworkProxiesHTTPEnable: @NO,
                  (__bridge NSString *)kCFNetworkProxiesHTTPSEnable: @YES,
                  (__bridge NSString *)kCFNetworkProxiesHTTPSProxy: proxy_host,
                  (__bridge NSString *)kCFNetworkProxiesHTTPSPort: proxy_port,
                  (__bridge NSString *)kCFNetworkProxiesSOCKSEnable: @NO };
    return @{ (__bridge NSString *)kCFNetworkProxiesHTTPEnable: @YES,
              (__bridge NSString *)kCFNetworkProxiesHTTPProxy: proxy_host,
              (__bridge NSString *)kCFNetworkProxiesHTTPPort: proxy_port,
              (__bridge NSString *)kCFNetworkProxiesHTTPSEnable: @NO,
              (__bridge NSString *)kCFNetworkProxiesSOCKSEnable: @NO };
}

// WinHTTP's machine default is distinct from the interactive user's Internet proxy settings. The runtime has no
// WinHttpSetDefaultProxyConfiguration implementation or WinHTTP registry overlay yet, so its initial default is direct.
static WINAPI BOOL WinHttpGetDefaultProxyConfiguration(winhttp_proxy_info *info) {
    if (!info) { vx_set_last_error(87 /* ERROR_INVALID_PARAMETER */); return NO; }
    *info = (winhttp_proxy_info){ .access_type = 1 /* WINHTTP_ACCESS_TYPE_NO_PROXY */ };
    return YES;
}

// Map the active macOS network's proxy preferences to the legacy per-user WinHTTP configuration shape.
static WINAPI BOOL WinHttpGetIEProxyConfigForCurrentUser(winhttp_ie_proxy_config *config) {
    if (!config) { vx_set_last_error(87 /* ERROR_INVALID_PARAMETER */); return NO; }
    *config = (winhttp_ie_proxy_config){ 0 };
    NSDictionary *settings = CFBridgingRelease(SCDynamicStoreCopyProxies(NULL));
    if (![settings isKindOfClass:[NSDictionary class]]) {
        vx_set_last_error(2 /* ERROR_FILE_NOT_FOUND */);
        return NO;
    }

    config->auto_detect = proxy_setting_enabled(settings, kSCPropNetProxiesProxyAutoDiscoveryEnable);
    NSString *auto_config_url = nil;
    if (proxy_setting_enabled(settings, kSCPropNetProxiesProxyAutoConfigEnable)) {
        id candidate = settings[(__bridge NSString *)kSCPropNetProxiesProxyAutoConfigURLString];
        if ([candidate isKindOfClass:[NSString class]] && [candidate length]) {
            auto_config_url = candidate;
            config->auto_config_url = global_wide_copy(auto_config_url);
        }
    }

    NSMutableArray<NSString *> *proxies = [NSMutableArray arrayWithCapacity:3];
    if (proxy_setting_enabled(settings, kSCPropNetProxiesHTTPEnable)) {
        NSString *endpoint = proxy_endpoint(settings, kSCPropNetProxiesHTTPProxy, kSCPropNetProxiesHTTPPort);
        if (endpoint) [proxies addObject:[@"http=" stringByAppendingString:endpoint]];
    }
    if (proxy_setting_enabled(settings, kSCPropNetProxiesHTTPSEnable)) {
        NSString *endpoint = proxy_endpoint(settings, kSCPropNetProxiesHTTPSProxy, kSCPropNetProxiesHTTPSPort);
        if (endpoint) [proxies addObject:[@"https=" stringByAppendingString:endpoint]];
    }
    if (proxy_setting_enabled(settings, kSCPropNetProxiesSOCKSEnable)) {
        NSString *endpoint = proxy_endpoint(settings, kSCPropNetProxiesSOCKSProxy, kSCPropNetProxiesSOCKSPort);
        if (endpoint) [proxies addObject:[@"socks=" stringByAppendingString:endpoint]];
    }
    if (proxies.count) config->proxy = global_wide_copy([proxies componentsJoinedByString:@";"]);

    NSArray *exceptions = settings[(__bridge NSString *)kSCPropNetProxiesExceptionsList];
    BOOL has_bypass = NO;
    if ([exceptions isKindOfClass:[NSArray class]] && exceptions.count) {
        NSMutableArray<NSString *> *valid = [NSMutableArray arrayWithCapacity:exceptions.count];
        for (id exception in exceptions)
            if ([exception isKindOfClass:[NSString class]] && [exception length]) [valid addObject:exception];
        if (valid.count) {
            has_bypass = YES;
            config->proxy_bypass = global_wide_copy([valid componentsJoinedByString:@";"]);
        }
    }

    BOOL allocation_failed = (auto_config_url && !config->auto_config_url) ||
                             (proxies.count && !config->proxy) || (has_bypass && !config->proxy_bypass);
    if (allocation_failed) {
        free_ie_proxy_config(config);
        vx_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return NO;
    }
    return YES;
}

static BOOL valid_handle(HANDLE handle, uint32_t kind) {
    winhttp_handle *h = handle;
    if (!h || h->kind != kind) {
        vx_set_last_error(12018 /* ERROR_WINHTTP_INCORRECT_HANDLE_TYPE */);
        return NO;
    }
    return YES;
}

// Requests of an asynchronous session are not done yet (see the top of this file): refused, not run synchronously.
static BOOL refuse_async(const winhttp_handle *h) {
    if (!h->async) return NO;
    vx_set_last_error(ERROR_NOT_SUPPORTED);
    return YES;
}

// For winhttp_proxy.m's WinHttpCreateProxyResolver: 1 when handle is a session opened with WINHTTP_FLAG_ASYNC.
int vx_winhttp_session_async(HANDLE handle) {
    const winhttp_handle *h = handle;
    return h && h->kind == WH_SESSION && h->async;
}

static void append_bytes(uint8_t **buffer, size_t *size, const void *data, size_t length) {
    if (!length) return;
    if (!data || length > SIZE_MAX - *size) return;
    uint8_t *next = realloc(*buffer, *size + length);
    if (!next) return;
    memcpy(next + *size, data, length);
    *buffer = next;
    *size += length;
}

static void append_header(winhttp_handle *h, NSString *line) {
    if (!line.length) return;
    const char *utf8 = line.UTF8String;
    size_t length = strlen(utf8);
    if (length > SIZE_MAX - (h->headers ? strlen(h->headers) : 0) - 3) return;
    size_t old = h->headers ? strlen(h->headers) : 0;
    char *next = realloc(h->headers, old + length + 3);
    if (!next) { vx_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return; }
    memcpy(next + old, utf8, length);
    old += length;
    if (old < 2 || next[old - 2] != '\r' || next[old - 1] != '\n') {
        next[old++] = '\r';
        next[old++] = '\n';
    }
    next[old] = 0;
    h->headers = next;
}

static void apply_headers(NSMutableURLRequest *request, const char *headers) {
    if (!headers) return;
    const char *line = headers;
    while (*line) {
        const char *end = strstr(line, "\r\n");
        if (!end) break;
        const char *colon = memchr(line, ':', (size_t)(end - line));
        if (colon) {
            NSString *name = [[NSString alloc] initWithBytes:line length:(NSUInteger)(colon - line)
                                                    encoding:NSUTF8StringEncoding];
            const char *value = colon + 1;
            while (value < end && (*value == ' ' || *value == '\t')) value++;
            NSString *field = [[NSString alloc] initWithBytes:value length:(NSUInteger)(end - value)
                                                      encoding:NSUTF8StringEncoding];
            if (name && field) [request setValue:field forHTTPHeaderField:name];
        }
        line = end + 2;
    }
}

static NSString *header_value(winhttp_handle *h, NSString *name) {
    for (NSString *line in [h->response_headers componentsSeparatedByString:@"\r\n"]) {
        NSRange colon = [line rangeOfString:@":"];
        if (colon.location == NSNotFound) continue;
        NSString *field = [line substringToIndex:colon.location];
        if ([field caseInsensitiveCompare:name] != NSOrderedSame) continue;
        NSString *value = [line substringFromIndex:colon.location + 1];
        return [value stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    }
    return nil;
}

static BOOL perform_request(winhttp_handle *h) {
    @autoreleasepool {
        if (h->upload_expected && h->upload_size != h->upload_expected) {
            vx_set_last_error(12019 /* ERROR_WINHTTP_CANNOT_CALL_BEFORE_OPEN */);
            return NO;
        }
        NSString *scheme = h->secure ? @"https" : @"http";
        NSURLComponents *base = [NSURLComponents new];
        base.scheme = scheme;
        base.host = string_a(h->host);
        uint16_t default_port = h->secure ? 443 : 80;
        uint16_t port = h->port ? h->port : default_port;
        if (port != default_port) base.port = @(port);
        base.path = @"/";
        NSURL *base_url = base.URL;
        NSURL *url = base_url ? [NSURL URLWithString:string_a(h->path) relativeToURL:base_url].absoluteURL : nil;
        if (!url || !url.scheme || !url.host) {
            vx_set_last_error(ERROR_WINHTTP_INVALID_URL);
            return NO;
        }
        NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:url];
        request.HTTPMethod = string_a(h->verb) ?: @"GET";
        if (h->timeout_ms) request.timeoutInterval = (NSTimeInterval)h->timeout_ms / 1000.0;
        if (h->upload_size) request.HTTPBody = [NSData dataWithBytes:h->upload length:h->upload_size];
        if (h->agent) [request setValue:string_a(h->agent) forHTTPHeaderField:@"User-Agent"];
        apply_headers(request, h->headers);

        __block NSData *data = nil;
        __block NSHTTPURLResponse *response = nil;
        __block NSError *error = nil;
        dispatch_semaphore_t completed = dispatch_semaphore_create(0);
        NSURLSessionConfiguration *configuration = [NSURLSessionConfiguration defaultSessionConfiguration];
        NSDictionary *proxy_settings = proxy_settings_for_request(h, url);
        if (proxy_settings) configuration.connectionProxyDictionary = proxy_settings;
        static VXNoRedirectDelegate *no_redirect;
        if (!no_redirect) no_redirect = [VXNoRedirectDelegate new];
        NSURLSession *session = [NSURLSession sessionWithConfiguration:configuration
            delegate:h->no_redirect ? no_redirect : nil delegateQueue:nil];
        [[session dataTaskWithRequest:request completionHandler:^(NSData *received,
                                                                                      NSURLResponse *url_response,
                                                                                      NSError *failure) {
            data = received;
            response = (NSHTTPURLResponse *)url_response;
            error = failure;
            dispatch_semaphore_signal(completed);
        }] resume];
        dispatch_semaphore_wait(completed, DISPATCH_TIME_FOREVER);
        [session finishTasksAndInvalidate];
        if (error || !response) {
            if (error.code == NSURLErrorTimedOut) vx_set_last_error(ERROR_WINHTTP_TIMEOUT);
            else vx_set_last_error(ERROR_WINHTTP_CANNOT_CONNECT);
            if (vx_trace) vx_log("winhttp: %s %s failed: %s", h->verb, url.absoluteString.UTF8String,
                                 error.localizedDescription.UTF8String);
            return NO;
        }
        free(h->body);
        h->body_size = data.length;
        h->body_read = 0;
        h->body = malloc(data.length ? data.length : 1);
        if (!h->body) { h->body_size = 0; vx_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return NO; }
        if (data.length) memcpy(h->body, data.bytes, data.length);
        h->status = (uint32_t)response.statusCode;
        h->status_text = [NSHTTPURLResponse localizedStringForStatusCode:response.statusCode];
        NSMutableString *headers = [NSMutableString stringWithFormat:@"HTTP/1.1 %u %@\r\n", h->status, h->status_text];
        [response.allHeaderFields enumerateKeysAndObjectsUsingBlock:^(id key, id value, BOOL *stop) {
            (void)stop;
            if ([key isKindOfClass:[NSString class]] && [value isKindOfClass:[NSString class]])
                [headers appendFormat:@"%@: %@\r\n", key, value];
        }];
        h->response_headers = headers;
        h->received = 1;
        if (vx_trace) vx_log("winhttp: %s %s: %u, %zu bytes", h->verb, url.absoluteString.UTF8String,
                             h->status, h->body_size);
        return YES;
    }
}

static WINAPI HANDLE WinHttpOpen(const WCHAR *agent, DWORD access_type, const WCHAR *proxy,
                                 const WCHAR *bypass, DWORD flags) {
    if ((access_type != WH_ACCESS_DEFAULT_PROXY && access_type != WH_ACCESS_NO_PROXY &&
         access_type != WH_ACCESS_NAMED_PROXY && access_type != WH_ACCESS_AUTOMATIC_PROXY) ||
        (access_type == WH_ACCESS_NAMED_PROXY && (!proxy || !valid_proxy_list(string_w(proxy))))) {
        vx_set_last_error(87 /* ERROR_INVALID_PARAMETER */);
        return NULL;
    }
    winhttp_handle *h = calloc(1, sizeof *h);
    if (!h) { vx_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    h->kind = WH_SESSION;
    h->async = (flags & WINHTTP_FLAG_ASYNC) != 0;   // (also part of WINHTTP_FLAG_SECURE_DEFAULTS)
    h->access_type = access_type;
    h->agent = utf8_copy(string_w(agent));
    // WinHttpOpen ignores proxy and bypass strings for every access type except NAMED_PROXY.
    h->proxy = access_type == WH_ACCESS_NAMED_PROXY ? utf8_copy(string_w(proxy)) : NULL;
    h->proxy_bypass = access_type == WH_ACCESS_NAMED_PROXY ? utf8_copy(string_w(bypass)) : NULL;
    h->timeout_ms = 60000;
    if ((agent && !h->agent) || (access_type == WH_ACCESS_NAMED_PROXY &&
        ((proxy && !h->proxy) || (bypass && !h->proxy_bypass)))) {
        free(h->agent); free(h->proxy); free(h->proxy_bypass); free(h);
        vx_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    return h;
}

static WINAPI HANDLE WinHttpConnect(HANDLE session, const WCHAR *server, uint16_t port, DWORD reserved) {
    if (!valid_handle(session, WH_SESSION)) return NULL;
    if (!server || reserved) {
        vx_set_last_error(87 /* ERROR_INVALID_PARAMETER */);
        return NULL;
    }
    winhttp_handle *parent = session;
    if (refuse_async(parent)) return NULL;
    NSString *host = string_w(server);
    if (!host.length || !host.UTF8String) { vx_set_last_error(87); return NULL; }
    winhttp_handle *h = calloc(1, sizeof *h);
    if (!h) { vx_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    h->kind = WH_CONNECTION;
    h->async = parent->async;
    h->host = utf8_copy(host);
    h->port = port;
    h->agent = copy_string(parent->agent);
    h->access_type = parent->access_type;
    h->proxy = copy_string(parent->proxy);
    h->proxy_bypass = copy_string(parent->proxy_bypass);
    h->timeout_ms = parent->timeout_ms;
    h->connect_timeout_ms = parent->connect_timeout_ms;
    h->send_timeout_ms = parent->send_timeout_ms;
    if (!h->host || (parent->agent && !h->agent) || (parent->proxy && !h->proxy) ||
        (parent->proxy_bypass && !h->proxy_bypass)) {
        free(h->host); free(h->agent); free(h->proxy); free(h->proxy_bypass); free(h);
        vx_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    return h;
}

static WINAPI HANDLE WinHttpOpenRequest(HANDLE connection, const WCHAR *verb, const WCHAR *object,
                                        const WCHAR *version, const WCHAR *referrer,
                                        const WCHAR **accept_types, DWORD flags) {
    (void)version; (void)referrer;
    if (!valid_handle(connection, WH_CONNECTION)) return NULL;
    winhttp_handle *parent = connection;
    if (refuse_async(parent)) return NULL;
    NSString *path = string_w(object);
    if (!path.length) path = @"/";
    if (![path hasPrefix:@"/"] && ![path hasPrefix:@"http://"] && ![path hasPrefix:@"https://"])
        path = [@"/" stringByAppendingString:path];
    winhttp_handle *h = calloc(1, sizeof *h);
    if (!h) { vx_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    h->kind = WH_REQUEST;
    h->async = parent->async;
    h->host = copy_string(parent->host);
    h->port = parent->port;
    h->secure = (flags & WINHTTP_FLAG_SECURE) != 0;
    h->verb = utf8_copy(string_w(verb));
    if (!h->verb) h->verb = copy_string("GET");
    h->path = utf8_copy(path);
    h->agent = copy_string(parent->agent);
    h->access_type = parent->access_type;
    h->proxy = copy_string(parent->proxy);
    h->proxy_bypass = copy_string(parent->proxy_bypass);
    h->timeout_ms = parent->timeout_ms;
    h->connect_timeout_ms = parent->connect_timeout_ms;
    h->send_timeout_ms = parent->send_timeout_ms;
    if (!h->host || !h->verb || !h->path || (parent->agent && !h->agent) ||
        (parent->proxy && !h->proxy) || (parent->proxy_bypass && !h->proxy_bypass)) {
        free(h->host); free(h->verb); free(h->path); free(h->agent); free(h->proxy); free(h->proxy_bypass); free(h);
        vx_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    if (accept_types) {
        NSMutableArray<NSString *> *types = [NSMutableArray array];
        for (uint32_t i = 0; accept_types[i]; i++) {
            NSString *type = string_w(accept_types[i]);
            if (type) [types addObject:type];
        }
        if (types.count) append_header(h, [NSString stringWithFormat:@"Accept: %@", [types componentsJoinedByString:@", "]]);
    }
    return h;
}

static WINAPI BOOL WinHttpAddRequestHeaders(HANDLE request, const WCHAR *headers, DWORD length, DWORD modifiers) {
    (void)modifiers;
    if (!valid_handle(request, WH_REQUEST)) return NO;
    NSString *all = string_w(headers);
    if (!all) return YES;
    if (length != UINT32_MAX && length < all.length) all = [all substringToIndex:length];
    append_header(request, all);
    return YES;
}

static WINAPI BOOL WinHttpSendRequest(HANDLE request, const WCHAR *headers, DWORD header_length,
                                      void *optional, DWORD optional_length, DWORD total_length, uintptr_t context) {
    (void)context;
    if (!valid_handle(request, WH_REQUEST) || request == NULL) return NO;
    winhttp_handle *h = request;
    if (refuse_async(h)) return NO;
    if (h->sent || optional_length > total_length || (optional_length && !optional)) {
        vx_set_last_error(87 /* ERROR_INVALID_PARAMETER */);
        return NO;
    }
    if (headers) {
        NSString *text = string_w(headers);
        if (text && header_length != UINT32_MAX && header_length < text.length)
            text = [text substringToIndex:header_length];
        if (text) append_header(h, text);
    }
    h->upload_expected = total_length;
    if (optional_length) append_bytes(&h->upload, &h->upload_size, optional, optional_length);
    if (optional_length && h->upload_size != optional_length) { vx_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return NO; }
    h->sent = 1;
    return YES;
}

static WINAPI BOOL WinHttpWriteData(HANDLE request, const void *buffer, DWORD bytes_to_write, DWORD *bytes_written) {
    if (!valid_handle(request, WH_REQUEST)) return NO;
    winhttp_handle *h = request;
    if (!h->sent || h->received || !buffer || h->upload_size > h->upload_expected ||
        bytes_to_write > h->upload_expected - h->upload_size) {
        vx_set_last_error(87 /* ERROR_INVALID_PARAMETER */);
        return NO;
    }
    size_t before = h->upload_size;
    append_bytes(&h->upload, &h->upload_size, buffer, bytes_to_write);
    if (h->upload_size - before != bytes_to_write) { vx_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return NO; }
    if (bytes_written) *bytes_written = bytes_to_write;
    return YES;
}

static WINAPI BOOL WinHttpReceiveResponse(HANDLE request, void *reserved) {
    if (!valid_handle(request, WH_REQUEST)) return NO;
    winhttp_handle *h = request;
    if (reserved || !h->sent || h->received) { vx_set_last_error(87 /* ERROR_INVALID_PARAMETER */); return NO; }
    // VXRT_NO_NETWORK: runs with made-up data must not reach a server with it. =1 refuses every request;
    // =allow:/path,/other lets through only plain requests (no body, no query) whose path is exactly one listed and
    // refuses all the rest. The host and path are logged, never the query or the body.
    const char *no_network = getenv("VXRT_NO_NETWORK");
    if (no_network && *no_network) {
        const char *path = h->path ? h->path : "";
        size_t path_length = strcspn(path, "?");
        int has_data = h->upload_size > 0 || h->upload_expected > 0 || path[path_length] == '?';
        int refuse = 1;
        if (!strncmp(no_network, "allow:", 6) && !has_data) {
            for (const char *p = no_network + 6; *p;) {
                size_t n = strcspn(p, ",");
                if (n == path_length && !strncmp(p, path, n)) refuse = 0;
                p += n + (p[n] == ',');
            }
        }
        char shape[512] = "";                                          // the query as key=<length of the value>&..., never the values
        if (path[path_length] == '?') {
            size_t used = 0;
            for (const char *q = path + path_length + 1; *q && used + 40 < sizeof shape;) {
                size_t pair = strcspn(q, "&"), key = strcspn(q, "=");
                if (key > pair) key = pair;
                used += (size_t)snprintf(shape + used, sizeof shape - used, "%s%.*s=<%zu>", used ? "&" : "", (int)key, q, pair > key ? pair - key - 1 : 0);
                q += pair + (q[pair] == '&');
            }
        }
        vx_log("WinHTTP: VXRT_NO_NETWORK: %s %s%.*s%s%s, body %zu bytes: %s", h->verb ? h->verb : "?", h->host ? h->host : "?", (int)path_length, path,
               shape[0] ? " ?" : "", shape, h->upload_size, refuse ? "refused (nothing is sent)" : "allowed (listed, no data in it)");
        if (refuse) { vx_set_last_error(12029 /* ERROR_WINHTTP_CANNOT_CONNECT */); return NO; }
    }
    return perform_request(h);
}

static WINAPI BOOL WinHttpReadData(HANDLE request, void *buffer, DWORD bytes_to_read, DWORD *bytes_read) {
    if (!valid_handle(request, WH_REQUEST) || !bytes_read || (bytes_to_read && !buffer)) return NO;
    winhttp_handle *h = request;
    if (!h->received) { vx_set_last_error(12019 /* ERROR_WINHTTP_CANNOT_CALL_BEFORE_OPEN */); return NO; }
    size_t left = h->body_size - h->body_read;
    size_t take = left < bytes_to_read ? left : bytes_to_read;
    if (take) memcpy(buffer, h->body + h->body_read, take);
    h->body_read += take;
    *bytes_read = (DWORD)take;
    return YES;
}

static WINAPI BOOL WinHttpQueryDataAvailable(HANDLE request, DWORD *available) {
    if (!valid_handle(request, WH_REQUEST) || !available) return NO;
    winhttp_handle *h = request;
    if (!h->received) { vx_set_last_error(12019 /* ERROR_WINHTTP_CANNOT_CALL_BEFORE_OPEN */); return NO; }
    size_t left = h->body_size - h->body_read;
    *available = left > UINT32_MAX ? UINT32_MAX : (DWORD)left;
    return YES;
}

static BOOL query_wide_string(NSString *value, void *buffer, DWORD *byte_length) {
    if (!value || !byte_length || value.length > (UINT32_MAX / sizeof(WCHAR)) - 1) {
        vx_set_last_error(87 /* ERROR_INVALID_PARAMETER */);
        return NO;
    }
    DWORD needed = (DWORD)((value.length + 1) * sizeof(WCHAR));
    if (!buffer || *byte_length < needed) {
        *byte_length = needed;
        vx_set_last_error(ERROR_INSUFFICIENT_BUFFER);
        return NO;
    }
    [value getCharacters:(unichar *)buffer range:NSMakeRange(0, value.length)];
    ((WCHAR *)buffer)[value.length] = 0;
    *byte_length = needed - sizeof(WCHAR);
    return YES;
}

static BOOL query_raw_headers(NSString *value, void *buffer, DWORD *byte_length) {
    if (!value || !byte_length) { vx_set_last_error(87 /* ERROR_INVALID_PARAMETER */); return NO; }
    NSArray<NSString *> *lines = [value componentsSeparatedByString:@"\r\n"];
    size_t chars = 1;
    for (NSString *line in lines) if (line.length) {
        if (line.length > SIZE_MAX - chars - 1) { vx_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return NO; }
        chars += line.length + 1;
    }
    if (chars > UINT32_MAX / sizeof(WCHAR)) { vx_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return NO; }
    DWORD needed = (DWORD)(chars * sizeof(WCHAR));
    if (!buffer || *byte_length < needed) {
        *byte_length = needed;
        vx_set_last_error(ERROR_INSUFFICIENT_BUFFER);
        return NO;
    }
    WCHAR *out = buffer;
    size_t at = 0;
    for (NSString *line in lines) if (line.length) {
        [line getCharacters:(unichar *)(out + at) range:NSMakeRange(0, line.length)];
        at += line.length;
        out[at++] = 0;
    }
    out[at++] = 0;
    *byte_length = (DWORD)(at * sizeof(WCHAR));
    return YES;
}

static WINAPI BOOL WinHttpQueryHeaders(HANDLE request, DWORD level, const WCHAR *name,
                                       void *buffer, DWORD *byte_length, DWORD *index) {
    (void)index;
    if (!valid_handle(request, WH_REQUEST)) return NO;
    winhttp_handle *h = request;
    if (!h->received) { vx_set_last_error(12019 /* ERROR_WINHTTP_CANNOT_CALL_BEFORE_OPEN */); return NO; }
    DWORD query = level & 0xffffu;
    BOOL number = (level & WINHTTP_QUERY_FLAG_NUMBER) != 0;
    NSString *value = nil;
    switch (query) {
    case WINHTTP_QUERY_STATUS_CODE: value = [NSString stringWithFormat:@"%u", h->status]; break;
    case WINHTTP_QUERY_STATUS_TEXT: value = h->status_text; break;
    case WINHTTP_QUERY_VERSION: value = @"HTTP/1.1"; break;
    case WINHTTP_QUERY_RAW_HEADERS_CRLF: value = h->response_headers; break;
    case WINHTTP_QUERY_CONTENT_TYPE: value = header_value(h, @"Content-Type"); break;
    case WINHTTP_QUERY_CONTENT_LENGTH: value = header_value(h, @"Content-Length"); break;
    case WINHTTP_QUERY_DATE: value = header_value(h, @"Date"); break;
    case WINHTTP_QUERY_EXPIRES: value = header_value(h, @"Expires"); break;
    case WINHTTP_QUERY_LAST_MODIFIED: value = header_value(h, @"Last-Modified"); break;
    case WINHTTP_QUERY_LOCATION: value = header_value(h, @"Location"); break;
    case WINHTTP_QUERY_SERVER: value = header_value(h, @"Server"); break;
    case WINHTTP_QUERY_SET_COOKIE: value = header_value(h, @"Set-Cookie"); break;
    case WINHTTP_QUERY_RAW_HEADERS: {
        return query_raw_headers(h->response_headers, buffer, byte_length);
    }
    case WINHTTP_QUERY_CUSTOM:
        if (name) value = header_value(h, string_w(name));
        break;
    default:
        value = header_value(h, [NSString stringWithFormat:@"%u", query]);
        break;
    }
    if (!value) { vx_set_last_error(ERROR_WINHTTP_HEADER_NOT_FOUND); return NO; }
    if (number) {
        if (!byte_length || !buffer || *byte_length < sizeof(DWORD)) {
            if (byte_length) *byte_length = sizeof(DWORD);
            vx_set_last_error(ERROR_INSUFFICIENT_BUFFER);
            return NO;
        }
        *(DWORD *)buffer = (DWORD)value.longLongValue;
        *byte_length = sizeof(DWORD);
        return YES;
    }
    return query_wide_string(value, buffer, byte_length);
}

static WINAPI BOOL WinHttpSetTimeouts(HANDLE handle, int resolve, int connect, int send, int receive) {
    (void)resolve;
    winhttp_handle *h = handle;
    if (!h || h->kind < WH_SESSION || h->kind > WH_REQUEST) { vx_set_last_error(12018); return NO; }
    uint32_t effective = 0;
    int values[] = { resolve, connect, send, receive };
    for (size_t i = 0; i < sizeof values / sizeof values[0]; i++)
        if (values[i] > 0 && (!effective || (uint32_t)values[i] < effective)) effective = (uint32_t)values[i];
    if (effective) h->timeout_ms = effective;
    if (connect > 0) h->connect_timeout_ms = (uint32_t)connect;
    if (send > 0) h->send_timeout_ms = (uint32_t)send;
    return YES;
}

static WINAPI BOOL WinHttpSetOption(HANDLE handle, DWORD option, void *buffer, DWORD length) {
    winhttp_handle *h = handle;
    if (!h || h->kind < WH_SESSION || h->kind > WH_REQUEST) { vx_set_last_error(12018); return NO; }
    if (!buffer || length != sizeof(DWORD)) { vx_set_last_error(87 /* ERROR_INVALID_PARAMETER */); return NO; }
    uint32_t timeout = *(DWORD *)buffer;
    switch (option) {
    case WINHTTP_OPTION_REDIRECT_POLICY: h->no_redirect = timeout == WINHTTP_OPTION_REDIRECT_POLICY_NEVER; return YES;
    case WINHTTP_OPTION_DISABLE_FEATURE: if (timeout & WINHTTP_DISABLE_REDIRECTS) h->no_redirect = 1; return YES;
    case WINHTTP_OPTION_CONNECT_TIMEOUT: h->connect_timeout_ms = timeout; h->timeout_ms = timeout; return YES;
    case WINHTTP_OPTION_SEND_TIMEOUT: h->send_timeout_ms = timeout; h->timeout_ms = timeout; return YES;
    case WINHTTP_OPTION_RECEIVE_TIMEOUT: h->timeout_ms = timeout; return YES;
    default: vx_set_last_error(ERROR_NOT_SUPPORTED); return NO;
    }
}

static WINAPI BOOL WinHttpCloseHandle(HANDLE handle) {
    if (vx_winhttp_proxy_close(handle)) return YES;                  // a proxy resolver; else its callback hears HANDLE_CLOSING (winhttp_proxy.m)
    winhttp_handle *h = handle;
    if (!h || h->kind < WH_SESSION || h->kind > WH_REQUEST) { vx_set_last_error(12018); return NO; }
    free(h->agent); free(h->proxy); free(h->proxy_bypass); free(h->host); free(h->verb); free(h->path); free(h->headers);
    free(h->upload); free(h->body);
    h->response_headers = nil;
    h->status_text = nil;
    free(h);
    return YES;
}

static const vx_export g_winhttp[] = {
    VX_EXPORT(WinHttpGetDefaultProxyConfiguration), VX_EXPORT(WinHttpGetIEProxyConfigForCurrentUser),
    VX_EXPORT(WinHttpOpen), VX_EXPORT(WinHttpConnect), VX_EXPORT(WinHttpOpenRequest),
    VX_EXPORT(WinHttpAddRequestHeaders), VX_EXPORT(WinHttpSendRequest), VX_EXPORT(WinHttpWriteData),
    VX_EXPORT(WinHttpReceiveResponse), VX_EXPORT(WinHttpReadData), VX_EXPORT(WinHttpQueryDataAvailable),
    VX_EXPORT(WinHttpQueryHeaders), VX_EXPORT(WinHttpSetTimeouts), VX_EXPORT(WinHttpSetOption),
    VX_EXPORT(WinHttpCloseHandle)
};

void vx_winhttp_init(void) {
    vx_register_module("winhttp.dll", g_winhttp, sizeof g_winhttp / sizeof g_winhttp[0]);
}

#ifdef __aarch64__
#include "thunks/winhttp.m.inc"
#endif
