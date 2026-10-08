/*
 * hash.h — byte-range view and deterministic hashing (FNV-1a 64 + fmix64).
 */
#ifndef ODIN3_UTIL_HASH_H
#define ODIN3_UTIL_HASH_H

#include <stddef.h>
#include <stdint.h>

typedef struct odin3_bytes {
    const void *ptr;
    size_t len;
} odin3_bytes;

/* Fixed seed ("odin3"); no randomness anywhere. */
#define ODIN3_HASH_SEED UINT64_C(0x6f64696e33)

/* View of a NUL-terminated string (pointer + strlen); NULL gives {NULL, 0}. */
odin3_bytes odin3_bytes_cstr(const char *str);

/*
 * FNV-1a 64 over the bytes, finalised with fmix64. {NULL, 0} hashes like the empty string;
 * key.ptr must be non-NULL when key.len > 0.
 */
uint64_t odin3_hash_bytes(odin3_bytes key, uint64_t seed);

/* fmix64 of an integer; odin3_hash_u64(0) == 0. */
uint64_t odin3_hash_u64(uint64_t value);

/*
 * Mixes value into hash: odin3_hash_u64(hash ^ value). Order-sensitive when chained, so it combines
 * the fields of a record into one finalised 64-bit hash.
 */
uint64_t odin3_hash_combine(uint64_t hash, uint64_t value);

#endif
