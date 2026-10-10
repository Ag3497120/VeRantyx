#include "steam_support.h"
#include <string.h>
#include <stdlib.h>
#include <zlib.h>

#include "lzma/LzmaDec.h"

#define ZSTD_STATIC_LINKING_ONLY
#include "zstd/zstd_decl.h"

#if defined(__APPLE__)
#include <CommonCrypto/CommonCryptor.h>
#include <CommonCrypto/CommonDigest.h>

static int aes_ecb_block(int op, const uint8_t key[32], const uint8_t in[16], uint8_t out[16]) {
    size_t moved = 0;
    return CCCrypt(op, kCCAlgorithmAES, kCCOptionECBMode, key, 32, NULL, in, 16, out, 16, &moved) == kCCSuccess ? 0 : -1;
}

static long aes_cbc(int op, const uint8_t key[32], const uint8_t iv[16], const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap) {
    size_t moved = 0;
    CCCryptorStatus s = CCCrypt(op, kCCAlgorithmAES, kCCOptionPKCS7Padding, key, 32, iv, in, in_len, out, out_cap, &moved);
    return s == kCCSuccess ? (long)moved : -1;
}
#define OP_DECRYPT kCCDecrypt
#define OP_ENCRYPT kCCEncrypt

void steam_sha1(const uint8_t *data, size_t len, uint8_t out[20]) { CC_SHA1(data, (CC_LONG)len, out); }

#else
#include <openssl/evp.h>
#include <openssl/sha.h>

static int aes_ecb_block(int enc, const uint8_t key[32], const uint8_t in[16], uint8_t out[16]) {
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    int n = 0, ok = EVP_CipherInit_ex(c, EVP_aes_256_ecb(), NULL, key, NULL, enc) &&
                    EVP_CIPHER_CTX_set_padding(c, 0) && EVP_CipherUpdate(c, out, &n, in, 16);
    EVP_CIPHER_CTX_free(c);
    return ok && n == 16 ? 0 : -1;
}

static long aes_cbc(int enc, const uint8_t key[32], const uint8_t iv[16], const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap) {
    (void)out_cap;
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    int n1 = 0, n2 = 0;
    int ok = EVP_CipherInit_ex(c, EVP_aes_256_cbc(), NULL, key, iv, enc) &&
             EVP_CipherUpdate(c, out, &n1, in, (int)in_len) && EVP_CipherFinal_ex(c, out + n1, &n2);
    EVP_CIPHER_CTX_free(c);
    return ok ? (long)(n1 + n2) : -1;
}
#define OP_DECRYPT 0
#define OP_ENCRYPT 1

void steam_sha1(const uint8_t *data, size_t len, uint8_t out[20]) { SHA1(data, len, out); }
#endif

long steam_aes_decrypt(const uint8_t key[32], const uint8_t *in, size_t in_len, uint8_t *out) {
    if (in_len < 32 || (in_len % 16) != 0) return -1;
    uint8_t iv[16];
    if (aes_ecb_block(OP_DECRYPT, key, in, iv) != 0) return -1;
    return aes_cbc(OP_DECRYPT, key, iv, in + 16, in_len - 16, out, in_len);
}

long steam_aes_encrypt(const uint8_t key[32], const uint8_t iv[16], const uint8_t *in, size_t in_len, uint8_t *out) {
    if (aes_ecb_block(OP_ENCRYPT, key, iv, out) != 0) return -1;
    long n = aes_cbc(OP_ENCRYPT, key, iv, in, in_len, out + 16, in_len + 32);
    return n < 0 ? -1 : n + 16;
}

static void *lz_alloc(ISzAllocPtr p, size_t size) { (void)p; return malloc(size); }
static void lz_free(ISzAllocPtr p, void *addr) { (void)p; free(addr); }
static const ISzAlloc lz_allocator = { lz_alloc, lz_free };

long steam_lzma_decode(uint8_t prop_bits, uint32_t dict_size,
                       const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_len) {
    uint8_t props[5] = { prop_bits, (uint8_t)dict_size, (uint8_t)(dict_size >> 8),
                         (uint8_t)(dict_size >> 16), (uint8_t)(dict_size >> 24) };
    SizeT out_len = dst_len, in_len = src_len;
    ELzmaStatus status;
    SRes r = LzmaDecode(dst, &out_len, src, &in_len, props, 5, LZMA_FINISH_END, &status, &lz_allocator);
    if (r != SZ_OK && !(r == SZ_ERROR_INPUT_EOF && out_len == dst_len)) return -1;
    return (long)out_len;
}

long steam_zstd_decode(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_len) {
    size_t n = ZSTD_decompress(dst, dst_len, src, src_len);
    return ZSTD_isError(n) ? -1 : (long)n;
}

long steam_inflate(int window_bits, const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_len) {
    z_stream s;
    memset(&s, 0, sizeof s);
    if (inflateInit2(&s, window_bits) != Z_OK) return -1;
    s.next_in = (Bytef *)src; s.avail_in = (uInt)src_len;
    s.next_out = dst; s.avail_out = (uInt)dst_len;
    int r = inflate(&s, Z_FINISH);
    long n = (long)s.total_out;
    inflateEnd(&s);
    return (r == Z_STREAM_END || (r == Z_BUF_ERROR && s.avail_out == 0)) ? n : -1;
}

uint32_t steam_crc32(const uint8_t *data, size_t len) { return (uint32_t)crc32(0L, data, (uInt)len); }

uint32_t steam_adler32_seed0(const uint8_t *data, size_t len) { return (uint32_t)adler32(0L, data, (uInt)len); }
