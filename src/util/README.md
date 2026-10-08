# src/util — Phase 1
The only generic containers in the tree (spec §15.1): `arena`, `vec`, `hashmap`, `str`, `log`. Nothing else may define a container.

## Modules so far
- `attr.h` — `ODIN3_EXPORT` (marks the four ABI definitions; everything else is hidden) and `ODIN3_PRINTF(f, a)`.
- `alloc.h` — `odin3_util_malloc/calloc/realloc/free` plus the hidden test hook `odin3_util_set_alloc_fail_after(n)`.

Adding a module: append its `.c` to `odin3_core` in `CMakeLists.txt`, and register its test with `odin3_add_unit_test(test_util_<name>)`.
