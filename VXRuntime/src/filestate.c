// What an open file has on Windows that its Mac descriptor does not keep: byte-range locks (LockFile, LockFileEx,
// UnlockFile, UnlockFileEx) and deletion when it is closed (FILE_FLAG_DELETE_ON_CLOSE, SetFileInformationByHandle's
// FileDispositionInfo), and the rest of SetFileInformationByHandle.
//
// Locks, as Microsoft documents LockFileEx: a lock belongs to the handle that took it. An exclusive lock may not
// overlap any other lock, the caller's own included; a shared lock may overlap shared locks and the caller's own
// exclusive lock (the same range locked both ways takes two unlocks, the exclusive lock goes first). An unlock names
// exactly a range that was locked (ERROR_NOT_LOCKED otherwise). Closing the handle releases its locks. Without
// LOCKFILE_FAIL_IMMEDIATELY the caller waits for the lock. Ranges past the end of the file are fine: SQLite (Chromium's
// profile databases) locks bytes at 1 GiB, in a page it never stores data in.
// Not here: locks taken in other processes (each process keeps its own table) and the mandatory part, reads and
// writes of a range that another handle locked do not fail.
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "vx.h"

#define ERROR_INVALID_HANDLE 6
#define ERROR_LOCK_VIOLATION 33
#define ERROR_INVALID_PARAMETER 87
#define ERROR_NOT_LOCKED 158
#define LOCKFILE_FAIL_IMMEDIATELY 0x1
#define LOCKFILE_EXCLUSIVE_LOCK 0x2

int vx_handle_fd(HANDLE h);                                          // kernel32.c
void vx_io_completed(HANDLE file, void *ov, DWORD bytes, DWORD status);   // sync.c
DWORD vx_error_from_errno(int e);                                     // files.c

// OVERLAPPED: Internal, InternalHigh, Offset, OffsetHigh, hEvent.
typedef struct { uint64_t internal, internal_high; DWORD offset, offset_high; HANDLE event; } OVERLAPPED;

typedef struct file_lock { struct file_lock *next; dev_t dev; ino_t ino; HANDLE owner; uint64_t start, end; int exclusive; } file_lock;
typedef struct doomed { struct doomed *next; HANDLE owner; dev_t dev; ino_t ino; int dir; char path[]; } doomed;   // deleted on close
static file_lock *g_locks;
static doomed *g_doomed;
static int g_entries;                    // locks and doomed files: closing a handle looks only when there are any
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_unlocked = PTHREAD_COND_INITIALIZER;

// The file a handle is open on (a regular file: Windows locks bytes of files only).
static int identify(HANDLE h, dev_t *dev, ino_t *ino) {
    int fd = vx_handle_fd(h);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode)) return 0;
    *dev = st.st_dev; *ino = st.st_ino;
    return 1;
}
// [start, end); a range that would run past 2^64 ends there. An empty range overlaps nothing.
static uint64_t range_end(uint64_t start, uint64_t len) { return start + len < start ? UINT64_MAX : start + len; }
static int conflicts(dev_t dev, ino_t ino, HANDLE owner, uint64_t start, uint64_t end, int exclusive) {
    for (file_lock *l = g_locks; l; l = l->next)
        if (l->dev == dev && l->ino == ino && l->start < end && start < l->end && (exclusive || (l->exclusive && l->owner != owner)))
            return 1;
    return 0;
}

static BOOL lock_range(HANDLE h, uint64_t start, uint64_t len, int exclusive, int wait) {
    dev_t dev; ino_t ino;
    if (!identify(h, &dev, &ino)) { vx_set_last_error(ERROR_INVALID_HANDLE); return 0; }
    uint64_t end = range_end(start, len);
    file_lock *l = malloc(sizeof *l);
    pthread_mutex_lock(&g_mutex);
    while (conflicts(dev, ino, h, start, end, exclusive)) {
        if (!wait) { pthread_mutex_unlock(&g_mutex); free(l); vx_set_last_error(ERROR_LOCK_VIOLATION); return 0; }
        pthread_cond_wait(&g_unlocked, &g_mutex);
    }
    *l = (file_lock){ g_locks, dev, ino, h, start, end, exclusive };
    g_locks = l;
    __atomic_add_fetch(&g_entries, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_mutex);
    return 1;
}

static BOOL unlock_range(HANDLE h, uint64_t start, uint64_t len) {
    dev_t dev; ino_t ino;
    if (!identify(h, &dev, &ino)) { vx_set_last_error(ERROR_INVALID_HANDLE); return 0; }
    uint64_t end = range_end(start, len);
    pthread_mutex_lock(&g_mutex);
    file_lock **found = NULL;
    for (file_lock **p = &g_locks; *p; p = &(*p)->next) {
        file_lock *l = *p;
        if (l->owner != h || l->dev != dev || l->ino != ino || l->start != start || l->end != end) continue;
        if (!found || l->exclusive) found = p;                     // (an exclusive lock on the range goes first)
        if (l->exclusive) break;
    }
    file_lock *l = found ? *found : NULL;
    if (l) { *found = l->next; __atomic_sub_fetch(&g_entries, 1, __ATOMIC_RELEASE); pthread_cond_broadcast(&g_unlocked); }
    pthread_mutex_unlock(&g_mutex);
    if (!l) { vx_set_last_error(ERROR_NOT_LOCKED); return 0; }
    free(l);
    return 1;
}

static WINAPI BOOL LockFileEx(HANDLE h, DWORD flags, DWORD reserved, DWORD len_low, DWORD len_high, OVERLAPPED *ov) {
    if (!ov || reserved) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }   // (the offset is in the OVERLAPPED)
    uint64_t start = (uint64_t)ov->offset_high << 32 | ov->offset, len = (uint64_t)len_high << 32 | len_low;
    if (!lock_range(h, start, len, (flags & LOCKFILE_EXCLUSIVE_LOCK) != 0, !(flags & LOCKFILE_FAIL_IMMEDIATELY))) return 0;
    vx_io_completed(h, ov, 0, 0);                                  // (its event, as for a read)
    return 1;
}
static WINAPI BOOL UnlockFileEx(HANDLE h, DWORD reserved, DWORD len_low, DWORD len_high, OVERLAPPED *ov) {
    if (!ov || reserved) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    return unlock_range(h, (uint64_t)ov->offset_high << 32 | ov->offset, (uint64_t)len_high << 32 | len_low);
}
// LockFile: an exclusive lock, refused at once when it cannot be had.
static WINAPI BOOL LockFile(HANDLE h, DWORD off_low, DWORD off_high, DWORD len_low, DWORD len_high) {
    return lock_range(h, (uint64_t)off_high << 32 | off_low, (uint64_t)len_high << 32 | len_low, 1, 0);
}
static WINAPI BOOL UnlockFile(HANDLE h, DWORD off_low, DWORD off_high, DWORD len_low, DWORD len_high) {
    return unlock_range(h, (uint64_t)off_high << 32 | off_low, (uint64_t)len_high << 32 | len_low);
}

// ---- deleting on close ----------------------------------------------------------------------------------------------------
// The file or (empty) directory, by its Mac path, goes when the handle is closed, if that path still names the same
// file then. (Windows waits for the last handle to the file; other handles here keep reading and writing it after the
// name is gone.) Under VXRT_OVERLAY's tree the original stays, as DeleteFileW leaves it (AGENTS.md: Valve's and the
// games' files are never changed): only the overlay's copy of the name goes, when there is one (vx_overlay_deletable,
// files.c). That is decided here, again when the handle is closed, and for POSIX semantics.
#define VX_PATH_CAP 4096                                             // (files.c's Mac paths)
void vx_file_delete_on_close(HANDLE h, const char *unix_path, int on) {
    char target[VX_PATH_CAP];
    struct stat st;
    int fd = vx_handle_fd(h);
    doomed *d = NULL, *gone = NULL;
    if (on && unix_path && fd >= 0 && vx_overlay_deletable(unix_path, target, sizeof target) &&
        !(strcmp(target, unix_path) ? stat(target, &st) : fstat(fd, &st))) {   // (the overlay's copy: that file's identity)
        size_t n = strlen(target) + 1;
        d = malloc(sizeof *d + n);
        d->owner = h; d->dev = st.st_dev; d->ino = st.st_ino; d->dir = S_ISDIR(st.st_mode);
        memcpy(d->path, target, n);
    }
    pthread_mutex_lock(&g_mutex);
    for (doomed **p = &g_doomed; *p; p = &(*p)->next)
        if ((*p)->owner == h) { gone = *p; *p = gone->next; __atomic_sub_fetch(&g_entries, 1, __ATOMIC_RELEASE); break; }
    if (d) { d->next = g_doomed; g_doomed = d; __atomic_add_fetch(&g_entries, 1, __ATOMIC_RELEASE); }
    pthread_mutex_unlock(&g_mutex);
    free(gone);
}

// Called before a file handle's descriptor is closed (CloseHandle, the C runtime's _close, DUPLICATE_CLOSE_SOURCE).
void vx_file_closing(HANDLE h) {
    if (!__atomic_load_n(&g_entries, __ATOMIC_ACQUIRE)) return;
    doomed *gone = NULL;
    int released = 0;
    pthread_mutex_lock(&g_mutex);
    for (file_lock **p = &g_locks; *p; ) {
        file_lock *l = *p;
        if (l->owner == h) { *p = l->next; free(l); __atomic_sub_fetch(&g_entries, 1, __ATOMIC_RELEASE); released = 1; } else p = &l->next;
    }
    for (doomed **p = &g_doomed; *p; ) {
        doomed *d = *p;
        if (d->owner == h) { *p = d->next; d->next = gone; gone = d; __atomic_sub_fetch(&g_entries, 1, __ATOMIC_RELEASE); } else p = &d->next;
    }
    if (released) pthread_cond_broadcast(&g_unlocked);
    pthread_mutex_unlock(&g_mutex);
    while (gone) {
        doomed *d = gone;
        gone = d->next;
        struct stat st;
        char target[VX_PATH_CAP];
        if (vx_overlay_deletable(d->path, target, sizeof target) && !strcmp(target, d->path) &&   // (never an original)
            !stat(d->path, &st) && st.st_dev == d->dev && st.st_ino == d->ino) { if (d->dir) rmdir(d->path); else unlink(d->path); }
        free(d);
    }
}

// ---- SetFileInformationByHandle ---------------------------------------------------------------------------------------------
enum { FileBasicInfo = 0, FileDispositionInfo = 4, FileAllocationInfo = 5, FileEndOfFileInfo = 6, FileDispositionInfoEx = 21 };
#define FILE_DISPOSITION_FLAG_DELETE 0x1
#define FILE_DISPOSITION_FLAG_POSIX_SEMANTICS 0x2

static BOOL set_size(int fd, uint64_t size) {
    if ((fcntl(fd, F_GETFL) & O_ACCMODE) == O_RDONLY) { vx_set_last_error(5 /* ERROR_ACCESS_DENIED */); return 0; }
    if (ftruncate(fd, (off_t)size)) { vx_set_last_error(vx_error_from_errno(errno)); return 0; }
    return 1;
}
static WINAPI BOOL SetFileInformationByHandle(HANDLE h, int cls, const void *in, DWORD size) {
    int fd = vx_handle_fd(h);
    if (fd < 0) { vx_set_last_error(ERROR_INVALID_HANDLE); return 0; }
    char path[PATH_MAX];
    switch (cls) {
    case FileBasicInfo:                                            // times and attributes: accepted, not kept
        return 1;
    case FileDispositionInfo:                                      // FILE_DISPOSITION_INFO { BOOLEAN DeleteFile; }
        if (!in || size < 1) break;
        vx_file_delete_on_close(h, fcntl(fd, F_GETPATH, path) ? NULL : path, *(const uint8_t *)in != 0);
        return 1;
    case FileDispositionInfoEx: {                                  // FILE_DISPOSITION_INFO_EX { ULONG Flags; }
        if (!in || size < 4) break;
        DWORD flags = *(const DWORD *)in;
        if (fcntl(fd, F_GETPATH, path)) { vx_set_last_error(ERROR_INVALID_HANDLE); return 0; }
        if ((flags & FILE_DISPOSITION_FLAG_DELETE) && (flags & FILE_DISPOSITION_FLAG_POSIX_SEMANTICS)) {
            struct stat st;                                        // the name goes now; the handle still works
            char target[VX_PATH_CAP];
            vx_file_delete_on_close(h, NULL, 0);
            if (!vx_overlay_deletable(path, target, sizeof target)) return 1;   // an overlaid original stays (DeleteFileW's answer)
            if ((strcmp(target, path) ? lstat(target, &st) : fstat(fd, &st)) || (S_ISDIR(st.st_mode) ? rmdir(target) : unlink(target))) {
                vx_set_last_error(vx_error_from_errno(errno));
                return 0;
            }
            return 1;
        }
        vx_file_delete_on_close(h, path, (flags & FILE_DISPOSITION_FLAG_DELETE) != 0);
        return 1;
    }
    case FileAllocationInfo: {                                     // FILE_ALLOCATION_INFO { LARGE_INTEGER AllocationSize; }
        if (!in || size < 8) break;
        struct stat st;                                            // less than the file's size cuts the file there
        uint64_t want = *(const uint64_t *)in;
        if (fstat(fd, &st)) { vx_set_last_error(ERROR_INVALID_HANDLE); return 0; }
        return want < (uint64_t)st.st_size ? set_size(fd, want) : 1;
    }
    case FileEndOfFileInfo:                                        // FILE_END_OF_FILE_INFO { LARGE_INTEGER EndOfFile; }
        if (!in || size < 8) break;
        return set_size(fd, *(const uint64_t *)in);
    default:
        vx_set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    vx_set_last_error(24 /* ERROR_BAD_LENGTH */);
    return 0;
}

static const vx_export filestate_k32[] = {
    VX_EXPORT(LockFile), VX_EXPORT(LockFileEx), VX_EXPORT(UnlockFile), VX_EXPORT(UnlockFileEx), VX_EXPORT(SetFileInformationByHandle),
};
void vx_filestate_init(void) {
    vx_register_module("kernel32.dll", filestate_k32, sizeof filestate_k32 / sizeof filestate_k32[0]);
    vx_register_module("kernelbase.dll", filestate_k32, sizeof filestate_k32 / sizeof filestate_k32[0]);
}

#ifdef __aarch64__
#include "thunks/filestate.c.inc"
#endif
