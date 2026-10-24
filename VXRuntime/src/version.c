// version.dll: a file's version resource and VerQueryValue over it.
//
// GetFileVersionInfoSize/GetFileVersionInfo give the file's RT_VERSION resource, the VS_VERSIONINFO tree Microsoft
// documents (each block: wLength, wValueLength, wType (1 text, 0 binary), the key in UTF-16, padding to 32 bits, the
// value, padding, the children): VS_FIXEDFILEINFO under "\", "\StringFileInfo\<language+code page>\<name>" strings
// and "\VarFileInfo\Translation". The file is read where it is (mapped, not loaded); a name without a folder is first
// a loaded module of that name. The runtime's own DLLs (kernel32.dll and the rest, "C:\windows\system32\<name>" to
// GetModuleFileName) have no file: they answer with a block made here for the Windows version the runtime reports
// (10.0.19045), file type DLL.
// Chromium (Steam's web helper) reads the version of kernel32.dll this way and stops at an int3 when it gets none:
// right after it created Steam's shared JS context, every time.
//
// VerQueryValueA returns strings in the ANSI code page (UTF-8 here): converted into the part of the buffer after the
// block that GetFileVersionInfoSize asks for, at a place of their own for each string.
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wctype.h>
#include "vx.h"
#include "pe.h"

#define ERROR_FILE_NOT_FOUND 2
#define ERROR_INSUFFICIENT_BUFFER 122
#define ERROR_RESOURCE_TYPE_NOT_FOUND 1813
#define CONVERTED_MAGIC 0x56584131u                     // "1AXV": the buffer has room for ANSI strings

int vx_is_builtin_dll(const char *dll);                 // loader.c

static size_t align4(size_t v) { return (v + 3) & ~(size_t)3; }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static void wr16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

// ---- a version block made for one of the runtime's DLLs --------------------------------------------------------------------------
typedef struct { uint8_t *b; size_t n, cap; } vbuf;
static void vb_reserve(vbuf *v, size_t more) {
    if (v->n + more <= v->cap) return;
    v->cap = (v->n + more) * 2;
    v->b = realloc(v->b, v->cap);
}
static void vb_pad(vbuf *v) { vb_reserve(v, 4); while (v->n & 3) v->b[v->n++] = 0; }
// Starts a block: its length is filled in by block_end. Value: `value_bytes` bytes (text: the UTF-16 with its NUL,
// wValueLength then counts characters).
static size_t block_begin(vbuf *v, const char *key, const void *value, size_t value_bytes, int text) {
    vb_pad(v);
    size_t at = v->n, klen = strlen(key);
    vb_reserve(v, 6 + 2 * (klen + 1) + 4 + value_bytes + 4);
    wr16(v->b + at + 2, (uint16_t)(text ? value_bytes / 2 : value_bytes));
    wr16(v->b + at + 4, (uint16_t)(text ? 1 : 0));
    v->n += 6;
    for (size_t i = 0; i <= klen; i++) { wr16(v->b + v->n, (uint8_t)key[i]); v->n += 2; }
    if (value_bytes) { vb_pad(v); memcpy(v->b + v->n, value, value_bytes); v->n += value_bytes; }
    return at;
}
static void block_end(vbuf *v, size_t at) { wr16(v->b + at, (uint16_t)(v->n - at)); }
static void string_entry(vbuf *v, const char *key, const char *text) {
    WCHAR w[128];
    size_t n = vx_utf8_to_utf16(text, strlen(text), w, 127);
    if (n > 127) n = 127;
    w[n] = 0;
    block_end(v, block_begin(v, key, w, 2 * (n + 1), 1));
}
// VS_VERSIONINFO for a runtime DLL `name`: version 10.0.19045.0, VFT_DLL, US English / Unicode strings.
static uint8_t *made_block(const char *name, uint32_t *length) {
    vbuf v = { 0 };
    uint8_t fixed[52] = { 0 };
    wr32(fixed, 0xfeef04bdu); wr32(fixed + 4, 0x00010000u);                       // signature, structure version
    wr32(fixed + 8, 10u << 16); wr32(fixed + 12, 19045u << 16);                   // file version 10.0.19045.0
    wr32(fixed + 16, 10u << 16); wr32(fixed + 20, 19045u << 16);                  // product version
    wr32(fixed + 24, 0x3f); wr32(fixed + 32, 0x00040004u);                        // flags mask; VOS_NT_WINDOWS32
    wr32(fixed + 36, 2);                                                          // VFT_DLL
    size_t root = block_begin(&v, "VS_VERSION_INFO", fixed, sizeof fixed, 0);
    size_t sfi = block_begin(&v, "StringFileInfo", NULL, 0, 1);
    size_t table = block_begin(&v, "040904B0", NULL, 0, 1);
    char internal[64], description[96];
    snprintf(internal, sizeof internal, "%s", name);
    char *dot = strrchr(internal, '.');
    if (dot) *dot = 0;
    snprintf(description, sizeof description, "%s (VXRuntime)", name);
    string_entry(&v, "CompanyName", "VXRuntime");
    string_entry(&v, "FileDescription", description);
    string_entry(&v, "FileVersion", "10.0.19045.0");
    string_entry(&v, "InternalName", internal);
    string_entry(&v, "OriginalFilename", name);
    string_entry(&v, "ProductName", "VXRuntime");
    string_entry(&v, "ProductVersion", "10.0.19045.0");
    block_end(&v, table);
    block_end(&v, sfi);
    size_t vfi = block_begin(&v, "VarFileInfo", NULL, 0, 1);
    uint8_t translation[4]; wr16(translation, 0x0409); wr16(translation + 2, 0x04b0);
    block_end(&v, block_begin(&v, "Translation", translation, 4, 0));
    block_end(&v, vfi);
    block_end(&v, root);
    *length = (uint32_t)v.n;
    return v.b;
}

// ---- finding the file ------------------------------------------------------------------------------------------------------------------
static const char *base_name(const char *path) {
    const char *b = path;
    for (const char *c = path; *c; c++) if (*c == '\\' || *c == '/') b = c + 1;
    return b;
}
// The version block of the file `name` (UTF-8, Windows path or a module name), malloc'd. 0 with the last error set if
// there is none.
static uint8_t *version_block(const char *name, uint32_t *length) {
    char unix_path[4096] = "";
    int found = 0;
    const char *base = base_name(name);
    if (base == name) {                                   // a bare name: a loaded module first
        const vx_image *images[512];
        int n = vx_image_list(images, 512);
        for (int i = 0; i < n && i < 512 && !found; i++)
            if (!strcasecmp(images[i]->name, name) && images[i]->unix_path) { snprintf(unix_path, sizeof unix_path, "%s", images[i]->unix_path); found = 1; }
    }
    struct stat st;
    if (!found && vx_unix_path(name, unix_path, sizeof unix_path) && !stat(unix_path, &st) && S_ISREG(st.st_mode)) found = 1;
    if (!found) {
        // The runtime's own DLLs: by name, or where GetModuleFileName puts them.
        int in_system32 = base == name || (base - name == 20 && !strncasecmp(name, "C:\\windows\\system32\\", 20));
        if (in_system32 && vx_is_builtin_dll(base)) return made_block(base, length);
        vx_set_last_error(ERROR_FILE_NOT_FOUND);
        return NULL;
    }
    int fd = open(unix_path, O_RDONLY);
    if (fd < 0 || fstat(fd, &st) || st.st_size < 64) { if (fd >= 0) close(fd); vx_set_last_error(ERROR_FILE_NOT_FOUND); return NULL; }
    size_t size = (size_t)st.st_size;
    uint8_t *data = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (data == MAP_FAILED) { vx_set_last_error(ERROR_FILE_NOT_FOUND); return NULL; }
    vx_pe_info pe;
    size_t offset; uint32_t len;
    uint8_t *out = NULL;
    if (vx_pe_inspect(data, size, &pe) && vx_pe_version_resource(data, size, &pe, &offset, &len) && len >= 6 &&
        (out = malloc(len))) {
        memcpy(out, data + offset, len);
        // The root block's wLength is the length (the resource can be longer than its root block). A root that says
        // it is longer than the resource is no version information: VerQueryValue finds its way and the room for the
        // ANSI strings by the root's wLength, and the buffer GetFileVersionInfoSize asks for must cover both.
        uint16_t wlength = rd16(out);
        if (wlength >= 6 && wlength <= len) *length = wlength;
        else { free(out); out = NULL; }
    }
    munmap(data, size);
    if (!out) vx_set_last_error(ERROR_RESOURCE_TYPE_NOT_FOUND);
    return out;
}

// What GetFileVersionInfoSize asks for: the block, then twice its length for the ANSI strings (each at 2 x its own
// offset: room for 4 bytes per UTF-16 unit).
static DWORD info_size(uint32_t length) { return (DWORD)(align4(length) + 2 * (size_t)length); }

static DWORD get_size(const char *name, DWORD *handle) {
    if (handle) *handle = 0;
    uint32_t length;
    uint8_t *b = name ? version_block(name, &length) : NULL;
    if (!name) vx_set_last_error(ERROR_FILE_NOT_FOUND);
    if (!b) {
        static int logged;
        if (name && logged < 20) { logged++; vx_log("GetFileVersionInfoSize(%s): no version information (error %u)", name, vx_get_last_error()); }
        return 0;
    }
    free(b);
    vx_set_last_error(0);
    return info_size(length);
}
static BOOL get_info(const char *name, DWORD n, void *out) {
    uint32_t length;
    uint8_t *b = name ? version_block(name, &length) : NULL;
    if (!b) { if (!name) vx_set_last_error(ERROR_FILE_NOT_FOUND); return 0; }
    if (!out || !n) { free(b); vx_set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    memcpy(out, b, n < length ? n : length);              // (a smaller buffer gets the start, as Windows does)
    if (n >= info_size(length)) wr32((uint8_t *)out + align4(length), CONVERTED_MAGIC);
    free(b);
    return 1;
}

static WINAPI DWORD GetFileVersionInfoSizeW(const WCHAR *f, DWORD *h) { char *a = vx_utf8_dup(f); DWORD r = get_size(a, h); free(a); return r; }
static WINAPI DWORD GetFileVersionInfoSizeA(const char *f, DWORD *h) { return get_size(f, h); }
static WINAPI DWORD GetFileVersionInfoSizeExW(DWORD flags, const WCHAR *f, DWORD *h) { (void)flags; return GetFileVersionInfoSizeW(f, h); }
static WINAPI DWORD GetFileVersionInfoSizeExA(DWORD flags, const char *f, DWORD *h) { (void)flags; return get_size(f, h); }
static WINAPI BOOL GetFileVersionInfoW(const WCHAR *f, DWORD h, DWORD n, void *d) { (void)h; char *a = vx_utf8_dup(f); BOOL r = get_info(a, n, d); free(a); return r; }
static WINAPI BOOL GetFileVersionInfoA(const char *f, DWORD h, DWORD n, void *d) { (void)h; return get_info(f, n, d); }
static WINAPI BOOL GetFileVersionInfoExW(DWORD flags, const WCHAR *f, DWORD h, DWORD n, void *d) { (void)flags; return GetFileVersionInfoW(f, h, n, d); }
static WINAPI BOOL GetFileVersionInfoExA(DWORD flags, const char *f, DWORD h, DWORD n, void *d) { (void)flags; (void)h; return get_info(f, n, d); }

// ---- VerQueryValue ---------------------------------------------------------------------------------------------------------------------
// A block as read: value_length is wValueLength as the file has it; value_bytes the value's size in bytes
// (wValueLength characters for text, bytes otherwise) cut to the block's own end. A wrong wValueLength (a byte count
// for text, as some resource tools write, or a malformed file) then never reads or writes past the block: everything
// that uses the value goes by value_bytes.
typedef struct { size_t at, end, value, value_bytes, children; uint16_t value_length, type; } vblock;
static int read_block(const uint8_t *b, size_t at, size_t limit, vblock *out) {
    if (at + 6 > limit) return 0;
    uint16_t length = rd16(b + at);
    if (length < 6 || at + length > limit) return 0;
    size_t end = at + length, c = at + 6;
    while (c + 2 <= end && rd16(b + c)) c += 2;
    if (c + 2 > end) return 0;
    out->at = at; out->end = end;
    out->value_length = rd16(b + at + 2); out->type = rd16(b + at + 4);
    out->value = align4(c + 2);
    size_t bytes = out->type == 1 ? 2 * (size_t)out->value_length : out->value_length;
    if (out->value > end) { out->value = end; bytes = 0; }
    if (bytes > end - out->value) bytes = end - out->value;
    out->value_bytes = bytes;
    out->children = align4(out->value + bytes);
    if (out->children > end) out->children = end;
    return 1;
}
// Whether block k's key equals the path part [p, q) (UTF-8; compared without case, as VerQueryValue does).
static int key_is(const uint8_t *b, const vblock *k, const char *p, const char *q) {
    WCHAR want[128];
    size_t n = vx_utf8_to_utf16(p, (size_t)(q - p), want, 127);
    size_t c = k->at + 6;
    for (size_t i = 0; i < n; i++, c += 2) if (c + 2 > k->end || towupper(rd16(b + c)) != towupper(want[i])) return 0;
    return c + 2 <= k->end && rd16(b + c) == 0;
}
// The block a sub-block path names ("\" the root; "\StringFileInfo\040904B0\FileVersion"), 0 if none.
static int find_path(const uint8_t *b, const char *path, vblock *out) {
    vblock cur;
    if (!read_block(b, 0, rd16(b), &cur)) return 0;
    const char *p = path;
    for (;;) {
        while (*p == '\\' || *p == '/') p++;
        if (!*p) { *out = cur; return 1; }
        const char *q = p;
        while (*q && *q != '\\' && *q != '/') q++;
        int found = 0;
        for (size_t at = cur.children; at + 6 <= cur.end;) {
            vblock child;
            if (!read_block(b, at, cur.end, &child)) break;
            if (key_is(b, &child, p, q)) { cur = child; found = 1; break; }
            at = align4(child.end);
        }
        if (!found) return 0;
        p = q;
    }
}
static BOOL query(const void *block, const char *sub, void **out, DWORD *n, int ansi) {
    const uint8_t *b = block;
    vblock k;
    if (out) *out = NULL;
    if (n) *n = 0;
    if (!b || !sub || !out || !find_path(b, sub, &k)) { vx_set_last_error(ERROR_RESOURCE_TYPE_NOT_FOUND); return 0; }
    // The length reported: bytes for binary values (VS_FIXEDFILEINFO, Translation), characters for text; what the
    // block holds, not more (see vblock).
    size_t length = k.type == 1 ? k.value_bytes / 2 : k.value_bytes;
    if (!k.value_length || !length) { vx_set_last_error(ERROR_RESOURCE_TYPE_NOT_FOUND); return 0; }
    if (k.type != 1 || !ansi) {
        *out = (void *)(b + k.value);
        if (n) *n = (DWORD)length;
        return 1;
    }
    size_t area = align4(rd16(b));
    uint32_t magic;
    memcpy(&magic, b + area, 4);
    if (magic != CONVERTED_MAGIC) { vx_set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    // This value's room in the area: twice its bytes in the block, from twice its offset. The value lies inside the
    // root block (k.value + k.value_bytes <= the root's wLength), so the room lies inside the 2 x wLength bytes
    // GetFileVersionInfoSize asked for after the block, and no two values' rooms overlap; the magic is before the
    // first (k.value >= 8). 4 bytes per UTF-16 unit, of which UTF-8 needs at most 3: the NUL always fits.
    char *s = (char *)b + area + 2 * k.value;
    size_t room = 2 * k.value_bytes, units = k.value_bytes / 2;
    while (units && rd16(b + k.value + 2 * (units - 1)) == 0) units--;
    WCHAR *w = malloc(2 * (units + 1));
    if (!w) { vx_set_last_error(8 /* ERROR_NOT_ENOUGH_MEMORY */); return 0; }
    for (size_t i = 0; i < units; i++) w[i] = rd16(b + k.value + 2 * i);
    size_t len = vx_utf16_to_utf8(w, units, s, room - 1);
    if (len > room - 1) len = room - 1;
    s[len] = 0;
    free(w);
    *out = s;
    if (n) *n = (DWORD)len + 1;
    return 1;
}
static WINAPI BOOL VerQueryValueW(const void *b, const WCHAR *s, void **out, DWORD *n) {
    char *a = vx_utf8_dup(s); BOOL r = query(b, a, out, n, 0); free(a); return r;
}
static WINAPI BOOL VerQueryValueA(const void *b, const char *s, void **out, DWORD *n) { return query(b, s, out, n, 1); }

static const vx_export version[] = {
    VX_EXPORT(GetFileVersionInfoSizeA), VX_EXPORT(GetFileVersionInfoSizeW), VX_EXPORT(GetFileVersionInfoSizeExA), VX_EXPORT(GetFileVersionInfoSizeExW),
    VX_EXPORT(GetFileVersionInfoA), VX_EXPORT(GetFileVersionInfoW), VX_EXPORT(GetFileVersionInfoExA), VX_EXPORT(GetFileVersionInfoExW),
    VX_EXPORT(VerQueryValueA), VX_EXPORT(VerQueryValueW),
};
void vx_version_init(void) { vx_register_module("version.dll", version, sizeof version / sizeof version[0]); }

#ifdef __aarch64__
#include "thunks/version.c.inc"
#endif
