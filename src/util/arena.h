/*
 * arena.h — bump allocator: pointers never move, everything is freed at once.
 */
#ifndef ODIN3_UTIL_ARENA_H
#define ODIN3_UTIL_ARENA_H

#include <stddef.h>

typedef struct odin3_arena odin3_arena;

/* chunk_bytes 0 means 64 KiB. Allocates the first chunk eagerly; NULL on out of memory. */
odin3_arena *odin3_arena_create(size_t chunk_bytes);

/* Frees the arena and every allocation made from it. NULL is a no-op. */
void odin3_arena_destroy(odin3_arena *arena);

/*
 * Zeroed, max_align_t-aligned block that never moves. 0 bytes returns a unique non-NULL
 * pointer. Requests larger than the chunk size get their own chunk. NULL on overflow or OOM.
 */
void *odin3_arena_alloc(odin3_arena *arena, size_t bytes);

/* NUL-terminated copy of the first `len` bytes of `src` (src may be NULL when len is 0); NULL on
 * overflow or OOM. */
char *odin3_arena_strndup(odin3_arena *arena, const char *src, size_t len);

/* Bytes handed out (after alignment rounding) and bytes of chunk payload reserved. */
size_t odin3_arena_bytes_used(const odin3_arena *arena);
size_t odin3_arena_bytes_reserved(const odin3_arena *arena);

#endif
