# 1G — Tech library: a target-agnostic cell library format

Status: draft by the agent; PHASE1 #7 (syntax) taken as the agent default, **#8 (adding 1G to
Phase 1 and amending spec §6/§7/§12/§13) waits for Peter** — this PR stays open for him.

## Purpose (Peter, PHASE1 #4)

A `read_arch` format that is target-agnostic: it describes the cells a target offers, starting
with plain gates (like an ASIC techlib), so the flow can map to "just logic" first; FPGA
targeting (LUT-K, carry chains, DSPs, RAMs) grows from the same format. In Phase 1 it also
gives the BLIF reader real types for the goldens' `adder`, `multiply`, `single_port_ram`,
`dual_port_ram` (IR-7b) instead of guessed black boxes.

## Decisions

1. **Own text format, `.o3lib`** (PHASE1 #7): line-oriented, diffable, comment with `#`. Not
   Liberty (huge, ASIC-timing-centric, no word-level functions), not genlib (combinational
   gates only), not VPR XML (no functions, no widths — widths live in pb_types). Importers for
   Liberty/genlib and VPR `<models>` come later and produce `.o3lib` cells.
2. **A cell's function is an expression over its ports**, compiled to an **IR fragment** of
   built-in word/bit cells. One mechanism then gives (a) simulation semantics for hard cells in
   1E, (b) equivalence models (replacing `equiv-check`'s hand-written `multiply`, PHASE0 #8), and
   (c) the matcher patterns of Phase 4 (spec §7's open DSL, §13 #1: patterns are IR fragments —
   D5). Sequential cells use `seq`; memories use `memory` (semantics in Phase 4).
3. **Widths may be parametric.** A port width is an integer expression over the cell's integer
   parameters (`A_WIDTH`, `A_WIDTH+B_WIDTH`). When a netlist declares the cell as a black box
   (`.model multiply .blackbox` with 36-bit `a`), the reader infers the parameters from the
   declared widths and checks the rest (IR-7b's compatibility check, widened to parametric
   ports).
4. **Granularity**: a library cell is `gate` (→ `bit` granularity) or `hard` (→ `hard`).
   Optional `area` and per-arc `delay` numbers are stored as cell attributes for later mapping
   cost models; Phase 1 ignores them.

## Syntax

```
library vtr_k6_frac_N10_mem32K          # one per file
cell adder hard
  in  a 1
  in  b 1
  in  cin 1
  out cout 1
  out sumout 1
  fn  sumout = a ^ b ^ cin
  fn  cout   = (a & b) | (a & cin) | (b & cin)
end
cell multiply hard
  param A_WIDTH int 36
  param B_WIDTH int 36
  in  a A_WIDTH
  in  b B_WIDTH
  out out A_WIDTH + B_WIDTH
  fn  out = a * b                       # unsigned unless `signed` follows the port width
end
cell AND2 gate area 1
  in A 1 ; in B 1 ; out Y 1
  fn Y = A & B
end
cell DFFP gate area 4
  in D 1 ; in C 1 clock ; out Q 1
  seq Q <= D @ posedge C init x
end
cell single_port_ram hard
  param ADDR_WIDTH int 15 ; param DATA_WIDTH int 1
  in addr ADDR_WIDTH ; in data DATA_WIDTH ; in we 1 ; in clk 1 clock
  out out DATA_WIDTH
  memory words 2 ** ADDR_WIDTH ; width DATA_WIDTH ; write sync clk we ; read sync clk
end
```

`;` separates statements on one line. Expressions: identifiers (ports, parameters), integer and
sized literals (`4'b10x1`), `~ ! & | ^ + - * << >> == != < <= > >= ?:`, slices `a[3:0]`, bit
selects `a[2]`, concatenation `{a, b}`, replication `{4{a}}`, parentheses. Precedence and
signedness follow Verilog-2005. Every `out` port needs exactly one `fn`, `seq` or `memory`
driving it (or the cell is a black box: no functions at all, legal only with `blackbox` kind).

## Phase 1 scope

- `src/techlib/` reader: parse `.o3lib`, report errors with file:line, register each cell as a
  design-local cell type (IR-11) with ports, parameters, width rules (expression evaluator over
  integer parameters), flags (`clock` ports), attributes (`area`, `delay`), and the parsed
  function kept for 1E (simulation) — the IR-fragment compiler lands with 1E or Phase 4.
- Libraries in `lib/`: `generic_gates.o3lib` (BUF, INV, AND2/3/4, OR2/3/4, NAND2, NOR2, XOR2,
  XNOR2, MUX2, DFFP, DFFN, DLATCHP, DLATCHN, TIE0, TIE1) and `vtr.o3lib` (`adder`, `multiply`,
  `single_port_ram`, `dual_port_ram`, with parameters covering every width the goldens use).
- IR-7b compatibility widened to parametric widths; the BLIF reader (1C) loads `vtr.o3lib`
  before reading a golden.
- Tests: parse every construct; reject each malformed form with a located error; parametric
  width inference from a declared black box; the goldens' `adder`/`multiply`/RAM declarations
  all resolve against `vtr.o3lib`.

## Out of scope for Phase 1

Mapping onto a library (Phase 3 `abc` with a generic gate library; Phase 4 partial mapping),
Liberty/genlib/VPR-XML importers, timing, the IR-fragment compiler for `fn` (1E/Phase 4).

## Spec changes this implies (decision #8 — Peter)

- §6 step 7 `read_arch`: reads `.o3lib` tech libraries (target-agnostic; gates first), plus VPR
  XML via an importer later.
- §7: "pattern DSL" → library-cell functions are expressions compiled to IR fragments; §13 #1
  resolved by this.
- §12 Phase 1 deliverables gain "tech-library format + reader + generic gate library"; Phase 4
  keeps VPR-XML import and partial mapping.
