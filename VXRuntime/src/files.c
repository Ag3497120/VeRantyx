// Files, directories and paths.
//
// Windows paths are mapped onto the Mac's file system:
//   C:\...   ->  $VXRT_ROOT/drive_c/...   (VXRT_ROOT defaults to ~/.vxrt; created with C:\windows\system32 etc.)
//   Z:\...   ->  /...                     (the whole Mac file system, as in Wine)
// Relative paths resolve against the Windows current directory, which this runtime keeps itself (it starts as
// the Mac's working directory, seen through Z: or C:). Names are matched by the file system: APFS is
// case-insensitive by default, like NTFS. A file handle is 0x10000 + a file descriptor.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "vx.h"

#define MAX_PATH_LEN 4096
static char g_root[MAX_PATH_LEN];          // VXRT_ROOT
static char g_cwd[MAX_PATH_LEN];           // Windows current directory, e.g. "C:\games\x" (no trailing '\')
static pthread_mutex_t g_cwd_lock = PTHREAD_MUTEX_INITIALIZER;

const char *vx_root(void) { return g_root; }

// Windows error codes for errno values.
DWORD vx_error_from_errno(int e) {
    switch (e) {
    case ENOENT:    return 2;    // ERROR_FILE_NOT_FOUND
    case ENOTDIR:   return 3;    // ERROR_PATH_NOT_FOUND
    case EMFILE: case ENFILE: return 4;
    case EACCES: case EPERM: case EISDIR: case EROFS: return 5;   // ERROR_ACCESS_DENIED
    case EBADF:     return 6;
    case ENOMEM:    return 8;
    case EEXIST:    return 80;   // ERROR_FILE_EXISTS
    case ENOSPC:    return 112;  // ERROR_DISK_FULL
    case ENOTEMPTY: return 145;  // ERROR_DIR_NOT_EMPTY
    case ENAMETOOLONG: return 206;
    case EINVAL:    return 87;
    default:        return 31;   // ERROR_GEN_FAILURE
    }
}
static void set_error_from_errno(void) { vx_set_last_error(vx_error_from_errno(errno)); }

// ---- paths -------------------------------------------------------------------------------------------------------
// Turns any Windows path into a canonical absolute one: "X:\a\b" with '.' and '..' resolved, '/' accepted as a
// separator, no trailing separator except for a drive root ("C:\"). Returns 0 if it does not fit.
int vx_full_path(const char *in, char *out, size_t cap) {
    char tmp[MAX_PATH_LEN * 2];
    const char *p = in;
    if (!strncmp(p, "\\\\?\\", 4) || !strncmp(p, "\\\\.\\", 4)) p += 4;
    char cwd[MAX_PATH_LEN];
    pthread_mutex_lock(&g_cwd_lock); strcpy(cwd, g_cwd); pthread_mutex_unlock(&g_cwd_lock);
    if (((p[0] | 0x20) >= 'a' && (p[0] | 0x20) <= 'z') && p[1] == ':') {
        if (p[2] == '\\' || p[2] == '/') snprintf(tmp, sizeof tmp, "%s", p);
        else if ((p[0] | 0x20) == (cwd[0] | 0x20)) snprintf(tmp, sizeof tmp, "%s\\%s", cwd, p + 2);   // "C:rel"
        else snprintf(tmp, sizeof tmp, "%c:\\%s", p[0], p + 2);
    } else if (p[0] == '\\' || p[0] == '/') {
        snprintf(tmp, sizeof tmp, "%c:%s", cwd[0], p);
    } else {
        snprintf(tmp, sizeof tmp, "%s\\%s", cwd, p);
    }
    // Resolve components.
    size_t len = 0;
    if (cap < 4) return 0;
    out[len++] = (char)(tmp[0] >= 'a' && tmp[0] <= 'z' ? tmp[0] - 32 : tmp[0]);
    out[len++] = ':';
    const char *s = tmp + 2;
    while (*s) {
        while (*s == '\\' || *s == '/') s++;
        if (!*s) break;
        const char *e = s;
        while (*e && *e != '\\' && *e != '/') e++;
        size_t n = (size_t)(e - s);
        if (n == 1 && s[0] == '.') { s = e; continue; }
        if (n == 2 && s[0] == '.' && s[1] == '.') {
            while (len > 2 && out[len - 1] != '\\') len--;
            if (len > 2) len--;
            s = e;
            continue;
        }
        if (len + 1 + n + 1 > cap) return 0;
        out[len++] = '\\';
        memcpy(out + len, s, n);
        len += n;
        s = e;
    }
    if (len == 2) out[len++] = '\\';
    out[len] = 0;
    return 1;
}

// An overlay keeps a directory tree as it is: with VXRT_OVERLAY=<tree>:<overlay>, files written under <tree> go to
// the same place under <overlay> (and are read from there afterwards). Directory listings show both, the overlay's
// entries in place of the original's of the same name.
static char g_ov_tree[MAX_PATH_LEN], g_ov_shadow[MAX_PATH_LEN];
static size_t g_ov_len;
// (Paths compare without case, as on the Mac's file systems.)
static int overlay_shadow(const char *path, char *out, size_t cap) {
    if (!g_ov_len || strncasecmp(path, g_ov_tree, g_ov_len) || (path[g_ov_len] && path[g_ov_len] != '/')) return 0;
    int n = snprintf(out, cap, "%s%s", g_ov_shadow, path + g_ov_len);
    return n > 0 && (size_t)n < cap;
}
// A file under the overlaid tree that the overlay has a copy of: that copy (DLLs found by their Mac path).
int vx_overlay_file(const char *unix_path, char *out, size_t cap) {
    char shadow[MAX_PATH_LEN];
    struct stat st;
    if (!overlay_shadow(unix_path, shadow, sizeof shadow) || stat(shadow, &st) || !S_ISREG(st.st_mode)) return 0;
    snprintf(out, cap, "%s", shadow);
    return 1;
}
// Deleting by a Mac path keeps the overlaid tree too (DeleteFileW and RemoveDirectoryW below leave an original): under
// the tree only the overlay's copy of the name may go. The path is checked as given and as the file system resolves
// it, since a handle's own name for its file (F_GETPATH) is the resolved one (/private/tmp for /tmp, symbolic links
// followed; overlay_init resolves the tree's and the overlay's names too). The overlay's own files are not originals
// (it may sit inside the tree). Returns 1 with out set to what may be deleted (the path itself outside the tree, the
// overlay's copy under it), 0 when nothing may be.
static char g_ov_tree_real[MAX_PATH_LEN], g_ov_shadow_real[MAX_PATH_LEN];
static size_t path_under(const char *path, const char *dir) {   // the length of dir when path is dir or below it
    size_t n = strlen(dir);
    return n && !strncasecmp(path, dir, n) && (!path[n] || path[n] == '/') ? n : 0;
}
static size_t longer(size_t a, size_t b) { return a > b ? a : b; }
static int in_original_tree(const char *path, char *shadow, size_t cap) {
    size_t tree = longer(path_under(path, g_ov_tree), path_under(path, g_ov_tree_real));
    size_t ov = longer(path_under(path, g_ov_shadow), path_under(path, g_ov_shadow_real));
    if (!tree || ov > tree) return 0;                             // (in the overlay, inside the tree)
    int k = snprintf(shadow, cap, "%s%s", g_ov_shadow, path + tree);
    if (k < 0 || (size_t)k >= cap) *shadow = 0;                   // (no copy can have that name)
    return 1;
}
int vx_overlay_deletable(const char *unix_path, char *out, size_t cap) {
    char real[PATH_MAX], shadow[MAX_PATH_LEN];
    struct stat st;
    int k;
    if (g_ov_len && (in_original_tree(unix_path, shadow, sizeof shadow) || (realpath(unix_path, real) && in_original_tree(real, shadow, sizeof shadow)))) {
        if (!*shadow || lstat(shadow, &st)) return 0;             // the original alone: it stays
        k = snprintf(out, cap, "%s", shadow);
    } else {
        k = snprintf(out, cap, "%s", unix_path);
    }
    return k >= 0 && (size_t)k < cap;
}

static void make_parent_dirs(const char *path) {
    char p[MAX_PATH_LEN];
    snprintf(p, sizeof p, "%s", path);
    for (char *c = p + 1; *c; c++) if (*c == '/') { *c = 0; mkdir(p, 0777); *c = '/'; }
}
static void copy_file(const char *from, const char *to) {
    int in = open(from, O_RDONLY | O_CLOEXEC), out = in < 0 ? -1 : open(to, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    char buf[65536]; ssize_t n;
    if (out >= 0) while ((n = read(in, buf, sizeof buf)) > 0) if (write(out, buf, (size_t)n) != n) break;
    if (in >= 0) close(in);
    if (out >= 0) close(out);
}
static void overlay_init(void) {
    const char *ov = getenv("VXRT_OVERLAY");
    const char *colon = ov ? strchr(ov, ':') : NULL;
    if (!colon || colon == ov || !colon[1]) return;
    snprintf(g_ov_tree, sizeof g_ov_tree, "%.*s", (int)(colon - ov), ov);
    snprintf(g_ov_shadow, sizeof g_ov_shadow, "%s", colon + 1);
    while (strlen(g_ov_tree) > 1 && g_ov_tree[strlen(g_ov_tree) - 1] == '/') g_ov_tree[strlen(g_ov_tree) - 1] = 0;
    g_ov_len = strlen(g_ov_tree);
    mkdir(g_ov_shadow, 0777);
    char r[PATH_MAX];
    if (realpath(g_ov_tree, r)) snprintf(g_ov_tree_real, sizeof g_ov_tree_real, "%s", r);
    if (realpath(g_ov_shadow, r)) snprintf(g_ov_shadow_real, sizeof g_ov_shadow_real, "%s", r);
}

// Windows path -> Mac path. Returns 0 (with the last error set) for paths outside the mapped drives.
static int trace_opens_all(void) { static int on = -1; if (on < 0) { const char *e = getenv("VXRT_TRACE_FILES"); on = e && *e == '2'; } return on; }
int vx_unix_path(const char *win, char *out, size_t cap) {
    char full[MAX_PATH_LEN];
    if (!strcasecmp(win, "NUL")) { snprintf(out, cap, "/dev/null"); return 1; }
    if (!vx_full_path(win, full, sizeof full)) { vx_set_last_error(206 /* ERROR_FILENAME_EXCED_RANGE */); return 0; }
    const char *rest = full + 2;              // "\a\b" or "\"
    int n;
    if (full[0] == 'C') n = snprintf(out, cap, "%s/drive_c%s", g_root, rest[1] ? rest : "");
    else if (full[0] == 'Z') n = snprintf(out, cap, "%s", rest);
    else { vx_set_last_error(3 /* ERROR_PATH_NOT_FOUND */); return 0; }
    if (n < 0 || (size_t)n >= cap) { vx_set_last_error(206); return 0; }
    for (char *c = out; *c; c++) if (*c == '\\') *c = '/';
    char shadow[MAX_PATH_LEN];
    struct stat ss, os;
    if (overlay_shadow(out, shadow, sizeof shadow) && !stat(shadow, &ss) && (!S_ISDIR(ss.st_mode) || stat(out, &os)))
        snprintf(out, cap, "%s", shadow);             // written before: the overlay's copy (directories stay the original)
    return 1;
}

// For writing: under an overlaid tree, the path in the overlay, its directories made and (keep_contents) the
// original's contents copied there first.
int vx_unix_path_write(const char *win, char *out, size_t cap, int keep_contents) {
    if (!vx_unix_path(win, out, cap)) return 0;
    char shadow[MAX_PATH_LEN];
    if (!overlay_shadow(out, shadow, sizeof shadow)) return 1;
    make_parent_dirs(shadow);
    struct stat st;
    if (keep_contents && !stat(out, &st) && S_ISREG(st.st_mode)) copy_file(out, shadow);
    snprintf(out, cap, "%s", shadow);
    return 1;
}

// Mac path -> Windows path (C:\ inside the root's drive_c, Z:\ elsewhere).
void vx_windows_path(const char *unix_path, char *out, size_t cap) {
    char drive_c[MAX_PATH_LEN];
    snprintf(drive_c, sizeof drive_c, "%s/drive_c", g_root);
    size_t dl = strlen(drive_c);
    if (!strncmp(unix_path, drive_c, dl) && (unix_path[dl] == '/' || !unix_path[dl]))
        snprintf(out, cap, "C:%s", unix_path[dl] ? unix_path + dl : "\\");
    else
        snprintf(out, cap, "Z:%s", unix_path);
    for (char *c = out; *c; c++) if (*c == '/') *c = '\\';
}

// The W functions convert to UTF-8 and call the A versions.
#define WIDE_PATH(w, a) char *a = vx_utf8_dup(w)

// ---- current directory, special directories --------------------------------------------------------------------
int vx_set_cwd(const char *win) {
    char full[MAX_PATH_LEN], unix_path[MAX_PATH_LEN];
    if (!vx_full_path(win, full, sizeof full) || !vx_unix_path(full, unix_path, sizeof unix_path)) { if (vx_trace) vx_log("SetCurrentDirectory %s: bad path", win); return 0; }
    struct stat st;
    if (stat(unix_path, &st)) { if (vx_trace) vx_log("SetCurrentDirectory %s (%s): %s", win, unix_path, strerror(errno)); set_error_from_errno(); return 0; }
    if (!S_ISDIR(st.st_mode)) { vx_set_last_error(267 /* ERROR_DIRECTORY */); return 0; }
    size_t n = strlen(full);
    if (n > 3 && full[n - 1] == '\\') full[n - 1] = 0;
    pthread_mutex_lock(&g_cwd_lock); strcpy(g_cwd, full); pthread_mutex_unlock(&g_cwd_lock);
    return 1;
}
void vx_get_cwd(char *out, size_t cap) {
    pthread_mutex_lock(&g_cwd_lock); snprintf(out, cap, "%s", g_cwd); pthread_mutex_unlock(&g_cwd_lock);
}

static WINAPI DWORD GetCurrentDirectoryA(DWORD cap, char *buf) { char c[MAX_PATH_LEN]; vx_get_cwd(c, sizeof c); return vx_return_string_a(c, buf, cap); }
static WINAPI DWORD GetCurrentDirectoryW(DWORD cap, WCHAR *buf) { char c[MAX_PATH_LEN]; vx_get_cwd(c, sizeof c); return vx_return_string_w(c, buf, cap); }
static WINAPI BOOL SetCurrentDirectoryA(const char *p) { return vx_set_cwd(p); }
static WINAPI BOOL SetCurrentDirectoryW(const WCHAR *p) { WIDE_PATH(p, a); BOOL r = vx_set_cwd(a); free(a); return r; }

static WINAPI DWORD GetFullPathNameA(const char *name, DWORD cap, char *buf, char **file_part) {
    char full[MAX_PATH_LEN];
    if (!vx_full_path(name, full, sizeof full)) { vx_set_last_error(206); return 0; }
    DWORD r = vx_return_string_a(full, buf, cap);
    if (file_part && buf && r < cap) { char *b = strrchr(buf, '\\'); *file_part = b && b[1] ? b + 1 : NULL; }
    return r;
}
static WINAPI DWORD GetFullPathNameW(const WCHAR *name, DWORD cap, WCHAR *buf, WCHAR **file_part) {
    WIDE_PATH(name, a);
    char full[MAX_PATH_LEN];
    int ok = vx_full_path(a, full, sizeof full);
    free(a);
    if (!ok) { vx_set_last_error(206); return 0; }
    DWORD r = vx_return_string_w(full, buf, cap);
    if (file_part && buf && r < cap) {
        WCHAR *last = NULL;
        for (WCHAR *c = buf; *c; c++) if (*c == '\\') last = c;
        *file_part = last && last[1] ? last + 1 : NULL;
    }
    return r;
}

static WINAPI DWORD GetTempPathA(DWORD cap, char *buf) { return vx_return_string_a("C:\\windows\\temp\\", buf, cap); }
static WINAPI DWORD GetTempPathW(DWORD cap, WCHAR *buf) { return vx_return_string_w("C:\\windows\\temp\\", buf, cap); }
static WINAPI DWORD GetWindowsDirectoryA(char *buf, DWORD cap) { return vx_return_string_a("C:\\windows", buf, cap); }
static WINAPI DWORD GetWindowsDirectoryW(WCHAR *buf, DWORD cap) { return vx_return_string_w("C:\\windows", buf, cap); }
static WINAPI DWORD GetSystemDirectoryA(char *buf, DWORD cap) { return vx_return_string_a("C:\\windows\\system32", buf, cap); }
static WINAPI DWORD GetSystemDirectoryW(WCHAR *buf, DWORD cap) { return vx_return_string_w("C:\\windows\\system32", buf, cap); }

// ---- opening, reading, writing --------------------------------------------------------------------------------------
#define GENERIC_READ     0x80000000u
#define GENERIC_WRITE    0x40000000u
#define GENERIC_ALL      0x10000000u
#define FILE_READ_DATA   0x1u
#define FILE_WRITE_DATA  0x2u
#define FILE_APPEND_DATA 0x4u
#define INVALID_HANDLE   ((HANDLE)(intptr_t)-1)
#define FILE_FLAG_DELETE_ON_CLOSE 0x04000000u

HANDLE vx_pipe_open(const char *name, DWORD flags);
int vx_is_pipe_name(const char *name);
int vx_pipe_read(HANDLE h, void *buf, DWORD n, DWORD *got, void *ov, BOOL *ok);
int vx_pipe_peek(HANDLE h, void *buf, DWORD n, DWORD *got, DWORD *avail, DWORD *left);
int vx_is_pipe(HANDLE h);
static HANDLE create_file(const char *name, DWORD access, DWORD disposition, DWORD attrs) {
    if (vx_is_pipe_name(name)) return vx_pipe_open(name, attrs);   // the client end of a named pipe (pipes.c)
    if (!strcasecmp(name, "CONOUT$")) return (HANDLE)(intptr_t)-11;
    if (!strcasecmp(name, "CONIN$")) return (HANDLE)(intptr_t)-10;
    char path[MAX_PATH_LEN];
    int rd = (access & (GENERIC_READ | GENERIC_ALL | FILE_READ_DATA)) != 0;
    int wr = (access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0;
    int writes = wr || disposition == 1 || disposition == 2 || disposition == 4 || disposition == 5;
    static int trace = -1;                                    // VXRT_TRACE_FILES=1: opens that fail, with the path; 2: every open
    if (trace < 0) trace = getenv("VXRT_TRACE_FILES") != NULL;
    if (!(writes ? vx_unix_path_write(name, path, sizeof path, disposition != 2 && disposition != 5) : vx_unix_path(name, path, sizeof path))) {
        if (trace) vx_log("CreateFile(%s): no path", name);
        return INVALID_HANDLE;
    }
    int flags = wr ? (rd ? O_RDWR : O_WRONLY) : O_RDONLY;
    switch (disposition) {
    case 1: flags |= O_CREAT | O_EXCL; break;    // CREATE_NEW
    case 2: flags |= O_CREAT | O_TRUNC; break;   // CREATE_ALWAYS
    case 3: break;                               // OPEN_EXISTING
    case 4: flags |= O_CREAT; break;             // OPEN_ALWAYS
    case 5: flags |= O_TRUNC; break;             // TRUNCATE_EXISTING
    default: vx_set_last_error(87); return INVALID_HANDLE;
    }
    struct stat st;
    int existed = stat(path, &st) == 0;
    if (existed && S_ISDIR(st.st_mode)) flags = O_RDONLY;    // directories open for their attributes only
    int fd = open(path, flags | O_CLOEXEC, 0666);
    if (fd < 0) {
        DWORD e = vx_error_from_errno(errno);
        if (e == 2) {                                         // a missing parent is "path not found"
            char *slash = strrchr(path, '/');
            if (slash) { *slash = 0; if (stat(path, &st)) e = 3; }
        }
        vx_set_last_error(e);
        if (trace) vx_log("CreateFile(%s, access %#x, disposition %u): error %u", name, access, disposition, e);
        return INVALID_HANDLE;
    }
    if (fd >= 0x10000) { close(fd); vx_set_last_error(4); return INVALID_HANDLE; }   // (handles 0x10000 + fd)
    if (trace && trace_opens_all()) vx_log("CreateFile(%s, access %#x, disposition %u): %s, handle %#x", name, access, disposition, writes ? "for writing" : "opened", 0x10000 + fd);
    HANDLE h = (HANDLE)(intptr_t)(0x10000 + fd);
    if (attrs & FILE_FLAG_DELETE_ON_CLOSE) vx_file_delete_on_close(h, path, 1);   // (SQLite's temporary files; src/filestate.c)
    vx_set_last_error(existed && (disposition == 2 || disposition == 4) ? 183 /* ERROR_ALREADY_EXISTS */ : 0);
    return h;
}
static WINAPI HANDLE CreateFileA(const char *name, DWORD access, DWORD share, void *sa, DWORD disposition, DWORD flags, HANDLE tmpl) {
    (void)share; (void)sa; (void)tmpl;
    return create_file(name, access, disposition, flags);
}
static WINAPI HANDLE CreateFileW(const WCHAR *name, DWORD access, DWORD share, void *sa, DWORD disposition, DWORD flags, HANDLE tmpl) {
    (void)share; (void)sa; (void)tmpl;
    WIDE_PATH(name, a);
    HANDLE h = create_file(a, access, disposition, flags);
    free(a);
    return h;
}

// OVERLAPPED: Internal, InternalHigh, Offset, OffsetHigh, hEvent. With one, the transfer happens at its offset
// (synchronously).
typedef struct { uint64_t internal, internal_high; DWORD offset, offset_high; HANDLE event; } OVERLAPPED;

// The error for a failed read or write: a handle opened without that access is ERROR_ACCESS_DENIED on Windows (the
// descriptor's EBADF here).
DWORD vx_file_io_error(int fd, int e) { return e == EBADF && fcntl(fd, F_GETFL) >= 0 ? 5 /* ERROR_ACCESS_DENIED */ : vx_error_from_errno(e); }

// With an OVERLAPPED the read is at its offset; the file pointer of a synchronous handle then follows the bytes read, as
// without one (ReadFile's documentation; this runtime's handles are all synchronous). At or past the end of the file
// that read fails with ERROR_HANDLE_EOF; one that crosses it is short (SQLite fills the rest with zeros itself).
static WINAPI BOOL ReadFile(HANDLE h, void *buf, DWORD n, DWORD *got, OVERLAPPED *ov) {
    BOOL piped;
    if (vx_pipe_read(h, buf, n, got, ov, &piped)) return piped;
    int fd = vx_handle_fd(h);
    if (fd < 0) { vx_set_last_error(6); return 0; }
    off_t at = ov ? (off_t)ov->offset | (off_t)ov->offset_high << 32 : 0;
    ssize_t r = ov ? pread(fd, buf, n, at) : read(fd, buf, n);
    if (r < 0) { vx_set_last_error(vx_file_io_error(fd, errno)); if (got) *got = 0; return 0; }
    if (got) *got = (DWORD)r;
    if (ov) {
        if (!r && n) { vx_io_completed(h, ov, 0, 38 /* ERROR_HANDLE_EOF */); vx_set_last_error(38); return 0; }
        if (fd > 2) lseek(fd, at + r, SEEK_SET);
        vx_io_completed(h, ov, (DWORD)r, 0);
    }
    return 1;
}

static WINAPI DWORD SetFilePointer(HANDLE h, LONG low, LONG *high, DWORD method) {
    int fd = vx_handle_fd(h);
    if (fd < 0) { vx_set_last_error(6); return 0xffffffffu; }
    off_t dist = high ? (off_t)(((uint64_t)(uint32_t)*high << 32) | (uint32_t)low) : (off_t)low;
    off_t pos = lseek(fd, dist, method == 0 ? SEEK_SET : method == 1 ? SEEK_CUR : SEEK_END);
    if (pos < 0) { set_error_from_errno(); return 0xffffffffu; }
    if (high) *high = (LONG)(pos >> 32);
    vx_set_last_error(0);
    return (DWORD)pos;
}
static WINAPI BOOL SetFilePointerEx(HANDLE h, int64_t dist, int64_t *newpos, DWORD method) {
    int fd = vx_handle_fd(h);
    if (fd < 0) { vx_set_last_error(6); return 0; }
    off_t pos = lseek(fd, dist, method == 0 ? SEEK_SET : method == 1 ? SEEK_CUR : SEEK_END);
    if (pos < 0) { set_error_from_errno(); return 0; }
    if (newpos) *newpos = pos;
    return 1;
}
static WINAPI DWORD GetFileSize(HANDLE h, DWORD *high) {
    struct stat st; int fd = vx_handle_fd(h);
    if (fd < 0 || fstat(fd, &st)) { vx_set_last_error(6); return 0xffffffffu; }
    if (high) *high = (DWORD)((uint64_t)st.st_size >> 32);
    if ((DWORD)st.st_size == 0xffffffffu) vx_set_last_error(0);   // (INVALID_FILE_SIZE as a size: NO_ERROR tells it apart)
    return (DWORD)st.st_size;
}
static WINAPI BOOL GetFileSizeEx(HANDLE h, int64_t *size) {
    struct stat st; int fd = vx_handle_fd(h);
    if (fd < 0 || fstat(fd, &st)) { vx_set_last_error(6); return 0; }
    *size = st.st_size;
    return 1;
}
// Both need a handle opened for writing (ERROR_ACCESS_DENIED otherwise).
static int read_only_file(int fd) { struct stat st; return (fcntl(fd, F_GETFL) & O_ACCMODE) == O_RDONLY && !fstat(fd, &st) && S_ISREG(st.st_mode); }
static WINAPI BOOL SetEndOfFile(HANDLE h) {
    int fd = vx_handle_fd(h);
    if (fd < 0) { vx_set_last_error(6); return 0; }
    off_t pos = lseek(fd, 0, SEEK_CUR);
    if (pos < 0) { set_error_from_errno(); return 0; }
    if (read_only_file(fd)) { vx_set_last_error(5); return 0; }
    if (ftruncate(fd, pos)) { set_error_from_errno(); return 0; }
    return 1;
}
static WINAPI BOOL FlushFileBuffers(HANDLE h) {
    if (vx_is_pipe(h)) return 1;
    int fd = vx_handle_fd(h);
    if (fd < 0) { vx_set_last_error(6); return 0; }
    if (fd <= 2) return 1;
    if (read_only_file(fd)) { vx_set_last_error(5); return 0; }
    if (fsync(fd)) { set_error_from_errno(); return 0; }
    return 1;
}
static WINAPI DWORD GetFileType(HANDLE h) {
    if (vx_is_pipe(h)) return 3;                             // FILE_TYPE_PIPE
    struct stat st; int fd = vx_handle_fd(h);
    if (fd < 0 || fstat(fd, &st)) return 0;                  // FILE_TYPE_UNKNOWN
    if (S_ISCHR(st.st_mode)) return 2;                       // FILE_TYPE_CHAR
    if (S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode)) return 3;   // FILE_TYPE_PIPE
    return 1;                                                // FILE_TYPE_DISK
}

// ---- attributes ---------------------------------------------------------------------------------------------------------
#define FILE_ATTRIBUTE_READONLY  0x01
#define FILE_ATTRIBUTE_HIDDEN    0x02
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define FILE_ATTRIBUTE_NORMAL    0x80
#define INVALID_FILE_ATTRIBUTES  0xffffffffu

static DWORD attributes(const struct stat *st, const char *name) {
    DWORD a = S_ISDIR(st->st_mode) ? FILE_ATTRIBUTE_DIRECTORY : 0;
    if (!(st->st_mode & S_IWUSR)) a |= FILE_ATTRIBUTE_READONLY;
    if (name && name[0] == '.' && strcmp(name, ".") && strcmp(name, "..")) a |= FILE_ATTRIBUTE_HIDDEN;
    return a ? a : FILE_ATTRIBUTE_NORMAL;
}
static void filetime(const struct timespec *ts, DWORD out[2]) {
    uint64_t t = ((uint64_t)ts->tv_sec + 11644473600ull) * 10000000ull + (uint64_t)ts->tv_nsec / 100;
    out[0] = (DWORD)t; out[1] = (DWORD)(t >> 32);
}

static int stat_windows(const char *name, struct stat *st) {
    char path[MAX_PATH_LEN];
    if (!vx_unix_path(name, path, sizeof path)) return -1;
    if (stat(path, st)) {
        DWORD e = vx_error_from_errno(errno);
        char *slash = strrchr(path, '/');
        if (e == 2 && slash) { *slash = 0; struct stat parent; if (stat(path, &parent)) e = 3; }
        vx_set_last_error(e);
        return -1;
    }
    return 0;
}
static const char *base_name(const char *p) {
    const char *b = p;
    for (; *p; p++) if (*p == '\\' || *p == '/') b = p + 1;
    return b;
}

static WINAPI DWORD GetFileAttributesA(const char *name) {
    struct stat st;
    return stat_windows(name, &st) ? INVALID_FILE_ATTRIBUTES : attributes(&st, base_name(name));
}
static WINAPI DWORD GetFileAttributesW(const WCHAR *name) { WIDE_PATH(name, a); DWORD r = GetFileAttributesA(a); free(a); return r; }
static WINAPI BOOL SetFileAttributesA(const char *name, DWORD attrs) { (void)attrs; struct stat st; return stat_windows(name, &st) == 0; }
static WINAPI BOOL SetFileAttributesW(const WCHAR *name, DWORD attrs) { WIDE_PATH(name, a); BOOL r = SetFileAttributesA(a, attrs); free(a); return r; }

// WIN32_FILE_ATTRIBUTE_DATA: attributes, creation/access/write FILETIMEs, size high, size low.
typedef struct { DWORD attrs, created[2], accessed[2], written[2], size_high, size_low; } FILE_ATTRIBUTE_DATA;
static WINAPI BOOL GetFileAttributesExA(const char *name, int level, FILE_ATTRIBUTE_DATA *d) {
    (void)level;
    struct stat st;
    if (stat_windows(name, &st)) return 0;
    d->attrs = attributes(&st, base_name(name));
    filetime(&st.st_birthtimespec, d->created); filetime(&st.st_atimespec, d->accessed); filetime(&st.st_mtimespec, d->written);
    d->size_high = S_ISDIR(st.st_mode) ? 0 : (DWORD)((uint64_t)st.st_size >> 32);
    d->size_low = S_ISDIR(st.st_mode) ? 0 : (DWORD)st.st_size;
    return 1;
}
static WINAPI BOOL GetFileAttributesExW(const WCHAR *name, int level, FILE_ATTRIBUTE_DATA *d) {
    WIDE_PATH(name, a); BOOL r = GetFileAttributesExA(a, level, d); free(a); return r;
}

// BY_HANDLE_FILE_INFORMATION: attributes, three FILETIMEs, volume serial, size high/low, links, file index.
static WINAPI BOOL GetFileInformationByHandle(HANDLE h, DWORD *info) {
    struct stat st; int fd = vx_handle_fd(h);
    if (fd < 0 || fstat(fd, &st)) { vx_set_last_error(6); return 0; }
    memset(info, 0, 52);
    info[0] = attributes(&st, NULL);
    filetime(&st.st_birthtimespec, info + 1); filetime(&st.st_atimespec, info + 3); filetime(&st.st_mtimespec, info + 5);
    info[7] = (DWORD)st.st_dev;
    info[8] = (DWORD)((uint64_t)st.st_size >> 32); info[9] = (DWORD)st.st_size;
    info[10] = (DWORD)st.st_nlink;
    info[11] = (DWORD)((uint64_t)st.st_ino >> 32); info[12] = (DWORD)st.st_ino;
    return 1;
}
static WINAPI DWORD GetDriveTypeA(const char *root) {
    if (!root) return 3;
    char d = (char)(root[0] | 0x20);
    return (d == 'c' || d == 'z') && root[1] == ':' ? 3 /* DRIVE_FIXED */ : 1 /* DRIVE_NO_ROOT_DIR */;
}
static WINAPI DWORD GetDriveTypeW(const WCHAR *root) { char r[4] = { root ? (char)root[0] : 0, root ? (char)root[1] : 0, 0, 0 }; return GetDriveTypeA(root ? r : NULL); }
static WINAPI DWORD GetLogicalDrives(void) { return (1u << 2) | (1u << 25); }   // C: and Z:
static WINAPI BOOL PeekNamedPipe(HANDLE h, void *buf, DWORD n, DWORD *got, DWORD *avail, DWORD *left) {
    int peeked = vx_pipe_peek(h, buf, n, got, avail, left);
    if (peeked) return peeked > 0;
    int fd = vx_handle_fd(h), pending = 0;
    if (fd < 0 || ioctl(fd, FIONREAD, &pending) < 0) { vx_set_last_error(6); return 0; }
    if (got) *got = 0;
    if (avail) *avail = (DWORD)pending;
    if (left) *left = 0;
    (void)buf; (void)n;
    return 1;
}

// ---- directories, deleting, moving, copying ---------------------------------------------------------------------------------
// Runs op on the Mac path of a Windows path; returns the Windows BOOL.
static BOOL path_op(const char *name, int (*op)(const char *)) {
    char path[MAX_PATH_LEN], shadow[MAX_PATH_LEN];
    if (!vx_unix_path(name, path, sizeof path)) return 0;
    if (overlay_shadow(path, shadow, sizeof shadow)) {      // the original tree stays: act on the overlay only
        if (op == rmdir || op == unlink) return 1;
        make_parent_dirs(shadow);
        snprintf(path, sizeof path, "%s", shadow);
    }
    if (op(path)) { set_error_from_errno(); return 0; }
    return 1;
}
static int make_dir(const char *p) { return mkdir(p, 0777); }
static WINAPI BOOL CreateDirectoryA(const char *name, void *sa) {
    (void)sa;
    BOOL ok = path_op(name, make_dir);
    if (!ok && vx_get_last_error() == 80) vx_set_last_error(183);   // ERROR_ALREADY_EXISTS
    return ok;
}
static WINAPI BOOL CreateDirectoryW(const WCHAR *name, void *sa) { WIDE_PATH(name, a); BOOL r = CreateDirectoryA(a, sa); free(a); return r; }
static WINAPI BOOL RemoveDirectoryA(const char *name) { return path_op(name, rmdir); }
static WINAPI BOOL RemoveDirectoryW(const WCHAR *name) { WIDE_PATH(name, a); BOOL r = RemoveDirectoryA(a); free(a); return r; }
static WINAPI BOOL DeleteFileA(const char *name) { return path_op(name, unlink); }
static WINAPI BOOL DeleteFileW(const WCHAR *name) { WIDE_PATH(name, a); BOOL r = DeleteFileA(a); free(a); return r; }

#define MOVEFILE_REPLACE_EXISTING 1
static WINAPI BOOL MoveFileExA(const char *from, const char *to, DWORD flags) {
    char a[MAX_PATH_LEN], b[MAX_PATH_LEN], shadow[MAX_PATH_LEN];
    if (!vx_unix_path(from, a, sizeof a) || !vx_unix_path(to, b, sizeof b)) return 0;
    struct stat st;
    if (!(flags & MOVEFILE_REPLACE_EXISTING) && !stat(b, &st) && strcasecmp(a, b)) { vx_set_last_error(183); return 0; }
    if (!vx_unix_path_write(to, b, sizeof b, 0)) return 0;
    if (overlay_shadow(a, shadow, sizeof shadow)) { unlink(b); copy_file(a, b); return 1; }   // the original stays
    if (rename(a, b)) { set_error_from_errno(); return 0; }
    return 1;
}
static WINAPI BOOL MoveFileExW(const WCHAR *from, const WCHAR *to, DWORD flags) {
    char *a = vx_utf8_dup(from), *b = vx_utf8_dup(to);
    BOOL r = MoveFileExA(a, b, flags);
    free(a); free(b);
    return r;
}
static WINAPI BOOL MoveFileA(const char *from, const char *to) { return MoveFileExA(from, to, 0); }
static WINAPI BOOL MoveFileW(const WCHAR *from, const WCHAR *to) { return MoveFileExW(from, to, 0); }

static WINAPI BOOL CopyFileA(const char *from, const char *to, BOOL fail_if_exists) {
    char a[MAX_PATH_LEN], b[MAX_PATH_LEN];
    if (!vx_unix_path(from, a, sizeof a) || !vx_unix_path_write(to, b, sizeof b, 0)) return 0;
    int in = open(a, O_RDONLY | O_CLOEXEC);
    if (in < 0) { set_error_from_errno(); return 0; }
    int out = open(b, O_WRONLY | O_CREAT | O_CLOEXEC | (fail_if_exists ? O_EXCL : O_TRUNC), 0666);
    if (out < 0) { set_error_from_errno(); close(in); return 0; }
    char buf[65536]; ssize_t n; BOOL ok = 1;
    while ((n = read(in, buf, sizeof buf)) > 0)
        if (write(out, buf, (size_t)n) != n) { set_error_from_errno(); ok = 0; break; }
    close(in); close(out);
    return ok;
}
static WINAPI BOOL CopyFileW(const WCHAR *from, const WCHAR *to, BOOL fail_if_exists) {
    char *a = vx_utf8_dup(from), *b = vx_utf8_dup(to);
    BOOL r = CopyFileA(a, b, fail_if_exists);
    free(a); free(b);
    return r;
}

// ---- directory listing --------------------------------------------------------------------------------------------------------
// WIN32_FIND_DATAA/W: attributes, three FILETIMEs, size high/low, two reserved DWORDs, then the name (260
// characters) and the 8.3 name (14).
typedef struct { DWORD attrs, created[2], accessed[2], written[2], size_high, size_low, reserved[2]; char name[260], alt[14]; } FIND_DATA_A;
typedef struct { DWORD attrs, created[2], accessed[2], written[2], size_high, size_low, reserved[2]; WCHAR name[260], alt[14]; } FIND_DATA_W;
_Static_assert(sizeof(FIND_DATA_A) == 320, "WIN32_FIND_DATAA");
_Static_assert(sizeof(FIND_DATA_W) == 592, "WIN32_FIND_DATAW");

// A listing reads the overlay's copy of the directory first (when there is one), then the original, leaving out names
// the overlay has. (Half-Life: Alyx gathers save/*.hl? into each save: listing only the original, where the save folder
// of an earlier Windows installation was, its saves came out empty, and loading one left another game's level state
// in place: the next level change crashed.)
typedef struct { DIR *dirs[2]; char paths[2][MAX_PATH_LEN]; int ndirs, at; char pattern[256]; } find_state;
static void find_destroy(void *p) { find_state *f = p; for (int i = 0; i < f->ndirs; i++) if (f->dirs[i]) closedir(f->dirs[i]); free(f); }

// Fills *d with the next entry matching the pattern (Windows wildcards: '*' and '?', case-insensitive; "*.*"
// matches every name). Returns 0 at the end.
static int find_next(find_state *f, FIND_DATA_A *d) {
    struct dirent *e;
    for (; f->at < f->ndirs; f->at++) while ((e = readdir(f->dirs[f->at]))) {
        const char *pat = !strcmp(f->pattern, "*.*") ? "*" : f->pattern;
        if (fnmatch(pat, e->d_name, FNM_CASEFOLD | FNM_NOESCAPE)) continue;
        char full[MAX_PATH_LEN * 2];
        struct stat st;
        if (f->at > 0) {                                           // (the original: unless the overlay has the name)
            snprintf(full, sizeof full, "%s/%s", f->paths[0], e->d_name);
            if (!lstat(full, &st)) continue;
        }
        snprintf(full, sizeof full, "%s/%s", f->paths[f->at], e->d_name);
        if (stat(full, &st)) continue;
        memset(d, 0, sizeof *d);
        d->attrs = attributes(&st, e->d_name);
        filetime(&st.st_birthtimespec, d->created); filetime(&st.st_atimespec, d->accessed); filetime(&st.st_mtimespec, d->written);
        if (!S_ISDIR(st.st_mode)) { d->size_high = (DWORD)((uint64_t)st.st_size >> 32); d->size_low = (DWORD)st.st_size; }
        snprintf(d->name, sizeof d->name, "%s", e->d_name);
        return 1;
    }
    return 0;
}

static HANDLE find_first(const char *spec, FIND_DATA_A *d) {
    char full[MAX_PATH_LEN];
    if (!vx_full_path(spec, full, sizeof full)) { vx_set_last_error(206); return INVALID_HANDLE; }
    char *sep = strrchr(full, '\\');
    find_state *f = calloc(1, sizeof *f);
    snprintf(f->pattern, sizeof f->pattern, "%s", sep + 1);
    sep[sep == full + 2 ? 1 : 0] = 0;           // keep "C:\" for the root
    char dir[MAX_PATH_LEN], shadow[MAX_PATH_LEN];
    if (!vx_unix_path(full, dir, sizeof dir)) { free(f); return INVALID_HANDLE; }
    struct stat st;
    if (overlay_shadow(dir, shadow, sizeof shadow) && !stat(shadow, &st) && S_ISDIR(st.st_mode) && (f->dirs[f->ndirs] = opendir(shadow)))
        snprintf(f->paths[f->ndirs++], MAX_PATH_LEN, "%s", shadow);
    if (strcmp(dir, f->ndirs ? f->paths[0] : "") && (f->dirs[f->ndirs] = opendir(dir))) snprintf(f->paths[f->ndirs++], MAX_PATH_LEN, "%s", dir);
    if (!f->ndirs) { if (vx_trace) vx_log("FindFirstFile(%s): no directory %s", spec, dir); DWORD e = vx_error_from_errno(errno); free(f); vx_set_last_error(e == 2 ? 3 : e); return INVALID_HANDLE; }
    if (!find_next(f, d)) {
        if (vx_trace) vx_log("FindFirstFile(%s): nothing in %s%s%s matches %s", spec, f->paths[0], f->ndirs > 1 ? " or " : "", f->ndirs > 1 ? f->paths[1] : "", f->pattern);
        find_destroy(f); vx_set_last_error(2 /* ERROR_FILE_NOT_FOUND */); return INVALID_HANDLE;
    }
    if (vx_trace) vx_log("FindFirstFile(%s): %s", spec, d->name);
    HANDLE h = vx_handle_new(VX_HANDLE_FIND, f, find_destroy);
    if (!h) { find_destroy(f); vx_set_last_error(4); return INVALID_HANDLE; }
    return h;
}
static void to_wide(const FIND_DATA_A *a, FIND_DATA_W *w) {
    memcpy(w, a, offsetof(FIND_DATA_A, name));
    size_t n = vx_utf8_to_utf16(a->name, strlen(a->name), w->name, 259);
    w->name[n < 259 ? n : 259] = 0;
    w->alt[0] = 0;
}

static WINAPI HANDLE FindFirstFileA(const char *spec, FIND_DATA_A *d) { return find_first(spec, d); }
static WINAPI HANDLE FindFirstFileW(const WCHAR *spec, FIND_DATA_W *d) {
    WIDE_PATH(spec, a);
    FIND_DATA_A tmp;
    HANDLE h = find_first(a, &tmp);
    free(a);
    if (h != INVALID_HANDLE) to_wide(&tmp, d);
    return h;
}
static WINAPI HANDLE FindFirstFileExA(const char *spec, int level, FIND_DATA_A *d, int op, void *filter, DWORD flags) {
    (void)level; (void)op; (void)filter; (void)flags; return find_first(spec, d);
}
static WINAPI HANDLE FindFirstFileExW(const WCHAR *spec, int level, FIND_DATA_W *d, int op, void *filter, DWORD flags) {
    (void)level; (void)op; (void)filter; (void)flags; return FindFirstFileW(spec, d);
}
static WINAPI BOOL FindNextFileA(HANDLE h, FIND_DATA_A *d) {
    find_state *f = vx_handle_data(h, VX_HANDLE_FIND);
    if (!f) { vx_set_last_error(6); return 0; }
    if (!find_next(f, d)) { vx_set_last_error(18 /* ERROR_NO_MORE_FILES */); return 0; }
    return 1;
}
static WINAPI BOOL FindNextFileW(HANDLE h, FIND_DATA_W *d) {
    FIND_DATA_A tmp;
    if (!FindNextFileA(h, &tmp)) return 0;
    to_wide(&tmp, d);
    return 1;
}
static WINAPI BOOL FindClose(HANDLE h) {
    if (!vx_handle_data(h, VX_HANDLE_FIND)) { vx_set_last_error(6); return 0; }
    return vx_handle_close(h);
}

// ---- file mappings and shared memory --------------------------------------------------------------------------------------------
// A mapping of a file maps its descriptor; a mapping of the paging file (INVALID_HANDLE_VALUE) is POSIX shared
// memory, named after a hash of the Windows name ("Local\\" and "Global\\" dropped) so that other processes
// opening the same name share it.
typedef struct { int fd; uint64_t size; int writable; char shm[40]; DWORD attributes; } mapping;   // shm: a name this process created
#define SEC_FILE 0x800000u
#define SEC_IMAGE 0x1000000u
#define SEC_RESERVE 0x4000000u
#define SEC_COMMIT 0x8000000u
#define SEC_NOCACHE 0x10000000u
#define SEC_WRITECOMBINE 0x40000000u
#define SEC_LARGE_PAGES 0x80000000u
int vx_name_join(const char *object_shm);
int vx_name_leave(const char *object_shm);
static void mapping_destroy(void *p) {
    mapping *m = p;
    if (m->shm[0]) vx_name_leave(m->shm);                          // like Windows, the name goes with its last holder
    close(m->fd);
    free(m);
}

static void shm_name(const char *name, char *out, size_t cap) {
    if (!strncasecmp(name, "Local\\", 6)) name += 6;
    else if (!strncasecmp(name, "Global\\", 7)) name += 7;
    uint64_t h = 1469598103934665603ull;
    for (const char *c = name; *c; c++) { h ^= (uint8_t)*c; h *= 1099511628211ull; }
    snprintf(out, cap, "/vx.%016llx", (unsigned long long)h);
}

#define PAGE_READONLY 0x02
#define SECTION_QUERY 0x1
#define SECTION_MAP_WRITE 0x2
#define SECTION_MAP_READ 0x4
#define SECTION_MAP_EXECUTE 0x8
#define SECTION_ALL_ACCESS 0xf001fu
#define PAGE_WRITECOPY 0x08
#define PAGE_EXECUTE_READ 0x20
static HANDLE create_mapping(HANDLE file, DWORD protect, uint64_t size, const char *name) {
    if (vx_trace && name) vx_log("CreateFileMapping %s (%llu bytes)", name, (unsigned long long)size);
    int writable = !(protect & (PAGE_READONLY | PAGE_EXECUTE_READ | PAGE_WRITECOPY));
    int fd;
    int existed = 0;
    char created[40] = "";
    if (file == INVALID_HANDLE) {
        char shm[40];
        if (name) {
            shm_name(name, shm, sizeof shm);
            vx_name_join(shm);                                     // (removes what processes that ended left)
        } else {                                                   // unnamed: a private name, so a duplicate can share it
            static int counter;
            snprintf(shm, sizeof shm, "/vx.%d.%d", getpid(), __sync_fetch_and_add(&counter, 1));
            vx_name_join(shm);
        }
        fd = shm_open(shm, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd < 0 && errno == EEXIST && name) { fd = shm_open(shm, O_RDWR, 0600); existed = 1; }
        if (fd < 0) { set_error_from_errno(); return NULL; }
        snprintf(created, sizeof created, "%s", shm);              // every holder leaves the name when done
        struct stat st;
        if (!existed || (!fstat(fd, &st) && (uint64_t)st.st_size < size)) ftruncate(fd, (off_t)size);
        if (existed && !fstat(fd, &st)) size = (uint64_t)st.st_size;
        if (getenv("VXRT_TRACE_NAMES") && !fstat(fd, &st)) vx_log("[%d] mapping %s: existed %d, inode %llu, size %llu", getpid(), shm, existed, (unsigned long long)st.st_ino, (unsigned long long)st.st_size);
    } else {
        int ffd = vx_handle_fd(file);
        if (ffd < 0 || (fd = dup(ffd)) < 0) { vx_set_last_error(6); return NULL; }
        struct stat st;
        fstat(fd, &st);
        if (!size) size = (uint64_t)st.st_size;
        else if (size > (uint64_t)st.st_size && writable) ftruncate(fd, (off_t)size);   // Windows extends the file
    }
    if (!size) { close(fd); vx_set_last_error(1006 /* ERROR_FILE_INVALID */); return NULL; }
    mapping *m = malloc(sizeof *m);
    m->fd = fd; m->size = size; m->writable = writable;
    snprintf(m->shm, sizeof m->shm, "%s", created);
    // The SEC_* attributes NtQuerySection reports (src/section.c): what was asked for, and SEC_FILE for a file's.
    m->attributes = protect & (SEC_IMAGE | SEC_RESERVE | SEC_COMMIT | SEC_NOCACHE | SEC_WRITECOMBINE | SEC_LARGE_PAGES);
    if (file != INVALID_HANDLE) m->attributes = (m->attributes & ~(DWORD)(SEC_RESERVE | SEC_COMMIT)) | SEC_FILE;
    else if (!(m->attributes & (SEC_RESERVE | SEC_COMMIT))) m->attributes |= SEC_COMMIT;   // (the default)
    HANDLE h = vx_handle_new(VX_HANDLE_MAPPING, m, mapping_destroy);
    if (!h) { mapping_destroy(m); vx_set_last_error(4); return NULL; }
    vx_handle_set_access(h, SECTION_ALL_ACCESS);
    vx_set_last_error(existed ? 183 /* ERROR_ALREADY_EXISTS */ : 0);
    return h;
}
static WINAPI HANDLE CreateFileMappingA(HANDLE file, void *sa, DWORD protect, DWORD high, DWORD low, const char *name) {
    (void)sa; return create_mapping(file, protect, (uint64_t)high << 32 | low, name);
}
static WINAPI HANDLE CreateFileMappingW(HANDLE file, void *sa, DWORD protect, DWORD high, DWORD low, const WCHAR *name) {
    (void)sa; char *a = vx_utf8_dup(name); HANDLE h = create_mapping(file, protect, (uint64_t)high << 32 | low, a); free(a); return h;
}
// A file mapping's access rights (SECTION_*) from what a program asks for: FILE_MAP_* and the generic rights as
// "File Mapping Security and Access Rights" maps them; FILE_MAP_EXECUTE asks for SECTION_MAP_EXECUTE.
DWORD vx_mapping_access(DWORD a) {
    DWORD out = a & 0x00ffffffu & ~0x20u;
    if (a & 0x20) out |= SECTION_MAP_EXECUTE;                                    // FILE_MAP_EXECUTE
    if (a & 0x80000000u) out |= 0x20000 | SECTION_QUERY | SECTION_MAP_READ;      // GENERIC_READ
    if (a & 0x40000000u) out |= 0x20000 | SECTION_MAP_WRITE;                     // GENERIC_WRITE
    if (a & 0x20000000u) out |= 0x20000 | SECTION_MAP_EXECUTE;                   // GENERIC_EXECUTE
    if (a & 0x10000000u) out |= SECTION_ALL_ACCESS;                              // GENERIC_ALL
    return out;
}
static HANDLE open_mapping(const char *name) {
    if (vx_trace) vx_log("OpenFileMapping %s", name);
    char shm[40];
    shm_name(name, shm, sizeof shm);
    if (!vx_name_join(shm)) { vx_name_leave(shm); vx_set_last_error(2); return NULL; }   // nobody holds it
    int fd = shm_open(shm, O_RDWR, 0600);
    if (fd < 0) { vx_name_leave(shm); vx_set_last_error(2); return NULL; }
    struct stat st; fstat(fd, &st);
    mapping *m = malloc(sizeof *m);
    m->fd = fd; m->size = (uint64_t)st.st_size; m->writable = 1;
    m->attributes = SEC_COMMIT;                                // (named ones are always paging-file memory here)
    snprintf(m->shm, sizeof m->shm, "%s", shm);                // this holder leaves the name when done
    HANDLE h = vx_handle_new(VX_HANDLE_MAPPING, m, mapping_destroy);
    if (!h) mapping_destroy(m);
    return h;
}
// For DuplicateHandle into another process (sync.c): the shared memory name of a mapping, and a mapping object
// from such a name.
int vx_mapping_shm(HANDLE h, char *shm, size_t cap, uint64_t *size) {
    mapping *m = vx_handle_data(h, VX_HANDLE_MAPPING);
    if (!m || !m->shm[0]) return 0;
    snprintf(shm, cap, "%s", m->shm);
    *size = m->size;
    vx_name_join(m->shm);                                          // held for the other process until it closes it
    return 1;
}
void *vx_mapping_from_shm(const char *shm, uint64_t size, void (**destroy)(void *)) {
    int fd = shm_open(shm, O_RDWR, 0600);
    if (fd < 0) return NULL;
    mapping *m = malloc(sizeof *m);
    m->fd = fd; m->size = size; m->writable = 1;
    m->attributes = SEC_COMMIT;
    snprintf(m->shm, sizeof m->shm, "%s", shm);
    vx_name_join(shm);
    *destroy = mapping_destroy;
    return m;
}
// For NtQuerySection (src/section.c): a mapping's size and SEC_* attributes; 0 if h is not a mapping.
int vx_mapping_info(HANDLE h, uint64_t *size, DWORD *attributes) {
    mapping *m = vx_handle_data(h, VX_HANDLE_MAPPING);
    if (!m) return 0;
    *size = m->size; *attributes = m->attributes;
    return 1;
}
static WINAPI HANDLE OpenFileMappingA(DWORD access, BOOL inherit, const char *name) {
    (void)inherit; HANDLE h = open_mapping(name);
    if (h) vx_handle_set_access(h, vx_mapping_access(access));
    return h;
}
static WINAPI HANDLE OpenFileMappingW(DWORD access, BOOL inherit, const WCHAR *name) {
    char *a = vx_utf8_dup(name); HANDLE h = OpenFileMappingA(access, inherit, a); free(a); return h;
}

// Views, so UnmapViewOfFile knows their sizes.
typedef struct { void *addr; size_t size; } view;
static view *g_views; static int g_view_count, g_view_cap;
static pthread_mutex_t g_views_lock = PTHREAD_MUTEX_INITIALIZER;
#define FILE_MAP_COPY  0x1
#define FILE_MAP_WRITE 0x2
#define FILE_MAP_READ  0x4
#define FILE_MAP_EXECUTE 0x20
// Whether a handle with the access `granted` (SECTION_*) may map the view `access` (FILE_MAP_*), as Microsoft's "File
// Mapping Security and Access Rights" and MapViewOfFile pages put it: FILE_MAP_WRITE "allows mapping of read-only,
// copy-on-write, or read/write views", and FILE_MAP_WRITE | FILE_MAP_READ and FILE_MAP_ALL_ACCESS are the same as
// FILE_MAP_WRITE. So a view with the write bit (FILE_MAP_ALL_ACCESS too) needs SECTION_MAP_WRITE; a read-only or
// copy-on-write one SECTION_MAP_READ or SECTION_MAP_WRITE; FILE_MAP_EXECUTE also SECTION_MAP_EXECUTE.
static int view_allowed(DWORD access, DWORD granted) {
    if (access & FILE_MAP_WRITE) { if (!(granted & SECTION_MAP_WRITE)) return 0; }
    else if (access & (FILE_MAP_READ | FILE_MAP_COPY)) { if (!(granted & (SECTION_MAP_READ | SECTION_MAP_WRITE))) return 0; }
    if ((access & FILE_MAP_EXECUTE) && !(granted & SECTION_MAP_EXECUTE)) return 0;
    return 1;
}
// A copy-on-write view (FILE_MAP_COPY) of paging-file memory: macOS maps a POSIX shared memory object only shared
// (MAP_PRIVATE gives EINVAL), so the view is private memory with the object's bytes copied in when it is mapped.
// (Windows copies a page when the view first writes it; until then this copy does not see later writes through
// other views.)
static void *private_copy_view(mapping *m, uint64_t offset, size_t bytes, void *addr) {
    void *p = mmap(addr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) return MAP_FAILED;
    size_t have = offset < m->size ? (size_t)(m->size - offset) : 0;
    if (have > bytes) have = bytes;
    if (have) {
        void *src = mmap(NULL, have, PROT_READ, MAP_SHARED, m->fd, (off_t)offset);
        if (src == MAP_FAILED) { int e = errno; munmap(p, bytes); errno = e; return MAP_FAILED; }
        memcpy(p, src, have);
        munmap(src, have);
    }
    return p;
}
static WINAPI void *MapViewOfFileEx(HANDLE h, DWORD access, DWORD high, DWORD low, size_t bytes, void *addr) {
    mapping *m = vx_handle_data(h, VX_HANDLE_MAPPING);
    if (!m) { vx_set_last_error(6); return NULL; }
    uint64_t offset = (uint64_t)high << 32 | low;
    if (!bytes) bytes = (size_t)(m->size - offset);
    // FILE_MAP_ALL_ACCESS (0xf001f) contains the FILE_MAP_COPY bit; copy-on-write means FILE_MAP_COPY alone.
    int copy = (access & 0xffff) == FILE_MAP_COPY;
    DWORD granted;                                                 // the handle's access, where recorded (sync.c)
    if (vx_handle_access(h, &granted) && !view_allowed(access, granted)) { vx_set_last_error(5 /* ERROR_ACCESS_DENIED */); return NULL; }
    int prot = PROT_READ | ((access & FILE_MAP_WRITE) || copy ? PROT_WRITE : 0);
    void *p = mmap(addr, bytes, prot, copy ? MAP_PRIVATE : MAP_SHARED, m->fd, (off_t)offset);
    if (p == MAP_FAILED && copy && errno == EINVAL && m->shm[0]) p = private_copy_view(m, offset, bytes, addr);
    if (p == MAP_FAILED) { set_error_from_errno(); return NULL; }
    pthread_mutex_lock(&g_views_lock);
    if (g_view_count == g_view_cap) { g_view_cap = g_view_cap ? g_view_cap * 2 : 32; g_views = realloc(g_views, g_view_cap * sizeof *g_views); }
    g_views[g_view_count++] = (view){ p, bytes };
    pthread_mutex_unlock(&g_views_lock);
    return p;
}
static WINAPI void *MapViewOfFile(HANDLE h, DWORD access, DWORD high, DWORD low, size_t bytes) { return MapViewOfFileEx(h, access, high, low, bytes, NULL); }
// For kernelx.c's DiscardVirtualMemory: 1 when [start, end) touches a view of a file mapping (memory Windows reports
// as MEM_MAPPED, which VirtualQuery here does not tell apart from private memory).
int vx_in_mapped_view(uintptr_t start, uintptr_t end) {
    int found = 0;
    pthread_mutex_lock(&g_views_lock);
    for (int i = 0; i < g_view_count && !found; i++) {
        uintptr_t a = (uintptr_t)g_views[i].addr, b = a + g_views[i].size;
        found = start < b && a < end;
    }
    pthread_mutex_unlock(&g_views_lock);
    return found;
}
static WINAPI BOOL UnmapViewOfFile(void *addr) {
    pthread_mutex_lock(&g_views_lock);
    size_t size = 0;
    for (int i = 0; i < g_view_count; i++)
        if (g_views[i].addr == addr) { size = g_views[i].size; g_views[i] = g_views[--g_view_count]; break; }
    pthread_mutex_unlock(&g_views_lock);
    if (!size) { vx_set_last_error(487 /* ERROR_INVALID_ADDRESS */); return 0; }
    munmap(addr, size);
    return 1;
}
// A byte count of 0 flushes from addr to the end of its view (Microsoft's documentation). SQLite, with its database
// mapped (Chromium's profile databases), flushes that way at every commit: when this failed, each commit failed and
// was rolled back (steamwebhelper: "Failed to set up eq_classes table", an empty "Web Data", "Profile error occurred").
static WINAPI BOOL FlushViewOfFile(void *addr, size_t n) {
    uintptr_t start = (uintptr_t)addr, end = 0;
    pthread_mutex_lock(&g_views_lock);
    for (int i = 0; i < g_view_count && !end; i++) {
        uintptr_t a = (uintptr_t)g_views[i].addr, b = a + g_views[i].size;
        if (start >= a && start < b) end = n && n <= b - start ? start + n : b;
    }
    pthread_mutex_unlock(&g_views_lock);
    if (!end) { vx_set_last_error(487 /* ERROR_INVALID_ADDRESS */); return 0; }   // not in a view of a mapping
    uintptr_t page = (uintptr_t)getpagesize();
    start &= ~(page - 1);
    if (msync((void *)start, end - start, MS_SYNC)) { set_error_from_errno(); return 0; }
    return 1;
}

static const vx_export files_k32[] = {
    VX_EXPORT(GetFileInformationByHandle), VX_EXPORT(GetDriveTypeA), VX_EXPORT(GetDriveTypeW), VX_EXPORT(GetLogicalDrives),
    VX_EXPORT(PeekNamedPipe),
    VX_EXPORT(CreateFileMappingA), VX_EXPORT(CreateFileMappingW), VX_EXPORT(OpenFileMappingA), VX_EXPORT(OpenFileMappingW),
    VX_EXPORT(MapViewOfFile), VX_EXPORT(MapViewOfFileEx), VX_EXPORT(UnmapViewOfFile), VX_EXPORT(FlushViewOfFile),
    VX_EXPORT(GetCurrentDirectoryA), VX_EXPORT(GetCurrentDirectoryW), VX_EXPORT(SetCurrentDirectoryA), VX_EXPORT(SetCurrentDirectoryW),
    VX_EXPORT(GetFullPathNameA), VX_EXPORT(GetFullPathNameW), VX_EXPORT(GetTempPathA), VX_EXPORT(GetTempPathW),
    VX_EXPORT(GetWindowsDirectoryA), VX_EXPORT(GetWindowsDirectoryW), VX_EXPORT(GetSystemDirectoryA), VX_EXPORT(GetSystemDirectoryW),
    VX_EXPORT(CreateFileA), VX_EXPORT(CreateFileW), VX_EXPORT(ReadFile), VX_EXPORT(SetFilePointer), VX_EXPORT(SetFilePointerEx),
    VX_EXPORT(GetFileSize), VX_EXPORT(GetFileSizeEx), VX_EXPORT(SetEndOfFile), VX_EXPORT(FlushFileBuffers), VX_EXPORT(GetFileType),
    VX_EXPORT(GetFileAttributesA), VX_EXPORT(GetFileAttributesW), VX_EXPORT(SetFileAttributesA), VX_EXPORT(SetFileAttributesW),
    VX_EXPORT(GetFileAttributesExA), VX_EXPORT(GetFileAttributesExW),
    VX_EXPORT(CreateDirectoryA), VX_EXPORT(CreateDirectoryW), VX_EXPORT(RemoveDirectoryA), VX_EXPORT(RemoveDirectoryW),
    VX_EXPORT(DeleteFileA), VX_EXPORT(DeleteFileW), VX_EXPORT(MoveFileA), VX_EXPORT(MoveFileW), VX_EXPORT(MoveFileExA),
    VX_EXPORT(MoveFileExW), VX_EXPORT(CopyFileA), VX_EXPORT(CopyFileW),
    VX_EXPORT(FindFirstFileA), VX_EXPORT(FindFirstFileW), VX_EXPORT(FindFirstFileExA), VX_EXPORT(FindFirstFileExW),
    VX_EXPORT(FindNextFileA), VX_EXPORT(FindNextFileW), VX_EXPORT(FindClose),
};

static void make_dirs(const char *rel) {
    char p[MAX_PATH_LEN];
    snprintf(p, sizeof p, "%s/%s", g_root, rel);
    for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0777); *s = '/'; }
    mkdir(p, 0777);
}

void vx_files_init(void) {
    overlay_init();
    vx_filestate_init();
    const char *root = getenv("VXRT_ROOT");
    if (root && *root) {
        if (root[0] == '/') snprintf(g_root, sizeof g_root, "%s", root);
        else { char cwd[MAX_PATH_LEN]; snprintf(g_root, sizeof g_root, "%s/%s", getcwd(cwd, sizeof cwd) ? cwd : ".", root); }
    } else {
        snprintf(g_root, sizeof g_root, "%s/.vxrt", getenv("HOME") ? getenv("HOME") : "/tmp");
    }
    make_dirs("drive_c/windows/system32");
    make_dirs("drive_c/windows/temp");
    make_dirs("drive_c/users/vxrt");
    char cwd[MAX_PATH_LEN], real[MAX_PATH_LEN];
    if (getcwd(cwd, sizeof cwd) && realpath(cwd, real)) {
        char *r = realpath(g_root, NULL);
        if (r) { snprintf(g_root, sizeof g_root, "%s", r); free(r); }
        vx_windows_path(real, g_cwd, sizeof g_cwd);
    } else {
        snprintf(g_cwd, sizeof g_cwd, "C:\\");
    }
    vx_register_module("kernel32.dll", files_k32, sizeof files_k32 / sizeof files_k32[0]);
    vx_register_module("kernelbase.dll", files_k32, sizeof files_k32 / sizeof files_k32[0]);
}

#ifdef __aarch64__
#include "thunks/files.c.inc"
#endif
