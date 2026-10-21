# Phase 7: release (v1.0): flow scripts, user docs, reverse-engineering core support, packaging

Status: v1 (2026-10-21). Design approved by Peter section by section in the brainstorm of
2026-10-21 ("Yes" to sections 1 to 3, then "I'm just going to say yes from now on").
ADR: D12. Checklist: `docs/PHASE7.md`. Plan: `docs/plans/2026-10-21-7-release-plan.md`.
Background for the next tool: `docs/RE-ARCHITECTURE.md`.

## 0. Goal and decisions

Goal (Peter, 2026-10-21): "polish, write up, and completion of the tool such that a basic design
can go through", with user documentation like the VTR documentation and simple Python scripts
that take a basic design through the entire flow; and the core support the reverse-engineering
tool needs to build hierarchy from guesses on an anonymous BLIF.

Decisions (Peter, 2026-10-21):

| # | Decision |
|---|---|
| 1 | 7.1 CIRCT writer dropped ("We have outputs, so probably this is nothing"). D12. |
| 2 | Phase 7 = 7.3 user docs + flow script, 7.4 reverse-engineering core support and ABI review, 7.6 release. |
| 3 | The benchmark runner, a Python GUI, an RTL viewer and a Verilog Town rebuild (Pygame CE, linked to the Odin III simulator) come after v1.0, each with its own spec once Peter provides the material. |
| 4 | One flow script, `odin3_flow.py`, targets `vtr` (default) and `lpm`, option `--analysis` (brainstorm option 1). |
| 5 | User docs: Sphinx with MyST Markdown under `docs/user/` (option 1). |
| 6 | No API stability promise for v1.0 (option 3). 7.4 checks the ABI is fit for the reverse-engineering use instead. |
| 7 | 7.4 adds `submod`, design copy, `cone_check` and `raise` (A to D); the pyramid/overlay/e-graph model is written down for the next tool. |
| 8 | Release: a source release plus a pip-installable Python package bundling `libodin3` (option 3). |

## 1. Items

| Item | What | Done when |
|---|---|---|
| 7.1 | CIRCT writer: dropped (D12) | n/a |
| 7.2 | JSON reader: moved to 5.3, done | n/a |
| 7.3 | User docs (`docs/user/`) and `tools/flow/odin3_flow.py` | a basic design goes from Verilog to a routed VPR result and to LPM Verilog with one command; the docs build in CI with `-W` |
| 7.4a | `submod` pass | equivalent after `flatten` (`equiv-check`), `check` clean |
| 7.4b | `odin3_design_copy` (ABI) and `Design.copy()` (Python) | the copy is independent; `write_json` of both identical right after the copy |
| 7.4c | `cone_check` pass | equivalent / differs / unproven on micros, as expected |
| 7.4d | `raise` pass (Peter reviews the PR: mapping-style algorithm) | equivalent; provenance of the new cell reaches the fine nodes' sources |
| 7.4e | ABI fitness review, `docs/RE-ARCHITECTURE.md`, `plugins/python/re_round.py` | the example runs in CTest and finds `$add` |
| 7.5 | EDIF writer: dropped (D10) | n/a |
| 7.6 | Release: source tarball with submodules, `cmake --install`, pip package, CHANGELOG, built docs, clean-machine test, tag | `pip install` of the wheel and the getting-started page work on a clean machine |

Order: 7.3 and 7.4a/7.4b in parallel lanes (disjoint files); then 7.4c, 7.4d, 7.4e; 7.6 last.
**Exit test** (DESIGN section 12 row 7): every "done when" above, and the 7.6 clean-machine test.

## 2. 7.3: `odin3_flow.py` and user docs

### 2.1 Script

`tools/flow/odin3_flow.py` (stdlib only; command line and importable):

```
odin3_flow.py design.v [more.v ...] --top TOP
    --target vtr|lpm        default vtr
    --arch ARCH.xml         vtr; default $VTR_ROOT/vtr_flow/arch/timing/k6_frac_N10_frac_chain_mem32K_40nm.xml
    --family cyclonev       lpm; as the 5.1 LPM flow
    --vpr / --no-vpr        vtr; default on when $VTR_ROOT is set
    --analysis              also write_dot, write_json --odin3, levelize, dump_locs --types
    --odin3 PATH            default: odin3 on PATH, else build/release/odin3
    --out DIR               default ./odin3_out/<top>
```

- F-1 (`vtr`): the Odin III script is the vtr-qor O-leg script, imported from `tools/vtr-qor`
  (one definition of the flow; no copy): `read_verilog`, `proc; fsm; opt; flatten; opt -full;
  wreduce; hardmap ...`, `abc`, `write_blif --vtr`. Exact commands and options are taken from
  `tools/vtr-qor/qor_legs.py` and `docs/PASSES.md` at implementation time, never guessed.
- F-2 (VPR): when on, VPR packs, places and routes at the minimum channel width, as vtr-qor runs
  it (same options); `summary.json` and a printed table: LUTs, CLBs, hard blocks by type,
  minimum channel width, critical-path delay, wirelength, runtime per step.
- F-3 (`lpm`): the 5.1 LPM flow (`write_verilog` LPM flavour) and a read-back lint
  (`read_verilog -icells`), as the 5.1/5.2 tests run it.
- F-4 (`--analysis`): `write_dot --color-by` and `--locs`, `write_json --odin3`, `levelize`,
  `dump_locs --types`, into `analysis/`.
- F-5: one subdirectory per step with its log; a failing step stops with a message naming the
  step and its log; exit 0 ok, 1 a step failed, 2 usage error.
- F-6 tests: CTest runs `vtr --no-vpr` and `lpm` on two micros; a `sweep` CTest runs `vtr` with
  VPR on `single_ff` and `ch_intrinsics` (needs `ODIN3_HEAVY_OK`).

### 2.2 User docs

`docs/user/`, Sphinx with MyST Markdown (sources read on GitHub too); `docs/user/conf.py`;
`requirements-docs.txt` (sphinx, myst-parser; dev only, not a tool dependency). CI job `docs`
builds with `-W` (warnings are errors). Publishing (Read the Docs or GitHub Pages) is Peter's
decision at the tag.

Pages: getting started (build, install, one design); the flow (each stage with a diagram, in
the manner of the VTR documentation); `odin3_flow.py` reference; command and pass reference
generated from `docs/PASSES.md` and `odin3 help` at build time (a small Sphinx extension or a
pre-build script; no hand copy); inputs (Verilog, BLIF, JSON, `.o3lib`, VPR arch import);
outputs and read-back (BLIF, Verilog, JSON, LPM; HDL locations); analysis (`write_dot`, graph
export, `levelize`, `prov-cover`); Python API (bindings, plugin example); the
reverse-engineering workflow (written after 7.4).

## 3. 7.4: reverse-engineering core support

Every mutation is a pass, so only the core edits the IR and `check` runs around it; the ABI
gains one function (design copy). Plugins drive the rest with `odin3_design_run_script`,
attributes and the read ABI.

### 3.1 `submod [-attr KEY] [-name N]` (7.4a)

A port of the Yosys pass of the same name.
- S-1: live nodes whose attribute KEY (default `submod`) has the same value V form one new
  module named V (or N with `-name` for a single group).
- S-2: ports are the boundary nets: input when driven outside the group (or by a module input);
  output when read outside the group or by a module output; a net both driven and read inside
  and outside is an output (driver inside) or input (driver outside). Port names are the net
  names where they are real names, else `p<k>`; bus grouping only where a wire's bits are all on
  the boundary in order.
- S-3: one instance replaces the moved nodes; its connections are the boundary nets.
- S-4: provenance and attributes move with the nodes; the new module and instance get a
  `DERIVED` provenance record naming the pass.
- S-5 (located errors): a module name clash; a group holding a module port node; an empty group.
- S-6 tests: unit tests on small IR; `flatten` after `submod` is equivalent to the input
  (`equiv-check`) on the micros with a deterministic grouping (every second node by ID);
  `check` clean.

### 3.2 `odin3_design_copy` (7.4b)

`odin3_status odin3_design_copy(const odin3_design *src, odin3_design **out)`: deep copy of
modules, nodes, pins, nets, wires, attributes, provenance records and the source manager; IDs
are the same in the copy. Ownership: the caller destroys `*out` with `odin3_design_destroy`.
Failure: out of memory (nothing allocated is left behind). Python `Design.copy()`.
Tests: mutate the copy (a pass), the original's `write_json` is unchanged; `write_json` of both
identical right after the copy; ASan clean.

### 3.3 `cone_check -attr KEY=V -cell TYPE [-param P=V ...] [-timeout S]` (7.4c)

- C-1: proves that the nodes marked KEY=V compute one cell of TYPE with the given parameters.
- C-2 binding: each boundary net of the group carries `odin3:bind=<port>[<bit>]` naming the
  template port bit it stands for; every template port bit must be bound exactly once, else a
  located error. The binding search stays in the plugin.
- C-3: the group is cut into a temporary module (as `submod`), the template cell is built in
  another, both are lowered to gates and compared with the linked ABC `cec` under the time limit
  (default 60 s). Registers on the boundary are ports: v1.0 proves combinational cones only; a
  group containing a register is a located error.
- C-4 result: module attribute `odin3:cone_check=equivalent|differs|unproven`, the group key,
  and a hash of the group's node IDs and binding (`odin3:cone_hash`); on `differs`, a
  counterexample as `odin3:cone_cex` (input bit values). The temporary modules are removed.
- C-5 tests: on micros, a ripple adder cone against `$add` is equivalent; a swapped binding
  differs with a counterexample; a forced 0 s timeout is unproven; a group with a register is
  refused.

### 3.4 `raise -attr KEY=V -cell TYPE [-param P=V ...]` (7.4d; Peter reviews)

- R-1: replaces a group proven by `cone_check` with one cell of TYPE, connected through the
  binding.
- R-2: refuses unless the module holds `odin3:cone_check=equivalent` for that key and the hash of
  the current group and binding equals `odin3:cone_hash` (nothing changed since the proof).
- R-3 provenance: the new cell's provenance lists every source of the replaced nodes (a
  `DERIVED` record with the pass and the union of the sources), so tracing reaches the gates.
- R-4 tests: equivalent to the input; the new cell's provenance sources equal the union of the
  replaced nodes' sources; refusal without proof and after a change.

### 3.5 Fitness review and example (7.4e)

- A read-only review agent walks the loop of `docs/RE-ARCHITECTURE.md` section 2 against the ABI
  and the Python bindings and lists gaps; small gaps are fixed in 7.4e, large ones go to
  `docs/RE-ARCHITECTURE.md` section 5.
- `plugins/python/re_round.py`: reads an anonymous BLIF (a micro adder design through `abc`
  with generated names), guesses the adder cone with a simple structural rule, binds its ports,
  runs `cone_check` and `raise`, writes Verilog. CTest: `$add` in the output, equivalent to the
  input.

## 4. 7.6: release

- P-1 version: `1.0.0` in CMake, `odin3 --version`, `odin3_version_string`, the Python package.
- P-2 source tarball with the submodules (ABC, Unity), made by a script (`tools/release/
  make-tarball`), since GitHub archives leave submodules out; checksums.
- P-3 `cmake --install`: `odin3`, `libodin3`, the public header, `lib/*.o3lib`, the Python tools,
  `odin3_flow.py`, the docs.
- P-4 pip package `odin3`: the cffi binding (`plugins/python/odin3.py` becomes the package
  module) with `libodin3.so` inside; built by scikit-build-core through CMake (the library and
  the CLI); the binding finds the bundled library first, then `$ODIN3_LIB`, then the build tree.
  Linux x86-64 wheel; manylinux tagging with `auditwheel` when available, else a plain
  `linux_x86_64` wheel and the sdist (state which in the release notes). PyPI upload is
  Peter's action.
- P-5 CHANGELOG (from the phase checklists and PR titles), release notes, built docs attached.
- P-6 clean-machine test: a fresh clone in a container or an empty temporary directory, without
  the workspace's environment: build, install, `pip install` the wheel, run the getting-started
  page commands; the CTest `release_clean` drives it (sweep label).
- P-7 the tag `v1.0.0` and the GitHub release are pushed by Peter or by the controller after his
  explicit go.

## 5. Out of scope (after v1.0)

CIRCT writer (D12); EDIF (D10); benchmark runner, Python GUI, RTL viewer, Verilog Town (each its
own spec on Peter's material); the hypothesis overlay, Bayesian engine and e-graph raising
(`docs/RE-ARCHITECTURE.md`); sequential `cone_check`; API stability promise.

## 6. Risks

1. `submod` port rules on nets with several drivers or inout bits: located error first, support
   later.
2. `cone_check` binding errors look like real differences: the counterexample names the bits.
3. The wheel bundles ABC: build time and size; manylinux may not be reachable from WSL without a
   container (fallback stated in P-4).
4. Sphinx adds a CI job and dev dependency; the tool stays dependency-free.
5. `raise` depends on the proof hash being exact: any change to nodes or binding after the
   proof must refuse (tests R-4).
