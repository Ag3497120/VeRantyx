// Milestone 9a: kernel objects between processes. "ipc.exe first" and "ipc.exe second" run at the same time
// (make check starts both): they share a named mapping, a named mutex and two named auto-reset events. Each adds
// to a counter in the mapping 20000 times under the mutex; then they bounce the events 2000 times (the round trip
// time is printed).
#include <windows.h>
#include <stdio.h>
#include <string.h>

typedef struct { volatile LONG counter; volatile LONG ready; } shared;

int main(int argc, char **argv) {
    int first = argc > 1 && !strcmp(argv[1], "first");
    if (argc < 2) { printf("usage: ipc first|second\n"); return 2; }
    HANDLE map = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 4096, "Local\\vxrt_ipc_test_map");
    HANDLE mutex = CreateMutexA(NULL, FALSE, "vxrt_ipc_test_mutex");
    HANDLE ping = CreateEventA(NULL, FALSE, FALSE, "vxrt_ipc_test_ping");
    HANDLE pong = CreateEventA(NULL, FALSE, FALSE, "vxrt_ipc_test_pong");
    if (!map || !mutex || !ping || !pong) { printf("ipc %s: objects not created -> FAILED\n", argv[1]); return 1; }
    shared *s = MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof *s);
    // Meet: each says it is there and waits for the other.
    InterlockedIncrement(&s->ready);
    for (int i = 0; i < 5000 && s->ready < 2; i++) Sleep(1);
    if (s->ready < 2) { printf("ipc %s: the other process did not come -> FAILED\n", argv[1]); return 1; }
    for (int i = 0; i < 20000; i++) {
        if (WaitForSingleObject(mutex, 5000) != WAIT_OBJECT_0) { printf("ipc: mutex wait failed -> FAILED\n"); return 1; }
        LONG v = s->counter;                       // not atomic on purpose: the mutex must protect it
        s->counter = v + 1;
        ReleaseMutex(mutex);
    }
    InterlockedIncrement(&s->ready);
    for (int i = 0; i < 10000 && s->ready < 4; i++) Sleep(1);
    LONG total = s->counter;
    // Ping-pong.
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    int bounces = 0;
    for (int i = 0; i < 2000; i++) {
        if (first) {
            SetEvent(ping);
            if (WaitForSingleObject(pong, 5000) != WAIT_OBJECT_0) break;
        } else {
            if (WaitForSingleObject(ping, 5000) != WAIT_OBJECT_0) break;
            SetEvent(pong);
        }
        bounces++;
    }
    QueryPerformanceCounter(&t1);
    int ok = total == 40000 && bounces == 2000;
    if (first) printf("ipc between processes: counter %ld under a named mutex, %d event round trips, %.1f us each -> %s\n",
                      total, bounces, (double)(t1.QuadPart - t0.QuadPart) * 1e6 / f.QuadPart / (bounces ? bounces : 1), ok ? "ok" : "FAILED");
    UnmapViewOfFile(s);
    CloseHandle(map); CloseHandle(mutex); CloseHandle(ping); CloseHandle(pong);
    return !ok;
}
