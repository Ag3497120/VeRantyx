#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <winhttp.h>
#include <stdio.h>
#include <string.h>

static SOCKET listener;
static int server_ok, redirect_ok, proxy_ok, bypass_ok;

static SOCKET accept_with_timeout(void) {
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(listener, &readable);
    struct timeval timeout = { 10, 0 };
    if (select(0, &readable, NULL, NULL, &timeout) <= 0) return INVALID_SOCKET;
    return accept(listener, NULL, NULL);
}

static int receive_headers(SOCKET client, char *request, size_t capacity) {
    size_t used = 0;
    while (used + 1 < capacity) {
        int n = recv(client, request + used, (int)(capacity - used - 1), 0);
        if (n <= 0) break;
        used += (size_t)n;
        request[used] = 0;
        if (strstr(request, "\r\n\r\n")) break;
    }
    return (int)used;
}

static int send_all(SOCKET client, const char *response, size_t size) {
    size_t sent = 0;
    while (sent < size) {
        int n = send(client, response + sent, (int)(size - sent), 0);
        if (n <= 0) break;
        sent += (size_t)n;
    }
    return sent == size;
}

static DWORD WINAPI serve_one(void *unused) {
    (void)unused;
    SOCKET client = accept_with_timeout();
    if (client == INVALID_SOCKET) return 1;
    char request[2048] = { 0 };
    int used = 0;
    while (used < (int)sizeof request - 1) {
        int n = recv(client, request + used, (int)sizeof request - 1 - used, 0);
        if (n <= 0) break;
        used += n;
        request[used] = 0;
        if (strstr(request, "\r\n\r\nping")) break;
    }
    server_ok = strstr(request, "POST /health?probe=1 HTTP/") != NULL && strstr(request, "ping") != NULL;
    static const char response[] = "HTTP/1.1 200 OK\r\nContent-Length: 4\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\npong";
    int sent = 0;
    while (sent < (int)sizeof response - 1) {
        int n = send(client, response + sent, (int)sizeof response - 1 - sent, 0);
        if (n <= 0) break;
        sent += n;
    }
    closesocket(client);
    if (!server_ok || sent != (int)sizeof response - 1) return 2;
    // a second connection: GET /go is answered with a redirect; a client whose redirect policy is "never" must see it
    // as the response (nobody serves a third connection, so a client that followed it would fail)
    client = accept_with_timeout();
    if (client == INVALID_SOCKET) return 3;
    memset(request, 0, sizeof request);
    used = 0;
    while (used < (int)sizeof request - 1) {
        int n = recv(client, request + used, (int)sizeof request - 1 - used, 0);
        if (n <= 0) break;
        used += n;
        request[used] = 0;
        if (strstr(request, "\r\n\r\n")) break;
    }
    redirect_ok = strstr(request, "GET /go HTTP/") != NULL;
    static const char redirect[] = "HTTP/1.1 302 Found\r\nLocation: /health?probe=1\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    sent = 0;
    while (sent < (int)sizeof redirect - 1) {
        int n = send(client, redirect + sent, (int)sizeof redirect - 1 - sent, 0);
        if (n <= 0) break;
        sent += n;
    }
    closesocket(client);
    if (!redirect_ok || sent != (int)sizeof redirect - 1) return 4;

    // An HTTP proxy receives absolute-form request targets; bypassed hosts go directly to the origin.
    client = accept_with_timeout();
    if (client == INVALID_SOCKET) return 5;
    memset(request, 0, sizeof request);
    receive_headers(client, request, sizeof request);
    proxy_ok = strstr(request, "GET http://proxy-target.invalid/via-proxy HTTP/") != NULL;
    static const char proxy_response[] = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nproxy";
    int proxy_sent = send_all(client, proxy_response, sizeof proxy_response - 1);
    closesocket(client);

    client = accept_with_timeout();
    if (client == INVALID_SOCKET) return 6;
    memset(request, 0, sizeof request);
    receive_headers(client, request, sizeof request);
    bypass_ok = strstr(request, "GET /bypass HTTP/") != NULL;
    static const char bypass_response[] = "HTTP/1.1 200 OK\r\nContent-Length: 6\r\nConnection: close\r\n\r\ndirect";
    int bypass_sent = send_all(client, bypass_response, sizeof bypass_response - 1);
    closesocket(client);
    return proxy_ok && proxy_sent && bypass_ok && bypass_sent ? 0 : 7;
}

int main(void) {
    WINHTTP_PROXY_INFO default_proxy = { 0 };
    if (!WinHttpGetDefaultProxyConfiguration(&default_proxy) ||
        default_proxy.dwAccessType != WINHTTP_ACCESS_TYPE_NO_PROXY ||
        default_proxy.lpszProxy || default_proxy.lpszProxyBypass) {
        if (default_proxy.lpszProxy) GlobalFree(default_proxy.lpszProxy);
        if (default_proxy.lpszProxyBypass) GlobalFree(default_proxy.lpszProxyBypass);
        fprintf(stderr, "winhttp: default proxy configuration should start as a direct connection\n");
        return 1;
    }

    WINHTTP_CURRENT_USER_IE_PROXY_CONFIG user_proxy = { 0 };
    BOOL user_proxy_ok = WinHttpGetIEProxyConfigForCurrentUser(&user_proxy);
    DWORD user_proxy_error = user_proxy_ok ? ERROR_SUCCESS : GetLastError();
    int proxy_valid = user_proxy_ok || user_proxy_error == ERROR_FILE_NOT_FOUND;
    if (user_proxy_ok) {
        if (user_proxy.lpszAutoConfigUrl) GlobalFree(user_proxy.lpszAutoConfigUrl);
        if (user_proxy.lpszProxy) GlobalFree(user_proxy.lpszProxy);
        if (user_proxy.lpszProxyBypass) GlobalFree(user_proxy.lpszProxyBypass);
    }
    if (!proxy_valid) {
        fprintf(stderr, "winhttp: current-user proxy configuration failed with %lu\n", user_proxy_error);
        return 1;
    }
    puts("winhttp: default and current-user proxy configuration: ok");

    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data)) return 1;
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) return 2;
    struct sockaddr_in address = { 0 };
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, (struct sockaddr *)&address, sizeof address) || listen(listener, 4)) return 3;
    int address_size = sizeof address;
    if (getsockname(listener, (struct sockaddr *)&address, &address_size)) return 4;
    HANDLE thread = CreateThread(NULL, 0, serve_one, NULL, 0, NULL);
    if (!thread) return 5;

    HINTERNET session = WinHttpOpen(L"vxrt local test", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connection = session ? WinHttpConnect(session, L"127.0.0.1", ntohs(address.sin_port), 0) : NULL;
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"POST", L"/health?probe=1", NULL, NULL,
                                                         WINHTTP_DEFAULT_ACCEPT_TYPES, 0) : NULL;
    int ok = request && WinHttpSendRequest(request, L"Content-Type: text/plain\r\n", (DWORD)-1,
                                           "ping", 4, 4, 0);
    DWORD written = 0;
    if (ok) ok = WinHttpWriteData(request, "ping", 0, &written) && written == 0;
    if (ok) ok = WinHttpReceiveResponse(request, NULL);
    DWORD status = 0, length = sizeof status;
    if (ok) ok = WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                     WINHTTP_HEADER_NAME_BY_INDEX, &status, &length, WINHTTP_NO_HEADER_INDEX) && status == 200;
    WCHAR content_type[64] = { 0 };
    DWORD content_type_bytes = sizeof content_type;
    if (ok) ok = WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX,
                                     content_type, &content_type_bytes, WINHTTP_NO_HEADER_INDEX) &&
                 !wcscmp(content_type, L"text/plain");
    DWORD content_length = 0, content_length_bytes = sizeof content_length;
    if (ok) ok = WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                     WINHTTP_HEADER_NAME_BY_INDEX, &content_length, &content_length_bytes,
                                     WINHTTP_NO_HEADER_INDEX) && content_length == 4;
    WCHAR raw_headers[512] = { 0 };
    DWORD raw_header_bytes = sizeof raw_headers;
    if (ok) ok = WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS, WINHTTP_HEADER_NAME_BY_INDEX,
                                     raw_headers, &raw_header_bytes, WINHTTP_NO_HEADER_INDEX) &&
                 !wcsncmp(raw_headers, L"HTTP/1.1 200", 12) && raw_header_bytes >= 4;
    DWORD available = 0;
    if (ok) ok = WinHttpQueryDataAvailable(request, &available) && available == 4;
    char body[5] = { 0 };
    DWORD received = 0;
    if (ok) ok = WinHttpReadData(request, body, 3, &received) && received == 3 && !memcmp(body, "pon", 3);
    if (ok) ok = WinHttpReadData(request, body + 3, 2, &received) && received == 1 && !memcmp(body, "pong", 4);

    // the redirect, not followed: status 302 and its Location header
    HINTERNET redirected = ok && connection ? WinHttpOpenRequest(connection, L"GET", L"/go", NULL, NULL,
                                                                 WINHTTP_DEFAULT_ACCEPT_TYPES, 0) : NULL;
    DWORD never = WINHTTP_OPTION_REDIRECT_POLICY_NEVER, redirect_status = 0, redirect_status_size = sizeof redirect_status;
    WCHAR location[128] = { 0 };
    DWORD location_bytes = sizeof location;
    int redirect_seen = redirected && WinHttpSetOption(redirected, WINHTTP_OPTION_REDIRECT_POLICY, &never, sizeof never) &&
                        WinHttpSendRequest(redirected, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                        WinHttpReceiveResponse(redirected, NULL) &&
                        WinHttpQueryHeaders(redirected, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                            &redirect_status, &redirect_status_size, WINHTTP_NO_HEADER_INDEX) && redirect_status == 302 &&
                        WinHttpQueryHeaders(redirected, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                                            location, &location_bytes, WINHTTP_NO_HEADER_INDEX) && wcsstr(location, L"/health?probe=1");
    if (redirected) WinHttpCloseHandle(redirected);

    WCHAR proxy_name[64];
    swprintf(proxy_name, sizeof proxy_name / sizeof proxy_name[0], L"127.0.0.1:%u", ntohs(address.sin_port));
    HINTERNET named_session = WinHttpOpen(L"vxrt proxy test", WINHTTP_ACCESS_TYPE_NAMED_PROXY,
                                           proxy_name, L"127.0.0.1;*.bypass.invalid", 0);
    HINTERNET proxy_connection = named_session ? WinHttpConnect(named_session, L"proxy-target.invalid", 80, 0) : NULL;
    HINTERNET proxy_request = proxy_connection ? WinHttpOpenRequest(proxy_connection, L"GET", L"/via-proxy", NULL, NULL,
                                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, 0) : NULL;
    int named_proxy_seen = proxy_request && WinHttpSendRequest(proxy_request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                                               WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                           WinHttpReceiveResponse(proxy_request, NULL);
    DWORD proxy_status = 0, proxy_status_size = sizeof proxy_status;
    if (named_proxy_seen)
        named_proxy_seen = WinHttpQueryHeaders(proxy_request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                               WINHTTP_HEADER_NAME_BY_INDEX, &proxy_status, &proxy_status_size,
                                               WINHTTP_NO_HEADER_INDEX) && proxy_status == 200;
    char proxy_body[6] = { 0 };
    DWORD proxy_body_size = 0;
    if (named_proxy_seen)
        named_proxy_seen = WinHttpReadData(proxy_request, proxy_body, 5, &proxy_body_size) && proxy_body_size == 5 &&
                           !memcmp(proxy_body, "proxy", 5);
    if (proxy_request) WinHttpCloseHandle(proxy_request);
    if (proxy_connection) WinHttpCloseHandle(proxy_connection);

    HINTERNET bypass_connection = named_session ? WinHttpConnect(named_session, L"127.0.0.1", ntohs(address.sin_port), 0) : NULL;
    HINTERNET bypass_request = bypass_connection ? WinHttpOpenRequest(bypass_connection, L"GET", L"/bypass", NULL, NULL,
                                                                       WINHTTP_DEFAULT_ACCEPT_TYPES, 0) : NULL;
    int bypass_seen = bypass_request && WinHttpSendRequest(bypass_request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                      WinHttpReceiveResponse(bypass_request, NULL);
    DWORD bypass_status = 0, bypass_status_size = sizeof bypass_status;
    if (bypass_seen)
        bypass_seen = WinHttpQueryHeaders(bypass_request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                           WINHTTP_HEADER_NAME_BY_INDEX, &bypass_status, &bypass_status_size,
                                           WINHTTP_NO_HEADER_INDEX) && bypass_status == 200;
    char bypass_body[7] = { 0 };
    DWORD bypass_body_size = 0;
    if (bypass_seen)
        bypass_seen = WinHttpReadData(bypass_request, bypass_body, 6, &bypass_body_size) && bypass_body_size == 6 &&
                      !memcmp(bypass_body, "direct", 6);
    if (bypass_request) WinHttpCloseHandle(bypass_request);
    if (bypass_connection) WinHttpCloseHandle(bypass_connection);
    if (named_session) WinHttpCloseHandle(named_session);

    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    if (session) WinHttpCloseHandle(session);
    WaitForSingleObject(thread, INFINITE);
    DWORD server_exit = 1;
    GetExitCodeThread(thread, &server_exit);
    CloseHandle(thread);
    closesocket(listener);
    WSACleanup();
    if (!ok || !server_ok || server_exit || !redirect_seen || !named_proxy_seen || !proxy_ok ||
        !bypass_seen || !bypass_ok) {
        fprintf(stderr, "winhttp: local request failed (status=%lu, body=%s, server=%d/%lu, redirect=%d/%lu, proxy=%d/%d/%lu, bypass=%d/%d/%lu)\n",
                status, body, server_ok, server_exit, redirect_seen, redirect_status,
                named_proxy_seen, proxy_ok, proxy_status, bypass_seen, bypass_ok, bypass_status);
        return 1;
    }
    puts("winhttp: local POST, response headers, redirect policy, named proxy routing and bypass: ok");
    return 0;
}
