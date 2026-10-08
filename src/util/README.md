# src/util — Phase 1
The only generic containers in the tree (spec §15.1): `arena`, `vec`, `hashmap`, `str`, `log`. Nothing else may define a container.

## Modules
- `attr.h` — `ODIN3_EXPORT` (marks the four ABI definitions; everything else is hidden) and `ODIN3_PRINTF(f, a)`.
- `alloc` — `odin3_util_malloc/calloc/realloc/free`, the only heap entry points, plus the hidden test hook `odin3_util_set_alloc_fail_after(n)`.
- `arena` — bump allocator: zeroed, aligned blocks that never move, all freed at once.
- `vec` — growable contiguous array of fixed-size elements (elements may move on growth).
- `pagevec` — paged array: elements never move, O(1) index lookup.
- `hash` — fixed-seed (`ODIN3_HASH_SEED`) byte and integer hashing, and the `odin3_bytes` view type.
- `u64map` — integer-keyed map (Robin Hood open addressing, backward-shift deletion).
- `idindex` — ID-only hash-cons index (Robin Hood open addressing); stores IDs, the caller owns the keys.
- `str` — string interning (`strtab`, ID 0 is the empty string) and a growable string builder (`strbuf`).
- `log` — process-global diagnostics with levels, per-level counts and a replaceable sink.

Benchmark: `tests/bench/bench_util.c` (target `bench_util`, built by default, not in CTest) times 2,000,000 interns, `u64map` puts and `pagevec` pushes; run it from the release build.

Adding a module: append its `.c` to `odin3_core` in `CMakeLists.txt`, and register its test with `odin3_add_unit_test(test_util_<name>)`.
