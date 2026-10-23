// x86's memory order between threads for accesses through RBP (a register compilers use for anything, not only the
// frame: Unity's allocator keeps its heap there). One thread writes data, then a flag; the other reads the flag, then
// the data. Under x86's order a reader never sees the flag newer than the data; with RBP-based accesses translated as
// plain loads and stores (the ARM64 build before 2026-10-24) it saw that about a million times in these 40 million.
#include <windows.h>
#include <stdio.h>
#include <string.h>

// writer(shared, n): for i = 1..n: [rbp] = i; [rbp+40h] = i      reader(shared, n): n times: flag = [rbp+40h];
// data = [rbp]; counts flag > data
static const unsigned char code[] = {
    0x55, 0x48, 0x89, 0xCD, 0xB8, 0x01, 0x00, 0x00, 0x00, 0x89, 0x45, 0x00, 0x89, 0x45, 0x40, 0xFF, 0xC0, 0x39, 0xD0,
    0x7E, 0xF4, 0x5D, 0xC3,
    0x55, 0x48, 0x89, 0xCD, 0x31, 0xC0, 0x41, 0x89, 0xD0, 0x8B, 0x4D, 0x40, 0x44, 0x8B, 0x4D, 0x00, 0x44, 0x39, 0xC9,
    0x7E, 0x02, 0xFF, 0xC0, 0x41, 0xFF, 0xC8, 0x75, 0xED, 0x5D, 0xC3,
};
typedef int (*fn)(volatile int *shared, int n);
static fn writer, reader;
static volatile int *g_shared;
enum { N = 4000000 };
static DWORD WINAPI write_thread(void *arg) { (void)arg; writer(g_shared, N); return 0; }

int main(void) {
    unsigned char *p = VirtualAlloc(NULL, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(p, code, sizeof code);
    writer = (fn)p; reader = (fn)(p + 23);
    g_shared = VirtualAlloc(NULL, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);   // (not on the code's page)
    long seen = 0;
    for (int round = 0; round < 10; round++) {
        g_shared[0] = 0; g_shared[16] = 0;
        HANDLE t = CreateThread(NULL, 0, write_thread, NULL, 0, NULL);
        seen += reader(g_shared, N);
        WaitForSingleObject(t, INFINITE);
        CloseHandle(t);
    }
    printf("order: flag newer than the data it follows seen %ld times in 10 x %d reads -> %s\n", seen, N, seen ? "FAILED" : "ok");
    return seen != 0;
}
