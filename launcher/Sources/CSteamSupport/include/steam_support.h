// Small C helpers the Swift client needs for Steam content: AES for depot data,
// LZMA and Zstandard for depot chunks, zlib for manifests and CM multi-messages.
#ifndef STEAM_SUPPORT_H
#define STEAM_SUPPORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Steam's symmetric scheme: the first 16 bytes are the IV encrypted with AES-256-ECB,
/// the rest is AES-256-CBC with PKCS#7 padding. Returns the plaintext length, or -1.
/// `out` must hold at least `in_len` bytes.
long steam_aes_decrypt(const uint8_t key[32], const uint8_t *in, size_t in_len, uint8_t *out);

/// The inverse of steam_aes_decrypt with a caller-supplied random IV (used by tests
/// and by Steam Cloud uploads). `out` must hold `in_len + 48` bytes.
long steam_aes_encrypt(const uint8_t key[32], const uint8_t iv[16], const uint8_t *in, size_t in_len, uint8_t *out);

/// Raw LZMA1 decode with Steam's VZip property layout (1 byte lc/lp/pb + 4 byte dictionary).
/// Returns bytes written, or -1.
long steam_lzma_decode(uint8_t prop_bits, uint32_t dict_size,
                       const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_len);

/// One Zstandard frame. Returns bytes written, or -1.
long steam_zstd_decode(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_len);

/// zlib inflate. window_bits: -15 raw deflate (zip entries), 31 gzip (CMsgMulti), 15 zlib.
/// Returns bytes written, or -1.
long steam_inflate(int window_bits, const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_len);

/// CRC-32 (zlib polynomial), used to verify VZip/VZstd payloads and zip entries.
uint32_t steam_crc32(const uint8_t *data, size_t len);

/// Adler-32 with seed 0 as Steam computes it for depot chunk checksums.
uint32_t steam_adler32_seed0(const uint8_t *data, size_t len);

/// SHA-1, used for file integrity and cloud file hashes. `out` is 20 bytes.
void steam_sha1(const uint8_t *data, size_t len, uint8_t out[20]);

#ifdef __cplusplus
}
#endif
#endif
