// wininet.dll: HTTP and HTTPS for Windows programs, on NSURLSession. A session (InternetOpen), a connection to a
// server (InternetConnect), requests on it (HttpOpenRequest, HttpAddRequestHeaders, HttpSendRequest), the response
// read with InternetReadFile and HttpQueryInfo; InternetOpenUrl does a GET in one step. Requests are made when sent,
// synchronously, and the whole response is kept (programs read it in pieces afterwards).
#import <Foundation/Foundation.h>
#include "vx.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>

enum { H_SESSION = 1, H_CONNECTION, H_REQUEST };
typedef struct inet {
    int kind;
    char *agent;                                    // session
    char *host; uint16_t port; int secure;          // connection
    struct inet *conn;                              // request: its connection
    char *verb, *path, *headers;
    void *body; size_t body_size, read;             // the response
    int status; char *response_headers;             // "Name: value\r\n" lines, as HttpQueryInfo gives them
    char *content_type;
} inet;
#define INTERNET_FLAG_SECURE 0x00800000u
#define ERROR_INTERNET_CANNOT_CONNECT 12029
#define ERROR_HTTP_HEADER_NOT_FOUND 12150
#define ERROR_INSUFFICIENT_BUFFER 122

static char *copy_n(const char *s, size_t n) { char *d = malloc(n + 1); memcpy(d, s, n); d[n] = 0; return d; }
static char *copy_str(const char *s) { return s ? copy_n(s, strlen(s)) : NULL; }

static WINAPI HANDLE InternetOpenA(const char *agent, DWORD type, const char *proxy, const char *bypass, DWORD flags) {
    (void)type; (void)proxy; (void)bypass; (void)flags;
    inet *h = calloc(1, sizeof *h);
    h->kind = H_SESSION; h->agent = copy_str(agent);
    return h;
}
static WINAPI HANDLE InternetConnectA(HANDLE session, const char *server, uint16_t port, const char *user, const char *pass, DWORD service, DWORD flags, uintptr_t ctx) {
    (void)session; (void)user; (void)pass; (void)service; (void)ctx;
    if (!server) { vx_set_last_error(87); return NULL; }
    inet *h = calloc(1, sizeof *h);
    h->kind = H_CONNECTION; h->host = copy_str(server); h->port = port; h->secure = (flags & INTERNET_FLAG_SECURE) || port == 443;
    h->conn = session;
    return h;
}
static WINAPI HANDLE HttpOpenRequestA(HANDLE conn, const char *verb, const char *object, const char *version, const char *referrer,
                                      const char **accept, DWORD flags, uintptr_t ctx) {
    (void)version; (void)referrer; (void)accept; (void)ctx;
    inet *c = conn;
    if (!c || c->kind != H_CONNECTION) { vx_set_last_error(6); return NULL; }
    inet *h = calloc(1, sizeof *h);
    h->kind = H_REQUEST; h->conn = c;
    h->verb = copy_str(verb && *verb ? verb : "GET"); h->path = copy_str(object && *object ? object : "/");
    if (flags & INTERNET_FLAG_SECURE) c->secure = 1;
    return h;
}
static void add_headers(inet *r, const char *headers, DWORD len) {
    if (!headers) return;
    size_t n = len == 0xffffffffu ? strlen(headers) : len;
    size_t old = r->headers ? strlen(r->headers) : 0;
    r->headers = realloc(r->headers, old + n + 3);
    memcpy(r->headers + old, headers, n);
    r->headers[old + n] = 0;
    if (n < 2 || strcmp(r->headers + old + n - 2, "\r\n")) strcat(r->headers, "\r\n");
}
static WINAPI BOOL HttpAddRequestHeadersA(HANDLE req, const char *headers, DWORD len, DWORD modifiers) {
    (void)modifiers;
    inet *r = req;
    if (!r || r->kind != H_REQUEST) { vx_set_last_error(6); return 0; }
    add_headers(r, headers, len);
    return 1;
}
// The request, made and waited for; the response kept in r.
static BOOL perform(inet *r, NSString *url, const void *body, DWORD body_size) {
    @autoreleasepool {
        NSMutableURLRequest *q = [NSMutableURLRequest requestWithURL:[NSURL URLWithString:url]];
        if (!q) { vx_set_last_error(12005 /* ERROR_INTERNET_INVALID_URL */); return 0; }
        q.HTTPMethod = [NSString stringWithUTF8String:r->verb ? r->verb : "GET"];
        if (body && body_size) q.HTTPBody = [NSData dataWithBytes:body length:body_size];
        inet *session = r->conn ? (r->conn->kind == H_CONNECTION ? r->conn->conn : r->conn) : NULL;
        if (session && session->agent) [q setValue:[NSString stringWithUTF8String:session->agent] forHTTPHeaderField:@"User-Agent"];
        for (char *line = r->headers, *end; line && *line; line = end + 2) {
            end = strstr(line, "\r\n");
            if (!end) break;
            char *colon = memchr(line, ':', (size_t)(end - line));
            if (colon) {
                NSString *name = [[NSString alloc] initWithBytes:line length:(NSUInteger)(colon - line) encoding:NSUTF8StringEncoding];
                const char *v = colon + 1; while (*v == ' ') v++;
                NSString *value = [[NSString alloc] initWithBytes:v length:(NSUInteger)(end - v) encoding:NSUTF8StringEncoding];
                if (name && value) [q setValue:value forHTTPHeaderField:name];
            }
        }
        __block NSData *data = nil; __block NSHTTPURLResponse *response = nil; __block NSError *error = nil;
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        [[[NSURLSession sharedSession] dataTaskWithRequest:q completionHandler:^(NSData *d, NSURLResponse *resp, NSError *e) {
            data = d; response = (NSHTTPURLResponse *)resp; error = e;
            dispatch_semaphore_signal(done);
        }] resume];
        dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
        if (error || !response) {
            if (vx_trace) vx_log("wininet: %s %s failed: %s", r->verb, url.UTF8String, error.localizedDescription.UTF8String);
            vx_set_last_error(ERROR_INTERNET_CANNOT_CONNECT);
            return 0;
        }
        free(r->body); r->body = malloc(data.length ? data.length : 1); r->body_size = data.length; r->read = 0;
        memcpy(r->body, data.bytes, data.length);
        r->status = (int)response.statusCode;
        NSMutableString *h = [NSMutableString stringWithFormat:@"HTTP/1.1 %d %@\r\n", r->status, [NSHTTPURLResponse localizedStringForStatusCode:r->status]];
        // NSURLSession has decoded a compressed body already: its headers say so no more, and give its real length.
        [response.allHeaderFields enumerateKeysAndObjectsUsingBlock:^(id k, id v, BOOL *stop) {
            (void)stop;
            if ([k caseInsensitiveCompare:@"Content-Encoding"] == NSOrderedSame || [k caseInsensitiveCompare:@"Content-Length"] == NSOrderedSame) return;
            [h appendFormat:@"%@: %@\r\n", k, v];
        }];
        [h appendFormat:@"Content-Length: %lu\r\n", (unsigned long)data.length];
        [h appendString:@"\r\n"];
        free(r->response_headers); r->response_headers = copy_str(h.UTF8String);
        if (vx_trace) vx_log("wininet: %s %s: %d, %zu bytes; %s", r->verb, url.UTF8String, r->status, r->body_size, r->response_headers);
        if (getenv("VXRT_WININET_DUMP")) {          // the responses' bodies, numbered, in that directory
            static int n;
            char path[1024]; snprintf(path, sizeof path, "%s/response%03d.bin", getenv("VXRT_WININET_DUMP"), n++);
            FILE *f = fopen(path, "wb"); if (f) { fwrite(r->body, 1, r->body_size, f); fclose(f); }
        }
        return 1;
    }
}
static WINAPI BOOL HttpSendRequestA(HANDLE req, const char *headers, DWORD len, void *optional, DWORD optional_size) {
    inet *r = req;
    if (!r || r->kind != H_REQUEST) { vx_set_last_error(6); return 0; }
    add_headers(r, headers, len);
    inet *c = r->conn;
    NSString *url = [NSString stringWithFormat:@"%s://%s%s%@%s", c->secure ? "https" : "http", c->host,
                     (c->port && c->port != (c->secure ? 443 : 80)) ? ":" : "",
                     (c->port && c->port != (c->secure ? 443 : 80)) ? [NSString stringWithFormat:@"%u", c->port] : @"", r->path];
    return perform(r, url, optional, optional_size);
}
static WINAPI HANDLE InternetOpenUrlA(HANDLE session, const char *url, const char *headers, DWORD len, DWORD flags, uintptr_t ctx) {
    (void)flags; (void)ctx;
    if (!url) { vx_set_last_error(87); return NULL; }
    inet *r = calloc(1, sizeof *r);
    r->kind = H_REQUEST; r->conn = session; r->verb = copy_str("GET");
    add_headers(r, headers, len);
    if (!perform(r, [NSString stringWithUTF8String:url], NULL, 0)) { free(r->verb); free(r->headers); free(r); return NULL; }
    return r;
}
static WINAPI BOOL InternetReadFile(HANDLE req, void *buf, DWORD n, DWORD *got) {
    inet *r = req;
    if (!r || r->kind != H_REQUEST) { vx_set_last_error(6); return 0; }
    size_t left = r->body_size - r->read, take = left < n ? left : n;
    if (take) memcpy(buf, (uint8_t *)r->body + r->read, take);
    r->read += take;
    if (got) *got = (DWORD)take;
    return 1;
}
static WINAPI BOOL InternetQueryDataAvailable(HANDLE req, DWORD *n, DWORD flags, uintptr_t ctx) {
    (void)flags; (void)ctx;
    inet *r = req;
    if (!r || r->kind != H_REQUEST) { vx_set_last_error(6); return 0; }
    if (n) *n = (DWORD)(r->body_size - r->read);
    return 1;
}
// HttpQueryInfo: the status code, content length or type, all headers, or (HTTP_QUERY_CUSTOM) one by name.
static BOOL answer(const char *text, void *buf, DWORD *n) {
    size_t len = strlen(text);
    if (!n || !buf || *n < len + 1) { if (n) *n = (DWORD)len + 1; vx_set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    memcpy(buf, text, len + 1); *n = (DWORD)len;
    return 1;
}
static const char *header_value(inet *r, const char *name, char *out, size_t cap) {
    size_t len = strlen(name);
    for (const char *line = r->response_headers; line && *line; ) {
        const char *end = strstr(line, "\r\n");
        if (!end) break;
        if ((size_t)(end - line) > len && !strncasecmp(line, name, len) && line[len] == ':') {
            const char *v = line + len + 1; while (*v == ' ') v++;
            snprintf(out, cap, "%.*s", (int)(end - v), v);
            return out;
        }
        line = end + 2;
    }
    return NULL;
}
static WINAPI BOOL HttpQueryInfoA(HANDLE req, DWORD level, void *buf, DWORD *n, DWORD *index) {
    (void)index;
    inet *r = req;
    if (!r || r->kind != H_REQUEST || !r->response_headers) { vx_set_last_error(6); return 0; }
    DWORD what = level & 0xffff, number = level & 0x20000000u;   // HTTP_QUERY_FLAG_NUMBER
    char value[1024];
    const char *text = NULL;
    if (what == 21 && !number) {                       // HTTP_QUERY_RAW_HEADERS: the lines, each ended by a NUL, then a NUL
        size_t len = 0;
        char *raw = malloc(strlen(r->response_headers) + 2);
        for (const char *line = r->response_headers; *line; ) {
            const char *end = strstr(line, "\r\n");
            size_t l = end ? (size_t)(end - line) : strlen(line);
            if (l) { memcpy(raw + len, line, l); len += l; raw[len++] = 0; }
            line += l + (end ? 2 : 0);
        }
        raw[len++] = 0;
        BOOL ok = buf && n && *n >= len;
        if (ok) { memcpy(buf, raw, len); *n = (DWORD)len - 1; }
        else { if (n) *n = (DWORD)len; vx_set_last_error(ERROR_INSUFFICIENT_BUFFER); }
        free(raw);
        return ok;
    }
    switch (what) {
    case 19: snprintf(value, sizeof value, "%d", r->status); text = value; break;          // HTTP_QUERY_STATUS_CODE
    case 22: text = r->response_headers; break;                                             // HTTP_QUERY_RAW_HEADERS_CRLF
    case 5: text = header_value(r, "Content-Length", value, sizeof value);
            if (!text) { snprintf(value, sizeof value, "%zu", r->body_size); text = value; } break;
    case 1: text = header_value(r, "Content-Type", value, sizeof value); break;
    case 65535: text = buf ? header_value(r, (const char *)buf, value, sizeof value) : NULL; break;   // HTTP_QUERY_CUSTOM
    default: break;
    }
    if (!text) { vx_set_last_error(ERROR_HTTP_HEADER_NOT_FOUND); return 0; }
    if (number) {
        if (!buf || !n || *n < 4) { if (n) *n = 4; vx_set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
        *(DWORD *)buf = (DWORD)strtoul(text, NULL, 10); *n = 4;
        return 1;
    }
    return answer(text, buf, n);
}
static WINAPI BOOL InternetSetOptionA(HANDLE h, DWORD option, void *buf, DWORD n) { (void)h; (void)option; (void)buf; (void)n; return 1; }
static WINAPI BOOL InternetCloseHandle(HANDLE handle) {
    inet *h = handle;
    if (!h) return 0;
    free(h->agent); free(h->host); free(h->verb); free(h->path); free(h->headers); free(h->body); free(h->response_headers);
    free(h);
    return 1;
}
static WINAPI void *InternetSetStatusCallback(HANDLE h, void *cb) { (void)h; (void)cb; return NULL; }

static const vx_export g_wininet[] = {
    VX_EXPORT(InternetOpenA), VX_EXPORT(InternetConnectA), VX_EXPORT(HttpOpenRequestA), VX_EXPORT(HttpAddRequestHeadersA),
    VX_EXPORT(HttpSendRequestA), VX_EXPORT(InternetOpenUrlA), VX_EXPORT(InternetReadFile), VX_EXPORT(InternetQueryDataAvailable),
    VX_EXPORT(HttpQueryInfoA), VX_EXPORT(InternetSetOptionA), VX_EXPORT(InternetCloseHandle), VX_EXPORT(InternetSetStatusCallback),
};
void vx_wininet_init(void) { vx_register_module("wininet.dll", g_wininet, sizeof g_wininet / sizeof g_wininet[0]); }
