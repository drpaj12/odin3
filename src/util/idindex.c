/*
 * idindex.c — ID-only hash-cons index (Robin Hood open addressing).
 *
 * Same probing scheme as u64map.c, on slots {hash, id, psl}:
 *   - capacity is a power of two; home slot = hash & mask; probing is linear, wrapping.
 *   - each slot stores psl = probe distance from home + 1; psl == 0 means the slot is empty.
 *   - insert: walk from home carrying (entry, psl). At a slot with a smaller psl, swap (rich
 *     entries give way to poor ones) and keep carrying the displaced entry; stop at an empty slot.
 *   - lookup: walk from home with psl 1, 2, ...; stop at an empty slot or at a slot whose psl is
 *     smaller than ours. A slot matches when its hash equals the lookup's and the caller's eq
 *     callback accepts its ID against the probe bytes.
 *   - remove: matches on hash and ID (no callback), then backward shift: pull each following slot
 *     back by one (psl - 1) until reaching an empty slot or one already at home (psl == 1), then
 *     empty the last hole. No tombstones.
 *   - grow (double, rehash from the stored hashes, never calling back) before the load would
 *     exceed 85%.
 */
#include "util/idindex.h"

#include "util/alloc.h"

enum { IX_MIN_CAP = 16, LOAD_NUM = 85, LOAD_DEN = 100 };

typedef struct ix_slot {
    uint64_t hash;
    uint32_t id;
    uint32_t psl; /* probe distance from home + 1; 0 = empty */
} ix_slot;

struct odin3_idindex {
    ix_slot *slots;
    size_t cap; /* power of two */
    size_t count;
};

static size_t max_entries(size_t cap) {
    return cap / LOAD_DEN * LOAD_NUM + cap % LOAD_DEN * LOAD_NUM / LOAD_DEN;
}

/* Smallest power-of-two capacity (>= IX_MIN_CAP) that holds `entries`; 0 on overflow. */
static size_t cap_for(size_t entries) {
    size_t cap = IX_MIN_CAP;
    while (max_entries(cap) < entries) {
        if (cap > SIZE_MAX / 2 / sizeof(ix_slot)) {
            return 0;
        }
        cap *= 2;
    }
    return cap;
}

odin3_idindex *odin3_idindex_create(size_t initial_cap) {
    size_t cap = cap_for(initial_cap);
    if (cap == 0) {
        return NULL;
    }
    odin3_idindex *ix = odin3_util_calloc(sizeof *ix);
    if (ix == NULL) {
        return NULL;
    }
    ix->slots = odin3_util_calloc(cap * sizeof(ix_slot));
    if (ix->slots == NULL) {
        odin3_util_free(ix);
        return NULL;
    }
    ix->cap = cap;
    return ix;
}

void odin3_idindex_destroy(odin3_idindex *ix) {
    if (ix == NULL) {
        return;
    }
    odin3_util_free(ix->slots);
    odin3_util_free(ix);
}

size_t odin3_idindex_count(const odin3_idindex *ix) {
    return ix->count;
}

/* Index of the slot accepted by cmp, or ix->cap if none. */
static size_t find_index(const odin3_idindex *ix, const odin3_idcmp *cmp) {
    size_t mask = ix->cap - 1;
    size_t idx = (size_t)cmp->hash & mask;
    for (uint32_t psl = 1;; psl++) {
        const ix_slot *slot = &ix->slots[idx];
        if (slot->psl == 0 || slot->psl < psl) {
            return ix->cap;
        }
        if (slot->hash == cmp->hash && cmp->eq(cmp->ctx, slot->id, cmp->probe)) {
            return idx;
        }
        idx = (idx + 1) & mask;
    }
}

bool odin3_idindex_find(const odin3_idindex *ix, const odin3_idcmp *cmp, uint32_t *id) {
    size_t idx = find_index(ix, cmp);
    if (idx == ix->cap) {
        return false;
    }
    if (id != NULL) {
        *id = ix->slots[idx].id;
    }
    return true;
}

/* Robin Hood insert of an entry known to be absent; the table must have a free slot. */
static void insert_new(odin3_idindex *ix, odin3_identry entry) {
    size_t mask = ix->cap - 1;
    size_t idx = (size_t)entry.hash & mask;
    ix_slot carry = {entry.hash, entry.id, 1};
    for (;; idx = (idx + 1) & mask) {
        ix_slot *slot = &ix->slots[idx];
        if (slot->psl == 0) {
            *slot = carry;
            break;
        }
        if (slot->psl < carry.psl) {
            ix_slot displaced = *slot;
            *slot = carry;
            carry = displaced;
        }
        carry.psl++;
    }
    ix->count++;
}

/* Doubles the capacity and reinserts everything; the index is untouched on failure. */
static odin3_status grow(odin3_idindex *ix) {
    if (ix->cap > SIZE_MAX / 2 / sizeof(ix_slot)) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_idindex bigger = {0};
    bigger.cap = ix->cap * 2;
    bigger.slots = odin3_util_calloc(bigger.cap * sizeof(ix_slot));
    if (bigger.slots == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (size_t i = 0; i < ix->cap; i++) {
        const ix_slot *slot = &ix->slots[i];
        if (slot->psl != 0) {
            insert_new(&bigger, (odin3_identry){slot->hash, slot->id});
        }
    }
    odin3_util_free(ix->slots);
    ix->slots = bigger.slots;
    ix->cap = bigger.cap;
    return ODIN3_OK;
}

odin3_status odin3_idindex_insert(odin3_idindex *ix, odin3_identry entry) {
    if (ix->count + 1 > max_entries(ix->cap)) {
        odin3_status status = grow(ix);
        if (status != ODIN3_OK) {
            return status;
        }
    }
    insert_new(ix, entry);
    return ODIN3_OK;
}

/* Index of the slot holding exactly this hash and id, or ix->cap if none. */
static size_t find_entry(const odin3_idindex *ix, odin3_identry entry) {
    size_t mask = ix->cap - 1;
    size_t idx = (size_t)entry.hash & mask;
    for (uint32_t psl = 1;; psl++) {
        const ix_slot *slot = &ix->slots[idx];
        if (slot->psl == 0 || slot->psl < psl) {
            return ix->cap;
        }
        if (slot->hash == entry.hash && slot->id == entry.id) {
            return idx;
        }
        idx = (idx + 1) & mask;
    }
}

bool odin3_idindex_remove(odin3_idindex *ix, odin3_identry entry) {
    size_t idx = find_entry(ix, entry);
    if (idx == ix->cap) {
        return false;
    }
    size_t mask = ix->cap - 1;
    for (;;) {
        size_t next = (idx + 1) & mask;
        if (ix->slots[next].psl <= 1) {
            break;
        }
        ix->slots[idx] = ix->slots[next];
        ix->slots[idx].psl--;
        idx = next;
    }
    ix->slots[idx].psl = 0;
    ix->count--;
    return true;
}
