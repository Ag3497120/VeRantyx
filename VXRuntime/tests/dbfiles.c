// The file calls a database engine makes on Windows, in the patterns SQLite's documentation describes (its file
// locking, lockingv3.html; the lock-byte page, fileformat2.html; the WAL index, walformat.html), each checked against
// what Microsoft documents: byte-range locks of two connections at the PENDING/RESERVED/SHARED bytes near 1 GiB,
// reads and writes at OVERLAPPED offsets (short at the end of the file, the file pointer after them), truncating and
// deleting a -journal, a database mapped while it is written (FlushViewOfFile of a whole view: Chromium's profile
// databases failed every commit on it), a -shm file mapped by two connections, deleting on close, the access a handle
// was opened with, and the small helpers (GetTempPathW, GetFullPathNameW, FormatMessageW, the clocks). Also
// RegDeleteKeyExW (steamwebhelper looks it up when it starts over with an empty profile). Made-up names and data only.
#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("check failed (line %d): %s (last error %lu)\n", __LINE__, #c, GetLastError()); failures++; } } while (0)

#define DIR_W L"C:\\vxdbtest"
#define DB_W DIR_W L"\\notes.db"
#define PENDING_BYTE 0x40000000u                       // lockingv3.html: the lock bytes, in the page at 1 GiB
#define RESERVED_BYTE (PENDING_BYTE + 1)
#define SHARED_FIRST (PENDING_BYTE + 2)
#define SHARED_SIZE 510

static HANDLE open_rw(const WCHAR *path, DWORD disposition) {
    return CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                       disposition, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, NULL);
}
static BOOL lock(HANDLE h, DWORD flags, DWORD offset, DWORD len) {
    OVERLAPPED ov = { 0 };
    ov.Offset = offset;
    return LockFileEx(h, flags, 0, len, 0, &ov);
}
static BOOL unlock(HANDLE h, DWORD offset, DWORD len) {
    OVERLAPPED ov = { 0 };
    ov.Offset = offset;
    return UnlockFileEx(h, 0, len, 0, &ov);
}
#define EXCL (LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY)
#define SHARED LOCKFILE_FAIL_IMMEDIATELY
static BOOL read_at(HANDLE h, void *buf, DWORD n, ULONGLONG at, DWORD *got) {
    OVERLAPPED ov = { 0 };
    ov.Offset = (DWORD)at; ov.OffsetHigh = (DWORD)(at >> 32);
    return ReadFile(h, buf, n, got, &ov);
}
static BOOL write_at(HANDLE h, const void *buf, DWORD n, ULONGLONG at) {
    OVERLAPPED ov = { 0 };
    DWORD put = 0;
    ov.Offset = (DWORD)at; ov.OffsetHigh = (DWORD)(at >> 32);
    return WriteFile(h, buf, n, &put, &ov) && put == n;
}
static LONGLONG pointer(HANDLE h) { LARGE_INTEGER zero = { 0 }, at; return SetFilePointerEx(h, zero, &at, FILE_CURRENT) ? at.QuadPart : -1; }
static LONGLONG size_of(HANDLE h) { LARGE_INTEGER s; return GetFileSizeEx(h, &s) ? s.QuadPart : -1; }
static BOOL exists(const WCHAR *path) { return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES; }

// ---- reads and writes at offsets, the -journal --------------------------------------------------------------------------
static void test_io(void) {
    char page[4096], back[4096];
    DWORD got;
    for (int i = 0; i < 4096; i++) page[i] = (char)('a' + i % 26);
    DeleteFileW(DB_W);
    HANDLE db = open_rw(DB_W, OPEN_ALWAYS);
    CHECK(db != INVALID_HANDLE_VALUE && GetLastError() == 0);           // made: no ERROR_ALREADY_EXISTS
    // An empty database: the 100-byte header read at offset 0 hits the end of the file.
    SetLastError(0);
    CHECK(!read_at(db, back, 100, 0, &got) && GetLastError() == ERROR_HANDLE_EOF && got == 0);
    CHECK(write_at(db, page, 4096, 0) && write_at(db, page, 4096, 4096));
    CHECK(size_of(db) == 8192);
    // A read that crosses the end of the file is short; one past it fails with ERROR_HANDLE_EOF.
    CHECK(read_at(db, back, 4096, 6144, &got) && got == 2048 && !memcmp(back, page + 2048, 2048));
    CHECK(!read_at(db, back, 4096, 8192, &got) && GetLastError() == ERROR_HANDLE_EOF && got == 0);
    CHECK(!read_at(db, back, 4096, 1ull << 30, &got) && GetLastError() == ERROR_HANDLE_EOF);
    // A synchronous handle's file pointer follows a read or write at an OVERLAPPED offset.
    LARGE_INTEGER zero = { 0 };
    CHECK(SetFilePointerEx(db, zero, NULL, FILE_BEGIN));
    CHECK(read_at(db, back, 50, 100, &got) && got == 50 && pointer(db) == 150);
    CHECK(write_at(db, "made-up", 7, 300) && pointer(db) == 307);
    // An offset of 0xFFFFFFFF:0xFFFFFFFF writes at the end of the file.
    OVERLAPPED append = { .Offset = 0xffffffff, .OffsetHigh = 0xffffffff };
    CHECK(WriteFile(db, "tail", 4, &got, &append) && got == 4 && size_of(db) == 8196 && pointer(db) == 8196);
    CHECK(read_at(db, back, 4, 8192, &got) && got == 4 && !memcmp(back, "tail", 4));
    LARGE_INTEGER cut = { .QuadPart = 8192 };
    CHECK(SetFilePointerEx(db, cut, NULL, FILE_BEGIN) && SetEndOfFile(db) && size_of(db) == 8192);
    // Without an OVERLAPPED, at the end of the file: TRUE and nothing read.
    LARGE_INTEGER end = { .QuadPart = 8192 };
    CHECK(SetFilePointerEx(db, end, NULL, FILE_BEGIN) && ReadFile(db, back, 10, &got, NULL) && got == 0);
    // A write past the end extends the file; the gap reads as zeros.
    CHECK(write_at(db, page, 4096, 16384) && size_of(db) == 20480);
    CHECK(read_at(db, back, 4096, 8192, &got) && got == 4096);
    int zeros = 1;
    for (int i = 0; i < 4096; i++) zeros &= back[i] == 0;
    CHECK(zeros);
    // Truncating with SetFilePointerEx + SetEndOfFile; the pointer may stand past the end.
    LARGE_INTEGER two = { .QuadPart = 8192 }, past = { .QuadPart = 1 << 20 };
    CHECK(SetFilePointerEx(db, two, NULL, FILE_BEGIN) && SetEndOfFile(db) && size_of(db) == 8192);
    CHECK(SetFilePointerEx(db, past, NULL, FILE_BEGIN) && size_of(db) == 8192 && pointer(db) == 1 << 20);
    CHECK(FlushFileBuffers(db));
    // OPEN_ALWAYS and CREATE_ALWAYS on a file that is there say so; CREATE_NEW refuses it.
    HANDLE again = open_rw(DB_W, OPEN_ALWAYS);
    CHECK(again != INVALID_HANDLE_VALUE && GetLastError() == ERROR_ALREADY_EXISTS);
    CloseHandle(again);
    CHECK(open_rw(DB_W, CREATE_NEW) == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_EXISTS);

    // The rollback journal: written, synced, truncated to nothing (journal_mode=TRUNCATE), its header zeroed
    // (PERSIST), deleted (DELETE). A journal that is not there is "file not found" to GetFileAttributesExW.
    const WCHAR *jn = DB_W L"-journal";
    WIN32_FILE_ATTRIBUTE_DATA a;
    DeleteFileW(jn);
    CHECK(!GetFileAttributesExW(jn, GetFileExInfoStandard, &a) && GetLastError() == ERROR_FILE_NOT_FOUND);
    CHECK(!GetFileAttributesExW(DIR_W L"\\nowhere\\notes.db-journal", GetFileExInfoStandard, &a) && GetLastError() == ERROR_PATH_NOT_FOUND);
    HANDLE j = open_rw(jn, OPEN_ALWAYS);
    CHECK(j != INVALID_HANDLE_VALUE);
    char header[512] = { (char)0xd9, (char)0xd5, 0x05, (char)0xf9 };
    CHECK(write_at(j, header, 512, 0) && write_at(j, page, 4096, 512) && FlushFileBuffers(j));
    CHECK(GetFileAttributesExW(jn, GetFileExInfoStandard, &a) && a.nFileSizeLow == 4608 && a.nFileSizeHigh == 0);
    CHECK(SetFilePointerEx(j, zero, NULL, FILE_BEGIN) && SetEndOfFile(j) && FlushFileBuffers(j) && size_of(j) == 0);
    CHECK(GetFileAttributesExW(jn, GetFileExInfoStandard, &a) && a.nFileSizeLow == 0);
    memset(header, 0, sizeof header);
    CHECK(write_at(j, header, 28, 0) && size_of(j) == 28);
    CloseHandle(j);
    CHECK(DeleteFileW(jn) && !exists(jn) && GetLastError() == ERROR_FILE_NOT_FOUND);
    CHECK(!DeleteFileW(jn) && GetLastError() == ERROR_FILE_NOT_FOUND);
    CloseHandle(db);
}

// ---- locks: two connections to one database ------------------------------------------------------------------------------
static HANDLE g_waiter;
static DWORD WINAPI wait_for_reserved(void *arg) {
    (void)arg;
    return lock(g_waiter, LOCKFILE_EXCLUSIVE_LOCK, RESERVED_BYTE, 1) ? 1 : 2;   // no FAIL_IMMEDIATELY: it waits
}
static void test_locks(void) {
    HANDLE c1 = open_rw(DB_W, OPEN_EXISTING), c2 = open_rw(DB_W, OPEN_EXISTING);
    CHECK(c1 != INVALID_HANDLE_VALUE && c2 != INVALID_HANDLE_VALUE);
    // SHARED: each connection takes PENDING for a moment, a shared lock on the SHARED range, lets PENDING go.
    CHECK(lock(c1, EXCL, PENDING_BYTE, 1) && lock(c1, SHARED, SHARED_FIRST, SHARED_SIZE) && unlock(c1, PENDING_BYTE, 1));
    CHECK(lock(c2, EXCL, PENDING_BYTE, 1) && lock(c2, SHARED, SHARED_FIRST, SHARED_SIZE) && unlock(c2, PENDING_BYTE, 1));
    // RESERVED: one connection at a time.
    CHECK(lock(c1, EXCL, RESERVED_BYTE, 1));
    CHECK(!lock(c2, EXCL, RESERVED_BYTE, 1) && GetLastError() == ERROR_LOCK_VIOLATION);
    // PENDING held by the writer: no new reader gets in.
    CHECK(lock(c1, EXCL, PENDING_BYTE, 1));
    CHECK(!lock(c2, EXCL, PENDING_BYTE, 1) && GetLastError() == ERROR_LOCK_VIOLATION);
    CHECK(!lock(c2, SHARED, PENDING_BYTE, 1) && GetLastError() == ERROR_LOCK_VIOLATION);
    // EXCLUSIVE: the writer gives up its shared lock and asks for the whole SHARED range, which waits for the reader.
    CHECK(unlock(c1, SHARED_FIRST, SHARED_SIZE));
    CHECK(!lock(c1, EXCL, SHARED_FIRST, SHARED_SIZE) && GetLastError() == ERROR_LOCK_VIOLATION);
    CHECK(unlock(c2, SHARED_FIRST, SHARED_SIZE));
    CHECK(lock(c1, EXCL, SHARED_FIRST, SHARED_SIZE));
    CHECK(!lock(c2, SHARED, SHARED_FIRST, SHARED_SIZE) && GetLastError() == ERROR_LOCK_VIOLATION);
    CHECK(!lock(c2, SHARED, SHARED_FIRST + 100, 1) && GetLastError() == ERROR_LOCK_VIOLATION);   // (any byte of it)
    // Back to nothing; an unlock must name a locked range exactly.
    CHECK(unlock(c1, SHARED_FIRST, SHARED_SIZE) && unlock(c1, PENDING_BYTE, 1) && unlock(c1, RESERVED_BYTE, 1));
    CHECK(!unlock(c1, RESERVED_BYTE, 1) && GetLastError() == ERROR_NOT_LOCKED);
    CHECK(lock(c1, SHARED, SHARED_FIRST, SHARED_SIZE));
    CHECK(!unlock(c1, SHARED_FIRST, 100) && GetLastError() == ERROR_NOT_LOCKED);
    CHECK(!unlock(c2, SHARED_FIRST, SHARED_SIZE) && GetLastError() == ERROR_NOT_LOCKED);    // (another handle's)
    CHECK(unlock(c1, SHARED_FIRST, SHARED_SIZE));
    // A shared lock may join the caller's own exclusive lock; it then takes two unlocks, the exclusive one first.
    CHECK(lock(c1, EXCL, RESERVED_BYTE, 1) && lock(c1, SHARED, RESERVED_BYTE, 1));
    CHECK(!lock(c1, EXCL, RESERVED_BYTE, 1) && GetLastError() == ERROR_LOCK_VIOLATION);    // (an exclusive lock may not)
    CHECK(unlock(c1, RESERVED_BYTE, 1));
    CHECK(lock(c2, SHARED, RESERVED_BYTE, 1));                                             // only the shared one is left
    CHECK(!lock(c2, EXCL, RESERVED_BYTE, 1) && GetLastError() == ERROR_LOCK_VIOLATION);
    CHECK(unlock(c1, RESERVED_BYTE, 1) && unlock(c2, RESERVED_BYTE, 1));
    CHECK(!unlock(c1, RESERVED_BYTE, 1) && GetLastError() == ERROR_NOT_LOCKED);
    // LockFile and UnlockFile: exclusive, at once.
    CHECK(LockFile(c1, RESERVED_BYTE, 0, 1, 0));
    CHECK(!LockFile(c2, RESERVED_BYTE, 0, 1, 0) && GetLastError() == ERROR_LOCK_VIOLATION);
    CHECK(UnlockFile(c1, RESERVED_BYTE, 0, 1, 0) && LockFile(c2, RESERVED_BYTE, 0, 1, 0) && UnlockFile(c2, RESERVED_BYTE, 0, 1, 0));
    CHECK(!UnlockFile(c2, RESERVED_BYTE, 0, 1, 0) && GetLastError() == ERROR_NOT_LOCKED);
    // A lock past the end of the file (the whole range of offsets), and one that waits until it can be had.
    CHECK(LockFile(c2, 0, 0, MAXDWORD, MAXDWORD));
    CHECK(!lock(c1, SHARED, 0, 1) && GetLastError() == ERROR_LOCK_VIOLATION);
    CHECK(UnlockFile(c2, 0, 0, MAXDWORD, MAXDWORD));
    CHECK(lock(c2, EXCL, RESERVED_BYTE, 1));
    g_waiter = c1;
    HANDLE t = CreateThread(NULL, 0, wait_for_reserved, NULL, 0, NULL);
    CHECK(t != NULL && WaitForSingleObject(t, 300) == WAIT_TIMEOUT);                     // waits while c2 holds it
    CHECK(unlock(c2, RESERVED_BYTE, 1));
    DWORD code = 0;
    CHECK(WaitForSingleObject(t, 5000) == WAIT_OBJECT_0 && GetExitCodeThread(t, &code) && code == 1);
    CloseHandle(t);
    // Closing a handle lets its locks go.
    CHECK(!lock(c2, EXCL, RESERVED_BYTE, 1) && GetLastError() == ERROR_LOCK_VIOLATION);
    CloseHandle(c1);
    CHECK(lock(c2, EXCL, RESERVED_BYTE, 1) && unlock(c2, RESERVED_BYTE, 1));
    // Not a file.
    CHECK(!lock(INVALID_HANDLE_VALUE, EXCL, RESERVED_BYTE, 1) && GetLastError() == ERROR_INVALID_HANDLE);
    CloseHandle(c2);
}

// ---- the database mapped (PRAGMA mmap_size): views, writes beside them, flushing a whole view -----------------------------
static void test_mapped(void) {
    HANDLE db = open_rw(DB_W, OPEN_EXISTING);
    char page[4096];
    DWORD got;
    CHECK(db != INVALID_HANDLE_VALUE && size_of(db) == 8192);
    HANDLE m = CreateFileMappingW(db, NULL, PAGE_READONLY, 0, 0, NULL);
    CHECK(m != NULL);
    const char *view = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
    CHECK(view != NULL);
    if (!view) return;
    // A page written with WriteFile shows in the view at once.
    memset(page, 'Q', sizeof page);
    memcpy(page, "page two, made up", 17);
    CHECK(write_at(db, page, 4096, 4096) && !memcmp(view + 4096, "page two, made up", 17) && view[8191] == 'Q');
    // The commit's sync: 0 bytes is "to the end of the view", from its start or from inside it; a range of it.
    CHECK(FlushViewOfFile(view, 0));
    CHECK(FlushViewOfFile(view + 100, 0));
    CHECK(FlushViewOfFile(view + 4096, 4096));
    CHECK(FlushFileBuffers(db));
    int local = 0;
    CHECK(!FlushViewOfFile(&local, 0) && GetLastError() == ERROR_INVALID_ADDRESS);         // not a view
    // The database grows: unmapped, extended, mapped again larger.
    CHECK(UnmapViewOfFile(view) && CloseHandle(m));
    CHECK(!FlushViewOfFile(view, 0));
    LARGE_INTEGER four = { .QuadPart = 16384 };
    CHECK(SetFilePointerEx(db, four, NULL, FILE_BEGIN) && SetEndOfFile(db) && size_of(db) == 16384);
    m = CreateFileMappingW(db, NULL, PAGE_READONLY, 0, 0, NULL);
    view = m ? MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0) : NULL;
    CHECK(view != NULL && !memcmp(view + 4096, "page two, made up", 17) && view[16383] == 0);
    CHECK(view && FlushViewOfFile(view, 0));
    if (view) UnmapViewOfFile(view);
    CloseHandle(m);
    // A writable view: written through, flushed, read back with ReadFile.
    m = CreateFileMappingW(db, NULL, PAGE_READWRITE, 0, 0, NULL);
    char *w = m ? MapViewOfFile(m, FILE_MAP_WRITE, 0, 0, 0) : NULL;
    CHECK(w != NULL);
    if (w) {
        memcpy(w + 12288, "through the view", 16);
        CHECK(FlushViewOfFile(w + 12288, 16) && FlushViewOfFile(w, 0));
        CHECK(read_at(db, page, 16, 12288, &got) && got == 16 && !memcmp(page, "through the view", 16));
        UnmapViewOfFile(w);
    }
    CloseHandle(m);
    // A larger size than the file, with write access, extends the file; an empty file cannot be mapped.
    m = CreateFileMappingW(db, NULL, PAGE_READWRITE, 0, 32768, NULL);
    CHECK(m != NULL && size_of(db) == 32768);
    CloseHandle(m);
    HANDLE empty = open_rw(DIR_W L"\\empty.db", CREATE_ALWAYS);
    CHECK(empty != INVALID_HANDLE_VALUE);
    CHECK(CreateFileMappingW(empty, NULL, PAGE_READONLY, 0, 0, NULL) == NULL && GetLastError() == ERROR_FILE_INVALID);
    CloseHandle(empty);
    DeleteFileW(DIR_W L"\\empty.db");
    CloseHandle(db);
}

// ---- WAL: the -shm file, mapped by two connections, locked at its bytes 120 and on --------------------------------------
static void test_shm(void) {
    const WCHAR *shm = DB_W L"-shm";
    HANDLE c1 = open_rw(shm, OPEN_ALWAYS), c2 = open_rw(shm, OPEN_ALWAYS);
    CHECK(c1 != INVALID_HANDLE_VALUE && c2 != INVALID_HANDLE_VALUE);
    LARGE_INTEGER region = { .QuadPart = 32768 };
    CHECK(SetFilePointerEx(c1, region, NULL, FILE_BEGIN) && SetEndOfFile(c1) && size_of(c2) == 32768);
    HANDLE m1 = CreateFileMappingW(c1, NULL, PAGE_READWRITE, 0, 32768, NULL), m2 = CreateFileMappingW(c2, NULL, PAGE_READWRITE, 0, 32768, NULL);
    volatile LONG *v1 = m1 ? MapViewOfFile(m1, FILE_MAP_WRITE | FILE_MAP_READ, 0, 0, 32768) : NULL;
    volatile LONG *v2 = m2 ? MapViewOfFile(m2, FILE_MAP_WRITE | FILE_MAP_READ, 0, 0, 32768) : NULL;
    CHECK(v1 != NULL && v2 != NULL && v1 != v2);
    if (v1 && v2) {
        v1[10] = 0x5eed1234;
        CHECK(v2[10] == 0x5eed1234);                                // one memory: the file's
        InterlockedIncrement(&v2[11]);
        CHECK(v1[11] == 1);
        UnmapViewOfFile((void *)v1); UnmapViewOfFile((void *)v2);
    }
    CloseHandle(m1); CloseHandle(m2);
    // The write lock (byte 120) is one connection's; read-mark locks may be shared.
    CHECK(lock(c1, EXCL, 120, 1) && !lock(c2, EXCL, 120, 1) && GetLastError() == ERROR_LOCK_VIOLATION);
    CHECK(lock(c1, SHARED, 123, 1) && lock(c2, SHARED, 123, 1));
    CHECK(!lock(c2, EXCL, 123, 1) && GetLastError() == ERROR_LOCK_VIOLATION);
    CHECK(unlock(c1, 120, 1) && lock(c2, EXCL, 120, 1));
    CloseHandle(c1); CloseHandle(c2);
    CHECK(DeleteFileW(shm));
}

// ---- deleting: open files, on close, by disposition ----------------------------------------------------------------------
static void test_delete(void) {
    char buf[16];
    DWORD got;
    // DeleteFileW of a file open with FILE_SHARE_DELETE succeeds; the open handle still reads it.
    const WCHAR *gone = DIR_W L"\\gone.db";
    HANDLE h = open_rw(gone, CREATE_ALWAYS);
    CHECK(h != INVALID_HANDLE_VALUE && write_at(h, "still here", 10, 0));
    CHECK(DeleteFileW(gone));
    CHECK(read_at(h, buf, 10, 0, &got) && got == 10 && !memcmp(buf, "still here", 10));
    CHECK(open_rw(gone, OPEN_EXISTING) == INVALID_HANDLE_VALUE);
    CloseHandle(h);
    CHECK(!exists(gone));
    // FILE_FLAG_DELETE_ON_CLOSE (a temporary file): there while open, gone once closed.
    WCHAR tmp[MAX_PATH], path[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, tmp);
    CHECK(n > 0 && n < MAX_PATH && tmp[n - 1] == L'\\' && (GetFileAttributesW(tmp) & FILE_ATTRIBUTE_DIRECTORY));
    swprintf(path, MAX_PATH, L"%lsvxdb_made_up_%lu.tmp", tmp, GetCurrentProcessId());
    h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    CHECK(h != INVALID_HANDLE_VALUE && write_at(h, "scratch", 7, 0) && exists(path));
    CloseHandle(h);
    CHECK(!exists(path) && GetLastError() == ERROR_FILE_NOT_FOUND);
    // FileDispositionInfo (on a handle opened with DELETE access): deleted on close, unless taken back first.
    const WCHAR *d = DIR_W L"\\disposed.db";
    FILE_DISPOSITION_INFO yes = { TRUE }, no = { FALSE };
    const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    h = CreateFileW(d, GENERIC_READ | GENERIC_WRITE | DELETE, share, NULL, CREATE_ALWAYS, 0, NULL);
    CHECK(SetFileInformationByHandle(h, FileDispositionInfo, &yes, sizeof yes) && SetFileInformationByHandle(h, FileDispositionInfo, &no, sizeof no));
    CloseHandle(h);
    CHECK(exists(d));
    h = CreateFileW(d, GENERIC_READ | DELETE, share, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(SetFileInformationByHandle(h, FileDispositionInfo, &yes, sizeof yes) && exists(d));
    CloseHandle(h);
    CHECK(!exists(d));
    // FileEndOfFileInfo sets the size; FileAllocationInfo below the size cuts the file there.
    h = open_rw(d, CREATE_ALWAYS);
    FILE_END_OF_FILE_INFO eof = { .EndOfFile.QuadPart = 5000 };
    FILE_ALLOCATION_INFO alloc = { .AllocationSize.QuadPart = 1000 };
    CHECK(SetFileInformationByHandle(h, FileEndOfFileInfo, &eof, sizeof eof) && size_of(h) == 5000);
    alloc.AllocationSize.QuadPart = 1 << 20;
    CHECK(SetFileInformationByHandle(h, FileAllocationInfo, &alloc, sizeof alloc) && size_of(h) == 5000);
    alloc.AllocationSize.QuadPart = 1000;
    CHECK(SetFileInformationByHandle(h, FileAllocationInfo, &alloc, sizeof alloc) && size_of(h) == 1000);
    CloseHandle(h);
    h = CreateFileW(d, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(!SetFileInformationByHandle(h, FileEndOfFileInfo, &eof, sizeof eof) && GetLastError() == ERROR_ACCESS_DENIED);
    CloseHandle(h);
    CHECK(DeleteFileW(d));
}

// ---- the access a handle was opened with ----------------------------------------------------------------------------------
static void test_access(void) {
    DWORD got;
    char buf[8];
    HANDLE r = CreateFileW(DB_W, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(r != INVALID_HANDLE_VALUE);
    CHECK(!WriteFile(r, "x", 1, &got, NULL) && GetLastError() == ERROR_ACCESS_DENIED);
    CHECK(!write_at(r, "x", 1, 0) && GetLastError() == ERROR_ACCESS_DENIED);
    CHECK(!SetEndOfFile(r) && GetLastError() == ERROR_ACCESS_DENIED);
    CHECK(!FlushFileBuffers(r) && GetLastError() == ERROR_ACCESS_DENIED);
    CloseHandle(r);
    HANDLE w = CreateFileW(DB_W, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(w != INVALID_HANDLE_VALUE);
    CHECK(!ReadFile(w, buf, 4, &got, NULL) && GetLastError() == ERROR_ACCESS_DENIED);
    CHECK(FlushFileBuffers(w));
    CloseHandle(w);
    // A size whose low part is INVALID_FILE_SIZE: GetFileSize returns it with NO_ERROR (a sparse 4 GiB - 1 file).
    HANDLE big = open_rw(DIR_W L"\\big.db", CREATE_ALWAYS);
    LARGE_INTEGER at = { .QuadPart = 0xffffffffll };
    DWORD high = 7;
    CHECK(SetFilePointerEx(big, at, NULL, FILE_BEGIN) && SetEndOfFile(big));
    SetLastError(ERROR_ACCESS_DENIED);
    CHECK(GetFileSize(big, &high) == INVALID_FILE_SIZE && high == 0 && GetLastError() == NO_ERROR);
    CloseHandle(big);
    CHECK(DeleteFileW(DIR_W L"\\big.db"));
}

// ---- helpers: full paths, messages, clocks, a registry key --------------------------------------------------------------
static void test_helpers(void) {
    WCHAR full[MAX_PATH], *part = NULL;
    DWORD need = GetFullPathNameW(DB_W, 0, NULL, NULL);
    CHECK(need == wcslen(DB_W) + 1);                                // with its terminating null
    CHECK(GetFullPathNameW(DB_W, MAX_PATH, full, &part) == need - 1 && part && !wcscmp(part, L"notes.db"));
    CHECK(GetFullPathNameW(L"C:\\vxdbtest\\sub\\..\\notes.db-wal", MAX_PATH, full, NULL) && !wcscmp(full, DB_W L"-wal"));
    DWORD codes[] = { ERROR_LOCK_VIOLATION, ERROR_HANDLE_EOF, ERROR_NOT_LOCKED, ERROR_ACCESS_DENIED };
    for (int i = 0; i < 4; i++) {
        WCHAR *msg = NULL;
        DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
                                 codes[i], 0, (WCHAR *)&msg, 0, NULL);
        CHECK(n > 0 && msg && wcslen(msg) == n);
        LocalFree(msg);
    }
    SYSTEMTIME st;
    FILETIME a, b;
    GetSystemTime(&st);
    GetSystemTimeAsFileTime(&b);
    CHECK(SystemTimeToFileTime(&st, &a));
    ULONGLONG ta = (ULONGLONG)a.dwHighDateTime << 32 | a.dwLowDateTime, tb = (ULONGLONG)b.dwHighDateTime << 32 | b.dwLowDateTime;
    LONGLONG apart = (LONGLONG)(tb - ta);                           // 100 ns units: within 2 s of each other
    CHECK(apart > -20000000ll && apart < 20000000ll && st.wYear >= 2024);
    DWORD t0 = GetTickCount();
    Sleep(20);
    CHECK(GetTickCount() - t0 >= 10);
    // RegDeleteKeyExW, as steamwebhelper finds it: by GetProcAddress.
    LONG (WINAPI *del)(HKEY, const WCHAR *, REGSAM, DWORD) =
        (LONG (WINAPI *)(HKEY, const WCHAR *, REGSAM, DWORD))GetProcAddress(LoadLibraryA("advapi32.dll"), "RegDeleteKeyExW");
    CHECK(del != NULL);
    HKEY k;
    CHECK(RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\VXDbTest\\Gone", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &k, NULL) == ERROR_SUCCESS);
    RegCloseKey(k);
    if (del) {
        CHECK(del(HKEY_CURRENT_USER, L"Software\\VXDbTest", KEY_WOW64_64KEY, 0) != ERROR_SUCCESS);   // (it has a subkey)
        CHECK(del(HKEY_CURRENT_USER, L"Software\\VXDbTest\\Gone", KEY_WOW64_64KEY, 0) == ERROR_SUCCESS);
        CHECK(RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\VXDbTest\\Gone", 0, KEY_READ, &k) == ERROR_FILE_NOT_FOUND);
        CHECK(del(HKEY_CURRENT_USER, L"Software\\VXDbTest", 0, 0) == ERROR_SUCCESS);
    }
}

// ---- "dbfiles.exe overlay <tree>": run with VXRT_OVERLAY=<tree>:<overlay>, the tree holding a.db .. e.db ("orig") and
// the folder keep. An overlaid tree stays as it is (writes go to the overlay; DeleteFileW leaves an original): deleting
// on close, by disposition or with POSIX semantics removes the overlay's copy of a file, never the original. The
// Makefile looks at the originals afterwards, and runs this once more with the tree named through a symbolic link
// (the name a handle reports for its file is the resolved one).
static BOOL holds(const char *path, const char *text) {
    char buf[32];
    DWORD got = 0;
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    BOOL ok = h != INVALID_HANDLE_VALUE && ReadFile(h, buf, sizeof buf, &got, NULL) && got >= strlen(text) && !memcmp(buf, text, strlen(text));
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    return ok;
}
static void change(const char *path) {
    DWORD put = 0;
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(h != INVALID_HANDLE_VALUE && WriteFile(h, "changed", 7, &put, NULL) && put == 7);
    CloseHandle(h);
    CHECK(holds(path, "changed"));
}
static int overlay_test(const char *tree) {
    char p[MAX_PATH];
    const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    FILE_DISPOSITION_INFO yes = { TRUE };
    FILE_DISPOSITION_INFO_EX posix = { FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS };
    // An original opened to read with FILE_FLAG_DELETE_ON_CLOSE: closing it leaves it.
    snprintf(p, sizeof p, "%s\\a.db", tree);
    HANDLE h = CreateFileA(p, GENERIC_READ, share, NULL, OPEN_EXISTING, FILE_FLAG_DELETE_ON_CLOSE, NULL);
    CHECK(h != INVALID_HANDLE_VALUE);
    CloseHandle(h);
    CHECK(holds(p, "orig"));
    // FileDispositionInfo on an original: accepted (as DeleteFileW is), the file stays.
    snprintf(p, sizeof p, "%s\\b.db", tree);
    h = CreateFileA(p, GENERIC_READ | DELETE, share, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(h != INVALID_HANDLE_VALUE && SetFileInformationByHandle(h, FileDispositionInfo, &yes, sizeof yes));
    CloseHandle(h);
    CHECK(holds(p, "orig"));
    // FileDispositionInfoEx with POSIX semantics (the name goes at once): the original stays.
    snprintf(p, sizeof p, "%s\\c.db", tree);
    h = CreateFileA(p, GENERIC_READ | DELETE, share, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(h != INVALID_HANDLE_VALUE && SetFileInformationByHandle(h, FileDispositionInfoEx, &posix, sizeof posix) && holds(p, "orig"));
    CloseHandle(h);
    CHECK(holds(p, "orig"));
    // A file changed (the overlay has a copy), then opened with FILE_FLAG_DELETE_ON_CLOSE: the copy goes, the original
    // is what the name shows again.
    snprintf(p, sizeof p, "%s\\d.db", tree);
    change(p);
    h = CreateFileA(p, GENERIC_READ, share, NULL, OPEN_EXISTING, FILE_FLAG_DELETE_ON_CLOSE, NULL);
    CHECK(h != INVALID_HANDLE_VALUE);
    CloseHandle(h);
    CHECK(holds(p, "orig"));
    // An original open, then changed through another handle, then deleted with POSIX semantics on the first: the copy
    // goes at once, the original stays.
    snprintf(p, sizeof p, "%s\\e.db", tree);
    h = CreateFileA(p, GENERIC_READ | DELETE, share, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(h != INVALID_HANDLE_VALUE);
    change(p);
    CHECK(SetFileInformationByHandle(h, FileDispositionInfoEx, &posix, sizeof posix) && holds(p, "orig"));
    CloseHandle(h);
    CHECK(holds(p, "orig"));
    // A new file under the tree (it is in the overlay only) goes on close as anywhere else.
    snprintf(p, sizeof p, "%s\\new.db", tree);
    h = CreateFileA(p, GENERIC_READ | GENERIC_WRITE, share, NULL, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, NULL);
    CHECK(h != INVALID_HANDLE_VALUE && GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES);
    CloseHandle(h);
    CHECK(GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES);
    // An original (empty) folder opened with FILE_FLAG_DELETE_ON_CLOSE stays.
    snprintf(p, sizeof p, "%s\\keep", tree);
    h = CreateFileA(p, GENERIC_READ | DELETE, share, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    CHECK(h != INVALID_HANDLE_VALUE);
    CloseHandle(h);
    DWORD attrs = GetFileAttributesA(p);
    CHECK(attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY));
    printf("deleting under an overlaid tree: %d failed checks -> %s\n", failures, failures ? "FAILED" : "ok");
    return failures != 0;
}

int main(int argc, char **argv) {
    if (argc > 2 && !strcmp(argv[1], "overlay")) return overlay_test(argv[2]);
    CreateDirectoryW(DIR_W, NULL);
    CHECK(GetFileAttributesW(DIR_W) & FILE_ATTRIBUTE_DIRECTORY);
    test_io();
    test_locks();
    test_mapped();
    test_shm();
    test_delete();
    test_access();
    test_helpers();
    DeleteFileW(DB_W);
    RemoveDirectoryW(DIR_W);
    printf("database file patterns: %d failed checks -> %s\n", failures, failures ? "FAILED" : "ok");
    return failures != 0;
}
