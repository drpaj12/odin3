# third_party/slang — vendoring note

This directory is the SystemVerilog front end's language library for Odin III v1.1.0
(`docs/DESIGN.md` §4.2, §12.1; ADR D12). It is **source-vendored**: the files are committed here,
not a git submodule, so a plain checkout builds with no network and no sibling repository.

## Upstream

| Tree | Upstream | Version | Commit |
|---|---|---|---|
| `third_party/slang/` (this directory's source) | https://github.com/MikePopoloski/slang | v12.0 | `545f88b2ed3a2d20325fbd17413af177a6bca72f` |
| `third_party/slang/third_party/fmt/` | https://github.com/fmtlib/fmt | 12.2.0 | `1be298e1bd68957e4cd352e1f676f00e07dcfb57` |
| `third_party/slang/third_party/boost_regex/` | https://github.com/MikePopoloski/regex (boost.regex, standalone) | boost-1.91.0 | `2b3ac0834f31086c6e3c0e0ceb8516e427d5c39d` |
| `third_party/slang/third_party/tomlplusplus/` | https://github.com/marzer/tomlplusplus | v3.4.0 | `30172438cee64926dc41fdd9c11fb3ba5b2ba9de` |

The source was copied from the populated Yosys submodule at
`external/yosys/libs/slang` (slang v12.0, commit `545f88b2`). The brief named sv-elab commit
`8911a6ca` as the pin: that is the sv-elab commit "slang: fix commit id to actual 12.0 tag",
whose `third_party/slang` submodule is exactly `545f88b2` (the sv-elab checkout's submodule was
not populated on disk, so the vendored bytes are slang's own v12.0 tree from the Yosys
submodule, the same commit). Only `.git`, the build output and editor junk were left out.

`fmt`, `boost.regex` and `tomlplusplus` are vendored because upstream slang's CMake fetches them
with FetchContent from GitHub; see `BUILDING.md`. slang's own `external/` headers
(`boost_unordered.hpp`, `expected.hpp`, `BS_thread_pool.hpp`, the IEEE 1800 headers) ship with
slang itself and are part of the copy.

## Editing

Treat every file here as third-party: no Odin III style changes. A pin bump replaces this tree
and updates this note and `BUILDING.md`. The Odin III adapter that uses the library lives in
`adapters/slang/`; `tools/lint.sh` excludes all of `third_party/`.
