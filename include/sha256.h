/**
 * @file sha256.h
 * @brief Minimal, dependency-free SHA-256 implementation used as the basis
 *        for password hashing (see auth.c). Not a general-purpose crypto
 *        library -- just enough to keep the project self-contained and
 *        avoid pulling in OpenSSL as a build dependency.
 */
#ifndef SM_SHA256_H
#define SM_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SM_SHA256_DIGEST_SIZE 32u

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  buffer[64];
    size_t   buffer_len;
} sm_sha256_ctx_t;

void sm_sha256_init(sm_sha256_ctx_t *ctx);
void sm_sha256_update(sm_sha256_ctx_t *ctx, const uint8_t *data, size_t len);
void sm_sha256_final(sm_sha256_ctx_t *ctx, uint8_t out_digest[SM_SHA256_DIGEST_SIZE]);

/** Convenience one-shot helper. */
void sm_sha256(const uint8_t *data, size_t len, uint8_t out_digest[SM_SHA256_DIGEST_SIZE]);

/** Hex-encode a digest into a buffer of at least len*2+1 bytes. */
void sm_hex_encode(const uint8_t *data, size_t len, char *out_hex);
/** Decode a hex string of exactly len*2 chars into len raw bytes. Returns 0 on success, -1 on bad input. */
int sm_hex_decode(const char *hex, uint8_t *out, size_t len);

#ifdef __cplusplus
}
#endif
#endif /* SM_SHA256_H */
