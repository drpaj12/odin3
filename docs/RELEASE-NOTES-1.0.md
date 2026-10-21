# Odin III 1.0.0 release notes

## What it is

Odin III is an MIT-licensed HDL elaboration and front-end synthesis framework for FPGA CAD
research, the successor to Odin (FPL 2005) and Odin II (FCCM 2010). It reads Verilog-2005 (plus
BLIF, Yosys JSON, structural gate-level Verilog and VPR architecture files) into one hierarchical,
provenance-tracked IR, does architecture-driven partial mapping, links ABC for soft logic, and
writes BLIF for VPR, LPM Verilog for an Altera-style flow, and dot and JSON for analysis. Every
object in the output can be traced to the HDL text it came from.

What v1.0 does: a basic design goes from Verilog to a routed VPR result, or to LPM Verilog, with
one command (`odin3-flow`). Three goals, in order of care: elaboration correctness, a graph and
reverse-engineering research platform (`submod`, `cone_check`, `raise`, `Design.copy()`, the Python
binding), and a flow that is comparable to VTR's own (not QoR parity). The manual is
`docs/user/` (Sphinx and MyST); the design is `docs/DESIGN.md`.

## Install

From source (Ubuntu 24.04; CMake 3.25 or later, Ninja, a C compiler, `bison`, `flex`,
`libreadline-dev`):

```bash
git clone --recursive https://github.com/drpaj12/odin3.git && cd odin3
cmake --preset release && cmake --build --preset release
cmake --install build/release --prefix "$HOME/odin3-1.0.0"
```

The install holds `bin/odin3`, `lib/libodin3.so`, `include/odin3/odin3.h` and `share/odin3/`
(technology libraries, the flow script and the QoR tools). `odin3-flow` finds `share/odin3` from
its own location, or from `$ODIN3_SHARE`.

Source tarball: `odin3-1.0.0.tar.gz` with a `SHA256SUMS` file. It carries the ABC and Unity
submodules, which GitHub's own archives leave out (`tools/release/make-tarball`).

Python wheel: `odin3-1.0.0-py3-none-linux_x86_64.whl`, built with scikit-build-core, bundling
`libodin3`, the in-process ABC, the `odin3` CLI and the `odin3-flow` command. The tag is
`linux_x86_64`, not `manylinux`: `auditwheel repair` from the build host (glibc 2.39) reaches only
`manylinux_2_39_x86_64`, too new for most users and for PyPI. A manylinux wheel needs a build in an
older-glibc container (later work; `docs/PHASE7.md` decision 8). Install it with
`pip install odin3-1.0.0-py3-none-linux_x86_64.whl`.

`tools/release/clean-check` (CTest `release_clean`, local only) checks all of this on a fresh clone.

## Stability

There is no API or ABI stability promise for v1.0 (`docs/PHASE7.md` decision 4). The C ABI
(`include/odin3/odin3.h`, ABI version 4), the Python binding, the `.o3lib` format and the JSON
keys may change in later releases.

## Not in v1.0

- CIRCT writer: dropped (ADR D12).
- EDIF and VQM writers, device atoms: dropped (ADR D10).
- FF feature matching (PHASE4 4.6): later plugin or v1.1.
- SystemVerilog (slang) and VHDL (GHDL) front ends: after v1.0 (ADR D11). Parity with VTR's QoR is
  not a goal (ADR D10).
- Benchmark runner, Python GUI, RTL viewer: after v1.0, each with its own spec
  (`docs/PHASE7.md` decision 7).

## Known gaps

- Plain-LUT architecture `k4_N4_90nm` (4.13 arch sweep): 14 of 15 designs route; `mkDelayWorker32B`
  hits the 3600 s ABC limit with all adders soft (`docs/PHASE4.md` decision 18).
- Dead-adder warning: on some EArch sweep designs the QoR harness prints the
  `hardblocks_lost` WARNING (adders unused after VTR's ABC; `tools/vtr-qor/qor_table.py`). Not
  investigated.
- Functional check (`fcheck`): designs with latches or the `--nolatches` precheck are classified
  untested, not simulated (`docs/PHASE3.md` decisions 50 and 57); `diffeq1` is not equivalence-tested
  (`equiv-check` cannot name 3 of its 5 multiply black boxes; PHASE3, Phase 3 exit notes).
  Big VTR designs are checked to get through the VTR flow with similar QoR, not for equivalence.
- Cell types `$_DFFSR_*`, `$_ALDFF_*` and `$_SR_*` (and latches with reset) are not registered; a
  netlist using them reads them as implicit black boxes (`docs/PHASE5.md`, 5.3 known gaps).
- ABI gaps for tools built on the binding: attributes are string-only with no remove, `cone_check`
  is combinational and keeps one result per module, node IDs are module-local, no bulk readers.
  The full list with suggested fixes is `docs/RE-ARCHITECTURE.md` section 5.
