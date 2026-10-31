# scoap pass: SCOAP testability measures and signal probability as circuit properties (design, 2026-10-31)

Status: spec first, code follows on branch `feat/scoap`. Purpose: Odin III goal 2 (graph and
reverse-engineering research platform), circuit characteristics as properties. Consumers: Leman
Russ III (rare-net site selection for hardware-trojan benchmarks), later a Mimir measurer and Loki.
Measures: exactness (identical integers to an independent Python reference), runtime. Peter
2026-10-31: "a way of doing scope parameters scoap"; 2026-10-30: build generically, implement our own.

## 1. Reference and its status

- Goldstein, "Controllability/observability analysis of digital circuits", IEEE Trans. Circuits and
  Systems 26(9):685-693, 1979, doi:10.1109/TCS.1979.1084687 (verified on Crossref 2026-10-31; the
  program paper is Goldstein and Thigpen, DAC 1980, doi:10.1145/800139.804528).
- The paper text was NOT available to this lane. The rules below are the standard SCOAP rules as
  given in textbooks (Bushnell and Agrawal, Essentials of Electronic Testing, ch. 6) and recalled
  for the primitive gates; every place where this spec generalises or chooses is marked DEFAULT.
  Numbers are therefore "SCOAP as defined here", comparable with other tools only for the textbook
  gates (AND/OR/NOT/NAND/NOR/XOR on primary inputs, where the textbook values are reproduced by the
  unit tests).

## 2. Measures

Per net n (one bit):

- CC0(n), CC1(n): combinational controllability, the effort to set n to 0 or 1 (counts nets that
  must be set, i.e. the textbook depth-plus-width measure). CO(n): combinational observability,
  the effort to propagate n to a primary output.
- SC0, SC1, SO: the sequential versions, counting flip-flops (time frames) instead of gates.
  Computed only with `--sequential`.
- INF = 1000000000 (named `SCOAP_INF`, `odin3:scoap_inf`): unreachable (no assignment, no path to an
  output, undriven net). Sums saturate at INF. Every other value is a non-negative integer below INF.

## 3. Rules

Notation: c(x,v) = CCv of the net on input x; "cost of a cube" = sum over its specified literals
of c(x, literal value). Boundary:

- Primary input (a module input port bit): CC0 = CC1 = 1, SC0 = SC1 = 0. A clock input is a primary
  input like any other. Primary output (a module output port bit): CO = 0, SO = 0. A bidirectional
  port is both.
- Constant net (`odin3_net_const_value` 0 or 1): the held value v has CCv = 1, SCv = 0; the other
  value is INF (DEFAULT). x/z constants: both values 1 (DEFAULT: free). Undriven live net: INF.
- Black box, hard block, module instance, tech-library cell with state, any other cell type the
  pass has no function for (word-level cells, memories, tristate): its outputs are controllable
  like primary inputs (CC = 1, SC = 0) and its inputs observable like primary outputs (CO = 0,
  SO = 0) (DEFAULT). Counted in the summary (`opaque_cells`) and listed by type in the log, never
  skipped silently.

Combinational function cells (one output; the cell is a function f of n inputs). Covered types:
every bit-level gate (`$_BUF_ $_NOT_ $_AND_ $_OR_ $_XOR_ $_NAND_ $_NOR_ $_XNOR_ $_MUX_` and
tech-library gate cells the simulator can tabulate: `odin3_gate_truth`, at most 6 inputs), `$lut`
(its truth table) and `$sop` (its cover, ON-set or OFF-set rows; sim semantics). All are turned
into a truth table first, so the same function gives the same numbers whichever cell holds it.

- Controllability. For v in {0,1}: CCv(y) = 1 + min over the prime implicants P of {f = v} of
  cost(P), where a prime implicant is a ternary cube (0, 1, don't care per input) all of whose
  minterms have f = v and that cannot be widened. Don't-care inputs cost nothing. This gives
  AND: CC1 = CC1(a)+CC1(b)+1, CC0 = min(CC0(a),CC0(b))+1; OR dually; NOT: CC1(y)=CC0(a)+1;
  XOR: CC1 = min(CC1a+CC0b, CC0a+CC1b)+1. (DEFAULT: a `$_MUX_` includes the consensus term
  A=B=v, so CCv(Y) = 1+min(CCv(A)+CC0(S), CCv(B)+CC1(S), CCv(A)+CCv(B)).) SCv uses the same cubes
  with SC costs and without the +1 (Goldstein: sequential counts frames, not gates).
- Observability. Input x_i: CO(x_i) = CO(y) + 1 + min over assignments r of the other n-1 inputs
  for which f(x_i=0,r) != f(x_i=1,r) of the sum over the other inputs j of c(x_j, r_j). Every other
  input is driven to a known value, as the textbook XOR rule (CO(a) = CO(y)+min(CC0b,CC1b)+1)
  requires. No such r: INF. SO(x_i): the same with SC costs, SO(y), and no +1.
- A net with several readers (stem): CO(net) = min over its reader pins of the pin's value; a net
  read by a module output port has CO 0. A net with several drivers (wired, tristate): CCv = min
  over the drivers.
- DEFAULT size cap: truth-table work is exact for functions of up to 8 inputs (`SCOAP_MAX_EXACT`).
  Wider ($sop or $lut over 8 inputs): the value matching the cover rows of a `$sop` uses the cover
  rows as cubes (min over rows); every other value uses the bound CCv = 1 + sum over inputs of
  min(CC0, CC1), and CO(x_i) = CO(y) + 1 + sum over the other inputs of min(CC0, CC1) (SC, SO the
  same without +1). Counted in the summary (`wide_cells`).

Flip-flops and latches (`--sequential`; cell types with the registry flag SEQ_EDGE or SEQ_LEVEL and
ports D and Q; clock/enable pin = port 0 when the type has CLOCK_PIN0). DEFAULT: all other control
inputs (set, reset, enable) are ignored for controllability, and observed like D.

- CCv(Q) = CCv(D) + CC0(C) + CC1(C); SCv(Q) = SCv(D) + SC0(C) + SC1(C) + 1.
- CO(D) = CO(Q) + CC0(C) + CC1(C); SO(D) = SO(Q) + SC0(C) + SC1(C) + 1. Same for other
  non-clock inputs.
- CO(C) = CO(Q) + CC0(C) + CC1(C) + min(CC1(D)+CC0(Q), CC0(D)+CC1(Q)); SO(C) likewise with SC and
  +1 (the clock edge must change Q).

Without `--sequential` (default, scan view): storage cells are opaque cells (Q is a pseudo-primary
input, D a pseudo-primary output), only CC and CO are computed.

Loops. No recursion. Each measure is the least fixed point of a min-plus system, started at INF at
every non-source net and iterated by full sweeps in cell order (controllability forward, then
observability in reverse order, SC and SO the same) until a sweep changes nothing. Values only
decrease, and the value of the k-th net to settle is final after k sweeps, so at most
(nets + 2) sweeps; `SCOAP_MAX_SWEEPS` = nets + 2 is the cap and a hit is a warning and
`odin3:scoap_converged` = 0. Loops through combinational cells and flip-flops are covered by the
same iteration.

## 4. The pass

`scoap [--sequential] [--out FILE] [--top N] [--focus ...] [--sigprob VECTORS] [--seed S]
[--threshold P]`. Runs on every module with a netlist view (instances are opaque cells, so
`flatten` first for a whole-design value).

- Per-net values: JSON `odin3-scoap` version 1: per module an array of nets
  `{"id","name","cc0","cc1","co"}` plus `"sc0","sc1","so"` with `--sequential` and
  `"p1_ppm"` with `--sigprob`. `id` is the module-local net ID and `name` its printed name
  (`$n<ID>` for an unnamed net): the key to provenance (`odin3_net_prov`).
- Design summary as module attributes (INT unless noted): `odin3:scoap_version`, `scoap_inf`,
  `scoap_nets`, `scoap_cells`, `scoap_opaque_cells`, `scoap_wide_cells`, `scoap_sweeps`,
  `scoap_converged`, `scoap_max_cc`, `scoap_max_co` (largest finite values),
  `scoap_unreachable` (nets with an INF value), `scoap_sequential`. Per-net values are only in the
  file and the Python dict (and one INT attribute per net would not scale).
- `--top N`: logs and stores in the JSON (`"top"`) two lists of N nets: hardest to control (largest
  max(CC0,CC1)) and hardest to observe (largest CO); INF sorts first, ties by net ID.
- `--focus` uses write_dot syntax (cone of a net, instance path) and restricts the JSON and the
  `--top` lists to the nets of that subgraph; the values themselves are always computed on the whole
  module (a cone value depends on its surroundings).
- Info line: `scoap: M modules, N nets, C cells, mode comb|seq, S sweeps, U unreachable, T ms`.

## 5. Signal probability (an option of `scoap`, not a separate pass)

`--sigprob V`: simulates V random vectors with `sim` (`odin3_sim_build` on the top module, the
seeded PRNG of `sim`, `--seed S`, default 1, V cycles) and records, per top-module net that the
simulator lists as a signal, P(net = 1) = ones / V as `p1_ppm` (parts per million, integer). Nets
the simulator does not list are `-1`. `--threshold P` (default 0.01, a decimal) lists the rare nets:
min(P, 1-P) < P (JSON `"rare"`). Only the top module (the simulator flattens). V and the seed are
logged and stored in the JSON. Checked against exact enumeration over all 2^k input vectors on small
combinational circuits (statistical tolerance 5 sigma). A sequential design gives the
cycle-average, not a steady-state value.

## 6. Python API and docs

`Design.scoap(sequential=False, top=0, focus=(), sigprob=0, seed=1, threshold=0.01)` returns the JSON
as a dict. `docs/user/scoap.md`, PASSES.md row. No IR change, no ADR (attributes only).

## 7. Gates

1. Unit tests (Unity) on hand-computed circuits: AND/OR/NOT/XOR chains, a mux, a 2-bit counter
   (SC values), a `$sop` cover, a `$lut`.
2. Independent Python reference (`tools/scoap/scoap_ref.py`, reads the BLIF itself, no Odin III
   code) equals the pass integer for integer on the micro goldens (tests/micro, as BLIFs in
   ~/odin3-ws/golden) and the 14 small VTR BLIFs, comb and sequential modes.
3. Construction check: every live net of every module has a value; opaque cell types listed.
4. Signal probability against exact enumeration on small circuits.

## 8. Next step (resume note)

Written when the lane stopped or finished; see the PR description for the state.
