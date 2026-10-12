/*
 * sha256.h — SHA-256 (FIPS 180-4), streaming, no allocation. Identity hashing for run info (AB-20).
 */
#ifndef ODIN3_UTIL_SHA256_H
#define ODIN3_UTIL_SHA256_H

#include <stddef.h>
#include <stdint.h>

enum {
    ODIN3_SHA256_BLOCK_BYTES = 64,
    ODIN3_SHA256_DIGEST_BYTES = 32,
    ODIN3_SHA256_HEX_CHARS = 65, /* 64 digits and the NUL */
    ODIN3_SHA256_STATE_WORDS = 8
};

typedef struct odin3_sha256 {
    uint32_t state[ODIN3_SHA256_STATE_WORDS];
    uint64_t bytes; /* total bytes absorbed so far */
    uint8_t block[ODIN3_SHA256_BLOCK_BYTES];
    uint32_t used; /* bytes pending in block, < 64 between calls */
} odin3_sha256;

/* Starts a fresh hash. */
void odin3_sha256_init(odin3_sha256 *ctx);

/* Absorbs len bytes; data may be NULL when len is 0. */
void odin3_sha256_update(odin3_sha256 *ctx, const void *data, size_t len);

/* Pads and writes the digest; ctx must be re-initialised before reuse. */
void odin3_sha256_final(odin3_sha256 *ctx, uint8_t digest[ODIN3_SHA256_DIGEST_BYTES]);

/* Lower-case hex of digest, NUL-terminated. */
void odin3_sha256_hex(const uint8_t digest[ODIN3_SHA256_DIGEST_BYTES],
                      char hex[ODIN3_SHA256_HEX_CHARS]);

#endif
