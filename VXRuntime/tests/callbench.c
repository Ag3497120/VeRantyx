// The cost of the runtime's small functions as a program calls them (numbers only, not a test): critical
// sections, TLS, last error, the performance counter, the heap, the C runtime's memcpy and strlen.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double now_us(void) { LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f); return (double)c.QuadPart * 1e6 / (double)f.QuadPart; }
static volatile size_t sink;
#define BENCH(name, n, body) do { double t0 = now_us(); for (int i_ = 0; i_ < (n); i_++) { body; } \
    printf("  %-28s %8.1f ns\n", name, (now_us() - t0) * 1000.0 / (n)); } while (0)

int main(void) {
    CRITICAL_SECTION cs; InitializeCriticalSection(&cs);
    DWORD slot = TlsAlloc(); TlsSetValue(slot, (void *)42);
    static char a[256], b[256];
    memset(a, 'x', 255);
    void *(*volatile mcpy)(void *, const void *, size_t) = memcpy;   // (calls, not inlined)
    size_t (*volatile slen)(const char *) = strlen;
    printf("callbench:\n");
    BENCH("Enter+LeaveCriticalSection", 200000, EnterCriticalSection(&cs); LeaveCriticalSection(&cs));
    BENCH("TlsGetValue", 200000, sink += (size_t)TlsGetValue(slot));
    BENCH("GetLastError", 200000, sink += GetLastError());
    BENCH("QueryPerformanceCounter", 200000, { LARGE_INTEGER c; QueryPerformanceCounter(&c); sink += (size_t)c.QuadPart; });
    BENCH("malloc+free (64 bytes)", 200000, { void *p = malloc(64); sink += (size_t)p; free(p); });
    BENCH("HeapAlloc+HeapFree", 200000, { void *p = HeapAlloc(GetProcessHeap(), 0, 64); sink += (size_t)p; HeapFree(GetProcessHeap(), 0, p); });
    BENCH("memcpy (256 bytes)", 200000, mcpy(b, a, 256));
    BENCH("strlen (255)", 200000, sink += slen(a));
    printf("callbench done (%zu)\n", sink & 1);
    return 0;
}
