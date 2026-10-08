/*
 * hash.c — FNV-1a 64 with an fmix64 finaliser, and the odin3_bytes helper.
 */
#include "util/hash.h"

#include <string.h>

enum { FMIX_SHIFT = 33 };
#define FNV_OFFSET 0xcbf29ce484222325ULL
#define FNV_PRIME 0x100000001b3ULL
#define FMIX_MUL1 0xff51afd7ed558ccdULL
#define FMIX_MUL2 0xc4ceb9fe1a85ec53ULL

odin3_bytes odin3_bytes_cstr(const char *str) {
    odin3_bytes view = {NULL, 0};
    if (str != NULL) {
        view.ptr = str;
        view.len = strlen(str);
    }
    return view;
}

uint64_t odin3_hash_u64(uint64_t value) {
    value ^= value >> FMIX_SHIFT;
    value *= FMIX_MUL1;
    value ^= value >> FMIX_SHIFT;
    value *= FMIX_MUL2;
    value ^= value >> FMIX_SHIFT;
    return value;
}

uint64_t odin3_hash_bytes(odin3_bytes key, uint64_t seed) {
    uint64_t state = FNV_OFFSET ^ seed;
    const unsigned char *bytes = key.ptr;
    for (size_t i = 0; i < key.len; i++) {
        state ^= bytes[i];
        state *= FNV_PRIME;
    }
    return odin3_hash_u64(state);
}

uint64_t odin3_hash_combine(uint64_t hash, uint64_t value) {
    return odin3_hash_u64(hash ^ value);
}
