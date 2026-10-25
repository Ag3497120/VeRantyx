// imagehlp's certificate functions (the Windows Steam client's steamclient64.dll imports them): the attribute
// certificate table of a PE file, read and changed through a file handle. From Microsoft's documentation of
// ImageEnumerateCertificates, ImageGetCertificateHeader, ImageGetCertificateData, ImageAddCertificate and
// ImageRemoveCertificate, and of the PE format: the table is data directory 4 (IMAGE_DIRECTORY_ENTRY_SECURITY), whose
// "address" is a file offset; it holds WIN_CERTIFICATE entries (dwLength, wRevision, wCertificateType, then the
// certificate), each starting on an 8-byte boundary. The table is not mapped into memory, so it is normally the last
// thing in the file; one that is not cannot be grown or shrunk here (ERROR_NOT_SUPPORTED).
// Nothing is verified here (that is WinVerifyTrust's job); the PE checksum is left as it is.
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "vx.h"

enum { ERROR_INVALID_HANDLE = 6, ERROR_NOT_SUPPORTED = 50, ERROR_INVALID_PARAMETER = 87, ERROR_INSUFFICIENT_BUFFER = 122,
       ERROR_BAD_EXE_FORMAT = 193 };
#define CERT_SECTION_TYPE_ANY 0xff

typedef struct {
    int fd;
    uint64_t dir_at;               // file offset of data directory 4 (its offset, then its size)
    uint32_t table, size;          // the table's file offset and size
    uint64_t file_size;
} cert_file;

static int rd(int fd, uint64_t at, void *p, size_t n) { return pread(fd, p, n, (off_t)at) == (ssize_t)n; }
static int wr(int fd, uint64_t at, const void *p, size_t n) { return pwrite(fd, p, n, (off_t)at) == (ssize_t)n; }

// The file's certificate table; 0 with the last error set if the handle is no file or the file no PE image.
static int open_table(HANDLE h, cert_file *f) {
    f->fd = vx_handle_fd(h);
    struct stat st;
    if (f->fd < 0 || fstat(f->fd, &st)) { vx_set_last_error(ERROR_INVALID_HANDLE); return 0; }
    f->file_size = (uint64_t)st.st_size;
    uint8_t mz[64], pe[24 + 2];
    uint32_t lfanew;
    if (!rd(f->fd, 0, mz, sizeof mz) || mz[0] != 'M' || mz[1] != 'Z') { vx_set_last_error(ERROR_BAD_EXE_FORMAT); return 0; }
    memcpy(&lfanew, mz + 0x3c, 4);
    if (!rd(f->fd, lfanew, pe, sizeof pe) || memcmp(pe, "PE\0\0", 4)) { vx_set_last_error(ERROR_BAD_EXE_FORMAT); return 0; }
    uint16_t magic; memcpy(&magic, pe + 24, 2);
    uint64_t opt = (uint64_t)lfanew + 24;
    uint64_t dirs = magic == 0x20b ? opt + 112 : magic == 0x10b ? opt + 96 : 0;
    uint32_t count;
    if (!dirs || !rd(f->fd, dirs - 4, &count, 4) || count <= 4) { vx_set_last_error(ERROR_BAD_EXE_FORMAT); return 0; }   // (NumberOfRvaAndSizes)
    f->dir_at = dirs + 4 * 8;
    uint32_t d[2];
    if (!rd(f->fd, f->dir_at, d, sizeof d)) { vx_set_last_error(ERROR_BAD_EXE_FORMAT); return 0; }
    f->table = d[0]; f->size = d[1];
    if (f->size && (uint64_t)f->table + f->size > f->file_size) { vx_set_last_error(ERROR_BAD_EXE_FORMAT); return 0; }
    if (!f->size) f->table = 0;
    return 1;
}
static uint32_t align8(uint32_t n) { return (n + 7) & ~7u; }
// The n-th entry: its offset and header (length, revision, type); 0 if there is none.
static int entry(const cert_file *f, DWORD n, uint32_t *at, uint8_t hdr[8]) {
    uint32_t off = 0;
    for (DWORD i = 0;; i++) {
        if (off + 8 > f->size || !rd(f->fd, (uint64_t)f->table + off, hdr, 8)) return 0;
        uint32_t len; memcpy(&len, hdr, 4);
        if (len < 8 || off + len > f->size) return 0;
        if (i == n) { *at = off; return 1; }
        off += align8(len);
    }
}
static int write_dir(const cert_file *f, uint32_t table, uint32_t size) {
    uint32_t d[2] = { size ? table : 0, size };
    return wr(f->fd, f->dir_at, d, sizeof d);
}

static WINAPI BOOL ImageEnumerateCertificates(HANDLE h, uint16_t filter, DWORD *count, DWORD *indices, DWORD index_count) {
    cert_file f;
    if (!open_table(h, &f)) return 0;
    if (!count) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    DWORD found = 0;
    uint8_t hdr[8]; uint32_t at;
    for (DWORD i = 0; entry(&f, i, &at, hdr); i++) {
        uint16_t type; memcpy(&type, hdr + 6, 2);
        if (filter != CERT_SECTION_TYPE_ANY && type != filter) continue;
        if (indices && found < index_count) indices[found] = i;
        found++;
    }
    *count = found;
    return 1;
}
static WINAPI BOOL ImageGetCertificateHeader(HANDLE h, DWORD index, uint8_t *out) {
    cert_file f; uint8_t hdr[8]; uint32_t at;
    if (!open_table(h, &f)) return 0;
    if (!out || !entry(&f, index, &at, hdr)) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    memcpy(out, hdr, 8);
    return 1;
}
static WINAPI BOOL ImageGetCertificateData(HANDLE h, DWORD index, uint8_t *out, DWORD *len) {
    cert_file f; uint8_t hdr[8]; uint32_t at, n;
    if (!open_table(h, &f)) return 0;
    if (!len || !entry(&f, index, &at, hdr)) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    memcpy(&n, hdr, 4);
    if (!out || *len < n) { *len = n; vx_set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    if (!rd(f.fd, (uint64_t)f.table + at, out, n)) { vx_set_last_error(ERROR_INVALID_HANDLE); return 0; }
    *len = n;
    return 1;
}
// Appended after the last entry. A file without a table gets one at its end, on an 8-byte boundary.
static WINAPI BOOL ImageAddCertificate(HANDLE h, const uint8_t *cert, DWORD *index) {
    cert_file f;
    if (!open_table(h, &f)) return 0;
    uint32_t len;
    if (!cert || (memcpy(&len, cert, 4), len < 8)) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    uint32_t table = f.size ? f.table : (uint32_t)((f.file_size + 7) & ~7ull);
    if (f.size && (uint64_t)f.table + align8(f.size) < f.file_size) { vx_set_last_error(ERROR_NOT_SUPPORTED); return 0; }
    DWORD n = 0; uint8_t hdr[8]; uint32_t at;
    while (entry(&f, n, &at, hdr)) n++;
    uint32_t start = align8(f.size), padded = align8(len);
    uint8_t *buf = calloc(1, padded);
    memcpy(buf, cert, len);
    static const uint8_t zeros[8];
    int ok = (f.size || f.file_size == table || wr(f.fd, f.file_size, zeros, (size_t)(table - f.file_size))) &&
             (start == f.size || wr(f.fd, (uint64_t)table + f.size, zeros, start - f.size)) &&
             wr(f.fd, (uint64_t)table + start, buf, padded) && write_dir(&f, table, start + padded);
    free(buf);
    if (!ok) { vx_set_last_error(ERROR_INVALID_HANDLE); return 0; }
    if (index) *index = n;
    return 1;
}
// The entries after it move down; the file ends where the table now does.
static WINAPI BOOL ImageRemoveCertificate(HANDLE h, DWORD index) {
    cert_file f; uint8_t hdr[8]; uint32_t at, len;
    if (!open_table(h, &f)) return 0;
    if (!entry(&f, index, &at, hdr)) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    if ((uint64_t)f.table + align8(f.size) < f.file_size) { vx_set_last_error(ERROR_NOT_SUPPORTED); return 0; }
    memcpy(&len, hdr, 4);
    uint32_t gone = align8(len), next = at + gone < f.size ? at + gone : f.size, rest = f.size - next;
    uint8_t *buf = rest ? malloc(rest) : NULL;
    int ok = (!rest || (rd(f.fd, (uint64_t)f.table + next, buf, rest) && wr(f.fd, (uint64_t)f.table + at, buf, rest)));
    free(buf);
    uint32_t size = at + rest;
    ok = ok && write_dir(&f, f.table, size) && !ftruncate(f.fd, (off_t)(size ? (uint64_t)f.table + size : f.table));
    if (!ok) { vx_set_last_error(ERROR_INVALID_HANDLE); return 0; }
    return 1;
}

static const vx_export imagehlp[] = {
    VX_EXPORT(ImageEnumerateCertificates), VX_EXPORT(ImageGetCertificateHeader), VX_EXPORT(ImageGetCertificateData),
    VX_EXPORT(ImageAddCertificate), VX_EXPORT(ImageRemoveCertificate),
};
void vx_imagehlp_init(void) {
    vx_register_module("imagehlp.dll", imagehlp, sizeof imagehlp / sizeof imagehlp[0]);
}

#ifdef __aarch64__
#include "thunks/imagehlp.c.inc"
#endif
