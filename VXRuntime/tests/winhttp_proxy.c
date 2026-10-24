// WinHttpCrackUrl per its documentation, status callbacks, and the proxy resolver of Windows 8 against PAC files this
// test serves itself on 127.0.0.1 (made-up proxies under .test): a list of proxies and DIRECT, a script that does
// not run, one that cannot be downloaded, one that never arrives while the resolver is closed (cancelled), and the
// Mac's static settings. The resolver's session is an asynchronous one (WINHTTP_FLAG_ASYNC), as documented; a
// synchronous session is refused. WinHttpGetProxyForUrl gives the same answers at once on either kind of session.
// Nothing leaves this computer.
#define _WIN32_WINNT 0x0A00                                      // (Windows 10: the Vista and Windows 8 declarations)
#include <winsock2.h>
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define SECURE_DEFAULTS 0x30000000u                              // WINHTTP_FLAG_SECURE_DEFAULTS (not in MinGW's winhttp.h)

static int failures;
#define CHECK(c) do { if (!(c)) { printf("check failed (line %d): %s\n", __LINE__, #c); failures++; } } while (0)

// ---- the PAC server ------------------------------------------------------------------------------------------------
static SOCKET listener;
static volatile LONG stop_server;
static const char pac_script[] =
    "function FindProxyForURL(url, host) {\n"
    "  if (dnsDomainIs(host, \"direct.test\")) return \"DIRECT\";\n"
    "  return \"PROXY proxy.test:3128; SOCKS socks.test:1081; DIRECT\";\n"
    "}\n";
static const char bad_script[] = "function FindProxyForURL(url, host) { return ( ; }\n";
static DWORD WINAPI serve(void *unused) {
    (void)unused;
    SOCKET held[8];
    int held_count = 0;
    while (!stop_server) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(listener, &readable);
        struct timeval timeout = { 0, 100000 };
        if (select(0, &readable, NULL, NULL, &timeout) <= 0) continue;
        SOCKET client = accept(listener, NULL, NULL);
        if (client == INVALID_SOCKET) continue;
        char request[2048] = { 0 };
        int used = 0;
        while (used < (int)sizeof request - 1 && !strstr(request, "\r\n\r\n")) {
            int n = recv(client, request + used, (int)sizeof request - 1 - used, 0);
            if (n <= 0) break;
            used += n;
        }
        if (strstr(request, "GET /hang.pac ") && held_count < 8) { held[held_count++] = client; continue; }   // never answered
        const char *body = strstr(request, "GET /proxy.pac ") ? pac_script : strstr(request, "GET /bad.pac ") ? bad_script : NULL;
        char response[1024];
        int length = body ? snprintf(response, sizeof response, "HTTP/1.1 200 OK\r\nContent-Type: application/x-ns-proxy-autoconfig\r\n"
                                     "Content-Length: %u\r\nConnection: close\r\n\r\n%s", (unsigned)strlen(body), body)
                          : snprintf(response, sizeof response, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        send(client, response, length, 0);
        closesocket(client);
    }
    for (int i = 0; i < held_count; i++) closesocket(held[i]);
    return 0;
}

// ---- the status callback ----------------------------------------------------------------------------------------------
static HANDLE done_event, closing_event;
static volatile HINTERNET seen_handle, closed_handle;
static volatile DWORD_PTR seen_context;
static volatile DWORD seen_status, seen_api, seen_error, closings;
static void CALLBACK status(HINTERNET handle, DWORD_PTR context, DWORD code, void *info, DWORD info_length) {
    if (code == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) {
        closed_handle = info && info_length == sizeof(HINTERNET) ? *(HINTERNET *)info : NULL;
        InterlockedIncrement((LONG *)&closings);
        SetEvent(closing_event);
        return;
    }
    seen_handle = handle;
    seen_context = context;
    seen_status = code;
    if (code == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR && info && info_length == sizeof(WINHTTP_ASYNC_RESULT)) {
        seen_api = (DWORD)((WINHTTP_ASYNC_RESULT *)info)->dwResult;
        seen_error = ((WINHTTP_ASYNC_RESULT *)info)->dwError;
    }
    SetEvent(done_event);
}
static void CALLBACK other_status(HINTERNET h, DWORD_PTR c, DWORD s, void *i, DWORD l) { (void)h; (void)c; (void)s; (void)i; (void)l; }

static DWORD resolve(HINTERNET resolver, const WCHAR *url, DWORD flags, const WCHAR *pac, DWORD_PTR context) {
    WINHTTP_AUTOPROXY_OPTIONS options;
    memset(&options, 0, sizeof options);
    options.dwFlags = flags;
    options.lpszAutoConfigUrl = pac;
    ResetEvent(done_event);
    seen_status = seen_error = seen_api = 0;
    DWORD r = WinHttpGetProxyForUrlEx(resolver, url, &options, context);
    if (r != ERROR_IO_PENDING) return r;
    if (WaitForSingleObject(done_event, 30000) != WAIT_OBJECT_0) return WAIT_TIMEOUT;
    return seen_status == WINHTTP_CALLBACK_STATUS_GETPROXYFORURL_COMPLETE ? 0 : seen_error;
}

// ---- WinHttpCrackUrl --------------------------------------------------------------------------------------------------
static int same(const WCHAR *p, DWORD n, const WCHAR *expected) { return p && n == wcslen(expected) && !wcsncmp(p, expected, n); }
static void crack_urls(void) {
    URL_COMPONENTS c;
    const WCHAR *url = L"https://user:pa%20ss@Example.COM:8443/a%20b/c?q=1&r=2#frag";
    // pointers into the URL
    memset(&c, 0, sizeof c);
    c.dwStructSize = sizeof c;
    c.dwSchemeLength = c.dwHostNameLength = c.dwUserNameLength = c.dwPasswordLength = c.dwUrlPathLength = c.dwExtraInfoLength = (DWORD)-1;
    CHECK(WinHttpCrackUrl(url, 0, 0, &c));
    CHECK(c.nScheme == INTERNET_SCHEME_HTTPS && c.nPort == 8443);
    CHECK(same(c.lpszScheme, c.dwSchemeLength, L"https") && c.lpszScheme == url);
    CHECK(same(c.lpszHostName, c.dwHostNameLength, L"Example.COM"));
    CHECK(same(c.lpszUserName, c.dwUserNameLength, L"user") && same(c.lpszPassword, c.dwPasswordLength, L"pa%20ss"));
    CHECK(same(c.lpszUrlPath, c.dwUrlPathLength, L"/a%20b/c") && same(c.lpszExtraInfo, c.dwExtraInfoLength, L"?q=1&r=2#frag"));
    // without extra information asked for, the path carries it (documented); components not asked for stay as they are
    memset(&c, 0, sizeof c);
    c.dwStructSize = sizeof c;
    c.dwUrlPathLength = (DWORD)-1;
    CHECK(WinHttpCrackUrl(url, 0, 0, &c));
    CHECK(same(c.lpszUrlPath, c.dwUrlPathLength, L"/a%20b/c?q=1&r=2#frag") && !c.lpszHostName && !c.dwHostNameLength && c.nPort == 8443);
    // copies into buffers, %xx decoded
    WCHAR host[64], password[64], path[64], extra[64], scheme[8];
    memset(&c, 0, sizeof c);
    c.dwStructSize = sizeof c;
    c.lpszScheme = scheme; c.dwSchemeLength = 8;
    c.lpszHostName = host; c.dwHostNameLength = 64;
    c.lpszPassword = password; c.dwPasswordLength = 64;
    c.lpszUrlPath = path; c.dwUrlPathLength = 64;
    c.lpszExtraInfo = extra; c.dwExtraInfoLength = 64;
    CHECK(WinHttpCrackUrl(url, 0, ICU_DECODE, &c));
    CHECK(!wcscmp(scheme, L"https") && c.dwSchemeLength == 5 && !wcscmp(host, L"Example.COM") && c.dwHostNameLength == 11);
    CHECK(!wcscmp(password, L"pa ss") && c.dwPasswordLength == 5 && !wcscmp(path, L"/a b/c") && c.dwUrlPathLength == 6);
    CHECK(!wcscmp(extra, L"?q=1&r=2#frag"));
    // escaped: spaces and non-ASCII (as UTF-8)
    memset(&c, 0, sizeof c);
    c.dwStructSize = sizeof c;
    c.lpszUrlPath = path; c.dwUrlPathLength = 64;
    CHECK(WinHttpCrackUrl(L"http://h/a b/\x00e9", 0, ICU_ESCAPE, &c) && !wcscmp(path, L"/a%20b/%C3%A9") && c.nPort == 80);
    // a buffer too small
    WCHAR tiny[4];
    memset(&c, 0, sizeof c);
    c.dwStructSize = sizeof c;
    c.lpszHostName = tiny; c.dwHostNameLength = 4;
    CHECK(!WinHttpCrackUrl(url, 0, 0, &c) && GetLastError() == ERROR_INSUFFICIENT_BUFFER);
    // default ports, an empty path, an IPv6 literal, a length that ends the URL early
    memset(&c, 0, sizeof c);
    c.dwStructSize = sizeof c;
    c.dwHostNameLength = c.dwUrlPathLength = (DWORD)-1;
    CHECK(WinHttpCrackUrl(L"http://host.test", 0, 0, &c) && c.nPort == 80 && c.dwUrlPathLength == 0 && same(c.lpszHostName, c.dwHostNameLength, L"host.test"));
    c.dwHostNameLength = c.dwUrlPathLength = (DWORD)-1;
    CHECK(WinHttpCrackUrl(L"https://[::1]:8080/x", 0, 0, &c) && c.nPort == 8080 && same(c.lpszHostName, c.dwHostNameLength, L"[::1]") && same(c.lpszUrlPath, c.dwUrlPathLength, L"/x"));
    c.dwHostNameLength = c.dwUrlPathLength = (DWORD)-1;
    CHECK(WinHttpCrackUrl(L"https://host.test/path#ignored", 17, 0, &c) && c.nPort == 443 && same(c.lpszUrlPath, c.dwUrlPathLength, L""));
    // refused: credentials with ICU_REJECT_USERPWD, other schemes, malformed ones, the wrong structure size
    CHECK(!WinHttpCrackUrl(url, 0, ICU_REJECT_USERPWD, &c) && GetLastError() == ERROR_WINHTTP_INVALID_URL);
    CHECK(!WinHttpCrackUrl(L"ftp://host.test/", 0, 0, &c) && GetLastError() == ERROR_WINHTTP_UNRECOGNIZED_SCHEME);
    CHECK(!WinHttpCrackUrl(L"host.test/path", 0, 0, &c) && GetLastError() == ERROR_WINHTTP_UNRECOGNIZED_SCHEME);
    CHECK(!WinHttpCrackUrl(L"http:/host.test", 0, 0, &c) && GetLastError() == ERROR_WINHTTP_INVALID_URL);
    CHECK(!WinHttpCrackUrl(L"http://:80/", 0, 0, &c) && GetLastError() == ERROR_WINHTTP_INVALID_URL);
    CHECK(!WinHttpCrackUrl(L"http://host.test:99999/", 0, 0, &c) && GetLastError() == ERROR_WINHTTP_INVALID_URL);
    CHECK(!WinHttpCrackUrl(L"http://host.test:8x/", 0, 0, &c) && GetLastError() == ERROR_WINHTTP_INVALID_URL);
    c.dwStructSize = 8;
    CHECK(!WinHttpCrackUrl(url, 0, 0, &c) && GetLastError() == ERROR_INVALID_PARAMETER);
}

int main(void) {
    crack_urls();

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in address = { 0 };
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int address_length = sizeof address;
    CHECK(!bind(listener, (struct sockaddr *)&address, sizeof address) && !listen(listener, 8) &&
          !getsockname(listener, (struct sockaddr *)&address, &address_length));
    SOCKET closed = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);           // a port nothing listens on
    struct sockaddr_in closed_address = { 0 };
    closed_address.sin_family = AF_INET;
    closed_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int closed_length = sizeof closed_address;
    bind(closed, (struct sockaddr *)&closed_address, sizeof closed_address);
    getsockname(closed, (struct sockaddr *)&closed_address, &closed_length);
    closesocket(closed);
    HANDLE server = CreateThread(NULL, 0, serve, NULL, 0, NULL);
    WCHAR pac[96], bad[96], hang[96], missing[96];
    swprintf(pac, 96, L"http://127.0.0.1:%u/proxy.pac", ntohs(address.sin_port));
    swprintf(bad, 96, L"http://127.0.0.1:%u/bad.pac", ntohs(address.sin_port));
    swprintf(hang, 96, L"http://127.0.0.1:%u/hang.pac", ntohs(address.sin_port));
    swprintf(missing, 96, L"http://127.0.0.1:%u/proxy.pac", ntohs(closed_address.sin_port));
    done_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    closing_event = CreateEventW(NULL, FALSE, FALSE, NULL);

    // ---- the resolver's session is opened with WINHTTP_FLAG_ASYNC, as documented; a synchronous one is the wrong kind
    HINTERNET sync_session = WinHttpOpen(L"vxrt proxy resolver test", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    CHECK(sync_session != NULL);
    HINTERNET from_sync = (HINTERNET)1;
    CHECK(WinHttpCreateProxyResolver(sync_session, &from_sync) == ERROR_WINHTTP_INCORRECT_HANDLE_TYPE && from_sync == NULL);
    CHECK(WinHttpCloseHandle(sync_session));
    HINTERNET session = WinHttpOpen(L"vxrt proxy resolver test", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
    CHECK(session != NULL);
    // requests in an asynchronous session are refused (not run synchronously) until VXRuntime completes them by callback
    SetLastError(0);
    CHECK(WinHttpConnect(session, L"127.0.0.1", 80, 0) == NULL && GetLastError() == ERROR_NOT_SUPPORTED);
    // WINHTTP_FLAG_SECURE_DEFAULTS sets WINHTTP_FLAG_ASYNC too (WinHttpOpen's documentation): such a session is taken
    HINTERNET secure_session = WinHttpOpen(L"vxrt proxy resolver test", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, SECURE_DEFAULTS);
    CHECK(secure_session != NULL);
    HINTERNET from_secure = NULL;
    CHECK(WinHttpCreateProxyResolver(secure_session, &from_secure) == ERROR_SUCCESS && from_secure);
    CHECK(WinHttpCloseHandle(from_secure));
    CHECK(WinHttpCloseHandle(secure_session));

    // ---- callbacks: the previous one comes back; the session's goes to its resolver
    DWORD flags = WINHTTP_CALLBACK_FLAG_REQUEST_ERROR | WINHTTP_CALLBACK_FLAG_GETPROXYFORURL_COMPLETE | WINHTTP_CALLBACK_FLAG_HANDLES;
    CHECK(WinHttpSetStatusCallback(session, other_status, flags, 0) == NULL);
    CHECK(WinHttpSetStatusCallback(session, status, flags, 0) == other_status);
    CHECK(WinHttpSetStatusCallback(NULL, status, flags, 0) == WINHTTP_INVALID_STATUS_CALLBACK);

    HINTERNET resolver = NULL;
    CHECK(WinHttpCreateProxyResolver(NULL, &resolver) == ERROR_INVALID_HANDLE);
    CHECK(WinHttpCreateProxyResolver(session, NULL) == ERROR_INVALID_PARAMETER);
    CHECK(WinHttpCreateProxyResolver(session, &resolver) == ERROR_SUCCESS && resolver);
    HINTERNET not_a_session = NULL;
    CHECK(WinHttpCreateProxyResolver(resolver, &not_a_session) == ERROR_WINHTTP_INCORRECT_HANDLE_TYPE);
    WINHTTP_PROXY_RESULT result;
    memset(&result, 0, sizeof result);
    CHECK(WinHttpGetProxyResult(resolver, &result) == ERROR_WINHTTP_INCORRECT_HANDLE_STATE);
    CHECK(WinHttpGetProxyResult(session, &result) == ERROR_WINHTTP_INCORRECT_HANDLE_TYPE);

    // ---- the PAC file's answer: its list, in order
    CHECK(resolve(resolver, L"http://www.example.test/page", WINHTTP_AUTOPROXY_CONFIG_URL, pac, 0x51) == 0);
    CHECK(seen_handle == resolver && seen_context == 0x51);
    CHECK(WinHttpGetProxyResult(resolver, &result) == ERROR_SUCCESS && result.cEntries == 3 && result.pEntries);
    if (result.cEntries == 3) {
        WINHTTP_PROXY_RESULT_ENTRY *e = result.pEntries;
        CHECK(e[0].fProxy && e[0].ProxyScheme == INTERNET_SCHEME_HTTP && e[0].pwszProxy && !wcscmp(e[0].pwszProxy, L"proxy.test") && e[0].ProxyPort == 3128);
        CHECK(e[1].fProxy && e[1].ProxyScheme == INTERNET_SCHEME_SOCKS && e[1].pwszProxy && !wcscmp(e[1].pwszProxy, L"socks.test") && e[1].ProxyPort == 1081);
        CHECK(!e[2].fProxy && !e[2].fBypass);
    } else printf("winhttp_proxy: %lu entries\n", result.cEntries);
    WinHttpFreeProxyResult(&result);
    CHECK(result.cEntries == 0 && result.pEntries == NULL);
    CHECK(resolve(resolver, L"https://www.direct.test/", WINHTTP_AUTOPROXY_CONFIG_URL, pac, 0x52) == 0);
    CHECK(WinHttpGetProxyResult(resolver, &result) == ERROR_SUCCESS && result.cEntries == 1 && result.pEntries && !result.pEntries[0].fProxy);
    WinHttpFreeProxyResult(&result);

    // ---- failures arrive as REQUEST_ERROR for API_GET_PROXY_FOR_URL
    DWORD r = resolve(resolver, L"http://www.example.test/", WINHTTP_AUTOPROXY_CONFIG_URL, bad, 0x53);
    CHECK(r == ERROR_WINHTTP_BAD_AUTO_PROXY_SCRIPT && seen_api == API_GET_PROXY_FOR_URL && seen_context == 0x53);
    if (r != ERROR_WINHTTP_BAD_AUTO_PROXY_SCRIPT) printf("winhttp_proxy: a bad script gave %lu\n", r);
    r = resolve(resolver, L"http://www.example.test/", WINHTTP_AUTOPROXY_CONFIG_URL, missing, 0x54);
    CHECK(r == ERROR_WINHTTP_UNABLE_TO_DOWNLOAD_SCRIPT && seen_api == API_GET_PROXY_FOR_URL);
    if (r != ERROR_WINHTTP_UNABLE_TO_DOWNLOAD_SCRIPT) printf("winhttp_proxy: an unreachable script gave %lu\n", r);
    CHECK(WinHttpGetProxyResult(resolver, &result) == ERROR_WINHTTP_INCORRECT_HANDLE_STATE);   // (the last call failed)

    // ---- the Mac's static settings: whatever they are, a well-formed list
    CHECK(resolve(resolver, L"http://www.example.test/", WINHTTP_AUTOPROXY_ALLOW_STATIC, NULL, 0x55) == 0);
    CHECK(WinHttpGetProxyResult(resolver, &result) == ERROR_SUCCESS && result.cEntries >= 1);
    for (DWORD i = 0; i < result.cEntries; i++) CHECK(!result.pEntries[i].fProxy || (result.pEntries[i].pwszProxy && result.pEntries[i].ProxyPort));
    WinHttpFreeProxyResult(&result);

    // ---- refused at once
    WINHTTP_AUTOPROXY_OPTIONS options;
    memset(&options, 0, sizeof options);
    options.dwFlags = WINHTTP_AUTOPROXY_CONFIG_URL;
    CHECK(WinHttpGetProxyForUrlEx(resolver, L"http://www.example.test/", NULL, 0) == ERROR_INVALID_PARAMETER);
    CHECK(WinHttpGetProxyForUrlEx(resolver, L"http://www.example.test/", &options, 0) == ERROR_INVALID_PARAMETER);   // no PAC URL
    options.lpszAutoConfigUrl = L"ftp://127.0.0.1/proxy.pac";
    CHECK(WinHttpGetProxyForUrlEx(resolver, L"http://www.example.test/", &options, 0) == ERROR_WINHTTP_UNRECOGNIZED_SCHEME);
    options.lpszAutoConfigUrl = pac;
    CHECK(WinHttpGetProxyForUrlEx(resolver, L"not a url", &options, 0) == ERROR_WINHTTP_INVALID_URL);
    CHECK(WinHttpGetProxyForUrlEx(NULL, L"http://www.example.test/", &options, 0) == ERROR_INVALID_HANDLE);
    CHECK(WinHttpGetProxyForUrlEx(session, L"http://www.example.test/", &options, 0) == ERROR_WINHTTP_INCORRECT_HANDLE_TYPE);
    options.dwFlags = WINHTTP_AUTOPROXY_AUTO_DETECT;                     // detection without a method
    options.lpszAutoConfigUrl = NULL;
    CHECK(WinHttpGetProxyForUrlEx(resolver, L"http://www.example.test/", &options, 0) == ERROR_INVALID_PARAMETER);

    // ---- WinHttpGetProxyForUrl: the same answers at once, on any session, as a WINHTTP_PROXY_INFO freed with GlobalFree
    HINTERNET plain = WinHttpOpen(L"vxrt proxy resolver test", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    CHECK(plain != NULL);
    WINHTTP_PROXY_INFO info;
    memset(&options, 0, sizeof options);
    options.dwFlags = WINHTTP_AUTOPROXY_CONFIG_URL;
    options.lpszAutoConfigUrl = pac;
    memset(&info, 0xcc, sizeof info);
    CHECK(WinHttpGetProxyForUrl(plain, L"http://www.example.test/page", &options, &info));
    CHECK(info.dwAccessType == WINHTTP_ACCESS_TYPE_NAMED_PROXY && info.lpszProxy && !info.lpszProxyBypass &&
          !wcscmp(info.lpszProxy, L"proxy.test:3128;socks=socks.test:1081"));   // (the proxies before DIRECT, in order)
    if (info.lpszProxy && info.dwAccessType == WINHTTP_ACCESS_TYPE_NAMED_PROXY) {
        printf("winhttp_proxy: WinHttpGetProxyForUrl gave \"%ls\"\n", info.lpszProxy);
        CHECK(GlobalFree(info.lpszProxy) == NULL);
    }
    memset(&info, 0xcc, sizeof info);
    CHECK(WinHttpGetProxyForUrl(session, L"https://www.direct.test/", &options, &info) &&   // (an asynchronous session too)
          info.dwAccessType == WINHTTP_ACCESS_TYPE_NO_PROXY && !info.lpszProxy && !info.lpszProxyBypass);
    // its failures are WinHttpGetProxyForUrlEx's, as the last error
    options.lpszAutoConfigUrl = bad;
    SetLastError(0);
    CHECK(!WinHttpGetProxyForUrl(plain, L"http://www.example.test/", &options, &info) && GetLastError() == ERROR_WINHTTP_BAD_AUTO_PROXY_SCRIPT);
    options.lpszAutoConfigUrl = missing;
    SetLastError(0);
    CHECK(!WinHttpGetProxyForUrl(plain, L"http://www.example.test/", &options, &info) && GetLastError() == ERROR_WINHTTP_UNABLE_TO_DOWNLOAD_SCRIPT);
    options.lpszAutoConfigUrl = L"ftp://127.0.0.1/proxy.pac";
    SetLastError(0);
    CHECK(!WinHttpGetProxyForUrl(plain, L"http://www.example.test/", &options, &info) && GetLastError() == ERROR_WINHTTP_UNRECOGNIZED_SCHEME);
    options.lpszAutoConfigUrl = pac;
    SetLastError(0);
    CHECK(!WinHttpGetProxyForUrl(plain, L"not a url", &options, &info) && GetLastError() == ERROR_WINHTTP_INVALID_URL);
    SetLastError(0);
    CHECK(!WinHttpGetProxyForUrl(resolver, L"http://www.example.test/", &options, &info) && GetLastError() == ERROR_WINHTTP_INCORRECT_HANDLE_TYPE);
    SetLastError(0);
    CHECK(!WinHttpGetProxyForUrl(NULL, L"http://www.example.test/", &options, &info) && GetLastError() == ERROR_INVALID_HANDLE);
    SetLastError(0);
    CHECK(!WinHttpGetProxyForUrl(plain, L"http://www.example.test/", &options, NULL) && GetLastError() == ERROR_INVALID_PARAMETER);
    options.lpszAutoConfigUrl = NULL;                                     // CONFIG_URL without the URL
    SetLastError(0);
    CHECK(!WinHttpGetProxyForUrl(plain, L"http://www.example.test/", &options, &info) && GetLastError() == ERROR_INVALID_PARAMETER);
    // the Mac's static settings: whatever they are, a well-formed answer
    options.dwFlags = WINHTTP_AUTOPROXY_ALLOW_STATIC;
    memset(&info, 0xcc, sizeof info);
    CHECK(WinHttpGetProxyForUrl(plain, L"http://www.example.test/", &options, &info) && !info.lpszProxyBypass &&
          (info.dwAccessType == WINHTTP_ACCESS_TYPE_NO_PROXY ? !info.lpszProxy : info.dwAccessType == WINHTTP_ACCESS_TYPE_NAMED_PROXY && info.lpszProxy && *info.lpszProxy));
    if (info.dwAccessType == WINHTTP_ACCESS_TYPE_NAMED_PROXY && info.lpszProxy) GlobalFree(info.lpszProxy);
    CHECK(WinHttpCloseHandle(plain));

    // ---- closing: the last notification is HANDLE_CLOSING; afterwards the handle is gone
    closings = 0;
    CHECK(WinHttpCloseHandle(resolver));
    CHECK(WaitForSingleObject(closing_event, 5000) == WAIT_OBJECT_0 && closed_handle == resolver && closings == 1);
    CHECK(WinHttpGetProxyResult(resolver, &result) == ERROR_WINHTTP_INCORRECT_HANDLE_TYPE);

    // closed while its PAC file never arrives: cancelled, then closed
    HINTERNET waiting = NULL;
    CHECK(WinHttpCreateProxyResolver(session, &waiting) == ERROR_SUCCESS);
    memset(&options, 0, sizeof options);
    options.dwFlags = WINHTTP_AUTOPROXY_CONFIG_URL;
    options.lpszAutoConfigUrl = hang;
    ResetEvent(done_event);
    seen_error = 0;
    CHECK(WinHttpGetProxyForUrlEx(waiting, L"http://www.example.test/", &options, 0x56) == ERROR_IO_PENDING);
    Sleep(300);
    CHECK(WinHttpCloseHandle(waiting));
    CHECK(WaitForSingleObject(done_event, 5000) == WAIT_OBJECT_0 && seen_status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR &&
          seen_error == ERROR_WINHTTP_OPERATION_CANCELLED);
    CHECK(WaitForSingleObject(closing_event, 5000) == WAIT_OBJECT_0 && closed_handle == waiting);

    // the session's own closing is reported too
    CHECK(WinHttpCloseHandle(session));
    CHECK(WaitForSingleObject(closing_event, 5000) == WAIT_OBJECT_0 && closed_handle == session);

    stop_server = 1;
    WaitForSingleObject(server, 5000);
    closesocket(listener);
    WSACleanup();
    if (failures) { printf("winhttp_proxy: %d checks failed\n", failures); return 1; }
    puts("winhttp_proxy: WinHttpCrackUrl, async session, status callbacks, PAC resolution (list, DIRECT, bad, unreachable, cancelled), static settings, WinHttpGetProxyForUrl: ok");
    return 0;
}
