// AES-256-GCM (NIST SP 800-38D) on CommonCrypto's AES block cipher: the authenticated encryption behind DPAPI
// (dpapi.c). 96-bit nonces, 128-bit tags. No vx.h: the host test (tests/aes_gcm.c) links it alone.
#ifndef VX_AES_GCM_H
#define VX_AES_GCM_H
#include <stddef.h>
#include <stdint.h>

enum { VX_GCM_KEY = 32, VX_GCM_NONCE = 12, VX_GCM_TAG = 16 };

// out receives n bytes of ciphertext (it may be in), tag the 16-byte tag. 0 on success.
int vx_gcm_seal(const uint8_t key[VX_GCM_KEY], const uint8_t nonce[VX_GCM_NONCE], const uint8_t *aad, size_t aad_n,
                const uint8_t *in, size_t n, uint8_t *out, uint8_t tag[VX_GCM_TAG]);
// The tag is checked (in constant time) before anything is decrypted; on a mismatch out is left untouched and -1 is
// returned. 0 on success.
int vx_gcm_open(const uint8_t key[VX_GCM_KEY], const uint8_t nonce[VX_GCM_NONCE], const uint8_t *aad, size_t aad_n,
                const uint8_t *in, size_t n, const uint8_t tag[VX_GCM_TAG], uint8_t *out);
#endif
