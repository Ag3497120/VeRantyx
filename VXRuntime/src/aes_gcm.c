// AES-256-GCM from NIST SP 800-38D: the counter mode (GCTR) and the GHASH authenticator written here, the AES block
// cipher from CommonCrypto (ECB over counter blocks). Only 96-bit nonces (J0 = nonce || 0^31 || 1) and full 128-bit
// tags, the parameters the spec recommends; messages stay far below the 2^32 - 2 blocks one nonce may cover.
#include <CommonCrypto/CommonCryptor.h>
#include <string.h>
#include "aes_gcm.h"

typedef struct { uint64_t hi, lo; } block128;           // a field element, bit 0 = the most significant bit of hi

static block128 load128(const uint8_t *p) {
    block128 b = { 0, 0 };
    for (int i = 0; i < 8; i++) { b.hi = b.hi << 8 | p[i]; b.lo = b.lo << 8 | p[i + 8]; }
    return b;
}
static void store128(uint8_t *p, block128 b) {
    for (int i = 7; i >= 0; i--) { p[i] = (uint8_t)b.hi; p[i + 8] = (uint8_t)b.lo; b.hi >>= 8; b.lo >>= 8; }
}

// X * Y in GF(2^128) with the spec's bit order (algorithm 1 of SP 800-38D), without branches on the data.
static block128 gf_mul(block128 x, block128 y) {
    block128 z = { 0, 0 }, v = y;
    for (int i = 0; i < 128; i++) {
        uint64_t bit = i < 64 ? x.hi >> (63 - i) & 1 : x.lo >> (127 - i) & 1;
        uint64_t take = 0 - bit;
        z.hi ^= v.hi & take; z.lo ^= v.lo & take;
        uint64_t carry = 0 - (v.lo & 1);                        // R = 11100001 || 0^120 when the low bit falls out
        v.lo = v.lo >> 1 | v.hi << 63;
        v.hi = v.hi >> 1 ^ (0xe100000000000000ull & carry);
    }
    return z;
}

typedef struct { block128 h, y; } ghash_state;
static void ghash_blocks(ghash_state *g, const uint8_t *p, size_t n) {      // n bytes, the last block zero-padded
    while (n) {
        uint8_t blk[16] = { 0 };
        size_t k = n < 16 ? n : 16;
        memcpy(blk, p, k);
        block128 x = load128(blk);
        g->y.hi ^= x.hi; g->y.lo ^= x.lo;
        g->y = gf_mul(g->y, g->h);
        p += k; n -= k;
    }
}

// AES with the key, one ECB pass over whole blocks (the counter blocks and E(K, 0^128)); in and out must differ
// (CommonCryptor.h: no in-place operation in the block modes).
static int aes_blocks(CCCryptorRef aes, const uint8_t *in, uint8_t *out, size_t n) {
    size_t moved = 0;
    return CCCryptorUpdate(aes, in, n, out, n, &moved) == kCCSuccess && moved == n ? 0 : -1;
}

// GCTR from counter block cb (incremented in its low 32 bits), XORed into in -> out.
static int gctr(CCCryptorRef aes, const uint8_t icb[16], const uint8_t *in, size_t n, uint8_t *out) {
    uint8_t cb[16], ctrs[512], ks[512];
    memcpy(cb, icb, 16);
    uint32_t ctr = (uint32_t)cb[12] << 24 | (uint32_t)cb[13] << 16 | (uint32_t)cb[14] << 8 | cb[15];
    while (n) {
        size_t blocks = (n + 15) / 16;
        if (blocks > sizeof ks / 16) blocks = sizeof ks / 16;
        for (size_t b = 0; b < blocks; b++) {
            memcpy(ctrs + b * 16, cb, 12);
            ctrs[b * 16 + 12] = (uint8_t)(ctr >> 24); ctrs[b * 16 + 13] = (uint8_t)(ctr >> 16);
            ctrs[b * 16 + 14] = (uint8_t)(ctr >> 8); ctrs[b * 16 + 15] = (uint8_t)ctr;
            ctr++;
        }
        if (aes_blocks(aes, ctrs, ks, blocks * 16)) return -1;
        size_t k = n < blocks * 16 ? n : blocks * 16;
        for (size_t i = 0; i < k; i++) out[i] = in[i] ^ ks[i];
        in += k; out += k; n -= k;
    }
    memset(ks, 0, sizeof ks);
    return 0;
}

// The tag over aad and the ciphertext c: E(K, J0) xor GHASH(H, A || C || len(A) || len(C)).
static int compute_tag(CCCryptorRef aes, const uint8_t j0[16], const uint8_t *aad, size_t aad_n, const uint8_t *c, size_t n,
                       uint8_t tag[16]) {
    uint8_t zero[16] = { 0 }, h[16], lens[16], s[16];
    if (aes_blocks(aes, zero, h, 16)) return -1;
    ghash_state g = { load128(h), { 0, 0 } };
    ghash_blocks(&g, aad, aad_n);
    ghash_blocks(&g, c, n);
    uint64_t abits = (uint64_t)aad_n * 8, cbits = (uint64_t)n * 8;
    for (int i = 0; i < 8; i++) { lens[7 - i] = (uint8_t)(abits >> (8 * i)); lens[15 - i] = (uint8_t)(cbits >> (8 * i)); }
    ghash_blocks(&g, lens, 16);
    store128(s, g.y);
    int r = gctr(aes, j0, s, 16, tag);
    memset(h, 0, sizeof h); memset(s, 0, sizeof s);
    return r;
}

static CCCryptorRef aes_open(const uint8_t key[VX_GCM_KEY]) {
    CCCryptorRef aes = NULL;
    if (CCCryptorCreateWithMode(kCCEncrypt, kCCModeECB, kCCAlgorithmAES, ccNoPadding, NULL, key, VX_GCM_KEY, NULL, 0, 0, 0, &aes) != kCCSuccess)
        return NULL;
    return aes;
}

static void j0_of(const uint8_t nonce[VX_GCM_NONCE], uint8_t j0[16], uint8_t icb[16]) {
    memcpy(j0, nonce, 12);
    j0[12] = j0[13] = j0[14] = 0; j0[15] = 1;
    memcpy(icb, j0, 16);
    icb[15] = 2;                                                  // inc32(J0)
}

int vx_gcm_seal(const uint8_t key[VX_GCM_KEY], const uint8_t nonce[VX_GCM_NONCE], const uint8_t *aad, size_t aad_n,
                const uint8_t *in, size_t n, uint8_t *out, uint8_t tag[VX_GCM_TAG]) {
    if (n > ((uint64_t)1 << 36) - 32) return -1;
    CCCryptorRef aes = aes_open(key);
    if (!aes) return -1;
    uint8_t j0[16], icb[16];
    j0_of(nonce, j0, icb);
    int r = gctr(aes, icb, in, n, out);
    if (!r) r = compute_tag(aes, j0, aad, aad_n, out, n, tag);
    CCCryptorRelease(aes);
    return r;
}

int vx_gcm_open(const uint8_t key[VX_GCM_KEY], const uint8_t nonce[VX_GCM_NONCE], const uint8_t *aad, size_t aad_n,
                const uint8_t *in, size_t n, const uint8_t tag[VX_GCM_TAG], uint8_t *out) {
    if (n > ((uint64_t)1 << 36) - 32) return -1;
    CCCryptorRef aes = aes_open(key);
    if (!aes) return -1;
    uint8_t j0[16], icb[16], want[16];
    j0_of(nonce, j0, icb);
    int r = compute_tag(aes, j0, aad, aad_n, in, n, want);
    if (!r) {
        uint8_t diff = 0;                                         // constant-time comparison of the tags
        for (int i = 0; i < 16; i++) diff |= (uint8_t)(want[i] ^ tag[i]);
        r = diff ? -1 : gctr(aes, icb, in, n, out);
    }
    memset(want, 0, sizeof want);
    CCCryptorRelease(aes);
    return r;
}
