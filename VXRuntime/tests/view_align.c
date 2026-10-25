// Views of file mappings start on the allocation granularity (64 KB), as on Windows (src/files.c): many small views
// in a row are each 64 KB aligned, so a program that rounds a view's address down to the granularity (Chromium's
// shared memory does, for UnmapViewOfFile) names that view and no other; an unaligned address for
// MapViewOfFileEx is refused with ERROR_MAPPED_ALIGNMENT.
#include <windows.h>
#include <stdio.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

int main(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    CHECK(si.dwAllocationGranularity == 0x10000, "granularity %lu", si.dwAllocationGranularity);
    enum { N = 8 };
    HANDLE m[N];
    unsigned char *v[N];
    for (int i = 0; i < N; i++) {
        m[i] = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 0x1000 + 0x800 * i, NULL);
        v[i] = MapViewOfFile(m[i], FILE_MAP_ALL_ACCESS, 0, 0, 0);
        CHECK(v[i] && !((ULONG_PTR)v[i] & 0xffff), "view %d at %p", i, (void *)v[i]);
        if (v[i]) v[i][0] = (unsigned char)(0x40 + i);
    }
    for (int i = 0; i < N; i++)                                        // no two views share a 64 KB block
        for (int k = i + 1; k < N; k++)
            CHECK(((ULONG_PTR)v[i] >> 16) != ((ULONG_PTR)v[k] >> 16), "views %d and %d in one block", i, k);
    // Unmapping one view by its address rounded down (the same address here) leaves the others alone.
    CHECK(UnmapViewOfFile((void *)((ULONG_PTR)(v[3] + 0x10) & ~(ULONG_PTR)0xffff)), "unmap view 3");
    for (int i = 0; i < N; i++) if (i != 3 && v[i]) CHECK(v[i][0] == 0x40 + i, "view %d after unmapping view 3", i);
    // Not a view's base: refused, nothing unmapped.
    SetLastError(0);
    CHECK(!UnmapViewOfFile(v[4] + 0x1000) && GetLastError() == ERROR_INVALID_ADDRESS, "unmap inside a view: %lu", GetLastError());
    CHECK(v[4][0] == 0x44, "view 4 still mapped");
    // Two views of one mapping: the same bytes, each aligned.
    unsigned char *again = MapViewOfFile(m[5], FILE_MAP_READ, 0, 0, 0);
    CHECK(again && !((ULONG_PTR)again & 0xffff) && again[0] == 0x45, "second view of mapping 5");
    if (again) UnmapViewOfFile(again);
    // MapViewOfFileEx at an address off the granularity.
    SetLastError(0);
    void *odd = MapViewOfFileEx(m[0], FILE_MAP_READ, 0, 0, 0, (void *)0x7e001000);
    CHECK(!odd && GetLastError() == ERROR_MAPPED_ALIGNMENT, "unaligned address: %p, error %lu", odd, GetLastError());
    for (int i = 0; i < N; i++) { if (i != 3 && v[i]) UnmapViewOfFile(v[i]); CloseHandle(m[i]); }
    printf(fails ? "view_align: %d failures\n" : "view_align: ok\n", fails);
    return fails != 0;
}
