// What a file mapping handle may do (src/sync.c "what a handle may do", src/files.c): DuplicateHandle gives the access
// asked for (or the source's with DUPLICATE_SAME_ACCESS) and never more than the source has; MapViewOfFile needs the
// handle's map rights; OpenFileMapping gives the access asked for. Chromium's read-only shared memory: a handle
// duplicated with FILE_MAP_READ | SECTION_QUERY must not become writable again (Steam's web helper CHECKs it).
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_fail = 1; } } while (0)

static HANDLE dup(HANDLE h, DWORD access, DWORD options) {
    HANDLE out = NULL;
    SetLastError(0);
    if (!DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &out, access, FALSE, options)) return NULL;
    return out;
}

int main(void) {
    HANDLE all = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 65536, NULL);
    CHECK(all != NULL, "CreateFileMapping failed, error %lu", GetLastError());
    HANDLE rw = dup(all, FILE_MAP_READ | FILE_MAP_WRITE | SECTION_QUERY, 0);
    CHECK(rw != NULL, "read-write duplicate failed, error %lu", GetLastError());
    CloseHandle(all);
    HANDLE ro = dup(rw, FILE_MAP_READ | SECTION_QUERY, 0);
    CHECK(ro != NULL, "read-only duplicate failed, error %lu", GetLastError());

    // No more access than the source: write from a read-only handle fails, also from its same-access copy.
    HANDLE w = dup(ro, FILE_MAP_WRITE, 0);
    CHECK(w == NULL && GetLastError() == ERROR_ACCESS_DENIED, "a read-only handle was duplicated writable (%p, error %lu)", w, GetLastError());
    HANDLE same = dup(ro, 0, DUPLICATE_SAME_ACCESS);
    CHECK(same != NULL, "same-access duplicate failed, error %lu", GetLastError());
    w = dup(same, FILE_MAP_WRITE, 0);
    CHECK(w == NULL && GetLastError() == ERROR_ACCESS_DENIED, "its same-access copy was duplicated writable (%p, error %lu)", w, GetLastError());
    w = dup(same, FILE_MAP_ALL_ACCESS, 0);
    CHECK(w == NULL && GetLastError() == ERROR_ACCESS_DENIED, "FILE_MAP_ALL_ACCESS from read-only (%p, error %lu)", w, GetLastError());
    // Less access is fine; MAXIMUM_ALLOWED gives the source's.
    HANDLE query = dup(rw, SECTION_QUERY, 0);
    CHECK(query != NULL, "query-only duplicate failed, error %lu", GetLastError());
    HANDLE most = dup(ro, MAXIMUM_ALLOWED, 0);
    CHECK(most != NULL, "MAXIMUM_ALLOWED duplicate failed, error %lu", GetLastError());

    // Views: write through rw, read through ro; no writable view of ro, no view at all of the query-only handle.
    char *pw = MapViewOfFile(rw, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
    CHECK(pw != NULL, "read-write view failed, error %lu", GetLastError());
    if (pw) strcpy(pw, "made-up shared bytes");
    char *pr = MapViewOfFile(ro, FILE_MAP_READ, 0, 0, 0);
    CHECK(pr != NULL && !strcmp(pr, "made-up shared bytes"), "read-only view: %p", (void *)pr);
    SetLastError(0);
    void *bad = MapViewOfFile(ro, FILE_MAP_WRITE, 0, 0, 0);
    CHECK(bad == NULL && GetLastError() == ERROR_ACCESS_DENIED, "a writable view of a read-only handle (%p, error %lu)", bad, GetLastError());
    SetLastError(0);
    bad = MapViewOfFile(query, FILE_MAP_READ, 0, 0, 0);
    CHECK(bad == NULL && GetLastError() == ERROR_ACCESS_DENIED, "a view of a query-only handle (%p, error %lu)", bad, GetLastError());
    char *pm = MapViewOfFile(most, FILE_MAP_READ, 0, 0, 0);
    CHECK(pm != NULL && !strcmp(pm, "made-up shared bytes"), "view through the MAXIMUM_ALLOWED copy: %p", (void *)pm);

    // DUPLICATE_CLOSE_SOURCE closes the source even when the duplicate is refused.
    HANDLE spare = dup(ro, 0, DUPLICATE_SAME_ACCESS);
    w = dup(spare, FILE_MAP_WRITE, DUPLICATE_CLOSE_SOURCE);
    CHECK(w == NULL, "refused duplicate");
    SetLastError(0);
    CHECK(!CloseHandle(spare) && GetLastError() == ERROR_INVALID_HANDLE, "the source survived DUPLICATE_CLOSE_SOURCE");

    // OpenFileMapping: the access asked for.
    HANDLE named = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 4096, L"Local\\vxrt-section-access-test");
    HANDLE opened = OpenFileMappingW(FILE_MAP_READ, FALSE, L"Local\\vxrt-section-access-test");
    CHECK(opened != NULL, "OpenFileMapping failed, error %lu", GetLastError());
    SetLastError(0);
    bad = opened ? MapViewOfFile(opened, FILE_MAP_WRITE, 0, 0, 0) : NULL;
    CHECK(bad == NULL && GetLastError() == ERROR_ACCESS_DENIED, "a writable view through a FILE_MAP_READ open (%p)", bad);
    HANDLE opened_rw = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, L"Local\\vxrt-section-access-test");
    void *ok = opened_rw ? MapViewOfFile(opened_rw, FILE_MAP_WRITE, 0, 0, 0) : NULL;
    CHECK(ok != NULL, "a writable view through a FILE_MAP_ALL_ACCESS open failed, error %lu", GetLastError());

    if (ok) UnmapViewOfFile(ok);
    if (pm) UnmapViewOfFile(pm);
    if (pr) UnmapViewOfFile(pr);
    if (pw) UnmapViewOfFile(pw);
    CloseHandle(opened_rw); CloseHandle(opened); CloseHandle(named);
    CloseHandle(most); CloseHandle(query); CloseHandle(same); CloseHandle(ro); CloseHandle(rw);
    printf(g_fail ? "section_access: FAILED\n" : "section_access: no more access than the source, views by access -> ok\n");
    return g_fail;
}
