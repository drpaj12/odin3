/* pinpool.c — size-class blocks for net pin arrays, with a free list per class. */
#include "ir/pinpool.h"

#include "util/arena.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

/* Small chunks keep an empty module cheap; oversize requests get a chunk of their own. */
enum { PINPOOL_CHUNK_BYTES = 2048 };

/* A free block stores the next free block's address in its first bytes (class 0 is 8 bytes). */
_Static_assert(sizeof(void *) <= 2 * sizeof(odin3_pin_id), "class-0 block cannot hold a link");

odin3_status odin3_pinpool_init(odin3_pinpool *pool) {
    memset(pool, 0, sizeof *pool);
    pool->arena = odin3_arena_create(PINPOOL_CHUNK_BYTES);
    return pool->arena != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
}

void odin3_pinpool_destroy(odin3_pinpool *pool) {
    odin3_arena_destroy(pool->arena);
    memset(pool, 0, sizeof *pool);
}

uint32_t odin3_pinpool_capacity(uint32_t cls) {
    assert(cls < ODIN3_PINPOOL_CLASSES);
    return UINT32_C(2) << cls;
}

odin3_pin_id *odin3_pinpool_alloc(odin3_pinpool *pool, uint32_t cls) {
    assert(cls < ODIN3_PINPOOL_CLASSES);
    void *head = pool->free_heads[cls];
    if (head != NULL) {
        void *next = NULL;
        memcpy((void *)&next, head, sizeof next);
        pool->free_heads[cls] = next;
        return head;
    }
    size_t cap = odin3_pinpool_capacity(cls);
    if (cap > SIZE_MAX / sizeof(odin3_pin_id)) {
        return NULL;
    }
    return odin3_arena_alloc(pool->arena, cap * sizeof(odin3_pin_id));
}

void odin3_pinpool_release(odin3_pinpool *pool, odin3_pin_id *block, uint32_t cls) {
    assert(cls < ODIN3_PINPOOL_CLASSES && block != NULL);
    void *next = pool->free_heads[cls];
    memcpy(block, (const void *)&next, sizeof next);
    pool->free_heads[cls] = block;
}

size_t odin3_pinpool_bytes_reserved(const odin3_pinpool *pool) {
    return odin3_arena_bytes_reserved(pool->arena);
}
