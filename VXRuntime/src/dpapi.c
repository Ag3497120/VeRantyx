// DPAPI (crypt32.dll): CryptProtectData / CryptUnprotectData keep data encrypted at rest (Chromium keeps the key of
// its cookie and password stores this way), CryptProtectMemory / CryptUnprotectMemory hide memory in place.
//
// Windows derives the key from the user's logon credentials. Here it is 32 random bytes made on first use in
// $VXRT_ROOT/dpapi/user.key (mode 0600 in a 0700 directory), so a blob opens only with the same VXRT_ROOT, as Windows'
// open only for the same user on the same machine. Any program running as this user can unprotect, as on Windows.
// At rest this is weaker than Windows, where the key itself is encrypted with the logon password: whoever can read the
// home folder offline (a copy of it, another administrator, a disk without FileVault) has the key and the blobs. So the
// directory is kept out of Time Machine backups (NSURLIsExcludedFromBackupKey; a backup then holds blobs without their
// key), and VXRuntime/README.md says so to the user. It is not in the Keychain: an item there trusts the code signature
// of the binary that made it, this runtime is rebuilt (and signed ad hoc) often, so each new binary would have to be
// allowed by the user again; and the tests would add items to the user's own keychain.
// Each blob gets its own AES-256-GCM key (aes_gcm.c): HKDF-SHA256 (RFC 5869) of that key
// with a random salt and, as info, the scope (user or CRYPTPROTECT_LOCAL_MACHINE) and the SHA-256 of the caller's
// optional entropy, so the wrong entropy fails the tag like a changed byte does.
//
// The blob is this runtime's own format (blobs made by Windows, with Windows' keys, never open here):
//   "VXDP" | version 1 | flags (bit 0: local machine) | 0, 0 | salt[16] | nonce[12]       (36 bytes, the AAD)
//   | ciphertext of (description size in bytes, u32 little-endian | description, UTF-16 | data) | tag[16]
#include <CommonCrypto/CommonCrypto.h>
#include <CommonCrypto/CommonRandom.h>
#include <CoreFoundation/CoreFoundation.h>
#if !VX_XROS
#include <Security/AuthSession.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include "aes_gcm.h"
#include "vx.h"

#ifdef __aarch64__
#define PROG_PTR(p) ((void *)(uintptr_t)vx_unlow((uint64_t)(uintptr_t)(p)))   // a pointer stored for the program
#else
#define PROG_PTR(p) ((void *)(p))
#endif

#define ERROR_INVALID_DATA 13
#define ERROR_NOT_ENOUGH_MEMORY 8
#define ERROR_INVALID_PARAMETER 87
#define ERROR_NOT_SUPPORTED 50
#define ERROR_PASSWORD_RESTRICTION 1325
#define NTE_BAD_KEYSET 0x80090016u
#define CRYPTPROTECT_UI_FORBIDDEN 0x1
#define CRYPTPROTECT_LOCAL_MACHINE 0x4
#define CRYPTPROTECTMEMORY_BLOCK_SIZE 16

typedef struct { DWORD cbData; uint8_t *pbData; } data_blob;                 // DATA_BLOB
typedef struct { DWORD cbSize, dwPromptFlags; HANDLE hwndApp; const WCHAR *szPrompt; } prompt_struct;   // CRYPTPROTECT_PROMPTSTRUCT

enum { HEADER = 36, KEY = 32, SALT = 16 };
static const uint8_t magic[4] = { 'V', 'X', 'D', 'P' };

static void wipe(void *p, size_t n) { volatile uint8_t *v = p; while (n--) *v++ = 0; }

// ---- the installation's key -------------------------------------------------------------------------------------
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t g_master[KEY];
static int g_have_master;

// Reads the key file through an open descriptor: a regular file of exactly 32 bytes, owned by this user and closed to
// everyone else (a key others could read may have been copied: refused rather than used).
static int read_key(int fd, const char *path) {
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 077) || st.st_size != KEY) {
        vx_log("dpapi: %s is not a 32-byte file readable only by this user; DPAPI refuses to use it", path);
        return -1;
    }
    size_t got = 0;
    while (got < KEY) {
        ssize_t r = read(fd, g_master + got, KEY - got);
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; return -1; }
        got += (size_t)r;
    }
    return 0;
}

// Marks the key's directory to be left out of backups (Time Machine honours it), each time the key is loaded, so a
// directory made before this was done is marked too. A volume that cannot keep the mark gets a log line, not a failure.
static void exclude_from_backup(const char *dir) {
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)dir, (CFIndex)strlen(dir), true);
    CFBooleanRef excluded = NULL;
    int done = url && CFURLCopyResourcePropertyForKey(url, kCFURLIsExcludedFromBackupKey, &excluded, NULL) && excluded &&
               CFGetTypeID(excluded) == CFBooleanGetTypeID() && CFBooleanGetValue(excluded);
    if (excluded) CFRelease(excluded);
    if (!done && (!url || !CFURLSetResourcePropertyForKey(url, kCFURLIsExcludedFromBackupKey, kCFBooleanTrue, NULL)))
        vx_log("dpapi: could not keep %s out of backups; the key in it may be copied with them", dir);
    if (url) CFRelease(url);
}

// The key, made on first use. Two processes starting together race to make it: each writes a temporary file and
// links it to the name, so the loser reads the winner's whole key, never a half-written one.
static DWORD load_master(void) {
    pthread_mutex_lock(&g_lock);
    if (g_have_master) { pthread_mutex_unlock(&g_lock); return 0; }
    char dir[4200], path[4300], tmp[4400];
    snprintf(dir, sizeof dir, "%s/dpapi", vx_root());
    snprintf(path, sizeof path, "%s/user.key", dir);
    DWORD err = NTE_BAD_KEYSET;
    if (mkdir(dir, 0700) && errno != EEXIST) { vx_log("dpapi: cannot make %s: %s", dir, strerror(errno)); goto out; }
    struct stat ds;
    if (lstat(dir, &ds) || !S_ISDIR(ds.st_mode) || ds.st_uid != geteuid()) { vx_log("dpapi: %s is not this user's directory", dir); goto out; }
    if (ds.st_mode & 077) chmod(dir, 0700);
    exclude_from_backup(dir);
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0 && errno == ENOENT) {
        uint8_t key[KEY], rnd[8];
        if (CCRandomGenerateBytes(key, sizeof key) != kCCSuccess || CCRandomGenerateBytes(rnd, sizeof rnd) != kCCSuccess) goto out;
        snprintf(tmp, sizeof tmp, "%s/.user.key.%d.%02x%02x%02x%02x%02x%02x%02x%02x", dir, (int)getpid(),
                 rnd[0], rnd[1], rnd[2], rnd[3], rnd[4], rnd[5], rnd[6], rnd[7]);
        int w = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (w < 0) { wipe(key, sizeof key); vx_log("dpapi: cannot write %s: %s", tmp, strerror(errno)); goto out; }
        int ok = fchmod(w, 0600) == 0 && write(w, key, KEY) == KEY && fsync(w) == 0;
        close(w);
        wipe(key, sizeof key);
        if (ok && link(tmp, path) && errno != EEXIST) ok = 0;   // EEXIST: another process made it first; use theirs
        unlink(tmp);
        if (!ok) { vx_log("dpapi: cannot make %s", path); goto out; }
        fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    }
    if (fd < 0) { vx_log("dpapi: cannot open %s: %s", path, strerror(errno)); goto out; }
    int r = read_key(fd, path);
    close(fd);
    if (r) { wipe(g_master, sizeof g_master); goto out; }
    g_have_master = 1;
    err = 0;
out:
    pthread_mutex_unlock(&g_lock);
    return err;
}

// HKDF-SHA256 with one output block (RFC 5869): extract with the salt, expand with info.
static void hkdf(const uint8_t *salt, size_t salt_n, const uint8_t *ikm, size_t ikm_n, const uint8_t *info, size_t info_n,
                 uint8_t out[KEY]) {
    uint8_t prk[CC_SHA256_DIGEST_LENGTH], one = 1;
    CCHmac(kCCHmacAlgSHA256, salt, salt_n, ikm, ikm_n, prk);
    CCHmacContext h;
    CCHmacInit(&h, kCCHmacAlgSHA256, prk, sizeof prk);
    CCHmacUpdate(&h, info, info_n);
    CCHmacUpdate(&h, &one, 1);
    CCHmacFinal(&h, out);
    wipe(&h, sizeof h);
    wipe(prk, sizeof prk);
}

// The key of one blob: the scope and the entropy are part of what it is derived from.
static void blob_key(const uint8_t salt[SALT], int machine, const data_blob *entropy, uint8_t out[KEY]) {
    uint8_t info[13 + 2 + CC_SHA256_DIGEST_LENGTH] = "VXRT DPAPI v1";
    info[13] = machine ? 'M' : 'U';
    const uint8_t *e = entropy ? VX_LOW(entropy->pbData) : NULL;
    info[14] = entropy && entropy->cbData && e;
    if (info[14]) CC_SHA256(e, entropy->cbData, info + 15);
    else memset(info + 15, 0, CC_SHA256_DIGEST_LENGTH);
    hkdf(salt, SALT, g_master, KEY, info, sizeof info, out);
}

// A prompt is a dialog this runtime cannot show; asking for one fails as Windows fails when UI is forbidden.
static int refuse_prompt(const prompt_struct *prompt, DWORD flags) {
    if (!prompt || !prompt->dwPromptFlags) return 0;
    vx_set_last_error(flags & CRYPTPROTECT_UI_FORBIDDEN ? ERROR_PASSWORD_RESTRICTION : ERROR_NOT_SUPPORTED);
    return 1;
}

static WINAPI BOOL CryptProtectData(const data_blob *in, const WCHAR *description, const data_blob *entropy, void *reserved,
                                    const prompt_struct *prompt, DWORD flags, data_blob *out) {
    (void)reserved;
    if (!in || !out || (in->cbData && !in->pbData)) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    if (refuse_prompt(prompt, flags)) return 0;
    DWORD err = load_master();
    if (err) { vx_set_last_error(err); return 0; }
    size_t dbytes = description ? vx_wcslen(description) * 2 : 0, plain_n = 4 + dbytes + in->cbData;
    size_t total = HEADER + plain_n + VX_GCM_TAG;
    if (total > 0xffffffffu) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    uint8_t *plain = malloc(plain_n), *blob = vx_heap_alloc(total);
    if (!plain || !blob) { free(plain); free(blob); vx_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    plain[0] = (uint8_t)dbytes; plain[1] = (uint8_t)(dbytes >> 8); plain[2] = (uint8_t)(dbytes >> 16); plain[3] = (uint8_t)(dbytes >> 24);
    if (dbytes) memcpy(plain + 4, description, dbytes);
    if (in->cbData) memcpy(plain + 4 + dbytes, VX_LOW(in->pbData), in->cbData);
    int machine = !!(flags & CRYPTPROTECT_LOCAL_MACHINE);
    memcpy(blob, magic, 4);
    blob[4] = 1; blob[5] = (uint8_t)machine; blob[6] = blob[7] = 0;
    uint8_t key[KEY];
    int ok = CCRandomGenerateBytes(blob + 8, SALT + VX_GCM_NONCE) == kCCSuccess;
    if (ok) {
        blob_key(blob + 8, machine, entropy, key);
        ok = !vx_gcm_seal(key, blob + 8 + SALT, blob, HEADER, plain, plain_n, blob + HEADER, blob + HEADER + plain_n);
        wipe(key, sizeof key);
    }
    wipe(plain, plain_n);
    free(plain);
    if (!ok) { free(blob); vx_set_last_error(NTE_BAD_KEYSET); return 0; }
    out->cbData = (DWORD)total;
    out->pbData = PROG_PTR(blob);
    return 1;
}

static WINAPI BOOL CryptUnprotectData(const data_blob *in, WCHAR **description, const data_blob *entropy, void *reserved,
                                      const prompt_struct *prompt, DWORD flags, data_blob *out) {
    (void)reserved;
    if (description) *description = NULL;
    if (!in || !out || (in->cbData && !in->pbData)) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    if (refuse_prompt(prompt, flags)) return 0;
    const uint8_t *blob = VX_LOW(in->pbData);
    if (in->cbData < HEADER + 4 + VX_GCM_TAG || memcmp(blob, magic, 4) || blob[4] != 1 || (blob[5] & ~1) || blob[6] || blob[7]) {
        vx_set_last_error(ERROR_INVALID_DATA);                     // not a blob made here (a Windows blob, or damaged)
        return 0;
    }
    DWORD err = load_master();
    if (err) { vx_set_last_error(err); return 0; }
    size_t plain_n = in->cbData - HEADER - VX_GCM_TAG;
    uint8_t *plain = malloc(plain_n), key[KEY];
    if (!plain) { vx_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    blob_key(blob + 8, blob[5] & 1, entropy, key);
    int bad = vx_gcm_open(key, blob + 8 + SALT, blob, HEADER, blob + HEADER, plain_n, blob + HEADER + plain_n, plain);
    wipe(key, sizeof key);
    size_t dbytes = bad ? 0 : (size_t)plain[0] | (size_t)plain[1] << 8 | (size_t)plain[2] << 16 | (size_t)plain[3] << 24;
    if (bad || dbytes > plain_n - 4 || (dbytes & 1)) {
        if (!bad) wipe(plain, plain_n);
        free(plain);
        vx_set_last_error(ERROR_INVALID_DATA);                     // changed, or the wrong entropy
        return 0;
    }
    size_t data_n = plain_n - 4 - dbytes;
    uint8_t *data = vx_heap_alloc(data_n);
    WCHAR *text = description ? vx_heap_alloc(dbytes + 2) : NULL;
    if (!data || (description && !text)) {
        free(data); free(text); wipe(plain, plain_n); free(plain);
        vx_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    if (data_n) memcpy(data, plain + 4 + dbytes, data_n);
    if (text) { memcpy(text, plain + 4, dbytes); text[dbytes / 2] = 0; *description = PROG_PTR(text); }
    wipe(plain, plain_n);
    free(plain);
    out->cbData = (DWORD)data_n;
    out->pbData = PROG_PTR(data);
    return 1;
}

// ---- memory: AES-256-CBC in place, with a key per process, per logon session or per boot --------------------------
// Length-preserving, so without a tag (Windows' CryptProtectMemory has none either). The keys of the shared scopes
// come from the installation's key and the boot session (memory protected before a restart does not open after it,
// as the documentation says); same-logon adds the user and macOS's security session (the login).
#define CRYPTPROTECTMEMORY_SAME_PROCESS 0
#define CRYPTPROTECTMEMORY_CROSS_PROCESS 1
#define CRYPTPROTECTMEMORY_SAME_LOGON 2
static uint8_t g_mem_key[3][KEY], g_mem_iv[3][16];
static int g_mem_ready[3];

static DWORD memory_key(DWORD scope) {
    pthread_mutex_lock(&g_lock);
    int ready = g_mem_ready[scope];
    pthread_mutex_unlock(&g_lock);
    if (ready) return 0;
    uint8_t key[KEY];
    if (scope == CRYPTPROTECTMEMORY_SAME_PROCESS) {
        if (CCRandomGenerateBytes(key, sizeof key) != kCCSuccess) return NTE_BAD_KEYSET;
    } else {
        DWORD err = load_master();
        if (err) return err;
        uint8_t info[128] = { 0 };
        size_t n = (size_t)snprintf((char *)info, 64, "VXRT memory %s ", scope == CRYPTPROTECTMEMORY_SAME_LOGON ? "logon" : "boot");
        size_t boot_n = sizeof info - n - 16;
        if (sysctlbyname("kern.bootsessionuuid", info + n, &boot_n, NULL, 0)) return NTE_BAD_KEYSET;
        n += boot_n;
        if (scope == CRYPTPROTECTMEMORY_SAME_LOGON) {
#if VX_XROS
            uint32_t session = 0;                                   // (visionOS: one user, no security sessions)
#else
            SecuritySessionId session = 0;
            if (SessionGetInfo(callerSecuritySession, &session, NULL) != errSessionSuccess) return NTE_BAD_KEYSET;
#endif
            uint32_t uid = (uint32_t)geteuid(), sid = (uint32_t)session;
            memcpy(info + n, &uid, 4); memcpy(info + n + 4, &sid, 4);
            n += 8;
        }
        hkdf((const uint8_t *)"VXRT CryptProtectMemory", 23, g_master, KEY, info, n, key);
    }
    uint8_t iv[CC_SHA256_DIGEST_LENGTH];
    CCHmac(kCCHmacAlgSHA256, key, sizeof key, "iv", 2, iv);
    pthread_mutex_lock(&g_lock);
    if (!g_mem_ready[scope]) {                                    // (another thread may have made it meanwhile: same key)
        memcpy(g_mem_key[scope], key, KEY);
        memcpy(g_mem_iv[scope], iv, 16);
        g_mem_ready[scope] = 1;
    }
    pthread_mutex_unlock(&g_lock);
    wipe(key, sizeof key);
    wipe(iv, sizeof iv);
    return 0;
}

static BOOL protect_memory(CCOperation op, uint8_t *data, DWORD n, DWORD flags) {
    if (flags > CRYPTPROTECTMEMORY_SAME_LOGON || n % CRYPTPROTECTMEMORY_BLOCK_SIZE || (n && !data)) {
        vx_set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (!n) return 1;
    DWORD err = memory_key(flags);
    if (err) { vx_set_last_error(err); return 0; }
    uint8_t *tmp = malloc(n);                                     // (CBC is not done in place by CommonCrypto)
    if (!tmp) { vx_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    size_t moved = 0;
    CCCryptorStatus s = CCCrypt(op, kCCAlgorithmAES, 0, g_mem_key[flags], KEY, g_mem_iv[flags], data, n, tmp, n, &moved);
    if (s == kCCSuccess && moved == n) memcpy(data, tmp, n);
    wipe(tmp, n);
    free(tmp);
    if (s != kCCSuccess || moved != n) { vx_set_last_error(NTE_BAD_KEYSET); return 0; }
    return 1;
}
static WINAPI BOOL CryptProtectMemory(void *data, DWORD n, DWORD flags) { return protect_memory(kCCEncrypt, data, n, flags); }
static WINAPI BOOL CryptUnprotectMemory(void *data, DWORD n, DWORD flags) { return protect_memory(kCCDecrypt, data, n, flags); }

static const vx_export dpapi[] = {
    VX_EXPORT(CryptProtectData), VX_EXPORT(CryptUnprotectData), VX_EXPORT(CryptProtectMemory), VX_EXPORT(CryptUnprotectMemory),
};
void vx_dpapi_init(void) {
    vx_register_module("crypt32.dll", dpapi, sizeof dpapi / sizeof dpapi[0]);
    vx_register_module("dpapi.dll", dpapi, sizeof dpapi / sizeof dpapi[0]);
}

#ifdef __aarch64__
#include "thunks/dpapi.c.inc"
#endif
