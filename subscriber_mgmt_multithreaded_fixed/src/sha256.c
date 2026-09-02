/**
 * @file sha256.c
 * @brief Public-domain-style SHA-256 implementation (single file, no deps).
 */
#include "sha256.h"
#include <string.h>
#include <stdio.h>

static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

#define ROTR(x,n) (((x) >> (n)) | ((x) << (32u - (n))))
#define CH(x,y,z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x,y,z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define BSIG0(x)   (ROTR(x,2)  ^ ROTR(x,13) ^ ROTR(x,22))
#define BSIG1(x)   (ROTR(x,6)  ^ ROTR(x,11) ^ ROTR(x,25))
#define SSIG0(x)   (ROTR(x,7)  ^ ROTR(x,18) ^ ((x) >> 3))
#define SSIG1(x)   (ROTR(x,17) ^ ROTR(x,19) ^ ((x) >> 10))

static void sha256_transform(sm_sha256_ctx_t *ctx, const uint8_t block[64])
{
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = ((uint32_t)block[i*4] << 24) | ((uint32_t)block[i*4+1] << 16) |
               ((uint32_t)block[i*4+2] << 8) | (uint32_t)block[i*4+3];
    }
    for (int i = 16; i < 64; ++i) {
        w[i] = SSIG1(w[i-2]) + w[i-7] + SSIG0(w[i-15]) + w[i-16];
    }

    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];

    for (int i = 0; i < 64; ++i) {
        uint32_t t1 = h + BSIG1(e) + CH(e,f,g) + K[i] + w[i];
        uint32_t t2 = BSIG0(a) + MAJ(a,b,c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

void sm_sha256_init(sm_sha256_ctx_t *ctx)
{
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
    ctx->bitlen = 0;
    ctx->buffer_len = 0;
}

void sm_sha256_update(sm_sha256_ctx_t *ctx, const uint8_t *data, size_t len)
{
    size_t i = 0;
    while (i < len) {
        size_t take = 64 - ctx->buffer_len;
        if (take > len - i) take = len - i;
        memcpy(ctx->buffer + ctx->buffer_len, data + i, take);
        ctx->buffer_len += take;
        i += take;
        ctx->bitlen += (uint64_t)take * 8u;
        if (ctx->buffer_len == 64) {
            sha256_transform(ctx, ctx->buffer);
            ctx->buffer_len = 0;
        }
    }
}

void sm_sha256_final(sm_sha256_ctx_t *ctx, uint8_t out_digest[SM_SHA256_DIGEST_SIZE])
{
    uint64_t bitlen = ctx->bitlen;
    uint8_t pad = 0x80;
    sm_sha256_update(ctx, &pad, 1);

    uint8_t zero = 0x00;
    while (ctx->buffer_len != 56) {
        sm_sha256_update(ctx, &zero, 1);
    }

    uint8_t len_bytes[8];
    for (int i = 0; i < 8; ++i) {
        len_bytes[i] = (uint8_t)(bitlen >> (56 - 8*i));
    }
    /* bypass bitlen accounting for the length field itself */
    memcpy(ctx->buffer + ctx->buffer_len, len_bytes, 8);
    ctx->buffer_len += 8;
    sha256_transform(ctx, ctx->buffer);
    ctx->buffer_len = 0;

    for (int i = 0; i < 8; ++i) {
        out_digest[i*4]   = (uint8_t)(ctx->state[i] >> 24);
        out_digest[i*4+1] = (uint8_t)(ctx->state[i] >> 16);
        out_digest[i*4+2] = (uint8_t)(ctx->state[i] >> 8);
        out_digest[i*4+3] = (uint8_t)(ctx->state[i]);
    }
}

void sm_sha256(const uint8_t *data, size_t len, uint8_t out_digest[SM_SHA256_DIGEST_SIZE])
{
    sm_sha256_ctx_t ctx;
    sm_sha256_init(&ctx);
    sm_sha256_update(&ctx, data, len);
    sm_sha256_final(&ctx, out_digest);
}

void sm_hex_encode(const uint8_t *data, size_t len, char *out_hex)
{
    static const char hexchars[] = "0123456789abcdef";
    for (size_t i = 0; i < len; ++i) {
        out_hex[i*2]   = hexchars[(data[i] >> 4) & 0x0Fu];
        out_hex[i*2+1] = hexchars[data[i] & 0x0Fu];
    }
    out_hex[len*2] = '\0';
}

int sm_hex_decode(const char *hex, uint8_t *out, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        unsigned int byte;
        if (sscanf(hex + i*2, "%2x", &byte) != 1) return -1;
        out[i] = (uint8_t)byte;
    }
    return 0;
}
