// src/aes_gcm.c against known answers: test cases 13 and 14 of the GCM specification (all-zero key and nonce), and
// vectors made here from patterned inputs by an independent implementation (Apple's CryptoKit, AES.GCM.seal); then
// a round trip and the rejection of a changed ciphertext, tag and associated data.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aes_gcm.h"

static int failures;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

static void unhex(const char *s, uint8_t *out, size_t *n) {
    size_t k = 0;
    for (; s[0] && s[1]; s += 2) { unsigned v; sscanf(s, "%2x", &v); out[k++] = (uint8_t)v; }
    *n = k;
}
static void pattern(uint8_t *p, size_t n, uint8_t seed) { for (size_t i = 0; i < n; i++) p[i] = (uint8_t)((seed * 31 + i * 7) & 0xff); }

// { plaintext bytes, associated data bytes, seed, ciphertext (a prefix of it for the long one), tag }
static const struct { size_t pn, an; uint8_t seed; const char *c, *tag; } made[] = {
    { 0, 0, 1, "", "da1cf548456e67bc18408415cb6b7e2b" },
    { 1, 0, 2, "44", "05126980980094b48da63927fedb3681" },
    { 15, 5, 3, "86e96ef80bc697a4e70896b76c7fad", "6b0feb5ed49c998cbd698435d756ba51" },
    { 16, 16, 4, "9b5fb8d91fff22565e5c22a06666947f", "780a036fcc64f8f55fbef88b95e47447" },
    { 17, 3, 5, "9936bbab8377e26970759d49384b189f84", "0c60bf8b3425d59904806e10c753459c" },
    { 33, 20, 6, "f6ec6e27dde3d1193be47b4cfa60a7b71c35de2198c9408a375f0b85628c3263dd", "d12f9f622c712946a124616e83bda87e" },
    { 100, 0, 7, "0010481a2ca38a7f0b39458bfb5176aed6c34a97ed33a1ef48c23e5bf12f4d2a46d91c5f2a1dc5cf2493c03d9147fd3ab09a53a23f5ca64d7e90e4f4aadb5295f41b91eae61a70974e844b6c81ac28f05f6849ed61656736469c9484bdc923669d90d8b7",
      "078fd1ed939d08e5980386f7833dedab" },
    { 600, 37, 8, "a1026709fd7c6d35b991c960c6deba393c05b0f15dda7fe9b112df71ce7f931c", "4ac08273956467057f2a3649c6eda50f" },
};

int main(void) {
    uint8_t key[32] = { 0 }, nonce[12] = { 0 }, zero16[16] = { 0 }, c[700], p[700], tag[16], want[700];
    size_t n;

    // GCM specification, test case 13: empty plaintext; test case 14: one zero block.
    CHECK(!vx_gcm_seal(key, nonce, NULL, 0, NULL, 0, c, tag), "seal 13");
    unhex("530f8afbc74536b9a963b4f1c4cb738b", want, &n);
    CHECK(!memcmp(tag, want, 16), "tag 13");
    CHECK(!vx_gcm_seal(key, nonce, NULL, 0, zero16, 16, c, tag), "seal 14");
    unhex("cea7403d4d606b6e074ec5d3baf39d18", want, &n);
    CHECK(!memcmp(c, want, 16), "ciphertext 14");
    unhex("d0d1c8a799996bf0265b98b5d48ab919", want, &n);
    CHECK(!memcmp(tag, want, 16), "tag 14");

    for (size_t t = 0; t < sizeof made / sizeof made[0]; t++) {
        uint8_t in[700], aad[64];
        pattern(key, 32, (uint8_t)(made[t].seed + 100));
        pattern(nonce, 12, (uint8_t)(made[t].seed + 200));
        pattern(in, made[t].pn, made[t].seed);
        pattern(aad, made[t].an, (uint8_t)(made[t].seed + 50));
        CHECK(!vx_gcm_seal(key, nonce, aad, made[t].an, in, made[t].pn, c, tag), "seal %zu", t);
        unhex(made[t].c, want, &n);
        CHECK(!memcmp(c, want, n), "ciphertext %zu", t);
        unhex(made[t].tag, want, &n);
        CHECK(!memcmp(tag, want, 16), "tag %zu", t);
        memset(p, 0xee, sizeof p);
        CHECK(!vx_gcm_open(key, nonce, aad, made[t].an, c, made[t].pn, tag, p) && !memcmp(p, in, made[t].pn), "open %zu", t);

        // anything changed is refused, and nothing is written
        uint8_t bad[16];
        memcpy(bad, tag, 16); bad[t % 16] ^= 0x01;
        memset(p, 0xee, sizeof p);
        CHECK(vx_gcm_open(key, nonce, aad, made[t].an, c, made[t].pn, bad, p) == -1 && p[0] == 0xee, "changed tag %zu", t);
        if (made[t].pn) {
            c[made[t].pn / 2] ^= 0x80;
            CHECK(vx_gcm_open(key, nonce, aad, made[t].an, c, made[t].pn, tag, p) == -1, "changed ciphertext %zu", t);
            c[made[t].pn / 2] ^= 0x80;
        }
        if (made[t].an) {
            aad[0] ^= 0x01;
            CHECK(vx_gcm_open(key, nonce, aad, made[t].an, c, made[t].pn, tag, p) == -1, "changed aad %zu", t);
            aad[0] ^= 0x01;
        }
        nonce[11] ^= 0x01;
        CHECK(vx_gcm_open(key, nonce, aad, made[t].an, c, made[t].pn, tag, p) == -1, "changed nonce %zu", t);
    }
    if (failures) { printf("aes_gcm: %d failures\n", failures); return 1; }
    printf("aes_gcm: ok\n");
    return 0;
}
