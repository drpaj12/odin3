# Odin III simulator: 4-state values, any width (design note)

Date: 2026-11-05. Status: draft, for Peter and the controller before any code.
Lane: odin3-sim-parity phase 1 (brief: work/controller-scripts/briefs/odin3-sim-parity.md).

Purpose: simulate on Odin III's own IR so activity ties to HDL lines, cells, LUTs and placed objects
through provenance, with iverilog as the oracle only. Measure: number of Mimir OpenCores testbenches
that pass under `sim_tb` with output identical to iverilog (3 today). Blockers this note serves:
x/z values (usbhostslave, des, spi, i2c, simple_spi) and signals wider than 64 bits (sha3_high,
sha3_low). `real`, speed and parse items are other phases.

## 1. Today

- Netlist simulator (`src/sim`, sim.h, cell.h): one byte per net bit in `sim->values`, always 0 or 1.
  Cell `simulate` hooks (IR-8) read and write bytes through `odin3_cells_sim_in/out` (bool) and, for
  word cells, through little-endian 64-bit limb words (`word.h`, any width, no x/z). Const X/Z read
  as 0, latch INIT 2 and 3 start at 0, a top input starts at 0, `$tribuf` has no hook ("cannot
  simulate"), a net with two drivers is refused unless the drivers are tristate or inout.
- Testbench interpreter (`tb_*`): `uint64_t` values, `TB_MAX_WIDTH = 64`, `x`/`z` digits, `casez`,
  `casex`, `===` rejected; `tb_inst_signal` refuses DUT signals over 64 bits.
- Public API (S1): `odin3_simulation_set/get` use `uint64_t`; `set_bits/get_bits` use 0/1 bytes.
- IR already has 4 states in parameters: `odin3_bit` is `ODIN3_BIT_0, _1, _X, _Z` = 0,1,2,3 (value.h,
  IR-10); `$const` BITS parameters may hold x/z.

## 2. Decision: one encoding, two views of it

A 4-state bit is the two-bit code `(u v)` with `v` the low bit, equal to the IR's `odin3_bit`:

| code | v | u | meaning |
|------|---|---|---------|
| 0 | 0 | 0 | 0 |
| 1 | 1 | 0 | 1 |
| 2 | 0 | 1 | x |
| 3 | 1 | 1 | z |

- **Netlist storage (`sim->values`)**: stays one byte per bit slot; the byte is the code. A 2-state
  design has bytes 0/1 only, so storage, `set_input`/`get_output`, VCD and signal peeks are unchanged.
  Unknown test: `byte & 2`. No layout change, no extra array.
- **Word and testbench values**: two planes of `odin3_word_limbs(width)` limbs, `v` and `u`, any
  width. Bit k of the value is `(u_k v_k)`. `word_load4/store4` convert byte spans to planes and back
  (the same widening and truncation rules as `word_load/store`: extension by signedness; an x or z
  sign bit extends as x or z). A known word is a word whose `u` plane is all zero; it is then exactly
  today's word, so every existing `odin3_word_*` operation is the 2-state kernel of the 4-state one.
- Why planes plus bytes, not one representation: the netlist is bit-blasted and indexed through
  `idx[]` (a byte per slot is the cheapest random access, and the levelized engine is built on it);
  the testbench and word cells work on whole words, where planes give word-parallel x-propagation
  (`u_out = (u_a | u_b) & ~kill` per limb). Both are the same 2 bits, so conversion is shifts and masks.
- Why this code and not VPI's `aval/bval`: VPI swaps x and z (x = `a1 b1`). Using the IR's `odin3_bit`
  keeps one code across IR parameters, simulator, BLIF/JSON writers and the testbench; a VPI/DPI
  adapter, if ever wanted, swaps in one function.
- `tb_var` (reg/wire/integer/memory words) becomes `tb_val`: width, signedness, planes. Up to 64
  bits the planes are inline (`uint64_t v, u`), so the common case keeps today's cost; wider values
  point into the module's limb arena sized at parse (no allocation in the hot path; no recursion
  stays true). `TB_MAX_WIDTH` goes away; `TB_WR_MAX` and the explicit eval stack hold limb offsets.

## 3. Modes and the 2-state fast path

The cost must be paid only by callers that want x/z.

- `odin3_sim_options` gets `bool four_state` (default false). False: bit-identical behaviour to
  today (INIT 2/3 and const X/Z read 0, top inputs start 0, no resolution cells). All current callers
  (scoap `--sigprob`, `odin3_simulation_*`, sim-check, equiv flows, `sim_pass`) stay 2-state and pay
  nothing. True: x/z semantics of this note; `sim_tb` turns it on by default (`--2state` keeps the
  old mode for regression and for speed when a bench is known x-free).
- In 4-state mode the per-cell fast path is dynamic: a hook (or the generic wrapper, section 5) first
  tests its input bytes for `& 2`; all known runs the existing 2-state hook body unchanged. Word cells
  test the `u` planes (one OR per limb). Expected cost: an OR and a predictable branch per input
  bit/limb. Budget: 4-state mode at most 1.3x the time of 2-state mode on a design that never
  produces x after reset (measured in phase 2 on LU64PEEng and the 14 small VTR; a miss is fixed
  before phase 2 is called done, or the gap is reported).
- A static, build-time cut (compile the design down to 2-state when no x source exists) is rejected:
  nearly every registered design has x sources (flop Q before reset), so it would rarely fire.
  Phase 4 (levelized/compiled evaluation) may add per-cell "inputs unchanged since last settle"
  skipping, which helps both modes.

## 4. Where x and z come from

4-state mode only:
- Flop and latch Q before the first edge or reset: x, unless INIT is 0 or 1 (INIT 2 and 3 become x;
  today 0). Memories (`$mem` arrays): x until written.
- `$const` x/z bits drive x/z. Unconnected cell input pins read z (new reserved slot
  `ODIN3_SIM_SLOT_Z`; `ZERO` stays for 2-state mode and for constant-0 uses). Unconnected outputs
  still write `DISCARD`.
- Top-level inputs and testbench regs/wires start x, as iverilog; reset therefore matters, as in
  iverilog (the testbench spec's "powers on with all state 0, where iverilog has x" caveat goes away
  in this mode).
- An undriven net (no driver at all) reads z.

## 5. Operator semantics (IEEE 1364-2005, iverilog as oracle)

Testbench expressions and the netlist word cells share the kernels (`odin3_w4_*` in word.h/word.c).
`known(a)` = `a.u == 0`. "All-x" = every result bit x. z as an operand bit behaves as x in every
operator below except where stated.

| operator | result |
|---|---|
| `~a` | per bit: x/z gives x |
| `a & b` | per bit: 0 if either is 0, else x if either unknown, else 1 |
| `a | b` | per bit: 1 if either is 1, else x if either unknown, else 0 |
| `a ^ b`, `a ~^ b` | per bit: x if either unknown |
| `&a`, `|a`, `^a` (reductions, and nand/nor/xnor forms) | `&`: 0 if any bit 0, else x if any unknown, else 1; `|`: 1 if any bit 1, else x if any unknown, else 0; `^`: x if any unknown |
| `!a`, `a && b`, `a || b` | operand truth: 1 if any bit is 1; 0 if all bits 0; else x. `&&`: 0 if either is 0; `||`: 1 if either is 1; otherwise x if either is x |
| `+ - * / % **`, unary `-` | any unknown bit in any operand: all-x (result width); division or modulus by 0: all-x |
| `<< >> <<< >>>` | x/z in the shift amount: all-x. Bits of the shifted operand move with their state; fill is 0, except `>>>` of a signed operand fills with the sign bit (which may be x/z) |
| `< <= > >=` | any unknown bit in either operand: 1'bx |
| `==`, `!=` | if a bit position is known in both and differs: 0 for `==`, 1 for `!=`, whatever the other bits; else if any bit is unknown: 1'bx; else equal gives 1/0 |
| `===`, `!==` | bitwise on all 4 states (x equals x, z equals z only); always 0 or 1 |
| `c ? a : b` | c truth 1: a; 0: b; x/z: bitwise merge, bit equal and known in both gives that bit, else x |
| `{}`, `{n{}}`, `[i]`, `[m:l]` | states move unchanged; a select with unknown index, or out of range, reads x (part select: all-x); a write through an unknown or out-of-range index is dropped |
| extension | a literal with a leading x or z digit extends with x or z (unsized `'bx` fills the context width); sign extension of a signed value copies the sign bit's state |
| `if (c)` | c truth 1: then branch; otherwise (0, x or z): else branch. `while`, `for`, `wait` conditions: x is false; `repeat (x)`: zero iterations |
| `case` | labels compared by `===`, first match wins |
| `casez` | z bits (and `?`) in the label or the expression are don't-care |
| `casex` | x and z bits in either side are don't-care |
| `@(posedge s)` | 0 to 1, 0 to x/z, x/z to 1 (a change toward 1); `@(negedge s)` the mirror; on a multi-bit signal the least significant bit |
| `$display` | `%b`: x or z per bit. `%h`/`%o`: a digit whose bits are all x prints `x`, all z prints `z`, a mix with any unknown prints `X` or `Z` (uppercase) as iverilog; `%d`: all bits unknown prints `x` (or `z`), part unknown prints `X` (or `Z`), width and justification as for known values; `%t`, `%c`, `%s` use the known bits (x as 0, with iverilog's behaviour checked in the micro) |

Net-level behaviour of netlist cells (the DUT): gate cells use the bit rows above; `$mux`/`$pmux`
with an unknown select use the `?:` merge; `$sop` and `$lut` are exact (the output is known when all
completions of the unknown inputs agree: enumerate unknown inputs, at most 2^6, wider LUTs are rare
and fall back to all-x); flip-flop D unknown loads x; a level latch with unknown enable holds if D
equals Q and else goes x; edge-triggered cells fire on the 4-state edge rule above (`odin3_sim_clock_edge`
and `odin3_sim_cycle` keep 2-state clocks, since they drive only 0 and 1).

Known divergence to flag, not hide: `sim_tb` simulates the elaborated netlist, not the RTL. An RTL
`if (x)` takes the else branch, but after `proc` lowering it is a `$mux`, which merges. Two defences:
(1) lowering of `if`/`case` marks its muxes with a node attribute (`sim:rtl_if`, set by `proc`, no
IR parameter change), and the simulator gives those muxes the RTL rule (unknown select takes the
else side), while muxes from `?:` get the merge rule; (2) the parity sweep reports every difference
as x-vs-known or known-vs-known, so optimism or pessimism from optimization passes is visible, and
the sweep can also be run on an un-optimized netlist. Phase 2 must verify how `if`/`case` are
represented after `proc` before relying on (1); if the information is not there the fix is in the
front end, not the simulator. Where Odin III cannot match iverilog, it prints a WARNING (one per
construct), per the Parmys-default rule applied to simulation.

Types with no exact 4-state hook (tech-library hard cells interpreted by `fnsim_eval`, plugin types)
get a generic wrapper: all inputs known runs the 2-state hook. With up to 6 unknown input bits and
at most 64 output bits the hook runs on every 0/1 completion of the unknown bits and an output bit
is known only when all completions agree (exact, as the gate netlist `lower` makes; v1.6 step 0:
the old rule, any unknown input makes every output x, made a hardmap'd adder chain x where the
lowered gates are known, and failed testbenches that pass on the lowered netlist). Beyond those
caps every output is x.

## 6. Tri-state and several drivers

- Resolution per bit, strengths not modelled (all strong): `z` with `v` gives `v`; `v` with the same
  `v` gives `v`; `0` with `1` gives x; any x gives x; `z` with `z` gives z.
- Build, 4-state mode: each driver of a net that has several tristate or inout drivers writes a
  private slot; a synthesized flat resolve cell (a built-in internal type, comb, levelized like any
  gate) combines them into the net slot. A net with one driver is untouched (no cell, no cost). The
  existing "more than one driver" refusal keeps its meaning for non-tristate drivers.
- `$tribuf` gets hooks: Y = A when EN is 1, z when EN is 0, x when EN is x (all in 2-state mode: EN
  unknown cannot occur, but a $tribuf then reads as buffer plus a 2-state "z reads as 0" so that the
  existing refusal "cannot simulate" is lifted only in 4-state mode).
- `inout` of the top: allowed in 4-state mode, as a bus between the testbench net and the DUT pin; the
  testbench resolves `wire` nets with several drivers (`assign`, port outputs) with the same function.
- Not supported (located error or WARNING): net strengths, `pullup/pulldown`, `tri0/tri1`, `trireg`,
  `wand/wor`; an unsupported use is refused at parse with a located message, never silently treated
  as `wire`.

## 7. API and ABI impact (additive only)

- `odin3_sim_options.four_state` (new trailing field, zero default); `odin3_sim_input_*` and
  `odin3_sim_get_output` keep their `bool`; a `bool` read of an unknown bit returns 0 (documented).
  New: `odin3_sim_set_input4/get_output4/get_input4` with an `odin3_bit`, and
  `odin3_sim_is_four_state`.
- `odin3_simulation_*` (public, ABI 7): unchanged functions; new `odin3_simulation_open4` (or a flag
  in the open options), `odin3_simulation_get4(sim, signal, uint64_t *v, uint64_t *u)` for
  width <= 64, `odin3_simulation_get_bits4/set_bits4` with `odin3_bit` bytes for any width. `get`
  and `get_bits` on a 4-state simulation return the `v` plane with unknown bits as 0. Needs
  `tools/check-symbols.sh` and the ABI doc updated; the ABI number is decided in phase 2 (additive
  functions: minor note only, no bump if the rule in docs/ABI says so).
- `odin3_sim_cell` (internal, opaque): new `bool four_state` flag; values bytes are codes 0..3 when
  set. Existing hooks keep working in 2-state mode untouched. `odin3_celltype_def` and
  `odin3_plugin_celltype`: the 4-state hook goes in `reserved[0]` (`simulate4`, NULL allowed), so no
  field moves and old plugins load; NULL falls back to the generic wrapper of section 5.
- `word.h` additions only: the planes types, `odin3_w4_*` operations and `word_load4/store4`.
  The existing `odin3_word_*` functions are unchanged and are the kernels.
- Testbench language: removes the `x`/`z`/`casez`/`casex`/`===` rejections; spec
  2026-10-31-testbench-design.md updated in phase 2 together with the code.
- VCD: dumps `x` and `z` characters, and vectors of any width (header unchanged).
- Provenance, IDs, hierarchy, attrs: untouched. The one new IR-side item is the `sim:rtl_if` node
  attribute (an attr key, not a layout change), only if phase 2 confirms it is needed.

## 8. Testing (phase 2, written before the code)

- Micros under `tests/sim4/`, one per operator class in section 5 (logic, reduction, arithmetic,
  shift, relational, equality, `===`, `?:`, concat/select, case/casez/casex, if with x, edge rules,
  tri-state buses, `%b/%h/%d` formats, >64-bit add/mul/shift/compare), each a small testbench whose
  `sim_tb` output must equal iverilog's byte for byte (the existing `tests/tb` harness and
  `cmp_x.py` pattern). A micro that Odin III cannot match is a failing test first, then the fix, or
  a recorded WARNING case.
- 2-state regression: every existing sim test, goldens and `sim-check` unchanged (four_state off);
  4-state mode on the same designs with all inputs and state known must give the same outputs
  (differential test of the fast path).
- Then the 7 blocked designs (usbhostslave, des, spi, i2c, simple_spi, sha3_high, sha3_low); results
  recorded per design as identical or first differing line.

## 9. Decisions for Peter and the controller

1. Encoding: IR `odin3_bit` code (0,1,x,z = 0..3), byte per bit in the netlist, two planes in words.
   Recommended yes.
2. 4-state is opt-in per simulator (`four_state`), default on in `sim_tb` only; all other callers stay
   2-state and bit-identical. Recommended yes (zero regression risk, zero cost for them).
3. Netlist-vs-RTL x semantics: mark `if`/`case` muxes with an attribute and give them the RTL rule;
   accept that some x-vs-known differences vs iverilog may remain from optimization passes, reported
   by the sweep. Recommended yes, with the sweep as the judge.
4. Tri-state: resolution cell for multi-driver nets, no strengths or pulls in this lane.
5. Speed budget for the 4-state mode: at most 1.3x the 2-state time on x-free runs.
6. ABI: new hooks in `reserved[0]`, new `_open4/_get4` functions; no existing signature changes.

## 10. Addendum 1 (2026-11-05, phase 2 start): carrying the RTL meaning of `if`/`case` through proc and opt

Risk check result (micro `ifx.v`, `read_verilog; proc; opt; lower`, checked against iverilog with
selectors forced to x): iverilog gives `if (x)` the else branch, `case (x)` the default branch and
`c ? a : b` the bitwise merge. After `proc`, `if` and `?:` are both a `$mux` (told apart only by their
`src` range), and `case` is `$eq` plus `$pmux`. After `lower` all of them are `$_MUX_`/AND/OR/XOR
gates. A mark therefore does not survive on its own; this addendum fixes where it is made and kept.

Mark: node attribute `rtl_sel` (INT 1), set by `proc` (proc_mux, the cell builder `cb_attrs`, the
place that already sets `full_case`/`parallel_case`) on every cell the mux phase builds from a
switch: `$mux`, `$pmux`, and the `$eq`, `$reduce_or` and `$logic_not` that build the case selects.
`?:` muxes come from the front end, not from `proc`, and carry no mark: they keep the merge rule.
The mark adds an attribute only; no cell type, parameter or pin changes, and `write_json` golden
comparisons that ignore attributes are unaffected (checked in the PR with the golden tests).

Meaning in the 4-state simulator (cell flag `ODIN3_SIM_CELL_RTL_SEL` in the cell view, read from the
attribute once at build):
- `$mux` marked: select unknown takes the A side (the else/default side, as in Yosys's `if`
  lowering); unmarked: merge.
- `$eq`, `$ne` marked: compare with `===` (x equals x, z equals z), result always known. Together
  with the `casez`/`casex` masks proc already builds, a `case` select is then always 0 or 1, and
  `$pmux` (and `$reduce_or`, `$logic_not` over known inputs) behave as plain 2-state cells.
- `$pmux` marked: the first set select wins as in the RTL, none set gives the A (default) side.

Through opt: opt passes that rebuild or replace a cell must copy `rtl_sel` (checked by a unit test
per pass: `opt_expr`, `opt_merge`, `opt_muxtree`, `opt_clean`; a pass that merges a marked and an
unmarked cell drops the mark, i.e. keeps the conservative merge rule). Through lower: a marked
`$mux` copies the mark to its `$_MUX_` bits (same place `keep` is copied); lowered `$eq`/`$pmux`
become plain gates and cannot carry `===`, so `lower` prints one WARNING per module with marked
cells it flattened ("rtl_sel lost: x selectors propagate x here, iverilog takes the default").
Consequence stated for the parity sweep: `sim_tb` of an x-bearing bench is run on the netlist before
`lower` (`read_verilog; proc; opt; sim_tb`); the word cells all have simulate hooks, so nothing needs
lowering for the simulator.
