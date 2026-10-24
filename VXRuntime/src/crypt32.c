// crypt32.dll and wintrust.dll: certificates read from their DER (RFC 5280), memory and collection stores of them, their
// names as text, key usages, hashes, and signatures checked by macOS's Security framework. DPAPI is in dpapi.c.
//
// Nothing here answers "trusted" or "valid" for a check it did not do:
// - CryptVerifyCertificateSignatureEx succeeds only when SecKeyVerifySignature verified the signature with the
//   issuer's public key. What macOS cannot check (MD5, DSA, unsigned hashes, chains as issuers) is NTE_BAD_ALGID or
//   E_INVALIDARG, never success.
// - Chains are built and judged by SecTrust (macOS's roots, or an engine's exclusive roots); their error bits come only
//   from its evaluations, and a chain context not made here is never reported trusted.
// - The system stores (ROOT, CA, MY, ...) open empty: no root is trusted through them and there are no private keys
//   (CryptAcquireCertificatePrivateKey and CertFindChainInStore find none).
// - WinVerifyTrust never returns 0 for a verification: Authenticode is not checked here, so a file with a signature is
//   TRUST_E_SUBJECT_NOT_TRUSTED (one without is TRUST_E_NOSIGNATURE); there are no catalogs (CryptCATAdmin*).
// These tables are registered before client.c's and extras.c's stand-ins for the same functions (loader.c): they
// replace them.
#include <CommonCrypto/CommonDigest.h>
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wctype.h>
#include "vx.h"

#ifdef __aarch64__
#define PROG_PTR(p) ((void *)(uintptr_t)vx_unlow((uint64_t)(uintptr_t)(p)))   // a pointer stored for the program
#else
#define PROG_PTR(p) ((void *)(p))
#endif

// ---- errors and constants (wincrypt.h, wintrust.h, mscat.h) --------------------------------------------------------
#define ERROR_INVALID_HANDLE 6
#define ERROR_NOT_ENOUGH_MEMORY 8
#define ERROR_NOT_SUPPORTED 50
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_INVALID_PARAMETER 87
#define ERROR_MORE_DATA 234
#define ERROR_NOT_FOUND 1168
#define E_INVALIDARG 0x80070057u
#define E_ACCESSDENIED 0x80070005u
#define CRYPT_E_NOT_FOUND 0x80092004u
#define CRYPT_E_EXISTS 0x80092005u
#define CRYPT_E_NO_KEY_PROPERTY 0x8009200Bu
#define CRYPT_E_PENDING_CLOSE 0x8009200Fu
#define CRYPT_E_ASN1_CORRUPT 0x80093103u
#define CRYPT_E_ASN1_EOD 0x80093102u
#define CRYPT_E_ASN1_BADTAG 0x8009310Bu
#define NTE_BAD_SIGNATURE 0x80090006u
#define NTE_BAD_ALGID 0x80090008u
#define NTE_BAD_PUBLIC_KEY 0x80090015u
#define TRUST_E_PROVIDER_UNKNOWN 0x800B0001u
#define TRUST_E_SUBJECT_FORM_UNKNOWN 0x800B0003u
#define TRUST_E_SUBJECT_NOT_TRUSTED 0x800B0004u
#define TRUST_E_NOSIGNATURE 0x800B0100u

#define X509_ASN_ENCODING 1
#define CERT_NAME_ISSUER_FLAG 0x1
#define CERT_NAME_SEARCH_ALL_NAMES_FLAG 0x2
#define CERT_NAME_DISABLE_IE4_UTF8_FLAG 0x10000
#define CERT_SIMPLE_NAME_STR 1
#define CERT_OID_NAME_STR 2
#define CERT_X500_NAME_STR 3
#define CERT_NAME_STR_SEMICOLON_FLAG 0x40000000u
#define CERT_NAME_STR_NO_PLUS_FLAG 0x20000000u
#define CERT_NAME_STR_NO_QUOTING_FLAG 0x10000000u
#define CERT_NAME_STR_CRLF_FLAG 0x08000000u
#define CERT_NAME_STR_REVERSE_FLAG 0x02000000u
#define CERT_FIND_EXT_ONLY_ENHKEY_USAGE_FLAG 0x2
#define CERT_FIND_PROP_ONLY_ENHKEY_USAGE_FLAG 0x4
#define CERT_FIND_OPTIONAL_ENHKEY_USAGE_FLAG 0x1
#define CERT_CLOSE_STORE_CHECK_FLAG 0x2
#define CERT_STORE_READONLY_FLAG 0x8000
#define CERT_STORE_DELETE_FLAG 0x10
#define CERT_PHYSICAL_STORE_ADD_ENABLE_FLAG 0x1
enum { ADD_NEW = 1, ADD_USE_EXISTING, ADD_REPLACE_EXISTING, ADD_ALWAYS, ADD_REPLACE_EXISTING_INHERIT_PROPERTIES, ADD_NEWER,
       ADD_NEWER_INHERIT_PROPERTIES };
enum { CTRL_RESYNC = 1, CTRL_NOTIFY_CHANGE, CTRL_COMMIT, CTRL_AUTO_RESYNC, CTRL_CANCEL_NOTIFY };
enum { PROP_ACCESS_STATE = 14, PROP_SHA1 = 3, PROP_MD5 = 4, PROP_KEY_IDENTIFIER = 20, PROP_PUBKEY_MD5 = 25, PROP_SUBJECT_MD5 = 29,
       PROP_SHA256 = 107 };
#define CERT_ACCESS_STATE_SYSTEM_STORE_FLAG 0x2

// ---- the structures programs see (x64 layouts of wincrypt.h) ---------------------------------------------------------
typedef struct { DWORD cbData; uint8_t *pbData; } blob_t;                     // CRYPT_*_BLOB, CERT_NAME_BLOB
typedef struct { char *pszObjId; blob_t Parameters; } alg_id;                 // CRYPT_ALGORITHM_IDENTIFIER
typedef struct { DWORD cbData; uint8_t *pbData; DWORD cUnusedBits; } bit_blob; // CRYPT_BIT_BLOB
typedef struct { alg_id Algorithm; bit_blob PublicKey; } pubkey_info;         // CERT_PUBLIC_KEY_INFO
typedef struct { char *pszObjId; BOOL fCritical; blob_t Value; } cert_ext;     // CERT_EXTENSION
typedef struct { DWORD lo, hi; } filetime;
typedef struct {                                                              // CERT_INFO
    DWORD dwVersion; blob_t SerialNumber; alg_id SignatureAlgorithm; blob_t Issuer; filetime NotBefore, NotAfter;
    blob_t Subject; pubkey_info SubjectPublicKeyInfo; bit_blob IssuerUniqueId, SubjectUniqueId; DWORD cExtension; cert_ext *rgExtension;
} cert_info;
typedef struct { DWORD dwCertEncodingType; uint8_t *pbCertEncoded; DWORD cbCertEncoded; cert_info *pCertInfo; void *hCertStore; } cert_context;
_Static_assert(sizeof(alg_id) == 24 && sizeof(bit_blob) == 24 && sizeof(pubkey_info) == 48 && sizeof(cert_ext) == 32, "wincrypt layouts");
_Static_assert(__builtin_offsetof(cert_info, NotBefore) == 64 && __builtin_offsetof(cert_info, SubjectPublicKeyInfo) == 96 &&
               __builtin_offsetof(cert_info, rgExtension) == 200 && sizeof(cert_info) == 208, "CERT_INFO");
_Static_assert(__builtin_offsetof(cert_context, hCertStore) == 32 && sizeof(cert_context) == 40, "CERT_CONTEXT");

static void *host(const void *p) { return VX_LOW(p); }   // a pointer the program stored in a structure

// ---- DER (X.690) reading ------------------------------------------------------------------------------------------
typedef struct { const uint8_t *p; size_t n; } span;
typedef struct { uint8_t tag; span whole, body; } tlv;

// One element (definite length, single-byte tag) from the front of *cur; 0 or a CRYPT_E_ASN1 error.
static DWORD der_next(span *cur, tlv *out) {
    if (cur->n < 2) return CRYPT_E_ASN1_EOD;
    uint8_t tag = cur->p[0], l = cur->p[1];
    if ((tag & 0x1f) == 0x1f) return CRYPT_E_ASN1_BADTAG;
    size_t len = l, hdr = 2;
    if (l & 0x80) {
        size_t k = l & 0x7f;
        if (!k || k > 4) return CRYPT_E_ASN1_CORRUPT;              // indefinite lengths are not DER
        if (cur->n < 2 + k) return CRYPT_E_ASN1_EOD;
        len = 0;
        for (size_t i = 0; i < k; i++) len = len << 8 | cur->p[2 + i];
        hdr += k;
    }
    if (len > cur->n - hdr) return CRYPT_E_ASN1_EOD;
    out->tag = tag;
    out->whole = (span){ cur->p, hdr + len };
    out->body = (span){ cur->p + hdr, len };
    cur->p += hdr + len; cur->n -= hdr + len;
    return 0;
}
static DWORD der_expect(span *cur, uint8_t tag, tlv *out) {
    DWORD e = der_next(cur, out);
    return e ? e : out->tag == tag ? 0 : CRYPT_E_ASN1_BADTAG;
}
#define TRY(x) do { DWORD e_ = (x); if (e_) return e_; } while (0)

// An OBJECT IDENTIFIER's content as dotted text ("2.5.4.3"); 0 or an error.
static DWORD oid_text(span o, char *out, size_t cap) {
    size_t k = 0;
    int first = 1;
    if (!o.n || cap < 2) return CRYPT_E_ASN1_CORRUPT;
    for (size_t i = 0; i < o.n;) {
        uint64_t v = 0;
        if (o.p[i] == 0x80) return CRYPT_E_ASN1_CORRUPT;            // (a sub-identifier never starts with a zero group)
        do {
            if (i >= o.n || v >> 57) return CRYPT_E_ASN1_CORRUPT;
            v = v << 7 | (o.p[i] & 0x7f);
        } while (o.p[i++] & 0x80);
        char part[48];
        int n = first ? snprintf(part, sizeof part, "%u.%llu", v < 40 ? 0 : v < 80 ? 1 : 2,
                                 (unsigned long long)(v < 40 ? v : v < 80 ? v - 40 : v - 80))
                      : snprintf(part, sizeof part, ".%llu", (unsigned long long)v);
        if (k + (size_t)n + 1 > cap) return CRYPT_E_ASN1_CORRUPT;
        memcpy(out + k, part, (size_t)n);
        k += (size_t)n;
        first = 0;
    }
    out[k] = 0;
    return 0;
}

// UTCTime / GeneralizedTime (UTC, "Z"; seconds optional) as a FILETIME value.
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {   // proleptic Gregorian, days since 1970-01-01
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400), doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}
static DWORD der_time(const tlv *t, uint64_t *ft) {
    const uint8_t *s = t->body.p;
    size_t n = t->body.n, yd = t->tag == 0x17 ? 2 : t->tag == 0x18 ? 4 : 0;
    if (!yd) return CRYPT_E_ASN1_BADTAG;
    if (n < yd + 8 + 1 || s[n - 1] != 'Z') return CRYPT_E_ASN1_CORRUPT;
    unsigned v[7] = { 0 };
    size_t digits = n - 1, pos = 0;
    int fields = (int)((digits - yd) / 2) + 1;                      // year, month, day, hour, minute [, second]
    if ((digits - yd) % 2 || fields < 5 || fields > 6) return CRYPT_E_ASN1_CORRUPT;
    for (int f = 0; f < fields; f++) {
        size_t w = f ? 2 : yd;
        for (size_t i = 0; i < w; i++, pos++) {
            if (s[pos] < '0' || s[pos] > '9') return CRYPT_E_ASN1_CORRUPT;
            v[f] = v[f] * 10 + (unsigned)(s[pos] - '0');
        }
    }
    if (yd == 2) v[0] += v[0] >= 50 ? 1900 : 2000;                  // RFC 5280 4.1.2.5.1
    if (v[1] < 1 || v[1] > 12 || v[2] < 1 || v[2] > 31 || v[3] > 23 || v[4] > 59 || v[5] > 60) return CRYPT_E_ASN1_CORRUPT;
    int64_t secs = days_from_civil(v[0], v[1], v[2]) * 86400 + v[3] * 3600 + v[4] * 60 + v[5];
    *ft = (uint64_t)(secs + 11644473600LL) * 10000000ull;
    return 0;
}

// ---- certificates -----------------------------------------------------------------------------------------------
typedef struct store store;
typedef struct cert cert;
typedef struct { const char *oid; int critical; span value; } ext;
#define CERT_MAGIC 0x56584354u                                      // 'VXCT'
struct cert {
    cert_context ctx;          // first: what the program holds
    cert_info info;
    uint32_t magic;
    int refs;                  // the program's references (each also holds one on the store)
    int listed;                // in its store's list
    store *owner;
    cert *next;
    uint8_t *der;              // the encoded certificate (ctx.pbCertEncoded)
    DWORD der_n;
    span tbs, sig_alg, sig_bits, tbs_alg, issuer, subject, spki, key_alg, key_params, key_bits, serial;
    uint64_t not_before, not_after;
    ext *exts; int n_ext;
    char *arena; size_t arena_used, arena_cap;
};

static char *arena_put(cert *c, const char *s) {
    size_t n = strlen(s) + 1;
    if (c->arena_used + n > c->arena_cap) return NULL;
    char *d = c->arena + c->arena_used;
    memcpy(d, s, n);
    c->arena_used += n;
    return d;
}

static void cert_free_memory(cert *c) {
    c->magic = 0;
    free(c->der); free(c->arena); free(c->info.rgExtension ? host(c->info.rgExtension) : NULL); free(c->exts);
    free(c);
}

static DWORD parse_alg(span *cur, tlv *whole, span *oid, span *params) {
    tlv a, o;
    TRY(der_expect(cur, 0x30, &a));
    span b = a.body;
    TRY(der_expect(&b, 0x06, &o));
    *oid = o.body;
    *params = (span){ b.p, b.n };                                   // the encoded parameters, if any (often NULL)
    if (whole) *whole = a;
    return 0;
}

static DWORD fill_alg(cert *c, span oid, span params, alg_id *out) {
    char text[256];
    TRY(oid_text(oid, text, sizeof text));
    char *s = arena_put(c, text);
    if (!s) return CRYPT_E_ASN1_CORRUPT;
    out->pszObjId = PROG_PTR(s);
    out->Parameters.cbData = (DWORD)params.n;
    out->Parameters.pbData = params.n ? PROG_PTR(params.p) : NULL;
    return 0;
}

// Parses the DER of a certificate (copied into c->der) and fills c->info.
static DWORD parse_cert(cert *c) {
    span all = { c->der, c->der_n }, body, t;
    tlv crt, tbs, sigalg, sig, x, alg, iss, val, sub, spki;
    TRY(der_expect(&all, 0x30, &crt));
    c->der_n = (DWORD)crt.whole.n;                                  // (trailing bytes after the certificate are ignored)
    body = crt.body;
    TRY(der_expect(&body, 0x30, &tbs));
    span soid, sparams;
    TRY(parse_alg(&body, &sigalg, &soid, &sparams));
    TRY(der_expect(&body, 0x03, &sig));
    if (body.n || !sig.body.n || sig.body.p[0] > 7) return CRYPT_E_ASN1_CORRUPT;
    c->tbs = tbs.whole; c->sig_alg = sigalg.whole; c->sig_bits = (span){ sig.body.p + 1, sig.body.n - 1 };
    c->arena_cap = c->der_n * 6 + 256;
    if (!(c->arena = vx_heap_alloc(c->arena_cap))) return ERROR_NOT_ENOUGH_MEMORY;
    t = tbs.body;
    TRY(der_next(&t, &x));
    c->info.dwVersion = 0;
    if (x.tag == 0xa0) {                                            // [0] EXPLICIT Version DEFAULT v1
        span v = x.body; tlv vi;
        TRY(der_expect(&v, 0x02, &vi));
        if (vi.body.n != 1 || vi.body.p[0] > 2) return CRYPT_E_ASN1_CORRUPT;
        c->info.dwVersion = vi.body.p[0];
        TRY(der_next(&t, &x));
    }
    if (x.tag != 0x02 || !x.body.n) return CRYPT_E_ASN1_BADTAG;
    c->serial = x.body;
    if (c->arena_used + x.body.n > c->arena_cap) return CRYPT_E_ASN1_CORRUPT;
    uint8_t *rev = (uint8_t *)c->arena + c->arena_used;              // CRYPT_INTEGER_BLOB: least significant byte first
    for (size_t i = 0; i < x.body.n; i++) rev[i] = x.body.p[x.body.n - 1 - i];
    c->arena_used += x.body.n;
    c->info.SerialNumber = (blob_t){ (DWORD)x.body.n, PROG_PTR(rev) };
    span aoid, aparams;
    TRY(parse_alg(&t, &alg, &aoid, &aparams));
    c->tbs_alg = alg.whole;
    TRY(fill_alg(c, soid, sparams, &c->info.SignatureAlgorithm));   // (the outer one; the two must be equal: checked
    TRY(der_expect(&t, 0x30, &iss));                                //  where the signature is verified)
    c->issuer = iss.whole;
    c->info.Issuer = (blob_t){ (DWORD)iss.whole.n, PROG_PTR(iss.whole.p) };
    TRY(der_expect(&t, 0x30, &val));
    span vb = val.body; tlv t0, t1;
    TRY(der_next(&vb, &t0)); TRY(der_next(&vb, &t1));
    TRY(der_time(&t0, &c->not_before)); TRY(der_time(&t1, &c->not_after));
    c->info.NotBefore = (filetime){ (DWORD)c->not_before, (DWORD)(c->not_before >> 32) };
    c->info.NotAfter = (filetime){ (DWORD)c->not_after, (DWORD)(c->not_after >> 32) };
    TRY(der_expect(&t, 0x30, &sub));
    c->subject = sub.whole;
    c->info.Subject = (blob_t){ (DWORD)sub.whole.n, PROG_PTR(sub.whole.p) };
    TRY(der_expect(&t, 0x30, &spki));
    c->spki = spki.whole;
    span sb = spki.body; tlv kbits;
    TRY(parse_alg(&sb, NULL, &c->key_alg, &c->key_params));
    TRY(der_expect(&sb, 0x03, &kbits));
    if (!kbits.body.n || kbits.body.p[0] > 7) return CRYPT_E_ASN1_CORRUPT;
    c->key_bits = (span){ kbits.body.p + 1, kbits.body.n - 1 };
    TRY(fill_alg(c, c->key_alg, c->key_params, &c->info.SubjectPublicKeyInfo.Algorithm));
    c->info.SubjectPublicKeyInfo.PublicKey = (bit_blob){ (DWORD)c->key_bits.n, c->key_bits.n ? PROG_PTR(c->key_bits.p) : NULL, kbits.body.p[0] };
    while (t.n) {
        TRY(der_next(&t, &x));
        if (x.tag == 0x81 || x.tag == 0x82) {                       // [1] issuerUniqueID, [2] subjectUniqueID
            if (!x.body.n || x.body.p[0] > 7) return CRYPT_E_ASN1_CORRUPT;
            bit_blob *b = x.tag == 0x81 ? &c->info.IssuerUniqueId : &c->info.SubjectUniqueId;
            *b = (bit_blob){ (DWORD)(x.body.n - 1), x.body.n > 1 ? PROG_PTR(x.body.p + 1) : NULL, x.body.p[0] };
        } else if (x.tag == 0xa3 && !c->exts) {                     // [3] EXPLICIT Extensions (once)
            span eb = x.body; tlv seq, e;
            TRY(der_expect(&eb, 0x30, &seq));
            int count = 0;
            for (span s = seq.body; s.n; count++) TRY(der_next(&s, &e));
            c->exts = calloc((size_t)count + 1, sizeof *c->exts);
            cert_ext *pe = vx_heap_zalloc(((size_t)count + 1) * sizeof *pe);
            if (!c->exts || !pe) { free(pe); return ERROR_NOT_ENOUGH_MEMORY; }
            c->info.rgExtension = PROG_PTR(pe);
            span s = seq.body;
            for (int i = 0; i < count; i++) {
                tlv o, f, v;
                TRY(der_expect(&s, 0x30, &e));
                span es = e.body;
                TRY(der_expect(&es, 0x06, &o));
                int critical = 0;
                if (es.n && es.p[0] == 0x01) {                      // critical BOOLEAN DEFAULT FALSE
                    TRY(der_next(&es, &f));
                    if (f.body.n != 1) return CRYPT_E_ASN1_CORRUPT;
                    critical = f.body.p[0] != 0;
                }
                TRY(der_expect(&es, 0x04, &v));
                char text[256];
                TRY(oid_text(o.body, text, sizeof text));
                char *name = arena_put(c, text);
                if (!name) return CRYPT_E_ASN1_CORRUPT;
                c->exts[i] = (ext){ name, critical, v.body };
                pe[i] = (cert_ext){ PROG_PTR(name), critical, { (DWORD)v.body.n, v.body.n ? PROG_PTR(v.body.p) : NULL } };
            }
            c->n_ext = count;
            c->info.cExtension = (DWORD)count;
        } else return CRYPT_E_ASN1_BADTAG;
    }
    return 0;
}

static const ext *find_ext(const cert *c, const char *oid) {
    for (int i = 0; i < c->n_ext; i++) if (!strcmp(c->exts[i].oid, oid)) return &c->exts[i];
    return NULL;
}

// ---- stores -------------------------------------------------------------------------------------------------------
// A store lives while it has references: its open handles, the collections it is a sibling of, and each certificate of
// it the program holds. Then its certificates (none held any more) go with it.
enum { STORE_MEMORY = 1, STORE_COLLECTION, STORE_SYSTEM };
#define STORE_MAGIC 0x56585354u                                     // 'VXST'
typedef struct { store *s; DWORD flags, priority; } sibling;
struct store {
    uint32_t magic;
    int kind, readonly, dirty;
    int refs;
    cert *first, *last;
    sibling *sib; int nsib;
};
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static store *store_of(const void *h) {
    store *s = (store *)h;
    return s && s->magic == STORE_MAGIC ? s : NULL;
}
static cert *cert_of(const void *ctx) {
    cert *c = (cert *)ctx;
    return c && c->magic == CERT_MAGIC ? c : NULL;
}

static void store_release(store *s) {
    if (--s->refs > 0) return;
    for (cert *c = s->first, *n; c; c = n) { n = c->next; cert_free_memory(c); }
    for (int i = 0; i < s->nsib; i++) store_release(s->sib[i].s);
    free(s->sib);
    s->magic = 0;
    free(s);
}
static void cert_addref(cert *c) { if (c->refs++ == 0) c->owner->refs++; }
static void cert_release(cert *c) {
    if (--c->refs > 0) return;
    store *s = c->owner;
    if (!c->listed) cert_free_memory(c);
    store_release(s);
}
static void unlist(cert *c) {
    store *s = c->owner;
    cert **pp = &s->first, *prev = NULL;
    while (*pp && *pp != c) { prev = *pp; pp = &(*pp)->next; }
    if (!*pp) return;
    *pp = c->next;
    if (s->last == c) s->last = prev;
    c->listed = 0;
    c->next = NULL;
    s->dirty = 1;
    if (!c->refs) cert_free_memory(c);
}

static store *store_new(int kind) {
    store *s = vx_heap_zalloc(sizeof *s);
    if (!s) return NULL;
    s->magic = STORE_MAGIC; s->kind = kind; s->refs = 1;
    return s;
}

// A new certificate of store s from its encoding (copied), listed last; 0 or an error.
static DWORD cert_new(store *s, const uint8_t *data, DWORD n, cert **out) {
    cert *c = vx_heap_zalloc(sizeof *c);
    uint8_t *der = vx_heap_alloc(n);
    if (!c || !der) { free(c); free(der); return ERROR_NOT_ENOUGH_MEMORY; }
    memcpy(der, data, n);
    c->der = der; c->der_n = n;
    DWORD e = parse_cert(c);
    if (e) { cert_free_memory(c); return e; }
    c->magic = CERT_MAGIC;
    c->owner = s;
    c->listed = 1;
    c->ctx = (cert_context){ X509_ASN_ENCODING, PROG_PTR(c->der), c->der_n, PROG_PTR(&c->info), PROG_PTR(s) };
    if (s->last) s->last->next = c; else s->first = c;
    s->last = c;
    s->dirty = 1;
    *out = c;
    return 0;
}

// Every certificate a store shows, in order: its own, or its siblings' by priority.
typedef struct { cert **v; int n, cap; } cert_list;
static int list_push(cert_list *l, cert *c) {
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 16;
        cert **v = realloc(l->v, (size_t)cap * sizeof *v);
        if (!v) return -1;
        l->v = v; l->cap = cap;
    }
    l->v[l->n++] = c;
    return 0;
}
static int flatten(store *s, cert_list *l, int depth) {
    if (depth > 16) return 0;
    if (s->kind != STORE_COLLECTION) {
        for (cert *c = s->first; c; c = c->next) if (list_push(l, c)) return -1;
        return 0;
    }
    for (int i = 0; i < s->nsib; i++) if (flatten(s->sib[i].s, l, depth + 1)) return -1;
    return 0;
}
static int contains_store(store *s, store *t, int depth) {
    if (s == t) return 1;
    if (depth > 16) return 1;
    for (int i = 0; i < s->nsib; i++) if (contains_store(s->sib[i].s, t, depth + 1)) return 1;
    return 0;
}

// Memory and collection stores hold what the program adds. The system stores (ROOT, CA, MY, ... in any location) open
// empty: nothing of this Mac is shown as a Windows root or a personal certificate, and nothing added is saved.
static WINAPI void *CertOpenStore(const char *provider, DWORD encoding, uintptr_t prov, DWORD flags, const void *para) {
    (void)encoding; (void)prov; (void)para;
    uintptr_t id = (uintptr_t)provider;
    int kind;
    if (id <= 0xffff) {
        kind = id == 2 ? STORE_MEMORY : id == 11 ? STORE_COLLECTION : 0;
        if (id == 4 || (id >= 9 && id <= 10) || (id >= 12 && id <= 14)) kind = STORE_SYSTEM;   // registry, system, physical
    } else {
        const char *p = host(provider);
        kind = !strcasecmp(p, "Memory") ? STORE_MEMORY : !strcasecmp(p, "Collection") ? STORE_COLLECTION
             : !strcasecmp(p, "System") || !strcasecmp(p, "SystemRegistry") || !strcasecmp(p, "Physical") ? STORE_SYSTEM : 0;
    }
    if (!kind) {                                                     // files, PKCS #7, serialized, LDAP...: not read
        static int logged;
        if (!logged++) vx_log("crypt32: CertOpenStore(%s): opened as an empty store", id <= 0xffff ? "provider number" : (const char *)host(provider));
        kind = STORE_MEMORY;
    }
    if (flags & CERT_STORE_DELETE_FLAG) { vx_set_last_error(0); return NULL; }   // (deleting a store: nothing kept here)
    pthread_mutex_lock(&g_lock);
    store *s = store_new(kind);
    if (s) s->readonly = !!(flags & CERT_STORE_READONLY_FLAG);
    pthread_mutex_unlock(&g_lock);
    if (!s) vx_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
    return s ? PROG_PTR(s) : NULL;
}
static WINAPI void *CertOpenSystemStoreW(uintptr_t prov, const WCHAR *name) { (void)name; return CertOpenStore((const char *)10, 0, prov, 0, NULL); }
static WINAPI void *CertOpenSystemStoreA(uintptr_t prov, const char *name) { (void)name; return CertOpenStore((const char *)9, 0, prov, 0, NULL); }
static WINAPI void *CertDuplicateStore(void *h) {
    pthread_mutex_lock(&g_lock);
    store *s = store_of(h);
    if (s) s->refs++;
    pthread_mutex_unlock(&g_lock);
    if (!s) vx_set_last_error(ERROR_INVALID_HANDLE);
    return s ? h : NULL;
}

static int store_has_held_certs(store *s) {
    for (cert *c = s->first; c; c = c->next) if (c->refs) return 1;
    return 0;
}
static WINAPI BOOL CertCloseStore(void *h, DWORD flags) {
    if (!h) return 1;
    pthread_mutex_lock(&g_lock);
    store *s = store_of(h);
    int pending = s && (flags & CERT_CLOSE_STORE_CHECK_FLAG) && store_has_held_certs(s);
    if (s) store_release(s);                                         // (a forced close keeps held certificates valid too)
    pthread_mutex_unlock(&g_lock);
    if (!s) { vx_set_last_error(ERROR_INVALID_HANDLE); return 0; }
    if (pending) { vx_set_last_error(CRYPT_E_PENDING_CLOSE); return 0; }   // closed all the same, as documented
    return 1;
}

static WINAPI BOOL CertAddStoreToCollection(void *collection, void *sibling_store, DWORD flags, DWORD priority) {
    pthread_mutex_lock(&g_lock);
    store *c = store_of(collection), *s = store_of(sibling_store);
    DWORD err = 0;
    if (!c || c->kind != STORE_COLLECTION || (sibling_store && !s)) err = E_INVALIDARG;
    else if (s && contains_store(s, c, 0)) err = E_INVALIDARG;     // (a collection inside itself)
    else if (s) {
        sibling *v = realloc(c->sib, (size_t)(c->nsib + 1) * sizeof *v);
        if (!v) err = ERROR_NOT_ENOUGH_MEMORY;
        else {
            c->sib = v;
            int at = c->nsib;                                       // priority 0: last; otherwise before lower priorities
            if (priority) for (at = 0; at < c->nsib && c->sib[at].priority >= priority; at++) {}
            memmove(c->sib + at + 1, c->sib + at, (size_t)(c->nsib - at) * sizeof *v);
            c->sib[at] = (sibling){ s, flags, priority };
            c->nsib++;
            s->refs++;
        }
    }
    pthread_mutex_unlock(&g_lock);
    if (err) { vx_set_last_error(err); return 0; }
    return 1;
}
static WINAPI void CertRemoveStoreFromCollection(void *collection, void *sibling_store) {
    pthread_mutex_lock(&g_lock);
    store *c = store_of(collection), *s = store_of(sibling_store);
    if (c && s) for (int i = 0; i < c->nsib; i++) if (c->sib[i].s == s) {
        memmove(c->sib + i, c->sib + i + 1, (size_t)(c->nsib - i - 1) * sizeof *c->sib);
        c->nsib--;
        store_release(s);
        break;
    }
    pthread_mutex_unlock(&g_lock);
}

// Nothing here is persisted or changed from outside: a resync finds nothing new and a change event never fires.
// A commit succeeds only when there is nothing to write (a system store changed here is not saved).
static int store_dirty(store *s, int depth) {
    if (depth > 16) return 0;
    if (s->kind == STORE_SYSTEM && s->dirty) return 1;
    for (int i = 0; i < s->nsib; i++) if (store_dirty(s->sib[i].s, depth + 1)) return 1;
    return 0;
}
static WINAPI BOOL CertControlStore(void *h, DWORD flags, DWORD type, const void *para) {
    (void)flags; (void)para;
    pthread_mutex_lock(&g_lock);
    store *s = store_of(h);
    DWORD err = !s ? ERROR_INVALID_HANDLE
              : type == CTRL_COMMIT ? (store_dirty(s, 0) ? ERROR_NOT_SUPPORTED : 0)
              : type >= CTRL_RESYNC && type <= CTRL_CANCEL_NOTIFY ? 0 : ERROR_NOT_SUPPORTED;
    pthread_mutex_unlock(&g_lock);
    if (err) { vx_set_last_error(err); return 0; }
    return 1;
}

// The store a certificate goes to: the store itself, or a collection's first sibling that allows adding.
static store *add_target(store *s, int depth) {
    if (!s || depth > 16) return NULL;
    if (s->kind != STORE_COLLECTION) return s->readonly ? NULL : s;
    for (int i = 0; i < s->nsib; i++)
        if (s->sib[i].flags & CERT_PHYSICAL_STORE_ADD_ENABLE_FLAG) { store *t = add_target(s->sib[i].s, depth + 1); if (t) return t; }
    return NULL;
}

static DWORD add_encoded(void *h, const uint8_t *data, DWORD n, DWORD how, cert **out) {
    if (how < ADD_NEW || how > ADD_NEWER_INHERIT_PROPERTIES) return E_INVALIDARG;
    if (!data || !n) return CRYPT_E_ASN1_EOD;
    store *s;
    if (h) {
        s = store_of(h);
        if (!s) return E_INVALIDARG;
        if (!(s = add_target(s, 0))) return E_ACCESSDENIED;
    } else if (!(s = store_new(STORE_MEMORY))) return ERROR_NOT_ENOUGH_MEMORY;   // no store: a context of its own
    cert *existing = NULL;
    if (how != ADD_ALWAYS) {
        tlv whole; span all = { data, n };
        DWORD e = der_expect(&all, 0x30, &whole);
        if (e) { if (!h) store_release(s); return e; }
        for (cert *c = s->first; c; c = c->next)
            if (c->der_n == whole.whole.n && !memcmp(c->der, data, c->der_n)) { existing = c; break; }
    }
    if (existing && how == ADD_NEW) return CRYPT_E_EXISTS;
    if (existing && how == ADD_USE_EXISTING) { if (out) { cert_addref(existing); *out = existing; } return 0; }
    if (existing && (how == ADD_NEWER || how == ADD_NEWER_INHERIT_PROPERTIES)) return CRYPT_E_EXISTS;   // (same NotBefore)
    cert *c;
    DWORD e = cert_new(s, data, n, &c);
    if (e) { if (!h) store_release(s); return e; }
    if (existing) unlist(existing);                                  // ADD_REPLACE_EXISTING*
    if (out) { cert_addref(c); *out = c; }
    if (!h) store_release(s);                                        // (the context keeps its store)
    return 0;
}

static WINAPI BOOL CertAddEncodedCertificateToStore(void *h, DWORD encoding, const uint8_t *data, DWORD n, DWORD how, void **added) {
    if (added) *added = NULL;
    if (!(encoding & X509_ASN_ENCODING)) { vx_set_last_error(E_INVALIDARG); return 0; }
    pthread_mutex_lock(&g_lock);
    cert *c = NULL;
    DWORD e = add_encoded(h, data, n, how, added ? &c : NULL);
    pthread_mutex_unlock(&g_lock);
    if (e) { vx_set_last_error(e); return 0; }
    if (added) *added = PROG_PTR(&c->ctx);
    return 1;
}
static WINAPI BOOL CertAddCertificateContextToStore(void *h, const void *ctx, DWORD how, void **added) {
    if (added) *added = NULL;
    const cert_context *x = ctx;
    if (!x) { vx_set_last_error(E_INVALIDARG); return 0; }
    return CertAddEncodedCertificateToStore(h, X509_ASN_ENCODING, host(x->pbCertEncoded), x->cbCertEncoded, how, added);
}
static WINAPI void *CertCreateCertificateContext(DWORD encoding, const uint8_t *data, DWORD n) {
    void *ctx = NULL;
    return CertAddEncodedCertificateToStore(NULL, encoding, data, n, ADD_ALWAYS, &ctx) ? ctx : NULL;
}
static WINAPI BOOL CertFreeCertificateContext(const void *ctx) {
    if (!ctx) return 1;
    pthread_mutex_lock(&g_lock);
    cert *c = cert_of(ctx);
    if (c && c->refs > 0) cert_release(c);
    pthread_mutex_unlock(&g_lock);
    return 1;
}
static WINAPI void *CertDuplicateCertificateContext(const void *ctx) {
    pthread_mutex_lock(&g_lock);
    cert *c = cert_of(ctx);
    if (c && c->refs > 0) cert_addref(c);
    pthread_mutex_unlock(&g_lock);
    return c ? (void *)ctx : NULL;
}
static WINAPI BOOL CertDeleteCertificateFromStore(const void *ctx) {
    pthread_mutex_lock(&g_lock);
    cert *c = cert_of(ctx);
    DWORD err = !c || c->refs <= 0 ? E_INVALIDARG : c->owner->readonly ? E_ACCESSDENIED : 0;
    if (c && c->refs > 0) {
        if (!err && c->listed) unlist(c);                            // (the program's reference keeps it until released here)
        cert_release(c);                                             // the context passed is always freed
    }
    pthread_mutex_unlock(&g_lock);
    if (err) { vx_set_last_error(err); return 0; }
    return 1;
}

// ---- names as text ------------------------------------------------------------------------------------------------
typedef struct { WCHAR *p; size_t n, cap; int oom; } wbuf;
static void wb_put(wbuf *b, WCHAR c) {
    if (b->n == b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 64;
        WCHAR *p = realloc(b->p, cap * sizeof *p);
        if (!p) { b->oom = 1; return; }
        b->p = p; b->cap = cap;
    }
    b->p[b->n++] = c;
}
static void wb_ascii(wbuf *b, const char *s) { while (*s) wb_put(b, (uint8_t)*s++); }
static void wb_cp(wbuf *b, uint32_t c) {
    if (c >= 0x10000) { c -= 0x10000; wb_put(b, (WCHAR)(0xd800 | c >> 10)); wb_put(b, (WCHAR)(0xdc00 | (c & 0x3ff))); }
    else wb_put(b, (WCHAR)c);
}

// UTF-8 to UTF-16; with strict, -1 (nothing written) at the first malformed sequence, else U+FFFD for it.
static int utf8_to(wbuf *b, span v, int strict) {
    for (int pass = strict ? 0 : 1; pass < 2; pass++) {
        for (size_t i = 0; i < v.n;) {
            uint8_t c = v.p[i];
            uint32_t cp; int k;
            if (c < 0x80) { cp = c; k = 0; }
            else if ((c & 0xe0) == 0xc0) { cp = c & 0x1f; k = 1; }
            else if ((c & 0xf0) == 0xe0) { cp = c & 0x0f; k = 2; }
            else if ((c & 0xf8) == 0xf0) { cp = c & 0x07; k = 3; }
            else k = -1;
            int ok = k >= 0 && i + (size_t)k < v.n;
            for (int j = 1; ok && j <= k; j++) { if ((v.p[i + j] & 0xc0) != 0x80) ok = 0; else cp = cp << 6 | (v.p[i + j] & 0x3f); }
            if (ok && ((k == 1 && cp < 0x80) || (k == 2 && cp < 0x800) || (k == 3 && (cp < 0x10000 || cp > 0x10ffff)) ||
                       (cp >= 0xd800 && cp < 0xe000))) ok = 0;
            if (!ok) {
                if (!pass) return -1;
                wb_put(b, 0xfffd); i++; continue;
            }
            if (pass) wb_cp(b, cp);
            i += (size_t)k + 1;
        }
    }
    return 0;
}

// A directory string's value as text; 0 when it is not a string type (the caller shows it in hex).
static int value_text(wbuf *b, uint8_t tag, span v, DWORD flags) {
    switch (tag) {
    case 0x0c: utf8_to(b, v, 0); return 1;                              // UTF8String
    case 0x14:                                                          // TeletexString: UTF-8 first, unless disabled
        if (!(flags & CERT_NAME_DISABLE_IE4_UTF8_FLAG) && !utf8_to(b, v, 1)) return 1;
        /* fall through */
    case 0x12: case 0x13: case 0x15: case 0x16: case 0x19: case 0x1a: case 0x1b:   // 8-bit strings
        for (size_t i = 0; i < v.n; i++) wb_put(b, v.p[i]);
        return 1;
    case 0x1e:                                                          // BMPString: UTF-16 big-endian
        for (size_t i = 0; i + 1 < v.n; i += 2) wb_put(b, (WCHAR)(v.p[i] << 8 | v.p[i + 1]));
        return 1;
    case 0x1c:                                                          // UniversalString: UCS-4 big-endian
        for (size_t i = 0; i + 3 < v.n; i += 4) {
            uint32_t c = (uint32_t)v.p[i] << 24 | (uint32_t)v.p[i + 1] << 16 | (uint32_t)v.p[i + 2] << 8 | v.p[i + 3];
            wb_cp(b, c <= 0x10ffff && (c < 0xd800 || c >= 0xe000) ? c : 0xfffd);
        }
        return 1;
    }
    return 0;
}
static void hex_value(wbuf *b, span whole) {
    static const char digits[] = "0123456789ABCDEF";
    wb_put(b, '#');
    for (size_t i = 0; i < whole.n; i++) { wb_put(b, (WCHAR)digits[whole.p[i] >> 4]); wb_put(b, (WCHAR)digits[whole.p[i] & 15]); }
}

typedef struct { char oid[128]; uint8_t tag; span value, whole; int rdn; } name_attr;
// The attributes of an encoded Name, in order; -1 if it is not one.
static int name_attrs(span name, name_attr *out, int cap) {
    tlv seq, rdn, atv, o, v;
    if (der_expect(&name, 0x30, &seq)) return -1;
    int n = 0, r = 0;
    for (span rs = seq.body; rs.n; r++) {
        if (der_expect(&rs, 0x31, &rdn)) return -1;
        for (span as = rdn.body; as.n;) {
            if (der_expect(&as, 0x30, &atv)) return -1;
            span ab = atv.body;
            if (der_expect(&ab, 0x06, &o) || der_next(&ab, &v)) return -1;
            if (n < cap) {
                if (oid_text(o.body, out[n].oid, sizeof out[n].oid)) return -1;
                out[n].tag = v.tag; out[n].value = v.body; out[n].whole = v.whole; out[n].rdn = r;
            }
            n++;
        }
    }
    return n < cap ? n : cap;
}
#define MAX_ATTRS 64

static const struct { const char *oid, *key; } x500_keys[] = {
    { "2.5.4.3", "CN" }, { "2.5.4.7", "L" }, { "2.5.4.8", "S" }, { "2.5.4.10", "O" }, { "2.5.4.11", "OU" }, { "2.5.4.6", "C" },
    { "2.5.4.9", "STREET" }, { "1.2.840.113549.1.9.1", "E" }, { "2.5.4.12", "T" }, { "2.5.4.42", "G" }, { "2.5.4.43", "I" },
    { "2.5.4.4", "SN" }, { "0.9.2342.19200300.100.1.25", "DC" }, { "2.5.4.5", "SERIALNUMBER" },
};

// CertNameToStr's text of an encoded Name (wbuf without the terminator); -1 if it is not one.
static int name_to_text(span name, DWORD type, wbuf *out) {
    name_attr attrs[MAX_ATTRS];
    int n = name_attrs(name, attrs, MAX_ATTRS);
    if (n < 0) return -1;
    DWORD fmt = type & 0xffff;
    if (fmt < CERT_SIMPLE_NAME_STR || fmt > CERT_X500_NAME_STR) fmt = CERT_SIMPLE_NAME_STR;
    const char *rdn_sep = type & CERT_NAME_STR_SEMICOLON_FLAG ? "; " : type & CERT_NAME_STR_CRLF_FLAG ? "\r\n" : ", ";
    const char *plus = type & CERT_NAME_STR_NO_PLUS_FLAG ? " " : fmt == CERT_SIMPLE_NAME_STR ? " + " : "+ ";
    int reverse = !!(type & CERT_NAME_STR_REVERSE_FLAG);
    int last_rdn = n ? attrs[n - 1].rdn : -1, any = 0;
    for (int k = 0; k <= last_rdn; k++) {
        int r = reverse ? last_rdn - k : k, first_in_rdn = 1;
        for (int i = 0; i < n; i++) {
            if (attrs[i].rdn != r) continue;
            if (any) wb_ascii(out, first_in_rdn ? rdn_sep : plus);
            first_in_rdn = 0;
            any = 1;
            if (fmt != CERT_SIMPLE_NAME_STR) {
                const char *key = NULL;
                if (fmt == CERT_X500_NAME_STR)
                    for (size_t j = 0; j < sizeof x500_keys / sizeof x500_keys[0]; j++) if (!strcmp(x500_keys[j].oid, attrs[i].oid)) key = x500_keys[j].key;
                if (!key && fmt == CERT_X500_NAME_STR) wb_ascii(out, "OID.");
                wb_ascii(out, key ? key : attrs[i].oid);
                wb_put(out, '=');
            }
            wbuf v = { 0 };
            if (!value_text(&v, attrs[i].tag, attrs[i].value, type)) { hex_value(out, attrs[i].whole); free(v.p); continue; }
            int quote = 0;
            if (!(type & CERT_NAME_STR_NO_QUOTING_FLAG)) {
                quote = !v.n || v.p[0] == ' ' || v.p[0] == '\t' || v.p[v.n - 1] == ' ' || v.p[v.n - 1] == '\t';
                for (size_t j = 0; j < v.n && !quote; j++) quote = v.p[j] < 0x80 && strchr(",+=\"\\\r\n<>#;", (char)v.p[j]) && v.p[j];
            }
            if (quote) wb_put(out, '"');
            for (size_t j = 0; j < v.n; j++) { if (quote && v.p[j] == '"') wb_put(out, '"'); wb_put(out, v.p[j]); }
            if (quote) wb_put(out, '"');
            if (v.oom) out->oom = 1;
            free(v.p);
        }
    }
    return 0;
}

// The first value of an attribute in an encoded Name; 1 if found.
static int name_attr_text(span name, const char *oid, DWORD flags, wbuf *out) {
    name_attr attrs[MAX_ATTRS];
    int n = name_attrs(name, attrs, MAX_ATTRS);
    for (int i = 0; i < n; i++) {
        if (oid && strcmp(attrs[i].oid, oid)) continue;
        if (!value_text(out, attrs[i].tag, attrs[i].value, flags)) hex_value(out, attrs[i].whole);
        return 1;
    }
    return 0;
}

// The alternative-name extension (GeneralNames) of the subject or the issuer.
static int alt_names(const cert *c, int issuer, span *out) {
    const ext *e = find_ext(c, issuer ? "2.5.29.18" : "2.5.29.17");
    if (!e) e = find_ext(c, issuer ? "2.5.29.8" : "2.5.29.7");      // (the older OIDs)
    tlv seq;
    span v = e ? e->value : (span){ 0 };
    if (!e || der_expect(&v, 0x30, &seq)) return 0;
    *out = seq.body;
    return 1;
}
// The next GeneralName with the given tag (0x81 rfc822Name, 0x82 dNSName, 0x86 URI, 0xa4 directoryName, 0xa0 otherName).
static int alt_next(span *names, uint8_t tag, tlv *out) {
    while (names->n) {
        if (der_next(names, out)) { names->n = 0; return 0; }
        if (out->tag == tag) return 1;
    }
    return 0;
}

#define CERT_NAME_EMAIL_TYPE 1
#define CERT_NAME_RDN_TYPE 2
#define CERT_NAME_ATTR_TYPE 3
#define CERT_NAME_SIMPLE_DISPLAY_TYPE 4
#define CERT_NAME_FRIENDLY_DISPLAY_TYPE 5
#define CERT_NAME_DNS_TYPE 6
#define CERT_NAME_URL_TYPE 7
#define CERT_NAME_UPN_TYPE 8

// The name (subject or issuer) itself, or when it is empty the first directoryName of the alternative names.
static span name_or_alt_dir(const cert *c, int issuer) {
    span name = issuer ? c->issuer : c->subject, alts;
    name_attr one[1];
    tlv dn;
    if (name_attrs(name, one, 1) <= 0 && alt_names(c, issuer, &alts) && alt_next(&alts, 0xa4, &dn)) return dn.body;
    return name;
}

// CertGetNameString's text (without the final terminator; for CERT_NAME_SEARCH_ALL_NAMES_FLAG, names separated by 0).
static void name_string(const cert *c, DWORD type, DWORD flags, const void *para, wbuf *out, int *multi) {
    int issuer = flags & CERT_NAME_ISSUER_FLAG;
    span name = issuer ? c->issuer : c->subject, alts = { 0 };
    int has_alts = alt_names(c, issuer, &alts);
    tlv g;
    *multi = 0;
    if ((flags & CERT_NAME_SEARCH_ALL_NAMES_FLAG) && type != CERT_NAME_DNS_TYPE) return;
    switch (type) {
    case CERT_NAME_EMAIL_TYPE: {
        span a = alts;
        if (has_alts && alt_next(&a, 0x81, &g)) { value_text(out, 0x16, g.body, flags); return; }
        name_attr_text(name, "1.2.840.113549.1.9.1", flags, out);
        return;
    }
    case CERT_NAME_RDN_TYPE:
        name_to_text(name_or_alt_dir(c, issuer), para ? *(const DWORD *)para : 0, out);
        return;
    case CERT_NAME_ATTR_TYPE:
        if (para) name_attr_text(name_or_alt_dir(c, issuer), para, flags, out);
        return;
    case CERT_NAME_SIMPLE_DISPLAY_TYPE: case CERT_NAME_FRIENDLY_DISPLAY_TYPE: {   // (no friendly-name properties here)
        static const char *const order[] = { "2.5.4.3", "2.5.4.11", "2.5.4.10", "1.2.840.113549.1.9.1" };
        for (int i = 0; i < 4; i++) {
            if (name_attr_text(name, order[i], flags, out)) return;
            span a = alts;
            while (has_alts && alt_next(&a, 0xa4, &g)) if (name_attr_text(g.body, order[i], flags, out)) return;
        }
        span a = alts;
        if (has_alts && alt_next(&a, 0x81, &g)) { value_text(out, 0x16, g.body, flags); return; }
        name_attr_text(name, NULL, flags, out);
        return;
    }
    case CERT_NAME_DNS_TYPE: {
        span a = alts;
        int found = 0;
        while (has_alts && alt_next(&a, 0x82, &g)) {
            if (found) wb_put(out, 0);
            value_text(out, 0x16, g.body, flags);
            found = 1;
            if (!(flags & CERT_NAME_SEARCH_ALL_NAMES_FLAG)) break;
        }
        if (!found) found = name_attr_text(name, "2.5.4.3", flags, out);
        *multi = (flags & CERT_NAME_SEARCH_ALL_NAMES_FLAG) && found;
        return;
    }
    case CERT_NAME_URL_TYPE: {
        span a = alts;
        if (has_alts && alt_next(&a, 0x86, &g)) value_text(out, 0x16, g.body, flags);
        return;
    }
    case CERT_NAME_UPN_TYPE: {
        span a = alts;
        while (has_alts && alt_next(&a, 0xa0, &g)) {                // otherName: type-id, [0] EXPLICIT value
            span o = g.body; tlv id, wrap, val;
            char text[64];
            if (der_expect(&o, 0x06, &id) || oid_text(id.body, text, sizeof text) || strcmp(text, "1.3.6.1.4.1.311.20.2.3")) continue;
            if (der_expect(&o, 0xa0, &wrap)) continue;
            span w = wrap.body;
            if (!der_next(&w, &val) && value_text(out, val.tag, val.body, flags)) return;
        }
        return;
    }
    }
}

// A certificate from a context the program passes: ours, or (made by the program itself) parsed for the call.
static cert *cert_for_call(const void *ctx, cert **temp) {
    *temp = NULL;
    cert *c = cert_of(ctx);
    if (c || !ctx) return c;
    const cert_context *x = ctx;
    cert *t = calloc(1, sizeof *t);
    uint8_t *der = t ? malloc(x->cbCertEncoded ? x->cbCertEncoded : 1) : NULL;
    if (!t || !der || !x->pbCertEncoded) { free(t); free(der); return NULL; }
    memcpy(der, host(x->pbCertEncoded), x->cbCertEncoded);
    t->der = der; t->der_n = x->cbCertEncoded;
    if (parse_cert(t)) { cert_free_memory(t); return NULL; }
    return *temp = t;
}

// Copies a UTF-16 result out (n units including the terminators) the way the CertGetNameString family does.
static DWORD return_w(const wbuf *b, DWORD total, WCHAR *out, DWORD cap) {
    if (!out || !cap) return total;
    if (cap >= total) { if (b->n) memcpy(out, b->p, b->n * sizeof(WCHAR)); for (DWORD i = (DWORD)b->n; i < total; i++) out[i] = 0; return total; }
    if (cap > 1) memcpy(out, b->p, (cap - 1) * sizeof(WCHAR));
    out[cap - 1] = 0;
    return cap;
}
static DWORD return_a(const wbuf *b, DWORD total_w, char *out, DWORD cap) {   // ANSI here is UTF-8 (GetACP)
    size_t units = total_w;                                          // (with the terminators, converted alike)
    WCHAR *w = calloc(units ? units : 1, sizeof *w);
    if (!w) return 0;
    if (b->n) memcpy(w, b->p, b->n * sizeof *w);
    size_t need = vx_utf16_to_utf8(w, units, NULL, 0);
    char *a = malloc(need ? need : 1);
    if (!a) { free(w); return 0; }
    vx_utf16_to_utf8(w, units, a, need);
    free(w);
    DWORD r;
    if (!out || !cap) r = (DWORD)need;
    else if (cap >= need) { memcpy(out, a, need); r = (DWORD)need; }
    else {
        size_t k = cap - 1;
        while (k && (a[k] & 0xc0) == 0x80) k--;                      // (not inside a character)
        memcpy(out, a, k);
        out[k] = 0;
        r = cap;
    }
    free(a);
    return r;
}

static DWORD get_name_string(const void *ctx, DWORD type, DWORD flags, const void *para, wbuf *b) {
    cert *temp, *c = cert_for_call(ctx, &temp);
    int multi = 0;
    if (c) name_string(c, type, flags, para, b, &multi);
    if (temp) cert_free_memory(temp);
    if (b->oom) b->n = 0;
    return (DWORD)b->n + 1 + (multi ? 1 : 0);
}
static WINAPI DWORD CertGetNameStringW(const void *ctx, DWORD type, DWORD flags, void *para, WCHAR *out, DWORD cap) {
    wbuf b = { 0 };
    DWORD total = get_name_string(ctx, type, flags, para, &b);
    DWORD r = return_w(&b, total, out, cap);
    free(b.p);
    return r;
}
static WINAPI DWORD CertGetNameStringA(const void *ctx, DWORD type, DWORD flags, void *para, char *out, DWORD cap) {
    wbuf b = { 0 };
    DWORD total = get_name_string(ctx, type, flags, para, &b);
    DWORD r = return_a(&b, total, out, cap);
    free(b.p);
    return r;
}
static WINAPI DWORD CertNameToStrW(DWORD encoding, const blob_t *name, DWORD type, WCHAR *out, DWORD cap) {
    (void)encoding;
    wbuf b = { 0 };
    if (name && name->pbData) name_to_text((span){ host(name->pbData), name->cbData }, type, &b);
    if (b.oom) b.n = 0;
    DWORD r = return_w(&b, (DWORD)b.n + 1, out, cap);
    free(b.p);
    return r;
}
static WINAPI DWORD CertNameToStrA(DWORD encoding, const blob_t *name, DWORD type, char *out, DWORD cap) {
    (void)encoding;
    wbuf b = { 0 };
    if (name && name->pbData) name_to_text((span){ host(name->pbData), name->cbData }, type, &b);
    if (b.oom) b.n = 0;
    DWORD r = return_a(&b, (DWORD)b.n + 1, out, cap);
    free(b.p);
    return r;
}

static WINAPI BOOL CertCompareCertificateName(DWORD encoding, const blob_t *a, const blob_t *b) {
    (void)encoding;
    if (!a || !b || a->cbData != b->cbData) return 0;
    return !a->cbData || !memcmp(host(a->pbData), host(b->pbData), a->cbData);
}
static int same_serial(const blob_t *a, const blob_t *b) {          // (both little-endian, as in CERT_INFO)
    return a->cbData == b->cbData && (!a->cbData || !memcmp(host(a->pbData), host(b->pbData), a->cbData));
}
static WINAPI BOOL CertCompareCertificate(DWORD encoding, const cert_info *a, const cert_info *b) {
    return a && b && same_serial(&a->SerialNumber, &b->SerialNumber) && CertCompareCertificateName(encoding, &a->Issuer, &b->Issuer);
}
static WINAPI BOOL CertComparePublicKeyInfo(DWORD encoding, const pubkey_info *a, const pubkey_info *b) {
    (void)encoding;
    if (!a || !b || !a->Algorithm.pszObjId || !b->Algorithm.pszObjId) return 0;
    return !strcmp(host(a->Algorithm.pszObjId), host(b->Algorithm.pszObjId)) && a->PublicKey.cbData == b->PublicKey.cbData &&
           a->PublicKey.cUnusedBits == b->PublicKey.cUnusedBits &&
           (!a->PublicKey.cbData || !memcmp(host(a->PublicKey.pbData), host(b->PublicKey.pbData), a->PublicKey.cbData));
}

static uint64_t now_filetime(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)(ts.tv_sec + 11644473600LL) * 10000000ull + (uint64_t)ts.tv_nsec / 100;
}
static WINAPI LONG CertVerifyTimeValidity(const filetime *when, const cert_info *info) {
    if (!info) return 1;                                             // (no certificate: not valid now)
    uint64_t t = when ? (uint64_t)when->hi << 32 | when->lo : now_filetime();
    uint64_t nb = (uint64_t)info->NotBefore.hi << 32 | info->NotBefore.lo, na = (uint64_t)info->NotAfter.hi << 32 | info->NotAfter.lo;
    return t < nb ? -1 : t > na ? 1 : 0;
}

static WINAPI cert_ext *CertFindExtension(const char *oid, DWORD count, cert_ext *exts) {
    for (DWORD i = 0; oid && exts && i < count; i++) {
        const char *o = host(exts[i].pszObjId);
        if (o && !strcmp(o, oid)) return PROG_PTR(&exts[i]);
    }
    vx_set_last_error(CRYPT_E_NOT_FOUND);
    return NULL;
}

// ---- key usage ------------------------------------------------------------------------------------------------------
static WINAPI BOOL CertGetEnhancedKeyUsage(const void *ctx, DWORD flags, void *usage, DWORD *size) {
    cert *temp, *c = cert_for_call(ctx, &temp);
    if (!c || !size) { if (temp) cert_free_memory(temp); vx_set_last_error(E_INVALIDARG); return 0; }
    // Only the extension: this runtime keeps no CERT_ENHKEY_USAGE_PROP_ID properties.
    const ext *e = flags & CERT_FIND_PROP_ONLY_ENHKEY_USAGE_FLAG ? NULL : find_ext(c, "2.5.29.37");
    char oids[64][128];
    int n = 0;
    DWORD err = 0;
    if (e) {
        span v = e->value; tlv seq, o;
        err = der_expect(&v, 0x30, &seq);
        for (span s = seq.body; !err && s.n && n < 64; n++) {
            err = der_expect(&s, 0x06, &o);
            if (!err) err = oid_text(o.body, oids[n], sizeof oids[n]);
        }
    }
    if (temp) cert_free_memory(temp);
    if (err) { vx_set_last_error(err); return 0; }
    size_t need = 16 + (size_t)n * 8;
    for (int i = 0; i < n; i++) need += strlen(oids[i]) + 1;
    if (usage) {
        if (*size < need) { *size = (DWORD)need; vx_set_last_error(ERROR_MORE_DATA); return 0; }
        uint8_t *base = usage;
        char **array = (char **)(base + 16), *strings = (char *)(base + 16 + (size_t)n * 8);
        *(DWORD *)base = (DWORD)n;
        *(DWORD *)(base + 4) = 0;
        *(void **)(base + 8) = n ? PROG_PTR(array) : NULL;
        for (int i = 0; i < n; i++) {
            size_t len = strlen(oids[i]) + 1;
            memcpy(strings, oids[i], len);
            array[i] = PROG_PTR(strings);
            strings += len;
        }
    }
    *size = (DWORD)need;
    // no extension: good for all uses (CRYPT_E_NOT_FOUND); an extension listing nothing: good for none (0)
    vx_set_last_error(e ? 0 : CRYPT_E_NOT_FOUND);
    return 1;
}

static WINAPI BOOL CertGetIntendedKeyUsage(DWORD encoding, const cert_info *info, uint8_t *out, DWORD n) {
    (void)encoding;
    if (out && n) memset(out, 0, n);
    if (!info) { vx_set_last_error(E_INVALIDARG); return 0; }
    const cert_ext *exts = host(info->rgExtension);
    for (DWORD pass = 0; pass < 2; pass++)                           // szOID_KEY_USAGE, then szOID_KEY_ATTRIBUTES
        for (DWORD i = 0; exts && i < info->cExtension; i++) {
            const char *oid = host(exts[i].pszObjId);
            if (!oid || strcmp(oid, pass ? "2.5.29.2" : "2.5.29.15")) continue;
            span v = { host(exts[i].Value.pbData), exts[i].Value.cbData };
            tlv bits;
            DWORD e;
            if (pass) {                                              // KeyAttributes: the intendedKeyUsage BIT STRING
                tlv seq, x;
                e = der_expect(&v, 0x30, &seq);
                bits.tag = 0;
                for (span s = seq.body; !e && s.n;) { e = der_next(&s, &x); if (!e && x.tag == 0x03) { bits = x; break; } }
                if (!e && bits.tag != 0x03) continue;
            } else e = der_expect(&v, 0x03, &bits);
            if (e || !bits.body.n || bits.body.p[0] > 7) { vx_set_last_error(e ? e : CRYPT_E_ASN1_CORRUPT); return 0; }
            size_t k = bits.body.n - 1;
            if (out) memcpy(out, bits.body.p + 1, k < n ? k : n);
            return 1;
        }
    vx_set_last_error(0);                                            // none: as documented, FALSE with no error
    return 0;
}

// ---- properties -----------------------------------------------------------------------------------------------------
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"     // (SHA-1 and MD5 name certificates here; nothing is trusted on them)
static WINAPI BOOL CertGetCertificateContextProperty(const void *ctx, DWORD prop, void *data, DWORD *size) {
    cert *temp, *c = cert_for_call(ctx, &temp);
    uint8_t buf[256];
    size_t n = 0;
    DWORD err = 0;
    if (!c || !size) err = E_INVALIDARG;
    else switch (prop) {
    case PROP_SHA1: CC_SHA1(c->der, c->der_n, buf); n = 20; break;
    case PROP_MD5: CC_MD5(c->der, c->der_n, buf); n = 16; break;
    case PROP_SHA256: CC_SHA256(c->der, c->der_n, buf); n = 32; break;
    case PROP_SUBJECT_MD5: CC_MD5(c->subject.p, (CC_LONG)c->subject.n, buf); n = 16; break;
    case PROP_PUBKEY_MD5: CC_MD5(c->key_bits.p, (CC_LONG)c->key_bits.n, buf); n = 16; break;
    case PROP_KEY_IDENTIFIER: {                                      // the subject key identifier, else SHA-1 of the key info
        const ext *e = find_ext(c, "2.5.29.14");
        span v = e ? e->value : (span){ 0 }; tlv id;
        if (e && !der_expect(&v, 0x04, &id) && id.body.n <= sizeof buf) { memcpy(buf, id.body.p, id.body.n); n = id.body.n; }
        else { CC_SHA1(c->spki.p, (CC_LONG)c->spki.n, buf); n = 20; }
        break;
    }
    case PROP_ACCESS_STATE: {
        DWORD v = c->owner && c->owner->kind == STORE_SYSTEM ? CERT_ACCESS_STATE_SYSTEM_STORE_FLAG : 0;
        memcpy(buf, &v, 4); n = 4;
        break;
    }
    default: err = CRYPT_E_NOT_FOUND;                                 // no other property is kept here (no keys, no names)
    }
    if (temp) cert_free_memory(temp);
    if (err) { vx_set_last_error(err); return 0; }
    if (data && *size < n) { *size = (DWORD)n; vx_set_last_error(ERROR_MORE_DATA); return 0; }
    if (data) memcpy(data, buf, n);
    *size = (DWORD)n;
    return 1;
}

// ---- finding ------------------------------------------------------------------------------------------------------
#define CERT_FIND_ANY 0
#define CERT_FIND_SHA1_HASH 0x10000
#define CERT_FIND_MD5_HASH 0x40000
#define CERT_FIND_SUBJECT_NAME 0x20007
#define CERT_FIND_ISSUER_NAME 0x20004
#define CERT_FIND_SUBJECT_STR_A 0x70007
#define CERT_FIND_ISSUER_STR_A 0x70004
#define CERT_FIND_SUBJECT_STR_W 0x80007
#define CERT_FIND_ISSUER_STR_W 0x80004
#define CERT_FIND_PUBLIC_KEY 0x60000
#define CERT_FIND_KEY_SPEC 0x90000
#define CERT_FIND_ENHKEY_USAGE 0xa0000
#define CERT_FIND_SUBJECT_CERT 0xb0000
#define CERT_FIND_ISSUER_OF 0xc0000
#define CERT_FIND_EXISTING 0xd0000
#define CERT_FIND_KEY_IDENTIFIER 0xf0000

static int contains_ci(const wbuf *hay, const WCHAR *needle) {     // case-insensitive substring
    size_t m = 0;
    while (needle[m]) m++;
    if (!m) return 1;
    for (size_t i = 0; i + m <= hay->n; i++) {
        size_t j = 0;
        while (j < m && towlower(hay->p[i + j]) == towlower(needle[j])) j++;
        if (j == m) return 1;
    }
    return 0;
}

static int matches(cert *c, DWORD type, DWORD flags, const void *para) {
    uint8_t h[32];
    switch (type) {
    case CERT_FIND_ANY: return 1;
    case CERT_FIND_SHA1_HASH: case CERT_FIND_MD5_HASH: {
        const blob_t *want = para;
        size_t n = type == CERT_FIND_MD5_HASH ? 16 : 20;
        if (type == CERT_FIND_MD5_HASH) CC_MD5(c->der, c->der_n, h); else CC_SHA1(c->der, c->der_n, h);
        return want && want->cbData == n && !memcmp(host(want->pbData), h, n);
    }
    case CERT_FIND_SUBJECT_NAME: case CERT_FIND_ISSUER_NAME: {
        const blob_t *want = para;
        span name = type == CERT_FIND_SUBJECT_NAME ? c->subject : c->issuer;
        return want && want->cbData == name.n && !memcmp(host(want->pbData), name.p, name.n);
    }
    case CERT_FIND_SUBJECT_STR_A: case CERT_FIND_ISSUER_STR_A: case CERT_FIND_SUBJECT_STR_W: case CERT_FIND_ISSUER_STR_W: {
        if (!para) return 0;
        WCHAR *needle = type == CERT_FIND_SUBJECT_STR_A || type == CERT_FIND_ISSUER_STR_A ? vx_utf16_dup(para) : NULL;
        wbuf text = { 0 };
        name_to_text(type == CERT_FIND_SUBJECT_STR_A || type == CERT_FIND_SUBJECT_STR_W ? c->subject : c->issuer, CERT_SIMPLE_NAME_STR, &text);
        int r = !text.oom && contains_ci(&text, needle ? needle : para);
        free(text.p); free(needle);
        return r;
    }
    case CERT_FIND_PUBLIC_KEY: return para && CertComparePublicKeyInfo(X509_ASN_ENCODING, para, &c->info.SubjectPublicKeyInfo);
    case CERT_FIND_SUBJECT_CERT: {
        const cert_info *want = para;
        return want && CertCompareCertificate(X509_ASN_ENCODING, want, &c->info);
    }
    case CERT_FIND_ISSUER_OF: {
        cert *of = cert_of(para);
        return of && of->issuer.n == c->subject.n && !memcmp(of->issuer.p, c->subject.p, c->subject.n);
    }
    case CERT_FIND_EXISTING: {
        const cert_context *x = para;
        return x && x->cbCertEncoded == c->der_n && !memcmp(host(x->pbCertEncoded), c->der, c->der_n);
    }
    case CERT_FIND_KEY_IDENTIFIER: {
        const blob_t *want = para;
        uint8_t id[256]; DWORD n = sizeof id;
        return want && CertGetCertificateContextProperty(&c->ctx, PROP_KEY_IDENTIFIER, id, &n) && want->cbData == n &&
               !memcmp(host(want->pbData), id, n);
    }
    case CERT_FIND_ENHKEY_USAGE: {                                   // every usage asked for is in the extension
        const ext *e = find_ext(c, "2.5.29.37");
        if (!e) return !!(flags & CERT_FIND_OPTIONAL_ENHKEY_USAGE_FLAG);
        const uint8_t *u = para;
        DWORD count = u ? *(const DWORD *)u : 0;
        char **want = u ? host(*(char *const *)(u + 8)) : NULL;
        for (DWORD i = 0; i < count; i++) {
            int found = 0;
            span v = e->value; tlv seq, o; char text[128];
            if (der_expect(&v, 0x30, &seq)) return 0;
            for (span s = seq.body; s.n && !found;)
                found = !der_expect(&s, 0x06, &o) && !oid_text(o.body, text, sizeof text) && !strcmp(text, host(want[i]));
            if (!found) return 0;
        }
        return 1;
    }
    }
    return 0;                                                        // CERT_FIND_KEY_SPEC, ...: no keys, no such properties
}
#pragma clang diagnostic pop

static void *find_in_store(void *h, DWORD flags, DWORD type, const void *para, const void *prev) {
    pthread_mutex_lock(&g_lock);
    store *s = store_of(h);
    cert *p = cert_of(prev), *found = NULL;
    DWORD err = CRYPT_E_NOT_FOUND;
    cert_list l = { 0 };
    if (!s || (prev && !p)) err = E_INVALIDARG;
    else if (flatten(s, &l, 0)) err = ERROR_NOT_ENOUGH_MEMORY;
    else {
        int i = 0;
        if (p) { while (i < l.n && l.v[i] != p) i++; i++; }          // (a prev not in this store finds nothing more)
        for (; i < l.n && !found; i++) if (matches(l.v[i], type, flags, para)) found = l.v[i];
        if (found) cert_addref(found);
    }
    free(l.v);
    if (p && p->refs > 0) cert_release(p);                           // prev is always freed, as documented
    pthread_mutex_unlock(&g_lock);
    if (!found) { vx_set_last_error(err); return NULL; }
    return PROG_PTR(&found->ctx);
}
static WINAPI void *CertEnumCertificatesInStore(void *h, const void *prev) { return find_in_store(h, 0, CERT_FIND_ANY, NULL, prev); }
static WINAPI void *CertFindCertificateInStore(void *h, DWORD encoding, DWORD flags, DWORD type, const void *para, const void *prev) {
    (void)encoding;
    return find_in_store(h, flags, type, para, prev);
}

// ---- signatures: SecKeyVerifySignature or failure -------------------------------------------------------------------
static const struct { const char *oid; int ec; } sig_oids[] = {
    { "1.2.840.113549.1.1.5", 0 }, { "1.2.840.113549.1.1.14", 0 }, { "1.2.840.113549.1.1.11", 0 }, { "1.2.840.113549.1.1.12", 0 },
    { "1.2.840.113549.1.1.13", 0 }, { "1.2.840.10045.4.1", 1 }, { "1.2.840.10045.4.3.1", 1 }, { "1.2.840.10045.4.3.2", 1 },
    { "1.2.840.10045.4.3.3", 1 }, { "1.2.840.10045.4.3.4", 1 },
};
static const struct { const char *oid; int size; } hash_oids[] = {      // SHA-1, SHA-224, SHA-256, SHA-384, SHA-512
    { "1.3.14.3.2.26", 20 }, { "2.16.840.1.101.3.4.2.4", 28 }, { "2.16.840.1.101.3.4.2.1", 32 },
    { "2.16.840.1.101.3.4.2.2", 48 }, { "2.16.840.1.101.3.4.2.3", 64 },
};

// An AlgorithmIdentifier's hash OID (inside RSASSA-PSS parameters) as text.
static int alg_oid_text(span *b, char *out, size_t cap) {
    tlv a, o;
    if (der_expect(b, 0x30, &a)) return -1;
    span ab = a.body;
    return der_expect(&ab, 0x06, &o) || oid_text(o.body, out, cap) ? -1 : 0;
}

// The SecKey algorithm for a signature algorithm; NULL (NTE_BAD_ALGID) for anything macOS cannot verify exactly:
// MD5 and MD2, DSA, and RSASSA-PSS other than macOS's (MGF1 with the same hash, salt as long as the hash, trailer 1).
static SecKeyAlgorithm signature_algorithm(span alg_oid, span params, int *is_ec) {
    char oid[128];
    if (oid_text(alg_oid, oid, sizeof oid)) return NULL;
    const SecKeyAlgorithm algs[] = {
        kSecKeyAlgorithmRSASignatureMessagePKCS1v15SHA1, kSecKeyAlgorithmRSASignatureMessagePKCS1v15SHA224,
        kSecKeyAlgorithmRSASignatureMessagePKCS1v15SHA256, kSecKeyAlgorithmRSASignatureMessagePKCS1v15SHA384,
        kSecKeyAlgorithmRSASignatureMessagePKCS1v15SHA512, kSecKeyAlgorithmECDSASignatureMessageX962SHA1,
        kSecKeyAlgorithmECDSASignatureMessageX962SHA224, kSecKeyAlgorithmECDSASignatureMessageX962SHA256,
        kSecKeyAlgorithmECDSASignatureMessageX962SHA384, kSecKeyAlgorithmECDSASignatureMessageX962SHA512,
    };
    for (size_t i = 0; i < sizeof sig_oids / sizeof sig_oids[0]; i++)
        if (!strcmp(oid, sig_oids[i].oid)) { *is_ec = sig_oids[i].ec; return algs[i]; }
    if (strcmp(oid, "1.2.840.113549.1.1.10")) return NULL;
    char hash[128] = "1.3.14.3.2.26", mgf_hash[128] = "1.3.14.3.2.26", text[128];   // (the defaults: SHA-1, salt 20)
    int salt = 20;
    span p = params; tlv seq, x, v;
    if (der_expect(&p, 0x30, &seq)) return NULL;
    for (span s = seq.body; s.n;) {
        if (der_next(&s, &x)) return NULL;
        span b = x.body;
        if (x.tag == 0xa0) { if (alg_oid_text(&b, hash, sizeof hash)) return NULL; }
        else if (x.tag == 0xa1) {                                    // MaskGenAlgorithm: id-mgf1 with a hash
            tlv a, o;
            if (der_expect(&b, 0x30, &a)) return NULL;
            span ab = a.body;
            if (der_expect(&ab, 0x06, &o) || oid_text(o.body, text, sizeof text) || strcmp(text, "1.2.840.113549.1.1.8") ||
                alg_oid_text(&ab, mgf_hash, sizeof mgf_hash)) return NULL;
        } else if (x.tag == 0xa2) {
            if (der_expect(&b, 0x02, &v) || !v.body.n || v.body.n > 2 || (v.body.p[0] & 0x80)) return NULL;
            salt = 0;
            for (size_t k = 0; k < v.body.n; k++) salt = salt << 8 | v.body.p[k];
        } else if (x.tag == 0xa3) {
            if (der_expect(&b, 0x02, &v) || v.body.n != 1 || v.body.p[0] != 1) return NULL;
        } else return NULL;
    }
    const SecKeyAlgorithm pss[] = { kSecKeyAlgorithmRSASignatureMessagePSSSHA1, kSecKeyAlgorithmRSASignatureMessagePSSSHA224,
                                    kSecKeyAlgorithmRSASignatureMessagePSSSHA256, kSecKeyAlgorithmRSASignatureMessagePSSSHA384,
                                    kSecKeyAlgorithmRSASignatureMessagePSSSHA512 };
    for (size_t i = 0; i < sizeof hash_oids / sizeof hash_oids[0]; i++)
        if (!strcmp(hash, hash_oids[i].oid) && !strcmp(mgf_hash, hash) && salt == hash_oids[i].size) { *is_ec = 0; return pss[i]; }
    return NULL;
}

// The issuer's public key for SecKey: RSA (the PKCS #1 key) or EC on P-256/384/521 (the X9.63 point); NULL otherwise.
static SecKeyRef public_key(const char *alg_oid, span params, span bits, DWORD unused_bits, int *is_ec, DWORD *err) {
    char curve[64];
    *err = NTE_BAD_ALGID;
    if (unused_bits) return NULL;
    CFMutableDictionaryRef attrs = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(attrs, kSecAttrKeyClass, kSecAttrKeyClassPublic);
    if (!strcmp(alg_oid, "1.2.840.113549.1.1.1") || !strcmp(alg_oid, "1.2.840.113549.1.1.10")) {
        *is_ec = 0;
        CFDictionarySetValue(attrs, kSecAttrKeyType, kSecAttrKeyTypeRSA);
    } else if (!strcmp(alg_oid, "1.2.840.10045.2.1")) {
        span p = params; tlv o;
        int size = 0;
        if (!der_expect(&p, 0x06, &o) && !oid_text(o.body, curve, sizeof curve))
            size = !strcmp(curve, "1.2.840.10045.3.1.7") ? 256 : !strcmp(curve, "1.3.132.0.34") ? 384 : !strcmp(curve, "1.3.132.0.35") ? 521 : 0;
        if (!size || bits.n != 1 + 2 * (size_t)((size + 7) / 8) || bits.p[0] != 4) { CFRelease(attrs); return NULL; }   // uncompressed
        *is_ec = 1;
        CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &size);
        CFDictionarySetValue(attrs, kSecAttrKeyType, kSecAttrKeyTypeECSECPrimeRandom);
        CFDictionarySetValue(attrs, kSecAttrKeySizeInBits, n);
        CFRelease(n);
    } else { CFRelease(attrs); return NULL; }
    CFDataRef data = CFDataCreate(NULL, bits.p, (CFIndex)bits.n);
    SecKeyRef key = SecKeyCreateWithData(data, attrs, NULL);
    CFRelease(data); CFRelease(attrs);
    *err = key ? 0 : NTE_BAD_PUBLIC_KEY;
    return key;
}

#define CRYPT_VERIFY_CERT_SIGN_SUBJECT_BLOB 1
#define CRYPT_VERIFY_CERT_SIGN_SUBJECT_CERT 2
#define CRYPT_VERIFY_CERT_SIGN_SUBJECT_CRL 3
#define CRYPT_VERIFY_CERT_SIGN_ISSUER_PUBKEY 1
#define CRYPT_VERIFY_CERT_SIGN_ISSUER_CERT 2

// 0 only when macOS verified the subject's signature with the issuer's key.
static DWORD verify_signature(DWORD subject_type, const void *subject, DWORD issuer_type, const void *issuer) {
    span signed_data;
    if (!subject) return E_INVALIDARG;
    if (subject_type == CRYPT_VERIFY_CERT_SIGN_SUBJECT_BLOB) {
        const blob_t *b = subject;
        signed_data = (span){ host(b->pbData), b->cbData };
    } else if (subject_type == CRYPT_VERIFY_CERT_SIGN_SUBJECT_CERT || subject_type == CRYPT_VERIFY_CERT_SIGN_SUBJECT_CRL) {
        const cert_context *x = subject;                            // (a CRL_CONTEXT starts the same way)
        signed_data = (span){ host(x->pbCertEncoded), x->cbCertEncoded };
    } else return E_INVALIDARG;                                      // (OCSP responses are not read here)
    if (!signed_data.p) return E_INVALIDARG;
    // SEQUENCE { toBeSigned, AlgorithmIdentifier, BIT STRING }: certificates, CRLs and requests alike
    tlv whole, tbs, sig, aw;
    span alg_oid, alg_params;
    TRY(der_expect(&signed_data, 0x30, &whole));
    span w = whole.body;
    TRY(der_expect(&w, 0x30, &tbs));
    TRY(parse_alg(&w, &aw, &alg_oid, &alg_params));
    TRY(der_expect(&w, 0x03, &sig));
    if (!sig.body.n || sig.body.p[0]) return CRYPT_E_ASN1_CORRUPT;
    if (subject_type == CRYPT_VERIFY_CERT_SIGN_SUBJECT_CERT) {      // the signature field inside names the same algorithm
        span t = tbs.body; tlv x, inner;                             //  (RFC 5280 4.1.1.2)
        TRY(der_next(&t, &x));
        if (x.tag == 0xa0) TRY(der_next(&t, &x));
        TRY(der_expect(&t, 0x30, &inner));
        if (inner.whole.n != aw.whole.n || memcmp(inner.whole.p, aw.whole.p, aw.whole.n)) return NTE_BAD_SIGNATURE;
    }
    int is_ec_sig = 0, is_ec_key = 0;
    SecKeyAlgorithm alg = signature_algorithm(alg_oid, alg_params, &is_ec_sig);
    if (!alg) return NTE_BAD_ALGID;
    // The issuer's key: a CERT_PUBLIC_KEY_INFO or a certificate's. A chain, or no issuer at all (a bare hash in place of
    // a signature), is not verified here.
    char key_oid[128];
    span key_params, key_bits;
    DWORD unused, err;
    cert *temp = NULL, *ic = NULL;
    if (issuer_type == CRYPT_VERIFY_CERT_SIGN_ISSUER_PUBKEY && issuer) {
        const pubkey_info *k = issuer;
        const char *o = host(k->Algorithm.pszObjId);
        if (!o || strlen(o) >= sizeof key_oid) return E_INVALIDARG;
        strcpy(key_oid, o);
        key_params = (span){ host(k->Algorithm.Parameters.pbData), k->Algorithm.Parameters.cbData };
        key_bits = (span){ host(k->PublicKey.pbData), k->PublicKey.cbData };
        unused = k->PublicKey.cUnusedBits;
        if (!key_bits.p) return E_INVALIDARG;
    } else if (issuer_type == CRYPT_VERIFY_CERT_SIGN_ISSUER_CERT && issuer && (ic = cert_for_call(issuer, &temp))) {
        if (oid_text(ic->key_alg, key_oid, sizeof key_oid)) { if (temp) cert_free_memory(temp); return CRYPT_E_ASN1_CORRUPT; }
        key_params = ic->key_params; key_bits = ic->key_bits;
        unused = ic->info.SubjectPublicKeyInfo.PublicKey.cUnusedBits;
    } else return E_INVALIDARG;
    SecKeyRef key = public_key(key_oid, key_params, key_bits, unused, &is_ec_key, &err);
    if (temp) cert_free_memory(temp);
    if (!key) return err;
    Boolean ok = 0;
    if (is_ec_key == is_ec_sig && SecKeyIsAlgorithmSupported(key, kSecKeyOperationTypeVerify, alg)) {
        CFDataRef msg = CFDataCreate(NULL, tbs.whole.p, (CFIndex)tbs.whole.n);
        CFDataRef sg = CFDataCreate(NULL, sig.body.p + 1, (CFIndex)sig.body.n - 1);
        ok = SecKeyVerifySignature(key, alg, msg, sg, NULL);
        CFRelease(msg); CFRelease(sg);
        err = ok ? 0 : NTE_BAD_SIGNATURE;
    } else err = NTE_BAD_ALGID;
    CFRelease(key);
    return ok ? 0 : err;
}

static WINAPI BOOL CryptVerifyCertificateSignatureEx(uintptr_t prov, DWORD encoding, DWORD subject_type, void *subject, DWORD issuer_type,
                                                     void *issuer, DWORD flags, void *extra) {
    (void)prov; (void)flags; (void)extra;
    if ((encoding & 0xffff) != X509_ASN_ENCODING) { vx_set_last_error(ERROR_FILE_NOT_FOUND); return 0; }   // (as documented)
    DWORD err = verify_signature(subject_type, subject, issuer_type, issuer);
    if (err) { vx_set_last_error(err); return 0; }
    return 1;
}
static WINAPI BOOL CryptVerifyCertificateSignature(uintptr_t prov, DWORD encoding, const uint8_t *data, DWORD n, void *key) {
    blob_t b = { n, (uint8_t *)data };
    DWORD err = (encoding & 0xffff) != X509_ASN_ENCODING ? ERROR_FILE_NOT_FOUND
              : verify_signature(CRYPT_VERIFY_CERT_SIGN_SUBJECT_BLOB, &b, CRYPT_VERIFY_CERT_SIGN_ISSUER_PUBKEY, key);
    (void)prov;
    if (err) { vx_set_last_error(err); return 0; }
    return 1;
}

// ---- chains: built and judged by macOS (SecTrust) -------------------------------------------------------------------
// CertGetCertificateChain hands the end certificate and the certificates it may use (the additional store, the
// certificate's own store, an engine's stores) to SecTrust with the basic X.509 policy, at the time asked for. The
// chain's error bits come only from evaluations macOS did:
//   - a chain macOS trusts (to the system's roots, or to an engine's exclusive roots) has no error;
//   - otherwise the same certificates are evaluated again with the chain's own last certificate as the only anchor:
//     if macOS accepts that, the one problem is the root (CERT_TRUST_IS_UNTRUSTED_ROOT, or IS_PARTIAL_CHAIN when the
//     last certificate is not self-issued); if not, the signatures are not known good either (IS_NOT_SIGNATURE_VALID,
//     which no policy flag ignores, with UNTRUSTED_ROOT).
//   - validity times are compared here; when one is off, the evaluations above use a time inside every validity
//     period, so the rest is still checked.
// Revocation, when asked for, is macOS's check (SecPolicyCreateRevocation); not checked is "status unknown, offline".
// CertVerifyCertificateChainPolicy reports these bits as Windows' errors; its SSL policy is one more SecTrust
// evaluation with macOS's SSL policy and the server name. A chain context not made here is never reported trusted.
typedef struct { DWORD err, info; } trust_status;
typedef struct { DWORD cbSize; void *pCertContext; trust_status TrustStatus; void *pRevocationInfo, *pIssuanceUsage, *pApplicationUsage;
                 const WCHAR *pwszExtendedErrorInfo; } chain_element;
typedef struct { DWORD cbSize; trust_status TrustStatus; DWORD cElement; chain_element **rgpElement; void *pTrustListInfo;
                 BOOL fHasRevocationFreshnessTime; DWORD dwRevocationFreshnessTime; } simple_chain;
typedef struct { DWORD cbSize; trust_status TrustStatus; DWORD cChain; simple_chain **rgpChain; DWORD cLowerQualityChainContext;
                 void **rgpLowerQualityChainContext; BOOL fHasRevocationFreshnessTime; DWORD dwRevocationFreshnessTime; DWORD dwCreateFlags;
                 uint8_t ChainId[16]; } chain_context;
_Static_assert(sizeof(chain_element) == 56 && __builtin_offsetof(chain_element, TrustStatus) == 16, "CERT_CHAIN_ELEMENT");
_Static_assert(sizeof(simple_chain) == 40 && __builtin_offsetof(simple_chain, rgpElement) == 16, "CERT_SIMPLE_CHAIN");
_Static_assert(sizeof(chain_context) == 72 && __builtin_offsetof(chain_context, rgpChain) == 16 &&
               __builtin_offsetof(chain_context, ChainId) == 52, "CERT_CHAIN_CONTEXT");

#define TRUST_NOT_TIME_VALID 0x1
#define TRUST_REVOKED 0x4
#define TRUST_NOT_SIGNATURE_VALID 0x8
#define TRUST_NOT_VALID_FOR_USAGE 0x10
#define TRUST_UNTRUSTED_ROOT 0x20
#define TRUST_REVOCATION_UNKNOWN 0x40
#define TRUST_PARTIAL_CHAIN 0x10000
#define TRUST_OFFLINE_REVOCATION 0x1000000
#define TRUST_INFO_NAME_MATCH_ISSUER 0x4
#define TRUST_INFO_SELF_SIGNED 0x8
#define TRUST_INFO_PREFERRED_ISSUER 0x100
#define TRUST_INFO_FROM_EXCLUSIVE 0x2000
#define CERT_CHAIN_CACHE_ONLY_URL_RETRIEVAL 0x4
#define CERT_CHAIN_DISABLE_AIA 0x2000
#define CERT_CHAIN_REVOCATION_FLAGS 0x70000000u

// A chain engine: whose roots it trusts, and which stores it adds.
#define ENGINE_MAGIC 0x56584345u                                     // 'VXCE'
typedef struct {
    uint32_t magic;
    CFArrayRef exclusive, restricted;       // anchors only (hExclusiveRoot); and anchors that must also be system-trusted
    int no_fetch;                           // hRestrictedOther: only the certificates given, nothing fetched
    store **stores; int nstores;
} engine;
#define CHAIN_MAGIC 0x56584348u                                      // 'VXCH'
typedef struct {
    chain_context pub;                      // first: what the program holds
    uint32_t magic;
    int refs;
    simple_chain sc;
    simple_chain *psc;
    chain_element *el, **pel;
    int n;
    cert **certs;                           // each element's certificate (a reference held)
    store *own;                             // the store of the certificates made for the chain
    CFArrayRef sec_certs;                   // the chain as macOS built it, end certificate first
    CFArrayRef anchors; int anchors_only;   // what its trust was judged against (for the SSL policy)
    CFDateRef date, relaxed;                // the time asked for; one inside every validity period (or NULL)
} chain;

static CFDateRef cf_date(uint64_t ft) {
    return CFDateCreate(NULL, (double)(int64_t)(ft - 116444736000000000ull) / 1e7 - kCFAbsoluteTimeIntervalSince1970);
}

// One evaluation: 0 when macOS trusts the certificates (end certificate first) under the policies at the date, else
// its error. *built receives the chain it built; *revocation_checked whether revocation status was established.
static OSStatus evaluate(CFArrayRef certs, CFTypeRef policies, CFDateRef date, CFArrayRef anchors, int anchors_only, int fetch,
                         CFArrayRef *built, int *revocation_checked) {
    SecTrustRef trust = NULL;
    if (built) *built = NULL;
    if (SecTrustCreateWithCertificates(certs, policies, &trust) != errSecSuccess || !trust) return errSecInternalComponent;
    if (date) SecTrustSetVerifyDate(trust, date);
    SecTrustSetNetworkFetchAllowed(trust, fetch);
    if (anchors) { SecTrustSetAnchorCertificates(trust, anchors); SecTrustSetAnchorCertificatesOnly(trust, anchors_only); }
    CFErrorRef error = NULL;
    OSStatus st = 0;
    if (!SecTrustEvaluateWithError(trust, &error)) {
        st = error ? (OSStatus)CFErrorGetCode(error) : errSecNotTrusted;
        if (!st) st = errSecNotTrusted;                              // (never 0 for a failed evaluation)
    }
    if (error) CFRelease(error);
    if (built) *built = SecTrustCopyCertificateChain(trust);
    if (revocation_checked) {
        CFDictionaryRef r = SecTrustCopyResult(trust);
        CFTypeRef v = r ? CFDictionaryGetValue(r, kSecTrustRevocationChecked) : NULL;
        *revocation_checked = v && CFGetTypeID(v) == CFBooleanGetTypeID() && CFBooleanGetValue(v);
        if (r) CFRelease(r);
    }
    CFRelease(trust);
    return st;
}

static SecCertificateRef sec_cert(const uint8_t *der, size_t n) {
    CFDataRef d = CFDataCreate(NULL, der, (CFIndex)n);
    SecCertificateRef c = d ? SecCertificateCreateWithData(NULL, d) : NULL;
    if (d) CFRelease(d);
    return c;
}
// The certificates of a store (and of its siblings) as SecCertificateRefs, appended to out; with held, each also kept
// (a reference taken) in it. Under g_lock.
static void append_store(CFMutableArrayRef out, store *s, cert_list *held) {
    cert_list l = { 0 };
    if (s && !flatten(s, &l, 0))
        for (int i = 0; i < l.n; i++) {
            SecCertificateRef c = sec_cert(l.v[i]->der, l.v[i]->der_n);
            if (c) { CFArrayAppendValue(out, c); CFRelease(c); }
            if (held && !list_push(held, l.v[i])) cert_addref(l.v[i]);
        }
    free(l.v);
}
static CFArrayRef store_anchors(void *h) {                            // (under g_lock)
    store *s = store_of(h);
    if (!s) return NULL;
    CFMutableArrayRef a = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    append_store(a, s, NULL);
    return a;
}

static WINAPI BOOL CertCreateCertificateChainEngine(const uint8_t *config, void **out) {
    if (!out || !config) { vx_set_last_error(E_INVALIDARG); return 0; }
    *out = NULL;
    DWORD size = *(const DWORD *)config;
    engine *e = vx_heap_zalloc(sizeof *e);
    if (!e) { vx_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    pthread_mutex_lock(&g_lock);
    void *restricted_root = host(*(void *const *)(config + 8)), *restricted_other = host(*(void *const *)(config + 24));
    DWORD nadd = *(const DWORD *)(config + 32);
    void *const *add = host(*(void *const *const *)(config + 40));
    void *exclusive = size >= 72 ? host(*(void *const *)(config + 64)) : NULL;
    e->magic = ENGINE_MAGIC;
    if (exclusive) e->exclusive = store_anchors(exclusive);
    if (restricted_root) e->restricted = store_anchors(restricted_root);
    e->no_fetch = restricted_other != NULL;
    if ((e->stores = calloc((size_t)nadd + 2, sizeof *e->stores))) {  // (the root stores supply issuers too)
        for (DWORD i = 0; add && i < nadd; i++) { store *s = store_of(host(add[i])); if (s) { s->refs++; e->stores[e->nstores++] = s; } }
        store *x = store_of(exclusive), *r = store_of(restricted_root);
        if (x) { x->refs++; e->stores[e->nstores++] = x; }
        if (r) { r->refs++; e->stores[e->nstores++] = r; }
    }
    pthread_mutex_unlock(&g_lock);
    *out = PROG_PTR(e);
    return 1;
}
static WINAPI void CertFreeCertificateChainEngine(void *h) {
    engine *e = h;
    if (!e || (uintptr_t)h <= 2 || e->magic != ENGINE_MAGIC) return;    // (HCCE_CURRENT_USER/LOCAL_MACHINE are no objects)
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < e->nstores; i++) store_release(e->stores[i]);
    pthread_mutex_unlock(&g_lock);
    if (e->exclusive) CFRelease(e->exclusive);
    if (e->restricted) CFRelease(e->restricted);
    free(e->stores);
    e->magic = 0;
    free(e);
}

static void chain_free(chain *ch) {                                   // (under g_lock)
    for (int i = 0; i < ch->n; i++) if (ch->certs && ch->certs[i]) cert_release(ch->certs[i]);
    if (ch->own) store_release(ch->own);
    if (ch->sec_certs) CFRelease(ch->sec_certs);
    if (ch->anchors) CFRelease(ch->anchors);
    if (ch->date) CFRelease(ch->date);
    if (ch->relaxed) CFRelease(ch->relaxed);
    free(ch->certs); free(ch->el); free(ch->pel);
    ch->magic = 0;
    free(ch);
}
static WINAPI void CertFreeCertificateChain(const void *p) {
    chain *ch = (chain *)p;
    if (!ch || ch->magic != CHAIN_MAGIC) return;
    pthread_mutex_lock(&g_lock);
    if (--ch->refs <= 0) chain_free(ch);
    pthread_mutex_unlock(&g_lock);
}
static WINAPI void *CertDuplicateCertificateChain(const void *p) {
    chain *ch = (chain *)p;
    if (!ch || ch->magic != CHAIN_MAGIC) { vx_set_last_error(E_INVALIDARG); return NULL; }
    pthread_mutex_lock(&g_lock);
    ch->refs++;
    pthread_mutex_unlock(&g_lock);
    return (void *)p;
}

// Does the end certificate's extended key usage allow what the caller asks for (CERT_USAGE_MATCH)? No extension: any.
static int usage_allowed(const cert *c, DWORD match_type, DWORD count, char *const *oids) {
    const ext *e = find_ext(c, "2.5.29.37");
    if (!e || !count || !oids) return 1;
    int found_all = 1, found_any = 0;
    for (DWORD i = 0; i < count; i++) {
        const char *want = host(oids[i]);
        int found = 0;
        span v = e->value; tlv seq, o; char text[128];
        if (!want || der_expect(&v, 0x30, &seq)) return 0;
        for (span s = seq.body; s.n && !found;)
            found = !der_expect(&s, 0x06, &o) && !oid_text(o.body, text, sizeof text) && (!strcmp(text, want) || !strcmp(text, "2.5.29.37.0"));
        found_all &= found; found_any |= found;
    }
    return match_type == 1 ? found_any : found_all;                  // USAGE_MATCH_TYPE_OR : _AND
}

static WINAPI BOOL CertGetCertificateChain(void *engine_h, const void *ctx, const filetime *when, void *additional, const uint8_t *para,
                                           DWORD flags, void *reserved, void **out) {
    (void)reserved;
    if (!out || !ctx) { vx_set_last_error(E_INVALIDARG); return 0; }
    *out = NULL;
    engine *eng = (uintptr_t)engine_h > 2 && ((engine *)engine_h)->magic == ENGINE_MAGIC ? engine_h : NULL;
    uint64_t at = when ? (uint64_t)when->hi << 32 | when->lo : now_filetime();
    CFDateRef date = cf_date(at);

    // the end certificate (a context of ours) and every certificate the chain may use
    pthread_mutex_lock(&g_lock);
    cert *leaf = cert_of(ctx);
    store *own = store_new(STORE_MEMORY);
    if (!leaf && own) {                                              // (a context the program made: copied)
        const cert_context *x = ctx;
        if (x->pbCertEncoded && cert_new(own, host(x->pbCertEncoded), x->cbCertEncoded, &leaf)) leaf = NULL;
    }
    if (!leaf || !own) {
        if (own) store_release(own);
        pthread_mutex_unlock(&g_lock);
        CFRelease(date);
        vx_set_last_error(leaf ? ERROR_NOT_ENOUGH_MEMORY : CRYPT_E_ASN1_CORRUPT);
        return 0;
    }
    cert_addref(leaf);
    CFMutableArrayRef certs = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    SecCertificateRef first = sec_cert(leaf->der, leaf->der_n);
    if (first) { CFArrayAppendValue(certs, first); CFRelease(first); }
    cert_list candidates = { 0 };                                    // (the possible issuers, held until the end)
    append_store(certs, store_of(additional), &candidates);
    if (leaf->owner != own) append_store(certs, leaf->owner, &candidates);
    for (int i = 0; eng && i < eng->nstores; i++) append_store(certs, eng->stores[i], &candidates);
    pthread_mutex_unlock(&g_lock);

    DWORD usage_type = 0, usage_n = 0;
    char *const *usage = NULL;
    if (para && *(const DWORD *)para >= 32) {                         // CERT_CHAIN_PARA.RequestedUsage
        usage_type = *(const DWORD *)(para + 8);
        usage_n = *(const DWORD *)(para + 16);
        usage = host(*(char *const *const *)(para + 24));
    }
    int fetch = !(flags & (CERT_CHAIN_CACHE_ONLY_URL_RETRIEVAL | CERT_CHAIN_DISABLE_AIA)) && !(eng && eng->no_fetch);
    SecPolicyRef basic = SecPolicyCreateBasicX509();
    CFArrayRef anchors = eng && eng->exclusive ? eng->exclusive : NULL;
    int anchors_only = anchors != NULL;

    // 1. the chain macOS builds, and whether the time asked for is inside every certificate's validity
    CFArrayRef built = NULL;
    OSStatus full = first ? evaluate(certs, basic, date, anchors, anchors_only, fetch, &built, NULL) : errSecInternalComponent;
    CFIndex n = built ? CFArrayGetCount(built) : 0;
    uint64_t latest_start = 0, earliest_end = ~0ull;
    DWORD time_bad = 0;
    cert **made = calloc((size_t)(n ? n : 1), sizeof *made);
    pthread_mutex_lock(&g_lock);
    made[0] = leaf;
    for (CFIndex i = 1; i < n; i++) {
        CFDataRef d = SecCertificateCopyData((SecCertificateRef)CFArrayGetValueAtIndex(built, i));
        cert *c = NULL;
        if (d && !cert_new(own, CFDataGetBytePtr(d), (DWORD)CFDataGetLength(d), &c)) { cert_addref(c); made[i] = c; }
        if (d) CFRelease(d);
    }
    pthread_mutex_unlock(&g_lock);
    if (!n) n = 1;
    for (CFIndex i = 0; i < n; i++) {
        if (!made[i]) continue;
        if (made[i]->not_before > latest_start) latest_start = made[i]->not_before;
        if (made[i]->not_after < earliest_end) earliest_end = made[i]->not_after;
        if (at < made[i]->not_before || at > made[i]->not_after) time_bad = 1;
    }
    CFDateRef relaxed = time_bad && latest_start <= earliest_end ? cf_date(latest_start) : NULL;

    // 2. the error bits, each from an evaluation macOS did (see above)
    DWORD err = 0;
    int root_untrusted = 0, partial = 0;
    const cert *last = made[n - 1];
    int last_self_issued = last && last->subject.n == last->issuer.n && !memcmp(last->subject.p, last->issuer.p, last->subject.n);
    CFDateRef eval_date = time_bad ? relaxed : date;
    OSStatus e1 = !eval_date ? errSecCertificateExpired : !time_bad ? full : evaluate(built ? built : certs, basic, eval_date, anchors, anchors_only, 0, NULL, NULL);
    if (!e1 && eng && eng->restricted)                                 // a restricted root must also be one of the engine's
        e1 = evaluate(built ? built : certs, basic, eval_date, eng->restricted, 1, 0, NULL, NULL);
    if (e1) {
        OSStatus e2 = errSecNotTrusted;
        if (eval_date && built && n) {
            const void *root = CFArrayGetValueAtIndex(built, n - 1);
            CFArrayRef self = CFArrayCreate(NULL, &root, 1, &kCFTypeArrayCallBacks);
            e2 = evaluate(built, basic, eval_date, self, 1, 0, NULL, NULL);
            CFRelease(self);
        }
        if (!e2) { if (last_self_issued) root_untrusted = 1; else partial = 1; }
        else err |= TRUST_NOT_SIGNATURE_VALID | TRUST_UNTRUSTED_ROOT;
        if (e1 == errSecInvalidExtendedKeyUsage || e1 == errSecInvalidKeyUsageForPolicy) err |= TRUST_NOT_VALID_FOR_USAGE;
        if (e1 == errSecCertificateRevoked) err |= TRUST_REVOKED;
    }
    if (root_untrusted) err |= TRUST_UNTRUSTED_ROOT;
    if (partial) err |= TRUST_PARTIAL_CHAIN;
    // An anchor is taken as it is: when the last certificate was not one of macOS's own roots, its signature is checked
    // here (SecKey) — its own key when it is self-issued, else the key of any certificate given that names its issuer
    // (macOS did not link to it: a signature that does not verify is reported as such, not as a missing issuer).
    if (e1 && last && !(err & TRUST_NOT_SIGNATURE_VALID)) {
        if (last_self_issued) {
            if (verify_signature(CRYPT_VERIFY_CERT_SIGN_SUBJECT_CERT, &last->ctx, CRYPT_VERIFY_CERT_SIGN_ISSUER_CERT, &last->ctx))
                err |= TRUST_NOT_SIGNATURE_VALID;
        } else {
            int verified = 0, refused = 0;
            for (int i = 0; i < candidates.n; i++) {
                const cert *c = candidates.v[i];
                if (c->subject.n != last->issuer.n || memcmp(c->subject.p, last->issuer.p, c->subject.n)) continue;
                DWORD v = verify_signature(CRYPT_VERIFY_CERT_SIGN_SUBJECT_CERT, &last->ctx, CRYPT_VERIFY_CERT_SIGN_ISSUER_CERT, &c->ctx);
                if (!v) verified = 1; else refused = 1;
            }
            if (refused && !verified) err |= TRUST_NOT_SIGNATURE_VALID;
        }
    }
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < candidates.n; i++) cert_release(candidates.v[i]);
    pthread_mutex_unlock(&g_lock);
    free(candidates.v);
    if (time_bad) err |= TRUST_NOT_TIME_VALID;
    if (!usage_allowed(leaf, usage_type, usage_n, usage)) err |= TRUST_NOT_VALID_FOR_USAGE;
    if (flags & CERT_CHAIN_REVOCATION_FLAGS) {                        // revocation: macOS's check, or "unknown"
        SecPolicyRef rev = SecPolicyCreateRevocation(kSecRevocationUseAnyAvailableMethod);
        const void *both[2] = { basic, rev };
        CFArrayRef policies = CFArrayCreate(NULL, both, 2, &kCFTypeArrayCallBacks);
        int checked = 0;
        OSStatus r = evaluate(built ? built : certs, policies, eval_date ? eval_date : date, anchors, anchors_only, fetch, NULL, &checked);
        if (r == errSecCertificateRevoked) err |= TRUST_REVOKED;
        else if (!checked) err |= TRUST_REVOCATION_UNKNOWN | TRUST_OFFLINE_REVOCATION;
        CFRelease(policies); CFRelease(rev);
    }
    CFRelease(basic);
    CFRelease(certs);

    // 3. the chain context
    chain *ch = vx_heap_zalloc(sizeof *ch);
    chain_element *el = ch ? vx_heap_zalloc((size_t)n * sizeof *el) : NULL;
    chain_element **pel = el ? vx_heap_zalloc((size_t)n * sizeof *pel) : NULL;
    if (!ch || !el || !pel) {
        free(ch); free(el); free(pel);
        pthread_mutex_lock(&g_lock);
        for (CFIndex i = 0; i < n; i++) if (made[i]) cert_release(made[i]);
        store_release(own);
        pthread_mutex_unlock(&g_lock);
        free(made);
        if (built) CFRelease(built);
        if (relaxed) CFRelease(relaxed);
        CFRelease(date);
        vx_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    int m = 0;                                                        // (elements macOS built that parse here)
    for (CFIndex i = 0; i < n; i++) {
        cert *c = made[i];
        if (!c) continue;
        trust_status s = { 0, 0 };
        if (at < c->not_before || at > c->not_after) s.err |= TRUST_NOT_TIME_VALID;
        if (c->subject.n == c->issuer.n && !memcmp(c->subject.p, c->issuer.p, c->subject.n)) s.info |= TRUST_INFO_SELF_SIGNED;
        if (i + 1 < n && made[i + 1] && made[i + 1]->subject.n == c->issuer.n && !memcmp(made[i + 1]->subject.p, c->issuer.p, c->issuer.n))
            s.info |= TRUST_INFO_NAME_MATCH_ISSUER;
        if (i == 0) s.err |= err & (TRUST_NOT_VALID_FOR_USAGE | TRUST_REVOKED | TRUST_NOT_SIGNATURE_VALID | TRUST_REVOCATION_UNKNOWN | TRUST_OFFLINE_REVOCATION);
        if (i == n - 1) {
            s.err |= err & (TRUST_UNTRUSTED_ROOT | TRUST_PARTIAL_CHAIN);
            if (anchors_only && !e1) s.info |= TRUST_INFO_FROM_EXCLUSIVE;
        }
        el[m] = (chain_element){ sizeof(chain_element), PROG_PTR(&c->ctx), s, NULL, NULL, NULL, NULL };
        pel[m] = PROG_PTR(&el[m]);
        made[m++] = c;
    }
    ch->magic = CHAIN_MAGIC; ch->refs = 1;
    ch->el = el; ch->pel = pel; ch->n = m; ch->certs = made; ch->own = own;
    ch->sec_certs = built; ch->date = date; ch->relaxed = relaxed;
    if (anchors) { CFRetain(anchors); ch->anchors = anchors; ch->anchors_only = 1; }
    DWORD info = err & (TRUST_UNTRUSTED_ROOT | TRUST_PARTIAL_CHAIN) ? 0 : TRUST_INFO_PREFERRED_ISSUER;
    ch->sc = (simple_chain){ sizeof(simple_chain), { err, info }, (DWORD)m, PROG_PTR(pel), NULL, 0, 0 };
    ch->psc = PROG_PTR(&ch->sc);
    ch->pub = (chain_context){ sizeof(chain_context), { err, info }, 1, PROG_PTR(&ch->psc), 0, NULL, 0, 0, 0, { 0 } };
    arc4random_buf(ch->pub.ChainId, sizeof ch->pub.ChainId);
    *out = PROG_PTR(&ch->pub);
    return 1;
}

// ---- chain policies -----------------------------------------------------------------------------------------------
#define CERT_E_EXPIRED 0x800B0101u
#define CERT_E_UNTRUSTEDROOT 0x800B0109u
#define CERT_E_CHAINING 0x800B010Au
#define CERT_E_CN_NO_MATCH 0x800B010Fu
#define CERT_E_WRONG_USAGE 0x800B0110u
#define TRUST_E_CERT_SIGNATURE 0x80096004u
#define CRYPT_E_REVOKED 0x80092010u
#define CRYPT_E_REVOCATION_OFFLINE 0x80092013u
#define POLICY_IGNORE_NOT_TIME_VALID 0x1
#define POLICY_ALLOW_UNKNOWN_CA 0x10
#define POLICY_IGNORE_WRONG_USAGE 0x20
#define POLICY_IGNORE_INVALID_NAME 0x40
#define POLICY_IGNORE_ALL_REV_UNKNOWN 0xf00
#define SSL_IGNORE_REVOCATION 0x80
#define SSL_IGNORE_UNKNOWN_CA 0x100
#define SSL_IGNORE_WRONG_USAGE 0x200
#define SSL_IGNORE_CN_INVALID 0x1000
#define SSL_IGNORE_DATE_INVALID 0x2000
typedef struct { DWORD cbSize, dwError; LONG lChainIndex, lElementIndex; void *pvExtraPolicyStatus; } policy_status;
_Static_assert(sizeof(policy_status) == 24, "CERT_CHAIN_POLICY_STATUS");

// The first error of the chain that the flags do not excuse, the ones no flag excuses first.
static DWORD base_error(DWORD bits, DWORD ignore) {
    if (bits & TRUST_NOT_SIGNATURE_VALID) return TRUST_E_CERT_SIGNATURE;
    if (bits & TRUST_REVOKED) return CRYPT_E_REVOKED;
    if ((bits & TRUST_UNTRUSTED_ROOT) && !(ignore & POLICY_ALLOW_UNKNOWN_CA)) return CERT_E_UNTRUSTEDROOT;
    if (bits & TRUST_PARTIAL_CHAIN) return CERT_E_CHAINING;          // (nothing above the last certificate was checked)
    if ((bits & TRUST_NOT_TIME_VALID) && !(ignore & POLICY_IGNORE_NOT_TIME_VALID)) return CERT_E_EXPIRED;
    if ((bits & TRUST_NOT_VALID_FOR_USAGE) && !(ignore & POLICY_IGNORE_WRONG_USAGE)) return CERT_E_WRONG_USAGE;
    if ((bits & (TRUST_REVOCATION_UNKNOWN | TRUST_OFFLINE_REVOCATION)) && (ignore & POLICY_IGNORE_ALL_REV_UNKNOWN) != POLICY_IGNORE_ALL_REV_UNKNOWN)
        return CRYPT_E_REVOCATION_OFFLINE;
    bits &= ~(DWORD)(TRUST_UNTRUSTED_ROOT | TRUST_PARTIAL_CHAIN | TRUST_NOT_TIME_VALID | TRUST_NOT_VALID_FOR_USAGE | TRUST_REVOCATION_UNKNOWN |
                     TRUST_OFFLINE_REVOCATION);
    return bits ? CERT_E_UNTRUSTEDROOT : 0;                           // (any other error: not trusted)
}

static WINAPI BOOL CertVerifyCertificateChainPolicy(const char *policy, const void *chain_p, const uint8_t *para, policy_status *status) {
    if (!status) { vx_set_last_error(E_INVALIDARG); return 0; }
    status->dwError = CERT_E_UNTRUSTEDROOT;                           // (until shown otherwise)
    status->lChainIndex = status->lElementIndex = -1;
    chain *ch = (chain *)chain_p;
    if (!ch || ch->magic != CHAIN_MAGIC) return 1;                    // not a chain macOS judged: not trusted
    uintptr_t id = (uintptr_t)policy;
    DWORD ignore = para ? *(const DWORD *)(para + 4) : 0, bits = ch->pub.TrustStatus.err;
    const uint8_t *ssl = para ? host(*(void *const *)(para + 8)) : NULL;
    DWORD ssl_checks = id == 4 && ssl ? *(const DWORD *)(ssl + 8) : 0;
    if (ssl_checks & SSL_IGNORE_UNKNOWN_CA) ignore |= POLICY_ALLOW_UNKNOWN_CA;
    if (ssl_checks & SSL_IGNORE_DATE_INVALID) ignore |= POLICY_IGNORE_NOT_TIME_VALID;
    if (ssl_checks & SSL_IGNORE_WRONG_USAGE) ignore |= POLICY_IGNORE_WRONG_USAGE;
    if (ssl_checks & SSL_IGNORE_REVOCATION) ignore |= POLICY_IGNORE_ALL_REV_UNKNOWN;
    if (ssl_checks & SSL_IGNORE_CN_INVALID) ignore |= POLICY_IGNORE_INVALID_NAME;
    DWORD e = base_error(bits, ignore);
    if (!e && id == 4) {                                              // SSL: macOS's SSL policy, with the server's name
        int server = !ssl || *(const DWORD *)(ssl + 4) != 1;          // (AUTHTYPE_CLIENT is 1)
        const WCHAR *name = ssl && !(ignore & POLICY_IGNORE_INVALID_NAME) ? host(*(const WCHAR *const *)(ssl + 16)) : NULL;
        char *utf8 = name ? vx_utf8_dup(name) : NULL;
        CFStringRef host_name = utf8 ? CFStringCreateWithCString(NULL, utf8, kCFStringEncodingUTF8) : NULL;
        free(utf8);
        SecPolicyRef p = SecPolicyCreateSSL(server, host_name);
        // judged as the chain was, with the excused problems taken out: an untrusted root as its own anchor, a time
        // outside the validity replaced by one inside it
        CFArrayRef anchors = ch->anchors;
        int only = ch->anchors_only;
        CFArrayRef self = NULL;
        CFIndex n = ch->sec_certs ? CFArrayGetCount(ch->sec_certs) : 0;
        if ((bits & TRUST_UNTRUSTED_ROOT) && n) {
            const void *last = CFArrayGetValueAtIndex(ch->sec_certs, n - 1);
            anchors = self = CFArrayCreate(NULL, &last, 1, &kCFTypeArrayCallBacks);
            only = 1;
        }
        CFDateRef date = bits & TRUST_NOT_TIME_VALID ? ch->relaxed : ch->date;
        OSStatus r = n && date ? evaluate(ch->sec_certs, p, date, anchors, only, 0, NULL, NULL) : errSecNotTrusted;
        if (self) CFRelease(self);
        CFRelease(p);
        if (host_name) CFRelease(host_name);
        e = !r ? 0 : r == errSecHostNameMismatch ? CERT_E_CN_NO_MATCH : CERT_E_WRONG_USAGE;   // (macOS's TLS rules: a usage)
        if (e == CERT_E_WRONG_USAGE && (ignore & POLICY_IGNORE_WRONG_USAGE)) e = 0;   // (the base checks above held)
    } else if (!e && id != 1 && id != 5) e = CERT_E_UNTRUSTEDROOT;    // Authenticode, NT auth, Microsoft root, EV: not here
    status->dwError = e;
    if (e) { status->lChainIndex = 0; status->lElementIndex = e == CERT_E_UNTRUSTEDROOT || e == CERT_E_CHAINING ? (LONG)ch->n - 1 : 0; }
    return 1;
}

// ---- private keys: none here ------------------------------------------------------------------------------------
static WINAPI BOOL CryptAcquireCertificatePrivateKey(const void *ctx, DWORD flags, void *para, uintptr_t *key, DWORD *spec, BOOL *caller_frees) {
    (void)ctx; (void)flags; (void)para;
    if (key) *key = 0;
    if (spec) *spec = 0;
    if (caller_frees) *caller_frees = 0;
    vx_set_last_error(CRYPT_E_NO_KEY_PROPERTY);                     // no certificate here has a private key
    return 0;
}
// The certificates it looks for need a private key: none has one here, so no chain is ever found.
static WINAPI void *CertFindChainInStore(void *h, DWORD encoding, DWORD flags, DWORD type, const void *para, const void *prev) {
    (void)h; (void)encoding; (void)flags; (void)type; (void)para;
    if (prev) CertFreeCertificateChain(prev);                        // always freed, even on error
    vx_set_last_error(CRYPT_E_NOT_FOUND);
    return NULL;
}
// No message handles are made here (CryptQueryObject finds no messages): closing nothing succeeds, anything else is not ours.
static WINAPI BOOL CryptMsgClose(void *msg) {
    if (!msg) return 1;
    vx_set_last_error(ERROR_INVALID_HANDLE);
    return 0;
}

// ---- wintrust ---------------------------------------------------------------------------------------------------
typedef struct { DWORD cbStruct; const WCHAR *pcwszFilePath; HANDLE hFile; const void *pgKnownSubject; } wintrust_file_info;
typedef struct {
    DWORD cbStruct; void *pPolicyCallbackData, *pSIPClientData; DWORD dwUIChoice, fdwRevocationChecks, dwUnionChoice;
    void *pChoice; DWORD dwStateAction; HANDLE hWVTStateData; WCHAR *pwszURLReference; DWORD dwProvFlags, dwUIContext; void *pSignatureSettings;
} wintrust_data;
_Static_assert(__builtin_offsetof(wintrust_data, pChoice) == 40 && __builtin_offsetof(wintrust_data, hWVTStateData) == 56 &&
               sizeof(wintrust_data) == 88, "WINTRUST_DATA");
#define WTD_CHOICE_FILE 1
#define WTD_STATEACTION_CLOSE 2
#define WTD_STATEACTION_AUTO_CACHE_FLUSH 4

// The action GUIDs of Microsoft's trust providers (softpub.h); anything else has no provider here.
static const uint8_t trust_actions[][16] = {
    { 0x6b, 0xc5, 0xaa, 0x00, 0x44, 0xcd, 0xd0, 0x11, 0x8c, 0xc2, 0x00, 0xc0, 0x4f, 0xc2, 0x95, 0xee },   // GENERIC_VERIFY_V2
    { 0xc3, 0xe6, 0x50, 0xf7, 0xee, 0x38, 0xd1, 0x11, 0x85, 0xe5, 0x00, 0xc0, 0x4f, 0xc2, 0x95, 0xee },   // DRIVER_ACTION_VERIFY
    { 0xf8, 0x31, 0x3e, 0x57, 0xba, 0xaa, 0xd0, 0x11, 0x8c, 0xcb, 0x00, 0xc0, 0x4f, 0xc2, 0x95, 0xee },   // HTTPSPROV_ACTION
    { 0x16, 0x1c, 0x45, 0xfc, 0x75, 0xac, 0xd1, 0x11, 0xb4, 0xb8, 0x00, 0xc0, 0x4f, 0xb6, 0x6e, 0xa0 },   // GENERIC_CHAIN_VERIFY
    { 0x42, 0x38, 0x9a, 0x18, 0x41, 0x30, 0xd1, 0x11, 0x85, 0xe1, 0x00, 0xc0, 0x4f, 0xc2, 0x95, 0xee },   // GENERIC_CERT_VERIFY
    { 0xf8, 0x31, 0x3e, 0x57, 0xba, 0xdd, 0xd0, 0x11, 0x8c, 0xcb, 0x00, 0xc0, 0x4f, 0xc2, 0x95, 0xee },   // TRUSTPROVIDER_TEST
};

// What a file says about its own signature: 1 a PE with an attribute certificate table, 0 a PE without one, -1 not a
// PE (or unreadable). Nothing about whether the signature is good.
static int pe_signature_state(int fd) {
    uint8_t h[0x40], nt[24 + 240];
    if (pread(fd, h, sizeof h, 0) != (ssize_t)sizeof h || h[0] != 'M' || h[1] != 'Z') return -1;
    uint32_t at = (uint32_t)h[0x3c] | (uint32_t)h[0x3d] << 8 | (uint32_t)h[0x3e] << 16 | (uint32_t)h[0x3f] << 24;
    ssize_t got = pread(fd, nt, sizeof nt, at);
    if (got < 24 + 2 || memcmp(nt, "PE\0\0", 4)) return -1;
    uint16_t magic = (uint16_t)(nt[24] | nt[25] << 8);
    size_t dirs = magic == 0x10b ? 96 : magic == 0x20b ? 112 : 0;     // the data directories in the optional header
    if (!dirs || (size_t)got < 24 + dirs + 5 * 8) return -1;
    const uint8_t *count = nt + 24 + dirs - 4, *sec = nt + 24 + dirs + 4 * 8;   // NumberOfRvaAndSizes, entry 4 (security)
    uint32_t ndirs = (uint32_t)count[0] | (uint32_t)count[1] << 8 | (uint32_t)count[2] << 16 | (uint32_t)count[3] << 24;
    uint32_t size = (uint32_t)sec[4] | (uint32_t)sec[5] << 8 | (uint32_t)sec[6] << 16 | (uint32_t)sec[7] << 24;
    return ndirs > 4 && size ? 1 : 0;
}

static WINAPI LONG WinVerifyTrust(HANDLE hwnd, const uint8_t *action, wintrust_data *data) {
    (void)hwnd;
    if (!data || !action) return (LONG)E_INVALIDARG;
    if (data->dwStateAction == WTD_STATEACTION_CLOSE || data->dwStateAction == WTD_STATEACTION_AUTO_CACHE_FLUSH) {
        data->hWVTStateData = NULL;                                   // releasing: no state data is ever kept here
        return 0;
    }
    data->hWVTStateData = NULL;                                       // (a verification leaves none to inspect)
    int known = 0;
    for (size_t i = 0; i < sizeof trust_actions / sizeof trust_actions[0]; i++) if (!memcmp(action, trust_actions[i], 16)) known = 1;
    if (!known) return (LONG)TRUST_E_PROVIDER_UNKNOWN;
    if (data->dwUnionChoice != WTD_CHOICE_FILE) return (LONG)TRUST_E_SUBJECT_NOT_TRUSTED;   // catalogs, blobs, signers: unverified
    const wintrust_file_info *f = host(data->pChoice);
    if (!f) return (LONG)E_INVALIDARG;
    int fd = f->hFile && f->hFile != (HANDLE)(intptr_t)-1 ? vx_handle_fd(f->hFile) : -1, own = 0;
    if (fd < 0 && f->pcwszFilePath) {
        char *win = vx_utf8_dup(host(f->pcwszFilePath)), path[4096];
        if (win && vx_unix_path(win, path, sizeof path)) { fd = open(path, O_RDONLY | O_CLOEXEC); own = fd >= 0; }
        free(win);
    }
    int state = fd >= 0 ? pe_signature_state(fd) : -2;
    if (own) close(fd);
    // Only the absence of a signature is a fact checked here; a signature present is not verified, so not trusted.
    return (LONG)(state == 0 ? TRUST_E_NOSIGNATURE : state == 1 ? TRUST_E_SUBJECT_NOT_TRUSTED
                : state == -1 ? TRUST_E_SUBJECT_FORM_UNKNOWN : TRUST_E_SUBJECT_NOT_TRUSTED);
}

// No WinVerifyTrust state data is made here, so there is never provider data, a signer or its certificate to return.
static WINAPI void *WTHelperProvDataFromStateData(HANDLE state) { (void)state; vx_set_last_error(ERROR_INVALID_PARAMETER); return NULL; }
static WINAPI void *WTHelperGetProvSignerFromChain(void *prov, DWORD signer, BOOL counter, DWORD counter_index) {
    (void)prov; (void)signer; (void)counter; (void)counter_index;
    vx_set_last_error(ERROR_INVALID_PARAMETER);
    return NULL;
}
static WINAPI void *WTHelperGetProvCertFromChain(void *signer, DWORD index) { (void)signer; (void)index; vx_set_last_error(ERROR_INVALID_PARAMETER); return NULL; }

// Catalogs: the administrator context opens on an empty catalog database, so no catalog ever contains a file's hash.
#define CATADMIN_MAGIC 0x56584341u                                   // 'VXCA'
static WINAPI BOOL CryptCATAdminAcquireContext(void **admin, const void *subsystem, DWORD flags) {
    (void)subsystem; (void)flags;
    if (!admin) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    uint32_t *h = vx_heap_alloc(sizeof *h);
    if (!h) { *admin = NULL; vx_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    *h = CATADMIN_MAGIC;
    *admin = PROG_PTR(h);
    return 1;
}
static WINAPI BOOL CryptCATAdminReleaseContext(void *admin, DWORD flags) {
    (void)flags;
    uint32_t *h = admin;
    if (!h || *h != CATADMIN_MAGIC) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    *h = 0;
    free(h);
    return 1;
}
// Hashing for a catalog lookup (Authenticode's own hash of a PE) is not done: with no catalogs it would find nothing.
static WINAPI BOOL CryptCATAdminCalcHashFromFileHandle(HANDLE file, DWORD *size, uint8_t *hash, DWORD flags) {
    (void)file; (void)size; (void)hash; (void)flags;
    vx_set_last_error(ERROR_NOT_SUPPORTED);
    return 0;
}
static WINAPI void *CryptCATAdminEnumCatalogFromHash(void *admin, const uint8_t *hash, DWORD size, DWORD flags, void **prev) {
    (void)admin; (void)hash; (void)size; (void)flags; (void)prev;
    vx_set_last_error(ERROR_NOT_FOUND);
    return NULL;
}
static WINAPI BOOL CryptCATAdminReleaseCatalogContext(void *admin, void *info, DWORD flags) {
    (void)admin; (void)info; (void)flags;
    vx_set_last_error(ERROR_INVALID_PARAMETER);                      // (no catalog context is ever handed out)
    return 0;
}
static WINAPI BOOL CryptCATCatalogInfoFromContext(void *info, void *catalog, DWORD flags) {
    (void)info; (void)catalog; (void)flags;
    vx_set_last_error(ERROR_INVALID_PARAMETER);
    return 0;
}

static const vx_export crypt32[] = {
    VX_EXPORT(CertOpenStore), VX_EXPORT(CertOpenSystemStoreW), VX_EXPORT(CertOpenSystemStoreA), VX_EXPORT(CertDuplicateStore),
    VX_EXPORT(CertCloseStore), VX_EXPORT(CertAddStoreToCollection), VX_EXPORT(CertRemoveStoreFromCollection), VX_EXPORT(CertControlStore),
    VX_EXPORT(CertAddEncodedCertificateToStore), VX_EXPORT(CertAddCertificateContextToStore), VX_EXPORT(CertCreateCertificateContext),
    VX_EXPORT(CertFreeCertificateContext), VX_EXPORT(CertDuplicateCertificateContext), VX_EXPORT(CertDeleteCertificateFromStore),
    VX_EXPORT(CertEnumCertificatesInStore), VX_EXPORT(CertFindCertificateInStore), VX_EXPORT(CertGetNameStringW),
    VX_EXPORT(CertGetNameStringA), VX_EXPORT(CertNameToStrW), VX_EXPORT(CertNameToStrA), VX_EXPORT(CertCompareCertificateName),
    VX_EXPORT(CertCompareCertificate), VX_EXPORT(CertComparePublicKeyInfo), VX_EXPORT(CertVerifyTimeValidity), VX_EXPORT(CertFindExtension),
    VX_EXPORT(CertGetEnhancedKeyUsage), VX_EXPORT(CertGetIntendedKeyUsage), VX_EXPORT(CertGetCertificateContextProperty),
    VX_EXPORT(CryptAcquireCertificatePrivateKey), VX_EXPORT(CertFindChainInStore), VX_EXPORT(CryptMsgClose),
    VX_EXPORT(CryptVerifyCertificateSignatureEx), VX_EXPORT(CryptVerifyCertificateSignature), VX_EXPORT(CertGetCertificateChain),
    VX_EXPORT(CertFreeCertificateChain), VX_EXPORT(CertDuplicateCertificateChain), VX_EXPORT(CertVerifyCertificateChainPolicy),
    VX_EXPORT(CertCreateCertificateChainEngine), VX_EXPORT(CertFreeCertificateChainEngine),
};
static const vx_export wintrust[] = {
    VX_EXPORT(WinVerifyTrust), VX_EXPORT(WTHelperProvDataFromStateData), VX_EXPORT(WTHelperGetProvSignerFromChain),
    VX_EXPORT(WTHelperGetProvCertFromChain), VX_EXPORT(CryptCATAdminAcquireContext), VX_EXPORT(CryptCATAdminReleaseContext),
    VX_EXPORT(CryptCATAdminCalcHashFromFileHandle), VX_EXPORT(CryptCATAdminEnumCatalogFromHash),
    VX_EXPORT(CryptCATAdminReleaseCatalogContext), VX_EXPORT(CryptCATCatalogInfoFromContext),
};
void vx_crypt32_init(void) {
    vx_register_module("crypt32.dll", crypt32, sizeof crypt32 / sizeof crypt32[0]);
    vx_register_module("wintrust.dll", wintrust, sizeof wintrust / sizeof wintrust[0]);
}

#ifdef __aarch64__
#include "thunks/crypt32.c.inc"
#endif
