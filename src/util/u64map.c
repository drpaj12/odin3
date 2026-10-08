/*
 * u64map.c — Robin Hood hash map from uint64_t to uint64_t.
 *
 * Probing scheme (idindex reuses it on different slots):
 *   - capacity is a power of two; home slot = hash & mask; probing is linear, wrapping.
 *   - each slot stores psl = probe distance from home + 1; psl == 0 means the slot is empty.
 *   - insert: walk from home carrying (entry, psl). At a slot with a smaller psl, swap (rich
 *     entries give way to poor ones) and keep carrying the displaced entry; stop at an empty slot.
 *   - lookup: walk from home with psl 1, 2, ...; stop at an empty slot or at a slot whose psl is
 *     smaller than ours (the key would have displaced it, so it is absent).
 *   - delete: backward shift. Pull each following slot back by one (psl - 1) until reaching an
 *     empty slot or one already at home (psl == 1), then empty the last hole. No tombstones.
 *   - grow (double, rehash) before the load would exceed 85%.
 */
#include "util/u64map.h"

#include "util/alloc.h"
#include "util/hash.h"

#include <assert.h>

enum { MAP_MIN_CAP = 16, LOAD_NUM = 85, LOAD_DEN = 100 };

typedef struct map_slot {
    uint64_t key;
    uint64_t value;
    uint32_t psl; /* probe distance from home + 1; 0 = empty */
} map_slot;

struct odin3_u64map {
    map_slot *slots;
    size_t cap; /* power of two */
    size_t count;
    uint64_t modcount;
    uint64_t iter_modcount; /* modcount seen when the current iteration started */
};

static uint64_t (*g_hash)(uint64_t) = odin3_hash_u64;

void odin3_u64map_test_set_hash(uint64_t (*fn)(uint64_t)) {
    g_hash = fn != NULL ? fn : odin3_hash_u64;
}

uint64_t odin3_u64map_test_modcount(const odin3_u64map *map) {
    return map->modcount;
}

static size_t home_of(const odin3_u64map *map, uint64_t key) {
    return (size_t)g_hash(key) & (map->cap - 1);
}

static size_t max_entries(size_t cap) {
    return cap / LOAD_DEN * LOAD_NUM + cap % LOAD_DEN * LOAD_NUM / LOAD_DEN;
}

/* Smallest power-of-two capacity (>= MAP_MIN_CAP) that holds `entries`; 0 on overflow. */
static size_t cap_for(size_t entries) {
    size_t cap = MAP_MIN_CAP;
    while (max_entries(cap) < entries) {
        if (cap > SIZE_MAX / 2 / sizeof(map_slot)) {
            return 0;
        }
        cap *= 2;
    }
    return cap;
}

odin3_u64map *odin3_u64map_create(size_t initial_cap) {
    size_t cap = cap_for(initial_cap);
    if (cap == 0) {
        return NULL;
    }
    odin3_u64map *map = odin3_util_calloc(sizeof *map);
    if (map == NULL) {
        return NULL;
    }
    map->slots = odin3_util_calloc(cap * sizeof(map_slot));
    if (map->slots == NULL) {
        odin3_util_free(map);
        return NULL;
    }
    map->cap = cap;
    return map;
}

void odin3_u64map_destroy(odin3_u64map *map) {
    if (map == NULL) {
        return;
    }
    odin3_util_free(map->slots);
    odin3_util_free(map);
}

size_t odin3_u64map_count(const odin3_u64map *map) {
    return map->count;
}

/* Index of key's slot, or map->cap if absent. */
static size_t find_index(const odin3_u64map *map, uint64_t key) {
    size_t mask = map->cap - 1;
    size_t idx = home_of(map, key);
    for (uint32_t psl = 1;; psl++) {
        const map_slot *slot = &map->slots[idx];
        if (slot->psl == 0 || slot->psl < psl) {
            return map->cap;
        }
        if (slot->key == key) {
            return idx;
        }
        idx = (idx + 1) & mask;
    }
}

bool odin3_u64map_get(const odin3_u64map *map, uint64_t key, uint64_t *value) {
    size_t idx = find_index(map, key);
    if (idx == map->cap) {
        return false;
    }
    if (value != NULL) {
        *value = map->slots[idx].value;
    }
    return true;
}

/* Robin Hood insert of a key known to be absent; the table must have a free slot. */
static void insert_new(odin3_u64map *map, odin3_kv entry) {
    size_t mask = map->cap - 1;
    size_t idx = home_of(map, entry.key);
    map_slot carry = {entry.key, entry.value, 1};
    for (;; idx = (idx + 1) & mask) {
        map_slot *slot = &map->slots[idx];
        if (slot->psl == 0) {
            *slot = carry;
            break;
        }
        if (slot->psl < carry.psl) {
            map_slot displaced = *slot;
            *slot = carry;
            carry = displaced;
        }
        carry.psl++;
    }
    map->count++;
}

/* Doubles the capacity and reinserts everything; the map is untouched on failure. */
static odin3_status grow(odin3_u64map *map) {
    if (map->cap > SIZE_MAX / 2 / sizeof(map_slot)) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_u64map bigger = {0};
    bigger.cap = map->cap * 2;
    bigger.slots = odin3_util_calloc(bigger.cap * sizeof(map_slot));
    if (bigger.slots == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (size_t i = 0; i < map->cap; i++) {
        const map_slot *slot = &map->slots[i];
        if (slot->psl != 0) {
            insert_new(&bigger, (odin3_kv){slot->key, slot->value});
        }
    }
    odin3_util_free(map->slots);
    map->slots = bigger.slots;
    map->cap = bigger.cap;
    return ODIN3_OK;
}

odin3_status odin3_u64map_put(odin3_u64map *map, odin3_kv entry) {
    size_t idx = find_index(map, entry.key);
    if (idx != map->cap) {
        map->slots[idx].value = entry.value;
        map->modcount++;
        return ODIN3_OK;
    }
    if (map->count + 1 > max_entries(map->cap)) {
        odin3_status status = grow(map);
        if (status != ODIN3_OK) {
            return status;
        }
    }
    insert_new(map, entry);
    map->modcount++;
    return ODIN3_OK;
}

bool odin3_u64map_remove(odin3_u64map *map, uint64_t key) {
    size_t idx = find_index(map, key);
    if (idx == map->cap) {
        return false;
    }
    size_t mask = map->cap - 1;
    for (;;) {
        size_t next = (idx + 1) & mask;
        if (map->slots[next].psl <= 1) {
            break;
        }
        map->slots[idx] = map->slots[next];
        map->slots[idx].psl--;
        idx = next;
    }
    map->slots[idx].psl = 0;
    map->count--;
    map->modcount++;
    return true;
}

bool odin3_u64map_next(const odin3_u64map *map, size_t *cursor, odin3_kv *entry) {
    odin3_u64map *iter = (odin3_u64map *)map; /* iteration bookkeeping only */
    if (*cursor == 0) {
        iter->iter_modcount = map->modcount;
    }
    assert(map->iter_modcount == map->modcount && "u64map modified during iteration");
    while (*cursor < map->cap) {
        const map_slot *slot = &map->slots[*cursor];
        (*cursor)++;
        if (slot->psl != 0) {
            entry->key = slot->key;
            entry->value = slot->value;
            return true;
        }
    }
    return false;
}
