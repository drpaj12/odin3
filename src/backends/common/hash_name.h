/*
 * hash_name.h — hashed object names (spec docs/specs/2026-10-20-5.4-hdl-locations-design.md
 * section 13: H-2, H-3, H-8).
 *
 * A hash name is `h` plus N characters of lower-case base32 (`a-z2-7`) taken from the first
 * 5 * N bits (big-endian) of the SHA-256 of the full provenance name. N is 10 unless two names of
 * one module and kind collide (then both take 15, 20, ... up to 52). Pure functions over byte
 * strings: no IR, no global state beyond the test hook, the same result on every platform.
 */
#ifndef ODIN3_BACKENDS_COMMON_HASH_NAME_H
#define ODIN3_BACKENDS_COMMON_HASH_NAME_H

#include "odin3/odin3.h"
#include "util/hash.h"
#include "util/sha256.h"

#include <stdint.h>

enum {
    ODIN3_HASH_NAME_WIDTH = 10,           /* H-3: base characters after the `h` */
    ODIN3_HASH_NAME_STEP = 5,             /* H-3: each extension adds this many characters */
    ODIN3_HASH_NAME_MAX_CHARS = 52,       /* 52 * 5 = 260 bits: the whole 256-bit digest */
    ODIN3_HASH_NAME_BUF = 54,             /* `h`, 52 characters and the NUL */
    ODIN3_HASH_NAME_MAX_ENTRIES = 1 << 22 /* one resolve call: names of one module and kind */
};

/* A hash name as text (NUL-terminated). */
typedef struct odin3_hash_name {
    char text[ODIN3_HASH_NAME_BUF];
} odin3_hash_name;

/*
 * Writes the first `chars` base32 characters of digest (32 bytes) to out, then a NUL (out holds
 * chars + 1 bytes). The alphabet is `abcdefghijklmnopqrstuvwxyz234567`; character k holds bits
 * 5k to 5k + 4, big-endian, bits past the 256th read as 0. chars must be 1 to 52.
 */
void odin3_base32_encode(const uint8_t digest[ODIN3_SHA256_DIGEST_BYTES], uint32_t chars,
                         char *out);

/*
 * The hash name of full at `chars` characters (1 to 52; other values are clamped into that range).
 * full is hashed exactly as given: no terminator, no normalisation (H-2).
 */
void odin3_hash_name_make(odin3_bytes full, uint32_t chars, odin3_hash_name *out);

/*
 * H-3 collision resolution over the full names of ONE module and kind. fulls[0..n) are the names
 * to rename, reserved[0..nreserved) are the names that stay as they are (a user net called
 * `habcdefghij`). Writes out[i] for fulls[i]. Every name starts at ODIN3_HASH_NAME_WIDTH
 * characters; if two distinct full names give the same text, BOTH take 5 more characters, and so
 * on up to 52, and a name that equals a reserved name takes 5 more the same way. Equal full names
 * are one name (same result, no collision). The result depends only on the set of names, never on
 * their order. A copy of the inputs is never kept; out must hold n entries.
 *
 * ODIN3_ERR_INVALID_ARG when n is over ODIN3_HASH_NAME_MAX_ENTRIES (logged), or when a collision
 * remains at 52 characters (logged; needs a SHA-256 collision or a crafted reserved name).
 * ODIN3_ERR_NO_MEMORY on out of memory; out is then unspecified.
 */
odin3_status odin3_hash_names_resolve(const odin3_bytes *fulls, uint32_t n,
                                      const odin3_bytes *reserved, uint32_t nreserved,
                                      odin3_hash_name *out);

/*
 * Test hook (T-hash-3, hidden, not part of the ABI): the starting width of resolve becomes
 * `chars` (1 to 52) so a test can force collisions; 0 restores ODIN3_HASH_NAME_WIDTH.
 */
void odin3_hash_name_test_width(uint32_t chars);

#endif
