# 1A — `src/util/`: the core containers

Status: draft for light review (PHASE1 decision #6); revised after the design critique.
Phase 1, sub-project 1A.

## Purpose

`src/util/` is the only place in the tree allowed to define a generic container (spec §15.1).
Every later sub-project builds on it: 1B stores IR objects in paged arrays with `uint32_t` IDs,
keeps name→ID maps, and hash-conses the provenance lineage records (PHASE1 #3); 1C–1F write
through the string builder and report through `log`.

Success: the modules below exist with unit tests under ASan/UBSan and pass the full lint gate;
a benchmark (numbers recorded in the PR, not a gate) shows 2M interned names plus 2M map entries
and a 2M-element paged array fit comfortably in the 2 GB budget of spec §5.1.

## Constraints

- C17, the §15.1 rules and lint limits (complexity ≤ 15, ≤ 60 lines, ≤ 5 parameters, and the
  enabled clang-tidy checks, notably `bugprone-easily-swappable-parameters`: no two adjacent
  parameters of convertible types — keys travel as structs).
- No `abort()`/`exit()`; fallible functions return `odin3_status` or a documented sentinel
  (`NULL`, `false`). Out of memory, or running out of 32-bit IDs, is `ODIN3_ERR_NO_MEMORY`.
- No recursion. Single-threaded: no locks.
- Private: nothing here goes into `include/odin3/odin3.h`.
- Deterministic: no randomness, no address-dependent behaviour visible in output. The hash seed
  is a fixed constant; only a test hook changes it.

## Approaches considered

**Generic containers in C** — (a) `void *` storage plus an element size, typed at the call site;
(b) macro-generated typed containers; (c) intrusive containers. **Chosen: (a)**: plain C a
reviewer reads in one pass, lint-clean, one implementation per container. The casts are confined
to the per-kind accessors 1B writes. (b) hides control flow from clang-tidy and lizard; (c) leaks
container fields into IR structs.

**Hash tables** — open addressing with Robin Hood probing and backward-shift deletion (chosen:
flat, no per-entry allocation, no tombstones) vs separate chaining. `u64map` and `idindex` each
carry their own Robin Hood code (different slot types); extract a shared core only if a third
open-addressing table appears.

**Where keys live** — a byte-string map that copies keys would store every name and provenance
record twice and never reclaim removed keys. **Chosen:** an ID-only hash-cons index
(`odin3_idindex`) whose slots hold `uint32_t` IDs; the caller owns the objects and supplies the
hash and equality. String interning and provenance records both sit on it with no duplicate
storage. Name→ID lookups are then `odin3_u64map`s from a string ID to an object ID.

## Library structure, linking and visibility

- All of `src/` except `src/cli/` compiles into an **object library** `odin3_core` (`-fPIC`).
  `libodin3.so` is built from those objects with `-fvisibility=hidden`; each public function's
  *definition* in `src/api/` carries `ODIN3_EXPORT` (a visibility macro in a private header), so
  `odin3.h` stays attribute-free for the cffi cdef (verified with gcc 13 and clang 18).
- **One home for state.** The CLI and plugins link `libodin3.so` and use only `odin3.h`, as the
  header already requires; anything the CLI needs (log level, readers, passes) becomes public
  ABI in 1D. Global state (the logger now; registries in 1B/1D) therefore exists once per
  process.
- **Tests** link either `odin3_core` (unit tests of internals; they never load plugins) or
  `libodin3.so` (ABI and plugin tests) — never both.
- `tools/check-symbols.sh`, run on `libodin3.so` only, also checks that the exported function
  symbols equal the function declarations between `ODIN3_CDEF_BEGIN`/`END` (typedefs excluded).
- Internal names follow decision #23: `odin3_` prefix, snake_case.

## Modules

### `alloc.h` — the allocation wrapper

`odin3_util_malloc/calloc/realloc/free`; every `util` allocation goes through them. A hidden
hook `odin3_util_set_alloc_fail_after(n)` makes the n-th allocation fail so tests can drive
every out-of-memory path (one counter check per allocation; not exported).

### `arena.h` — bump allocator

```c
typedef struct odin3_arena odin3_arena;                 /* opaque */
odin3_arena *odin3_arena_create(size_t chunk_bytes);    /* 0 → 64 KiB; NULL on OOM */
void odin3_arena_destroy(odin3_arena *arena);           /* NULL is a no-op */
void *odin3_arena_alloc(odin3_arena *arena, size_t bytes);  /* max_align_t-aligned, zeroed */
char *odin3_arena_strndup(odin3_arena *arena, const char *s, size_t len);  /* NUL-terminated */
size_t odin3_arena_bytes_used(const odin3_arena *arena);
size_t odin3_arena_bytes_reserved(const odin3_arena *arena);
```

Allocations never move. A request of 0 bytes returns a unique non-NULL pointer (1 byte is
reserved). Requests larger than the chunk get their own chunk. Size overflow returns `NULL`.

### `vec.h` — growable contiguous array

```c
typedef struct odin3_vec { void *data; size_t len, cap, elem_size; } odin3_vec;
void odin3_vec_init(odin3_vec *v, size_t elem_size);
void odin3_vec_free(odin3_vec *v);
odin3_status odin3_vec_reserve(odin3_vec *v, size_t cap);
void *odin3_vec_push(odin3_vec *v);           /* zeroed slot, or NULL on OOM */
void *odin3_vec_at(odin3_vec *v, size_t i);   /* assert(i < len) */
const void *odin3_vec_cat(const odin3_vec *v, size_t i);
void odin3_vec_pop(odin3_vec *v);             /* assert(len > 0) */
void odin3_vec_clear(odin3_vec *v);           /* len = 0, keeps capacity */
```

Growth doubles capacity (minimum 8) and invalidates pointers into `data`. For small, short-lived
or append-then-read lists (pin lists, writer output). Outside `vec.c` code may read `data` and
`len` but never writes a field.

### `pagevec.h` — paged array: stable addresses, O(1) by index

```c
typedef struct odin3_pagevec odin3_pagevec;     /* opaque */
odin3_pagevec *odin3_pagevec_create(size_t elem_size);   /* pages of 4096 elements */
void odin3_pagevec_destroy(odin3_pagevec *pv);
void *odin3_pagevec_push(odin3_pagevec *pv, size_t *index);   /* zeroed; NULL on OOM */
void *odin3_pagevec_at(odin3_pagevec *pv, size_t i);          /* assert(i < len) */
const void *odin3_pagevec_cat(const odin3_pagevec *pv, size_t i);
size_t odin3_pagevec_len(const odin3_pagevec *pv);
size_t odin3_pagevec_bytes_reserved(const odin3_pagevec *pv);

enum { ODIN3_PAGEVEC_MIN_SHIFT = 4, ODIN3_PAGEVEC_MAX_SHIFT = 16 };
/* page of (1 << page_shift) elements; NULL on OOM or shift outside [MIN, MAX] */
typedef struct odin3_pagevec_spec { size_t elem_size; unsigned page_shift; } odin3_pagevec_spec;
odin3_pagevec *odin3_pagevec_create_paged(odin3_pagevec_spec spec);   /* struct: swappable-params rule */
/* next `extra` pushes cannot fail (allocates pages/page table now); len unchanged */
odin3_status odin3_pagevec_reserve(odin3_pagevec *pv, size_t extra);
/* shrink len to new_len; elements past it are zeroed, pages kept */
void odin3_pagevec_truncate(odin3_pagevec *pv, size_t new_len);
```

`odin3_pagevec_create` is `create_paged` with shift 12. The shift and mask are stored in the struct, so
indexing stays shift, mask and two loads. Small pages (e.g. shift 8) keep the first-page cost low for
per-module stores; `bytes_reserved` reflects the page size. `reserve` gives reserve-before-mutate
(on OOM len is unchanged; pages already allocated are kept), and `truncate` gives rollback without a
wrapper container: a later push returns a zeroed slot at the same address.

Fixed-size pages (`ODIN3_PAGEVEC_PAGE_ELEMS` = 4096) plus a growable page table: elements never
move, lookup by index is a shift, a mask and two loads, and growth never
copies elements, so a 2M-object kind needs no 3× peak. This is the storage 1B evaluates for IR
objects (PHASE1 #5). No removal: the IR marks dead objects; compaction, if ever needed, is 1B's
business.

### `hash.h` — hashing

```c
typedef struct odin3_bytes { const void *ptr; size_t len; } odin3_bytes;
uint64_t odin3_hash_bytes(odin3_bytes key, uint64_t seed);   /* FNV-1a 64 + fmix64 */
uint64_t odin3_hash_u64(uint64_t x);                         /* fmix64 */
uint64_t odin3_hash_combine(uint64_t hash, uint64_t value);       /* odin3_hash_u64(hash ^ value) */
#define ODIN3_HASH_SEED UINT64_C(0x6f64696e33)               /* fixed: "odin3" */
```

`ODIN3_HASH_SEED` is a macro, not an enum: an enum constant cannot exceed `int` in C17, so the
seed is not usable as a case label or array size. `odin3_hash_combine` mixes record fields into
one finalised hash (order-sensitive when chained). `odin3_hash_bytes` requires `ptr != NULL` when
`len > 0`. Short and dependency-free; swappable behind this header if the 1B benchmark says so.

### `idindex.h` — ID-only hash-cons index

```c
typedef struct odin3_idindex odin3_idindex;   /* opaque; slots hold uint32_t IDs + hashes */
typedef bool (*odin3_id_equals)(const void *ctx, uint32_t id, odin3_bytes probe);
typedef struct odin3_idcmp {      /* a lookup: hash + probe bytes + caller's equality */
    uint64_t hash; odin3_bytes probe; odin3_id_equals eq; const void *ctx;
} odin3_idcmp;
typedef struct odin3_identry { uint64_t hash; uint32_t id; } odin3_identry;
odin3_idindex *odin3_idindex_create(size_t initial_cap);
void odin3_idindex_destroy(odin3_idindex *ix);
/* find: true and *id when an entry with cmp->hash satisfies cmp->eq(cmp->ctx, id, cmp->probe) */
bool odin3_idindex_find(const odin3_idindex *ix, const odin3_idcmp *cmp, uint32_t *id);
odin3_status odin3_idindex_insert(odin3_idindex *ix, odin3_identry entry);
bool odin3_idindex_remove(odin3_idindex *ix, odin3_identry entry);
size_t odin3_idindex_count(const odin3_idindex *ix);
```

Lookups and entries travel as structs, so no call has adjacent swappable parameters. The caller computes the hash from its
own representation (the bytes of a string, or a provenance record's serialized fields) and owns
the objects; the index stores 16-byte slots `{hash, id, psl}` (ID + 64-bit hash, kept so growth never calls
back). Load factor ≤ 0.85, then double.

### `u64map.h` — integer map

```c
typedef struct odin3_kv { uint64_t key; uint64_t value; } odin3_kv;
typedef struct odin3_u64map odin3_u64map;     /* opaque */
odin3_u64map *odin3_u64map_create(size_t initial_cap);
void odin3_u64map_destroy(odin3_u64map *map);
odin3_status odin3_u64map_put(odin3_u64map *map, odin3_kv entry);   /* insert or overwrite */
bool odin3_u64map_get(const odin3_u64map *map, uint64_t key, uint64_t *value);
bool odin3_u64map_remove(odin3_u64map *map, uint64_t key);
size_t odin3_u64map_count(const odin3_u64map *map);
bool odin3_u64map_next(const odin3_u64map *map, size_t *cursor, odin3_kv *entry);
```

Any key is valid, including 0 (occupancy is `psl == 0`, the probe sequence length, not a separate per-slot byte). Iteration order is
unspecified; a modification counter makes mutation during iteration a debug assert. Anything
that writes files gets its order from insertion-ordered storage, never from map iteration.

### `str.h` — interning and building strings

```c
typedef struct odin3_strtab odin3_strtab;   /* bytes ↔ uint32_t ID, on idindex + an arena */
odin3_strtab *odin3_strtab_create(void);
void odin3_strtab_destroy(odin3_strtab *tab);
odin3_status odin3_strtab_intern(odin3_strtab *tab, odin3_bytes s, uint32_t *id);
bool odin3_strtab_find(const odin3_strtab *tab, odin3_bytes s, uint32_t *id);
const char *odin3_strtab_get(const odin3_strtab *tab, uint32_t id);   /* NUL-terminated */
size_t odin3_strtab_len(const odin3_strtab *tab, uint32_t id);
size_t odin3_strtab_count(const odin3_strtab *tab);

typedef struct odin3_strbuf { char *data; size_t len, cap; } odin3_strbuf;
void odin3_strbuf_init(odin3_strbuf *b);
void odin3_strbuf_free(odin3_strbuf *b);
odin3_status odin3_strbuf_append(odin3_strbuf *b, odin3_bytes s);
odin3_status odin3_strbuf_appendf(odin3_strbuf *b, const char *fmt, ...) ODIN3_PRINTF(2, 3);
void odin3_strbuf_clear(odin3_strbuf *b);
```

ID 0 is the empty string, so a zeroed field means "no name". Interned strings never move; IDs
are dense and stable for the table's life. Strings may hold any byte except NUL; a NUL inside
`len` is `ODIN3_ERR_INVALID_ARG`. Interned strings are never removed (names are cheap and
provenance keeps old ones alive anyway, PHASE1 #3). `appendf` formats twice with `va_copy`
(measure, grow, write). `ODIN3_PRINTF` expands to `__attribute__((format(printf, f, a)))`.

### `log.h` — diagnostics

```c
typedef enum odin3_log_level { ODIN3_LOG_ERROR, ODIN3_LOG_WARN, ODIN3_LOG_INFO,
                               ODIN3_LOG_DEBUG, ODIN3_LOG_LEVEL_COUNT } odin3_log_level;
typedef void (*odin3_log_sink)(odin3_log_level level, const char *msg, void *user);
void odin3_log_set_level(odin3_log_level max_level);        /* default: INFO */
void odin3_log_set_sink(odin3_log_sink sink, void *user);   /* NULL → stderr */
void odin3_log(odin3_log_level level, const char *fmt, ...) ODIN3_PRINTF(2, 3);
size_t odin3_log_count(odin3_log_level level);   /* per level, including filtered messages */
void odin3_log_reset_counts(void);
```

Process-global, living once in `libodin3.so` (see linking). Messages are formatted into a
1 KiB stack buffer and truncated with `...`. Every level is counted even when filtered, so a
pass can ask "were there warnings?". The sink lets tests capture output; 1D exposes level and
sink through the public ABI for the CLI, plugins and Python.

## Testing

- One Unity file per module, `tests/unit/test_util_<module>.c`, linked against `odin3_core`,
  run under the `debug` preset (ASan + UBSan) in CI, label `unit`.
- Edge cases: zero-size and overflowing requests; chunk-sized and larger arena allocations; vec
  and pagevec growth across page and reallocation boundaries (pagevec addresses stay put);
  idindex and u64map with colliding hashes (a test seed or a constant-hash callback forces
  them), key 0, delete-then-reinsert, growth in the middle of long probe chains; strtab round
  trips with high bytes, the empty string as ID 0, embedded NUL rejected; strbuf `appendf`
  beyond capacity; log truncation, filtering and per-level counts.
- Randomized differential tests for `idindex` and `u64map`: 100k random operations against a
  sorted-array reference, fixed seed.
- Out-of-memory: for each function, fail every allocation in turn via the alloc hook and check
  the documented failure and no leaks (ASan).
- `tests/unit/test_api.c` keeps linking only `libodin3.so`; the export-set check covers the
  visibility change.
- Benchmark `tests/bench/bench_util.c` (built, not run by CTest): intern 2M names, 2M `u64map`
  puts, 2M pushes of a 32-byte struct to a pagevec; print time and bytes reserved.

## Out of scope

IR structures and ID conventions beyond "0 = none" in `strtab` (1B); thread safety; file I/O
helpers; a bitset (1E adds one if the simulator needs it); byte-string maps that copy keys (not
needed once keys are interned).
