/*
 * pinpool.h — per-module size-class pool for net pin arrays (IR §3 Net, IR-18). Private to src/ir.
 */
#ifndef ODIN3_IR_PINPOOL_H
#define ODIN3_IR_PINPOOL_H

#include "ir/ids.h"
#include "odin3/odin3.h"
#include "util/arena.h"

#include <stddef.h>
#include <stdint.h>

/* Size classes 0 .. ODIN3_PINPOOL_CLASSES-1; class c holds 2 << c pin IDs (2, 4, 8, ... 2^31). */
enum { ODIN3_PINPOOL_CLASSES = 31 };

/*
 * Blocks are carved from the pool's arena and never returned to the system before the pool is
 * destroyed; a released block goes on its class's free list (linked through the block's first
 * bytes) and is handed out again before the arena grows.
 */
typedef struct odin3_pinpool {
    odin3_arena *arena;
    void *free_heads[ODIN3_PINPOOL_CLASSES];
} odin3_pinpool;

/* Initialises an empty pool; ODIN3_ERR_NO_MEMORY on out of memory (pool left destroyable). */
odin3_status odin3_pinpool_init(odin3_pinpool *pool);

/* Frees every block. Safe on a zeroed or partly initialised pool. */
void odin3_pinpool_destroy(odin3_pinpool *pool);

/* Capacity in pin IDs of class cls (< ODIN3_PINPOOL_CLASSES). */
uint32_t odin3_pinpool_capacity(uint32_t cls);

/* A block of class cls, from the free list if one is there; NULL on out of memory. */
odin3_pin_id *odin3_pinpool_alloc(odin3_pinpool *pool, uint32_t cls);

/*
 * Makes the next `count` odin3_pinpool_alloc calls for class 0 (a net's first pin) unable to fail
 * by putting blocks on the free list. ODIN3_ERR_NO_MEMORY on out of memory; blocks already added
 * stay free.
 */
odin3_status odin3_pinpool_reserve_class0(odin3_pinpool *pool, uint32_t count);

/* Returns a block obtained from odin3_pinpool_alloc with the same class. */
void odin3_pinpool_release(odin3_pinpool *pool, odin3_pin_id *block, uint32_t cls);

/* Bytes the pool has taken from the system (its arena's reservation). */
size_t odin3_pinpool_bytes_reserved(const odin3_pinpool *pool);

#endif
