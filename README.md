# Odin III

An open-source (MIT) HDL elaboration and front-end synthesis framework for CAD research — the
successor to Odin (FPL 2005) and Odin II (FCCM 2010). It reads Verilog, SystemVerilog, VHDL and
structural netlists into one hierarchical, provenance-tracked IR, performs architecture-driven
partial mapping, links ABC for soft logic, and writes netlists for VTR, Intel/Altera FPGAs, and
visualisation. Design: [`docs/DESIGN.md`](docs/DESIGN.md).

**Status: Phase 0** — repository skeleton, oracles and tooling. No synthesis yet.

## Build

Ubuntu 24.04 (native or WSL2), CMake ≥ 3.25, Ninja, gcc or clang.

```bash
git clone --recursive https://github.com/drpaj12/odin3.git && cd odin3
cmake --preset debug && cmake --build --preset debug     # ASan + UBSan
ctest --preset debug
cmake --preset release && cmake --build --preset release
```

`-DODIN3_WITH_ABC=OFF` skips building the bundled ABC (used by `tools/equiv-check`).

## Lint gate

`tools/lint.sh` runs clang-format, clang-tidy, cppcheck, lizard, ruff and mypy exactly as CI does
(spec §15). Install the Python tools with `python3 -m venv .venv && .venv/bin/pip install -r
requirements-dev.txt`, then `pre-commit install` to run the gate on every commit.

## Layout

| Path | What |
|---|---|
| `include/odin3/odin3.h` | The public C ABI — the only header plugins and Python see |
| `src/` | C17 core: `util/ ir/ ast/ frontends/ passes/ backends/ sim/ cli/ api/` |
| `adapters/slang/` | The only C++: SystemVerilog via slang (Phase 5) |
| `plugins/` | Example `.so` plugin and the cffi Python binding |
| `tools/` | `netlist-compare`, `equiv-check`, `run-oracle.sh`, `lint.sh` |
| `tests/` | `unit/` (Unity), `golden/`, `micro/`, `tools/` |
| `third_party/` | `abc` (Berkeley ABC), `unity` (submodules) |
| `docs/` | Design spec, IR and pass docs, oracle versions, Phase 0 checklist (ADRs arrive in PR #2) |

## License

MIT — see [LICENSE](LICENSE). ABC and Unity keep their own (permissive) licenses.
