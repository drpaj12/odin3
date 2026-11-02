# Odin III

An open-source (MIT) HDL elaboration and front-end synthesis framework for FPGA CAD research, the
successor to Odin (FPL 2005) and Odin II (FCCM 2010). It reads SystemVerilog (via the vendored
slang adapter), Verilog-2005, plus BLIF, Yosys JSON, structural gate-level Verilog and VPR
architecture files, into one hierarchical, provenance-tracked IR. It does architecture-driven partial mapping, links ABC for soft logic, and
writes BLIF for VPR, LPM Verilog for an Altera-style flow, and dot and JSON for analysis. Every
object in the output can be traced to the HDL text it came from.

**Status: version 1.3.0 (tag on the merge commit; the latest tagged release is 1.2.0).** Version 1.2.0 added provenance at every granularity; 1.3.0 adds netlist editing, simulation toggle counting, graphlets and front-end fixes, C ABI still 7 (SystemVerilog via the vendored slang adapter, structural Verilog netlists, as in 1.1.x). User manual: <https://drpaj12.github.io/odin3/> (source `docs/user/`).
Release notes: [v1.3.0 changes](CHANGELOG.md), [v1.2.0](docs/V1.2.0.md) (provenance as a two-way granularity DAG, ABI 7), [v1.1.1](docs/V1.1.1.md) (critical-path attribution to HDL), [v1.1.0](docs/V1.1.0.md) (SystemVerilog, gate mapping, unmap, simulation API; rolls up v1.0.1 to v1.0.5) and
[v1.0](docs/RELEASE-NOTES-1.0.md). Changes: [`CHANGELOG.md`](CHANGELOG.md). Design:
[`docs/DESIGN.md`](docs/DESIGN.md). Paper: [`docs/paper/`](docs/paper/README.md). Token, model and
time cost of building it: [`docs/log_cost_of_tokens_and_time.md`](docs/log_cost_of_tokens_and_time.md).

Odin III is the trunk of **Yggdrasil III**, a suite of research tools that includes Wyrd III (<https://github.com/drpaj12/wyrd>, the canopy, with Bifrost III and Midgard III), Loki III (<https://github.com/drpaj12/loki>), The Norns III (<https://github.com/drpaj12/nornsIII>) and Verdandi III (<https://github.com/drpaj12/verdandiIII>) on side branches, and Mímir III (<https://github.com/drpaj12/mimirIII>, the fruit). Odin III is the trunk of the tree, and the other pieces use it as a package without changing its core. Version 1.3.0 (`pyproject.toml`). The hub is <https://github.com/drpaj12/YggdrasilIII> (documentation: <https://drpaj12.github.io/YggdrasilIII/>, versions of every piece: <https://drpaj12.github.io/YggdrasilIII/versions.html>) and its picture of the tree is <https://github.com/drpaj12/YggdrasilIII/blob/main/images/yggdrasil-tree.png>.

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
- [Verdandi III manual](https://drpaj12.github.io/verdandiIII/).
- [The Norns III](https://github.com/drpaj12/nornsIII) (repository; plan and paper under `docs/`).
- [Set up the whole suite](https://drpaj12.github.io/YggdrasilIII/getting-started.html) on a clean
  Linux or Windows machine, and [drive it with Claude Code](https://drpaj12.github.io/YggdrasilIII/ai-agent.html).

## Features

Version 1.3.0 on `main` (C ABI 7). Status: **works** = on `main` with tests; **limit** = works with a documented limit.
Each line points to the manual page; the pass table is [`docs/PASSES.md`](docs/PASSES.md) and the changes are in
[`CHANGELOG.md`](CHANGELOG.md).

| Feature | Status | Manual |
|---|---|---|
| Inputs: SystemVerilog (slang adapter), Verilog-2005, BLIF, Yosys JSON, structural gate-level Verilog, VPR architecture files, project files (`.o3proj`, `.qsf`, Odin II) | works | [inputs](docs/user/inputs.md) |
| Elaboration, `proc`, `opt`, `fsm`, `flatten`, `wreduce`, architecture-driven partial mapping (`hardmap`), `lower`, ABC for soft logic, LPM mapping, `dfflibmap`, `unmap` | works | [passes](docs/user/passes.md), [flow](docs/user/flow.md) |
| One-command flow `odin3-flow` to a routed VPR result or LPM Verilog | works | [odin3-flow](docs/user/odin3-flow.md) |
| Outputs: BLIF (plain and `--vtr`; `--reachable` writes the top and what it reaches; modules `flatten` left unused are not written and `=` in model and `.subckt` names is written `?`, so ABC can read hierarchical netlists), Yosys JSON, Verilog, dot, RTLIL | works | [outputs](docs/user/outputs.md) |
| Provenance at every granularity: each output object traces to its HDL text, as a two-way granularity chain with labelled joins, readable from C and Python, with `names` look-ups and the map format | works | [provenance](docs/user/provenance.md) |
| Hierarchical references: a dotted path into another module is an undriven net with a WARNING (as Yosys); generate-block and named-block references resolve | works (limits listed in CHANGELOG 1.3.0) | [inputs](docs/user/inputs.md), `docs/notes/2026-11-02-hierarchical-references.md` |
| `$tribuf` sole-driver rule in `lower`: when a `$tribuf` is the only driver of its output the enable is dropped with a WARNING; a bus or other cases stay a located error | works | [passes](docs/user/passes.md), `docs/specs/2026-10-14-3B-lower-design.md` |
| Flatten marking: modules `flatten` leaves unused are marked and not written by plain `write_blif` | works | [outputs](docs/user/outputs.md) |
| Processor netlists (Seeker-Proc): hierarchical specialised-module netlists readable by ABC, dead generate branches not elaborated | works | CHANGELOG 1.3.0, [MCU testbench note](docs/notes/2026-10-31-mcu-testbench.md), [DarkRISCV note](docs/notes/2026-11-01-darkriscv-testbench.md) |
| Graph features: graphlet and orbit counts (undirected 2 to 5 nodes, 73 orbits; directed 2 to 4 nodes, 129 orbits), for a module or a subgraph, with signature comparison; own implementation, ORCA only as a test oracle; the suite hub (<https://github.com/drpaj12/YggdrasilIII>) lists the tools that use it | works | [graphlets](docs/user/graphlets.md) |
| SCOAP testability and signal probability per net (`scoap`) | works | [scoap](docs/user/scoap.md) |
| Analysis: `write_dot` focus and colouring, `levelize`, `write_json --odin3` graph export, `prov-cover`, critical-path attribution to HDL | works | [analysis](docs/user/analysis.md), [timing-attribution](docs/user/timing-attribution.md) |
| Netlist editing API (add and remove nodes and nets, connect and disconnect pins, edit tags) and edit events (added, param, rewired, removed) exported with `write_json --odin3` and `write_blif --edit-tags` | works | [edit](docs/user/edit.md) |
| Cycle simulator `sim` (vectors, random inputs, VCD), toggle counting and `run_vectors` from Python, derived clocks (gated, muxed, divided) | works (2-state, cycle-based; derived-clock limits in the manual) | [edit](docs/user/edit.md), [python-api](docs/user/python-api.md), [testbench](docs/user/testbench.md), [MCU testbench note](docs/notes/2026-10-31-mcu-testbench.md), [DarkRISCV note](docs/notes/2026-11-01-darkriscv-testbench.md) |
| Testbenches: `sim_tb` interprets a Verilog testbench subset against RTL or netlist, `--plusarg` with `$test$plusargs` and `$value$plusargs`, `%t` in `$display`, up to 4096 named DUT port connections, `--vcd` waveform | works (2-state; subset limits in the manual) | [testbench](docs/user/testbench.md), [MCU testbench note](docs/notes/2026-10-31-mcu-testbench.md), [DarkRISCV note](docs/notes/2026-11-01-darkriscv-testbench.md) |
| `testbench_gen` writes a testbench for a design; `tools/tb-compare` runs one testbench on two designs; `run_testbenches` runs the testbenches declared in a project (`testbench` key, `on rtl\|netlist\|both`) | works | [testbench](docs/user/testbench.md) |
| Multi-clock testbenches (on main after 1.3.0, Unreleased): a `sim_tb` DUT instance may have up to 16 independent clock inputs (more than 16, a clock on a multi-bit port, or a cell with no clock pin in a DUT with two or more clocks is an error) | works | [testbench](docs/user/testbench.md) |
| Read-only hierarchical reads in testbenches (on main after 1.3.0, Unreleased): `dut.u_regs.r`, `dut.u.v[3]` with literal indices, after `flatten`; a write, `$readmemh` into a path, a variable index, a part-select or a memory word after `lower` is a located error | works | [testbench](docs/user/testbench.md) |
| `$tribuf` exclusions in `lower` tested (on main after 1.3.0, Unreleased): a constant 0, x or z enable (`$_CONSTZ_` included) and a `$tribuf` fed by a `$tribuf` (in any cell order) are refused | works | [passes](docs/user/passes.md) |
| Reverse-engineering passes `submod`, `cone_check`, `raise` and the RE loop | works | [re-workflow](docs/user/re-workflow.md) |
| Python binding (cffi) over the C ABI 7, plugin loading with `--plugin` and ABI check, `odin3_graph.py` reader of the JSON export | works | [python-api](docs/user/python-api.md) |

## Install

From source (Ubuntu 24.04, native or WSL2; CMake 3.28 or later for the default
`ODIN3_WITH_SLANG=ON` build, Ninja, gcc or clang, `bison`, `flex`, `libreadline-dev`):

```bash
git clone --recursive https://github.com/drpaj12/odin3.git && cd odin3
cmake --preset release && cmake --build --preset release
cmake --install build/release --prefix "$HOME/odin3-1.3.0"
```

Or from the release's source tarball, which carries the ABC and Unity submodules and needs no git:

```sh
tar xzf odin3-1.3.0.tar.gz && cd odin3-1.3.0
cmake --preset release && cmake --build --preset release
cmake --install build/release --prefix "$HOME/odin3-1.3.0"
```

Or the Python wheel from the GitHub release (bundles `libodin3`, ABC, the `odin3` CLI and
`odin3-flow`): `pip install odin3-1.3.0-py3-none-linux_x86_64.whl`.

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
[`docs/paper/joined.bib`](docs/paper/joined.bib).

## License

MIT, see [LICENSE](LICENSE). ABC and Unity keep their own (permissive) licenses.
