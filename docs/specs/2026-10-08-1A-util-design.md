# 1A — `src/util/`: the core containers

Status: draft for light review (PHASE1 decision #6). Phase 1, sub-project 1A.

## Purpose

`src/util/` is the only place in the tree allowed to define a generic container (spec §15.1). It
provides `arena`, `vec`, `hashmap` (with the project's hash function), `str` (interning and a
string builder) and `log`. Every later sub-project builds on it: 1B stores IR objects in arenas
and vecs, keeps name→ID maps and the hash-consed provenance lineage (PHASE1 #3) in hashmaps,
and interns names and hierarchy paths; 1C–1F write through the string builder and report
through `log`.

Success: the five modules exist with unit tests under ASan/UBSan, pass the full lint gate, and a
benchmark shows 2M interned names plus 2M hashmap entries fit comfortably inside the 2 GB budget
of spec §5.1 (numbers recorded in the PR, not a gate).

## Constraints

- C17, the §15.1 rules and lint limits (complexity ≤ 15, ≤ 60 lines, ≤ 5 parameters).
- No `abort()`/`exit()`; fallible functions return `odin3_status` or a documented sentinel
  (`NULL`, `0`). Out of memory is `ODIN3_ERR_NO_MEMORY`, never a crash.
- No recursion. Single-threaded: no locks; documented on each module.
- Private: nothing here goes into `include/odin3/odin3.h`.

## Approaches considered

**Generic containers in C** — (a) `void *` storage plus an element size, typed at the call site;
(b) macro-generated typed containers (`ODIN3_VEC_DECLARE(node_vec, struct node)`); (c) intrusive
containers. **Chosen: (a).** It is plain C a reviewer reads in one pass, lints cleanly, and one
implementation serves every element type. The cost (a cast at each `get`) is confined to the
accessor functions 1B writes per object kind. (b) hides control flow in macros that clang-tidy
and lizard cannot see; (c) leaks container fields into IR structs.

**Hash map** — (a) open addressing with Robin Hood probing and backward-shift deletion;
(b) separate chaining. **Chosen: (a).** One flat allocation, no per-entry malloc, good cache
behaviour at 2M entries, and deletion without tombstones.

**Map keys and values** — one map type keyed by byte strings (copied into the map's own arena)
with a `uint64_t` value. Names are byte strings; provenance records are hash-consed by
serializing them to bytes. A map keyed directly by `uint64_t` (ID → ID) is a second, smaller
type, `odin3_u64map`, because converting integers to byte strings everywhere is wasteful.

## Library structure and symbol visibility

- `src/util/*.c` and the existing `src/api/*.c` move into a static, position-independent
  library `odin3_core`. `libodin3.so` is built from it with `-fvisibility=hidden`; each public
  function's *definition* in `src/api/` carries `ODIN3_EXPORT` (a visibility attribute macro in
  a private header), so `odin3.h` stays free of attributes for the cffi cdef.
- Unit tests link `odin3_core` directly and can test internal functions.
- `tools/check-symbols.sh` gains a second check: the set of symbols `libodin3.so` exports equals
  the set of functions declared between `ODIN3_CDEF_BEGIN`/`END` in `odin3.h`.
- Internal names keep the project rule (decision #23): `odin3_` prefix, snake_case, e.g.
  `odin3_arena_alloc`. Hidden visibility keeps them out of plugins' reach.

## Modules

All sizes and counts are `size_t` in the API; IDs handed out by `str` are `uint32_t`.

### `arena.h` — bump allocator

```c
typedef struct odin3_arena odin3_arena;          /* opaque */
odin3_arena *odin3_arena_create(size_t chunk_bytes);   /* 0 → 64 KiB default; NULL on OOM */
void odin3_arena_destroy(odin3_arena *arena);          /* frees every chunk; NULL is a no-op */
void *odin3_arena_alloc(odin3_arena *arena, size_t bytes);         /* max_align_t-aligned, zeroed */
void *odin3_arena_alloc_aligned(odin3_arena *arena, size_t bytes, size_t align);
char *odin3_arena_strndup(odin3_arena *arena, const char *s, size_t len);   /* NUL-terminated */
void odin3_arena_reset(odin3_arena *arena);            /* keeps the first chunk, drops the rest */
size_t odin3_arena_bytes_used(const odin3_arena *arena);
size_t odin3_arena_bytes_reserved(const odin3_arena *arena);
```

Allocations never move. Requests larger than the chunk size get their own chunk. Memory is
zeroed so IR structs start in a known state. Overflow in size arithmetic returns `NULL`.

### `vec.h` — growable contiguous array

```c
typedef struct odin3_vec { void *data; size_t len, cap, elem_size; } odin3_vec;
void odin3_vec_init(odin3_vec *v, size_t elem_size);
void odin3_vec_free(odin3_vec *v);
odin3_status odin3_vec_reserve(odin3_vec *v, size_t cap);
void *odin3_vec_push(odin3_vec *v);                 /* zeroed slot, or NULL on OOM */
void *odin3_vec_get(const odin3_vec *v, size_t i);  /* assert(i < len) */
void odin3_vec_pop(odin3_vec *v);                   /* assert(len > 0) */
void odin3_vec_clear(odin3_vec *v);                 /* len = 0, keeps capacity */
```

Growth doubles capacity (minimum 8). Pointers into `data` are invalidated by growth; callers
that need stable addresses use the arena or hold indices (the IR holds IDs, spec §5.1). The
struct is public so it can be embedded by value in IR structs; code outside `vec.c` may read
`data` and `len` but never writes any field.

### `hash.h` and `hashmap.h` — hashing and maps

```c
uint64_t odin3_hash_bytes(const void *data, size_t len, uint64_t seed);
uint64_t odin3_hash_u64(uint64_t x);    /* fmix64 finalizer */

typedef struct odin3_hashmap odin3_hashmap;     /* opaque; key = byte string, value = uint64_t */
odin3_hashmap *odin3_hashmap_create(size_t initial_cap);
void odin3_hashmap_destroy(odin3_hashmap *map);
odin3_status odin3_hashmap_put(odin3_hashmap *map, const void *key, size_t len, uint64_t value);
bool odin3_hashmap_get(const odin3_hashmap *map, const void *key, size_t len, uint64_t *value);
bool odin3_hashmap_remove(odin3_hashmap *map, const void *key, size_t len);
size_t odin3_hashmap_count(const odin3_hashmap *map);
/* iteration: cursor starts at 0; returns false when done; order is unspecified */
bool odin3_hashmap_next(const odin3_hashmap *map, size_t *cursor,
                        const void **key, size_t *len, uint64_t *value);
```

`odin3_u64map` has the same operations with a `uint64_t` key. The hash is 64-bit FNV-1a with an
`fmix64` finalizer: short, dependency-free, and adequate for names; it can be swapped behind
`odin3_hash_bytes` if the 1B benchmark shows it matters. Load factor ≤ 0.85, then double.
Iteration order is unspecified; anything that writes files sorts or uses insertion-ordered vecs
(writers must be deterministic, PHASE1 #2).

### `str.h` — interning and building strings

```c
typedef struct odin3_strtab odin3_strtab;    /* string interning: bytes ↔ uint32_t ID */
odin3_strtab *odin3_strtab_create(void);
void odin3_strtab_destroy(odin3_strtab *tab);
odin3_status odin3_strtab_intern(odin3_strtab *tab, const char *s, size_t len, uint32_t *id);
bool odin3_strtab_find(const odin3_strtab *tab, const char *s, size_t len, uint32_t *id);
const char *odin3_strtab_get(const odin3_strtab *tab, uint32_t id);  /* NUL-terminated, stable */
size_t odin3_strtab_len(const odin3_strtab *tab, uint32_t id);
size_t odin3_strtab_count(const odin3_strtab *tab);

typedef struct odin3_strbuf { char *data; size_t len, cap; } odin3_strbuf;   /* builder */
void odin3_strbuf_init(odin3_strbuf *b);
void odin3_strbuf_free(odin3_strbuf *b);
odin3_status odin3_strbuf_append(odin3_strbuf *b, const char *s, size_t len);
odin3_status odin3_strbuf_appendf(odin3_strbuf *b, const char *fmt, ...);   /* printf format */
void odin3_strbuf_clear(odin3_strbuf *b);
```

ID 0 is reserved for the empty string, so a zeroed struct field means "no name". Interned
strings live in the table's arena and never move; IDs are dense and stable for the table's
life. Strings may contain any bytes except NUL.

### `log.h` — diagnostics

```c
typedef enum odin3_log_level { ODIN3_LOG_ERROR, ODIN3_LOG_WARN, ODIN3_LOG_INFO,
                               ODIN3_LOG_DEBUG } odin3_log_level;
typedef void (*odin3_log_sink)(odin3_log_level level, const char *msg, void *user);
void odin3_log_set_level(odin3_log_level max_level);    /* default: INFO */
void odin3_log_set_sink(odin3_log_sink sink, void *user);   /* NULL → stderr */
void odin3_log(odin3_log_level level, const char *fmt, ...);
size_t odin3_log_count(odin3_log_level level);     /* messages emitted since last reset */
void odin3_log_reset_counts(void);
```

Process-global state (one logger per process) — the simplest thing that serves the CLI, tests
and plugins in Phase 1. Messages are formatted into a fixed 1 KiB stack buffer and truncated
with `...`. Errors are counted even when filtered out by level, so a pass can ask "did
anything fail?". A sink lets tests capture output and lets the C ABI forward messages to Python
later (1D). Source locations in messages are the caller's job (provenance formatting is 1B).

## Testing

- One Unity test file per module, `tests/unit/test_util_<module>.c`, run under the `debug`
  preset (ASan + UBSan) in CI, labelled `unit`.
- Edge cases: zero-size and huge (overflowing) requests; chunk-sized and over-chunk-sized arena
  allocations; vec growth from empty and across reallocation; hashmap with colliding hashes
  (a test-only seed forces collisions), delete-then-reinsert, growth during a long probe
  sequence, empty keys; strtab round trips including embedded high bytes; strbuf `appendf`
  longer than the current capacity; log truncation, level filtering and counts.
- A randomized differential test for `hashmap`/`u64map`: 100k random operations against a
  simple reference (sorted array), fixed seed so failures reproduce.
- Out-of-memory paths: every `util` allocation goes through one internal wrapper
  (`odin3_util_malloc`/`realloc`/`free`). A hidden internal hook,
  `odin3_util_set_alloc_fail_after(n)`, makes the n-th allocation fail (a counter check costs
  nothing measurable; it is not exported from `libodin3.so`). Tests check that every function
  returns its documented failure without leaking (ASan).
- Benchmark `tests/bench/bench_util.c` (not in CTest by default): intern 2M distinct names and
  insert 2M hashmap entries; print time and `bytes_reserved`. Numbers go in the PR.

## Out of scope

IR structures and IDs (1B), any thread safety, file I/O helpers (each reader/writer owns its
I/O), and a dynamic bitset (added in 1E if the simulator needs it).
