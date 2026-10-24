// What a file mapping handle may do (src/sync.c "what a handle may do", src/files.c): DuplicateHandle gives the access
// asked for (or the source's with DUPLICATE_SAME_ACCESS) and never more than the source has; MapViewOfFile needs the
// handle's map rights (the write right maps read-only, copy-on-write and read/write views; FILE_MAP_ALL_ACCESS is a
// write view); OpenFileMapping gives the access asked for. Chromium's read-only shared memory: a handle
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

    // FILE_MAP_WRITE allows read-only, copy-on-write and read/write views, and FILE_MAP_ALL_ACCESS and
    // FILE_MAP_READ | FILE_MAP_WRITE are the same as FILE_MAP_WRITE (Microsoft's MapViewOfFile page): a handle opened
    // or duplicated with FILE_MAP_WRITE alone maps them all.
    HANDLE write_only = OpenFileMappingW(FILE_MAP_WRITE, FALSE, L"Local\\vxrt-section-access-test");
    CHECK(write_only != NULL, "OpenFileMapping(FILE_MAP_WRITE) failed, error %lu", GetLastError());
    HANDLE write_dup = named ? dup(named, FILE_MAP_WRITE, 0) : NULL;
    CHECK(write_dup != NULL, "DuplicateHandle(FILE_MAP_WRITE) failed, error %lu", GetLastError());
    static const DWORD write_views[] = { FILE_MAP_READ, FILE_MAP_COPY, FILE_MAP_ALL_ACCESS, FILE_MAP_READ | FILE_MAP_WRITE, FILE_MAP_WRITE };
    HANDLE write_handles[] = { write_only, write_dup };
    for (int i = 0; i < 2; i++) {
        for (unsigned j = 0; write_handles[i] && j < sizeof write_views / sizeof write_views[0]; j++) {
            SetLastError(0);
            char *v = MapViewOfFile(write_handles[i], write_views[j], 0, 0, 0);
            CHECK(v != NULL, "%s handle, view %#lx: error %lu", i ? "FILE_MAP_WRITE duplicated" : "FILE_MAP_WRITE opened",
                  write_views[j], GetLastError());
            if (v && j) v[0] = 'x';                     // (each view but the read-only one is writable or copy-on-write)
            if (v) UnmapViewOfFile(v);
        }
    }
    // A FILE_MAP_READ handle: read-only and copy-on-write views, not write or FILE_MAP_ALL_ACCESS ones.
    if (ok) strcpy(ok, "made-up named bytes");
    char *copy = opened ? MapViewOfFile(opened, FILE_MAP_COPY, 0, 0, 0) : NULL;
    CHECK(copy != NULL, "a copy-on-write view through a FILE_MAP_READ open failed, error %lu", GetLastError());
    if (copy) {                                                    // the section's bytes; its own writes stay its own
        CHECK(!strcmp(copy, "made-up named bytes"), "copy-on-write view reads %.20s", copy);
        copy[0] = 'M';
        CHECK(!ok || ((char *)ok)[0] == 'm', "a write to a copy-on-write view reached the section");
        UnmapViewOfFile(copy);
    }
    SetLastError(0);
    bad = opened ? MapViewOfFile(opened, FILE_MAP_ALL_ACCESS, 0, 0, 0) : NULL;
    CHECK(bad == NULL && GetLastError() == ERROR_ACCESS_DENIED, "a FILE_MAP_ALL_ACCESS view through a FILE_MAP_READ open (%p)", bad);
    // FILE_MAP_EXECUTE needs the execute right, which FILE_MAP_WRITE does not give.
    SetLastError(0);
    bad = write_only ? MapViewOfFile(write_only, FILE_MAP_READ | FILE_MAP_EXECUTE, 0, 0, 0) : NULL;
    CHECK(bad == NULL && GetLastError() == ERROR_ACCESS_DENIED, "an executable view through a FILE_MAP_WRITE open (%p)", bad);
    CloseHandle(write_dup); CloseHandle(write_only);

    if (ok) UnmapViewOfFile(ok);
    if (pm) UnmapViewOfFile(pm);
    if (pr) UnmapViewOfFile(pr);
    if (pw) UnmapViewOfFile(pw);
    CloseHandle(opened_rw); CloseHandle(opened); CloseHandle(named);
    CloseHandle(most); CloseHandle(query); CloseHandle(same); CloseHandle(ro); CloseHandle(rw);
    printf(g_fail ? "section_access: FAILED\n" : "section_access: no more access than the source, views by access -> ok\n");
    return g_fail;
}
