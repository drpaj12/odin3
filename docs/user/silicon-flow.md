# Silicon flow (SKY130)

Version 1.4. The silicon flow takes a design from Verilog to a routed SKY130 layout (GDS) and back to
recovered cells, and writes a provenance file at every step. It has no Yosys in the path: Odin III maps
the logic onto `sky130_fd_sc_hd` cells with `abc --gates` and `dfflibmap`; OpenROAD, driven by Odin III's
own Tcl, places and routes; KLayout writes the GDS; Magic extracts a transistor netlist; Netgen checks
LVS; and a name-free join takes the extracted cells back to the placed cells and so to the HDL.

Purpose: Leman Russ III checks a fabricated layout against a golden that carries provenance. Research
question: does an Odin III to OpenROAD to GDS flow keep provenance to every placed cell, and what does
physical design add or change? The provenance files are described in {doc}`provenance-files`; the project
file keys that describe a flow are in {doc}`project-flows`.

## Install (user-space, no sudo, no Docker)

Odin III builds and its tests pass without any of this. The silicon steps `place` to `recover` need the
tools below; the steps `read`, `elab`, `map`, `equiv` and `netlist` need only Odin III. These are the
commands and versions that were run and checked.

```text
# micromamba (static binary)
curl -L https://micro.mamba.pm/api/micromamba/linux-64/latest | tar -xj bin/micromamba
export MAMBA_ROOT_PREFIX=$TOOLS/mamba
micromamba create -n orcond -c litex-hub -c conda-forge --override-channels openroad -y
micromamba create -n eda    -c litex-hub -c conda-forge --override-channels magic netgen -y
micromamba clean -a -y
# KLayout python module and the PDK manager
python3 -m venv $TOOLS/orvenv && $TOOLS/orvenv/bin/pip install klayout ciel
$TOOLS/orvenv/bin/ciel enable --pdk-root $TOOLS/pdk --pdk sky130 -l sky130_fd_sc_hd 792ef67037d74c8c51811eab3ce23cf793491296
```

| Tool | Version run | Source |
|---|---|---|
| OpenROAD | `2.0_3175_gf12e2f474` (git f12e2f474, March 2022) | conda `litex-hub` |
| Magic | 8.3.465 | conda `litex-hub` |
| Netgen | 1.5.272 | conda `litex-hub` |
| KLayout | python module 0.30.12 | `pip install klayout` |
| PDK | SKY130A, library `sky130_fd_sc_hd` only, ciel 3.2.0 | `ciel enable` |

`$TOOLS` is any directory you own; about 2.4 GB for tools and the PDK, a few MB per small design. The
flow finds the tools in `--tools $TOOLS` (layout `mamba/envs/orcond/bin`, `mamba/envs/eda/bin`,
`orvenv/bin`, `pdk/sky130A`), in `ODIN3_SILICON_TOOLS`, or on `PATH`.

**OpenROAD pin.** v1.4 is pinned to the March 2022 build above, because it is the one that installs
without sudo and runs the Tcl flow. The PyPI `openroad` wheel (a Python module) was tried and crashed in
`initialize_floorplan`; newer conda builds need a `libboost` that is not in the channels. The Tcl driver
is written to the 2022 command set and the run probes the version. Upgrading OpenROAD is a later change.

## Run

```text
odin3-flow design.v --target silicon --top TOP --clock CLOCK_PORT --tools $TOOLS --out OUT
python3 tools/silicon/silicon_flow.py design.v --top TOP --out OUT --tools $TOOLS   # the same, directly
```

Options: `--steps a,b` runs a subset (`read,elab,map,equiv,netlist` needs no external tool), `--clock` names
the clock port (it switches clock-tree synthesis on), `--pdk` names the `sky130A` directory. Missing
tools stop the run with exit 2 and a list of what to install. `--param NAME=VALUE` (repeatable) sets the default of a
parameter of the top module in a copy of the design (`OUT/param_src/`, same file name, the other files of the directory
linked beside it): the netlist has no parameters, so a testbench that instantiates `top #(.NAME(v))` needs the netlist
built with `v` (the i2c ladder design: `--param ARST_LVL=1'b0`), and `gate_sim.py --param NAME=VALUE` drops that override
from the testbench copy. The sha256 of the copy, not of the original, is recorded as the design input. A worked example is
`tests/silicon/comparator/` (the design, the stored provenance directory, the census and the join).

| Step | Stage | Tool | What it does |
|---|---|---|---|
| `read` | `hdl` | odin3 | the HDL spans the design is made of |
| `elab` | `ir` | odin3 | `proc; flatten; opt; lower`: word and gate cells with their spans |
| `map` | `gates` | odin3 | `abc --gates sky130hd; dfflibmap sky130hd`; `prov.csv` side table |
| `equiv` | `gates` | `tools/equiv-check` | RTL BLIF against the gate BLIF |
| `netlist` | `gates` | `silicon_flow.py` | strip attributes, legalise names, give every constant a `conb_1` tie cell |
| `place` | `placed` | OpenROAD | floorplan, tap cells, power grid, global and detailed placement, repair, clock tree, hold repair |
| `route` | `routed` | OpenROAD | global and detailed routing, filler cells, `route.def`, `final.v` |
| `gds` | `gds` | KLayout | DEF to GDS, standard cells merged from the PDK GDS |
| `extract` | `extracted` | Magic | hierarchical and flat transistor SPICE |
| `lvs` | `extracted` | Netgen | extracted SPICE against the OpenROAD netlist |
| `recover` | `recovered` | `silicon_flow.py` | join extracted cells to DEF placements by cell type and location |

The library `lib/sky130hd.o3lib` (84 cells: 83 single-output gates of at most 6 inputs at drive strength 1,
plus the D flip-flop `dfxtp_1`) is data generated by `tools/silicon/liberty_to_o3lib.py` from the Liberty
file; the file records the Liberty sha256. There is no Liberty importer in the core (gatemap decision
G-1). Constants use `conb_1` tie cells, added by the `netlist` step: a tie cell for an HDL constant is an
`hdl` object with a link to the constant when Odin III's dump has one with a location, and `physical:tie`
otherwise.

## Outputs

In `OUT`: `prov/` (the provenance files), `g.v` (Odin III netlist), `clean.v`, `prov.csv`, `or/` (one
instance dump per OpenROAD sub-step, `route.def`, `final.v`, `final.odb`), `NAME.gds`, `NAME.named.gds`
(KLayout stores the DEF instance names as GDS properties; this variant keeps them, to measure name
survival), `ext_hier.spice`, `ext_flat.spice`, `lvs.log`, `census.csv`, `census.json`, `join.json`.

## Gate-level simulation of `final.v`

`tools/silicon/gate_sim.py FINAL.V --pdk-verilog <PDK>/libs.ref/sky130_fd_sc_hd/verilog --tb-top TOP --tb FILE... --pass TEXT`
runs the testbench on `final.v` with the sky130 Verilog models (iverilog, `-DFUNCTIONAL`) three times: with every
flip-flop and latch starting at X (as the models ship), at 0 and at 1 (a copy of `primitives.v` with an `initial`
state; the PDK files are not changed). Verdict `pass` (the X run passes), `x-pessimism` (the X run fails, the 0 and 1
runs pass; a WARNING says so) or `fail` (a 0 or 1 run fails: a functional difference). Exit 1 only for `fail`.

Why: the netlist keeps registers the RTL never resets (they are loaded before use). The mapped logic can be equal for 0
and for 1 on such a register, yet a gate simulator computes X from it and keeps it (for example a 4-bit counter
cleared by a write strobe: `next = f(q, strobe)` with the `q` terms cancelling when the strobe is 1). The RTL
simulation takes the `if` branch and never sees the X. The mmuart ladder run failed 42 of 61 checks from the X
start and passes all 61 from a 0 start and from a 1 start. A UDP shows its initial state on its output at its first
input event, so a flip-flop whose input never changes before its first clock edge stays X in all three runs.

## What physical design changes

Peter's question: is there obfuscation in this path? Two answers.

**(a) Unintended: what the flow does to the netlist.** Census of the two designs run (smallest first),
per the instance dump after each OpenROAD sub-step, against the previous one:

| | `bm_DL_4_bit_comparator` | `bm_DL_4_bit_shift_register` (clocked) |
|---|---|---|
| Odin III cells | 17 | 12 (4 `dfxtp_1`) |
| inserted by tapcell | 3 | 4 |
| resized by `repair_design` (same name, other master) | 1 | 4 |
| inserted by clock-tree synthesis | 0 | 3 clock buffers |
| inserted hold buffers | 0 | 0 |
| inserted fill | 26 | 52 |
| removed or renamed | 0 / 0 | 0 / 0 |
| Odin III names kept through OpenROAD | 17 of 17 | 12 of 12 |
| logic cells with an HDL chain in the final layout | 17 of 17 | 12 of 15 (the 3 clock buffers have a physical origin) |
| LVS | circuits match uniquely | circuits match uniquely |
| Odin III names in the GDS (names stripped) | 0 | 0 |
| Odin III names in the extracted SPICE | 0 | 0 |
| name-free join (type and location) | 17 of 17 | 15 of 15 |

Reading: on these two small designs, OpenROAD adds only tap, clock and fill cells and resizes some cells; it
does not rename or remove one. Names survive to the DEF, not to the extracted layout (Magic regenerates
instance names), but a join on cell type and location recovers every placed cell, so provenance reaches a
layout when the DEF placement is kept as the golden. Not shown: larger designs, restructuring
(`restructure` was not run), setup repair, a full power grid. The numbers are in
`tests/silicon/comparator/census.json` and `join.json` for the comparator.

**(b) Intended layout obfuscation in the literature** (a short note, no implementation here; only
sources that were fetched are cited):

- *Camouflaged cells*: gates whose function an attacker cannot read from the layout. El Massad, Garg and
  Tripunitara, "Integrated Circuit (IC) Decamouflaging: Reverse Engineering Camouflaged ICs within Minutes",
  NDSS 2015, challenge claims that camouflaging forces exponential effort and show a SAT-based attack that
  decamouflages a circuit in minutes: <https://www.ndss-symposium.org/ndss2015/ndss-2015-programme/integrated-circuit-ic-decamouflaging-reverse-engineering-camouflaged-ics-within-minutes/>.
- *Split manufacturing*: front-end and back-end layers made at different foundries to hide the wiring. Li et
  al., "Attacking Split Manufacturing from a Deep Learning Perspective" (arXiv 2007.03989), train a network on
  placement and routing features to recover the hidden back-end connections: <https://arxiv.org/abs/2007.03989>.
- *Unused layout space and late edits*: Perez and Pagliarini, "Hardware Trojan Insertion in Finalized
  Layouts: From Methodology to a Silicon Demonstration" (arXiv 2112.02972), insert a side-channel Trojan
  into a finalized layout through an engineering-change-order flow, in four 65 nm cryptocores:
  <https://arxiv.org/abs/2112.02972>. This is the threat the golden layout and its provenance are meant to
  catch; the fetched page did not describe filler cells, so no claim about dummy fill is made here.

## Limits

Two small designs were run; the flow has not been run on large designs and has a cell-count guard in the
plan only. The library is drive strength 1 and has no latches, tri-states or multi-output cells. The power
grid is met1 rails and met4 straps (OpenROAD warns that no met5 shapes are made). No `restructure`, no setup
repair, no DRC deck beyond OpenROAD's route report. Device-level (transistor) objects are not modelled: the
`extracted` stage holds cells (see {doc}`provenance-files`). The join uses the DEF of the golden run; a real
scanned chip has no DEF (the Leman Russ lane covers extraction from an unknown layout). Tools are optional:
without them the silicon tests skip.

Equivalence that cannot be decided is a warning, not a stop. A proven difference stops the flow. An
output the design declares but never drives is tied to 0 (`--allow-undriven`), and any other tool
limit (such as an unsupported clock form) is printed as `WARNING equiv: not proven: <reason>`; the
`equiv` step file records `equiv: not proven: <reason>` and the flow goes on to place, route and LVS.
A register on the inverted clock counts as a register on the opposite edge of that clock.

Known case: wb_lcd (OpenCores, 1049 LUTs) ends `ABC dsec ended UNDECIDED`. `rtl.blif` has 660 registers and `gates.blif` 636
(the mapping removes 24) and no register name survives from one to the other, so the two cannot be matched by name for a
combinational check. Tried on the saved pair (ABC from the repository, one core each, 3.5 to 4 GB cap): `dsec` with
default settings (153 s), `-r` (no initial retiming, 164 s), `-F 1` (111 s), `-F 8` (145 s), `-m` (139 s), `-rm` (99 s), `-a`
(phase abstraction, 203 s); `miter; strash; dprove` (180 s), `miter; strash; scorr -F 2; dretime; scorr; dprove`
(132 s); `miter; strash; scorr; pdr` (still running at the 900 s limit, frame 222). Every `dsec` and `dprove` run stops with
`UNDECIDED` on a 60 s per-frame limit. No engine or bound proved it within 15 minutes, so the not-proven WARNING stays.

Output ports that the design joins with `assign` (`assign out[6] = out[0];`) are one net in the layout
and keep one port label there. LVS gives the reference netlist the label the layout kept and lists the
joined names in the step detail as a WARNING.
