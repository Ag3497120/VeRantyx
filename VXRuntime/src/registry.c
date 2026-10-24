// The registry (advapi32): a tree of keys and values kept in memory and saved to $VXRT_ROOT/registry.txt after
// every change. Key and value names compare case-insensitively (ASCII), like Windows. String values are stored
// as UTF-16 (what the W functions see); the A functions convert to and from UTF-8.
//
// registry.txt has one line per key and per value, meant for the runtime rather than people:
//   key HKEY_CURRENT_USER\Software\Example
//   value <type> <name in hex or -> <data in hex or ->
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "vx.h"

#define ERROR_SUCCESS        0
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_ACCESS_DENIED  5
#define ERROR_INVALID_HANDLE 6
#define ERROR_INVALID_PARAMETER 87
#define ERROR_MORE_DATA      234
#define ERROR_NO_MORE_ITEMS  259
enum { REG_SZ = 1, REG_EXPAND_SZ = 2, REG_BINARY = 3, REG_DWORD = 4, REG_MULTI_SZ = 7, REG_QWORD = 11 };
static int is_string(DWORD type) { return type == REG_SZ || type == REG_EXPAND_SZ || type == REG_MULTI_SZ; }

typedef struct reg_value { char *name; DWORD type, size; uint8_t *data; struct reg_value *next; } reg_value;
typedef struct reg_key { char *name; struct reg_key *parent, *children, *next; reg_value *values; int deleted; } reg_key;

static const char *g_root_names[] = { "HKEY_CLASSES_ROOT", "HKEY_CURRENT_USER", "HKEY_LOCAL_MACHINE", "HKEY_USERS",
                                      "HKEY_PERFORMANCE_DATA", "HKEY_CURRENT_CONFIG" };
#define NROOTS 6
static reg_key g_roots[NROOTS];
static pthread_mutex_t g_reg_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
static int g_loaded;

// An open key is a heap object pointing at its node; predefined keys are 0x80000000 + n (sign-extended).
typedef struct { uint32_t magic; reg_key *key; } reg_handle;
#define REG_MAGIC 0x56584b59u

static reg_key *key_of(void *hkey) {
    uint64_t v = (uint64_t)hkey;
    if ((v & 0xffffffff00000000ull) == 0xffffffff00000000ull || v < 0x100000000ull) {
        uint32_t low = (uint32_t)v;
        if (low >= 0x80000000u && low < 0x80000000u + NROOTS) return &g_roots[low - 0x80000000u];
        if (v < 0x10000) return NULL;
    }
    reg_handle *h = hkey;
    return h && h->magic == REG_MAGIC ? h->key : NULL;
}
static void *new_hkey(reg_key *k) { reg_handle *h = malloc(sizeof *h); h->magic = REG_MAGIC; h->key = k; return h; }

// Change notifications (RegNotifyChangeKeyValue, src/regnotify.c), told with g_reg_lock held: a key's subkeys
// (REG_NOTIFY_CHANGE_NAME) or values (_LAST_SET) changed, a key was deleted, a key handle is being closed.
void vx_regnotify_changed(const void *key, DWORD what);
void vx_regnotify_closed(const void *hkey);
enum { CHANGED_NAME = 1, CHANGED_VALUES = 4, CHANGED_DELETED = 0x80000000u };
static void load(void);
// For regnotify.c: the key a handle stands for (loading the registry first), whether it was deleted; and whether a
// key is another or below it (g_reg_lock held, as it is while vx_regnotify_changed runs).
const void *vx_reg_node(void *hkey, int *deleted) {
    pthread_mutex_lock(&g_reg_lock);
    load();
    reg_key *k = key_of(hkey);
    if (k && deleted) *deleted = k->deleted;
    pthread_mutex_unlock(&g_reg_lock);
    return k;
}
int vx_reg_within(const void *node, const void *ancestor) {
    for (const reg_key *k = node; k; k = k->parent) if (k == ancestor) return 1;
    return 0;
}

static reg_key *child(reg_key *k, const char *name, size_t n, int create) {
    reg_key **link = &k->children;
    for (; *link; link = &(*link)->next) {
        int c = strncasecmp((*link)->name, name, n);
        if (!c && !(*link)->name[n]) return *link;
        if (c > 0) break;                                    // children are kept sorted
    }
    if (!create) return NULL;
    reg_key *c = calloc(1, sizeof *c);
    c->name = strndup(name, n);
    c->parent = k;
    c->next = *link;
    *link = c;
    vx_regnotify_changed(k, CHANGED_NAME);
    return c;
}
static reg_key *walk(reg_key *k, const char *path, int create) {
    while (k && path && *path) {
        while (*path == '\\') path++;
        if (!*path) break;
        const char *e = strchr(path, '\\');
        size_t n = e ? (size_t)(e - path) : strlen(path);
        k = child(k, path, n, create);
        path += n;
    }
    return k;
}
static reg_value **find_value(reg_key *k, const char *name) {
    reg_value **v = &k->values;
    for (; *v; v = &(*v)->next) if (!strcasecmp((*v)->name, name ? name : "")) break;
    return v;
}

// ---- persistence ----------------------------------------------------------------------------------------------------
static void put_hex(FILE *f, const uint8_t *p, size_t n) {
    if (!n) { fputc('-', f); return; }
    for (size_t i = 0; i < n; i++) fprintf(f, "%02x", p[i]);
}
static size_t get_hex(const char *s, uint8_t *out) {
    size_t n = 0;
    if (*s == '-') return 0;
    for (; s[0] && s[1] && s[0] != ' ' && s[0] != '\n'; s += 2) { unsigned b; sscanf(s, "%2x", &b); out[n++] = (uint8_t)b; }
    return n;
}
static void save_key(FILE *f, reg_key *k, char *path, size_t len) {
    if (k->values || !k->children) {
        fprintf(f, "key %s\n", path);
        for (reg_value *v = k->values; v; v = v->next) {
            fprintf(f, "value %u ", v->type);
            put_hex(f, (const uint8_t *)v->name, strlen(v->name));
            fputc(' ', f);
            put_hex(f, v->data, v->size);
            fputc('\n', f);
        }
    }
    for (reg_key *c = k->children; c; c = c->next) {
        size_t n = snprintf(path + len, 4096 - len, "\\%s", c->name);
        save_key(f, c, path, len + n);
        path[len] = 0;
    }
}
static void save(void) {
    char file[4200], tmp[4210];
    snprintf(file, sizeof file, "%s/registry.txt", vx_root());
    snprintf(tmp, sizeof tmp, "%s.tmp", file);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    char path[4096];
    for (int i = 0; i < NROOTS; i++)
        for (reg_key *c = g_roots[i].children; c; c = c->next) {
            size_t n = snprintf(path, sizeof path, "%s\\%s", g_root_names[i], c->name);
            save_key(f, c, path, n);
        }
    fclose(f);
    rename(tmp, file);
}

static void set_value(reg_key *k, const char *name, DWORD type, const void *data, DWORD size);
static void defaults(void) {
    reg_key *nt = walk(&g_roots[2], "Software\\Microsoft\\Windows NT\\CurrentVersion", 1);
    struct { const char *name, *text; } s[] = { { "ProductName", "Windows 10 Pro" }, { "CurrentBuild", "19045" },
                                                { "CurrentBuildNumber", "19045" }, { "CurrentVersion", "6.3" } };
    for (size_t i = 0; i < sizeof s / sizeof s[0]; i++) {
        WCHAR *w = vx_utf16_dup(s[i].text);
        set_value(nt, s[i].name, REG_SZ, w, (DWORD)(vx_wcslen(w) + 1) * 2);
        free(w);
    }
    DWORD major = 10, minor = 0;
    set_value(nt, "CurrentMajorVersionNumber", REG_DWORD, &major, 4);
    set_value(nt, "CurrentMinorVersionNumber", REG_DWORD, &minor, 4);
}

// HKLM\SOFTWARE\Microsoft\Cryptography\MachineGuid: the installation's own id, which Windows makes once at setup and
// programs read to tell machines apart (Frontier's launcher hashes it). Made here once per VXRT_ROOT (a random UUID,
// kept in registry.txt), also in a registry.txt made before this existed.
static void ensure_machine_guid(void) {
    reg_key *crypto = walk(&g_roots[2], "Software\\Microsoft\\Cryptography", 1);
    reg_value *existing = *find_value(crypto, "MachineGuid");
    if (existing) return;
    uint8_t b[16];
    arc4random_buf(b, sizeof b);
    b[6] = (uint8_t)((b[6] & 0x0f) | 0x40);                        // version 4
    b[8] = (uint8_t)((b[8] & 0x3f) | 0x80);
    char text[40];
    snprintf(text, sizeof text, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    WCHAR *w = vx_utf16_dup(text);
    set_value(crypto, "MachineGuid", REG_SZ, w, (DWORD)(vx_wcslen(w) + 1) * 2);
    free(w);
}

void vx_steam_registry(void);   // steambridge.c
// A value set while the registry loads (the lock is held): the path starts with a root's full name.
void vx_reg_put(const char *path, const char *name, DWORD type, const void *data, DWORD size) {
    for (int i = 0; i < NROOTS; i++) {
        size_t n = strlen(g_root_names[i]);
        if (!strncmp(path, g_root_names[i], n) && path[n] == '\\') { set_value(walk(&g_roots[i], path + n + 1, 1), name, type, data, size); return; }
    }
}
static void load(void) {
    if (g_loaded) return;
    g_loaded = 1;
    for (int i = 0; i < NROOTS; i++) g_roots[i].name = (char *)g_root_names[i];
    char file[4200];
    snprintf(file, sizeof file, "%s/registry.txt", vx_root());
    FILE *f = fopen(file, "r");
    if (!f) { defaults(); ensure_machine_guid(); vx_steam_registry(); save(); return; }
    size_t cap = 1 << 16;
    char *line = malloc(cap);
    uint8_t *buf = malloc(cap);
    reg_key *k = NULL;
    while (fgets(line, (int)cap, f)) {
        line[strcspn(line, "\n")] = 0;
        if (!strncmp(line, "key ", 4)) {
            char *path = line + 4, *sep = strchr(path, '\\');
            k = NULL;
            for (int i = 0; i < NROOTS; i++)
                if (!strncmp(path, g_root_names[i], sep ? (size_t)(sep - path) : strlen(path)) && strlen(g_root_names[i]) == (sep ? (size_t)(sep - path) : strlen(path)))
                    k = walk(&g_roots[i], sep, 1);
        } else if (!strncmp(line, "value ", 6) && k) {
            unsigned type; char *p = line + 6;
            if (sscanf(p, "%u", &type) != 1) continue;
            p = strchr(p, ' ');
            if (!p) continue;
            size_t nlen = get_hex(++p, buf);
            char *name = strndup((char *)buf, nlen);
            p = strchr(p, ' ');
            size_t dlen = p ? get_hex(p + 1, buf) : 0;
            set_value(k, name, type, buf, (DWORD)dlen);
            free(name);
        }
    }
    free(line); free(buf);
    fclose(f);
    reg_value *had = *find_value(walk(&g_roots[2], "Software\\Microsoft\\Cryptography", 1), "MachineGuid");
    ensure_machine_guid();
    vx_steam_registry();
    if (!had) save();                                              // (a registry.txt from before the id existed)
}

static void set_value(reg_key *k, const char *name, DWORD type, const void *data, DWORD size) {
    reg_value **slot = find_value(k, name), *v = *slot;
    if (!v) { v = calloc(1, sizeof *v); v->name = strdup(name ? name : ""); *slot = v; }
    free(v->data);
    v->type = type; v->size = size;
    v->data = malloc(size ? size : 1);
    if (size) memcpy(v->data, data, size);
}

// ---- opening and creating keys ----------------------------------------------------------------------------------------------
static LONG open_key(void *parent, const char *sub, void **out, int create, DWORD *disposition) {
    pthread_mutex_lock(&g_reg_lock);
    load();
    reg_key *p = key_of(parent), *k = NULL;
    LONG r = ERROR_INVALID_HANDLE;
    if (p) {
        int existed = walk(p, sub, 0) != NULL;
        k = walk(p, sub, create);
        if (k) { *out = new_hkey(k); r = ERROR_SUCCESS; if (create && !existed) save(); }
        else r = ERROR_FILE_NOT_FOUND;
        if (disposition) *disposition = existed ? 2 /* REG_OPENED_EXISTING_KEY */ : 1 /* REG_CREATED_NEW_KEY */;
    }
    pthread_mutex_unlock(&g_reg_lock);
    return r;
}
static WINAPI LONG RegOpenKeyExA(void *hkey, const char *sub, DWORD opts, DWORD sam, void **out) { (void)opts; (void)sam; return open_key(hkey, sub, out, 0, NULL); }
static WINAPI LONG RegOpenKeyExW(void *hkey, const WCHAR *sub, DWORD opts, DWORD sam, void **out) {
    char *a = vx_utf8_dup(sub); LONG r = open_key(hkey, a, out, 0, NULL); free(a); return r;
}
static WINAPI LONG RegOpenKeyA(void *hkey, const char *sub, void **out) { return open_key(hkey, sub, out, 0, NULL); }
static WINAPI LONG RegOpenKeyW(void *hkey, const WCHAR *sub, void **out) { return RegOpenKeyExW(hkey, sub, 0, 0, out); }
static WINAPI LONG RegCreateKeyExA(void *hkey, const char *sub, DWORD reserved, char *cls, DWORD opts, DWORD sam, void *sa, void **out, DWORD *disp) {
    (void)reserved; (void)cls; (void)opts; (void)sam; (void)sa;
    return open_key(hkey, sub, out, 1, disp);
}
static WINAPI LONG RegCreateKeyExW(void *hkey, const WCHAR *sub, DWORD reserved, WCHAR *cls, DWORD opts, DWORD sam, void *sa, void **out, DWORD *disp) {
    (void)reserved; (void)cls; (void)opts; (void)sam; (void)sa;
    char *a = vx_utf8_dup(sub); LONG r = open_key(hkey, a, out, 1, disp); free(a); return r;
}
static WINAPI LONG RegCreateKeyA(void *hkey, const char *sub, void **out) { return open_key(hkey, sub, out, 1, NULL); }
static WINAPI LONG RegCreateKeyW(void *hkey, const WCHAR *sub, void **out) { return RegCreateKeyExW(hkey, sub, 0, NULL, 0, 0, NULL, out, NULL); }
static WINAPI LONG RegCloseKey(void *hkey) {
    uint32_t low = (uint32_t)(uint64_t)hkey;
    if (low >= 0x80000000u && low < 0x80000000u + NROOTS) return ERROR_SUCCESS;
    if (!key_of(hkey)) return ERROR_INVALID_HANDLE;
    pthread_mutex_lock(&g_reg_lock);
    vx_regnotify_closed(hkey);                                     // (its pending notifications are signalled)
    pthread_mutex_unlock(&g_reg_lock);
    ((reg_handle *)hkey)->magic = 0;
    free(hkey);
    return ERROR_SUCCESS;
}
static WINAPI LONG RegFlushKey(void *hkey) { (void)hkey; return ERROR_SUCCESS; }

// Deleted keys are unlinked but not freed: open handles may still point at them.
static LONG delete_key(void *hkey, const char *sub) {
    pthread_mutex_lock(&g_reg_lock);
    load();
    reg_key *p = key_of(hkey), *k = p ? walk(p, sub, 0) : NULL;
    LONG r = !p ? ERROR_INVALID_HANDLE : !k ? ERROR_FILE_NOT_FOUND : (k->children || !k->parent) ? ERROR_ACCESS_DENIED : ERROR_SUCCESS;
    if (r == ERROR_SUCCESS) {
        reg_key **link = &k->parent->children;
        while (*link != k) link = &(*link)->next;
        *link = k->next;
        k->deleted = 1;
        vx_regnotify_changed(k, CHANGED_DELETED);
        vx_regnotify_changed(k->parent, CHANGED_NAME);
        save();
    }
    pthread_mutex_unlock(&g_reg_lock);
    return r;
}
static WINAPI LONG RegDeleteKeyA(void *hkey, const char *sub) { return delete_key(hkey, sub); }
static WINAPI LONG RegDeleteKeyW(void *hkey, const WCHAR *sub) { char *a = vx_utf8_dup(sub); LONG r = delete_key(hkey, a); free(a); return r; }

// ---- values ---------------------------------------------------------------------------------------------------------------------
static LONG set(void *hkey, const char *name, DWORD type, const void *data, DWORD size, int ansi) {
    pthread_mutex_lock(&g_reg_lock);
    load();
    reg_key *k = key_of(hkey);
    if (k) {
        if (ansi && is_string(type)) {
            // UTF-8 in, UTF-16 stored (MULTI_SZ keeps its embedded terminators: convert the whole byte range).
            size_t n = vx_utf8_to_utf16(data, size, NULL, 0);
            WCHAR *w = malloc((n + 1) * sizeof(WCHAR));
            vx_utf8_to_utf16(data, size, w, n);
            set_value(k, name, type, w, (DWORD)(n * sizeof(WCHAR)));
            free(w);
        } else {
            set_value(k, name, type, data, size);
        }
        vx_regnotify_changed(k, CHANGED_VALUES);
        save();
    }
    pthread_mutex_unlock(&g_reg_lock);
    return k ? ERROR_SUCCESS : ERROR_INVALID_HANDLE;
}
static WINAPI LONG RegSetValueExA(void *hkey, const char *name, DWORD reserved, DWORD type, const uint8_t *data, DWORD size) {
    (void)reserved; return set(hkey, name, type, data, size, 1);
}
static WINAPI LONG RegSetValueExW(void *hkey, const WCHAR *name, DWORD reserved, DWORD type, const uint8_t *data, DWORD size) {
    (void)reserved; char *a = vx_utf8_dup(name); LONG r = set(hkey, a, type, data, size, 0); free(a); return r;
}

// Copies a value out in the Windows way: size only when data is NULL; ERROR_MORE_DATA with the needed size when
// the buffer is too small.
static LONG copy_out(const reg_value *v, DWORD *type, uint8_t *data, DWORD *size, int ansi) {
    if (type) *type = v->type;
    const uint8_t *src = v->data;
    DWORD need = v->size;
    char *conv = NULL;
    if (ansi && is_string(v->type)) {
        size_t n = vx_utf16_to_utf8((const WCHAR *)v->data, v->size / 2, NULL, 0);
        conv = malloc(n + 1);
        vx_utf16_to_utf8((const WCHAR *)v->data, v->size / 2, conv, n);
        src = (const uint8_t *)conv; need = (DWORD)n;
    }
    LONG r = ERROR_SUCCESS;
    if (size) {
        if (data && *size < need) r = ERROR_MORE_DATA;
        else if (data) memcpy(data, src, need);
        *size = need;
    } else if (data) {
        r = ERROR_INVALID_PARAMETER;
    }
    free(conv);
    return r;
}
static LONG query(void *hkey, const char *name, DWORD *type, uint8_t *data, DWORD *size, int ansi) {
    pthread_mutex_lock(&g_reg_lock);
    load();
    reg_key *k = key_of(hkey);
    reg_value *v = k ? *find_value(k, name) : NULL;
    LONG r = !k ? ERROR_INVALID_HANDLE : !v ? ERROR_FILE_NOT_FOUND : copy_out(v, type, data, size, ansi);
    pthread_mutex_unlock(&g_reg_lock);
    return r;
}
static WINAPI LONG RegQueryValueExA(void *hkey, const char *name, DWORD *reserved, DWORD *type, uint8_t *data, DWORD *size) {
    (void)reserved; return query(hkey, name, type, data, size, 1);
}
static WINAPI LONG RegQueryValueExW(void *hkey, const WCHAR *name, DWORD *reserved, DWORD *type, uint8_t *data, DWORD *size) {
    (void)reserved; char *a = vx_utf8_dup(name); LONG r = query(hkey, a, type, data, size, 0); free(a); return r;
}

// RegGetValue: open a subkey, query, filter by type (RRF_RT_* bits: 1 << type, 0xffff any).
static LONG get_value(void *hkey, const char *sub, const char *name, DWORD flags, DWORD *type, uint8_t *data, DWORD *size, int ansi) {
    void *k = hkey;
    if (sub && *sub) { LONG r = open_key(hkey, sub, &k, 0, NULL); if (r) return r; }
    DWORD t = 0;
    LONG r = query(k, name, &t, data, size, ansi);
    if (k != hkey) RegCloseKey(k);
    if (r == ERROR_SUCCESS || r == ERROR_MORE_DATA) {
        DWORD mask = flags & 0xffff;
        if (mask != 0xffff && !(mask & (1u << t))) return 1630;   // ERROR_UNSUPPORTED_TYPE
        if (type) *type = t;
    }
    return r;
}
static WINAPI LONG RegGetValueA(void *hkey, const char *sub, const char *name, DWORD flags, DWORD *type, void *data, DWORD *size) {
    return get_value(hkey, sub, name, flags, type, data, size, 1);
}
static WINAPI LONG RegGetValueW(void *hkey, const WCHAR *sub, const WCHAR *name, DWORD flags, DWORD *type, void *data, DWORD *size) {
    char *s = vx_utf8_dup(sub), *n = vx_utf8_dup(name);
    LONG r = get_value(hkey, s, n, flags, type, data, size, 0);
    free(s); free(n);
    return r;
}

static LONG delete_value(void *hkey, const char *name) {
    pthread_mutex_lock(&g_reg_lock);
    load();
    reg_key *k = key_of(hkey);
    reg_value **slot = k ? find_value(k, name) : NULL, *v = slot ? *slot : NULL;
    if (v) { *slot = v->next; free(v->name); free(v->data); free(v); vx_regnotify_changed(k, CHANGED_VALUES); save(); }
    pthread_mutex_unlock(&g_reg_lock);
    return !k ? ERROR_INVALID_HANDLE : !v ? ERROR_FILE_NOT_FOUND : ERROR_SUCCESS;
}
static WINAPI LONG RegDeleteValueA(void *hkey, const char *name) { return delete_value(hkey, name); }
static WINAPI LONG RegDeleteValueW(void *hkey, const WCHAR *name) { char *a = vx_utf8_dup(name); LONG r = delete_value(hkey, a); free(a); return r; }

// ---- enumeration --------------------------------------------------------------------------------------------------------------
// Copies a name into an A or W buffer of *cch characters; *cch becomes the length without the terminator.
static LONG name_out(const char *name, void *buf, DWORD *cch, int ansi) {
    size_t need = ansi ? strlen(name) : vx_utf8_to_utf16(name, strlen(name), NULL, 0);
    if (!buf || *cch < need + 1) { *cch = (DWORD)need; return ERROR_MORE_DATA; }
    if (ansi) memcpy(buf, name, need + 1);
    else { vx_utf8_to_utf16(name, strlen(name), buf, need); ((WCHAR *)buf)[need] = 0; }
    *cch = (DWORD)need;
    return ERROR_SUCCESS;
}
static LONG enum_key(void *hkey, DWORD index, void *name, DWORD *cch, int ansi) {
    pthread_mutex_lock(&g_reg_lock);
    load();
    reg_key *k = key_of(hkey), *c = k ? k->children : NULL;
    for (DWORD i = 0; c && i < index; i++) c = c->next;
    LONG r = !k ? ERROR_INVALID_HANDLE : !c ? ERROR_NO_MORE_ITEMS : name_out(c->name, name, cch, ansi);
    pthread_mutex_unlock(&g_reg_lock);
    return r;
}
static WINAPI LONG RegEnumKeyExA(void *hkey, DWORD index, char *name, DWORD *cch, DWORD *res, char *cls, DWORD *ccls, void *t) {
    (void)res; (void)t; if (ccls) *ccls = 0; if (cls) *cls = 0;
    return enum_key(hkey, index, name, cch, 1);
}
static WINAPI LONG RegEnumKeyExW(void *hkey, DWORD index, WCHAR *name, DWORD *cch, DWORD *res, WCHAR *cls, DWORD *ccls, void *t) {
    (void)res; (void)t; if (ccls) *ccls = 0; if (cls) *cls = 0;
    return enum_key(hkey, index, name, cch, 0);
}
static LONG enum_value(void *hkey, DWORD index, void *name, DWORD *cch, DWORD *type, uint8_t *data, DWORD *size, int ansi) {
    pthread_mutex_lock(&g_reg_lock);
    load();
    reg_key *k = key_of(hkey);
    reg_value *v = k ? k->values : NULL;
    for (DWORD i = 0; v && i < index; i++) v = v->next;
    LONG r = !k ? ERROR_INVALID_HANDLE : !v ? ERROR_NO_MORE_ITEMS : name_out(v->name, name, cch, ansi);
    if (r == ERROR_SUCCESS) r = copy_out(v, type, data, size, ansi);
    pthread_mutex_unlock(&g_reg_lock);
    return r;
}
static WINAPI LONG RegEnumValueA(void *hkey, DWORD index, char *name, DWORD *cch, DWORD *res, DWORD *type, uint8_t *data, DWORD *size) {
    (void)res; return enum_value(hkey, index, name, cch, type, data, size, 1);
}
static WINAPI LONG RegEnumValueW(void *hkey, DWORD index, WCHAR *name, DWORD *cch, DWORD *res, DWORD *type, uint8_t *data, DWORD *size) {
    (void)res; return enum_value(hkey, index, name, cch, type, data, size, 0);
}
static LONG query_info(void *hkey, DWORD *subkeys, DWORD *max_subkey, DWORD *values, DWORD *max_name, DWORD *max_data) {
    pthread_mutex_lock(&g_reg_lock);
    load();
    reg_key *k = key_of(hkey);
    DWORD ns = 0, ms = 0, nv = 0, mn = 0, md = 0;
    if (k) {
        for (reg_key *c = k->children; c; c = c->next) { ns++; if (strlen(c->name) > ms) ms = (DWORD)strlen(c->name); }
        for (reg_value *v = k->values; v; v = v->next) { nv++; if (strlen(v->name) > mn) mn = (DWORD)strlen(v->name); if (v->size > md) md = v->size; }
    }
    pthread_mutex_unlock(&g_reg_lock);
    if (subkeys) *subkeys = ns;
    if (max_subkey) *max_subkey = ms;
    if (values) *values = nv;
    if (max_name) *max_name = mn;
    if (max_data) *max_data = md;
    return k ? ERROR_SUCCESS : ERROR_INVALID_HANDLE;
}
static WINAPI LONG RegQueryInfoKeyA(void *hkey, char *cls, DWORD *ccls, DWORD *res, DWORD *subkeys, DWORD *max_subkey, DWORD *max_class,
                                    DWORD *values, DWORD *max_name, DWORD *max_data, DWORD *sd, void *t) {
    (void)res; (void)sd; (void)t;
    if (cls && ccls && *ccls) *cls = 0;
    if (ccls) *ccls = 0;
    if (max_class) *max_class = 0;
    return query_info(hkey, subkeys, max_subkey, values, max_name, max_data);
}
static WINAPI LONG RegQueryInfoKeyW(void *hkey, WCHAR *cls, DWORD *ccls, DWORD *res, DWORD *subkeys, DWORD *max_subkey, DWORD *max_class,
                                    DWORD *values, DWORD *max_name, DWORD *max_data, DWORD *sd, void *t) {
    (void)res; (void)sd; (void)t;
    if (cls && ccls && *ccls) *cls = 0;
    if (ccls) *ccls = 0;
    if (max_class) *max_class = 0;
    return query_info(hkey, subkeys, max_subkey, values, max_name, max_data);
}

// ---- the native registry calls (ntdll), as Chromium's early startup code makes them -------------------------------------------
// Paths are "\Registry\Machine\...", "\Registry\User\<sid>\..." (the current user), or relative to an open key.
typedef struct { uint16_t length, max; WCHAR *buffer; } UNICODE_STRING;
typedef struct { uint32_t length; void *root; UNICODE_STRING *name; uint32_t attributes; void *sd, *qos; } OBJECT_ATTRIBUTES;
#define STATUS_SUCCESS 0
#define STATUS_BUFFER_OVERFLOW 0x80000005u
#define STATUS_BUFFER_TOO_SMALL 0xC0000023u
#define STATUS_OBJECT_NAME_NOT_FOUND 0xC0000034u
#define STATUS_INVALID_HANDLE 0xC0000008u
#define STATUS_INVALID_PARAMETER 0xC000000Du
static const char g_user_key_path[] = "\\Registry\\User\\S-1-5-21-1000-1000-1000-1001";
static char *us_utf8(const UNICODE_STRING *u) {
    if (!u || !u->buffer) return strdup("");
    size_t n = u->length / 2;
    char *out = malloc(n * 3 + 1);
    size_t w = vx_utf16_to_utf8(u->buffer, n, out, n * 3);
    out[w] = 0;
    return out;
}
static DWORD status_of(LONG r) { return r == ERROR_SUCCESS ? STATUS_SUCCESS : r == ERROR_FILE_NOT_FOUND ? STATUS_OBJECT_NAME_NOT_FOUND : r == ERROR_INVALID_HANDLE ? STATUS_INVALID_HANDLE : STATUS_INVALID_PARAMETER; }
static DWORD nt_open(void **out, const OBJECT_ATTRIBUTES *oa, int create, DWORD *disposition) {
    if (!out || !oa) return STATUS_INVALID_PARAMETER;
    char *path = us_utf8(oa->name), *sub = path;
    void *parent = oa->root;
    if (!parent) {
        size_t un = strlen(g_user_key_path);
        if (!strncasecmp(path, g_user_key_path, un)) { parent = (void *)(intptr_t)(int32_t)0x80000001; sub = path + un; }
        else if (!strncasecmp(path, "\\Registry\\Machine", 17)) { parent = (void *)(intptr_t)(int32_t)0x80000002; sub = path + 17; }
        else if (!strncasecmp(path, "\\Registry\\User", 14)) { parent = (void *)(intptr_t)(int32_t)0x80000003; sub = path + 14; }
        else { free(path); return STATUS_OBJECT_NAME_NOT_FOUND; }
        while (*sub == '\\') sub++;
    }
    LONG r = open_key(parent, sub, out, create, disposition);
    free(path);
    return status_of(r);
}
static WINAPI DWORD NtOpenKeyEx(void **out, DWORD access, const OBJECT_ATTRIBUTES *oa, DWORD options) { (void)access; (void)options; return nt_open(out, oa, 0, NULL); }
static WINAPI DWORD NtOpenKey(void **out, DWORD access, const OBJECT_ATTRIBUTES *oa) { (void)access; return nt_open(out, oa, 0, NULL); }
static WINAPI DWORD NtCreateKey(void **out, DWORD access, const OBJECT_ATTRIBUTES *oa, DWORD title, const UNICODE_STRING *cls, DWORD options, DWORD *disposition) {
    (void)access; (void)title; (void)cls; (void)options;
    return nt_open(out, oa, 1, disposition);
}
static WINAPI DWORD NtDeleteKey(void *hkey) {
    pthread_mutex_lock(&g_reg_lock);
    reg_key *k = key_of(hkey);
    DWORD st = STATUS_INVALID_HANDLE;
    if (k && k->parent) {                                          // unlinked, as delete_key does
        for (reg_key **c = &k->parent->children; *c; c = &(*c)->next) if (*c == k) { *c = k->next; break; }
        if (!k->deleted) { k->deleted = 1; vx_regnotify_changed(k, CHANGED_DELETED); vx_regnotify_changed(k->parent, CHANGED_NAME); }
        save();
        st = STATUS_SUCCESS;
    }
    pthread_mutex_unlock(&g_reg_lock);
    return st;
}
static WINAPI DWORD NtSetValueKey(void *hkey, const UNICODE_STRING *name, DWORD title, DWORD type, const void *data, DWORD size) {
    (void)title;
    char *n = us_utf8(name);
    LONG r = set(hkey, n, type, data, size, 0);
    free(n);
    return status_of(r);
}
static WINAPI DWORD NtDeleteValueKey(void *hkey, const UNICODE_STRING *name) { char *n = us_utf8(name); LONG r = delete_value(hkey, n); free(n); return status_of(r); }
// KeyValueBasicInformation (0), KeyValueFullInformation (1), KeyValuePartialInformation (2).
static WINAPI DWORD NtQueryValueKey(void *hkey, const UNICODE_STRING *name, int cls, void *info, DWORD len, DWORD *result) {
    char *n = us_utf8(name);
    DWORD type = 0, size = 0;
    LONG r = query(hkey, n, &type, NULL, &size, 0);
    if (r != ERROR_SUCCESS) { free(n); return status_of(r); }
    uint8_t *data = malloc(size ? size : 1);
    query(hkey, n, &type, data, &size, 0);
    free(n);
    DWORD name_bytes = name ? name->length : 0, need, header;
    switch (cls) {
    case 0: header = 12; need = header + name_bytes; break;
    case 1: header = 20; need = header + name_bytes + size; break;
    case 2: header = 12; need = header + size; break;
    default: free(data); return STATUS_INVALID_PARAMETER;
    }
    if (result) *result = need;
    if (len < header) { free(data); return STATUS_BUFFER_TOO_SMALL; }
    uint32_t *h = info;
    h[0] = 0; h[1] = type;
    DWORD st = len < need ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
    if (cls == 0) { h[2] = name_bytes; if (!st) memcpy((uint8_t *)info + 12, name->buffer, name_bytes); }
    if (cls == 1) {
        h[2] = 20 + name_bytes; h[3] = size; h[4] = name_bytes;
        if (!st) { memcpy((uint8_t *)info + 20, name->buffer, name_bytes); memcpy((uint8_t *)info + 20 + name_bytes, data, size); }
    }
    if (cls == 2) { h[2] = size; if (!st) memcpy((uint8_t *)info + 12, data, size); }
    free(data);
    return st;
}
static WINAPI DWORD RtlFormatCurrentUserKeyPath(UNICODE_STRING *out) {
    WCHAR *w = vx_utf16_dup(g_user_key_path);
    out->buffer = w;
    out->length = (uint16_t)(vx_wcslen(w) * 2);
    out->max = (uint16_t)(out->length + 2);
    return STATUS_SUCCESS;
}
// NtClose of a registry key (heap pointers, unlike kernel handles, are above 4 GB).
int vx_reg_close(void *hkey) {
    if ((uint64_t)hkey < 0x100000000ull || !key_of(hkey)) return 0;
    RegCloseKey(hkey);
    return 1;
}
static const vx_export ntdll[] = {
    VX_EXPORT(NtOpenKeyEx), VX_EXPORT(NtOpenKey), VX_EXPORT(NtCreateKey), VX_EXPORT(NtDeleteKey), VX_EXPORT(NtSetValueKey),
    VX_EXPORT(NtDeleteValueKey), VX_EXPORT(NtQueryValueKey), VX_EXPORT(RtlFormatCurrentUserKeyPath),
    { "ZwOpenKeyEx", (void *)NtOpenKeyEx }, { "ZwOpenKey", (void *)NtOpenKey }, { "ZwCreateKey", (void *)NtCreateKey },
    { "ZwQueryValueKey", (void *)NtQueryValueKey }, { "ZwSetValueKey", (void *)NtSetValueKey },
};

static const vx_export advapi[] = {
    VX_EXPORT(RegOpenKeyExA), VX_EXPORT(RegOpenKeyExW), VX_EXPORT(RegOpenKeyA), VX_EXPORT(RegOpenKeyW),
    VX_EXPORT(RegCreateKeyExA), VX_EXPORT(RegCreateKeyExW), VX_EXPORT(RegCreateKeyA), VX_EXPORT(RegCreateKeyW),
    VX_EXPORT(RegCloseKey), VX_EXPORT(RegFlushKey), VX_EXPORT(RegDeleteKeyA), VX_EXPORT(RegDeleteKeyW),
    VX_EXPORT(RegSetValueExA), VX_EXPORT(RegSetValueExW), VX_EXPORT(RegQueryValueExA), VX_EXPORT(RegQueryValueExW),
    VX_EXPORT(RegGetValueA), VX_EXPORT(RegGetValueW), VX_EXPORT(RegDeleteValueA), VX_EXPORT(RegDeleteValueW),
    VX_EXPORT(RegEnumKeyExA), VX_EXPORT(RegEnumKeyExW), VX_EXPORT(RegEnumValueA), VX_EXPORT(RegEnumValueW),
    VX_EXPORT(RegQueryInfoKeyA), VX_EXPORT(RegQueryInfoKeyW),
};

void vx_registry_init(void) {
    vx_register_module("advapi32.dll", advapi, sizeof advapi / sizeof advapi[0]);
    vx_register_module("kernelbase.dll", advapi, sizeof advapi / sizeof advapi[0]);
    vx_register_module("kernel32.dll", advapi, sizeof advapi / sizeof advapi[0]);
    vx_register_module("ntdll.dll", ntdll, sizeof ntdll / sizeof ntdll[0]);
}

#ifdef __aarch64__
#include "thunks/registry.c.inc"
#endif
