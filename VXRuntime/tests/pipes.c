// Milestone 9a: named pipes. In one process: a message-mode server with overlapped ConnectNamedPipe and ReadFile
// (event and GetOverlappedResult), a client from CreateFile, messages longer than the reader's buffer
// (ERROR_MORE_DATA), PeekNamedPipe, a byte-mode pipe, and a broken pipe when the client goes. Between processes:
// "pipes.exe serve" answers one request, "pipes.exe ask" sends it (make check runs both).
#include <windows.h>
#include <stdio.h>
#include <string.h>
static int failures;
#define CHECK(c) do { if (!(c)) { printf("check failed (line %d): %s\n", __LINE__, #c); failures++; } } while (0)
#define NAME "\\\\.\\pipe\\vxrt_test_pipe"

static DWORD WINAPI client(void *arg) {
    (void)arg;
    CHECK(WaitNamedPipeA(NAME, 2000));
    HANDLE h = CreateFileA(NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(h != INVALID_HANDLE_VALUE);
    DWORD mode = PIPE_READMODE_MESSAGE;
    CHECK(SetNamedPipeHandleState(h, &mode, NULL, NULL));
    DWORD n;
    CHECK(WriteFile(h, "hello", 5, &n, NULL) && n == 5);
    CHECK(WriteFile(h, "a longer message, 34 bytes long..", 34, &n, NULL) && n == 34);
    char reply[64] = { 0 };
    CHECK(ReadFile(h, reply, sizeof reply, &n, NULL) && n == 6 && !memcmp(reply, "world!", 6));
    CloseHandle(h);
    return 0;
}

static void in_process(void) {
    HANDLE server = CreateNamedPipeA(NAME, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                                     4, 4096, 4096, 0, NULL);
    CHECK(server != INVALID_HANDLE_VALUE);
    CHECK(GetFileType(server) == FILE_TYPE_PIPE);
    OVERLAPPED ov = { 0 };
    ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    BOOL connected = ConnectNamedPipe(server, &ov);
    CHECK(!connected && GetLastError() == ERROR_IO_PENDING);
    HANDLE t = CreateThread(NULL, 0, client, NULL, 0, NULL);
    CHECK(WaitForSingleObject(ov.hEvent, 3000) == WAIT_OBJECT_0);
    DWORD n = 0;
    CHECK(GetOverlappedResult(server, &ov, &n, FALSE));
    // A message read in full, then one read in two parts.
    char buf[64] = { 0 };
    ResetEvent(ov.hEvent);
    BOOL r = ReadFile(server, buf, sizeof buf, &n, &ov);
    CHECK(r || GetLastError() == ERROR_IO_PENDING);
    CHECK(GetOverlappedResult(server, &ov, &n, TRUE) && n == 5 && !memcmp(buf, "hello", 5));
    DWORD avail = 0, left = 0;
    for (int i = 0; i < 200 && !avail; i++) { PeekNamedPipe(server, NULL, 0, NULL, &avail, NULL); if (!avail) Sleep(5); }
    CHECK(avail == 34);
    char part[20];
    ResetEvent(ov.hEvent);
    r = ReadFile(server, part, sizeof part, &n, &ov);
    BOOL got = GetOverlappedResult(server, &ov, &n, TRUE);
    CHECK(!got && GetLastError() == ERROR_MORE_DATA && n == 20 && !memcmp(part, "a longer message, 34", 20));
    CHECK(PeekNamedPipe(server, NULL, 0, NULL, &avail, &left) && avail == 14);
    ResetEvent(ov.hEvent);
    ReadFile(server, part, sizeof part, &n, &ov);
    CHECK(GetOverlappedResult(server, &ov, &n, TRUE) && n == 14 && !memcmp(part, " bytes long..", 13));
    CHECK(WriteFile(server, "world!", 6, &n, NULL) && n == 6);
    WaitForSingleObject(t, 3000);
    CloseHandle(t);
    // The client has gone: reading fails with a broken pipe; the instance can take another client.
    ResetEvent(ov.hEvent);
    ReadFile(server, buf, sizeof buf, &n, &ov);
    CHECK(!GetOverlappedResult(server, &ov, &n, TRUE) && GetLastError() == ERROR_BROKEN_PIPE);
    CHECK(DisconnectNamedPipe(server));
    CloseHandle(ov.hEvent);
    CloseHandle(server);
    // No server: opening fails.
    CHECK(CreateFileA(NAME, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL) == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_NOT_FOUND);
}

// Between processes: the server answers "ping" with "pong" once.
#define XNAME "\\\\.\\pipe\\vxrt_test_cross"
static int serve(void) {
    HANDLE h = CreateNamedPipeA(XNAME, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) { printf("pipes serve: CreateNamedPipe failed\n"); return 1; }
    if (!ConnectNamedPipe(h, NULL) && GetLastError() != ERROR_PIPE_CONNECTED) { printf("pipes serve: no client\n"); return 1; }
    char buf[16] = { 0 }; DWORD n;
    if (!ReadFile(h, buf, sizeof buf, &n, NULL) || n != 4 || memcmp(buf, "ping", 4)) { printf("pipes serve: bad request\n"); return 1; }
    WriteFile(h, "pong", 4, &n, NULL);
    FlushFileBuffers(h);
    Sleep(200);
    CloseHandle(h);
    return 0;
}
static int ask(void) {
    if (!WaitNamedPipeA(XNAME, 5000)) { printf("pipes ask: no server -> FAILED\n"); return 1; }
    HANDLE h = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 100 && h == INVALID_HANDLE_VALUE; i++) { h = CreateFileA(XNAME, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL); if (h == INVALID_HANDLE_VALUE) Sleep(20); }
    char buf[16] = { 0 }; DWORD n = 0;
    BOOL ok = h != INVALID_HANDLE_VALUE && WriteFile(h, "ping", 4, &n, NULL) && ReadFile(h, buf, sizeof buf, &n, NULL) && n == 4 && !memcmp(buf, "pong", 4);
    printf("pipes between processes: %s -> %s\n", ok ? "ping, pong" : "no answer", ok ? "ok" : "FAILED");
    return !ok;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "serve")) return serve();
    if (argc > 1 && !strcmp(argv[1], "ask")) return ask();
    in_process();
    printf("pipes: overlapped connect and reads, messages, ERROR_MORE_DATA, peek, broken pipe: %d failed checks -> %s\n", failures, failures ? "FAILED" : "ok");
    return failures;
}
