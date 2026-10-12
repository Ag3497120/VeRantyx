// Milestone 5a: I/O completion ports. Packets posted from several threads, overlapped file reads and writes
// completing through a port, events in OVERLAPPED, timeouts.
#include <windows.h>
#include <stdio.h>
#include <string.h>
static int failures;
#define CHECK(c) do { if (!(c)) { printf("check failed (line %d): %s (error %lu)\n", __LINE__, #c, GetLastError()); failures++; } } while (0)
static HANDLE port;

static DWORD WINAPI producer(void *arg) {
    for (int i = 0; i < 100; i++) PostQueuedCompletionStatus(port, (DWORD)i, (ULONG_PTR)arg, NULL);
    return 0;
}

int main(void) {
    port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    CHECK(port != NULL);
    HANDLE t[4];
    for (int i = 0; i < 4; i++) t[i] = CreateThread(NULL, 0, producer, (void *)(ULONG_PTR)(i + 1), 0, NULL);
    long long keys = 0, bytes = 0; int packets = 0;
    DWORD n; ULONG_PTR key; OVERLAPPED *ov;
    while (packets < 400 && GetQueuedCompletionStatus(port, &n, &key, &ov, 5000)) { packets++; keys += (long long)key; bytes += n; }
    CHECK(packets == 400 && keys == 100 * (1 + 2 + 3 + 4) && bytes == 4 * 4950);
    WaitForMultipleObjects(4, t, TRUE, 5000);
    CHECK(!GetQueuedCompletionStatus(port, &n, &key, &ov, 20) && GetLastError() == WAIT_TIMEOUT && ov == NULL);

    // Overlapped I/O on a file associated with the port.
    char path[MAX_PATH];
    GetTempPathA(sizeof path, path);
    strcat(path, "vxiocp.bin");
    HANDLE f = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_FLAG_OVERLAPPED, NULL);
    CHECK(f != INVALID_HANDLE_VALUE);
    CHECK(CreateIoCompletionPort(f, port, 77, 0) == port);
    OVERLAPPED wov = { 0 };
    wov.Offset = 4;
    DWORD done = 0;
    BOOL ok = WriteFile(f, "completion", 10, NULL, &wov);
    CHECK(ok || GetLastError() == ERROR_IO_PENDING);
    CHECK(GetQueuedCompletionStatus(port, &n, &key, &ov, 2000) && ov == &wov && key == 77 && n == 10);
    char buf[16] = { 0 };
    OVERLAPPED rov = { 0 };
    rov.Offset = 4;
    rov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    ok = ReadFile(f, buf, 10, NULL, &rov);
    CHECK(ok || GetLastError() == ERROR_IO_PENDING);
    CHECK(WaitForSingleObject(rov.hEvent, 2000) == WAIT_OBJECT_0);
    CHECK(GetOverlappedResult(f, &rov, &done, TRUE) && done == 10 && !memcmp(buf, "completion", 10));
    CHECK(GetQueuedCompletionStatus(port, &n, &key, &ov, 2000) && ov == &rov && n == 10);
    // Reading past the end: the operation fails with ERROR_HANDLE_EOF and its packet says so.
    OVERLAPPED eov = { 0 };
    eov.Offset = 1000;
    CHECK(!ReadFile(f, buf, 4, NULL, &eov) && GetLastError() == ERROR_HANDLE_EOF);
    CloseHandle(rov.hEvent);
    CloseHandle(f);
    DeleteFileA(path);

    OVERLAPPED_ENTRY entries[8]; ULONG got = 0;
    for (int i = 0; i < 3; i++) PostQueuedCompletionStatus(port, 1, 5, NULL);
    CHECK(GetQueuedCompletionStatusEx(port, entries, 8, &got, 1000, FALSE) && got >= 1);
    CloseHandle(port);
    printf("iocp: %d packets from 4 threads, overlapped file I/O through the port: %d failed checks -> %s\n", packets, failures, failures ? "FAILED" : "ok");
    return failures;
}
