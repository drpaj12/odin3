/*
 * sha256.c — SHA-256 per FIPS 180-4. Streams through a 64-byte block buffer; no allocation.
 */
#include "util/sha256.h"

#include <string.h>

enum {
    WORD_BITS = 32,
    WORD_BYTES = 4,
    BLOCK_WORDS = 16,
    SCHEDULE_WORDS = 64,
    STATE_WORDS = ODIN3_SHA256_STATE_WORDS,
    LENGTH_BYTES = 8,
    LENGTH_OFFSET = ODIN3_SHA256_BLOCK_BYTES - LENGTH_BYTES,
    PAD_MARKER = 0x80,
    BYTE_BITS = 8,
    NIBBLE_BITS = 4,
    NIBBLE_MASK = 0xF,
    SHIFT_BYTE1 = 8,
    SHIFT_BYTE2 = 16,
    SHIFT_BYTE3 = 24,
    /* Message schedule: small sigma 0 and 1 (FIPS 180-4 4.1.2). */
    SIG0_ROT_A = 7,
    SIG0_ROT_B = 18,
    SIG0_SHIFT = 3,
    SIG1_ROT_A = 17,
    SIG1_ROT_B = 19,
    SIG1_SHIFT = 10,
    SCHED_BACK_1 = 15, /* W[i-15] feeds sigma 0 */
    SCHED_BACK_2 = 2,  /* W[i-2] feeds sigma 1 */
    SCHED_BACK_7 = 7,
    /* Rounds: big sigma 0 and 1. */
    SUM0_ROT_A = 2,
    SUM0_ROT_B = 13,
    SUM0_ROT_C = 22,
    SUM1_ROT_A = 6,
    SUM1_ROT_B = 11,
    SUM1_ROT_C = 25,
    /* Indices into the working variables a..h. */
    VAR_A = 0,
    VAR_B = 1,
    VAR_C = 2,
    VAR_E = 4,
    VAR_F = 5,
    VAR_G = 6,
    VAR_H = 7
};

static const uint32_t SHA256_INIT[STATE_WORDS] = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U,
                                                  0xa54ff53aU, 0x510e527fU, 0x9b05688cU,
                                                  0x1f83d9abU, 0x5be0cd19U};

static const uint32_t SHA256_K[SCHEDULE_WORDS] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
    0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
    0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
    0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
    0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
    0xc67178f2U};

static uint32_t rotr(uint32_t value, uint32_t amount) {
    return (value >> amount) | (value << (WORD_BITS - amount));
}

static uint32_t load_be32(const uint8_t *src) {
    return ((uint32_t)src[0] << SHIFT_BYTE3) | ((uint32_t)src[1] << SHIFT_BYTE2) |
           ((uint32_t)src[2] << SHIFT_BYTE1) | (uint32_t)src[3];
}

static void store_be32(uint8_t *dst, uint32_t value) {
    dst[0] = (uint8_t)(value >> SHIFT_BYTE3);
    dst[1] = (uint8_t)(value >> SHIFT_BYTE2);
    dst[2] = (uint8_t)(value >> SHIFT_BYTE1);
    dst[3] = (uint8_t)value;
}

/* Message schedule W[0..63] for one block. */
static void expand_block(const uint8_t block[ODIN3_SHA256_BLOCK_BYTES],
                         uint32_t sched[SCHEDULE_WORDS]) {
    for (uint32_t i = 0; i < BLOCK_WORDS; i++) {
        sched[i] = load_be32(block + (size_t)i * WORD_BYTES);
    }
    for (uint32_t i = BLOCK_WORDS; i < SCHEDULE_WORDS; i++) {
        uint32_t early = sched[i - SCHED_BACK_1];
        uint32_t late = sched[i - SCHED_BACK_2];
        uint32_t sig0 = rotr(early, SIG0_ROT_A) ^ rotr(early, SIG0_ROT_B) ^ (early >> SIG0_SHIFT);
        uint32_t sig1 = rotr(late, SIG1_ROT_A) ^ rotr(late, SIG1_ROT_B) ^ (late >> SIG1_SHIFT);
        sched[i] = sched[i - BLOCK_WORDS] + sig0 + sched[i - SCHED_BACK_7] + sig1;
    }
}

/* One compression round on the working variables vars[0..7] = a..h. */
static void round_step(uint32_t vars[STATE_WORDS], uint32_t k_plus_w) {
    uint32_t evar = vars[VAR_E];
    uint32_t avar = vars[VAR_A];
    uint32_t sum1 = rotr(evar, SUM1_ROT_A) ^ rotr(evar, SUM1_ROT_B) ^ rotr(evar, SUM1_ROT_C);
    uint32_t choose = (evar & vars[VAR_F]) ^ (~evar & vars[VAR_G]);
    uint32_t temp1 = vars[VAR_H] + sum1 + choose + k_plus_w;
    uint32_t sum0 = rotr(avar, SUM0_ROT_A) ^ rotr(avar, SUM0_ROT_B) ^ rotr(avar, SUM0_ROT_C);
    uint32_t majority = (avar & vars[VAR_B]) ^ (avar & vars[VAR_C]) ^ (vars[VAR_B] & vars[VAR_C]);
    uint32_t temp2 = sum0 + majority;
    for (uint32_t i = STATE_WORDS - 1U; i > 0U; i--) {
        vars[i] = vars[i - 1U];
    }
    vars[VAR_E] += temp1;
    vars[VAR_A] = temp1 + temp2;
}

static void compress(odin3_sha256 *ctx, const uint8_t block[ODIN3_SHA256_BLOCK_BYTES]) {
    uint32_t sched[SCHEDULE_WORDS];
    uint32_t vars[STATE_WORDS];
    expand_block(block, sched);
    memcpy(vars, ctx->state, sizeof vars);
    for (uint32_t i = 0; i < SCHEDULE_WORDS; i++) {
        round_step(vars, SHA256_K[i] + sched[i]);
    }
    for (uint32_t i = 0; i < STATE_WORDS; i++) {
        ctx->state[i] += vars[i];
    }
}

void odin3_sha256_init(odin3_sha256 *ctx) {
    memcpy(ctx->state, SHA256_INIT, sizeof ctx->state);
    ctx->bytes = 0;
    ctx->used = 0;
    memset(ctx->block, 0, sizeof ctx->block);
}

void odin3_sha256_update(odin3_sha256 *ctx, const void *data, size_t len) {
    if (len == 0U) {
        return; /* data may be NULL */
    }
    const uint8_t *src = (const uint8_t *)data;
    ctx->bytes += (uint64_t)len;
    while (len > 0U) {
        size_t room = ODIN3_SHA256_BLOCK_BYTES - (size_t)ctx->used;
        size_t take = len < room ? len : room;
        memcpy(ctx->block + ctx->used, src, take);
        ctx->used += (uint32_t)take;
        src += take;
        len -= take;
        if (ctx->used == ODIN3_SHA256_BLOCK_BYTES) {
            compress(ctx, ctx->block);
            ctx->used = 0;
        }
    }
}

void odin3_sha256_final(odin3_sha256 *ctx, uint8_t digest[ODIN3_SHA256_DIGEST_BYTES]) {
    uint64_t bit_count = ctx->bytes * BYTE_BITS;
    ctx->block[ctx->used] = PAD_MARKER;
    ctx->used++;
    if (ctx->used > LENGTH_OFFSET) {
        memset(ctx->block + ctx->used, 0, ODIN3_SHA256_BLOCK_BYTES - (size_t)ctx->used);
        compress(ctx, ctx->block);
        ctx->used = 0;
    }
    memset(ctx->block + ctx->used, 0, (size_t)LENGTH_OFFSET - (size_t)ctx->used);
    store_be32(ctx->block + LENGTH_OFFSET, (uint32_t)(bit_count >> WORD_BITS));
    store_be32(ctx->block + LENGTH_OFFSET + WORD_BYTES, (uint32_t)bit_count);
    compress(ctx, ctx->block);
    for (uint32_t i = 0; i < STATE_WORDS; i++) {
        store_be32(digest + (size_t)i * WORD_BYTES, ctx->state[i]);
    }
    ctx->used = 0;
}

void odin3_sha256_hex(const uint8_t digest[ODIN3_SHA256_DIGEST_BYTES],
                      char hex[ODIN3_SHA256_HEX_CHARS]) {
    static const char DIGITS[] = "0123456789abcdef";
    for (size_t i = 0; i < ODIN3_SHA256_DIGEST_BYTES; i++) {
        hex[(size_t)2 * i] = DIGITS[(digest[i] >> NIBBLE_BITS) & NIBBLE_MASK];
        hex[((size_t)2 * i) + 1U] = DIGITS[digest[i] & NIBBLE_MASK];
    }
    hex[ODIN3_SHA256_HEX_CHARS - 1U] = '\0';
}
