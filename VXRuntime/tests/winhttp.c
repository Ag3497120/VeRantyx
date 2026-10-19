#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <winhttp.h>
#include <stdio.h>
#include <string.h>

static SOCKET listener;
static int server_ok, redirect_ok;

static DWORD WINAPI serve_one(void *unused) {
    (void)unused;
    SOCKET client = accept(listener, NULL, NULL);
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
    client = accept(listener, NULL, NULL);
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
    return redirect_ok && sent == (int)sizeof redirect - 1 ? 0 : 4;
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
    if (bind(listener, (struct sockaddr *)&address, sizeof address) || listen(listener, 1)) return 3;
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
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    if (session) WinHttpCloseHandle(session);
    WaitForSingleObject(thread, INFINITE);
    DWORD server_exit = 1;
    GetExitCodeThread(thread, &server_exit);
    CloseHandle(thread);
    closesocket(listener);
    WSACleanup();
    if (!ok || !server_ok || server_exit || !redirect_seen) {
        fprintf(stderr, "winhttp: local POST / chunked response failed (status=%lu, body=%s, server=%d/%lu, redirect seen=%d status=%lu)\n",
                status, body, server_ok, server_exit, redirect_seen, redirect_status);
        return 1;
    }
    puts("winhttp: local POST, body upload, status/content headers, chunked reads, a redirect kept as the response: ok");
    return 0;
}
