/*
 * sigcache.h — a map from a signal (an array of odin3_fsm_sig) to a small value, for the memo of
 * fsm_detect's mux-tree check (Yosys dict<SigSpec, bool>, fsm_detect.cc:35). Keys are compared
 * element by element; a hash only picks the chain. Failures: NO_MEMORY with the cache valid.
 */
#ifndef ODIN3_PASSES_FSM_SIGCACHE_H
#define ODIN3_PASSES_FSM_SIGCACHE_H

#include "odin3/odin3.h"
#include "passes/fsm/index.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct odin3_fsm_sigcache {
    odin3_u64map *map; /* hash -> index + 1 of the newest entry of that hash */
    odin3_vec ents;    /* entries */
    odin3_vec bits;    /* the keys' signal bits */
} odin3_fsm_sigcache;

odin3_status odin3_fsm_sigcache_init(odin3_fsm_sigcache *cache);
void odin3_fsm_sigcache_free(odin3_fsm_sigcache *cache);

/* Forgets every entry (keeps the allocations). NO_MEMORY if the map cannot be recreated. */
odin3_status odin3_fsm_sigcache_clear(odin3_fsm_sigcache *cache);

/* True and *value when key is present. */
bool odin3_fsm_sigcache_get(const odin3_fsm_sigcache *cache, odin3_fsm_sigref key, uint32_t *value);

/* Adds key (not present) with value. */
odin3_status odin3_fsm_sigcache_put(odin3_fsm_sigcache *cache, odin3_fsm_sigref key,
                                    uint32_t value);

#endif
