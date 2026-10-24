// advapi32's security objects: SIDs, ACLs and their ACEs, security descriptors in both layouts, SDDL strings, access
// tokens with impersonation, privileges, AccessCheck, and the security of handles and named objects.
//
// One user, as the rest of the runtime reports it: S-1-5-21-1000-1000-1000-1001, a standard user (in Users, not an
// administrator) at medium integrity. Tokens made from others (duplicates, restricted tokens, impersonation) carry
// their restrictions as data: AccessCheck and CheckTokenMembership evaluate them, but nothing on the Mac holds another
// process to them, so CreateProcessAsUser refuses a token that gives less than the user's own (the child would run
// with the user's). An object's descriptor says what the Mac really lets each account do to it, and one set on an
// object takes effect or is refused (see "the security of objects"). Layouts are the documented ones (winnt.h: SID,
// ACL, ACE_HEADER, SECURITY_DESCRIPTOR and SECURITY_DESCRIPTOR_RELATIVE; the struct offsets were checked against the
// MinGW-w64 headers).
#include "vx.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/acl.h>
#include <sys/stat.h>
#include <unistd.h>

// Pointers the program keeps in its own structures (an absolute descriptor's parts, TOKEN_USER's SID): HOST reads one,
// PROG stores one (both are the identity outside the ARM64 build's vCPU, vx.h).
#define HOST(p) ((uint8_t *)VX_LOW(p))
#ifdef __aarch64__
#define PROG(p) ((void *)(uintptr_t)vx_unlow((uint64_t)(uintptr_t)(p)))
#else
#define PROG(p) ((void *)(p))
#endif

enum {
    ERROR_FILE_NOT_FOUND = 2, ERROR_PATH_NOT_FOUND = 3, ERROR_ACCESS_DENIED = 5, ERROR_INVALID_HANDLE = 6,
    ERROR_NOT_ENOUGH_MEMORY = 8, ERROR_BAD_LENGTH = 24, ERROR_NOT_SUPPORTED = 50, ERROR_BAD_NETPATH = 53,
    ERROR_INVALID_PARAMETER = 87, ERROR_INSUFFICIENT_BUFFER = 122, ERROR_INVALID_FLAGS = 1004, ERROR_NO_TOKEN = 1008,
    ERROR_SERVICE_DOES_NOT_EXIST = 1060, ERROR_NOT_FOUND = 1168, ERROR_INVALID_LABEL = 1299, ERROR_NOT_ALL_ASSIGNED = 1300,
    ERROR_UNKNOWN_REVISION = 1305, ERROR_REVISION_MISMATCH = 1306, ERROR_INVALID_OWNER = 1307,
    ERROR_INVALID_PRIMARY_GROUP = 1308, ERROR_NO_IMPERSONATION_TOKEN = 1309, ERROR_NO_SUCH_LOGON_SESSION = 1312,
    ERROR_NO_SUCH_PRIVILEGE = 1313,
    ERROR_PRIVILEGE_NOT_HELD = 1314, ERROR_NONE_MAPPED = 1332, ERROR_INVALID_ACL = 1336, ERROR_INVALID_SID = 1337,
    ERROR_INVALID_SECURITY_DESCR = 1338, ERROR_ALLOTTED_SPACE_EXCEEDED = 1344, ERROR_BAD_IMPERSONATION_LEVEL = 1346,
    ERROR_BAD_TOKEN_TYPE = 1349, ERROR_GENERIC_NOT_MAPPED = 1360, ERROR_BAD_DESCRIPTOR_FORMAT = 1361,
    ERROR_INVALID_PRINTER_NAME = 1801,
};
enum {                                                    // SECURITY_DESCRIPTOR_CONTROL
    SE_OWNER_DEFAULTED = 0x1, SE_GROUP_DEFAULTED = 0x2, SE_DACL_PRESENT = 0x4, SE_DACL_DEFAULTED = 0x8,
    SE_SACL_PRESENT = 0x10, SE_SACL_DEFAULTED = 0x20, SE_DACL_AUTO_INHERIT_REQ = 0x100, SE_SACL_AUTO_INHERIT_REQ = 0x200,
    SE_DACL_AUTO_INHERITED = 0x400, SE_SACL_AUTO_INHERITED = 0x800, SE_DACL_PROTECTED = 0x1000, SE_SACL_PROTECTED = 0x2000,
    SE_SELF_RELATIVE = 0x8000,
    DACL_BITS = SE_DACL_PRESENT | SE_DACL_DEFAULTED | SE_DACL_AUTO_INHERIT_REQ | SE_DACL_AUTO_INHERITED | SE_DACL_PROTECTED,
    SACL_BITS = SE_SACL_PRESENT | SE_SACL_DEFAULTED | SE_SACL_AUTO_INHERIT_REQ | SE_SACL_AUTO_INHERITED | SE_SACL_PROTECTED,
};
enum {                                                    // SECURITY_INFORMATION
    OWNER_SI = 0x1, GROUP_SI = 0x2, DACL_SI = 0x4, SACL_SI = 0x8, LABEL_SI = 0x10,
    PROTECTED_DACL_SI = 0x80000000u, PROTECTED_SACL_SI = 0x40000000, UNPROTECTED_DACL_SI = 0x20000000,
    UNPROTECTED_SACL_SI = 0x10000000,
};
enum {                                                    // ACE types, flags, object ACE flags
    ACCESS_ALLOWED = 0, ACCESS_DENIED = 1, SYSTEM_AUDIT = 2, SYSTEM_ALARM = 3, ACCESS_ALLOWED_OBJECT = 5,
    ACCESS_DENIED_OBJECT = 6, SYSTEM_AUDIT_OBJECT = 7, SYSTEM_ALARM_OBJECT = 8, ACCESS_ALLOWED_CALLBACK = 9,
    ACCESS_DENIED_CALLBACK = 10, ACCESS_DENIED_CALLBACK_OBJECT = 12, SYSTEM_MANDATORY_LABEL = 17,
    SYSTEM_SCOPED_POLICY_ID = 19, SYSTEM_PROCESS_TRUST_LABEL = 20, LAST_ACE_TYPE = 21,
    OBJECT_INHERIT = 0x1, CONTAINER_INHERIT = 0x2, INHERIT_ONLY = 0x8, INHERITED = 0x10, VALID_INHERIT_FLAGS = 0x1f,
    SUCCESSFUL_ACCESS = 0x40, FAILED_ACCESS = 0x80,
    OBJECT_TYPE_PRESENT = 0x1, INHERITED_OBJECT_TYPE_PRESENT = 0x2,
};
enum {                                                    // access rights
    DELETE_RIGHT = 0x10000, READ_CONTROL = 0x20000, WRITE_DAC = 0x40000, WRITE_OWNER = 0x80000, SYNCHRONIZE = 0x100000,
    ACCESS_SYSTEM_SECURITY = 0x1000000, MAXIMUM_ALLOWED = 0x2000000,
    GENERIC_ALL = 0x10000000, GENERIC_EXECUTE = 0x20000000, GENERIC_WRITE = 0x40000000, GENERIC_READ = 0x80000000u,
    GENERIC_BITS = 0xf0000000u,
};
enum {
    SE_GROUP_MANDATORY = 0x1, SE_GROUP_ENABLED_BY_DEFAULT = 0x2, SE_GROUP_ENABLED = 0x4, SE_GROUP_OWNER = 0x8,
    SE_GROUP_USE_FOR_DENY_ONLY = 0x10, SE_GROUP_INTEGRITY = 0x20, SE_GROUP_INTEGRITY_ENABLED = 0x40,
    SE_GROUP_LOGON_ID = 0xc0000000u, GROUP_ON = SE_GROUP_MANDATORY | SE_GROUP_ENABLED_BY_DEFAULT | SE_GROUP_ENABLED,
    SE_PRIVILEGE_ENABLED_BY_DEFAULT = 0x1, SE_PRIVILEGE_ENABLED = 0x2, SE_PRIVILEGE_REMOVED = 0x4,
    SE_PRIVILEGE_USED_FOR_ACCESS = 0x80000000u, PRIVILEGE_SET_ALL_NECESSARY = 1,
    NO_WRITE_UP = 0x1, NO_READ_UP = 0x2, NO_EXECUTE_UP = 0x4,             // SYSTEM_MANDATORY_LABEL_* policy bits
    TOKEN_PRIMARY = 1, TOKEN_IMPERSONATION = 2,
    SECURITY_ANONYMOUS = 0, SECURITY_IDENTIFICATION = 1, SECURITY_IMPERSONATION = 2, SECURITY_DELEGATION = 3,
};
enum {                                                    // SE_OBJECT_TYPE
    SE_UNKNOWN_OBJECT_TYPE = 0, SE_FILE_OBJECT = 1, SE_SERVICE = 2, SE_PRINTER = 3, SE_REGISTRY_KEY = 4, SE_LMSHARE = 5,
    SE_KERNEL_OBJECT = 6, SE_WINDOW_OBJECT = 7, SE_REGISTRY_WOW64_32KEY = 12, SE_REGISTRY_WOW64_64KEY = 13,
};
#define SID_MAX 68                                        // a SID with the most (15) sub-authorities
#define SE_CHANGE_NOTIFY_PRIVILEGE 23

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;   // tokens' data, thread tokens, kept descriptors

static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static DWORD rd32(const uint8_t *p) { DWORD v; memcpy(&v, p, 4); return v; }
static void wr16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static void wr32(uint8_t *p, DWORD v) { memcpy(p, &v, 4); }
static uint8_t *rdptr(const uint8_t *p) { void *v; memcpy(&v, p, 8); return HOST(v); }
static void wrptr(uint8_t *p, const void *v) { void *q = v ? PROG(v) : NULL; memcpy(p, &q, 8); }

// ---- SIDs -----------------------------------------------------------------------------------------------------------
// Revision 1, a sub-authority count, a 48-bit big-endian identifier authority, the sub-authorities (32-bit).
static int sid_ok(const uint8_t *s) { return s && s[0] == 1 && s[1] <= 15; }
static DWORD sid_len(const uint8_t *s) { return 8 + 4 * (DWORD)s[1]; }
static DWORD sid_sub(const uint8_t *s, int i) { return rd32(s + 8 + 4 * i); }
static uint64_t sid_auth(const uint8_t *s) { uint64_t a = 0; for (int i = 2; i < 8; i++) a = a << 8 | s[i]; return a; }
static int sid_eq(const uint8_t *a, const uint8_t *b) { return sid_ok(a) && sid_ok(b) && a[1] == b[1] && !memcmp(a, b, sid_len(a)); }
static DWORD sid_make(uint8_t *out, uint64_t auth, int n, const DWORD *sub) {
    out[0] = 1; out[1] = (uint8_t)n;
    for (int i = 0; i < 6; i++) out[2 + i] = (uint8_t)(auth >> (8 * (5 - i)));
    for (int i = 0; i < n; i++) wr32(out + 8 + 4 * i, sub[i]);
    return 8 + 4 * (DWORD)n;
}
static DWORD sid_of(uint8_t *out, uint64_t auth, int n, ...) {
    DWORD sub[15]; va_list ap; va_start(ap, n);
    for (int i = 0; i < n; i++) sub[i] = va_arg(ap, DWORD);
    va_end(ap);
    return sid_make(out, auth, n, sub);
}
// The user, the machine's account domain (S-1-5-21-1000-1000-1000: the user's prefix), the logon session's SID.
#define MACHINE_SUBS 21, 1000, 1000, 1000
#define USER_RID 1001
#define NONE_RID 513                                      // the machine's "None" group: a local account's primary group
#define LOGON_ID 0x3a5c1u                                 // the logon session (made up): S-1-5-5-0-<it>, LUID {it, 0}
static void user_sid(uint8_t *s) { sid_of(s, 5, 5, MACHINE_SUBS, USER_RID); }
static int machine_rid(const uint8_t *s, DWORD *rid) {   // a SID of the machine's domain: its RID
    if (!sid_ok(s) || s[1] != 5 || sid_auth(s) != 5 || sid_sub(s, 0) != 21 || sid_sub(s, 1) != 1000 ||
        sid_sub(s, 2) != 1000 || sid_sub(s, 3) != 1000) return 0;
    *rid = sid_sub(s, 4);
    return 1;
}

// WELL_KNOWN_SID_TYPE values (CreateWellKnownSid), their SIDs and SDDL abbreviations (Microsoft's "Well-known SIDs"
// and "SID strings" tables).
typedef struct { uint8_t type; char sddl[3]; uint8_t auth, n; DWORD sub[6]; } wk_sid;
static const wk_sid k_well_known[] = {
    { 0, "", 0, 1, { 0 } }, { 1, "WD", 1, 1, { 0 } }, { 2, "", 2, 1, { 0 } }, { 3, "CO", 3, 1, { 0 } }, { 4, "CG", 3, 1, { 1 } },
    { 5, "", 3, 1, { 2 } }, { 6, "", 3, 1, { 3 } }, { 7, "", 5, 0, { 0 } }, { 8, "", 5, 1, { 1 } }, { 9, "NU", 5, 1, { 2 } },
    { 10, "", 5, 1, { 3 } }, { 11, "IU", 5, 1, { 4 } }, { 12, "SU", 5, 1, { 6 } }, { 13, "AN", 5, 1, { 7 } },
    { 14, "", 5, 1, { 8 } }, { 15, "ED", 5, 1, { 9 } }, { 16, "PS", 5, 1, { 10 } }, { 17, "AU", 5, 1, { 11 } },
    { 18, "RC", 5, 1, { 12 } }, { 19, "", 5, 1, { 13 } }, { 20, "", 5, 1, { 14 } }, { 21, "", 5, 1, { 5 } },
    { 22, "SY", 5, 1, { 18 } }, { 23, "LS", 5, 1, { 19 } }, { 24, "NS", 5, 1, { 20 } }, { 25, "", 5, 1, { 32 } },
    { 26, "BA", 5, 2, { 32, 544 } }, { 27, "BU", 5, 2, { 32, 545 } }, { 28, "BG", 5, 2, { 32, 546 } },
    { 29, "PU", 5, 2, { 32, 547 } }, { 30, "AO", 5, 2, { 32, 548 } }, { 31, "SO", 5, 2, { 32, 549 } },
    { 32, "PO", 5, 2, { 32, 550 } }, { 33, "BO", 5, 2, { 32, 551 } }, { 34, "RE", 5, 2, { 32, 552 } },
    { 35, "RU", 5, 2, { 32, 554 } }, { 36, "RD", 5, 2, { 32, 555 } }, { 37, "NO", 5, 2, { 32, 556 } },
    { 51, "", 5, 2, { 64, 10 } }, { 52, "", 5, 2, { 64, 21 } }, { 53, "", 5, 2, { 64, 14 } }, { 54, "", 5, 1, { 15 } },
    { 55, "", 5, 1, { 1000 } }, { 56, "", 5, 2, { 32, 557 } }, { 57, "MU", 5, 2, { 32, 558 } },
    { 58, "LU", 5, 2, { 32, 559 } }, { 59, "", 5, 2, { 32, 560 } }, { 60, "", 5, 2, { 32, 561 } },
    { 61, "", 5, 2, { 32, 562 } }, { 62, "IS", 5, 2, { 32, 568 } }, { 63, "", 5, 1, { 17 } }, { 64, "CY", 5, 2, { 32, 569 } },
    { 65, "", 16, 1, { 0 } }, { 66, "LW", 16, 1, { 4096 } }, { 67, "ME", 16, 1, { 8192 } }, { 68, "HI", 16, 1, { 12288 } },
    { 69, "SI", 16, 1, { 16384 } }, { 70, "WR", 5, 1, { 33 } }, { 71, "OW", 3, 1, { 4 } }, { 76, "ER", 5, 2, { 32, 573 } },
    { 78, "CD", 5, 2, { 32, 574 } }, { 79, "MP", 16, 1, { 8448 } }, { 80, "", 2, 1, { 0 } }, { 81, "", 2, 1, { 1 } },
    { 83, "", 15, 1, { 2 } }, { 84, "AC", 15, 2, { 2, 1 } }, { 85, "", 15, 2, { 3, 1 } }, { 86, "", 15, 2, { 3, 2 } },
    { 87, "", 15, 2, { 3, 3 } }, { 88, "", 15, 2, { 3, 4 } }, { 89, "", 15, 2, { 3, 5 } }, { 90, "", 15, 2, { 3, 6 } },
    { 91, "", 15, 2, { 3, 7 } }, { 92, "", 15, 2, { 3, 9 } }, { 93, "", 15, 2, { 3, 8 } }, { 94, "", 15, 2, { 3, 10 } },
    { 95, "RA", 5, 2, { 32, 575 } }, { 96, "ES", 5, 2, { 32, 576 } }, { 97, "MS", 5, 2, { 32, 577 } },
    { 98, "UD", 5, 6, { 84, 0, 0, 0, 0, 0 } }, { 99, "HA", 5, 2, { 32, 578 } }, { 101, "AA", 5, 2, { 32, 579 } },
    { 102, "RM", 5, 2, { 32, 580 } }, { 103, "AS", 18, 1, { 1 } }, { 104, "SS", 18, 1, { 2 } }, { 105, "", 5, 1, { 113 } },
    { 106, "", 5, 1, { 114 } },
};
// Types relative to a domain (CreateWellKnownSid's DomainSid); LA and LG name the machine's own.
typedef struct { uint8_t type; char sddl[3]; DWORD rid; } wk_rid;
static const wk_rid k_domain_rids[] = {
    { 38, "LA", 500 }, { 39, "LG", 501 }, { 40, "", 502 }, { 41, "", 512 }, { 42, "", 513 }, { 43, "", 514 },
    { 44, "", 515 }, { 45, "", 516 }, { 46, "", 517 }, { 47, "", 518 }, { 48, "", 519 }, { 49, "", 520 }, { 50, "", 553 },
};
static DWORD wk_make(const wk_sid *w, uint8_t *out) { return sid_make(out, w->auth, w->n, w->sub); }
static const wk_sid *wk_by_type(int type) {
    for (size_t i = 0; i < sizeof k_well_known / sizeof *k_well_known; i++) if (k_well_known[i].type == type) return &k_well_known[i];
    return NULL;
}

// "S-1-5-32-544" (authority in decimal, or 0x and hex; sub-authorities in decimal or 0x hex) or an SDDL abbreviation
// ("BA"): the SID's length (0 if none), *end past it.
static DWORD sid_parse(const char *s, const char **end, uint8_t *out) {
    if ((s[0] == 'S' || s[0] == 's') && s[1] == '-') {
        const char *p = s + 2; char *e;
        if (!isdigit((unsigned char)*p)) return 0;
        unsigned long long rev = strtoull(p, &e, 10);
        if (rev != 1 || *e != '-') return 0;
        p = e + 1;
        unsigned long long auth;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { if (!isxdigit((unsigned char)p[2])) return 0; auth = strtoull(p + 2, &e, 16); }
        else { if (!isdigit((unsigned char)*p)) return 0; auth = strtoull(p, &e, 10); }
        if (auth >> 48) return 0;
        DWORD sub[15]; int n = 0;
        p = e;
        while (*p == '-' && isdigit((unsigned char)p[1])) {
            const char *q = p + 1;
            unsigned long long v;
            if (q[0] == '0' && (q[1] == 'x' || q[1] == 'X')) { if (!isxdigit((unsigned char)q[2])) return 0; v = strtoull(q + 2, &e, 16); }
            else v = strtoull(q, &e, 10);
            if (v > 0xffffffffull || n == 15) return 0;
            sub[n++] = (DWORD)v;
            p = e;
        }
        if (end) *end = p;
        return sid_make(out, auth, n, sub);
    }
    if (!isalpha((unsigned char)s[0]) || !isalpha((unsigned char)s[1])) return 0;
    char ab[3] = { (char)toupper((unsigned char)s[0]), (char)toupper((unsigned char)s[1]), 0 };
    if (end) *end = s + 2;
    for (size_t i = 0; i < sizeof k_well_known / sizeof *k_well_known; i++)
        if (k_well_known[i].sddl[0] && !strcmp(k_well_known[i].sddl, ab)) return wk_make(&k_well_known[i], out);
    for (size_t i = 0; i < sizeof k_domain_rids / sizeof *k_domain_rids; i++)
        if (k_domain_rids[i].sddl[0] && !strcmp(k_domain_rids[i].sddl, ab)) return sid_of(out, 5, 5, MACHINE_SUBS, k_domain_rids[i].rid);
    return 0;                                             // (the domain groups, DA, DU and so on: this machine is in none)
}
static int sid_format(const uint8_t *s, char *out, size_t cap) {
    uint64_t a = sid_auth(s);
    int n = a >> 32 ? snprintf(out, cap, "S-%u-0x%012llX", s[0], (unsigned long long)a) : snprintf(out, cap, "S-%u-%llu", s[0], (unsigned long long)a);
    for (int i = 0; i < s[1]; i++) n += snprintf(out + n, cap - (size_t)n, "-%u", sid_sub(s, i));
    return n;
}
static void sid_format_sddl(const uint8_t *s, char *out, size_t cap) {   // abbreviated when SDDL has a name for it
    uint8_t w[SID_MAX];
    for (size_t i = 0; i < sizeof k_well_known / sizeof *k_well_known; i++)
        if (k_well_known[i].sddl[0] && wk_make(&k_well_known[i], w) && sid_eq(w, s)) { snprintf(out, cap, "%s", k_well_known[i].sddl); return; }
    DWORD rid;
    if (machine_rid(s, &rid))
        for (size_t i = 0; i < sizeof k_domain_rids / sizeof *k_domain_rids; i++)
            if (k_domain_rids[i].sddl[0] && k_domain_rids[i].rid == rid) { snprintf(out, cap, "%s", k_domain_rids[i].sddl); return; }
    sid_format(s, out, cap);
}

// ---- ACLs and ACEs ----------------------------------------------------------------------------------------------------
// ACL: revision (2, or 4 with object ACEs), size of the whole buffer, ACE count; then the ACEs, each with a header
// (type, flags, size) and, for the types with one, an access mask and a SID (object ACEs put flags and GUIDs between).
static int ace_is_object(uint8_t t) { return (t >= 5 && t <= 8) || t == 11 || t == 12 || t == 15 || t == 16; }
static int ace_has_sid(uint8_t t) { return t <= LAST_ACE_TYPE && t != 4; }   // (4: compound ACEs, never used)
static const uint8_t *ace_sid(const uint8_t *ace, size_t *room) {
    size_t n = rd16(ace + 2), off = 8;
    if (!ace_has_sid(ace[0])) return NULL;
    if (ace_is_object(ace[0])) {
        if (n < 12) return NULL;
        DWORD f = rd32(ace + 8);
        off = 12 + (f & OBJECT_TYPE_PRESENT ? 16 : 0) + (f & INHERITED_OBJECT_TYPE_PRESENT ? 16 : 0);
    }
    if (off + 8 > n) return NULL;
    if (room) *room = n - off;
    return ace + off;
}
static int acl_ok(const uint8_t *a) {
    if (!a || a[0] < 2 || a[0] > 4) return 0;
    DWORD size = rd16(a + 2), count = rd16(a + 4), off = 8;
    if (size < 8) return 0;
    for (DWORD i = 0; i < count; i++) {
        if (off + 4 > size) return 0;
        DWORD n = rd16(a + off + 2);
        if (n < 4 || off + n > size) return 0;
        if (ace_is_object(a[off]) && a[0] < 4) return 0;  // object ACEs need ACL_REVISION_DS
        if (ace_has_sid(a[off])) {
            size_t room; const uint8_t *s = ace_sid(a + off, &room);
            if (!s || !sid_ok(s) || sid_len(s) > room) return 0;
        }
        off += n;
    }
    return 1;
}
static DWORD acl_end(const uint8_t *a) {                 // (a valid ACL) where its ACEs end
    DWORD off = 8;
    for (DWORD i = 0; i < rd16(a + 4); i++) off += rd16(a + off + 2);
    return off;
}
static DWORD acl_bytes(const uint8_t *a) { return (rd16(a + 2) + 3u) & ~3u; }
#define FOR_ACE(a, ace) for (DWORD i_ = 0, o_ = 8; i_ < rd16((a) + 4) && ((ace) = (a) + o_, 1); i_++, o_ += rd16((a) + o_ + 2))

// A growing ACL (SDDL, SetEntriesInAcl, merged labels): ACEs appended, the header written at the end.
typedef struct { uint8_t *p; DWORD n, cap; int count; uint8_t rev; } acl_buf;
static void ab_init(acl_buf *b) { memset(b, 0, sizeof *b); b->cap = 256; b->p = malloc(b->cap); b->n = 8; b->rev = 2; }
static void ab_add(acl_buf *b, const uint8_t *ace) {
    DWORD n = rd16(ace + 2);
    if (b->n + n > b->cap) { b->cap = (b->n + n) * 2; b->p = realloc(b->p, b->cap); }
    memcpy(b->p + b->n, ace, n);
    b->n += n; b->count++;
    if (ace_is_object(ace[0])) b->rev = 4;
}
static uint8_t *ab_finish(acl_buf *b) {                   // malloc'd ACL, sized to its ACEs
    b->p[0] = b->rev; b->p[1] = 0; wr16(b->p + 2, (uint16_t)b->n); wr16(b->p + 4, (uint16_t)b->count); wr16(b->p + 6, 0);
    return b->p;
}
// One ACE with a mask and a SID; object ACEs with their GUIDs (NULL: absent). Returns its size (at most 112 bytes).
static DWORD ace_make(uint8_t *out, uint8_t type, uint8_t flags, DWORD mask, const uint8_t *sid, const uint8_t *obj, const uint8_t *inh) {
    DWORD off = 8;
    out[0] = type; out[1] = flags; wr32(out + 4, mask);
    if (ace_is_object(type)) {
        wr32(out + 8, (obj ? OBJECT_TYPE_PRESENT : 0) | (inh ? INHERITED_OBJECT_TYPE_PRESENT : 0));
        off = 12;
        if (obj) { memcpy(out + off, obj, 16); off += 16; }
        if (inh) { memcpy(out + off, inh, 16); off += 16; }
    }
    memcpy(out + off, sid, sid_len(sid));
    off += sid_len(sid);
    wr16(out + 2, (uint16_t)off);
    return off;
}

// ---- security descriptors --------------------------------------------------------------------------------------------
// Absolute: revision 1, control, then pointers to owner, group, SACL, DACL (40 bytes). Self-relative: the same with
// 32-bit offsets from its start (20 bytes), the parts following it.
typedef struct { uint16_t control; const uint8_t *owner, *group, *sacl, *dacl; } sd_parts;
static int sd_read(const uint8_t *sd, sd_parts *p) {
    memset(p, 0, sizeof *p);
    if (!sd || sd[0] != 1) return 0;
    p->control = rd16(sd + 2);
    if (p->control & SE_SELF_RELATIVE) {
        DWORD o[4];
        for (int i = 0; i < 4; i++) { o[i] = rd32(sd + 4 + 4 * i); if (o[i] && o[i] < 20) return 0; }
        p->owner = o[0] ? sd + o[0] : NULL; p->group = o[1] ? sd + o[1] : NULL;
        p->sacl = o[2] ? sd + o[2] : NULL; p->dacl = o[3] ? sd + o[3] : NULL;
    } else {
        p->owner = rdptr(sd + 8); p->group = rdptr(sd + 16); p->sacl = rdptr(sd + 24); p->dacl = rdptr(sd + 32);
    }
    if (!(p->control & SE_SACL_PRESENT)) p->sacl = NULL;
    if (!(p->control & SE_DACL_PRESENT)) p->dacl = NULL;
    return 1;
}
static int sd_valid(const uint8_t *sd, sd_parts *p) {
    return sd_read(sd, p) && (!p->owner || sid_ok(p->owner)) && (!p->group || sid_ok(p->group)) &&
           (!p->sacl || acl_ok(p->sacl)) && (!p->dacl || acl_ok(p->dacl));
}
static DWORD sd_rel_size(const sd_parts *p) {
    return 20 + (p->owner ? sid_len(p->owner) : 0) + (p->group ? sid_len(p->group) : 0) +
           (p->sacl ? acl_bytes(p->sacl) : 0) + (p->dacl ? acl_bytes(p->dacl) : 0);
}
static void sd_rel_write(const sd_parts *p, uint8_t *out) {
    memset(out, 0, sd_rel_size(p));
    out[0] = 1;
    wr16(out + 2, (uint16_t)(p->control | SE_SELF_RELATIVE));
    DWORD off = 20;
    const uint8_t *part[4] = { p->owner, p->group, p->sacl, p->dacl };
    for (int i = 0; i < 4; i++) {
        if (!part[i]) continue;
        DWORD n = i < 2 ? sid_len(part[i]) : rd16(part[i] + 2);
        wr32(out + 4 + 4 * i, off);
        memcpy(out + off, part[i], n);
        off += i < 2 ? n : acl_bytes(part[i]);
    }
}
static uint8_t *sd_rel(const sd_parts *p, DWORD *size, int for_program) {   // malloc'd, or LocalAlloc'd for the program
    DWORD n = sd_rel_size(p);
    uint8_t *sd = for_program ? vx_heap_alloc(n) : malloc(n);
    if (sd) sd_rel_write(p, sd);
    if (size) *size = n;
    return sd;
}

// The mandatory label ACEs of a SACL (LABEL_SECURITY_INFORMATION), as a new ACL; NULL if it has none.
static uint8_t *labels_of(const uint8_t *sacl, int labels) {
    if (!sacl) return NULL;
    acl_buf b; ab_init(&b);
    const uint8_t *ace;
    FOR_ACE(sacl, ace) if ((ace[0] == SYSTEM_MANDATORY_LABEL) == labels) ab_add(&b, ace);
    if (!b.count && labels) { free(b.p); return NULL; }
    return ab_finish(&b);
}
// The parts of `sd` that `info` asks for, as a self-relative descriptor for the program (LocalFree).
static uint8_t *sd_select(const uint8_t *sd, DWORD info, DWORD *size) {
    sd_parts s, p = { 0 };
    sd_read(sd, &s);
    uint8_t *labels = NULL;
    if (info & OWNER_SI) { p.owner = s.owner; p.control |= s.control & SE_OWNER_DEFAULTED; }
    if (info & GROUP_SI) { p.group = s.group; p.control |= s.control & SE_GROUP_DEFAULTED; }
    if (info & DACL_SI) { p.dacl = s.dacl; p.control |= s.control & DACL_BITS; }
    if (info & SACL_SI) { p.sacl = s.sacl; p.control |= s.control & SACL_BITS; }
    else if ((info & LABEL_SI) && (labels = labels_of(s.sacl, 1))) { p.sacl = labels; p.control |= (s.control & SACL_BITS) | SE_SACL_PRESENT; }
    uint8_t *r = sd_rel(&p, size, 1);
    free(labels);
    return r;
}
// `cur` (self-relative) with the parts `info` names replaced: a new malloc'd self-relative descriptor.
static uint8_t *sd_merge(const uint8_t *cur, DWORD info, const uint8_t *owner, const uint8_t *group, const uint8_t *dacl, const uint8_t *sacl) {
    sd_parts p;
    sd_read(cur, &p);
    p.control &= (uint16_t)~SE_SELF_RELATIVE;
    uint8_t *merged = NULL;
    if (info & OWNER_SI) { p.owner = owner; p.control &= (uint16_t)~SE_OWNER_DEFAULTED; }
    if (info & GROUP_SI) { p.group = group; p.control &= (uint16_t)~SE_GROUP_DEFAULTED; }
    if (info & DACL_SI) { p.dacl = dacl; p.control = (uint16_t)((p.control & ~SE_DACL_DEFAULTED) | SE_DACL_PRESENT); }
    if (info & SACL_SI) { p.sacl = sacl; p.control = (uint16_t)((p.control & ~SE_SACL_DEFAULTED) | SE_SACL_PRESENT); }
    else if (info & LABEL_SI) {                           // the labels replaced, the SACL's other ACEs kept
        acl_buf b; ab_init(&b);
        const uint8_t *ace;
        if (p.sacl) FOR_ACE(p.sacl, ace) if (ace[0] != SYSTEM_MANDATORY_LABEL) ab_add(&b, ace);
        if (sacl) FOR_ACE(sacl, ace) if (ace[0] == SYSTEM_MANDATORY_LABEL) ab_add(&b, ace);
        p.sacl = merged = ab_finish(&b);
        p.control |= SE_SACL_PRESENT;
    }
    if (info & PROTECTED_DACL_SI) p.control |= SE_DACL_PROTECTED;
    if (info & UNPROTECTED_DACL_SI) p.control &= (uint16_t)~SE_DACL_PROTECTED;
    if (info & PROTECTED_SACL_SI) p.control |= SE_SACL_PROTECTED;
    if (info & UNPROTECTED_SACL_SI) p.control &= (uint16_t)~SE_SACL_PROTECTED;
    uint8_t *r = sd_rel(&p, NULL, 0);
    free(merged);
    return r;
}

// ---- SDDL ------------------------------------------------------------------------------------------------------------
// "O:owner G:group D:flags(ace)(ace) S:flags(ace)": Microsoft's "Security Descriptor String Format" and
// "ACE Strings". Conditional and resource-attribute ACEs are not supported (their strings are refused).
typedef struct { const char *s; DWORD v; } name_value;
static const name_value k_ace_types[] = {
    { "A", ACCESS_ALLOWED }, { "D", ACCESS_DENIED }, { "AU", SYSTEM_AUDIT }, { "AL", SYSTEM_ALARM },
    { "OA", ACCESS_ALLOWED_OBJECT }, { "OD", ACCESS_DENIED_OBJECT }, { "OU", SYSTEM_AUDIT_OBJECT },
    { "OL", SYSTEM_ALARM_OBJECT }, { "ML", SYSTEM_MANDATORY_LABEL }, { "SP", SYSTEM_SCOPED_POLICY_ID },
    { "TL", SYSTEM_PROCESS_TRUST_LABEL },
};
static const name_value k_ace_flags[] = {
    { "OI", OBJECT_INHERIT }, { "CI", CONTAINER_INHERIT }, { "NP", 0x4 }, { "IO", INHERIT_ONLY }, { "ID", INHERITED },
    { "SA", SUCCESSFUL_ACCESS }, { "FA", FAILED_ACCESS },
};
static const name_value k_rights_whole[] = {               // whole masks: file and registry rights
    { "FA", 0x1f01ff }, { "FR", 0x120089 }, { "FW", 0x120116 }, { "FX", 0x1200a0 },
    { "KA", 0xf003f }, { "KR", 0x20019 }, { "KW", 0x20006 }, { "KX", 0x20019 },
};
static const name_value k_rights_bits[] = {
    { "GA", GENERIC_ALL }, { "GR", GENERIC_READ }, { "GW", GENERIC_WRITE }, { "GX", GENERIC_EXECUTE },
    { "CC", 0x1 }, { "DC", 0x2 }, { "LC", 0x4 }, { "SW", 0x8 }, { "RP", 0x10 }, { "WP", 0x20 }, { "DT", 0x40 },
    { "LO", 0x80 }, { "CR", 0x100 }, { "SD", DELETE_RIGHT }, { "RC", READ_CONTROL }, { "WD", WRITE_DAC }, { "WO", WRITE_OWNER },
};
static const name_value k_label_bits[] = { { "NW", NO_WRITE_UP }, { "NR", NO_READ_UP }, { "NX", NO_EXECUTE_UP } };
#define COUNT(a) (sizeof (a) / sizeof *(a))

typedef struct { char *p; size_t n, cap; } sbuf;
static void sb_add(sbuf *b, const char *s) {
    size_t n = strlen(s);
    if (b->n + n + 1 > b->cap) { b->cap = (b->n + n + 1) * 2; b->p = realloc(b->p, b->cap); }
    memcpy(b->p + b->n, s, n + 1);
    b->n += n;
}

static int guid_parse(const char *s, size_t n, uint8_t *g) {
    if (n == 38 && s[0] == '{' && s[37] == '}') { s++; n -= 2; }
    if (n != 36 || s[8] != '-' || s[13] != '-' || s[18] != '-' || s[23] != '-') return 0;
    char hex[33]; int k = 0;
    for (size_t i = 0; i < 36; i++) if (s[i] != '-') { if (!isxdigit((unsigned char)s[i])) return 0; hex[k++] = s[i]; }
    hex[32] = 0;
    uint8_t b[16];
    for (int i = 0; i < 16; i++) { char t[3] = { hex[2 * i], hex[2 * i + 1], 0 }; b[i] = (uint8_t)strtoul(t, NULL, 16); }
    // Data1, Data2, Data3 little-endian; Data4 as written
    g[0] = b[3]; g[1] = b[2]; g[2] = b[1]; g[3] = b[0]; g[4] = b[5]; g[5] = b[4]; g[6] = b[7]; g[7] = b[6];
    memcpy(g + 8, b + 8, 8);
    return 1;
}
static void guid_format(const uint8_t *g, char *out, size_t cap) {
    snprintf(out, cap, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", rd32(g), rd16(g + 4), rd16(g + 6),
             g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
}
static int field_is(const char *f, size_t n, const char *name) { return strlen(name) == n && !strncasecmp(f, name, n); }

// One "(type;flags;rights;object;inherited object;sid)": the ACE into `out` (its size, 0 and *err when refused).
static DWORD sddl_ace(const char *s, size_t len, uint8_t *out, DWORD *err) {
    const char *f[7]; size_t fl[7]; int nf = 0;
    const char *start = s;
    for (size_t i = 0; i <= len; i++)
        if (i == len || s[i] == ';') { if (nf == 7) break; f[nf] = start; fl[nf++] = (size_t)(s + i - start); start = s + i + 1; }
    *err = ERROR_INVALID_PARAMETER;
    if (nf != 6) return 0;                                // (a seventh field: a resource attribute)
    while (fl[0] && f[0][0] == ' ') { f[0]++; fl[0]--; }
    int type = -1;
    for (size_t i = 0; i < COUNT(k_ace_types); i++) if (field_is(f[0], fl[0], k_ace_types[i].s)) type = (int)k_ace_types[i].v;
    if (type < 0) return 0;
    DWORD flags = 0, mask = 0;
    for (size_t i = 0; i < fl[1]; i += 2) {
        int found = 0;
        for (size_t k = 0; k < COUNT(k_ace_flags) && !found; k++)
            if (i + 1 < fl[1] && !strncasecmp(f[1] + i, k_ace_flags[k].s, 2)) { flags |= k_ace_flags[k].v; found = 1; }
        if (!found) return 0;
    }
    if (fl[2] && isdigit((unsigned char)f[2][0])) {
        char num[24]; char *e;
        if (fl[2] >= sizeof num) return 0;
        memcpy(num, f[2], fl[2]); num[fl[2]] = 0;
        unsigned long long v = strtoull(num, &e, 0);
        if (*e || v > 0xffffffffull) return 0;
        mask = (DWORD)v;
    } else {
        for (size_t i = 0; i < fl[2]; i += 2) {
            int found = 0;
            if (i + 1 >= fl[2]) return 0;
            for (size_t k = 0; k < COUNT(k_rights_whole) && !found; k++) if (!strncasecmp(f[2] + i, k_rights_whole[k].s, 2)) { mask |= k_rights_whole[k].v; found = 1; }
            for (size_t k = 0; k < COUNT(k_rights_bits) && !found; k++) if (!strncasecmp(f[2] + i, k_rights_bits[k].s, 2)) { mask |= k_rights_bits[k].v; found = 1; }
            for (size_t k = 0; k < COUNT(k_label_bits) && !found; k++) if (!strncasecmp(f[2] + i, k_label_bits[k].s, 2)) { mask |= k_label_bits[k].v; found = 1; }
            if (!found) return 0;
        }
    }
    uint8_t obj[16], inh[16];
    int has_obj = fl[3] > 0, has_inh = fl[4] > 0;
    if ((has_obj || has_inh) && !ace_is_object((uint8_t)type)) return 0;
    if (has_obj && !guid_parse(f[3], fl[3], obj)) return 0;
    if (has_inh && !guid_parse(f[4], fl[4], inh)) return 0;
    char sid_text[200];
    if (!fl[5] || fl[5] >= sizeof sid_text) { *err = ERROR_INVALID_SID; return 0; }
    memcpy(sid_text, f[5], fl[5]); sid_text[fl[5]] = 0;
    uint8_t sid[SID_MAX]; const char *end;
    if (!sid_parse(sid_text, &end, sid) || *end) { *err = ERROR_INVALID_SID; return 0; }
    return ace_make(out, (uint8_t)type, (uint8_t)flags, mask, sid, has_obj ? obj : NULL, has_inh ? inh : NULL);
}
// The ACL part after "D:" or "S:": flags, then ACEs. *acl NULL with *null_acl for NO_ACCESS_CONTROL.
static const char *sddl_acl(const char *s, uint16_t *control, int dacl, uint8_t **acl, DWORD *err) {
    int null_acl = 0;
    *err = ERROR_INVALID_PARAMETER;
    for (;;) {
        if (!strncasecmp(s, "NO_ACCESS_CONTROL", 17)) { null_acl = 1; s += 17; }
        else if (*s == 'P' || *s == 'p') { *control |= dacl ? SE_DACL_PROTECTED : SE_SACL_PROTECTED; s++; }
        else if (!strncasecmp(s, "AI", 2)) { *control |= dacl ? SE_DACL_AUTO_INHERITED : SE_SACL_AUTO_INHERITED; s += 2; }
        else if (!strncasecmp(s, "AR", 2)) { *control |= dacl ? SE_DACL_AUTO_INHERIT_REQ : SE_SACL_AUTO_INHERIT_REQ; s += 2; }
        else break;
    }
    acl_buf b; ab_init(&b);
    while (*s == ' ') s++;
    while (*s == '(') {
        const char *close = strchr(s, ')');
        uint8_t ace[1024];
        if (null_acl || !close || close - s - 1 > 600 || !sddl_ace(s + 1, (size_t)(close - s - 1), ace, err)) { free(b.p); return NULL; }
        ab_add(&b, ace);
        s = close + 1;
        while (*s == ' ') s++;
    }
    *control |= dacl ? SE_DACL_PRESENT : SE_SACL_PRESENT;
    if (null_acl) { free(b.p); *acl = NULL; }
    else *acl = ab_finish(&b);
    return s;
}
// An SDDL string as a self-relative descriptor for the program (LocalFree); NULL and *err when refused.
static uint8_t *sddl_parse(const char *s, DWORD *size, DWORD *err) {
    uint8_t owner[SID_MAX], group[SID_MAX], *dacl = NULL, *sacl = NULL;
    sd_parts p = { 0 };
    int seen[4] = { 0 };
    *err = ERROR_INVALID_PARAMETER;
    for (;;) {
        while (*s == ' ') s++;
        if (!*s) break;
        int c = toupper((unsigned char)s[0]), k = c == 'O' ? 0 : c == 'G' ? 1 : c == 'D' ? 2 : c == 'S' ? 3 : -1;
        if (k < 0 || s[1] != ':' || seen[k]++) goto fail;
        s += 2;
        while (*s == ' ') s++;
        if (k < 2) {
            const char *end;
            uint8_t *sid = k ? group : owner;
            if (!sid_parse(s, &end, sid)) { *err = ERROR_INVALID_SID; goto fail; }
            if (k) p.group = group; else p.owner = owner;
            s = end;
        } else {
            uint8_t **acl = k == 2 ? &dacl : &sacl;
            s = sddl_acl(s, &p.control, k == 2, acl, err);
            if (!s) goto fail;
        }
    }
    p.dacl = dacl; p.sacl = sacl;
    uint8_t *sd = sd_rel(&p, size, 1);
    free(dacl); free(sacl);
    if (!sd) *err = ERROR_NOT_ENOUGH_MEMORY;
    return sd;
fail:
    free(dacl); free(sacl);
    return NULL;
}
static void rights_format(DWORD mask, int label, char *out, size_t cap) {
    out[0] = 0;
    if (!mask) return;
    if (!label)
        for (size_t i = 0; i < COUNT(k_rights_whole); i++) if (k_rights_whole[i].v == mask) { snprintf(out, cap, "%s", k_rights_whole[i].s); return; }
    const name_value *t = label ? k_label_bits : k_rights_bits;
    size_t nt = label ? COUNT(k_label_bits) : COUNT(k_rights_bits), n = 0;
    DWORD left = mask;
    for (size_t i = 0; i < nt; i++) if (left & t[i].v) left &= ~t[i].v;
    if (left) { snprintf(out, cap, "0x%x", mask); return; }  // bits without a name: the whole mask in hex
    for (size_t i = 0; i < nt; i++) if (mask & t[i].v) n += (size_t)snprintf(out + n, cap - n, "%s", t[i].s);
}
static int sddl_format_acl(sbuf *b, const char *tag, uint16_t control, int dacl, const uint8_t *acl, int labels_only) {
    sb_add(b, tag);
    if (control & (dacl ? SE_DACL_PROTECTED : SE_SACL_PROTECTED)) sb_add(b, "P");
    if (control & (dacl ? SE_DACL_AUTO_INHERIT_REQ : SE_SACL_AUTO_INHERIT_REQ)) sb_add(b, "AR");
    if (control & (dacl ? SE_DACL_AUTO_INHERITED : SE_SACL_AUTO_INHERITED)) sb_add(b, "AI");
    if (!acl) { sb_add(b, "NO_ACCESS_CONTROL"); return 1; }
    const uint8_t *ace;
    FOR_ACE(acl, ace) {
        if (labels_only && ace[0] != SYSTEM_MANDATORY_LABEL) continue;
        const char *type = NULL;
        for (size_t i = 0; i < COUNT(k_ace_types); i++) if (k_ace_types[i].v == ace[0]) type = k_ace_types[i].s;
        if (!type) return 0;                              // (conditional or resource-attribute ACEs)
        char flags[24] = "", rights[64], obj[40] = "", inh[40] = "", sid[200];
        for (size_t i = 0; i < COUNT(k_ace_flags); i++) if (ace[1] & k_ace_flags[i].v) strcat(flags, k_ace_flags[i].s);
        rights_format(rd32(ace + 4), ace[0] == SYSTEM_MANDATORY_LABEL, rights, sizeof rights);
        if (ace_is_object(ace[0])) {
            DWORD f = rd32(ace + 8), off = 12;
            if (f & OBJECT_TYPE_PRESENT) { guid_format(ace + off, obj, sizeof obj); off += 16; }
            if (f & INHERITED_OBJECT_TYPE_PRESENT) guid_format(ace + off, inh, sizeof inh);
        }
        sid_format_sddl(ace_sid(ace, NULL), sid, sizeof sid);
        char text[512];
        snprintf(text, sizeof text, "(%s;%s;%s;%s;%s;%s)", type, flags, rights, obj, inh, sid);
        sb_add(b, text);
    }
    return 1;
}
static char *sddl_format(const sd_parts *p, DWORD info, DWORD *err) {   // malloc'd; NULL and *err when it cannot
    sbuf b = { 0 };
    char sid[200];
    sb_add(&b, "");
    if ((info & OWNER_SI) && p->owner) { sid_format_sddl(p->owner, sid, sizeof sid); sb_add(&b, "O:"); sb_add(&b, sid); }
    if ((info & GROUP_SI) && p->group) { sid_format_sddl(p->group, sid, sizeof sid); sb_add(&b, "G:"); sb_add(&b, sid); }
    int ok = 1;
    if ((info & DACL_SI) && (p->control & SE_DACL_PRESENT)) ok = sddl_format_acl(&b, "D:", p->control, 1, p->dacl, 0);
    if (ok && (info & (SACL_SI | LABEL_SI)) && (p->control & SE_SACL_PRESENT))
        ok = sddl_format_acl(&b, "S:", p->control, 0, p->sacl, !(info & SACL_SI));
    if (!ok) { free(b.p); *err = ERROR_NOT_SUPPORTED; return NULL; }
    return b.p;
}
static WCHAR *program_wide(const char *s) {               // LocalFree'able UTF-16 copy
    size_t n = vx_utf8_to_utf16(s, strlen(s) + 1, NULL, 0);
    WCHAR *w = vx_heap_alloc(2 * n);
    if (w) vx_utf8_to_utf16(s, strlen(s) + 1, w, n);
    return w;
}
static char *program_ansi(const char *s) { size_t n = strlen(s) + 1; char *a = vx_heap_alloc(n); if (a) memcpy(a, s, n); return a; }

// ---- tokens ------------------------------------------------------------------------------------------------------------
// A token is data: the user, groups (with the integrity label among them, SE_GROUP_INTEGRITY), restricting SIDs,
// privileges, owner, primary group, default DACL, and its own security descriptor once one is set. Handles hold
// references; the process's token lives as long as the process.
#define TOKEN_MAGIC 0x6e6b6f74u
#define VX_HANDLE_TOKEN 0x70                              // (vx_handle_new kind: tokens, with TOKEN_MAGIC checked too)
typedef struct { uint8_t sid[SID_MAX]; DWORD attr; } sid_attr;
typedef struct { DWORD luid, attr; } privilege;           // (every privilege's LUID has a high part of 0)
typedef struct {
    uint32_t magic; int refs;
    int type, level;                                      // TokenPrimary / TokenImpersonation; SECURITY_IMPERSONATION_LEVEL
    uint64_t id, modified;
    sid_attr user, *groups, *restricted;
    int ngroups, nrestricted;
    privilege *privs; int nprivs;
    uint8_t owner[SID_MAX], primary[SID_MAX], *dacl, *sd;
    DWORD session, policy, sandbox_inert, write_restricted, filtered, virt_enabled;
    DWORD is_restricted;                                  // restricting SIDs apply (even when none is left: then nothing passes)
} token;
static token *g_process_token;
static uint64_t g_luid = 0x10000;
static uint64_t new_luid(void) { return __atomic_add_fetch(&g_luid, 1, __ATOMIC_RELAXED); }

static token *token_new(void) {
    token *t = calloc(1, sizeof *t);
    t->magic = TOKEN_MAGIC; t->refs = 1; t->id = t->modified = new_luid();
    return t;
}
static void token_free(token *t) { free(t->groups); free(t->restricted); free(t->privs); free(t->dacl); free(t->sd); t->magic = 0; free(t); }
static token *token_ref(token *t) { if (t) __atomic_add_fetch(&t->refs, 1, __ATOMIC_ACQ_REL); return t; }
static void token_unref(token *t) { if (t && !__atomic_sub_fetch(&t->refs, 1, __ATOMIC_ACQ_REL)) token_free(t); }
static void add_sid_attr(sid_attr **list, int *n, const uint8_t *sid, DWORD attr) {
    *list = realloc(*list, (size_t)(*n + 1) * sizeof **list);
    memcpy((*list)[*n].sid, sid, sid_len(sid)); (*list)[*n].attr = attr; (*n)++;
}
static token *token_copy(const token *s) {               // (g_lock held) a new token with the same contents
    token *t = token_new();
    uint64_t id = t->id;
    *t = *s;
    t->refs = 1; t->id = t->modified = id; t->sd = NULL;
    t->groups = s->ngroups ? malloc((size_t)s->ngroups * sizeof *t->groups) : NULL;
    if (s->ngroups) memcpy(t->groups, s->groups, (size_t)s->ngroups * sizeof *t->groups);
    t->restricted = s->nrestricted ? malloc((size_t)s->nrestricted * sizeof *t->restricted) : NULL;
    if (s->nrestricted) memcpy(t->restricted, s->restricted, (size_t)s->nrestricted * sizeof *t->restricted);
    t->privs = s->nprivs ? malloc((size_t)s->nprivs * sizeof *t->privs) : NULL;
    if (s->nprivs) memcpy(t->privs, s->privs, (size_t)s->nprivs * sizeof *t->privs);
    t->dacl = s->dacl ? malloc(acl_bytes(s->dacl)) : NULL;
    if (s->dacl) memcpy(t->dacl, s->dacl, acl_bytes(s->dacl));
    return t;
}
// The user's token as Windows gives a standard user signed in at the console: groups None, Everyone, Users,
// INTERACTIVE, CONSOLE LOGON, Authenticated Users, This Organization, Local account, the logon session, LOCAL,
// NTLM Authentication and the medium integrity label; the five privileges a standard user has (only "bypass traverse
// checking" enabled).
static token *token_default(void) {
    token *t = token_new();
    uint8_t s[SID_MAX];
    t->type = TOKEN_PRIMARY;
    user_sid(t->user.sid);
    sid_of(s, 5, 5, MACHINE_SUBS, NONE_RID); add_sid_attr(&t->groups, &t->ngroups, s, GROUP_ON);
    sid_of(s, 1, 1, 0); add_sid_attr(&t->groups, &t->ngroups, s, GROUP_ON);
    sid_of(s, 5, 2, 32, 545); add_sid_attr(&t->groups, &t->ngroups, s, GROUP_ON);
    sid_of(s, 5, 1, 4); add_sid_attr(&t->groups, &t->ngroups, s, GROUP_ON);
    sid_of(s, 2, 1, 1); add_sid_attr(&t->groups, &t->ngroups, s, GROUP_ON);
    sid_of(s, 5, 1, 11); add_sid_attr(&t->groups, &t->ngroups, s, GROUP_ON);
    sid_of(s, 5, 1, 15); add_sid_attr(&t->groups, &t->ngroups, s, GROUP_ON);
    sid_of(s, 5, 1, 113); add_sid_attr(&t->groups, &t->ngroups, s, GROUP_ON);
    sid_of(s, 5, 3, 5, 0, LOGON_ID); add_sid_attr(&t->groups, &t->ngroups, s, GROUP_ON | SE_GROUP_LOGON_ID);
    sid_of(s, 2, 1, 0); add_sid_attr(&t->groups, &t->ngroups, s, GROUP_ON);
    sid_of(s, 5, 2, 64, 10); add_sid_attr(&t->groups, &t->ngroups, s, GROUP_ON);
    sid_of(s, 16, 1, 8192); add_sid_attr(&t->groups, &t->ngroups, s, SE_GROUP_INTEGRITY | SE_GROUP_INTEGRITY_ENABLED);
    static const DWORD privs[] = { 19, SE_CHANGE_NOTIFY_PRIVILEGE, 25, 33, 34 };   // shutdown, change notify, undock, working set, time zone
    t->privs = calloc(COUNT(privs), sizeof *t->privs);
    for (size_t i = 0; i < COUNT(privs); i++) {
        t->privs[i].luid = privs[i];
        t->privs[i].attr = privs[i] == SE_CHANGE_NOTIFY_PRIVILEGE ? SE_PRIVILEGE_ENABLED | SE_PRIVILEGE_ENABLED_BY_DEFAULT : 0;
    }
    t->nprivs = (int)COUNT(privs);
    memcpy(t->owner, t->user.sid, SID_MAX);
    sid_of(t->primary, 5, 5, MACHINE_SUBS, NONE_RID);
    // the default DACL for what the user creates: the user and SYSTEM all access, the logon session read and execute
    acl_buf b; ab_init(&b);
    uint8_t ace[8 + SID_MAX];
    ace_make(ace, ACCESS_ALLOWED, 0, GENERIC_ALL, t->user.sid, NULL, NULL); ab_add(&b, ace);
    sid_of(s, 5, 1, 18); ace_make(ace, ACCESS_ALLOWED, 0, GENERIC_ALL, s, NULL, NULL); ab_add(&b, ace);
    sid_of(s, 5, 3, 5, 0, LOGON_ID); ace_make(ace, ACCESS_ALLOWED, 0, GENERIC_READ | GENERIC_EXECUTE, s, NULL, NULL); ab_add(&b, ace);
    t->dacl = ab_finish(&b);
    t->session = 1;
    t->policy = 3;                                        // TOKEN_MANDATORY_POLICY_NO_WRITE_UP | NEW_PROCESS_MIN
    return t;
}
static token *process_token(void) {                      // (g_lock held) this process's, made once; changes to it stay
    if (!g_process_token) g_process_token = token_default();
    return g_process_token;
}
static sid_attr *token_label(token *t) {
    for (int i = 0; i < t->ngroups; i++) if (t->groups[i].attr & SE_GROUP_INTEGRITY) return &t->groups[i];
    return NULL;
}
static DWORD token_integrity(token *t) { sid_attr *l = token_label(t); return l && l->sid[1] ? sid_sub(l->sid, 0) : 8192; }

// Impersonation: each thread's token, by thread id.
typedef struct { DWORD tid; token *t; } thread_token;
static thread_token *g_threads;
static int g_nthreads;
static token *thread_token_of(DWORD tid) {               // (g_lock held) no reference taken
    for (int i = 0; i < g_nthreads; i++) if (g_threads[i].tid == tid) return g_threads[i].t;
    return NULL;
}
static void thread_token_set(DWORD tid, token *t) {      // (g_lock held) takes over the caller's reference to t
    for (int i = 0; i < g_nthreads; i++)
        if (g_threads[i].tid == tid) {
            token_unref(g_threads[i].t);
            if (t) g_threads[i].t = t; else g_threads[i] = g_threads[--g_nthreads];
            return;
        }
    if (!t) return;
    g_threads = realloc(g_threads, (size_t)(g_nthreads + 1) * sizeof *g_threads);
    g_threads[g_nthreads++] = (thread_token){ tid, t };
}
int vx_process_info(HANDLE h, DWORD *code);               // sync.c: a process handle's pid (-1 if not one)
// A thread handle's thread id, 0 if it is none of this process's threads. *foreign: it is another process's thread
// (CreateProcess's hThread, which here is a second handle to the child process), whose tokens this process cannot
// see or set: each process keeps its own.
static DWORD thread_id_of(HANDLE h, int *foreign) {
    *foreign = 0;
    if ((intptr_t)h == -2) return vx_current_tid();
    DWORD (WINAPI *get)(HANDLE) = (DWORD (WINAPI *)(HANDLE))vx_find_export("kernel32.dll", "GetThreadId");
    DWORD tid = h && get ? get(h) : 0;
    if (!tid && h) { int pid = vx_process_info(h, NULL); *foreign = pid > 0 && pid != getpid(); }
    return tid;
}
// For a thread handle that is not one: ERROR_INVALID_HANDLE, or ERROR_NOT_SUPPORTED (logged) for another process's.
static BOOL no_thread(int foreign, const char *fn) {
    if (foreign) vx_log("%s: a thread of another process: its tokens are that process's own, out of this one's reach (refused)", fn);
    vx_set_last_error(foreign ? ERROR_NOT_SUPPORTED : ERROR_INVALID_HANDLE);
    return 0;
}

static void token_handle_closed(void *t) { token_unref(t); }
static HANDLE token_handle(token *t) {                    // a new handle holding its own reference
    token_ref(t);
    HANDLE h = vx_handle_new(VX_HANDLE_TOKEN, t, token_handle_closed);
    if (!h) { token_unref(t); vx_set_last_error(ERROR_NOT_ENOUGH_MEMORY); }
    return h;
}
// The token a handle means, referenced; the pseudo-handles of GetCurrentProcessToken (-4), GetCurrentThreadToken
// (-5) and GetCurrentThreadEffectiveToken (-6) included. NULL and *err if none.
static token *token_get(HANDLE h, DWORD *err) {
    intptr_t v = (intptr_t)h;
    token *t = NULL;
    if (v >= -6 && v <= -4) {
        pthread_mutex_lock(&g_lock);
        t = v == -4 ? NULL : thread_token_of(vx_current_tid());
        if (!t && v != -5) t = process_token();
        token_ref(t);
        pthread_mutex_unlock(&g_lock);
        if (!t) *err = ERROR_NO_TOKEN;
        return t;
    }
    t = h ? vx_handle_data(h, VX_HANDLE_TOKEN) : NULL;
    if (!t || t->magic != TOKEN_MAGIC) { *err = ERROR_INVALID_HANDLE; return NULL; }
    return token_ref(t);
}
static int token_has_sid(const token *t, const uint8_t *sid, int deny, int restricted) {
    if (restricted) {
        for (int i = 0; i < t->nrestricted; i++) if ((t->restricted[i].attr & SE_GROUP_ENABLED) && sid_eq(t->restricted[i].sid, sid)) return 1;
        return 0;
    }
    if (sid_eq(t->user.sid, sid) && (deny || !(t->user.attr & SE_GROUP_USE_FOR_DENY_ONLY))) return 1;
    for (int i = 0; i < t->ngroups; i++) {
        DWORD a = t->groups[i].attr;
        if (!sid_eq(t->groups[i].sid, sid)) continue;
        if (deny ? (a & (SE_GROUP_ENABLED | SE_GROUP_USE_FOR_DENY_ONLY)) != 0 : (a & SE_GROUP_ENABLED) && !(a & SE_GROUP_USE_FOR_DENY_ONLY)) return 1;
    }
    return 0;
}

// ---- GetTokenInformation's layouts ------------------------------------------------------------------------------------
// Built at offsets in a growing buffer; the pointers in it (to SIDs and arrays) are made when it is copied out.
typedef struct { uint8_t *p; size_t n, cap; size_t (*fix)[2]; int nfix; } blob;
static size_t b_room(blob *b, size_t n, size_t align) {
    size_t at = (b->n + align - 1) & ~(align - 1);
    if (at + n > b->cap) { b->cap = (at + n) * 2 + 64; b->p = realloc(b->p, b->cap); }
    memset(b->p + b->n, 0, at + n - b->n);
    b->n = at + n;
    return at;
}
static void b_ptr(blob *b, size_t at, size_t to) {
    b->fix = realloc(b->fix, (size_t)(b->nfix + 1) * sizeof *b->fix);
    b->fix[b->nfix][0] = at; b->fix[b->nfix++][1] = to;
}
static size_t b_sid(blob *b, const uint8_t *sid) { size_t at = b_room(b, sid_len(sid), 4); memcpy(b->p + at, sid, sid_len(sid)); return at; }
static void b_sid_ptr(blob *b, size_t at, const uint8_t *sid) { size_t s = b_sid(b, sid); b_ptr(b, at, s); }
static void b_out(const blob *b, uint8_t *out) {
    memcpy(out, b->p, b->n);
    for (int i = 0; i < b->nfix; i++) wrptr(out + b->fix[i][0], out + b->fix[i][1]);
}
static size_t b_sid_array(blob *b, const sid_attr *first, const sid_attr *g, int n) {   // SID_AND_ATTRIBUTES[]
    int total = n + (first != NULL);
    size_t at = b_room(b, 16 * (size_t)total, 8);
    for (int i = 0; i < total; i++) {
        const sid_attr *e = first ? (i ? &g[i - 1] : first) : &g[i];
        b_sid_ptr(b, at + 16 * (size_t)i, e->sid);
        wr32(b->p + at + 16 * (size_t)i + 8, e->attr);
    }
    return at;
}
static void b_groups(blob *b, const sid_attr *g, int n) {   // TOKEN_GROUPS
    size_t head = b_room(b, 8, 8);
    wr32(b->p + head, (DWORD)n);
    if (n) b_sid_array(b, NULL, g, n);
}
static size_t b_privileges(blob *b, const token *t) {   // LUID_AND_ATTRIBUTES[]
    size_t at = b_room(b, 12 * (size_t)t->nprivs, 4);
    for (int i = 0; i < t->nprivs; i++) { wr32(b->p + at + 12 * i, t->privs[i].luid); wr32(b->p + at + 12 * i + 8, t->privs[i].attr); }
    return at;
}
static void b_dword(blob *b, DWORD v) { size_t at = b_room(b, 4, 4); wr32(b->p + at, v); }

// ---- access checks ------------------------------------------------------------------------------------------------------
// One pass over the DACL with one set of SIDs (the token's, or its restricting SIDs): what it grants of `want` (of
// everything when `maximum`). Deny ACEs before allow ACEs win, as the DACL's order says; the owner has READ_CONTROL
// and WRITE_DAC unless an OWNER RIGHTS ACE says otherwise; conditional allow ACEs (whose conditions are not
// evaluated) grant nothing and conditional deny ACEs deny.
static DWORD dacl_pass(const token *t, int restricted, const sd_parts *p, DWORD want, int maximum, DWORD all) {
    if (!p->dacl) return maximum ? all : want;            // no DACL (or a NULL one): everything
    uint8_t owner_rights[12];
    sid_of(owner_rights, 3, 1, 4);
    DWORD granted = 0, denied = 0;
    int owner_ace = 0;
    const uint8_t *ace;
    FOR_ACE(p->dacl, ace) if (!(ace[1] & INHERIT_ONLY) && ace_has_sid(ace[0]) && sid_eq(ace_sid(ace, NULL), owner_rights)) owner_ace = 1;
    if (!owner_ace && p->owner && token_has_sid(t, p->owner, 0, restricted)) granted |= READ_CONTROL | WRITE_DAC;
    FOR_ACE(p->dacl, ace) {
        if (ace[1] & INHERIT_ONLY) continue;
        uint8_t type = ace[0];
        if ((type == ACCESS_ALLOWED_OBJECT || type == ACCESS_DENIED_OBJECT) && (rd32(ace + 8) & OBJECT_TYPE_PRESENT)) continue;   // (for one property set only)
        int allow = type == ACCESS_ALLOWED || type == ACCESS_ALLOWED_OBJECT;
        int deny = type == ACCESS_DENIED || type == ACCESS_DENIED_OBJECT || type == ACCESS_DENIED_CALLBACK || type == ACCESS_DENIED_CALLBACK_OBJECT;
        if (!allow && !deny) continue;
        const uint8_t *sid = ace_sid(ace, NULL);
        DWORD mask = rd32(ace + 4);
        if (sid_eq(sid, owner_rights)) { if (!p->owner || !token_has_sid(t, p->owner, deny, restricted)) continue; }
        else if (!token_has_sid(t, sid, deny, restricted)) continue;
        if (allow) granted |= mask & ~denied;
        else denied |= mask & ~granted;
        if (!maximum && (granted & want) == want) break;
    }
    return granted;
}
// What the mandatory policy takes away: a token below the object's integrity level may not write (or, when the
// label says so, read or execute) up.
static DWORD integrity_denies(token *t, const sd_parts *p, const DWORD *map) {
    if (!(t->policy & 1)) return 0;                       // (TOKEN_MANDATORY_POLICY_NO_WRITE_UP off)
    DWORD level = 8192, policy = NO_WRITE_UP;             // unlabelled objects: medium, no write up
    const uint8_t *ace;
    if (p->sacl) FOR_ACE(p->sacl, ace) if (ace[0] == SYSTEM_MANDATORY_LABEL && !(ace[1] & INHERIT_ONLY)) {
        const uint8_t *sid = ace_sid(ace, NULL);
        level = sid[1] ? sid_sub(sid, sid[1] - 1) : 0; policy = rd32(ace + 4);
        break;
    }
    if (token_integrity(t) >= level) return 0;
    DWORD keep = READ_CONTROL | SYNCHRONIZE, d = 0;
    if (policy & NO_WRITE_UP) d |= map[1] | DELETE_RIGHT | WRITE_DAC | WRITE_OWNER;
    if (policy & NO_READ_UP) d |= map[0];
    if (policy & NO_EXECUTE_UP) d |= map[2];
    return d & ~keep;
}
// The access `t` has to an object with descriptor p: 0 when denied.
static DWORD access_of(token *t, const sd_parts *p, DWORD desired, const DWORD *map) {
    int maximum = (desired & MAXIMUM_ALLOWED) != 0;
    DWORD want = desired & ~MAXIMUM_ALLOWED, all = map[3] | want;
    DWORD g = dacl_pass(t, 0, p, want, maximum, all);
    if (t->is_restricted) {
        DWORD r = dacl_pass(t, 1, p, want, maximum, all);
        DWORD writes = map[1] | DELETE_RIGHT | WRITE_DAC | WRITE_OWNER;
        g &= t->write_restricted ? r | ~writes : r;       // WRITE_RESTRICTED: only writing is checked twice
    }
    g &= ~integrity_denies(t, p, map);
    if (maximum) return (g & want) == want ? g : 0;
    return want && (g & want) == want ? want : 0;
}

// ---- names ---------------------------------------------------------------------------------------------------------------
// LookupAccountSid / LookupAccountName: the user, the machine's None group, and the well-known names an English Windows
// gives (SID_NAME_USE: 1 user, 2 group, 4 alias, 5 well-known group, 10 label).
typedef struct { const char *name, *domain; uint8_t use, auth, n; DWORD sub[2]; } account;
static const account k_accounts[] = {
    { "Everyone", "", 5, 1, 1, { 0 } }, { "LOCAL", "", 5, 2, 1, { 0 } }, { "CONSOLE LOGON", "", 5, 2, 1, { 1 } },
    { "CREATOR OWNER", "", 5, 3, 1, { 0 } }, { "CREATOR GROUP", "", 5, 3, 1, { 1 } }, { "OWNER RIGHTS", "", 5, 3, 1, { 4 } },
    { "NETWORK", "NT AUTHORITY", 5, 5, 1, { 2 } }, { "INTERACTIVE", "NT AUTHORITY", 5, 5, 1, { 4 } },
    { "SERVICE", "NT AUTHORITY", 5, 5, 1, { 6 } }, { "ANONYMOUS LOGON", "NT AUTHORITY", 5, 5, 1, { 7 } },
    { "SELF", "NT AUTHORITY", 5, 5, 1, { 10 } }, { "Authenticated Users", "NT AUTHORITY", 5, 5, 1, { 11 } },
    { "RESTRICTED", "NT AUTHORITY", 5, 5, 1, { 12 } }, { "This Organization", "NT AUTHORITY", 5, 5, 1, { 15 } },
    { "SYSTEM", "NT AUTHORITY", 5, 5, 1, { 18 } }, { "LOCAL SERVICE", "NT AUTHORITY", 5, 5, 1, { 19 } },
    { "NETWORK SERVICE", "NT AUTHORITY", 5, 5, 1, { 20 } }, { "WRITE RESTRICTED", "NT AUTHORITY", 5, 5, 1, { 33 } },
    { "Local account", "NT AUTHORITY", 5, 5, 1, { 113 } }, { "NTLM Authentication", "NT AUTHORITY", 5, 5, 2, { 64, 10 } },
    { "Administrators", "BUILTIN", 4, 5, 2, { 32, 544 } }, { "Users", "BUILTIN", 4, 5, 2, { 32, 545 } },
    { "Guests", "BUILTIN", 4, 5, 2, { 32, 546 } },
    { "ALL APPLICATION PACKAGES", "APPLICATION PACKAGE AUTHORITY", 5, 15, 2, { 2, 1 } },
    { "Untrusted Mandatory Level", "Mandatory Label", 10, 16, 1, { 0 } }, { "Low Mandatory Level", "Mandatory Label", 10, 16, 1, { 4096 } },
    { "Medium Mandatory Level", "Mandatory Label", 10, 16, 1, { 8192 } }, { "Medium Plus Mandatory Level", "Mandatory Label", 10, 16, 1, { 8448 } },
    { "High Mandatory Level", "Mandatory Label", 10, 16, 1, { 12288 } }, { "System Mandatory Level", "Mandatory Label", 10, 16, 1, { 16384 } },
};
static void name_from_export(const char *fn, char *out, size_t cap) {
    BOOL (WINAPI *get)(WCHAR *, DWORD *) = (BOOL (WINAPI *)(WCHAR *, DWORD *))vx_find_export(!strcmp(fn, "GetUserNameW") ? "advapi32.dll" : "kernel32.dll", fn);
    WCHAR w[256]; DWORD n = 256;
    out[0] = 0;
    if (get && get(w, &n)) vx_utf16_to_utf8(w, vx_wcslen(w) + 1, out, cap);
    if (!out[0]) snprintf(out, cap, "%s", !strcmp(fn, "GetUserNameW") ? "user" : "VXRT");
}
// The account a SID names: 1 and the name, domain and use; 0 if none.
static int account_of_sid(const uint8_t *sid, char *name, size_t nc, char *domain, size_t dc, DWORD *use) {
    DWORD rid;
    if (machine_rid(sid, &rid) && (rid == USER_RID || rid == NONE_RID)) {
        if (rid == USER_RID) name_from_export("GetUserNameW", name, nc); else snprintf(name, nc, "None");
        name_from_export("GetComputerNameW", domain, dc);
        *use = rid == USER_RID ? 1 : 2;
        return 1;
    }
    uint8_t s[SID_MAX];
    for (size_t i = 0; i < COUNT(k_accounts); i++)
        if (sid_make(s, k_accounts[i].auth, k_accounts[i].n, k_accounts[i].sub) && sid_eq(s, sid)) {
            snprintf(name, nc, "%s", k_accounts[i].name); snprintf(domain, dc, "%s", k_accounts[i].domain);
            *use = k_accounts[i].use;
            return 1;
        }
    return 0;
}
// A name ("Users", "BUILTIN\Users", the user's own, CURRENT_USER) as a SID: its length, 0 if unknown.
static DWORD sid_of_name(const char *full, uint8_t *sid, char *domain, size_t dc, DWORD *use) {
    const char *slash = strchr(full, '\\'), *name = slash ? slash + 1 : full;
    size_t dl = slash ? (size_t)(slash - full) : 0;
    char user[256], machine[256];
    name_from_export("GetUserNameW", user, sizeof user);
    name_from_export("GetComputerNameW", machine, sizeof machine);
    if ((!slash && !strcmp(full, "CURRENT_USER")) || (!strcasecmp(name, user) && (!slash || (dl == strlen(machine) && !strncasecmp(full, machine, dl))))) {
        user_sid(sid); if (domain) snprintf(domain, dc, "%s", machine); if (use) *use = 1;
        return sid_len(sid);
    }
    if (!strcasecmp(name, "None") && (!slash || (dl == strlen(machine) && !strncasecmp(full, machine, dl)))) {
        sid_of(sid, 5, 5, MACHINE_SUBS, NONE_RID); if (domain) snprintf(domain, dc, "%s", machine); if (use) *use = 2;
        return sid_len(sid);
    }
    for (size_t i = 0; i < COUNT(k_accounts); i++) {
        const account *a = &k_accounts[i];
        if (strcasecmp(name, a->name) || (slash && (dl != strlen(a->domain) || strncasecmp(full, a->domain, dl)))) continue;
        if (domain) snprintf(domain, dc, "%s", a->domain);
        if (use) *use = a->use;
        return sid_make(sid, a->auth, a->n, a->sub);
    }
    return 0;
}

// ---- trustees and explicit access (aclapi) ------------------------------------------------------------------------------
// TRUSTEE: pMultipleTrustee, MultipleTrusteeOperation, TrusteeForm (+12), TrusteeType (+16), ptstrName (+24): 32
// bytes. EXPLICIT_ACCESS: grfAccessPermissions, grfAccessMode, grfInheritance, Trustee (+16): 48 bytes.
// OBJECTS_AND_SID: ObjectsPresent, two GUIDs (+4, +20), pSid (+40).
enum { TRUSTEE_IS_SID = 0, TRUSTEE_IS_NAME = 1, TRUSTEE_IS_OBJECTS_AND_SID = 3, TRUSTEE_IS_OBJECTS_AND_NAME = 4 };
// The SID (and object GUIDs) a trustee names; 0 and an error code if it cannot.
static DWORD trustee_sid(const uint8_t *tr, int wide, uint8_t *sid, uint8_t *obj, uint8_t *inh, int *objects) {
    *objects = 0;
    if (rdptr(tr)) return ERROR_NOT_SUPPORTED;            // (multiple trustees: unsupported on Windows too)
    DWORD form = rd32(tr + 12);
    const uint8_t *name = rdptr(tr + 24);
    if (form == TRUSTEE_IS_OBJECTS_AND_SID) {
        if (!name) return ERROR_INVALID_PARAMETER;
        DWORD present = rd32(name);
        if (present & OBJECT_TYPE_PRESENT) { memcpy(obj, name + 4, 16); *objects |= 1; }
        if (present & INHERITED_OBJECT_TYPE_PRESENT) { memcpy(inh, name + 20, 16); *objects |= 2; }
        *objects |= 4;
        name = rdptr(name + 40);
        form = TRUSTEE_IS_SID;
    } else if (form == TRUSTEE_IS_OBJECTS_AND_NAME) {
        if (!name || rd32(name)) return ERROR_NOT_SUPPORTED;   // (object type names are a directory's)
        name = rdptr(name + 24);
        form = TRUSTEE_IS_NAME;
    }
    if (form == TRUSTEE_IS_SID) {
        if (!sid_ok(name)) return ERROR_INVALID_SID;
        memcpy(sid, name, sid_len(name));
        return 0;
    }
    if (form != TRUSTEE_IS_NAME || !name) return ERROR_INVALID_PARAMETER;
    char text[512];
    if (wide) vx_utf16_to_utf8((const WCHAR *)name, vx_wcslen((const WCHAR *)name) + 1, text, sizeof text);
    else snprintf(text, sizeof text, "%s", (const char *)name);
    return sid_of_name(text, sid, NULL, 0, NULL) ? 0 : ERROR_NONE_MAPPED;
}
// SetEntriesInAcl: the old ACL's ACEs changed by each entry in turn (GRANT adds rights to the trustee's allow ACE, SET
// replaces the trustee's allow and deny ACEs, DENY adds a deny ACE, REVOKE removes its allow ACEs, the audit modes
// add audit ACEs), then put in the canonical order: explicit deny, explicit others, inherited. A malloc'd ACL.
static DWORD entries_in_acl(DWORD n, const uint8_t *entries, const uint8_t *old, int wide, uint8_t **out) {
    if (old && !acl_ok(old)) return ERROR_INVALID_ACL;
    if (n && !entries) return ERROR_INVALID_PARAMETER;
    int cap = (old ? rd16(old + 4) : 0) + (int)n + 1, count = 0;
    uint8_t **aces = calloc((size_t)cap, sizeof *aces);
    const uint8_t *ace;
    if (old) FOR_ACE(old, ace) { aces[count] = malloc(rd16(ace + 2)); memcpy(aces[count++], ace, rd16(ace + 2)); }
    DWORD err = 0;
    for (DWORD e = 0; e < n && !err; e++) {
        const uint8_t *x = entries + 48 * (size_t)e;
        DWORD mask = rd32(x), mode = rd32(x + 4), inherit = rd32(x + 8) & 0xf;
        uint8_t sid[SID_MAX], obj[16], inh[16];
        int objects;
        if (mode == 0) continue;                          // NOT_USED_ACCESS
        if (mode > 6) { err = ERROR_INVALID_PARAMETER; break; }
        if ((err = trustee_sid(x + 16, wide, sid, obj, inh, &objects))) break;
        int kind = mode == 3 ? 1 : mode >= 5 ? 2 : 0;     // 0 allow, 1 deny, 2 audit
        uint8_t type = (uint8_t)(kind == 0 ? ACCESS_ALLOWED : kind == 1 ? ACCESS_DENIED : SYSTEM_AUDIT);
        if (objects) type = (uint8_t)(type == ACCESS_ALLOWED ? ACCESS_ALLOWED_OBJECT : type == ACCESS_DENIED ? ACCESS_DENIED_OBJECT : SYSTEM_AUDIT_OBJECT);
        uint8_t flags = (uint8_t)(inherit | (mode == 5 ? SUCCESSFUL_ACCESS : mode == 6 ? FAILED_ACCESS : 0));
        // drop what SET and REVOKE replace
        for (int i = 0; i < count; i++) {
            const uint8_t *a = aces[i];
            if ((a[1] & INHERITED) || !ace_has_sid(a[0]) || !sid_eq(ace_sid(a, NULL), sid)) continue;
            int is_allow = a[0] == ACCESS_ALLOWED || a[0] == ACCESS_ALLOWED_OBJECT, is_deny = a[0] == ACCESS_DENIED || a[0] == ACCESS_DENIED_OBJECT;
            int is_audit = a[0] == SYSTEM_AUDIT || a[0] == SYSTEM_AUDIT_OBJECT;
            if ((mode == 2 && (is_allow || is_deny)) || (mode == 4 && (is_allow || is_audit))) {   // (the others keep their order)
                free(aces[i]);
                memmove(&aces[i], &aces[i + 1], (size_t)(count - i - 1) * sizeof *aces);
                count--; i--;
            }
        }
        if (mode == 4) continue;
        int merged = 0;                                   // GRANT, DENY and the audits add to the trustee's ACE of that kind
        for (int i = 0; i < count && !merged && mode != 2; i++) {
            uint8_t *a = aces[i];
            if (a[0] != type || (a[1] & ~(SUCCESSFUL_ACCESS | FAILED_ACCESS)) != inherit || ace_is_object(type) || !sid_eq(ace_sid(a, NULL), sid)) continue;
            wr32(a + 4, rd32(a + 4) | mask); a[1] |= flags; merged = 1;
        }
        if (merged) continue;
        if (count == cap) aces = realloc(aces, (size_t)(cap *= 2) * sizeof *aces);
        aces[count] = malloc(12 + 32 + SID_MAX);
        ace_make(aces[count++], type, flags, mask, sid, objects & 1 ? obj : NULL, objects & 2 ? inh : NULL);
    }
    if (!err) {
        acl_buf b; ab_init(&b);
        for (int pass = 0; pass < 3; pass++)
            for (int i = 0; i < count; i++) {
                const uint8_t *a = aces[i];
                int deny = a[0] == ACCESS_DENIED || a[0] == ACCESS_DENIED_OBJECT, inherited = (a[1] & INHERITED) != 0;
                if ((pass == 0 && deny && !inherited) || (pass == 1 && !deny && !inherited) || (pass == 2 && inherited)) ab_add(&b, a);
            }
        *out = ab_finish(&b);
    }
    for (int i = 0; i < count; i++) free(aces[i]);
    free(aces);
    return err;
}

// ---- the security of objects -----------------------------------------------------------------------------------------
// A descriptor reported for an object says what the Mac really lets each account do to it, and a descriptor set on an
// object either takes effect or is refused: nothing is reported as protected that is not.
//  - Files and folders: their owner, group and mode bits, read with stat() every time (sd_of_file). A DACL set on one
//    is carried out with chmod as far as mode bits can carry it: what it takes from the owner, the file's group or
//    everyone else is taken away; what it would add is not (loosening is let pass, unapplied). When the Mac would
//    still give someone a right the DACL takes from them (a folder others may write to, a macOS ACL, a file the user
//    does not own), it is refused and nothing changes.
//  - Registry keys: all of them live in registry.txt, so other accounts may do what its mode bits let them.
//  - Kernel objects, tokens, the process and its threads: only this user's processes reach them (this process's
//    memory, or shared memory only the user may open), and nothing checks a descriptor when one of them opens an
//    object. A descriptor is accepted (and kept, to be reported back, where there is a place for it: tokens, the
//    process, named objects) when it leaves the user's own token every right to the object, for then it takes nothing
//    from anyone who can reach the object; one that would take a right from the user is refused with
//    ERROR_NOT_SUPPORTED, as nothing could hold anyone to it.
// The user's own token is the one every process here runs with (token_default): restricted and lower-integrity tokens
// never leave this process (CreateProcessAsUser refuses them), so apart from the user only other macOS accounts are
// there to keep out. What is compared for the user: a file's data (reading, writing, appending, running it), every
// right to other objects but READ_CONTROL, WRITE_DAC (an owner has both on Windows too), WRITE_OWNER and DELETE; a
// descriptor cannot take those, a file's attributes or its removal (its folder's business on the Mac) from the user
// here. For other accounts every right is compared but seeing the descriptor. Windows' SYSTEM and administrators
// have no counterpart a descriptor could keep out (root is not checked by the Mac either).
typedef struct { int type; char *name; uint8_t *sd; } named_sd;
static named_sd *g_named;
static int g_nnamed;
static uint8_t *g_process_sd;

// GENERIC_READ, _WRITE, _EXECUTE and _ALL for a type (its GENERIC_MAPPING): files' and registry keys' as documented;
// for kernel objects only GENERIC_ALL is taken to mean more than reading the descriptor, which can only make a
// descriptor seem to give the user less (and so be refused), never more.
static const DWORD k_file_map[4] = { 0x120089, 0x120116, 0x1200a0, 0x1f01ff };   // FILE_GENERIC_*, FILE_ALL_ACCESS
static const DWORD k_key_map[4] = { 0x20019, 0x20006, 0x20019, 0xf003f };        // KEY_READ, KEY_WRITE, KEY_EXECUTE, KEY_ALL_ACCESS
#define TOKEN_ALL_ACCESS 0xf01ffu
static void kernel_map(DWORD all, DWORD *map) { map[0] = map[1] = READ_CONTROL; map[2] = READ_CONTROL | SYNCHRONIZE; map[3] = all; }
static int is_key_type(int type) { return type == SE_REGISTRY_KEY || type == SE_REGISTRY_WOW64_32KEY || type == SE_REGISTRY_WOW64_64KEY; }
#define USER_KEEPS (~(DWORD)(READ_CONTROL | WRITE_DAC | WRITE_OWNER | DELETE_RIGHT))   // (see above)

// What full access to a kernel object behind a handle is (its type's *_ALL_ACCESS: Microsoft's "Security and Access
// Rights" page of each type); every standard and specific right (0x1fffff) for a type not known here.
const char *vx_object_type(HANDLE h);                     // sync.c
static DWORD kernel_all_access(HANDLE h) {
    if ((intptr_t)h == -1 || (intptr_t)h == -2) return 0x1fffff;                 // PROCESS_ALL_ACCESS, THREAD_ALL_ACCESS
    if (vx_handle_data(h, VX_HANDLE_MAPPING)) return 0xf001f;                     // SECTION_ALL_ACCESS
    const char *k = vx_object_type(h);
    if (k && (!strcmp(k, "Event") || !strcmp(k, "Semaphore") || !strcmp(k, "IoCompletion"))) return 0x1f0003;
    if (k && !strcmp(k, "Mutant")) return 0x1f0001;                               // MUTANT_ALL_ACCESS
    return 0x1fffff;
}

// A copy of an ACL (malloc'd; NULL for NULL) with each ACE's generic rights mapped, as Windows stores them on an object.
static uint8_t *acl_mapped(const uint8_t *acl, const DWORD *map) {
    if (!acl) return NULL;
    uint8_t *c = calloc(1, acl_bytes(acl)), *ace;
    memcpy(c, acl, rd16(acl + 2));
    FOR_ACE(c, ace) if (ace_has_sid(ace[0]) && ace[0] != SYSTEM_MANDATORY_LABEL) {
        DWORD m = rd32(ace + 4);
        if (m & GENERIC_READ) m |= map[0];
        if (m & GENERIC_WRITE) m |= map[1];
        if (m & GENERIC_EXECUTE) m |= map[2];
        if (m & GENERIC_ALL) m |= map[3];
        wr32(ace + 4, m & ~GENERIC_BITS);
    }
    return c;
}

// Another local account (made up: someone else on the Mac), in the groups every local account is in, and `group`.
#define OTHER_RID 0x7ffffffeu
static token *token_other(const uint8_t *group) {
    token *t = token_new();
    uint8_t s[SID_MAX];
    t->type = TOKEN_IMPERSONATION; t->level = SECURITY_IMPERSONATION;
    sid_of(t->user.sid, 5, 5, MACHINE_SUBS, OTHER_RID);
    sid_of(s, 5, 5, MACHINE_SUBS, NONE_RID); add_sid_attr(&t->groups, &t->ngroups, s, GROUP_ON);
    sid_of(s, 1, 1, 0); add_sid_attr(&t->groups, &t->ngroups, s, GROUP_ON);
    sid_of(s, 5, 2, 32, 545); add_sid_attr(&t->groups, &t->ngroups, s, GROUP_ON);
    sid_of(s, 5, 1, 4); add_sid_attr(&t->groups, &t->ngroups, s, GROUP_ON);
    sid_of(s, 2, 1, 0); add_sid_attr(&t->groups, &t->ngroups, s, GROUP_ON);
    sid_of(s, 5, 1, 11); add_sid_attr(&t->groups, &t->ngroups, s, GROUP_ON);
    sid_of(s, 5, 1, 15); add_sid_attr(&t->groups, &t->ngroups, s, GROUP_ON);
    sid_of(s, 5, 1, 113); add_sid_attr(&t->groups, &t->ngroups, s, GROUP_ON);
    if (group) add_sid_attr(&t->groups, &t->ngroups, group, GROUP_ON);
    sid_of(s, 16, 1, 8192); add_sid_attr(&t->groups, &t->ngroups, s, SE_GROUP_INTEGRITY | SE_GROUP_INTEGRITY_ENABLED);
    memcpy(t->owner, t->user.sid, SID_MAX);
    sid_of(t->primary, 5, 5, MACHINE_SUBS, NONE_RID);
    t->policy = 3;
    return t;
}
// The rights of `rel` that `real` (what the Mac does) gives `t` and `want` (a descriptor asked for) does not: the
// protection `want` would promise and not have.
static DWORD taken_from(token *t, const uint8_t *want, const uint8_t *real, const DWORD *map, DWORD rel) {
    sd_parts w, r;
    sd_read(want, &w); sd_read(real, &r);
    return access_of(t, &r, MAXIMUM_ALLOWED, map) & ~access_of(t, &w, MAXIMUM_ALLOWED, map) & rel;
}

// Whether every directory above `path` lets other accounts through (search permission for the group or the rest).
static int others_reach(const char *path) {
    char dir[1100];
    if (!realpath(path, dir)) snprintf(dir, sizeof dir, "%s", path);
    for (char *slash; (slash = strrchr(dir, '/')); ) {
        if (slash == dir) slash[1] = 0; else *slash = 0;
        struct stat st;
        if (!stat(dir, &st) && !(st.st_mode & 011)) return 0;
        if (slash == dir) break;
    }
    return 1;
}
// What the Mac in effect gives an object nothing checks (malloc'd, self-relative): the user has every right; for
// registry keys other accounts also what registry.txt's mode bits let them (read it, or rewrite it all).
static uint8_t *sd_unchecked(int type, DWORD all) {
    uint8_t user[SID_MAX], group[SID_MAX], world[12], ace[8 + SID_MAX];
    user_sid(user); sid_of(group, 5, 5, MACHINE_SUBS, NONE_RID); sid_of(world, 1, 1, 0);
    acl_buf b; ab_init(&b);
    ace_make(ace, ACCESS_ALLOWED, 0, all, user, NULL, NULL); ab_add(&b, ace);
    if (is_key_type(type)) {
        char path[1100]; struct stat st;
        snprintf(path, sizeof path, "%s/registry.txt", vx_root());
        DWORD others = 0;
        if (!stat(path, &st) && others_reach(path)) others = st.st_mode & 022 ? k_key_map[3] : st.st_mode & 044 ? k_key_map[0] : 0;
        if (others) { ace_make(ace, ACCESS_ALLOWED, 0, others, world, NULL, NULL); ab_add(&b, ace); }
    }
    uint8_t *acl = ab_finish(&b);
    sd_parts p = { SE_DACL_PRESENT, user, group, NULL, acl };
    uint8_t *sd = sd_rel(&p, NULL, 0);
    free(acl);
    return sd;
}
// A descriptor for an object nothing checks: `cur` (NULL: what the Mac gives) with the parts `info` names replaced
// (the DACL's generic rights mapped), into *out (malloc'd) when it takes from the user no right to the object and
// from other accounts none the Mac gives them; ERROR_NOT_SUPPORTED (logged) when it does.
static DWORD unchecked_set(int type, DWORD all, const uint8_t *cur, DWORD info, const uint8_t *owner, const uint8_t *group,
                           const uint8_t *dacl, const uint8_t *sacl, uint8_t **out, const char *fn) {
    DWORD map[4];
    if (is_key_type(type)) memcpy(map, k_key_map, sizeof map); else kernel_map(all, map);
    uint8_t *real = sd_unchecked(type, all), *mapped = info & DACL_SI ? acl_mapped(dacl, map) : NULL;
    uint8_t *want = sd_merge(cur ? cur : real, info, owner, group, mapped, sacl);
    token *u = token_default(), *o = token_other(NULL);
    DWORD from_user = taken_from(u, want, real, map, map[3] & USER_KEEPS), from_others = taken_from(o, want, real, map, map[3] & ~READ_CONTROL);
    token_free(u); token_free(o); free(real); free(mapped);
    *out = NULL;
    if (from_user || from_others) {
        vx_log("%s: the descriptor would take rights %#x from %s, and nothing here could hold anyone to that: refused", fn,
               from_user ? from_user : from_others, from_user ? "the user" : "other accounts, who have them");
        free(want);
        return ERROR_NOT_SUPPORTED;
    }
    *out = want;
    return 0;
}

// ---- files ----
// A file as the Mac protects it: its owner (the user, SYSTEM for root, S-1-22-1-<uid> for another Mac account: SIDs
// of our own), its group (None, the user's primary group, when the user is in it; S-1-22-2-<gid> otherwise) and its
// mode bits as ACEs: the owner's bits for the owner (with a deny ACE first for what the group or everyone else may do
// and the owner may not: Unix gives an owner only the owner's bits), the group's for the group, the rest's for
// Everyone. DELETE goes to whoever the folder lets remove the file. The allow entries of a macOS ACL on it are shown
// as granted to Everyone (to whom is not worked out here: shown wider, never narrower).
enum { F_RD = 0x1, F_WD = 0x2, F_AD = 0x4, F_REA = 0x8, F_WEA = 0x10, F_X = 0x20, F_DC = 0x40, F_RA = 0x80, F_WA = 0x100 };
#define F_SEEN (READ_CONTROL | SYNCHRONIZE | F_RA)        // what anyone given an ACE may do: see the file's attributes
typedef struct {
    struct stat st;
    int user_delete, owner_delete, others_delete;         // whom the folder lets remove it
    DWORD acl_allow;                                      // what a macOS ACL on it allows someone
} file_facts;
static DWORD macos_acl_allows(const char *path, int fd) {
    acl_t acl = path ? acl_get_file(path, ACL_TYPE_EXTENDED) : acl_get_fd_np(fd, ACL_TYPE_EXTENDED);
    if (!acl) return 0;
    static const struct { acl_perm_t perm; DWORD right; } k[] = {
        { ACL_READ_DATA, F_RD }, { ACL_WRITE_DATA, F_WD }, { ACL_APPEND_DATA, F_AD }, { ACL_READ_EXTATTRIBUTES, F_REA },
        { ACL_WRITE_EXTATTRIBUTES, F_WEA }, { ACL_EXECUTE, F_X }, { ACL_DELETE_CHILD, F_DC }, { ACL_READ_ATTRIBUTES, F_RA },
        { ACL_WRITE_ATTRIBUTES, F_WA }, { ACL_DELETE, DELETE_RIGHT }, { ACL_READ_SECURITY, READ_CONTROL },
        { ACL_WRITE_SECURITY, WRITE_DAC }, { ACL_CHANGE_OWNER, WRITE_OWNER },
    };
    DWORD r = 0;
    acl_entry_t e;
    for (int id = ACL_FIRST_ENTRY; !acl_get_entry(acl, id, &e); id = ACL_NEXT_ENTRY) {
        acl_tag_t tag; acl_permset_t ps;
        if (acl_get_tag_type(e, &tag) || tag != ACL_EXTENDED_ALLOW || acl_get_permset(e, &ps)) continue;
        for (size_t i = 0; i < COUNT(k); i++) if (acl_get_perm_np(ps, k[i].perm) == 1) r |= k[i].right;
    }
    acl_free(acl);
    return r;
}
// path: the file's Mac path; NULL when only its descriptor fd is known (a pipe, a terminal).
static DWORD file_facts_of(const char *path, int fd, file_facts *f) {
    memset(f, 0, sizeof *f);
    if (path ? stat(path, &f->st) : fstat(fd, &f->st)) return vx_error_from_errno(errno);
    f->acl_allow = macos_acl_allows(path, fd);
    char parent[1100];
    struct stat ps;
    if (!path || strlen(path) >= sizeof parent) return 0;
    snprintf(parent, sizeof parent, "%s", path);
    char *slash = strrchr(parent, '/');
    if (!slash) return 0;
    if (slash == parent) slash[1] = 0; else *slash = 0;
    if (stat(parent, &ps)) return 0;
    uid_t me = getuid();
    int sticky = (ps.st_mode & S_ISVTX) != 0, open_dir = !sticky && ((ps.st_mode & 030) == 030 || (ps.st_mode & 03) == 03);
    f->others_delete = open_dir || (ps.st_uid != me && ps.st_uid != f->st.st_uid && ps.st_uid != 0);   // (or the folder's owner)
    f->owner_delete = ps.st_uid == f->st.st_uid || open_dir;
    f->user_delete = !access(parent, W_OK | X_OK) && (!sticky || f->st.st_uid == me || ps.st_uid == me);
    return 0;
}
static DWORD unix_rights(int bits, int dir) {             // rwx (4 2 1) as file rights
    return (bits & 4 ? F_RD | F_REA : 0) | (bits & 2 ? F_WD | F_AD | F_WEA | F_WA | (dir ? F_DC : 0) : 0) | (bits & 1 ? F_X : 0);
}
static int mode_bits(DWORD rights) { return (rights & F_RD ? 4 : 0) | (rights & F_WD ? 2 : 0) | (rights & F_X ? 1 : 0); }
static void mac_owner_sid(uid_t uid, uint8_t *sid) {
    if (uid == getuid()) user_sid(sid);
    else if (!uid) sid_of(sid, 5, 1, 18);
    else sid_of(sid, 22, 2, 1, (DWORD)uid);
}
static int user_in_group(gid_t gid) {
    if (gid == getgid() || gid == getegid()) return 1;
    gid_t list[1024];
    int n = getgroups(1024, list);
    for (int i = 0; i < n; i++) if (list[i] == gid) return 1;
    return 0;
}
static void mac_group_sid(gid_t gid, uint8_t *sid) {
    if (user_in_group(gid)) sid_of(sid, 5, 5, MACHINE_SUBS, NONE_RID); else sid_of(sid, 22, 2, 2, (DWORD)gid);
}
static uint8_t *sd_of_file(const file_facts *f, mode_t mode) {   // malloc'd, self-relative
    uint8_t owner[SID_MAX], group[SID_MAX], user[SID_MAX], world[12], ace[8 + SID_MAX];
    int dir = S_ISDIR(f->st.st_mode), mine = f->st.st_uid == getuid();
    DWORD o = unix_rights(mode >> 6 & 7, dir), g = unix_rights(mode >> 3 & 7, dir), e = unix_rights(mode & 7, dir);
    DWORD others_delete = f->others_delete ? DELETE_RIGHT : 0;
    mac_owner_sid(f->st.st_uid, owner); mac_group_sid(f->st.st_gid, group); user_sid(user); sid_of(world, 1, 1, 0);
    acl_buf b; ab_init(&b);
    if (mine && ((g | e) & ~(o | F_WA))) { ace_make(ace, ACCESS_DENIED, 0, (g | e) & ~(o | F_WA), user, NULL, NULL); ab_add(&b, ace); }
    DWORD own = o | F_SEEN | F_WA | WRITE_DAC | ((mine ? f->user_delete : f->owner_delete) ? DELETE_RIGHT : 0);   // (an owner may chmod)
    ace_make(ace, ACCESS_ALLOWED, 0, own, owner, NULL, NULL); ab_add(&b, ace);
    if (!mine && f->user_delete) { ace_make(ace, ACCESS_ALLOWED, 0, DELETE_RIGHT, user, NULL, NULL); ab_add(&b, ace); }
    if (g || others_delete) { ace_make(ace, ACCESS_ALLOWED, 0, g | F_SEEN | others_delete, group, NULL, NULL); ab_add(&b, ace); }
    if (e || others_delete || f->acl_allow) { ace_make(ace, ACCESS_ALLOWED, 0, e | F_SEEN | others_delete | f->acl_allow, world, NULL, NULL); ab_add(&b, ace); }
    uint8_t *acl = ab_finish(&b);
    sd_parts p = { SE_DACL_PRESENT, owner, group, NULL, acl };
    uint8_t *sd = sd_rel(&p, NULL, 0);
    free(acl);
    return sd;
}

// A file or folder named by a Windows path, or behind a handle (its path asked of macOS, then taken as a name): one
// way to it for both, so a name and a handle see the same file. has_path 0: only fd (a pipe, a terminal).
typedef struct { char win[1100], path[1100]; int fd, has_path; } file_ref;
static DWORD file_by_name(const WCHAR *wname, file_ref *r) {
    memset(r, 0, sizeof *r);
    r->fd = -1;
    DWORD (WINAPI *attrs)(const WCHAR *) = (DWORD (WINAPI *)(const WCHAR *))vx_find_export("kernel32.dll", "GetFileAttributesW");
    if (!attrs) return ERROR_NOT_SUPPORTED;
    if (attrs(wname) == 0xffffffffu) { DWORD e = vx_get_last_error(); return e == ERROR_PATH_NOT_FOUND ? e : ERROR_FILE_NOT_FOUND; }
    vx_utf16_to_utf8(wname, vx_wcslen(wname) + 1, r->win, sizeof r->win);
    if (!vx_unix_path(r->win, r->path, sizeof r->path)) return ERROR_PATH_NOT_FOUND;
    r->has_path = 1;
    return 0;
}
static DWORD file_by_handle(HANDLE h, file_ref *r) {
    memset(r, 0, sizeof *r);
    if ((r->fd = vx_handle_fd(h)) < 0) return ERROR_INVALID_HANDLE;
    char mac[1024];                                       // (MAXPATHLEN: what F_GETPATH writes at most)
    if (fcntl(r->fd, F_GETPATH, mac) != -1) {
        vx_windows_path(mac, r->win, sizeof r->win);
        r->has_path = vx_unix_path(r->win, r->path, sizeof r->path);   // (the overlay's copy, if written: as by name)
    }
    return 0;
}
static DWORD file_get(const file_ref *r, DWORD info, uint8_t **out, DWORD *size) {
    if (info & SACL_SI) return ERROR_PRIVILEGE_NOT_HELD;
    file_facts f;
    DWORD err = file_facts_of(r->has_path ? r->path : NULL, r->fd, &f);
    if (err) return err;
    uint8_t *sd = sd_of_file(&f, f.st.st_mode);
    *out = sd_select(sd, info, size);                     // (no label: the Mac has none, and nothing below medium runs)
    free(sd);
    return *out ? 0 : ERROR_NOT_ENOUGH_MEMORY;
}
// Setting: the owner only as it is (chown is root's); the primary group and a label (no higher than the user's: what
// it keeps out runs nowhere here) are taken and not applied, as neither changes who may do what on the Mac; the DACL
// as the Mac's mode bits can carry it (see above).
static DWORD file_set(const file_ref *r, DWORD info, const uint8_t *owner, const uint8_t *dacl) {
    file_facts f;
    DWORD err = file_facts_of(r->has_path ? r->path : NULL, r->fd, &f);
    if (err) return err;
    uint8_t now_owner[SID_MAX], gsid[SID_MAX];
    mac_owner_sid(f.st.st_uid, now_owner); mac_group_sid(f.st.st_gid, gsid);
    if ((info & OWNER_SI) && !sid_eq(owner, now_owner)) { vx_log("security of %s: another owner: refused (only root changes owners on the Mac)", r->win); return ERROR_ACCESS_DENIED; }
    if (!(info & DACL_SI)) return 0;
    uint8_t *real = sd_of_file(&f, f.st.st_mode), *mapped = acl_mapped(dacl, k_file_map);
    uint8_t *want = sd_merge(real, DACL_SI, NULL, NULL, mapped, NULL);
    token *u = token_default(), *og = token_other(gsid), *oo = token_other(NULL);
    sd_parts w;
    sd_read(want, &w);
    int qu = mode_bits(access_of(u, &w, MAXIMUM_ALLOWED, k_file_map)), qg = mode_bits(access_of(og, &w, MAXIMUM_ALLOWED, k_file_map));
    int qo = mode_bits(access_of(oo, &w, MAXIMUM_ALLOWED, k_file_map));
    // each class of the mode the bits all its members may have (the user is the owner, or in the group or the rest)
    int mine = f.st.st_uid == getuid(), in_group = user_in_group(f.st.st_gid);
    int bits = mine ? qu << 6 | qg << 3 | qo : 7 << 6 | (in_group ? qu & qg : qg) << 3 | (in_group ? qo : qu & qo);
    mode_t now = f.st.st_mode & 07777, target = (mode_t)(now & (07000 | bits));   // (only taken away, never added)
    uint8_t *after = sd_of_file(&f, target);
    const DWORD others = F_RD | F_WD | F_AD | F_REA | F_WEA | F_X | F_DC | F_WA | DELETE_RIGHT | WRITE_DAC | WRITE_OWNER;
    const char *who = "the user";
    DWORD lost = taken_from(u, want, after, k_file_map, F_RD | F_WD | F_AD | F_X);
    if (!lost && (lost = taken_from(og, want, after, k_file_map, others))) who = "the file's group";
    if (!lost && (lost = taken_from(oo, want, after, k_file_map, others))) who = "everyone else";
    token_free(u); token_free(og); token_free(oo);
    free(real); free(mapped); free(want); free(after);
    if (lost) {
        vx_log("security of %s: the DACL takes rights %#x from %s, which the Mac's permissions would still give: refused", r->win, lost, who);
        return ERROR_NOT_SUPPORTED;
    }
    if (target == now) return 0;
    int rc;
    if (r->has_path) {                                    // under an overlaid tree, the overlay's copy (never the original)
        char wpath[1100];
        if (!vx_unix_path_write(r->win, wpath, sizeof wpath, 1)) return ERROR_PATH_NOT_FOUND;
        if (strcmp(wpath, r->path) && S_ISDIR(f.st.st_mode)) { vx_log("security of %s: a folder of an overlaid tree (its original is not changed): refused", r->win); return ERROR_NOT_SUPPORTED; }
        rc = chmod(wpath, target);
    } else rc = fchmod(r->fd, target);
    if (rc) return vx_error_from_errno(errno);
    if (vx_trace) vx_log("security of %s: mode %o -> %o", r->win, (unsigned)now, (unsigned)target);
    return 0;
}

// ---- handles and names ----
// The token a handle means, if it is one (pseudo-handles included); no error when not.
static token *token_if_any(HANDLE h) { DWORD err; return token_get(h, &err); }

// What the caller may set: an owner it is (or a group of its marked SE_GROUP_OWNER), labels no higher than its own
// integrity, no SACL (that needs SeSecurityPrivilege, which a standard user lacks).
static DWORD check_settable(DWORD info, const uint8_t *owner, const uint8_t *group, const uint8_t *dacl, const uint8_t *sacl) {
    if ((info & OWNER_SI) && !sid_ok(owner)) return ERROR_INVALID_SID;
    if ((info & GROUP_SI) && !sid_ok(group)) return ERROR_INVALID_SID;
    if ((info & DACL_SI) && dacl && !acl_ok(dacl)) return ERROR_INVALID_ACL;
    if ((info & (SACL_SI | LABEL_SI)) && sacl && !acl_ok(sacl)) return ERROR_INVALID_ACL;
    if (info & SACL_SI) return ERROR_PRIVILEGE_NOT_HELD;
    pthread_mutex_lock(&g_lock);
    token *t = thread_token_of(vx_current_tid());
    if (!t) t = process_token();
    DWORD err = 0;
    if (info & OWNER_SI) {
        int ok = sid_eq(owner, t->user.sid) && !(t->user.attr & SE_GROUP_USE_FOR_DENY_ONLY);
        for (int i = 0; i < t->ngroups && !ok; i++) ok = (t->groups[i].attr & SE_GROUP_OWNER) && sid_eq(t->groups[i].sid, owner);
        if (!ok) err = ERROR_INVALID_OWNER;
    }
    if (!err && (info & LABEL_SI) && sacl) {
        const uint8_t *ace;
        FOR_ACE(sacl, ace) if (ace[0] == SYSTEM_MANDATORY_LABEL) {
            const uint8_t *sid = ace_sid(ace, NULL);
            if (sid_auth(sid) != 16 || sid[1] != 1) err = ERROR_INVALID_LABEL;
            else if (sid_sub(sid, 0) > token_integrity(t)) err = ERROR_INVALID_LABEL;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return err;
}
static int is_kernel_type(int type) { return type == SE_KERNEL_OBJECT || type == SE_UNKNOWN_OBJECT_TYPE; }
static int is_file_handle(HANDLE h, int type) { return type == SE_FILE_OBJECT || (is_kernel_type(type) && vx_handle_fd(h) >= 0); }
static uint8_t **kept_sd(HANDLE h, int type, token *t) {   // (g_lock held) where a handle's descriptor is kept, if anywhere
    if (t) return &t->sd;
    return is_kernel_type(type) && (intptr_t)h == -1 ? &g_process_sd : NULL;
}
static DWORD handle_all_access(HANDLE h, int type, token *t) {
    return t ? TOKEN_ALL_ACCESS : is_key_type(type) ? k_key_map[3] : kernel_all_access(h);
}
static DWORD object_get(HANDLE h, int type, DWORD info, uint8_t **out, DWORD *size) {
    if (!h) return ERROR_INVALID_HANDLE;
    if (is_file_handle(h, type)) { file_ref r; DWORD err = file_by_handle(h, &r); return err ? err : file_get(&r, info, out, size); }
    if (info & SACL_SI) return ERROR_PRIVILEGE_NOT_HELD;
    token *t = is_kernel_type(type) ? token_if_any(h) : NULL;
    if (!t && (intptr_t)h == -5) return ERROR_NO_TOKEN;
    uint8_t *real = sd_unchecked(type, handle_all_access(h, type, t));
    pthread_mutex_lock(&g_lock);
    uint8_t **kept = kept_sd(h, type, t);
    *out = sd_select(kept && *kept ? *kept : real, info, size);
    pthread_mutex_unlock(&g_lock);
    free(real);
    token_unref(t);
    return *out ? 0 : ERROR_NOT_ENOUGH_MEMORY;
}
static DWORD object_set(HANDLE h, int type, DWORD info, const uint8_t *owner, const uint8_t *group, const uint8_t *dacl, const uint8_t *sacl) {
    if (!h) return ERROR_INVALID_HANDLE;
    int file = is_file_handle(h, type);
    file_ref r;
    DWORD err = file ? file_by_handle(h, &r) : 0;
    if (err || (err = check_settable(info, owner, group, dacl, sacl))) return err;
    if (file) return file_set(&r, info, owner, dacl);
    token *t = is_kernel_type(type) ? token_if_any(h) : NULL;
    if (!t && (intptr_t)h == -5) return ERROR_NO_TOKEN;
    pthread_mutex_lock(&g_lock);
    uint8_t **kept = kept_sd(h, type, t), *n;
    err = unchecked_set(type, handle_all_access(h, type, t), kept ? *kept : NULL, info, owner, group, dacl, sacl, &n, "SetSecurityInfo");
    if (!err && kept) { free(*kept); *kept = n; }
    else free(n);                                         // (a handle with nowhere to keep it: it takes nothing from anyone)
    pthread_mutex_unlock(&g_lock);
    token_unref(t);
    return err;
}

// Named objects other than files: the name checked as Windows would open it (a registry key that opens, a kernel
// object one of the Open* functions finds; *all: that type's full access), the descriptor kept by type and name.
static DWORD named_key(const WCHAR *wname, int type, char *key, size_t cap, DWORD *all) {
    if (!wname) return ERROR_INVALID_PARAMETER;
    char name[1024];
    vx_utf16_to_utf8(wname, vx_wcslen(wname) + 1, name, sizeof name);
    switch (type) {
    case SE_REGISTRY_KEY: case SE_REGISTRY_WOW64_32KEY: case SE_REGISTRY_WOW64_64KEY: {
        static const char *roots[] = { "CLASSES_ROOT", "CURRENT_USER", "MACHINE", "USERS", "", "CURRENT_CONFIG" };
        int root = -1;
        size_t rl = 0;
        for (int i = 0; i < 6; i++) {
            size_t n = strlen(roots[i]);
            if (n && !strncasecmp(name, roots[i], n) && (name[n] == '\\' || !name[n])) { root = i; rl = n; }
        }
        if (root < 0) return ERROR_INVALID_PARAMETER;
        LONG (WINAPI *open)(HANDLE, const WCHAR *, DWORD, DWORD, HANDLE *) = (LONG (WINAPI *)(HANDLE, const WCHAR *, DWORD, DWORD, HANDLE *))vx_find_export("advapi32.dll", "RegOpenKeyExW");
        LONG (WINAPI *close)(HANDLE) = (LONG (WINAPI *)(HANDLE))vx_find_export("advapi32.dll", "RegCloseKey");
        if (!open || !close) return ERROR_NOT_SUPPORTED;
        HANDLE k = NULL;
        LONG r = open((HANDLE)(intptr_t)(int32_t)(0x80000000u + (DWORD)root), wname + rl + (name[rl] ? 1 : 0), 0, 0x20019 /* KEY_READ */, &k);
        if (r) return (DWORD)r;
        close(k);
        snprintf(key, cap, "%s", name);
        *all = k_key_map[3];
        break;
    }
    case SE_KERNEL_OBJECT: {
        static const char *opens[] = { "OpenEventW", "OpenMutexW", "OpenSemaphoreW", "OpenFileMappingW" };
        static const DWORD alls[] = { 0x1f0003, 0x1f0001, 0x1f0003, 0xf001f };   // EVENT_, MUTANT_, SEMAPHORE_, SECTION_ALL_ACCESS
        int found = -1;
        for (int i = 0; i < 4 && found < 0; i++) {
            HANDLE (WINAPI *open)(DWORD, BOOL, const WCHAR *) = (HANDLE (WINAPI *)(DWORD, BOOL, const WCHAR *))vx_find_export("kernel32.dll", opens[i]);
            HANDLE o = open ? open(i == 3 ? 4 /* FILE_MAP_READ */ : SYNCHRONIZE, 0, wname) : NULL;
            if (o) { vx_handle_close(o); found = i; }
        }
        if (found < 0) return ERROR_FILE_NOT_FOUND;
        snprintf(key, cap, "%s", name);
        *all = alls[found];
        break;
    }
    case SE_SERVICE: return ERROR_SERVICE_DOES_NOT_EXIST;   // (no service manager)
    case SE_PRINTER: return ERROR_INVALID_PRINTER_NAME;
    case SE_LMSHARE: return ERROR_BAD_NETPATH;
    default: return ERROR_INVALID_PARAMETER;
    }
    return 0;
}
static named_sd *named_find(int type, const char *key) {   // (g_lock held)
    int base = is_key_type(type) ? SE_REGISTRY_KEY : type;
    for (int i = 0; i < g_nnamed; i++) if (g_named[i].type == base && !strcasecmp(g_named[i].name, key)) return &g_named[i];
    return NULL;
}
static DWORD named_get(const WCHAR *wname, int type, DWORD info, uint8_t **out, DWORD *size) {
    if (type == SE_FILE_OBJECT) {
        file_ref r;
        DWORD err = wname ? file_by_name(wname, &r) : ERROR_INVALID_PARAMETER;
        return err ? err : file_get(&r, info, out, size);
    }
    char key[1100];
    DWORD all, err = named_key(wname, type, key, sizeof key, &all);
    if (err) return err;
    if (info & SACL_SI) return ERROR_PRIVILEGE_NOT_HELD;
    uint8_t *real = sd_unchecked(type, all);
    pthread_mutex_lock(&g_lock);
    named_sd *n = named_find(type, key);
    *out = sd_select(n ? n->sd : real, info, size);
    pthread_mutex_unlock(&g_lock);
    free(real);
    return *out ? 0 : ERROR_NOT_ENOUGH_MEMORY;
}
static DWORD named_set(const WCHAR *wname, int type, DWORD info, const uint8_t *owner, const uint8_t *group, const uint8_t *dacl, const uint8_t *sacl) {
    if (type == SE_FILE_OBJECT) {
        file_ref r;
        DWORD err = wname ? file_by_name(wname, &r) : ERROR_INVALID_PARAMETER;
        if (err || (err = check_settable(info, owner, group, dacl, sacl))) return err;
        return file_set(&r, info, owner, dacl);
    }
    char key[1100];
    DWORD all, err = named_key(wname, type, key, sizeof key, &all);
    if (err || (err = check_settable(info, owner, group, dacl, sacl))) return err;
    pthread_mutex_lock(&g_lock);
    named_sd *n = named_find(type, key);
    uint8_t *m;
    err = unchecked_set(type, all, n ? n->sd : NULL, info, owner, group, dacl, sacl, &m, "SetNamedSecurityInfo");
    if (!err) {
        if (!n) {
            g_named = realloc(g_named, (size_t)(g_nnamed + 1) * sizeof *g_named);
            n = &g_named[g_nnamed++];
            n->type = is_key_type(type) ? SE_REGISTRY_KEY : type;
            n->name = strdup(key); n->sd = NULL;
        }
        free(n->sd); n->sd = m;
    }
    pthread_mutex_unlock(&g_lock);
    return err;
}
// GetSecurityInfo's outputs: pointers into the descriptor it returns (a caller that wants no descriptor gets the parts
// anyway, in memory it cannot free).
static void parts_out(uint8_t *sd, void **owner, void **group, void **dacl, void **sacl, void **out) {
    sd_parts p;
    sd_read(sd, &p);
    if (owner) *owner = p.owner ? PROG(p.owner) : NULL;
    if (group) *group = p.group ? PROG(p.group) : NULL;
    if (dacl) *dacl = p.dacl ? PROG(p.dacl) : NULL;
    if (sacl) *sacl = p.sacl ? PROG(p.sacl) : NULL;
    if (out) *out = PROG(sd);
}

// ==== the exported functions ================================================================================================
// ---- SIDs ----
static WINAPI BOOL AllocateAndInitializeSid(const uint8_t *auth, uint8_t n, DWORD a0, DWORD a1, DWORD a2, DWORD a3, DWORD a4, DWORD a5, DWORD a6, DWORD a7, void **sid) {
    if (n > 8 || !auth || !sid) { vx_set_last_error(n > 8 ? ERROR_INVALID_SID : ERROR_INVALID_PARAMETER); return 0; }
    DWORD sub[8] = { a0, a1, a2, a3, a4, a5, a6, a7 };
    uint8_t *s = vx_heap_alloc(8 + 4 * (size_t)n);
    if (!s) { vx_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    s[0] = 1; s[1] = n; memcpy(s + 2, auth, 6);
    for (int i = 0; i < n; i++) wr32(s + 8 + 4 * i, sub[i]);
    *sid = PROG(s);
    return 1;
}
static WINAPI void *FreeSid(void *sid) { free(sid); return NULL; }
static WINAPI DWORD GetLengthSid(const uint8_t *sid) { return sid_len(sid); }
static WINAPI BOOL IsValidSid(const uint8_t *sid) { return sid_ok(sid); }
static WINAPI DWORD GetSidLengthRequired(uint8_t n) { return 8 + 4 * (DWORD)n; }
static WINAPI BOOL InitializeSid(uint8_t *sid, const uint8_t *auth, uint8_t n) {
    if (n > 15) { vx_set_last_error(ERROR_INVALID_SID); return 0; }
    sid[0] = 1; sid[1] = n; memcpy(sid + 2, auth, 6);
    return 1;
}
static WINAPI uint8_t *GetSidIdentifierAuthority(uint8_t *sid) { return sid + 2; }
static WINAPI DWORD *GetSidSubAuthority(uint8_t *sid, DWORD i) { return (DWORD *)(sid + 8 + 4 * i); }
static WINAPI uint8_t *GetSidSubAuthorityCount(uint8_t *sid) { return sid + 1; }
static WINAPI BOOL CopySid(DWORD n, uint8_t *dst, const uint8_t *src) {
    if (!sid_ok(src)) { vx_set_last_error(ERROR_INVALID_SID); return 0; }
    if (n < sid_len(src)) { vx_set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    memcpy(dst, src, sid_len(src));
    return 1;
}
static WINAPI BOOL EqualSid(const uint8_t *a, const uint8_t *b) {
    if (!sid_ok(a) || !sid_ok(b)) { vx_set_last_error(ERROR_INVALID_SID); return 0; }
    vx_set_last_error(0);
    return sid_eq(a, b);
}
static WINAPI BOOL EqualPrefixSid(const uint8_t *a, const uint8_t *b) {   // equal but for the last sub-authority
    if (!sid_ok(a) || !sid_ok(b)) { vx_set_last_error(ERROR_INVALID_SID); return 0; }
    vx_set_last_error(0);
    if (a[1] != b[1]) return 0;
    if (!a[1]) return !memcmp(a, b, 8);
    return !memcmp(a, b, sid_len(a) - 4);
}
static WINAPI BOOL CreateWellKnownSid(int type, const uint8_t *domain, uint8_t *sid, DWORD *size) {
    uint8_t s[SID_MAX];
    DWORD n = 0;
    const wk_sid *w = wk_by_type(type);
    if (w) n = wk_make(w, s);
    else {
        for (size_t i = 0; i < COUNT(k_domain_rids) && !n; i++) {
            if (k_domain_rids[i].type != type) continue;
            if (!sid_ok(domain) || domain[1] < 1 || domain[1] > 14) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
            memcpy(s, domain, sid_len(domain));
            s[1] = domain[1] + 1;
            wr32(s + 8 + 4 * domain[1], k_domain_rids[i].rid);
            n = sid_len(s);
        }
    }
    if (!n) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    if (!size) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    if (!sid || *size < n) { *size = n; vx_set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    memcpy(sid, s, n);
    *size = n;
    return 1;
}
static WINAPI BOOL IsWellKnownSid(const uint8_t *sid, int type) {
    uint8_t s[SID_MAX];
    const wk_sid *w = wk_by_type(type);
    if (w) return wk_make(w, s) && sid_eq(s, sid);
    for (size_t i = 0; i < COUNT(k_domain_rids); i++)   // a domain's: the type's RID last, under any domain
        if (k_domain_rids[i].type == type) return sid_ok(sid) && sid[1] >= 2 && sid_auth(sid) == 5 && sid_sub(sid, 0) == 21 && sid_sub(sid, sid[1] - 1) == k_domain_rids[i].rid;
    return 0;
}
static BOOL sid_to_string(const uint8_t *sid, void **out, int wide) {
    if (!out) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    if (!sid_ok(sid)) { vx_set_last_error(ERROR_INVALID_SID); return 0; }
    char text[200];
    sid_format(sid, text, sizeof text);
    void *r = wide ? (void *)program_wide(text) : (void *)program_ansi(text);
    if (!r) { vx_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    *out = PROG(r);
    return 1;
}
static WINAPI BOOL ConvertSidToStringSidW(const uint8_t *sid, WCHAR **out) { return sid_to_string(sid, (void **)out, 1); }
static WINAPI BOOL ConvertSidToStringSidA(const uint8_t *sid, char **out) { return sid_to_string(sid, (void **)out, 0); }
static BOOL string_to_sid(const char *text, void **out) {
    uint8_t s[SID_MAX]; const char *end;
    DWORD n = sid_parse(text, &end, s);
    if (!n || *end) { vx_set_last_error(ERROR_INVALID_SID); return 0; }
    uint8_t *r = vx_heap_alloc(n);
    if (!r) { vx_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    memcpy(r, s, n);
    *out = PROG(r);
    return 1;
}
static WINAPI BOOL ConvertStringSidToSidW(const WCHAR *text, void **out) {
    if (!text || !out) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    char a[512];
    vx_utf16_to_utf8(text, vx_wcslen(text) + 1, a, sizeof a);
    return string_to_sid(a, out);
}
static WINAPI BOOL ConvertStringSidToSidA(const char *text, void **out) {
    if (!text || !out) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    return string_to_sid(text, out);
}
// LookupAccountSid / LookupAccountName: the lengths in characters (A: bytes) without the terminator when they fit,
// what they need with it when not.
static BOOL lookup_sid(const uint8_t *sid, void *name, DWORD *nn, void *dom, DWORD *dn, DWORD *use, int wide) {
    char n[256], d[256];
    DWORD u;
    if (!sid_ok(sid)) { vx_set_last_error(ERROR_INVALID_SID); return 0; }
    if (!nn || !dn) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    if (!account_of_sid(sid, n, sizeof n, d, sizeof d, &u)) { vx_set_last_error(ERROR_NONE_MAPPED); return 0; }
    DWORD need_n = wide ? (DWORD)vx_utf8_to_utf16(n, strlen(n) + 1, NULL, 0) : (DWORD)strlen(n) + 1;
    DWORD need_d = wide ? (DWORD)vx_utf8_to_utf16(d, strlen(d) + 1, NULL, 0) : (DWORD)strlen(d) + 1;
    if (!name || !dom || *nn < need_n || *dn < need_d) { *nn = need_n; *dn = need_d; vx_set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    if (wide) { vx_utf8_to_utf16(n, strlen(n) + 1, name, need_n); vx_utf8_to_utf16(d, strlen(d) + 1, dom, need_d); }
    else { memcpy(name, n, need_n); memcpy(dom, d, need_d); }
    *nn = need_n - 1; *dn = need_d - 1;
    if (use) *use = u;
    return 1;
}
static WINAPI BOOL LookupAccountSidW(const WCHAR *sys, const uint8_t *sid, WCHAR *name, DWORD *nn, WCHAR *dom, DWORD *dn, DWORD *use) { (void)sys; return lookup_sid(sid, name, nn, dom, dn, use, 1); }
static WINAPI BOOL LookupAccountSidA(const char *sys, const uint8_t *sid, char *name, DWORD *nn, char *dom, DWORD *dn, DWORD *use) { (void)sys; return lookup_sid(sid, name, nn, dom, dn, use, 0); }
static BOOL lookup_name(const char *account, uint8_t *sid, DWORD *sn, void *dom, DWORD *dn, DWORD *use, int wide) {
    uint8_t s[SID_MAX]; char d[256]; DWORD u = 0;
    if (!sn || !dn) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    DWORD n = sid_of_name(account, s, d, sizeof d, &u);
    if (!n) { vx_set_last_error(ERROR_NONE_MAPPED); return 0; }
    DWORD need_d = wide ? (DWORD)vx_utf8_to_utf16(d, strlen(d) + 1, NULL, 0) : (DWORD)strlen(d) + 1;
    if (!sid || !dom || *sn < n || *dn < need_d) { *sn = n; *dn = need_d; vx_set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    memcpy(sid, s, n);
    if (wide) vx_utf8_to_utf16(d, strlen(d) + 1, dom, need_d); else memcpy(dom, d, need_d);
    *sn = n; *dn = need_d - 1;
    if (use) *use = u;
    return 1;
}
static WINAPI BOOL LookupAccountNameW(const WCHAR *sys, const WCHAR *account, uint8_t *sid, DWORD *sn, WCHAR *dom, DWORD *dn, DWORD *use) {
    (void)sys;
    if (!account) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    char a[512];
    vx_utf16_to_utf8(account, vx_wcslen(account) + 1, a, sizeof a);
    return lookup_name(a, sid, sn, dom, dn, use, 1);
}
static WINAPI BOOL LookupAccountNameA(const char *sys, const char *account, uint8_t *sid, DWORD *sn, char *dom, DWORD *dn, DWORD *use) {
    (void)sys;
    if (!account) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    return lookup_name(account, sid, sn, dom, dn, use, 0);
}

// ---- ACLs and ACEs ----
static WINAPI BOOL InitializeAcl(uint8_t *acl, DWORD n, DWORD rev) {
    if (n < 8) { vx_set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    if (n > 0xffff) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    if (rev < 2 || rev > 4) { vx_set_last_error(ERROR_REVISION_MISMATCH); return 0; }
    acl[0] = (uint8_t)rev; acl[1] = 0; wr16(acl + 2, (uint16_t)(n & ~3u)); wr16(acl + 4, 0); wr16(acl + 6, 0);
    return 1;
}
static WINAPI BOOL IsValidAcl(const uint8_t *acl) { return acl_ok(acl); }
static WINAPI BOOL GetAce(const uint8_t *acl, DWORD index, void **ace) {
    if (!acl_ok(acl) || index >= rd16(acl + 4)) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    DWORD off = 8;
    for (DWORD i = 0; i < index; i++) off += rd16(acl + off + 2);
    *ace = PROG(acl + off);
    return 1;
}
// The ACE (type, flags, mask, SID, object GUIDs) appended to an ACL, as the Add*Ace functions do.
static BOOL add_ace(uint8_t *acl, DWORD rev, DWORD flags, DWORD valid_flags, uint8_t type, DWORD mask, const uint8_t *sid, const uint8_t *obj, const uint8_t *inh) {
    if (!acl_ok(acl)) { vx_set_last_error(ERROR_INVALID_ACL); return 0; }
    if (rev < 2 || rev > 4 || (ace_is_object(type) && rev < 4)) { vx_set_last_error(ERROR_REVISION_MISMATCH); return 0; }
    if (flags & ~valid_flags) { vx_set_last_error(ERROR_INVALID_FLAGS); return 0; }
    if (!sid_ok(sid)) { vx_set_last_error(ERROR_INVALID_SID); return 0; }
    uint8_t ace[12 + 32 + SID_MAX];
    DWORD n = ace_make(ace, type, (uint8_t)flags, mask, sid, obj, inh), end = acl_end(acl);
    if (end + n > rd16(acl + 2)) { vx_set_last_error(ERROR_ALLOTTED_SPACE_EXCEEDED); return 0; }
    memcpy(acl + end, ace, n);
    wr16(acl + 4, (uint16_t)(rd16(acl + 4) + 1));
    if (acl[0] < rev) acl[0] = (uint8_t)rev;
    return 1;
}
static WINAPI BOOL AddAccessAllowedAce(uint8_t *acl, DWORD rev, DWORD mask, const uint8_t *sid) { return add_ace(acl, rev, 0, 0, ACCESS_ALLOWED, mask, sid, NULL, NULL); }
static WINAPI BOOL AddAccessAllowedAceEx(uint8_t *acl, DWORD rev, DWORD flags, DWORD mask, const uint8_t *sid) { return add_ace(acl, rev, flags, VALID_INHERIT_FLAGS, ACCESS_ALLOWED, mask, sid, NULL, NULL); }
static WINAPI BOOL AddAccessDeniedAce(uint8_t *acl, DWORD rev, DWORD mask, const uint8_t *sid) { return add_ace(acl, rev, 0, 0, ACCESS_DENIED, mask, sid, NULL, NULL); }
static WINAPI BOOL AddAccessDeniedAceEx(uint8_t *acl, DWORD rev, DWORD flags, DWORD mask, const uint8_t *sid) { return add_ace(acl, rev, flags, VALID_INHERIT_FLAGS, ACCESS_DENIED, mask, sid, NULL, NULL); }
static WINAPI BOOL AddAuditAccessAceEx(uint8_t *acl, DWORD rev, DWORD flags, DWORD mask, const uint8_t *sid, BOOL success, BOOL failure) {
    if (flags & ~VALID_INHERIT_FLAGS) { vx_set_last_error(ERROR_INVALID_FLAGS); return 0; }
    flags |= (success ? SUCCESSFUL_ACCESS : 0) | (failure ? FAILED_ACCESS : 0);
    return add_ace(acl, rev, flags, VALID_INHERIT_FLAGS | SUCCESSFUL_ACCESS | FAILED_ACCESS, SYSTEM_AUDIT, mask, sid, NULL, NULL);
}
static WINAPI BOOL AddAuditAccessAce(uint8_t *acl, DWORD rev, DWORD mask, const uint8_t *sid, BOOL success, BOOL failure) { return AddAuditAccessAceEx(acl, rev, 0, mask, sid, success, failure); }
static WINAPI BOOL AddMandatoryAce(uint8_t *acl, DWORD rev, DWORD flags, DWORD policy, const uint8_t *label) {
    if (policy & ~(DWORD)(NO_WRITE_UP | NO_READ_UP | NO_EXECUTE_UP)) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    if (sid_ok(label) && (sid_auth(label) != 16 || label[1] != 1)) { vx_set_last_error(ERROR_INVALID_LABEL); return 0; }
    return add_ace(acl, rev, flags, VALID_INHERIT_FLAGS, SYSTEM_MANDATORY_LABEL, policy, label, NULL, NULL);
}
static BOOL add_object_ace(uint8_t *acl, DWORD rev, DWORD flags, uint8_t type, DWORD mask, const uint8_t *obj, const uint8_t *inh, const uint8_t *sid) {
    return add_ace(acl, rev, flags, VALID_INHERIT_FLAGS, type, mask, sid, obj, inh);
}
static WINAPI BOOL AddAccessAllowedObjectAce(uint8_t *acl, DWORD rev, DWORD flags, DWORD mask, const uint8_t *obj, const uint8_t *inh, const uint8_t *sid) { return add_object_ace(acl, rev, flags, ACCESS_ALLOWED_OBJECT, mask, obj, inh, sid); }
static WINAPI BOOL AddAccessDeniedObjectAce(uint8_t *acl, DWORD rev, DWORD flags, DWORD mask, const uint8_t *obj, const uint8_t *inh, const uint8_t *sid) { return add_object_ace(acl, rev, flags, ACCESS_DENIED_OBJECT, mask, obj, inh, sid); }
// AddAce: a list of ACEs inserted before ACE `index` (past the end: appended).
static WINAPI BOOL AddAce(uint8_t *acl, DWORD rev, DWORD index, const uint8_t *list, DWORD len) {
    if (!acl_ok(acl)) { vx_set_last_error(ERROR_INVALID_ACL); return 0; }
    if (rev < 2 || rev > 4) { vx_set_last_error(ERROR_REVISION_MISMATCH); return 0; }
    DWORD n = 0, count = 0;
    while (n < len) {
        if (n + 4 > len || rd16(list + n + 2) < 4 || n + rd16(list + n + 2) > len) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
        if (ace_is_object(list[n]) && rev < 4) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
        n += rd16(list + n + 2); count++;
    }
    DWORD end = acl_end(acl), at = 8, aces = rd16(acl + 4);
    if (end + n > rd16(acl + 2) || aces + count > 0xffff) { vx_set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    for (DWORD i = 0; i < index && i < aces; i++) at += rd16(acl + at + 2);
    memmove(acl + at + n, acl + at, end - at);
    memcpy(acl + at, list, n);
    wr16(acl + 4, (uint16_t)(aces + count));
    if (acl[0] < rev) acl[0] = (uint8_t)rev;
    return 1;
}
static WINAPI BOOL DeleteAce(uint8_t *acl, DWORD index) {
    if (!acl_ok(acl) || index >= rd16(acl + 4)) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    DWORD at = 8, end = acl_end(acl);
    for (DWORD i = 0; i < index; i++) at += rd16(acl + at + 2);
    DWORD n = rd16(acl + at + 2);
    memmove(acl + at, acl + at + n, end - at - n);
    wr16(acl + 4, (uint16_t)(rd16(acl + 4) - 1));
    return 1;
}
static WINAPI BOOL FindFirstFreeAce(uint8_t *acl, void **ace) {
    if (!acl_ok(acl)) { vx_set_last_error(ERROR_INVALID_ACL); return 0; }
    DWORD end = acl_end(acl);
    *ace = end < rd16(acl + 2) ? PROG(acl + end) : NULL;
    return 1;
}
static WINAPI BOOL GetAclInformation(const uint8_t *acl, uint8_t *info, DWORD n, int cls) {
    if (!acl_ok(acl)) { vx_set_last_error(ERROR_INVALID_ACL); return 0; }
    if (cls == 1) {                                       // AclRevisionInformation
        if (n < 4) { vx_set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
        wr32(info, acl[0]);
    } else if (cls == 2) {                                // AclSizeInformation: count, bytes in use, bytes free
        if (n < 12) { vx_set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
        DWORD end = acl_end(acl);
        wr32(info, rd16(acl + 4)); wr32(info + 4, end); wr32(info + 8, rd16(acl + 2) - end);
    } else { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    return 1;
}
static WINAPI BOOL SetAclInformation(uint8_t *acl, const uint8_t *info, DWORD n, int cls) {
    if (!acl_ok(acl)) { vx_set_last_error(ERROR_INVALID_ACL); return 0; }
    if (cls != 1 || n < 4) { vx_set_last_error(cls != 1 ? ERROR_INVALID_PARAMETER : ERROR_INSUFFICIENT_BUFFER); return 0; }
    DWORD rev = rd32(info);
    if (rev < 2 || rev > 4) { vx_set_last_error(ERROR_REVISION_MISMATCH); return 0; }
    acl[0] = (uint8_t)rev;
    return 1;
}
static WINAPI void MapGenericMask(DWORD *mask, const DWORD *map) {
    if (*mask & GENERIC_READ) *mask |= map[0];
    if (*mask & GENERIC_WRITE) *mask |= map[1];
    if (*mask & GENERIC_EXECUTE) *mask |= map[2];
    if (*mask & GENERIC_ALL) *mask |= map[3];
    *mask &= ~GENERIC_BITS;
}

// ---- security descriptors ----
static int sd_absolute(uint8_t *sd) {                     // sets the error unless sd is an absolute revision-1 descriptor
    if (!sd || sd[0] != 1) { vx_set_last_error(ERROR_UNKNOWN_REVISION); return 0; }
    if (rd16(sd + 2) & SE_SELF_RELATIVE) { vx_set_last_error(ERROR_INVALID_SECURITY_DESCR); return 0; }
    return 1;
}
static WINAPI BOOL InitializeSecurityDescriptor(uint8_t *sd, DWORD rev) {
    if (rev != 1) { vx_set_last_error(ERROR_UNKNOWN_REVISION); return 0; }
    memset(sd, 0, 40);
    sd[0] = 1;
    return 1;
}
static WINAPI BOOL IsValidSecurityDescriptor(const uint8_t *sd) {
    sd_parts p;
    if (!sd_valid(sd, &p)) { vx_set_last_error(ERROR_INVALID_SECURITY_DESCR); return 0; }
    return 1;
}
static WINAPI DWORD GetSecurityDescriptorLength(const uint8_t *sd) {
    sd_parts p;
    if (!sd_read(sd, &p)) return 0;
    return sd_rel_size(&p) + ((p.control & SE_SELF_RELATIVE) ? 0 : 20);
}
static WINAPI BOOL GetSecurityDescriptorControl(const uint8_t *sd, uint16_t *control, DWORD *rev) {
    *rev = sd[0];
    *control = rd16(sd + 2);
    if (sd[0] != 1) { vx_set_last_error(ERROR_UNKNOWN_REVISION); return 0; }
    return 1;
}
static WINAPI BOOL SetSecurityDescriptorControl(uint8_t *sd, uint16_t interest, uint16_t set) {
    const uint16_t settable = SE_DACL_AUTO_INHERIT_REQ | SE_SACL_AUTO_INHERIT_REQ | SE_DACL_AUTO_INHERITED |
                              SE_SACL_AUTO_INHERITED | SE_DACL_PROTECTED | SE_SACL_PROTECTED;
    if (!sd || sd[0] != 1) { vx_set_last_error(ERROR_UNKNOWN_REVISION); return 0; }
    if ((interest | set) & ~settable) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    wr16(sd + 2, (uint16_t)((rd16(sd + 2) & ~interest) | (set & interest)));
    return 1;
}
static BOOL sd_get_sid(const uint8_t *sd, int which, void **sid, BOOL *defaulted) {
    sd_parts p;
    if (!sd_read(sd, &p)) { vx_set_last_error(ERROR_UNKNOWN_REVISION); return 0; }
    const uint8_t *s = which ? p.group : p.owner;
    *sid = s ? PROG(s) : NULL;
    if (defaulted) *defaulted = (p.control & (which ? SE_GROUP_DEFAULTED : SE_OWNER_DEFAULTED)) != 0;
    return 1;
}
static WINAPI BOOL GetSecurityDescriptorOwner(const uint8_t *sd, void **owner, BOOL *defaulted) { return sd_get_sid(sd, 0, owner, defaulted); }
static WINAPI BOOL GetSecurityDescriptorGroup(const uint8_t *sd, void **group, BOOL *defaulted) { return sd_get_sid(sd, 1, group, defaulted); }
static BOOL sd_get_acl(const uint8_t *sd, int dacl, BOOL *present, void **acl, BOOL *defaulted) {
    sd_parts p;
    if (!sd_read(sd, &p)) { vx_set_last_error(ERROR_UNKNOWN_REVISION); return 0; }
    *present = (p.control & (dacl ? SE_DACL_PRESENT : SE_SACL_PRESENT)) != 0;
    if (*present) {
        const uint8_t *a = dacl ? p.dacl : p.sacl;
        *acl = a ? PROG(a) : NULL;
        if (defaulted) *defaulted = (p.control & (dacl ? SE_DACL_DEFAULTED : SE_SACL_DEFAULTED)) != 0;
    }
    return 1;
}
static WINAPI BOOL GetSecurityDescriptorDacl(const uint8_t *sd, BOOL *present, void **dacl, BOOL *defaulted) { return sd_get_acl(sd, 1, present, dacl, defaulted); }
static WINAPI BOOL GetSecurityDescriptorSacl(const uint8_t *sd, BOOL *present, void **sacl, BOOL *defaulted) { return sd_get_acl(sd, 0, present, sacl, defaulted); }
static BOOL sd_set_sid(uint8_t *sd, int which, const uint8_t *sid, BOOL defaulted) {
    if (!sd_absolute(sd)) return 0;
    uint16_t bit = which ? SE_GROUP_DEFAULTED : SE_OWNER_DEFAULTED;
    wrptr(sd + (which ? 16 : 8), sid);
    wr16(sd + 2, (uint16_t)((rd16(sd + 2) & ~bit) | (defaulted ? bit : 0)));
    return 1;
}
static WINAPI BOOL SetSecurityDescriptorOwner(uint8_t *sd, const uint8_t *owner, BOOL defaulted) { return sd_set_sid(sd, 0, owner, defaulted); }
static WINAPI BOOL SetSecurityDescriptorGroup(uint8_t *sd, const uint8_t *group, BOOL defaulted) { return sd_set_sid(sd, 1, group, defaulted); }
static BOOL sd_set_acl(uint8_t *sd, int dacl, BOOL present, const uint8_t *acl, BOOL defaulted) {
    if (!sd_absolute(sd)) return 0;
    uint16_t pres = dacl ? SE_DACL_PRESENT : SE_SACL_PRESENT, def = dacl ? SE_DACL_DEFAULTED : SE_SACL_DEFAULTED;
    uint16_t c = (uint16_t)(rd16(sd + 2) & ~(pres | def));
    if (present) c |= (uint16_t)(pres | (defaulted ? def : 0));
    wrptr(sd + (dacl ? 32 : 24), present ? acl : NULL);
    wr16(sd + 2, c);
    return 1;
}
static WINAPI BOOL SetSecurityDescriptorDacl(uint8_t *sd, BOOL present, const uint8_t *dacl, BOOL defaulted) { return sd_set_acl(sd, 1, present, dacl, defaulted); }
static WINAPI BOOL SetSecurityDescriptorSacl(uint8_t *sd, BOOL present, const uint8_t *sacl, BOOL defaulted) { return sd_set_acl(sd, 0, present, sacl, defaulted); }
static WINAPI BOOL MakeSelfRelativeSD(const uint8_t *abs, uint8_t *rel, DWORD *len) {
    sd_parts p;
    if (!sd_read(abs, &p)) { vx_set_last_error(ERROR_UNKNOWN_REVISION); return 0; }
    if (p.control & SE_SELF_RELATIVE) { vx_set_last_error(ERROR_BAD_DESCRIPTOR_FORMAT); return 0; }
    DWORD need = sd_rel_size(&p);
    if (!rel || *len < need) { *len = need; vx_set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    sd_rel_write(&p, rel);
    return 1;
}
static WINAPI BOOL MakeAbsoluteSD(const uint8_t *rel, uint8_t *abs, DWORD *abs_len, uint8_t *dacl, DWORD *dacl_len, uint8_t *sacl, DWORD *sacl_len,
                                  uint8_t *owner, DWORD *owner_len, uint8_t *group, DWORD *group_len) {
    sd_parts p;
    if (!sd_read(rel, &p)) { vx_set_last_error(ERROR_UNKNOWN_REVISION); return 0; }
    if (!(p.control & SE_SELF_RELATIVE)) { vx_set_last_error(ERROR_BAD_DESCRIPTOR_FORMAT); return 0; }
    DWORD need[5] = { 40, p.dacl ? acl_bytes(p.dacl) : 0, p.sacl ? acl_bytes(p.sacl) : 0, p.owner ? sid_len(p.owner) : 0, p.group ? sid_len(p.group) : 0 };
    DWORD *have[5] = { abs_len, dacl_len, sacl_len, owner_len, group_len };
    uint8_t *buf[5] = { abs, dacl, sacl, owner, group };
    int small = 0;
    for (int i = 0; i < 5; i++) if (!have[i] || *have[i] < need[i] || (need[i] && !buf[i])) small = 1;
    if (small) {
        for (int i = 0; i < 5; i++) if (have[i]) *have[i] = need[i];
        vx_set_last_error(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    memset(abs, 0, 40);
    abs[0] = 1;
    wr16(abs + 2, (uint16_t)(p.control & ~SE_SELF_RELATIVE));
    if (p.dacl) memcpy(dacl, p.dacl, rd16(p.dacl + 2));
    if (p.sacl) memcpy(sacl, p.sacl, rd16(p.sacl + 2));
    if (p.owner) memcpy(owner, p.owner, need[3]);
    if (p.group) memcpy(group, p.group, need[4]);
    wrptr(abs + 8, p.owner ? owner : NULL); wrptr(abs + 16, p.group ? group : NULL);
    wrptr(abs + 24, p.sacl ? sacl : NULL); wrptr(abs + 32, p.dacl ? dacl : NULL);
    for (int i = 0; i < 5; i++) *have[i] = need[i];
    return 1;
}

// ---- SDDL ----
static BOOL string_to_sd(const char *s, DWORD rev, void **sd, DWORD *size) {
    if (!s || !sd) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    if (rev != 1) { vx_set_last_error(ERROR_UNKNOWN_REVISION); return 0; }
    DWORD err, n;
    uint8_t *r = sddl_parse(s, &n, &err);
    if (!r) { vx_set_last_error(err); return 0; }
    *sd = PROG(r);
    if (size) *size = n;
    return 1;
}
static WINAPI BOOL ConvertStringSecurityDescriptorToSecurityDescriptorW(const WCHAR *s, DWORD rev, void **sd, DWORD *size) {
    char *a = vx_utf8_dup(s);
    BOOL r = string_to_sd(a, rev, sd, size);
    free(a);
    return r;
}
static WINAPI BOOL ConvertStringSecurityDescriptorToSecurityDescriptorA(const char *s, DWORD rev, void **sd, DWORD *size) { return string_to_sd(s, rev, sd, size); }
static BOOL sd_to_string(const uint8_t *sd, DWORD rev, DWORD info, void **out, DWORD *len, int wide) {
    if (rev != 1) { vx_set_last_error(ERROR_UNKNOWN_REVISION); return 0; }
    if (!out) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    sd_parts p;
    if (!sd_valid(sd, &p)) { vx_set_last_error(ERROR_INVALID_SECURITY_DESCR); return 0; }
    DWORD err;
    char *text = sddl_format(&p, info, &err);
    if (!text) { vx_set_last_error(err); return 0; }
    void *r = wide ? (void *)program_wide(text) : (void *)program_ansi(text);
    if (len) *len = (DWORD)(wide ? vx_utf8_to_utf16(text, strlen(text) + 1, NULL, 0) : strlen(text) + 1);   // (with the terminator)
    free(text);
    if (!r) { vx_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    *out = PROG(r);
    return 1;
}
static WINAPI BOOL ConvertSecurityDescriptorToStringSecurityDescriptorW(const uint8_t *sd, DWORD rev, DWORD info, WCHAR **out, DWORD *len) { return sd_to_string(sd, rev, info, (void **)out, len, 1); }
static WINAPI BOOL ConvertSecurityDescriptorToStringSecurityDescriptorA(const uint8_t *sd, DWORD rev, DWORD info, char **out, DWORD *len) { return sd_to_string(sd, rev, info, (void **)out, len, 0); }

// ---- tokens ----
static WINAPI BOOL OpenProcessToken(HANDLE process, DWORD access, HANDLE *out) {
    (void)access;
    int pid = (intptr_t)process == -1 ? getpid() : vx_process_info(process, NULL);
    if (pid < 0) { vx_set_last_error(ERROR_INVALID_HANDLE); return 0; }
    pthread_mutex_lock(&g_lock);
    // another vxrt process runs with the token every process starts with: a new one like it (changes to it stay its own)
    token *t = pid == getpid() ? token_ref(process_token()) : token_default();
    pthread_mutex_unlock(&g_lock);
    *out = token_handle(t);
    token_unref(t);
    return *out != NULL;
}
static WINAPI BOOL OpenThreadToken(HANDLE thread, DWORD access, BOOL as_self, HANDLE *out) {
    (void)access; (void)as_self;
    int foreign;
    DWORD tid = thread_id_of(thread, &foreign);
    *out = NULL;
    if (!tid) return no_thread(foreign, "OpenThreadToken");
    pthread_mutex_lock(&g_lock);
    token *t = token_ref(thread_token_of(tid));
    pthread_mutex_unlock(&g_lock);
    if (!t) { vx_set_last_error(ERROR_NO_TOKEN); return 0; }
    *out = token_handle(t);
    token_unref(t);
    return *out != NULL;
}
static WINAPI BOOL GetTokenInformation(HANDLE h, int cls, uint8_t *out, DWORD size, DWORD *ret) {
    DWORD err = 0;
    token *t = token_get(h, &err);
    if (!t) { vx_set_last_error(err); return 0; }
    blob b = { 0 };
    int fixed = 0;
    pthread_mutex_lock(&g_lock);
    switch (cls) {
    case 1: { size_t at = b_room(&b, 16, 8); b_sid_ptr(&b, at, t->user.sid); wr32(b.p + at + 8, t->user.attr); break; }   // TokenUser
    case 2: b_groups(&b, t->groups, t->ngroups); break;                                            // TokenGroups
    case 3: { size_t at = b_room(&b, 4, 4); wr32(b.p + at, (DWORD)t->nprivs); b_privileges(&b, t); break; }   // TokenPrivileges
    case 4: { size_t at = b_room(&b, 8, 8); b_sid_ptr(&b, at, t->owner); break; }                  // TokenOwner
    case 5: { size_t at = b_room(&b, 8, 8); b_sid_ptr(&b, at, t->primary); break; }                // TokenPrimaryGroup
    case 6: {                                                                                      // TokenDefaultDacl
        size_t at = b_room(&b, 8, 8);
        if (t->dacl) { size_t a = b_room(&b, rd16(t->dacl + 2), 4); memcpy(b.p + a, t->dacl, rd16(t->dacl + 2)); b_ptr(&b, at, a); }
        break;
    }
    case 7: { fixed = 1; size_t at = b_room(&b, 16, 4); memcpy(b.p + at, "User32  ", 8); wr32(b.p + at + 8, LOGON_ID); break; }   // TokenSource
    case 8: fixed = 1; b_dword(&b, (DWORD)t->type); break;                                          // TokenType
    case 9: if (t->type != TOKEN_IMPERSONATION) err = ERROR_INVALID_PARAMETER; else { fixed = 1; b_dword(&b, (DWORD)t->level); } break;
    case 10: {                                                                                     // TokenStatistics
        fixed = 1;
        size_t at = b_room(&b, 56, 8);
        uint64_t never = 0x7fffffffffffffffull, logon = LOGON_ID;
        memcpy(b.p + at, &t->id, 8); memcpy(b.p + at + 8, &logon, 8); memcpy(b.p + at + 16, &never, 8);
        wr32(b.p + at + 24, (DWORD)t->type); wr32(b.p + at + 28, (DWORD)t->level);
        DWORD used = t->dacl ? rd16(t->dacl + 2) : 0;
        wr32(b.p + at + 32, 4096); wr32(b.p + at + 36, 4096 - used);
        wr32(b.p + at + 40, (DWORD)t->ngroups); wr32(b.p + at + 44, (DWORD)t->nprivs);
        memcpy(b.p + at + 48, &t->modified, 8);
        break;
    }
    case 11: b_groups(&b, t->restricted, t->nrestricted); break;                                    // TokenRestrictedSids
    case 12: fixed = 1; b_dword(&b, t->session); break;                                             // TokenSessionId
    case 13: {                                                                                     // TokenGroupsAndPrivileges
        size_t at = b_room(&b, 56, 8);
        DWORD sids_len = 16 * (DWORD)(t->ngroups + 1) + sid_len(t->user.sid), rlen = 16 * (DWORD)t->nrestricted;
        for (int i = 0; i < t->ngroups; i++) sids_len += sid_len(t->groups[i].sid);
        for (int i = 0; i < t->nrestricted; i++) rlen += sid_len(t->restricted[i].sid);
        size_t sids = b_sid_array(&b, &t->user, t->groups, t->ngroups);
        size_t rs = t->nrestricted ? b_sid_array(&b, NULL, t->restricted, t->nrestricted) : 0;
        size_t privs = b_privileges(&b, t);
        uint64_t logon = LOGON_ID;
        wr32(b.p + at, (DWORD)t->ngroups + 1); wr32(b.p + at + 4, sids_len); b_ptr(&b, at + 8, sids);
        wr32(b.p + at + 16, (DWORD)t->nrestricted); wr32(b.p + at + 20, rlen); if (t->nrestricted) b_ptr(&b, at + 24, rs);
        wr32(b.p + at + 32, (DWORD)t->nprivs); wr32(b.p + at + 36, 12 * (DWORD)t->nprivs); if (t->nprivs) b_ptr(&b, at + 40, privs);
        memcpy(b.p + at + 48, &logon, 8);
        break;
    }
    case 15: fixed = 1; b_dword(&b, t->sandbox_inert); break;                                      // TokenSandBoxInert
    case 17: { fixed = 1; size_t at = b_room(&b, 8, 4); wr32(b.p + at, 0x3e7); break; }            // TokenOrigin: the system's logon session
    case 18: fixed = 1; b_dword(&b, 1); break;                                                     // TokenElevationType: TokenElevationTypeDefault
    case 19: err = ERROR_NO_SUCH_LOGON_SESSION; break;                                             // TokenLinkedToken: none (not a split admin token)
    case 20: fixed = 1; b_dword(&b, 0); break;                                                     // TokenElevation: not elevated
    case 21: fixed = 1; b_dword(&b, t->filtered); break;                                           // TokenHasRestrictions
    case 23: fixed = 1; b_dword(&b, 0); break;                                                     // TokenVirtualizationAllowed
    case 24: fixed = 1; b_dword(&b, t->virt_enabled); break;                                       // TokenVirtualizationEnabled
    case 25: {                                                                                     // TokenIntegrityLevel
        sid_attr *l = token_label(t);
        uint8_t medium[12];
        size_t at = b_room(&b, 16, 8);
        if (!l) sid_of(medium, 16, 1, 8192);
        b_sid_ptr(&b, at, l ? l->sid : medium);
        wr32(b.p + at + 8, SE_GROUP_INTEGRITY | SE_GROUP_INTEGRITY_ENABLED);
        break;
    }
    case 26: fixed = 1; b_dword(&b, 0); break;                                                     // TokenUIAccess
    case 27: fixed = 1; b_dword(&b, t->policy); break;                                             // TokenMandatoryPolicy
    case 28: {                                                                                     // TokenLogonSid
        sid_attr *logons = NULL; int n = 0;
        for (int i = 0; i < t->ngroups; i++) if ((t->groups[i].attr & SE_GROUP_LOGON_ID) == SE_GROUP_LOGON_ID) add_sid_attr(&logons, &n, t->groups[i].sid, t->groups[i].attr);
        if (n) b_groups(&b, logons, n); else err = ERROR_NOT_FOUND;
        free(logons);
        break;
    }
    case 29: fixed = 1; b_dword(&b, 0); break;                                                     // TokenIsAppContainer
    case 30: b_groups(&b, NULL, 0); break;                                                         // TokenCapabilities: none
    case 31: b_room(&b, 8, 8); break;                                                              // TokenAppContainerSid: none
    case 32: fixed = 1; b_dword(&b, 0); break;                                                     // TokenAppContainerNumber
    case 40: fixed = 1; b_dword(&b, t->is_restricted != 0); break;                                  // TokenIsRestricted
    default: err = ERROR_INVALID_PARAMETER;
    }
    pthread_mutex_unlock(&g_lock);
    token_unref(t);
    if (!err) {
        if (ret) *ret = (DWORD)b.n;
        if (!out || size < b.n) err = fixed ? ERROR_BAD_LENGTH : ERROR_INSUFFICIENT_BUFFER;
        else b_out(&b, out);
    } else if (ret) *ret = 0;
    free(b.p); free(b.fix);
    if (err) { vx_set_last_error(err); return 0; }
    return 1;
}
static WINAPI BOOL SetTokenInformation(HANDLE h, int cls, const uint8_t *in, DWORD size) {
    DWORD err = 0;
    token *t = token_get(h, &err);
    if (!t) { vx_set_last_error(err); return 0; }
    pthread_mutex_lock(&g_lock);
    switch (cls) {
    case 4: case 5: {                                     // TokenOwner, TokenPrimaryGroup: one of the token's own SIDs
        const uint8_t *sid = size >= 8 && in ? rdptr(in) : NULL;
        if (size < 8 || !in) { err = ERROR_BAD_LENGTH; break; }
        if (!sid_ok(sid)) { err = ERROR_INVALID_SID; break; }
        int ok = sid_eq(sid, t->user.sid);
        for (int i = 0; i < t->ngroups && !ok; i++) ok = sid_eq(sid, t->groups[i].sid) && (cls == 5 || (t->groups[i].attr & SE_GROUP_OWNER));
        if (!ok) { err = cls == 4 ? ERROR_INVALID_OWNER : ERROR_INVALID_PRIMARY_GROUP; break; }
        memcpy(cls == 4 ? t->owner : t->primary, sid, sid_len(sid));
        break;
    }
    case 6: {                                             // TokenDefaultDacl (NULL: none)
        const uint8_t *acl = size >= 8 && in ? rdptr(in) : NULL;
        if (size < 8 || !in) { err = ERROR_BAD_LENGTH; break; }
        if (acl && !acl_ok(acl)) { err = ERROR_INVALID_ACL; break; }
        free(t->dacl);
        t->dacl = acl ? malloc(acl_bytes(acl)) : NULL;
        if (acl) memcpy(t->dacl, acl, acl_bytes(acl));
        break;
    }
    case 24:                                              // TokenVirtualizationEnabled
        if (size < 4 || !in) { err = ERROR_BAD_LENGTH; break; }
        t->virt_enabled = rd32(in) != 0;
        break;
    case 25: {                                            // TokenIntegrityLevel: lowered, never raised (that takes SeTcbPrivilege)
        const uint8_t *sid = size >= 16 && in ? rdptr(in) : NULL;
        if (size < 16 || !in) { err = ERROR_BAD_LENGTH; break; }
        if (!sid_ok(sid) || sid_auth(sid) != 16 || sid[1] != 1) { err = ERROR_INVALID_PARAMETER; break; }
        sid_attr *l = token_label(t);
        if (sid_sub(sid, 0) > token_integrity(t)) { err = ERROR_PRIVILEGE_NOT_HELD; break; }
        if (l) memcpy(l->sid, sid, sid_len(sid));
        else add_sid_attr(&t->groups, &t->ngroups, sid, SE_GROUP_INTEGRITY | SE_GROUP_INTEGRITY_ENABLED);
        break;
    }
    case 12: case 14: case 15: case 17: case 26: case 27: // session, references, origin, UI access, mandatory policy: SeTcbPrivilege
        err = ERROR_PRIVILEGE_NOT_HELD;
        break;
    default: err = ERROR_INVALID_PARAMETER;
    }
    if (!err) t->modified = new_luid();
    pthread_mutex_unlock(&g_lock);
    token_unref(t);
    if (err) { vx_set_last_error(err); return 0; }
    return 1;
}
static token *duplicate(HANDLE h, int type, int level, const uint8_t *sa, DWORD *err) {
    token *s = token_get(h, err);
    if (!s) return NULL;
    if ((type != TOKEN_PRIMARY && type != TOKEN_IMPERSONATION) || level < 0 || level > 3) { token_unref(s); *err = ERROR_INVALID_PARAMETER; return NULL; }
    // an impersonation token gives no more than its level: a higher one, or a primary token from one below Impersonation
    if (s->type == TOKEN_IMPERSONATION && (level > s->level || (type == TOKEN_PRIMARY && s->level < SECURITY_IMPERSONATION))) {
        token_unref(s); *err = ERROR_BAD_IMPERSONATION_LEVEL; return NULL;
    }
    const uint8_t *sd = sa && rd32(sa) >= 16 ? rdptr(sa + 8) : NULL;
    sd_parts p;
    uint8_t *kept = NULL;
    if (sd && !sd_valid(sd, &p)) { token_unref(s); *err = ERROR_INVALID_SECURITY_DESCR; return NULL; }
    if (sd) {                                             // the new token's descriptor: what SetSecurityInfo would take
        uint8_t *audit = p.sacl ? labels_of(p.sacl, 0) : NULL;   // (a SACL's other ACEs need SeSecurityPrivilege)
        DWORD info = (p.owner ? OWNER_SI : 0) | (p.group ? GROUP_SI : 0) | (p.control & SE_DACL_PRESENT ? DACL_SI : 0) |
                     (p.sacl ? (rd16(audit + 4) ? SACL_SI : LABEL_SI) : 0);
        free(audit);
        if ((*err = check_settable(info, p.owner, p.group, p.dacl, p.sacl))) { token_unref(s); return NULL; }
        pthread_mutex_lock(&g_lock);
        *err = unchecked_set(SE_KERNEL_OBJECT, TOKEN_ALL_ACCESS, NULL, info, p.owner, p.group, p.dacl, p.sacl, &kept, "DuplicateTokenEx");
        pthread_mutex_unlock(&g_lock);
        if (*err) { token_unref(s); return NULL; }
    }
    pthread_mutex_lock(&g_lock);
    token *t = token_copy(s);
    t->type = type; t->level = type == TOKEN_IMPERSONATION ? level : 0;
    t->sd = kept;
    pthread_mutex_unlock(&g_lock);
    token_unref(s);
    return t;
}
static WINAPI BOOL DuplicateTokenEx(HANDLE h, DWORD access, const uint8_t *sa, int level, int type, HANDLE *out) {
    (void)access;
    DWORD err;
    token *t = duplicate(h, type, level, sa, &err);
    if (!t) { vx_set_last_error(err); return 0; }
    *out = token_handle(t);
    token_unref(t);
    return *out != NULL;
}
static WINAPI BOOL DuplicateToken(HANDLE h, int level, HANDLE *out) { return DuplicateTokenEx(h, 0, NULL, level, TOKEN_IMPERSONATION, out); }
static WINAPI BOOL CreateRestrictedToken(HANDLE h, DWORD flags, DWORD ndisable, const uint8_t *disable, DWORD ndelete, const uint8_t *del,
                                         DWORD nrestrict, const uint8_t *restrict_sids, HANDLE *out) {
    if ((flags & ~0xfu) || (ndisable && !disable) || (ndelete && !del) || (nrestrict && !restrict_sids) || !out) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    for (DWORD i = 0; i < ndisable; i++) if (!sid_ok(rdptr(disable + 16 * i))) { vx_set_last_error(ERROR_INVALID_SID); return 0; }
    for (DWORD i = 0; i < nrestrict; i++) if (!sid_ok(rdptr(restrict_sids + 16 * i))) { vx_set_last_error(ERROR_INVALID_SID); return 0; }
    DWORD err;
    token *s = token_get(h, &err);
    if (!s) { vx_set_last_error(err); return 0; }
    pthread_mutex_lock(&g_lock);
    token *t = token_copy(s);
    for (int i = 0; i < t->nprivs; i++) {                 // DISABLE_MAX_PRIVILEGE keeps only "bypass traverse checking"
        int drop = (flags & 1) && t->privs[i].luid != SE_CHANGE_NOTIFY_PRIVILEGE;
        for (DWORD k = 0; k < ndelete && !drop; k++) drop = rd32(del + 12 * k) == t->privs[i].luid && !rd32(del + 12 * k + 4);
        if (drop) { memmove(&t->privs[i], &t->privs[i + 1], (size_t)(t->nprivs - i - 1) * sizeof *t->privs); t->nprivs--; i--; }
    }
    for (DWORD k = 0; k < ndisable; k++) {               // SIDs to disable: deny-only from now on
        const uint8_t *sid = rdptr(disable + 16 * k);
        if (sid_eq(sid, t->user.sid)) t->user.attr |= SE_GROUP_USE_FOR_DENY_ONLY;
        for (int i = 0; i < t->ngroups; i++)
            if (!(t->groups[i].attr & SE_GROUP_INTEGRITY) && sid_eq(sid, t->groups[i].sid))
                t->groups[i].attr = (t->groups[i].attr & ~(DWORD)(SE_GROUP_ENABLED | SE_GROUP_ENABLED_BY_DEFAULT)) | SE_GROUP_USE_FOR_DENY_ONLY;
    }
    // A restricted token made from another gives no more than it: its restricting SIDs are those both lists have (none
    // in common: none, and the token stays restricted, so the second pass grants nothing), and it is write-restricted
    // only when both are (a fully restricted token's reads stay checked against its restricting SIDs).
    if (nrestrict) {
        sid_attr *list = NULL; int n = 0;
        for (DWORD k = 0; k < nrestrict; k++) {
            const uint8_t *sid = rdptr(restrict_sids + 16 * k);
            int keep = !s->is_restricted, dup = 0;
            for (int i = 0; i < s->nrestricted && !keep; i++) keep = sid_eq(s->restricted[i].sid, sid);
            for (int i = 0; i < n && !dup; i++) dup = sid_eq(list[i].sid, sid);
            if (keep && !dup) add_sid_attr(&list, &n, sid, GROUP_ON);
        }
        free(t->restricted); t->restricted = list; t->nrestricted = n;
    }
    int write_only = (flags & 8) != 0;                    // WRITE_RESTRICTED
    t->write_restricted = write_only && (!s->is_restricted || s->write_restricted);
    t->is_restricted = s->is_restricted || nrestrict || write_only;   // (once restricted, always)
    if (flags & 2) t->sandbox_inert = 1;
    t->filtered = 1;
    pthread_mutex_unlock(&g_lock);
    token_unref(s);
    *out = token_handle(t);
    token_unref(t);
    return *out != NULL;
}
static WINAPI BOOL IsTokenRestricted(HANDLE h) {
    DWORD err;
    token *t = token_get(h, &err);
    if (!t) { vx_set_last_error(err); return 0; }
    BOOL r = t->is_restricted != 0;
    token_unref(t);
    vx_set_last_error(0);
    return r;
}
static WINAPI BOOL AllocateLocallyUniqueId(DWORD *luid) { uint64_t v = new_luid(); luid[0] = (DWORD)v; luid[1] = (DWORD)(v >> 32); return 1; }

// ---- privileges ----
// Their names and LUIDs (winnt.h's SE_*_NAME; the LUID's low part is wdm.h's SE_*_PRIVILEGE, its high part 0).
static const char *const k_privileges[] = {
    [2] = "SeCreateTokenPrivilege", [3] = "SeAssignPrimaryTokenPrivilege", [4] = "SeLockMemoryPrivilege",
    [5] = "SeIncreaseQuotaPrivilege", [6] = "SeMachineAccountPrivilege", [7] = "SeTcbPrivilege", [8] = "SeSecurityPrivilege",
    [9] = "SeTakeOwnershipPrivilege", [10] = "SeLoadDriverPrivilege", [11] = "SeSystemProfilePrivilege",
    [12] = "SeSystemtimePrivilege", [13] = "SeProfileSingleProcessPrivilege", [14] = "SeIncreaseBasePriorityPrivilege",
    [15] = "SeCreatePagefilePrivilege", [16] = "SeCreatePermanentPrivilege", [17] = "SeBackupPrivilege",
    [18] = "SeRestorePrivilege", [19] = "SeShutdownPrivilege", [20] = "SeDebugPrivilege", [21] = "SeAuditPrivilege",
    [22] = "SeSystemEnvironmentPrivilege", [23] = "SeChangeNotifyPrivilege", [24] = "SeRemoteShutdownPrivilege",
    [25] = "SeUndockPrivilege", [26] = "SeSyncAgentPrivilege", [27] = "SeEnableDelegationPrivilege",
    [28] = "SeManageVolumePrivilege", [29] = "SeImpersonatePrivilege", [30] = "SeCreateGlobalPrivilege",
    [31] = "SeTrustedCredManAccessPrivilege", [32] = "SeRelabelPrivilege", [33] = "SeIncreaseWorkingSetPrivilege",
    [34] = "SeTimeZonePrivilege", [35] = "SeCreateSymbolicLinkPrivilege", [36] = "SeDelegateSessionUserImpersonatePrivilege",
};
static BOOL privilege_value(const char *name, DWORD *luid) {
    if (!luid) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    for (DWORD i = 0; name && i < COUNT(k_privileges); i++)
        if (k_privileges[i] && !strcasecmp(k_privileges[i], name)) { luid[0] = i; luid[1] = 0; return 1; }
    vx_set_last_error(ERROR_NO_SUCH_PRIVILEGE);
    return 0;
}
static WINAPI BOOL LookupPrivilegeValueA(const char *sys, const char *name, DWORD *luid) { (void)sys; return privilege_value(name, luid); }
static WINAPI BOOL LookupPrivilegeValueW(const WCHAR *sys, const WCHAR *name, DWORD *luid) {
    (void)sys;
    char a[64] = "";
    if (name && vx_wcslen(name) < sizeof a) vx_utf16_to_utf8(name, vx_wcslen(name) + 1, a, sizeof a);
    return privilege_value(name && a[0] ? a : NULL, luid);
}
// LookupPrivilegeName: *n is the buffer's size in characters; on return the name's length, or (too small) the size
// it needs with its terminating zero.
static BOOL privilege_name(const DWORD *luid, void *out, DWORD *n, int wide) {
    const char *name = luid && !luid[1] && luid[0] < COUNT(k_privileges) ? k_privileges[luid[0]] : NULL;
    if (!name) { vx_set_last_error(ERROR_NO_SUCH_PRIVILEGE); return 0; }
    if (!n) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    DWORD len = (DWORD)strlen(name);
    if (!out || *n <= len) { *n = len + 1; vx_set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    if (wide) vx_utf8_to_utf16(name, len + 1, out, len + 1); else memcpy(out, name, len + 1);
    *n = len;
    return 1;
}
static WINAPI BOOL LookupPrivilegeNameW(const WCHAR *sys, const DWORD *luid, WCHAR *name, DWORD *n) { (void)sys; return privilege_name(luid, name, n, 1); }
static WINAPI BOOL LookupPrivilegeNameA(const char *sys, const DWORD *luid, char *name, DWORD *n) { (void)sys; return privilege_name(luid, name, n, 0); }
// AdjustTokenPrivileges: enables, disables or removes privileges the token has (it gives none it lacks: those make
// the last error ERROR_NOT_ALL_ASSIGNED, with success). The previous state lists the privileges it changed, with
// their attributes before; when that does not fit, nothing changes and *ret says the size needed.
static WINAPI BOOL AdjustTokenPrivileges(HANDLE h, BOOL disable_all, const uint8_t *state, DWORD len, uint8_t *prev, DWORD *ret) {
    if ((!disable_all && !state) || (prev && !ret)) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    DWORD err;
    token *t = token_get(h, &err);
    if (!t) { vx_set_last_error(err); return 0; }
    pthread_mutex_lock(&g_lock);
    DWORD *attr = calloc((size_t)t->nprivs + 1, sizeof *attr), changed = 0;
    int missing = 0;
    for (int i = 0; i < t->nprivs; i++) attr[i] = t->privs[i].attr;
    if (disable_all) for (int i = 0; i < t->nprivs; i++) attr[i] &= ~(DWORD)SE_PRIVILEGE_ENABLED;
    else
        for (DWORD k = 0, count = rd32(state); k < count; k++) {
            const uint8_t *e = state + 4 + 12 * (size_t)k;
            DWORD low = rd32(e), high = rd32(e + 4), a = rd32(e + 8);
            int i = 0;
            while (i < t->nprivs && (high || t->privs[i].luid != low)) i++;
            if (i == t->nprivs) { missing = 1; continue; }
            if (a & SE_PRIVILEGE_REMOVED) attr[i] = SE_PRIVILEGE_REMOVED;   // (marks it to go)
            else if (a & SE_PRIVILEGE_ENABLED) attr[i] |= SE_PRIVILEGE_ENABLED;
            else attr[i] &= ~(DWORD)SE_PRIVILEGE_ENABLED;
        }
    for (int i = 0; i < t->nprivs; i++) changed += attr[i] != t->privs[i].attr;
    DWORD need = 4 + 12 * changed;
    if (ret) *ret = need;
    err = 0;
    if (prev && len < need) err = ERROR_INSUFFICIENT_BUFFER;
    else {
        if (prev) {
            wr32(prev, changed);
            for (int i = 0, k = 0; i < t->nprivs; i++)
                if (attr[i] != t->privs[i].attr) { wr32(prev + 4 + 12 * k, t->privs[i].luid); wr32(prev + 8 + 12 * k, 0); wr32(prev + 12 + 12 * k, t->privs[i].attr); k++; }
        }
        int n = 0;
        for (int i = 0; i < t->nprivs; i++) if (attr[i] != SE_PRIVILEGE_REMOVED) { t->privs[n] = t->privs[i]; t->privs[n++].attr = attr[i]; }
        t->nprivs = n;
        if (changed) t->modified = new_luid();
    }
    pthread_mutex_unlock(&g_lock);
    free(attr);
    token_unref(t);
    if (err) { vx_set_last_error(err); return 0; }
    vx_set_last_error(missing ? ERROR_NOT_ALL_ASSIGNED : 0);
    return 1;
}
// PrivilegeCheck: whether a client's (impersonation) token has the privileges of a PRIVILEGE_SET (count, control, then
// LUID_AND_ATTRIBUTES) enabled, all of them or any one as its control says; each one enabled is marked used.
static WINAPI BOOL PrivilegeCheck(HANDLE h, uint8_t *set, BOOL *result) {
    if (!set || !result) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    DWORD err;
    token *t = token_get(h, &err);
    if (!t) { vx_set_last_error(err); return 0; }
    if (t->type != TOKEN_IMPERSONATION) { token_unref(t); vx_set_last_error(ERROR_NO_IMPERSONATION_TOKEN); return 0; }
    if (t->level < SECURITY_IDENTIFICATION) { token_unref(t); vx_set_last_error(ERROR_BAD_IMPERSONATION_LEVEL); return 0; }
    DWORD count = rd32(set), held = 0;
    pthread_mutex_lock(&g_lock);
    for (DWORD k = 0; k < count; k++) {
        uint8_t *e = set + 8 + 12 * (size_t)k;
        int on = 0;
        for (int i = 0; i < t->nprivs && !on; i++) on = !rd32(e + 4) && t->privs[i].luid == rd32(e) && (t->privs[i].attr & SE_PRIVILEGE_ENABLED);
        wr32(e + 8, on ? rd32(e + 8) | SE_PRIVILEGE_USED_FOR_ACCESS : rd32(e + 8) & ~SE_PRIVILEGE_USED_FOR_ACCESS);
        held += on;
    }
    pthread_mutex_unlock(&g_lock);
    token_unref(t);
    *result = rd32(set + 4) & PRIVILEGE_SET_ALL_NECESSARY ? held == count : held > 0;
    return 1;
}

// ---- impersonation ----
static BOOL impersonate(DWORD tid, token *t) {            // takes over a reference to t (NULL: revert)
    pthread_mutex_lock(&g_lock);
    thread_token_set(tid, t);
    pthread_mutex_unlock(&g_lock);
    return 1;
}
static token *impersonation_copy(token *s, int level) {  // (g_lock not held)
    pthread_mutex_lock(&g_lock);
    token *t = token_copy(s);
    t->type = TOKEN_IMPERSONATION; t->level = level;
    pthread_mutex_unlock(&g_lock);
    return t;
}
static WINAPI BOOL SetThreadToken(HANDLE *thread, HANDLE h) {
    int foreign = 0;
    DWORD tid = thread ? thread_id_of(*thread, &foreign) : vx_current_tid(), err;
    if (!tid) return no_thread(foreign, "SetThreadToken");
    token *t = NULL;
    if (h) {
        if (!(t = token_get(h, &err))) { vx_set_last_error(err); return 0; }
        if (t->type != TOKEN_IMPERSONATION) { token_unref(t); vx_set_last_error(ERROR_BAD_TOKEN_TYPE); return 0; }
    }
    return impersonate(tid, t);
}
static WINAPI BOOL RevertToSelf(void) { return impersonate(vx_current_tid(), NULL); }
static WINAPI BOOL ImpersonateSelf(int level) {
    if (level < 0 || level > 3) { vx_set_last_error(ERROR_BAD_IMPERSONATION_LEVEL); return 0; }
    pthread_mutex_lock(&g_lock);
    token *p = token_ref(process_token());
    pthread_mutex_unlock(&g_lock);
    token *t = impersonation_copy(p, level);
    token_unref(p);
    return impersonate(vx_current_tid(), t);
}
static WINAPI BOOL ImpersonateLoggedOnUser(HANDLE h) {
    DWORD err;
    token *t = token_get(h, &err);
    if (!t) { vx_set_last_error(err); return 0; }
    if (t->type == TOKEN_PRIMARY) { token *c = impersonation_copy(t, SECURITY_IMPERSONATION); token_unref(t); t = c; }
    return impersonate(vx_current_tid(), t);
}
// "Network access: Let Everyone permissions apply to anonymous users" (HKLM\SYSTEM\CurrentControlSet\Control\Lsa,
// EveryoneIncludesAnonymous): off unless set to a nonzero DWORD, as Windows has it by default. Read at each use (a
// change to the policy needs no restart).
static int everyone_includes_anonymous(void) {
    LONG (WINAPI *get)(HANDLE, const char *, const char *, DWORD, DWORD *, void *, DWORD *) =
        (LONG (WINAPI *)(HANDLE, const char *, const char *, DWORD, DWORD *, void *, DWORD *))vx_find_export("advapi32.dll", "RegGetValueA");
    DWORD value = 0, size = sizeof value;
    return get && !get((HANDLE)(intptr_t)(int32_t)0x80000002 /* HKEY_LOCAL_MACHINE */, "SYSTEM\\CurrentControlSet\\Control\\Lsa",
                       "EveryoneIncludesAnonymous", 0x10 /* RRF_RT_REG_DWORD */, NULL, &value, &size) && value;
}
// The anonymous logon: ANONYMOUS LOGON as the user, in NETWORK, untrusted integrity, no privileges; in Everyone only
// when the policy above says so (by default, Everyone's permissions do not apply to anonymous users).
static WINAPI BOOL ImpersonateAnonymousToken(HANDLE thread) {
    int foreign;
    DWORD tid = thread_id_of(thread, &foreign);
    if (!tid) return no_thread(foreign, "ImpersonateAnonymousToken");
    int everyone = everyone_includes_anonymous();
    token *t = token_new();
    uint8_t s[SID_MAX];
    t->type = TOKEN_IMPERSONATION; t->level = SECURITY_IMPERSONATION;
    sid_of(t->user.sid, 5, 1, 7);
    if (everyone) { sid_of(s, 1, 1, 0); add_sid_attr(&t->groups, &t->ngroups, s, GROUP_ON); }
    sid_of(s, 5, 1, 2); add_sid_attr(&t->groups, &t->ngroups, s, GROUP_ON);
    sid_of(s, 16, 1, 0); add_sid_attr(&t->groups, &t->ngroups, s, SE_GROUP_INTEGRITY | SE_GROUP_INTEGRITY_ENABLED);
    memcpy(t->owner, t->user.sid, SID_MAX); memcpy(t->primary, t->user.sid, SID_MAX);
    t->policy = 3;
    return impersonate(tid, t);
}
// A pipe's client runs as the same user (every vxrt process does): its token is this process's, at Impersonation.
static WINAPI BOOL ImpersonateNamedPipeClient(HANDLE pipe) {
    if (!pipe || !vx_handle_data(pipe, VX_HANDLE_PIPE)) { vx_set_last_error(ERROR_INVALID_HANDLE); return 0; }
    return ImpersonateSelf(SECURITY_IMPERSONATION);
}
// What `t` gives less than the token every process here starts with (token_default), NULL if nothing: another user,
// restricting SIDs, a group made deny-only or left out, a privilege removed, a lower integrity level.
static const char *token_weaker(const token *t) {        // (g_lock held)
    token *d = token_default();
    const char *why = NULL;
    if (!sid_eq(t->user.sid, d->user.sid) || (t->user.attr & SE_GROUP_USE_FOR_DENY_ONLY)) why = "another user, or the user deny-only";
    else if (t->is_restricted) why = t->write_restricted ? "write-restricted" : "restricting SIDs";
    else if (token_integrity((token *)t) < token_integrity(d)) why = "a lower integrity level";
    for (int i = 0; i < d->ngroups && !why; i++) {
        if (d->groups[i].attr & SE_GROUP_INTEGRITY) continue;
        int on = 0;
        for (int k = 0; k < t->ngroups && !on; k++)
            on = sid_eq(t->groups[k].sid, d->groups[i].sid) && (t->groups[k].attr & SE_GROUP_ENABLED) && !(t->groups[k].attr & SE_GROUP_USE_FOR_DENY_ONLY);
        if (!on) why = "a group deny-only or missing";
    }
    for (int i = 0; i < d->nprivs && !why; i++) {          // (one only disabled can be enabled again: not less)
        int held = 0;
        for (int k = 0; k < t->nprivs && !held; k++) held = t->privs[k].luid == d->privs[i].luid;
        if (!held) why = "a privilege removed";
    }
    if (!why && t->filtered) why = "made by CreateRestrictedToken";
    token_free(d);
    return why;
}
// CreateProcessAsUser: CreateProcess with a primary token. Every process here starts with the user's own token and
// nothing on the Mac would hold the child to less, so a token that gives less (a sandbox's restricted or
// low-integrity token) is refused rather than silently not applied; the program then knows its child would not be
// confined (Chromium's sandbox, for one, can be turned off by its own switch, --no-sandbox). A token equal to the
// user's (a plain duplicate) starts the child.
typedef BOOL (WINAPI *create_process_fn)(const void *, void *, void *, void *, BOOL, DWORD, void *, const void *, void *, void *);
static BOOL as_user(HANDLE h, const char *fn, const void *app, void *cmd, void *pa, void *ta, BOOL inherit, DWORD flags, void *env, const void *dir, void *si, void *pi) {
    if (h) {
        DWORD err;
        token *t = token_get(h, &err);
        if (!t) { vx_set_last_error(err); return 0; }
        int primary = t->type == TOKEN_PRIMARY;
        pthread_mutex_lock(&g_lock);
        const char *why = primary ? token_weaker(t) : NULL;
        pthread_mutex_unlock(&g_lock);
        token_unref(t);
        if (!primary) { vx_set_last_error(ERROR_BAD_TOKEN_TYPE); return 0; }
        if (why) {
            vx_log("CreateProcessAsUser%c: the token gives less than the user's own (%s), and the child would run with the user's: refused", fn[13], why);
            vx_set_last_error(ERROR_NOT_SUPPORTED);
            return 0;
        }
    }
    create_process_fn create = (create_process_fn)vx_find_export("kernel32.dll", fn);
    if (!create) { vx_set_last_error(ERROR_NOT_SUPPORTED); return 0; }
    return create(app, cmd, pa, ta, inherit, flags, env, dir, si, pi);
}
static WINAPI BOOL CreateProcessAsUserW(HANDLE h, const WCHAR *app, WCHAR *cmd, void *pa, void *ta, BOOL inherit, DWORD flags, void *env, const WCHAR *dir, void *si, void *pi) {
    return as_user(h, "CreateProcessW", app, cmd, pa, ta, inherit, flags, env, dir, si, pi);
}
static WINAPI BOOL CreateProcessAsUserA(HANDLE h, const char *app, char *cmd, void *pa, void *ta, BOOL inherit, DWORD flags, void *env, const char *dir, void *si, void *pi) {
    return as_user(h, "CreateProcessA", app, cmd, pa, ta, inherit, flags, env, dir, si, pi);
}
// LogonUser checks a password: nothing here can, so it fails (the password is not looked at).
static WINAPI BOOL LogonUserW(const WCHAR *user, const WCHAR *domain, const WCHAR *password, DWORD type, DWORD provider, HANDLE *out) {
    (void)user; (void)domain; (void)password; (void)type; (void)provider;
    if (out) *out = NULL;
    vx_set_last_error(ERROR_NOT_SUPPORTED);
    return 0;
}
static WINAPI BOOL LogonUserA(const char *user, const char *domain, const char *password, DWORD type, DWORD provider, HANDLE *out) {
    (void)user; (void)domain; (void)password; (void)type; (void)provider;
    if (out) *out = NULL;
    vx_set_last_error(ERROR_NOT_SUPPORTED);
    return 0;
}

// ---- access checks ----
static WINAPI BOOL AccessCheck(const uint8_t *sd, HANDLE h, DWORD desired, const DWORD *map, uint8_t *privileges, DWORD *privileges_len,
                               DWORD *granted, BOOL *status) {
    sd_parts p;
    DWORD err;
    if (!sd_valid(sd, &p) || !p.owner || !p.group) { vx_set_last_error(ERROR_INVALID_SECURITY_DESCR); return 0; }
    if (desired & GENERIC_BITS) { vx_set_last_error(ERROR_GENERIC_NOT_MAPPED); return 0; }
    if (!map || !granted || !status || !privileges_len) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    token *t = token_get(h, &err);
    if (!t) { vx_set_last_error(err); return 0; }
    if (t->type != TOKEN_IMPERSONATION) { token_unref(t); vx_set_last_error(ERROR_NO_IMPERSONATION_TOKEN); return 0; }
    if (t->level < SECURITY_IDENTIFICATION) { token_unref(t); vx_set_last_error(ERROR_BAD_IMPERSONATION_LEVEL); return 0; }
    if (*privileges_len < 20 || !privileges) { *privileges_len = 20; token_unref(t); vx_set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    memset(privileges, 0, 8);                             // PRIVILEGE_SET: no privilege was used
    pthread_mutex_lock(&g_lock);
    DWORD g = desired & ACCESS_SYSTEM_SECURITY ? 0 : access_of(t, &p, desired, map);   // (SeSecurityPrivilege: not held)
    pthread_mutex_unlock(&g_lock);
    token_unref(t);
    *granted = g;
    *status = g != 0;
    if (!g) vx_set_last_error(desired & ACCESS_SYSTEM_SECURITY ? ERROR_PRIVILEGE_NOT_HELD : ERROR_ACCESS_DENIED);
    return 1;
}
static WINAPI BOOL CheckTokenMembership(HANDLE h, const uint8_t *sid, BOOL *member) {
    DWORD err;
    if (!sid_ok(sid) || !member) { vx_set_last_error(!member ? ERROR_INVALID_PARAMETER : ERROR_INVALID_SID); return 0; }
    token *t = token_get(h ? h : (HANDLE)(intptr_t)-6, &err);   // none: the thread's effective token
    if (!t) { vx_set_last_error(err); return 0; }
    int pseudo = (intptr_t)h >= -6 && (intptr_t)h <= -4;   // (the current tokens' pseudo-handles are taken as they are)
    if (h && !pseudo && t->type != TOKEN_IMPERSONATION) { token_unref(t); vx_set_last_error(ERROR_NO_IMPERSONATION_TOKEN); return 0; }
    pthread_mutex_lock(&g_lock);
    *member = token_has_sid(t, sid, 0, 0) && (!t->is_restricted || token_has_sid(t, sid, 0, 1));
    pthread_mutex_unlock(&g_lock);
    token_unref(t);
    return 1;
}

// ---- trustees, explicit access, descriptors built from them ----
static void trustee_build(uint8_t *tr, DWORD form, const void *name) {
    memset(tr, 0, 32);
    wr32(tr + 12, form);                                  // (MultipleTrusteeOperation: NO_MULTIPLE_TRUSTEE; TrusteeType: UNKNOWN)
    wrptr(tr + 24, name);
}
static WINAPI void BuildTrusteeWithSidW(uint8_t *tr, const uint8_t *sid) { trustee_build(tr, TRUSTEE_IS_SID, sid); }
static WINAPI void BuildTrusteeWithSidA(uint8_t *tr, const uint8_t *sid) { trustee_build(tr, TRUSTEE_IS_SID, sid); }
static WINAPI void BuildTrusteeWithNameW(uint8_t *tr, const WCHAR *name) { trustee_build(tr, TRUSTEE_IS_NAME, name); }
static WINAPI void BuildTrusteeWithNameA(uint8_t *tr, const char *name) { trustee_build(tr, TRUSTEE_IS_NAME, name); }
static WINAPI DWORD GetTrusteeFormW(const uint8_t *tr) { return rd32(tr + 12); }
static WINAPI DWORD GetTrusteeTypeW(const uint8_t *tr) { return tr ? rd32(tr + 16) : 0; }
static WINAPI void *GetTrusteeNameW(const uint8_t *tr) { void *v; memcpy(&v, tr + 24, 8); return v; }   // (the program's pointer as stored)
static void explicit_access(uint8_t *ea, const void *name, DWORD rights, DWORD mode, DWORD inherit) {
    wr32(ea, rights); wr32(ea + 4, mode); wr32(ea + 8, inherit); wr32(ea + 12, 0);
    trustee_build(ea + 16, TRUSTEE_IS_NAME, name);
}
static WINAPI void BuildExplicitAccessWithNameW(uint8_t *ea, const WCHAR *name, DWORD rights, DWORD mode, DWORD inherit) { explicit_access(ea, name, rights, mode, inherit); }
static WINAPI void BuildExplicitAccessWithNameA(uint8_t *ea, const char *name, DWORD rights, DWORD mode, DWORD inherit) { explicit_access(ea, name, rights, mode, inherit); }
static DWORD set_entries(DWORD n, const uint8_t *entries, const uint8_t *old, void **out, int wide) {
    if (!out) return ERROR_INVALID_PARAMETER;
    uint8_t *acl;
    DWORD err = entries_in_acl(n, entries, old, wide, &acl);
    if (err) return err;
    uint8_t *r = vx_heap_alloc(rd16(acl + 2));
    if (r) memcpy(r, acl, rd16(acl + 2));
    free(acl);
    if (!r) return ERROR_NOT_ENOUGH_MEMORY;
    *out = PROG(r);
    return 0;
}
static WINAPI DWORD SetEntriesInAclW(DWORD n, const uint8_t *entries, const uint8_t *old, void **out) { return set_entries(n, entries, old, out, 1); }
static WINAPI DWORD SetEntriesInAclA(DWORD n, const uint8_t *entries, const uint8_t *old, void **out) { return set_entries(n, entries, old, out, 0); }
static DWORD build_sd(const uint8_t *owner_tr, const uint8_t *group_tr, DWORD nacc, const uint8_t *acc, DWORD naud, const uint8_t *aud,
                      const uint8_t *old, DWORD *size, void **out, int wide) {
    if (!size || !out) return ERROR_INVALID_PARAMETER;
    sd_parts p = { 0 }, o;
    if (old && !sd_valid(old, &o)) return ERROR_INVALID_SECURITY_DESCR;
    if (old) { p = o; p.control &= (uint16_t)~SE_SELF_RELATIVE; }
    uint8_t owner[SID_MAX], group[SID_MAX], obj[16], inh[16], *dacl = NULL, *sacl = NULL;
    int objects;
    DWORD err = 0;
    if (owner_tr && !(err = trustee_sid(owner_tr, wide, owner, obj, inh, &objects))) p.owner = owner;
    if (!err && group_tr && !(err = trustee_sid(group_tr, wide, group, obj, inh, &objects))) p.group = group;
    if (!err && (nacc || (p.control & SE_DACL_PRESENT))) {
        if (!(p.control & SE_DACL_PRESENT) || p.dacl || nacc) err = entries_in_acl(nacc, acc, p.dacl, wide, &dacl);
        if (!err) { p.dacl = dacl; p.control |= SE_DACL_PRESENT; }
    }
    if (!err && (naud || (p.control & SE_SACL_PRESENT))) {
        err = entries_in_acl(naud, aud, p.sacl, wide, &sacl);
        if (!err) { p.sacl = sacl; p.control |= SE_SACL_PRESENT; }
    }
    if (!err) {
        uint8_t *r = sd_rel(&p, size, 1);
        if (r) *out = PROG(r); else err = ERROR_NOT_ENOUGH_MEMORY;
    }
    free(dacl); free(sacl);
    return err;
}
static WINAPI DWORD BuildSecurityDescriptorW(const uint8_t *owner, const uint8_t *group, DWORD nacc, const uint8_t *acc, DWORD naud, const uint8_t *aud,
                                             const uint8_t *old, DWORD *size, void **out) { return build_sd(owner, group, nacc, acc, naud, aud, old, size, out, 1); }
static WINAPI DWORD BuildSecurityDescriptorA(const uint8_t *owner, const uint8_t *group, DWORD nacc, const uint8_t *acc, DWORD naud, const uint8_t *aud,
                                             const uint8_t *old, DWORD *size, void **out) { return build_sd(owner, group, nacc, acc, naud, aud, old, size, out, 0); }

// ---- the security of handles and names ----
static WINAPI DWORD GetSecurityInfo(HANDLE h, int type, DWORD info, void **owner, void **group, void **dacl, void **sacl, void **sd) {
    uint8_t *r;
    DWORD err = object_get(h, type, info, &r, NULL);
    if (!err) parts_out(r, owner, group, dacl, sacl, sd);
    return err;
}
static WINAPI DWORD SetSecurityInfo(HANDLE h, int type, DWORD info, const uint8_t *owner, const uint8_t *group, const uint8_t *dacl, const uint8_t *sacl) {
    return object_set(h, type, info, owner, group, dacl, sacl);
}
static WINAPI DWORD GetNamedSecurityInfoW(const WCHAR *name, int type, DWORD info, void **owner, void **group, void **dacl, void **sacl, void **sd) {
    uint8_t *r;
    DWORD err = named_get(name, type, info, &r, NULL);
    if (!err) parts_out(r, owner, group, dacl, sacl, sd);
    return err;
}
static WINAPI DWORD GetNamedSecurityInfoA(const char *name, int type, DWORD info, void **owner, void **group, void **dacl, void **sacl, void **sd) {
    WCHAR *w = name ? vx_utf16_dup(name) : NULL;
    DWORD r = GetNamedSecurityInfoW(w, type, info, owner, group, dacl, sacl, sd);
    free(w);
    return r;
}
static WINAPI DWORD SetNamedSecurityInfoW(const WCHAR *name, int type, DWORD info, const uint8_t *owner, const uint8_t *group, const uint8_t *dacl, const uint8_t *sacl) {
    return named_set(name, type, info, owner, group, dacl, sacl);
}
static WINAPI DWORD SetNamedSecurityInfoA(const char *name, int type, DWORD info, const uint8_t *owner, const uint8_t *group, const uint8_t *dacl, const uint8_t *sacl) {
    WCHAR *w = name ? vx_utf16_dup(name) : NULL;
    DWORD r = SetNamedSecurityInfoW(w, type, info, owner, group, dacl, sacl);
    free(w);
    return r;
}
// The *ObjectSecurity / *FileSecurity forms: a self-relative descriptor copied into the caller's buffer.
static BOOL copy_out(DWORD err, uint8_t *r, DWORD n, uint8_t *sd, DWORD len, DWORD *need) {
    if (err) { vx_set_last_error(err); return 0; }
    if (need) *need = n;
    BOOL ok = sd && len >= n;
    if (ok) memcpy(sd, r, n); else vx_set_last_error(ERROR_INSUFFICIENT_BUFFER);
    free(r);
    return ok;
}
static WINAPI BOOL GetKernelObjectSecurity(HANDLE h, DWORD info, uint8_t *sd, DWORD len, DWORD *need) {
    uint8_t *r = NULL; DWORD n = 0;
    DWORD err = object_get(h, SE_KERNEL_OBJECT, info, &r, &n);
    return copy_out(err, r, n, sd, len, need);
}
static WINAPI BOOL SetKernelObjectSecurity(HANDLE h, DWORD info, const uint8_t *sd) {
    sd_parts p;
    if (!sd_valid(sd, &p)) { vx_set_last_error(ERROR_INVALID_SECURITY_DESCR); return 0; }
    if (info & DACL_SI) info |= p.control & SE_DACL_PROTECTED ? PROTECTED_DACL_SI : UNPROTECTED_DACL_SI;   // (the descriptor's own)
    DWORD err = object_set(h, SE_KERNEL_OBJECT, info, p.owner, p.group, p.dacl, p.sacl);
    if (err) { vx_set_last_error(err); return 0; }
    return 1;
}
static WINAPI BOOL GetFileSecurityW(const WCHAR *name, DWORD info, uint8_t *sd, DWORD len, DWORD *need) {
    uint8_t *r = NULL; DWORD n = 0;
    DWORD err = named_get(name, SE_FILE_OBJECT, info, &r, &n);
    return copy_out(err, r, n, sd, len, need);
}
static WINAPI BOOL GetFileSecurityA(const char *name, DWORD info, uint8_t *sd, DWORD len, DWORD *need) {
    WCHAR *w = name ? vx_utf16_dup(name) : NULL;
    BOOL r = GetFileSecurityW(w, info, sd, len, need);
    free(w);
    return r;
}

// ---- the registry's per-user cache: there is none to turn off ----
static WINAPI LONG RegDisablePredefinedCache(void) { return 0; }
static WINAPI LONG RegDisablePredefinedCacheEx(void) { return 0; }

static const vx_export security_exports[] = {
    VX_EXPORT(AllocateAndInitializeSid), VX_EXPORT(FreeSid), VX_EXPORT(GetLengthSid), VX_EXPORT(IsValidSid), VX_EXPORT(GetSidLengthRequired),
    VX_EXPORT(InitializeSid), VX_EXPORT(GetSidIdentifierAuthority), VX_EXPORT(GetSidSubAuthority), VX_EXPORT(GetSidSubAuthorityCount),
    VX_EXPORT(CopySid), VX_EXPORT(EqualSid), VX_EXPORT(EqualPrefixSid), VX_EXPORT(CreateWellKnownSid), VX_EXPORT(IsWellKnownSid),
    VX_EXPORT(ConvertSidToStringSidW), VX_EXPORT(ConvertSidToStringSidA), VX_EXPORT(ConvertStringSidToSidW), VX_EXPORT(ConvertStringSidToSidA),
    VX_EXPORT(LookupAccountSidW), VX_EXPORT(LookupAccountSidA), VX_EXPORT(LookupAccountNameW), VX_EXPORT(LookupAccountNameA),
    { "LookupAccountSidLocalW", (void *)LookupAccountSidW }, { "LookupAccountNameLocalW", (void *)LookupAccountNameW },
    VX_EXPORT(InitializeAcl), VX_EXPORT(IsValidAcl), VX_EXPORT(GetAce), VX_EXPORT(AddAce), VX_EXPORT(DeleteAce), VX_EXPORT(FindFirstFreeAce),
    VX_EXPORT(GetAclInformation), VX_EXPORT(SetAclInformation), VX_EXPORT(AddAccessAllowedAce), VX_EXPORT(AddAccessAllowedAceEx),
    VX_EXPORT(AddAccessDeniedAce), VX_EXPORT(AddAccessDeniedAceEx), VX_EXPORT(AddAuditAccessAce), VX_EXPORT(AddAuditAccessAceEx),
    VX_EXPORT(AddMandatoryAce), VX_EXPORT(AddAccessAllowedObjectAce), VX_EXPORT(AddAccessDeniedObjectAce), VX_EXPORT(MapGenericMask),
    VX_EXPORT(InitializeSecurityDescriptor), VX_EXPORT(IsValidSecurityDescriptor), VX_EXPORT(GetSecurityDescriptorLength),
    VX_EXPORT(GetSecurityDescriptorControl), VX_EXPORT(SetSecurityDescriptorControl), VX_EXPORT(GetSecurityDescriptorOwner),
    VX_EXPORT(GetSecurityDescriptorGroup), VX_EXPORT(GetSecurityDescriptorDacl), VX_EXPORT(GetSecurityDescriptorSacl),
    VX_EXPORT(SetSecurityDescriptorOwner), VX_EXPORT(SetSecurityDescriptorGroup), VX_EXPORT(SetSecurityDescriptorDacl),
    VX_EXPORT(SetSecurityDescriptorSacl), VX_EXPORT(MakeSelfRelativeSD), VX_EXPORT(MakeAbsoluteSD),
    VX_EXPORT(ConvertStringSecurityDescriptorToSecurityDescriptorW), VX_EXPORT(ConvertStringSecurityDescriptorToSecurityDescriptorA),
    VX_EXPORT(ConvertSecurityDescriptorToStringSecurityDescriptorW), VX_EXPORT(ConvertSecurityDescriptorToStringSecurityDescriptorA),
    VX_EXPORT(OpenProcessToken), VX_EXPORT(OpenThreadToken), VX_EXPORT(GetTokenInformation), VX_EXPORT(SetTokenInformation),
    VX_EXPORT(DuplicateTokenEx), VX_EXPORT(DuplicateToken), VX_EXPORT(CreateRestrictedToken), VX_EXPORT(IsTokenRestricted),
    VX_EXPORT(AllocateLocallyUniqueId), VX_EXPORT(LookupPrivilegeValueW), VX_EXPORT(LookupPrivilegeValueA), VX_EXPORT(LookupPrivilegeNameW),
    VX_EXPORT(LookupPrivilegeNameA), VX_EXPORT(AdjustTokenPrivileges), VX_EXPORT(PrivilegeCheck), VX_EXPORT(SetThreadToken), VX_EXPORT(RevertToSelf), VX_EXPORT(ImpersonateSelf),
    VX_EXPORT(ImpersonateLoggedOnUser), VX_EXPORT(ImpersonateAnonymousToken), VX_EXPORT(ImpersonateNamedPipeClient),
    VX_EXPORT(CreateProcessAsUserW), VX_EXPORT(CreateProcessAsUserA), VX_EXPORT(LogonUserW), VX_EXPORT(LogonUserA),
    VX_EXPORT(AccessCheck), VX_EXPORT(CheckTokenMembership),
    VX_EXPORT(BuildTrusteeWithSidW), VX_EXPORT(BuildTrusteeWithSidA), VX_EXPORT(BuildTrusteeWithNameW), VX_EXPORT(BuildTrusteeWithNameA),
    VX_EXPORT(GetTrusteeFormW), { "GetTrusteeFormA", (void *)GetTrusteeFormW }, VX_EXPORT(GetTrusteeTypeW), { "GetTrusteeTypeA", (void *)GetTrusteeTypeW },
    VX_EXPORT(GetTrusteeNameW), { "GetTrusteeNameA", (void *)GetTrusteeNameW },
    VX_EXPORT(BuildExplicitAccessWithNameW), VX_EXPORT(BuildExplicitAccessWithNameA), VX_EXPORT(SetEntriesInAclW), VX_EXPORT(SetEntriesInAclA),
    VX_EXPORT(BuildSecurityDescriptorW), VX_EXPORT(BuildSecurityDescriptorA),
    VX_EXPORT(GetSecurityInfo), VX_EXPORT(SetSecurityInfo), VX_EXPORT(GetNamedSecurityInfoW), VX_EXPORT(GetNamedSecurityInfoA),
    VX_EXPORT(SetNamedSecurityInfoW), VX_EXPORT(SetNamedSecurityInfoA), VX_EXPORT(GetKernelObjectSecurity), VX_EXPORT(SetKernelObjectSecurity),
    VX_EXPORT(GetFileSecurityW), VX_EXPORT(GetFileSecurityA), VX_EXPORT(RegDisablePredefinedCache), VX_EXPORT(RegDisablePredefinedCacheEx),
};
void vx_security_init(void) {
    vx_register_module("advapi32.dll", security_exports, COUNT(security_exports));
    vx_register_module("kernelbase.dll", security_exports, COUNT(security_exports));
}

#ifdef __aarch64__
#include "thunks/security.c.inc"
#endif
