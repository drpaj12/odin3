/*
 * sigcache.c — see sigcache.h.
 */
#include "passes/fsm/sigcache.h"

#include "odin3/odin3.h"
#include "passes/fsm/index.h"
#include "util/hash.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct entry {
    uint32_t off, len, value, next; /* next: index + 1 of the previous entry of this hash */
} entry;

odin3_status odin3_fsm_sigcache_init(odin3_fsm_sigcache *cache) {
    memset(cache, 0, sizeof *cache);
    odin3_vec_init(&cache->ents, sizeof(entry));
    odin3_vec_init(&cache->bits, sizeof(odin3_fsm_sig));
    cache->map = odin3_u64map_create(0);
    return cache->map != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
}

void odin3_fsm_sigcache_free(odin3_fsm_sigcache *cache) {
    odin3_u64map_destroy(cache->map);
    odin3_vec_free(&cache->ents);
    odin3_vec_free(&cache->bits);
    memset(cache, 0, sizeof *cache);
}

odin3_status odin3_fsm_sigcache_clear(odin3_fsm_sigcache *cache) {
    odin3_u64map *fresh = odin3_u64map_create(0);
    if (fresh == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_u64map_destroy(cache->map);
    cache->map = fresh;
    odin3_vec_clear(&cache->ents);
    odin3_vec_clear(&cache->bits);
    return ODIN3_OK;
}

static uint64_t hash_of(odin3_fsm_sigref key) {
    odin3_bytes bytes = {key.sig, (size_t)key.len * sizeof *key.sig};
    return odin3_hash_bytes(bytes, ODIN3_HASH_SEED);
}

static bool entry_equals(const odin3_fsm_sigcache *cache, const entry *ent, odin3_fsm_sigref key) {
    const odin3_fsm_sig *bits = cache->bits.data;
    return ent->len == key.len &&
           (key.len == 0 || memcmp(bits + ent->off, key.sig, (size_t)key.len * sizeof *bits) == 0);
}

bool odin3_fsm_sigcache_get(const odin3_fsm_sigcache *cache, odin3_fsm_sigref key,
                            uint32_t *value) {
    uint64_t head = 0;
    if (!odin3_u64map_get(cache->map, hash_of(key), &head)) {
        return false;
    }
    for (uint64_t at = head; at != 0;) {
        const entry *ent = odin3_vec_cat(&cache->ents, (size_t)at - 1);
        if (entry_equals(cache, ent, key)) {
            *value = ent->value;
            return true;
        }
        at = ent->next;
    }
    return false;
}

odin3_status odin3_fsm_sigcache_put(odin3_fsm_sigcache *cache, odin3_fsm_sigref key,
                                    uint32_t value) {
    uint64_t hash = hash_of(key);
    uint64_t head = 0;
    (void)odin3_u64map_get(cache->map, hash, &head);
    entry *ent = odin3_vec_push(&cache->ents);
    if (ent == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    ent->off = (uint32_t)cache->bits.len;
    ent->len = key.len;
    ent->value = value;
    ent->next = (uint32_t)head;
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < key.len; i++) {
        odin3_fsm_sig *slot = odin3_vec_push(&cache->bits);
        if (slot == NULL) {
            st = ODIN3_ERR_NO_MEMORY;
        } else {
            *slot = key.sig[i];
        }
    }
    odin3_kv kv = {hash, cache->ents.len};
    st = st == ODIN3_OK ? odin3_u64map_put(cache->map, kv) : st;
    if (st != ODIN3_OK) {
        /* undo: drop the entry and the bits it took */
        const entry *last = odin3_vec_cat(&cache->ents, cache->ents.len - 1);
        while (cache->bits.len > last->off) {
            odin3_vec_pop(&cache->bits);
        }
        odin3_vec_pop(&cache->ents);
    }
    return st;
}
