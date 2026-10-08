/*
 * idindex.h — ID-only hash-cons index (Robin Hood open addressing, backward-shift deletion).
 */
#ifndef ODIN3_UTIL_IDINDEX_H
#define ODIN3_UTIL_IDINDEX_H

#include "odin3/odin3.h"
#include "util/hash.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct odin3_idindex odin3_idindex; /* opaque; slots hold uint32_t IDs + hashes */

/* True when the object stored under id equals the probe bytes. */
typedef bool (*odin3_id_equals)(const void *ctx, uint32_t id, odin3_bytes probe);

/*
 * Hashes passed in odin3_idcmp / odin3_identry must be finalised 64-bit hashes (odin3_hash_bytes,
 * or odin3_hash_u64 / odin3_hash_combine over combined fields): the low bits choose the home slot.
 */

/* A lookup: hash + probe bytes + the caller's equality. */
typedef struct odin3_idcmp {
    uint64_t hash;
    odin3_bytes probe;
    odin3_id_equals eq;
    const void *ctx;
} odin3_idcmp;

typedef struct odin3_identry {
    uint64_t hash;
    uint32_t id;
} odin3_identry;

/* Empty index with room for at least initial_cap entries (0 = default); NULL on out of memory. */
odin3_idindex *odin3_idindex_create(size_t initial_cap);

/* Frees the index; NULL is allowed. */
void odin3_idindex_destroy(odin3_idindex *ix);

/* True and *id (may be NULL) when an entry with cmp->hash satisfies cmp->eq(cmp->ctx, id, probe).
 */
bool odin3_idindex_find(const odin3_idindex *ix, const odin3_idcmp *cmp, uint32_t *id);

/*
 * Adds an entry; the caller has already checked (via find) that it is absent, so duplicates are
 * not detected. Any ID is valid, including 0. On out of memory the index is intact.
 */
odin3_status odin3_idindex_insert(odin3_idindex *ix, odin3_identry entry);

/* Removes the entry with exactly this hash and id (no callback); false if absent. */
bool odin3_idindex_remove(odin3_idindex *ix, odin3_identry entry);

size_t odin3_idindex_count(const odin3_idindex *ix);

#endif
