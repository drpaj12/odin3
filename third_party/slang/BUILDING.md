# Building third_party/slang (Odin III v1.1.0)

slang is C++20. Odin III's build (`CMakeLists.txt`, `ODIN3_WITH_SLANG`) adds this directory with
`add_subdirectory`, builds the static `slang_slang` library (output `libsvlang.a`) and links it
into `libodin3_slang.so` (the adapter). Nothing here is installed and none of the third-party
style or warning settings reach Odin III's targets.

## Dependencies: none fetched at build time

Upstream slang's CMake fetches three dependencies with FetchContent: **fmt 12.2.0**,
**boost.regex boost-1.91.0** and **tomlplusplus v3.4.0** (and mimalloc, Catch2 when enabled).
This tree vendors exact copies under `third_party/` (see `VENDORING.md`), and the build replaces
each fetch by a local source directory:

```
-DFETCHCONTENT_SOURCE_DIR_FMT=<this dir>/third_party/fmt
-DFETCHCONTENT_SOURCE_DIR_BOOST_REGEX=<this dir>/third_party/boost_regex
-DFETCHCONTENT_SOURCE_DIR_TOMLPLUSPLUS=<this dir>/third_party/tomlplusplus
```

With those three variables, configure and build make **no network access**. Without them,
configure clones the three repositories from GitHub (fmt and boost.regex from the upstream URLs,
tomlplusplus from GitHub) — that is the one network dependency of an unpinned build, and the
reason for vendoring. mimalloc is switched off (`SLANG_USE_MIMALLOC=OFF`); its allocator is an
optional performance feature, not part of the language semantics. Catch2 would only be fetched
with `SLANG_INCLUDE_TESTS=ON` (off).

## Standalone build (what was run for v1.1.0)

```sh
S=$PWD/third_party/slang
cmake -S $S -B /tmp/slang-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DSLANG_INCLUDE_TOOLS=OFF -DSLANG_INCLUDE_TESTS=OFF -DSLANG_INCLUDE_INSTALL=OFF \
  -DSLANG_USE_MIMALLOC=OFF \
  -DFETCHCONTENT_SOURCE_DIR_FMT=$S/third_party/fmt \
  -DFETCHCONTENT_SOURCE_DIR_BOOST_REGEX=$S/third_party/boost_regex \
  -DFETCHCONTENT_SOURCE_DIR_TOMLPLUSPLUS=$S/third_party/tomlplusplus
cmake --build /tmp/slang-build --parallel 8
# -> /tmp/slang-build/lib/libsvlang.a (about 30 MB), 140 compilation units
```

Measured on the v1.1.0 lane (WSL2, gcc 13.3, 8 jobs): configure about 1 s, build about 3 min
release, about 5 min Debug with assertions. `cmake --preset debug` / `--preset release` of Odin
III builds the same library into the Odin build tree via `add_subdirectory`.

Requirements: CMake >= 3.28 (the project minimum 3.25 is raised by slang itself), a C++20
compiler, Ninja (any generator works), and a Python 3 interpreter (slang runs its syntax and
diagnostic generators at build time; the generated sources go into the build tree, not here).
