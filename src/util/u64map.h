/*
 * u64map.h — integer-keyed map (Robin Hood open addressing, backward-shift deletion).
 */
#ifndef ODIN3_UTIL_U64MAP_H
#define ODIN3_UTIL_U64MAP_H

#include "odin3/odin3.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct odin3_kv {
    uint64_t key;
    uint64_t value;
} odin3_kv;

typedef struct odin3_u64map odin3_u64map; /* opaque */

/* Empty map with room for at least initial_cap entries (0 = default); NULL on out of memory. */
odin3_u64map *odin3_u64map_create(size_t initial_cap);

/* Frees the map; NULL is allowed. */
void odin3_u64map_destroy(odin3_u64map *map);

/* Inserts or overwrites; any key is valid, including 0. On out of memory the map is intact. */
odin3_status odin3_u64map_put(odin3_u64map *map, odin3_kv entry);

/* Looks up key; stores the value in *value when found (value may be NULL). */
bool odin3_u64map_get(const odin3_u64map *map, uint64_t key, uint64_t *value);

/* Removes key; false if it was absent. */
bool odin3_u64map_remove(odin3_u64map *map, uint64_t key);

size_t odin3_u64map_count(const odin3_u64map *map);

/*
 * Iteration: start with *cursor = 0, call until false. Order is unspecified. The map must not
 * be modified between the first and last call (debug assert).
 */
bool odin3_u64map_next(const odin3_u64map *map, size_t *cursor, odin3_kv *entry);

/* Test hooks (hidden, not part of the ABI). NULL restores odin3_hash_u64. */
void odin3_u64map_test_set_hash(uint64_t (*fn)(uint64_t));
uint64_t odin3_u64map_test_modcount(const odin3_u64map *map);

#endif
