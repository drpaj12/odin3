# Odin III

An open-source (MIT) HDL elaboration and front-end synthesis framework for FPGA CAD research, the
successor to Odin (FPL 2005) and Odin II (FCCM 2010). It reads Verilog-2005, plus BLIF, Yosys
JSON, structural gate-level Verilog and VPR architecture files, into one hierarchical,
provenance-tracked IR. It does architecture-driven partial mapping, links ABC for soft logic, and
writes BLIF for VPR, LPM Verilog for an Altera-style flow, and dot and JSON for analysis. Every
object in the output can be traced to the HDL text it came from.

**Status: version 1.0.0.** User manual: <https://drpaj12.github.io/odin3/> (source `docs/user/`).
Release notes: [`docs/RELEASE-NOTES-1.0.md`](docs/RELEASE-NOTES-1.0.md). Changes:
[`CHANGELOG.md`](CHANGELOG.md). Design: [`docs/DESIGN.md`](docs/DESIGN.md).

Odin III is the trunk of **Yggdrasil III**, a suite of research tools that use it as a package
without changing its core: Wyrd III (experiment runner and GUI, <https://github.com/drpaj12/wyrd>),
Loki III (reverse engineering of structural netlists, <https://github.com/drpaj12/loki>), Bifrost III
(the simulation bridge) and Midgard III (the Verilog Town game).

## Documentation

Start at the Yggdrasil III hub; every manual links to the others.

- [Yggdrasil III hub](https://drpaj12.github.io/YggdrasilIII/): the suite, how the pieces connect, getting started.
- [Odin III manual](https://drpaj12.github.io/odin3/).
- [Wyrd III manual](https://drpaj12.github.io/wyrd/), with
  [simulation (Bifrost III)](https://drpaj12.github.io/wyrd/simulation.html) and
  [Midgard III (Verilog Town)](https://drpaj12.github.io/wyrd/midgard.html).
- Tutorials (PDF): [command line](https://drpaj12.github.io/wyrd/wyrd-tutorial.pdf) and
  [GUI](https://drpaj12.github.io/wyrd/wyrd-gui-tutorial.pdf).
- [Loki III manual](https://drpaj12.github.io/loki/).

## Install

From source (Ubuntu 24.04, native or WSL2; CMake 3.25 or later, Ninja, gcc or clang, `bison`,
`flex`, `libreadline-dev`):

```bash
git clone --recursive https://github.com/drpaj12/odin3.git && cd odin3
cmake --preset release && cmake --build --preset release
cmake --install build/release --prefix "$HOME/odin3-1.0.0"
```

Or the Python wheel from the GitHub release (bundles `libodin3`, ABC, the `odin3` CLI and
`odin3-flow`): `pip install odin3-1.0.0-py3-none-linux_x86_64.whl`.

Then follow the manual's getting-started page: one command, `odin3-flow`, takes a Verilog design
to a routed VPR result (with `VTR_ROOT` set) or to LPM Verilog.

## Develop

```bash
cmake --preset debug && cmake --build --preset debug     # C17, ASan + UBSan
ctest --preset debug
tools/lint.sh                                            # the CI gate
```

`tools/lint.sh` runs clang-format, clang-tidy, cppcheck, lizard, ruff and mypy exactly as CI does
(spec §15). Install the Python tools with `python3 -m venv .venv && .venv/bin/pip install -r
requirements-dev.txt`, then `pre-commit install` to run the gate on every commit. Oracle versions
and how goldens are made: [`docs/ORACLES.md`](docs/ORACLES.md). Every pass:
[`docs/PASSES.md`](docs/PASSES.md).

## Layout

| Path | What |
|---|---|
| `include/odin3/odin3.h` | The public C ABI, the only header plugins and Python see |
| `src/` | C17 core: `util/ ir/ ast/ frontends/ passes/ backends/ sim/ cli/ api/` |
| `adapters/abc/` | The linked-ABC adapter (C17) |
| `adapters/slang/` | SystemVerilog shim via slang (C++; front end after v1.0, ADR D11) |
| `plugins/` | Example `.so` plugin, the cffi Python binding, `re_round.py` |
| `tools/` | `flow` (`odin3-flow`), `vtr-qor`, `equiv-check`, `netlist-compare`, `release`, checkers |
| `tests/` | `unit/` (Unity), `golden/`, `micro/`, `tools/` |
| `third_party/` | `abc` (Berkeley ABC), `unity` (submodules) |
| `docs/` | Spec, IR, passes, oracles, ADRs, phase logs, user manual (`user/`), paper (`paper/`) |

## Resources and acknowledgements

Odin III builds on, links, or is measured against these projects:

| Project | Role in Odin III |
|---|---|
| [VTR (Verilog to Routing)](https://github.com/verilog-to-routing/vtr-verilog-to-routing), [docs](https://docs.verilogtorouting.org/) | Target flow (VPR pack, place, route), architectures, benchmarks and oracle; J. Rose et al., FPGA 2012; K. E. Murray et al., "VTR 8", ACM TRETS 2020 |
| Odin II ([in the VTR repository](https://github.com/verilog-to-routing/vtr-verilog-to-routing/tree/master/odin_ii)) | Predecessor and oracle; P. Jamieson, K. B. Kent, F. Gharibian, L. Shannon, FCCM 2010 |
| Odin | The first version; P. Jamieson and J. Rose, "A Verilog RTL Synthesis Tool for Heterogeneous FPGAs", FPL 2005 |
| [ABC](https://github.com/berkeley-abc/abc) | Linked for soft-logic optimisation, LUT mapping and equivalence checks (submodule); R. Brayton and A. Mishchenko, CAV 2010 |
| [Yosys](https://github.com/YosysHQ/yosys) and Parmys ([in VTR](https://github.com/verilog-to-routing/vtr-verilog-to-routing/tree/master/parmys)) | Oracles: elaboration, RTLIL and JSON formats, and VTR's default front end; C. Wolf et al., Austrochip 2013 |
| [slang](https://github.com/MikePopoloski/slang) and [yosys-slang](https://github.com/povik/yosys-slang) | SystemVerilog oracle and the planned front end |
| [Unity](https://github.com/ThrowTheSwitch/Unity) | C unit-test framework (submodule) |
| [Sphinx](https://www.sphinx-doc.org/) and [MyST](https://myst-parser.readthedocs.io/) | The user manual |
| [scikit-build-core](https://github.com/scikit-build/scikit-build-core) and [cffi](https://github.com/python-cffi/cffi) | The Python wheel and binding |
| [Claude Code](https://code.claude.com/docs) | Odin III was built by AI coding agents under the architect's specification and review; process, time and token cost are in [`docs/paper/`](docs/paper/) and [`docs/log_cost_of_tokens_and_time.md`](docs/log_cost_of_tokens_and_time.md) |

Exact oracle versions and commits are in [`docs/ORACLES.md`](docs/ORACLES.md); full references in
[`docs/paper/odin3.bib`](docs/paper/odin3.bib).

## License

MIT, see [LICENSE](LICENSE). ABC and Unity keep their own (permissive) licenses.
