/* arena.c — chunked bump allocator; each chunk is one allocation of header plus payload. */
#include "util/arena.h"

#include "util/alloc.h"

#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { ARENA_DEFAULT_CHUNK = 64 * 1024, ARENA_ALIGN = _Alignof(max_align_t) };

typedef struct arena_chunk {
    struct arena_chunk *next;
    size_t cap;  /* payload bytes */
    size_t used; /* payload bytes handed out */
} arena_chunk;

struct odin3_arena {
    arena_chunk *head; /* chunk currently bumped */
    size_t chunk_bytes;
    size_t bytes_used;
    size_t bytes_reserved;
};

/* Payload starts after the header rounded up to max alignment. */
static size_t chunk_header_bytes(void) {
    return (sizeof(arena_chunk) + ARENA_ALIGN - 1) / ARENA_ALIGN * ARENA_ALIGN;
}

static unsigned char *chunk_payload(arena_chunk *chunk) {
    return (unsigned char *)chunk + chunk_header_bytes();
}

static arena_chunk *chunk_new(size_t cap) {
    if (cap > SIZE_MAX - chunk_header_bytes()) {
        return NULL;
    }
    arena_chunk *chunk = odin3_util_calloc(chunk_header_bytes() + cap);
    if (chunk != NULL) {
        chunk->cap = cap;
    }
    return chunk;
}

/* Rounds up to the alignment, reserving one byte for a zero-byte request; 0 on overflow. */
static size_t round_request(size_t bytes) {
    size_t wanted = bytes == 0 ? 1 : bytes;
    if (wanted > SIZE_MAX - (ARENA_ALIGN - 1)) {
        return 0;
    }
    return (wanted + ARENA_ALIGN - 1) / ARENA_ALIGN * ARENA_ALIGN;
}

odin3_arena *odin3_arena_create(size_t chunk_bytes) {
    odin3_arena *arena = odin3_util_calloc(sizeof *arena);
    if (arena == NULL) {
        return NULL;
    }
    arena->chunk_bytes = chunk_bytes == 0 ? ARENA_DEFAULT_CHUNK : chunk_bytes;
    arena->head = chunk_new(arena->chunk_bytes);
    if (arena->head == NULL) {
        odin3_util_free(arena);
        return NULL;
    }
    arena->bytes_reserved = arena->chunk_bytes;
    return arena;
}

void odin3_arena_destroy(odin3_arena *arena) {
    if (arena == NULL) {
        return;
    }
    arena_chunk *chunk = arena->head;
    while (chunk != NULL) {
        arena_chunk *next = chunk->next;
        odin3_util_free(chunk);
        chunk = next;
    }
    odin3_util_free(arena);
}

/* Takes `size` (already rounded) from a fresh chunk; oversize requests do not displace head. */
static void *alloc_new_chunk(odin3_arena *arena, size_t size) {
    bool oversize = size > arena->chunk_bytes;
    arena_chunk *chunk = chunk_new(oversize ? size : arena->chunk_bytes);
    if (chunk == NULL) {
        return NULL;
    }
    chunk->used = size;
    if (oversize) {
        chunk->next = arena->head->next;
        arena->head->next = chunk;
    } else {
        chunk->next = arena->head;
        arena->head = chunk;
    }
    arena->bytes_reserved += chunk->cap;
    arena->bytes_used += size;
    return chunk_payload(chunk);
}

void *odin3_arena_alloc(odin3_arena *arena, size_t bytes) {
    size_t size = round_request(bytes);
    if (size == 0) {
        return NULL;
    }
    arena_chunk *head = arena->head;
    if (size <= head->cap - head->used) {
        void *ptr = chunk_payload(head) + head->used;
        head->used += size;
        arena->bytes_used += size;
        return ptr;
    }
    return alloc_new_chunk(arena, size);
}

char *odin3_arena_strndup(odin3_arena *arena, const char *src, size_t len) {
    if (len == SIZE_MAX) {
        return NULL;
    }
    char *copy = odin3_arena_alloc(arena, len + 1);
    if (copy != NULL) {
        if (len > 0) {
            memcpy(copy, src, len);
        }
        copy[len] = '\0';
    }
    return copy;
}

size_t odin3_arena_bytes_used(const odin3_arena *arena) {
    return arena->bytes_used;
}

size_t odin3_arena_bytes_reserved(const odin3_arena *arena) {
    return arena->bytes_reserved;
}
