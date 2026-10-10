# 2C — Verilog-2005 synthesizable-subset coverage audit

Status: agent draft for 2C/2D (PHASE2 decision #3). Phase 2, input to sub-projects 2A (AST node
kinds), 2C (parser) and 2D (elaboration). Spec: `docs/DESIGN.md` §4.1, §6 steps 1–4, §12 (Phase 2).

## 1. Question and answer in brief

Peter asked us to follow Odin II-style parsing and to find what in synthesizable Verilog-2005 the
Odin II grammar does not cover. Short answer: the Odin II grammar (VTR `3c9a4d23`) covers the
common RTL core: ANSI and non-ANSI modules, `parameter`/`localparam`/`defparam`, `#()`
overrides, `assign`, `always` with edges, `if`, plain `case`, `for`, generate `for`/`if`/`case`,
non-ANSI functions and tasks, n-input gates, and the preprocessor. It has four kinds of hole:

1. **Constructs missing from the grammar** that the books teach or that the standard puts in the
   synthesizable subset: `casez`/`casex` and `?` digits, multi-expression case items, attributes
   `(* *)`, ANSI function headers, block-local declarations, a `generate` region with more than
   one item, `repeat`, typed parameters, `` `default_nettype none ``, instance and gate arrays,
   `bufif`/`notif`, and escaped identifiers.
2. **Constructs that parse but do not elaborate**, or crash. Examples: non-constant bit-selects
   and indexed part-selects on vectors, ascending or negative ranges, `===`, `/` and `%` by
   non-powers of two, `**` with a variable exponent, `while`, `inout`, implicit nets, unary `+`,
   reduction `^` over more than 3 bits, and most declarations inside a generate loop.
3. **Constructs that elaborate to the wrong netlist** (proved with `tools/equiv-check` against
   Yosys). Signedness is dropped throughout. Comparisons are done at the narrower operand width.
   A blocking temporary in a clocked `always` becomes a register. Non-constant case items and `x`
   case items are mis-handled. `wand`/`wor` become multi-driver nets. Nested `defparam` is lost.
4. **Lexer and preprocessor bugs.** A nested `` `ifdef `` inside a skipped region breaks the
   region. `` `begin_keywords `` uses a Unicode quote. Real literals and spaced sized numbers do
   not lex.

§4 is the full table, §5 the gap list, §6 the notes for 2A and 2D, §7 the decisions for Peter.

## 2. Sources and method

| Source | What was used | Citation form |
|---|---|---|
| P. P. Chu, *FPGA Prototyping by Verilog Examples* (Wiley 2008), `BOOK_VERILOG_VHDL/FPGA Prototyping with Verilog examples-2.pdf` (primary) | Ch. 1, 3, 4, 7 and §12.4 read in full. Text layer extracted with `pypdf`. Board and peripheral chapters skipped. | `C p.N` = printed page (PDF page − 33) |
| J. Bhasker, *Verilog HDL Synthesis: A Practical Primer* (Star Galaxy 1998), `…(Bhasker)-1.pdf` | Scanned, with no text layer. Page images extracted (`pypdf`+`pillow`) and read: TOC, §1.2, §2.2.4, §2.4.2, §2.9–2.10, §2.14.5–6, §2.15, §2.16, §2.18, §2.20, §2.21, §5.7, **Appendix A** (support table of the ArchSyn v14.0 synthesis subset, pp. 191–197). | `B p.N` = printed page |
| IEEE 1364-2005 | Clause numbers are from memory of the 2005 text. The standard is not in the workspace, so spot-check a clause before quoting it in a spec. | `§N.N` |
| IEEE 1364.1-2002 (RTL synthesis) | Status is S = supported, I = ignored, N = not supported. It is recalled, not read, because the standard is not in the workspace. **`?` marks a status I am not sure of.** Bhasker App. A is used as the citable proxy where they agree. | column "1364.1" |
| Odin II source, VTR `3c9a4d23` | `y:` = `odin_ii/src/verilog/verilog_bison.y`, `l:` = `verilog_flex.l`, `pma:` = `odin_ii/src/ast/parse_making_ast.cpp`, `ae:` = `ast/ast_elaborate.cpp`, `au:` = `ast/ast_util.cpp`, `nca:` = `odin_ii/src/netlist/netlist_create_from_ast.cpp` | `y:740` = line 740 |
| Odin II binary | `build/odin_ii/odin_ii -V f.v -o f.blif`, also run with `-a vtr_flow/arch/timing/EArch.xml`. All verdicts were the same with and without the architecture. | probe name in *italics* |
| Yosys 0.69+270 (`c4a0a2c48`) | `read_verilog f.v; hierarchy -auto-top; proc; opt_clean` | column "Yosys" |
| Odin II goldens | `~/odin3-ws/golden/EArch/{regression,vtr}/**/*.odin.{prov,log}`: 133 of 563 micro runs failed, 1 of 31 VTR benchmarks failed. | "golden" |

**Behaviour probes.** About 340 one-module Verilog files, one per construct or variant, were run
through Odin II and Yosys. Each Odin II run that produced a BLIF was checked against Yosys
(`synth -flatten -top top; abc -lut 4; write_blif`) with `tools/equiv-check`. The probe sources
live in the session scratchpad and are not committed. Each one is quoted or named here so 2C can
recreate it as a test.

**Usage counts.** Each count is the number of files containing the construct. The scan is
grep-level, run after comments and strings are stripped. Corpora: `tests/micro/verilog` (610
`.v`/`.vh`), `vtr_flow/benchmarks/verilog/*.v` (31) and `…/koios/*.{v,vh}` (33). Counts are
shown as micro/vtr/koios. Some counts are heuristic and are marked `~`. A few were confirmed by
direct grep: multi-item case labels are 0/0/0, and hierarchical names occur only as `defparam`
targets.

### Codes used in the table

- **Odin II**: `P-` = syntax error. `E-` = parses, then elaboration errors out or crashes.
  `E≠` = produces a netlist that `equiv-check` proves NOT equivalent to Yosys. `OK=` = netlist
  equivalent to Yosys. `OK~` = netlist produced, not equivalence-checked (latches, async reset or
  several clocks are outside what equiv-check supports). `ok` = covered by passing micro goldens
  only.
- **Yosys**: `Y` = accepted. `Y(w)` = accepted with a warning. `N` = rejected.
- **Verdict** (Odin III, Phase 2):
  - **MUST** = parse (2C) and elaborate correctly (2D); needed for the books or the exit-test corpus.
  - **SHOULD** = synthesizable and wanted, but nothing in the corpus needs it; scheduled after the MUST rows.
  - **IGNORE** = parse, drop and emit a located warning.
  - **REJECT** = recognise the construct and emit a located error naming it, not a generic syntax error.
  - Every verdict except REJECT requires a grammar rule. REJECT requires one where the construct
    is legal Verilog-2005.

## 3. The Odin II grammar at a glance (what exists)

- `module` accepts an optional `#(parameter …)` list and an optional port list (y:233–258).
  Ports are ANSI or bare names (y:265–272). The body has items (y:279–329).
- **Declarations**: `variable` is `name`, `[r] name`, `[r] name [r]`, `[r] name [r][r]`, or
  `… = expr` (y:515–522). An unpacked dimension needs a packed range first. All wire-like net
  types collapse to `WIRE` (y:910–922). `supply0/1` are missing. `integer` and `genvar` take
  their own lists (y:462–468, 528–532).
- **Statements** (y:697–708): `begin[:name] stmts end`, task call, `$finish/$display/$…;`,
  `=`, `<=`, `assign` (the same rule as module-level `assign`, y:534, 703), `if`, plain
  `case`, `for`, `while`, `;`. There is no `casez/casex`, `repeat`, `forever`, `wait`,
  `disable`, `fork`, `force/release/deassign`, event trigger, or block-local declaration.
- **Timing**: `@(…)`, `@*`, `@(*)` (y:800–804). Delays are only `#int`, `#(int[,int[,int]])`
  (y:806–811).
- **Expressions**: every 2005 operator token is present (y:830–878). A primary is
  `id`, `id[e]`, `id[e][e]`, `id[e+:e]`, `id[e-:e]`, `id[e:e]`, `id[e:e][e:e]` or `{list}`
  (y:880–889). `a.b.c` is lexed as one identifier (l:394). Only `$clog2`, `$signed` and
  `$unsigned` are expression system functions (l:375–377). Other `$x` are statement-only
  (y:896–903).
- **Generate**: `generate <one item> endgenerate` (y:656–658). Loop, `if` and `case` generate
  are also accepted as bare module items (y:307–309).
- **Functions**: non-ANSI only in practice. `function_input_declaration` requires a `;` after
  each input, even inside the parenthesised header (y:416–420). Function bodies allow plain
  `case`, `for`, `while`, `if`, `=` (y:710–750).
- **Lexer**: there is no rule for `(*`. `?` is not a number digit (l:137–140). A real literal
  lexes as `1` `.` `5`. `\` falls into the catch-all and is dropped (escaped identifiers are
  silently re-lexed). Every keyword token passes through `ieee_filter` (y:950–1125). A
  token missing from its case list is rejected, and `none` is missing.

## 4. Construct table

### 4.1 Lexical conventions and preprocessor

| # | Construct | 1364-2005 | 1364.1 | Books | Odin II | micro/vtr/koios | Yosys | Verdict |
|---|---|---|---|---|---|---|---|---|
| L1 | Comments `//`, `/* */` | §3.3 | S | C p.3–4 | OK (l:~399–430) | all | Y | MUST |
| L2 | Identifiers with `_`, `$` | §3.7 | S | C p.3 | OK= *lex_dollar_ident* | — | Y | MUST |
| L3 | Escaped identifiers `\a+b ` | §3.7.1 | S | — | P-: `\` dropped by catch-all, rest re-lexed (*lex_escaped_id*) | 0/0/0 | Y | MUST (2C) — our own 1F writer and Yosys/Parmys/VQM netlists emit them |
| L4 | Sized based numbers, `_`, `x`/`z` digits | §3.5.1 | S | C p.5–6 Tab 1.2; B p.9 | OK= (l:137–140, 388–391); `x` bits are don't-cares, so tools differ (*lex_xz_literal*) | xz 5/0/8 | Y | MUST |
| L5 | Unsized based `'hF`, unsized decimal = 32-bit signed | §3.5.1 | S | C p.5, p.45 | OK= *lex_unsized_based*, *op2_unsized_neg* | 1/2/11 | Y | MUST |
| L6 | Signed based literal `4'sb1000` | §3.5.1 | S | C p.5 | OK= (l:388 `'[sS]?`) | 1/0/1 | Y | MUST |
| L7 | `?` as a `z` digit | §3.5.1 | S | C p.56 (casez items) | P- (digit classes lack `?`, l:137–140) | 0/0/0 | Y | MUST (needed for casez) |
| L8 | Spaces inside a number `8 'h FF` | §3.5.1 | S | — | P- (one regex, l:388–391) | 0/0/0 | Y | SHOULD |
| L9 | Real literals `1.5`, `1e3` | §3.5.2 | N | C p.5; B App.A p.193 | P- | 0/0/0 | Y | REJECT in expressions (2C must still lex them); IGNORE in delays and specify |
| L10 | String literals (parameters, compare) | §3.6, §5.2.3 | ? | B App.A p.193 "not supported" | compare OK=; bit-select of a string parameter leaves output undriven (*lex_string_param* E≠) | 14/1/8 outside `` `include `` | Y | MUST (micros `defparam_string` etc.) |
| L11 | Attributes `(* … *)` on items, ports, statements, instances, operators | §3.8 | S (cl. 6 defines synthesis attributes) | C p.57 (2001 attributes for full/parallel case) | P- (no lexer rule; `(*` → `(` `*`), *attr_\** | 0/0/0 (only inside comments) | Y (not on `assign`) | MUST (2C: kept on every AST node, DESIGN §4.1) |
| L12 | Metacomments `// synopsys full_case parallel_case`, `translate_off/on` | not IEEE | ? (deprecated) | B §5.7 p.183, §2.14.4–6 p.52–58 | Treated as comments. Result differs from Yosys, which honours them (*synopsys_metacomment* E≠ vs Yosys) | full/parallel 11/5/0; `translate_off` 6/6/0 (encloses only comments) | Y(w) | SHOULD → decision D1 |
| L13 | `` `define `` plain, with args, multi-line `\`, nested use, redefinition | §19.3.1 | S | B App.A p.193 "Text substitutions: Supported" | OK= (l:154–158, 205–211) *pp_define_\**, *macro_redefine* | 226/16/23; args 14/0/0 | Y | MUST (2B) |
| L14 | Undefined macro used | §19.3.1 | — | — | generic "Parser found errors" | — | N | REJECT (located, names the macro) |
| L15 | `` `undef `` | §19.3.2 | S | — | OK= | 1/0/0 | Y | MUST |
| L16 | `` `ifdef/`ifndef/`else/`elsif/`endif `` nesting | §19.4 | S | — | Taken nesting OK. P- when a nested `` `ifdef `` is inside a skipped region: the SKIP state has no `` `ifdef `` rule, so the inner `` `endif `` pops the outer one (l:163–179), *pp_nested_ifdef_skipped* | 17/1/29; elsif 3/0/0 | Y | MUST |
| L17 | `` `include `` (relative to includer; `+incdir` path) | §19.5 | S | — | OK relative to the file; no search-path option | 80/0/0 | Y | MUST (2B) |
| L18 | `` `timescale `` | §19.8 | I | C p.12 | skipped to EOL (l:188) | 3/0/15 | Y | IGNORE |
| L19 | `` `default_nettype wire/…/none `` | §19.2 | S | — | `wire` OK. `none` P-: `vNONE` is absent from `ieee_filter` (y:950–1125), giving "keyword index 355 is not a supported keyword" | 1/0/0 | Y | MUST |
| L20 | `` `resetall `` | §19.6 | S? | — | OK (l:185) | 0 | Y | SHOULD |
| L21 | `` `celldefine `endcelldefine `unconnected_drive `nounconnected_drive `pragma `line `` | §19.1, 19.9, 19.10, 19.7 | I | — | skipped to EOL (l:188–194) | 0 | `celldefine`/`line` Y; `pragma`/`unconnected_drive` N | IGNORE |
| L22 | `` `begin_keywords "1364-2005"` / `end_keywords `` | §19.11 | ? | — | P-: the rule is spelt with U+2018 `‘`, not a backtick (l:197) | 0 | N | SHOULD (accept 1364-1995/2001/2005; REJECT others) |

### 4.2 Modules and ports

| # | Construct | 1364-2005 | 1364.1 | Books | Odin II | micro/vtr/koios | Yosys | Verdict |
|---|---|---|---|---|---|---|---|---|
| M1 | `module…endmodule`, non-ANSI port list plus `input/output/inout` declarations, later `wire/reg` redeclaration | §12.1, §12.3.2–3 | S | C p.6–7; B throughout | OK= *mod_nonansi*, *mod_nonansi_output_reg* (y:233–238, 450–455) | most | Y | MUST |
| M2 | ANSI port list with net/var type, range, `signed`, shared range `input [3:0] a, b` | §12.3.4 | S | C p.6 (used throughout) | OK= *mod_ansi_shared_range*. `signed` dropped (pma:584): *mod_ansi_output_reg_signed* E≠ | 101/2/28 | Y | MUST |
| M3 | `output reg`, `output integer` | §12.3.3 | S | C p.48 | OK= | 56/6/26 | Y | MUST |
| M4 | Parameter port list `module m #(parameter A=1, B=2)(…)` | §12.2, §4.10.1 | S | C p.65–66 | OK= (y:240–252); untyped only | 1/0/14 | Y | MUST |
| M5 | Empty body / empty ports `module m(); endmodule`, `module m;` instantiated | §12.1 | S | — | `module m();` with an empty body is P- (y:274–277 needs ≥1 item). Instantiating a port-less module crashes (pma:1476 warning, then SIGSEGV), *mod2_\** | — | Y | SHOULD |
| M6 | Port expressions `.x(a)` and `{a,b}` in a module header | §12.3.2 | ? | — | P- | 0 | N | REJECT |
| M7 | `inout` ports and tri-state (`assign io = oe ? d : 'bz`) | §12.3.3, §4.6 | S | C p.46–47; B §3.14 p.143, §2.21.2 p.93 | E-: "Odin does not handle inouts" (pma:554, 569) | 3/0/0 | Y(w) | SHOULD → D3 |
| M8 | `macromodule` | §12.1 | S | B App.A p.197 | OK (lexed as `module`) | 6/0/0 | N | MUST (alias) |
| M9 | Several top-level modules; top selection | §12.1.1 | S | B App.A p.197 | E-: "Found multiple top level modules" (ae:162). Fails micros `common/adder`, `module_endmodule/multiple_topmodules` | — | Y | MUST (DESIGN §4.0 top selection) |
| M10 | Implicit nets (port connections, `assign` LHS) | §4.5 | S | C p.8; B App.A p.193, 195 | E-: "Missing declaration of this symbol" (ae:1828). **VTR `spree.v` fails for this reason** (golden log). *mod_implicit_net* | ≥1 VTR (`spree`) | Y(w) | MUST (warn, DESIGN §4.1) |
| M11 | Duplicate port, module or task names | §12 | — | — | located errors (pma:461, 468, 1542, 1712) | — | N | REJECT |
| M12 | `input reg`, `input integer` (illegal: an input cannot be a variable) | §12.3.3 | N | — | E-: "Input cannot be defined as a reg" (pma:476; nca:1272). Fails micros `reg_failure`, `integer_failure`; Parmys passes both | 2/0/0 | Y | SHOULD (accept as a net with a located warning, to match Parmys; D5) |

### 4.3 Parameters, `localparam`, `defparam`

| # | Construct | 1364-2005 | 1364.1 | Books | Odin II | micro/vtr/koios | Yosys | Verdict |
|---|---|---|---|---|---|---|---|---|
| P1 | `parameter` in body, lists `A=1, B=2` | §4.10.1 | S | C p.67; B p.10, 103 | OK= | 74/7/29 | Y | MUST |
| P2 | `localparam` | §4.10.2 | S | C p.64 | OK= | 6/1/7 | Y | MUST |
| P3 | Parameter with range or `signed` | §4.10.1 | S | — | range OK=; `signed` ignored (pma:606) | 5/4/2 | Y | MUST |
| P4 | Typed parameters `parameter/localparam integer` (also real/time/realtime) | §4.10.1 | integer S?, real N | C p.192 (`input integer`) | P- (y:246–252, 422–440 take only `signed`/`unsigned`/range) | 0/0/1 | Y | MUST (integer); REJECT (real, realtime, time) |
| P5 | Instance override, ordered `#(8)` and named `#(.W(8))` | §12.2.2 | S | C p.66; B p.103 | OK= (y:606–611, 645–649) | 14/12/29; named 5/12/26 | Y | MUST |
| P6 | `defparam` one level `u.W`, nested `m.l.W`, inside generate | §12.2.1 | ? | C p.67; B App.A p.197 "not supported" | one level OK=. Nested is accepted and **silently not applied** (*par_defparam_nested* E≠; leaf keeps default) | 30/3/23 | Y | MUST |
| P7 | `specparam` (module level, list in `specify`) | §4.10.3 | I | — | module level P- (not a module item, y:279–286); only one `specparam` per statement inside `specify` (y:359–361) | 6/0/0 | Y | IGNORE |
| P8 | Constant expressions in ranges and values: `2**W-1`, `$clog2`, constant function calls, forward references | §12.2.3, §10.4.5 | S | C p.64, p.90, p.192–193 | `**` and `$clog2` OK=. Constant function in `localparam` or a range is E- (asserts ae:2006, ae:2074), *fn2_const_\** | clog2 3/0/2 | Y | MUST |

### 4.4 Nets and variables

| # | Construct | 1364-2005 | 1364.1 | Books | Odin II | micro/vtr/koios | Yosys | Verdict |
|---|---|---|---|---|---|---|---|---|
| N1 | `wire`/`tri`, descending vectors | §4.2.1, §4.3, §4.6.1 | S | C p.4 | OK= | 155/25/30 | Y | MUST |
| N2 | Ascending or negative ranges `[0:3]`, `[1:3]`, `[-1:2]` (vectors and memories) | §4.3.1, §4.9 | S | **B p.31, 35, 58 use `[0:3]`/`[1:3]`**; C p.4 (legal, avoided) | E-: "arrays declared [m:n] where m is less than n" (nca:933, 1029, 1071, 1088, 1179; ae:2248). Negative range: Odin hangs (60 s timeout) | 0/0/0 | Y | MUST |
| N3 | `reg`, shared-range lists `reg [3:0] a, b` | §4.7 | S | C p.49 | OK= *reg_shared_range* | 208/30/30 | Y | MUST |
| N4 | `integer` (32-bit signed) | §4.8 | S | C p.49; B p.23 | parsed; signedness dropped (pma:650): *op2_integer_signed_div* E≠ | 17/0/14 | Y | MUST |
| N5 | `signed`/`unsigned` on nets, regs and ports | §4.2–4.3 | S | C §7.3.3 p.190 | parsed then ignored with a warning (pma:584, 606, 650): E≠, see E8/E23 | 58/0/1 | Y | MUST |
| N6 | `genvar` (module level; inside a generate region) | §12.4.1 | S | — | module level OK=; inside a region P- (region holds one item, y:656–658) | 8/0/11 | Y | MUST |
| N7 | Net declaration assignment `wire w = e;` | §6.1.1 | S | — | OK= | 9/0/3 | Y | MUST |
| N8 | Variable declaration initialiser `reg r = v;` | §6.2.1 | ? (B App.A p.193 "net initialization: not supported") | — | OK=, init value kept in `.latch` | 2/1/4 | Y | MUST → D2 |
| N9 | Memories `reg [w] m [d]` with dynamic read/write | §4.9.3, §5.2.2 | S | **C p.90 Listing 4.6, p.300–306**; B §3.3 p.111 | descending depth OK; ascending depth E- (N2) | 20/1/27 | Y | MUST |
| N10 | Scalar-element arrays `reg a [0:3]` | §4.9.2 | S | — | P- (an unpacked dimension needs a packed range first, y:515–522) | — | Y | MUST |
| N11 | Net arrays `wire [3:0] w [1:0]` | §4.9.1 | S | — | E-: "Assignment to implicit memories is only supported within sequential circuits" (nca:2923) | 0 | Y | SHOULD |
| N12 | Arrays of 2 or more unpacked dimensions | §4.9 | S? | — | 2-D only (y:519; flattened at ae:2247); 3-D P- | 2/0/0 | Y | SHOULD |
| N13 | Word select then bit/part select `m[i][j]`, `m[i][3:0]` | §5.2.2 | S | — | part-select P- (no `[e][e:e]` primary, y:880–889); bit-select E- (assert au:671) | — | Y | SHOULD |
| N14 | `wand/wor/triand/trior` | §4.6.2 | S? | C p.4 (exists, unused) | collapsed to `wire` (y:915–918) → multi-driver net (*net_wand*, *net_wor*: equiv-check refuses "more than one driver") | 0/0/0 | Y | SHOULD → D4 |
| N15 | `uwire`; `tri0/tri1/trireg` | §4.6.3–4.6.5 | N? | — | all collapsed to `wire` | 1/0/0 | N (with range) | `uwire` SHOULD (wire + single-driver check); `tri0/tri1/trireg` REJECT |
| N16 | `supply0/supply1` | §4.6.6 | S | C p.4 | P- (lexed, not in `wire_types`) | 0 | Y | SHOULD |
| N17 | `time`, `real`, `realtime` variables | §4.8 | N | C p.5, 49; B App.A p.193 | P- | 0 | N | REJECT |
| N18 | `event` | §9.7.3 | N | B App.A p.196 | P- | 0 | N | REJECT |
| N19 | `vectored/scalared`, drive/charge strength, net delay `wire #5 w` | §4.3.2, §4.4, §6.1.3 | I | B App.A p.193–195 | P- | 0 | N, N, Y | IGNORE |

### 4.5 Expressions and operators

| # | Construct | 1364-2005 | 1364.1 | Books | Odin II | micro/vtr/koios | Yosys | Verdict |
|---|---|---|---|---|---|---|---|---|
| E1 | Bitwise `~ & \| ^ ~^ ^~` | §5.1.8 | S | C p.42 | OK= | most | Y | MUST |
| E2 | Reduction `& ~& \| ~\| ^ ~^` | §5.1.9 | S | C p.42–43 | `& \| ~& ~\|` OK=. **`^`/`~^` on a 4-bit or wider operand crashes** (assert in `blif_writer.cpp:597/611`) | ~14/4/10 (`~` forms) | Y | MUST |
| E3 | Logical `! && \|\|` | §5.1.7 | S | C p.43 | OK= | — | Y | MUST |
| E4 | `+ - *`, unary `-`, unary `+` | §5.1.3 | S | C p.41; B p.22–24 | OK; **unary `+` SIGSEGV** (*pr2_unary_plus_only*) | — | Y | MUST |
| E5 | `/`, `%` by a power-of-2 constant | §5.1.3 | S? | C p.41 | `/4` OK=; `%4` E-: "Modulo operation not supported" (nca:3697) | `/` 7/0/16; `%` 2/0/1 | Y | MUST |
| E6 | `/`, `%` general | §5.1.3 | ? | C p.41 "usually cannot be synthesized" | E- (au:977; nca:3694, 3697). Fails micros `common/div`, `div_by_const`, `mod`, `operators/twobits_arithmetic_div/_mod` (Parmys passes all) | as above | Y | MUST (exit test) |
| E7 | `**`: constant operands; variable exponent | §5.1.3 | ? | C p.64, p.90 (`2**W`) | constant OK=; variable E- (au:983). Fails micros `common/pow`, `twobits_arithmetic_power` | 9/0/1 | Y | MUST |
| E8 | Relational and equality `< > <= >= == !=` | §5.1.5–6 | S | C p.42 | OK, but operands are cut to the **narrower** operand (nca:3680 `find_smallest_non_numerical`) and compared unsigned: *op_cmp_width_mismatch*, *op2_eq_mixed_width*, *op_signed_compare* E≠ | most | Y | MUST |
| E9 | Case equality `=== !==` | §5.1.6 | ? | C p.42 "cannot be synthesized"; B App.A p.193 "not supported" | E-: "Operation not supported" (nca:3700). Fails micros `operators/binary_equal`, `binary_not_equal` (Parmys passes) | 2/0/0 | Y | MUST (exit test; elaborate as `==` when neither side has x/z, else constant-fold; warn) |
| E10 | Shifts `<< >> <<< >>>`, variable amount | §5.1.10 | S | C p.41 | OK=. `>>>` sign-fills only for a declared-signed left operand (nca:3775) | 62/9/28; `>>>` 41/0/7 | Y | MUST |
| E11 | Conditional `?:`, nested | §5.1.11 | S | C p.44; B p.36 | OK= | — | Y | MUST |
| E12 | Concatenation, incl. as LHS; unsized operand → error | §5.1.12 | S | C p.43; B p.24 | OK=. Unsized operand gives a located error (au:457, 1491), which is correct | — | Y | MUST |
| E13 | Replication, nested, parameter count | §5.1.12 | S | C p.44 | OK= | 11/4/23 | Y | MUST |
| E14 | Constant bit- and part-select | §5.2.1 | S | C p.43; B p.32–33 | OK= | — | Y | MUST |
| E15 | **Non-constant bit-select** on a vector: rvalue → mux, lvalue → decoder | §5.2.1 | S | **B §2.10.2–3 p.34–35** | E-: assert `get_name_of_pins: rnode[1]->type == NUMBERS` (au:671) for `a[s]`, `y[s] = d` (*bs1*, *bs3*) | not counted | Y | MUST |
| E16 | Indexed part-select `+:`/`-:` with variable base, also as lvalue | §5.2.1 | S? | B p.32 (1995: "non-constant part-selects not supported") | constant base OK. Variable base E-: "Part-selects can only contain constant expressions" (ae:1654). **Fails micro `common/shiftx`** | 6/1/10 | Y | MUST |
| E17 | Out-of-range or x/z index read | §5.2.1 | — | — | not probed | — | — | MUST (2D: result x = don't-care) |
| E18 | `$signed()`, `$unsigned()` | §17.8 | S | C p.191 | parsed (l:376–377); `$signed(a)+$signed(b)` and `$unsigned(s)>>1` E-: "Operation not supported" (nca:3700) | 11/0/1 | Y | MUST |
| E19 | `$clog2` in constant context | §17.11.1 | — (2005) | — | OK= | 3/0/2 | Y | MUST |
| E20 | Other system functions in expressions (`$random`, `$time`, `$rtoi`…) | §17 | N | B App.A p.193 "system names ignored" | P- (statement-only, y:896–903) | ~11/1/4 any `$` call | Y(w) | REJECT |
| E21 | Hierarchical references in expressions (`u.w`) | §12.5 | N | B App.A p.197 "not supported" | E- (elaboration hangs) | 0 outside `defparam` | Y(w) | REJECT |
| E22 | Expression bit-length rules (LHS takes part; context- vs self-determined) | §5.4 | S | **C §3.2.8 p.45** (`(a+b)>>1` vs `(0+a+b)>>1`); B App.A p.194 | `(0+a+b)>>1` OK=; **`(a+b)>>1` into 4 bits keeps the carry: *op2_add_shift_ctx* E≠** | everywhere | Y | MUST (2D) |
| E23 | Signed expression rules: sign extension, mixed operands → unsigned | §5.5 | S | **C §7.3 p.188–191**; B §2.4.2 p.23 | E≠ throughout (*op_mixed_sign_extend*, *op2_signed_add_ext*, *op_signed_compare*, *fn2_signed_ret*) | 58/0/1 | Y | MUST (2D) |

### 4.6 Continuous assignment

| # | Construct | 1364-2005 | 1364.1 | Books | Odin II | micro/vtr/koios | Yosys | Verdict |
|---|---|---|---|---|---|---|---|---|
| A1 | `assign` to net, bit, part, concat; list `assign a=…, b=…;` | §6.1.2 | S | C p.7; B §2.1 p.16 | OK= (y:534–541) | 356/27/30 | Y | MUST |
| A2 | Delay `assign #1`, `#(1,2)`, `#D` | §6.1.3 | I | B App.A p.194 | integer literal OK (dropped, y:806–811). `#D` (parameter) P- | 1/0/0 | Y | IGNORE |
| A3 | Drive strength `assign (strong0, strong1)` | §6.1.4 | I | B App.A p.194 | P- | 0 | N | IGNORE |
| A4 | Several drivers on one net (not tri-state, not wired) | §6.1 | N | C p.51 Listing 3.3 / Fig 3.3(c) | emits a multi-driver net silently | — | — | REJECT |

### 4.7 Procedural blocks, assignments, timing

| # | Construct | 1364-2005 | 1364.1 | Books | Odin II | micro/vtr/koios | Yosys | Verdict |
|---|---|---|---|---|---|---|---|---|
| T1 | `always @(posedge/negedge …)` incl. async reset `@(posedge clk or posedge rst)` / `, posedge rst` | §9.9.2, §9.7.2, §9.7.4 | S | C p.87–88; B §2.17 p.68–81 | OK (async: OK~) | edges 235/30/30; async 19/1/0 | Y | MUST |
| T2 | `@*`, `@(*)`, comma lists, `or` lists | §9.7.4–5 | S | C p.50 | OK= | 26/18/20 | Y | MUST |
| T3 | Incomplete sensitivity list on combinational code | §9.7 | — | C p.61; B §5.6 p.182–183 | not probed | — | Y(w) | MUST (build as if complete; warn) |
| T4 | `@ident` without parentheses | §9.7.2 | S | — | P- | 0 | N | SHOULD |
| T5 | `initial`: constant init of regs and memories, `for` loops, `$readmemh/b` | §9.9.1, §17.2.9 | I? (B App.A p.196 "Ignored") | C p.194, p.200–201 | `initial r = 5` parses but the FF init comes out unknown (*pr_initial_reg_init* E≠). For-loop memory init OK~. **`$readmemh` SIGSEGV** | initial 3/0/8; readmem 0/0/4 | Y | MUST (initial values only → D2); other `initial` content IGNORE |
| T6 | `always` with no event control (`always #5 clk = ~clk`) | §9.9.2 | N | C p.194 | builds combinational logic silently | — | Y | REJECT |
| T7 | Blocking `=` | §9.2.1 | S | C p.49; B p.17 | OK= in combinational `always`. **In a clocked `always`, a temporary written then read (`t = a&b; r <= t;`) becomes an extra register: 8 FFs vs 4, *pr_blocking_in_seq* E≠** | ~35/0/0 (`=` inside posedge `always`) | Y | MUST |
| T8 | Nonblocking `<=` | §9.2.2 | S | C p.49, 87; B p.18 | OK | 220/30/30 | Y | MUST |
| T9 | Same target assigned with both `=` and `<=` | §9.2 | N | **B §2.2.4 p.20** | accepted; differs from Yosys (*pr_mixed_same_target*) | — | Y | REJECT |
| T10 | Same variable assigned in two `always` blocks | — | N | **C §3.7.1 p.60–61** | multi-driver net, silently | — | Y (latch) | REJECT |
| T11 | Delay `#n stmt`, intra-assignment `q <= #1 d` | §9.7.1, §9.7.7 | I | B p.20, p.176 | integer only, dropped (y:759–767, 806–811); `#(expr)` P- | 3/0/5 | Y | IGNORE |
| T12 | Event control inside a body (`@(posedge clk) x; @(…) y;`) | §9.7.2 | N | B App.A p.195 | P- | 0 | N | REJECT |
| T13 | `wait` | §9.7.6 | N | C p.195+ | P- | 0 | N | REJECT |
| T14 | Named events `->e` | §9.7.3 | N | — | P- | 0 | N | REJECT |
| T15 | `fork…join` | §9.8.2 | N | — | P- | 0 | N | REJECT |
| T16 | Procedural `assign/deassign`, `force/release` | §9.3 | N | B App.A p.197 | `deassign`/`force` P-. `assign` inside `always` is taken as a continuous assign (y:703) and asserts (nca:641) | deassign 1/0/0 (Parmys fails too) | N | REJECT |
| T17 | Named blocks `begin : name` | §9.8.3 | S | C p.48; B p.23 | OK (y:792) | 19/0/12 | Y | MUST |
| T18 | **Block-local declarations** in named blocks (`reg`, `integer`, `localparam`) | §9.8.1 | S | **C §3.3.1 p.48** (syntax); **B p.23, 63, 90** (`begin: L1 integer …`) | P- (`seq_block` holds statements only, y:790–798) | 0/0/1 | Y | MUST |
| T19 | `disable` | §10.3 | N? | B App.A p.197 "not supported" | P- | 0 | N | REJECT |
| T20 | Null statement `;` | §9 | S | — | OK= | — | Y | MUST |

### 4.8 `if`, `case`, `casez`, `casex`

| # | Construct | 1364-2005 | 1364.1 | Books | Odin II | micro/vtr/koios | Yosys | Verdict |
|---|---|---|---|---|---|---|---|---|
| C1 | `if / else if / else` | §9.4 | S | C p.51–53; B p.40 | OK= | 116/26/30 | Y | MUST |
| C2 | Incomplete `if` or `case` → latch | §9.4–9.5 | S (latch) | C p.61–63; B p.41, 51, 179 | OK~ (latch emitted) | — | Y(w) | MUST (warn, DESIGN §6 step 3) |
| C3 | `case` with `default` | §9.5 | S | C p.54; B p.45 | OK= | 118/23/22 | Y | MUST |
| C4 | **Several expressions per case item** `0, 1: …` | §9.5 | S | **C p.55 Listings 3.6, 3.7** | P- (`case_items` take one expression, y:785–788) | 0/0/0 | Y | MUST |
| C5 | **`casez` / `casex`** with `?`/`z`/`x` patterns | §9.5.1 | S | **C §3.5.3 p.56 Listing 3.8**; **B §2.14.1–2 p.48–49** | P- (tokens lexed l:230–231, no rule; y:740–746 accept `case` only) | 0/0/0 | Y | MUST |
| C6 | `default` not last; empty item statement `4'd1: ;` | §9.5 | S | — | crash (assert nca:4165; SIGSEGV) | — | Y | MUST |
| C7 | `default` without colon | §9.5 | S | — | P- | 0 | Y | SHOULD |
| C8 | Non-constant case items / `case (1'b1)` | §9.5 | S | **B §2.14.6 p.58** | parses; *pr_case_reverse* E≠ | — | Y | MUST |
| C9 | `x`/`z` bits in items of a plain `case` | §9.5 | S | **B §2.21.1 p.93** (item never matches) | *pr_case_x_item* E≠ | 0 | Y | MUST (2D) |
| C10 | `full_case` / `parallel_case` (attribute or metacomment) | 1364.1 cl. 6 | S | C §3.5.4 p.56–57; B §2.14.4–5 p.52–58, §5.7 p.183 | attribute P-; metacomment ignored | 11/5/0 | Y(w) | SHOULD → D1 |

### 4.9 Loops

| # | Construct | 1364-2005 | 1364.1 | Books | Odin II | micro/vtr/koios | Yosys | Verdict |
|---|---|---|---|---|---|---|---|---|
| F1 | `for` in `always`/function, constant bounds, `integer` or `reg` index, step ≠ 1 | §9.6 | S (B App.A p.196: constant index assignments) | C p.194; B §2.16 p.66 | ascending OK=. **A loop that counts down past 0 (`for (i=3; i>=0; i=i-1)`) E-** (assert in `string_of_radix_to_bitstring`, "INVALID BIT INPUT -1") | 17/0/16 | Y | MUST |
| F2 | `while` (statically bounded; in constant functions) | §9.6 | N (B App.A p.196) | C p.195 | E-: "While statements are NOT supported" (pma:968) | 1/0/0 (Parmys fails too) | N (except in constant functions) | SHOULD (in constant functions); REJECT elsewhere |
| F3 | `repeat (const)` | §9.6 | S (B App.A p.196, constant) | C p.195 | P- | 0 | Y | SHOULD |
| F4 | `forever` | §9.6 | N | C p.195; B App.A p.196 | P- | 0 | N | REJECT |

### 4.10 Generate

| # | Construct | 1364-2005 | 1364.1 | Books | Odin II | micro/vtr/koios | Yosys | Verdict |
|---|---|---|---|---|---|---|---|---|
| G1 | **`generate … endgenerate` region holding several items** (`genvar` declared inside) | §12.4 | S | — | P-: `generate <one item> endgenerate` (y:656–658). *gen_region_multi_items*, *gen_genvar_inside_region* | 20/0/12 | Y | MUST |
| G2 | Loop generate (named/unnamed block, nested, no region) with `assign` | §12.4.1 | S | — | OK= (y:660–662, 683–687) | named ~5/0/11 | Y | MUST |
| G3 | **Declarations and processes inside a loop-generate block**: `wire`/`reg`/`localparam`/`always`, instance in a named block | §12.4.1 | S | — | **SIGSEGV** for local `wire`/`reg`/`localparam` and for `always` in the loop body, named or not. A module instance in a named loop block aborts (`std::logic_error`); unnamed works | 0/0/1 decl; instances common | Y | MUST |
| G4 | Loop generate counting down | §12.4.1 | S | — | E- (same assert as F1) | — | Y | MUST |
| G5 | If-generate, else-if chain | §12.4.2 | S | — | OK= | ~14/0/2 | Y | MUST |
| G6 | Case-generate; several expressions per item | §12.4.2 | S | — | one expression OK=; several P- (y:678–681) | — | Y | MUST |
| G7 | Generate-scope names (`genblk<n>`, `g[i].x`) and references into them | §12.4.3, §12.5 | S | — | reference `g[0].t` P- | 0 | Y | MUST naming (net names must match Parmys); SHOULD references |

### 4.11 Functions and tasks

| # | Construct | 1364-2005 | 1364.1 | Books | Odin II | micro/vtr/koios | Yosys | Verdict |
|---|---|---|---|---|---|---|---|---|
| K1 | Function, non-ANSI (`input` declarations in the body), range return | §10.4.1 | S | B §2.19 p.88 | OK= *fn_nonansi* (y:331–336, 395–420). Odin golden failures on function micros are an oracle artifact, see §8 | 22/0/0 | Y | MUST |
| K2 | **ANSI function header** `function [3:0] f(input [3:0] x, …)` | §10.4.1 | S | **C §7.4 p.192–193** (`function integer log2 (input integer n)`) | P-: each input needs `;` (y:416–420). Fails micro `function_endfunction/inside_port` (Parmys passes) | 1/0/0 | Y | MUST |
| K3 | Return type `integer`, `signed`, parameterised range `[W-1:0]` | §10.4.1 | S | C p.192 | `integer` OK=; `signed` E≠ (*fn2_signed_ret*); `[W-1:0]` E- (assert nca:1023) | 2/0/0 | Y | MUST |
| K4 | Function locals (`reg`, `integer`), `for`, `if`, `case` | §10.4 | S | C p.192–193 | OK= *fn2_local_reg*, *fn2_for_loop*, *fn2_if* | — | Y | MUST |
| K5 | `casez`, `localparam`, block declarations inside a function | §10.4 | S | — | P- (`function_item`, y:395–403) | — | Y | SHOULD |
| K6 | Function calling function | §10.4.4 | S | — | E-: "This output … must exist" (nca:~2293). Fails micro `function_call_function` (Parmys passes) | ≥1/0/0 | Y | MUST |
| K7 | **Constant functions** (in `localparam`, in ranges, used before the declaration) | §10.4.5 | S | **C §7.4.2 p.192–193 Listing 7.12** | E- (asserts ae:2006, ae:2074; `input integer` → "Input cannot be defined as a reg", nca:1272) | (books) | Y | MUST |
| K8 | `automatic`; recursive (constant) functions and tasks | §10.4.1, §10.2.1 | ? | — | parsed; `automatic` ignored with a warning (pma:1566, 1644); recursion E- | 2/0/0 (Parmys fails both micros) | Y | SHOULD |
| K9 | Task with ports (ANSI or not), outputs, called from `always` | §10.2 | S | B §2.20 p.89–90 | OK= *task_basic*, *task_nonansi* | 16/0/0 | Y | MUST |
| K10 | Task enable without an argument list (`clr;`) | §10.2.2 | S | — | P- (y:582–587 requires parentheses) | — | Y | SHOULD |
| K11 | Task calling a task; task call inside a `for` loop | §10.2 | S | **B p.90** (task in a `for` loop) | task→task E- (pma:461 "already has input"; fails micros `task_call_task`, `task_call_function`, Parmys passes); task in a `for` loop SIGSEGV | — | Y | MUST |
| K12 | Timing control or nonblocking assignment inside a function | §10.4.4 | N | — | located parse error | — | — | REJECT |

### 4.12 Instantiation

| # | Construct | 1364-2005 | 1364.1 | Books | Odin II | micro/vtr/koios | Yosys | Verdict |
|---|---|---|---|---|---|---|---|---|
| I1 | Module instance: ordered, named, `.p()`, empty positional slot | §12.1.2, §12.3.5–6 | S | C p.9–10; B §2.23 p.98 | OK= | named 74/19/30 | Y | MUST |
| I2 | Mixing ordered and named connections | §12.3 | N | — | located error (ae:264) | — | — | REJECT |
| I3 | Expression on an input port; concatenation on an output port | §12.3.9 | S | — | input OK=; **output concatenation SIGSEGV** (*inst2_concat_out_named*) | — | Y | MUST |
| I4 | Port width mismatch (extend or truncate, warn); signed ports | §12.3.8, §12.3.11 | S | — | width OK=. Signed port leaves an output bit undriven (*inst_signed_port* E≠) | — | Y(w) | MUST |
| I5 | Several instances in one statement | §12.1.2 | S | — | OK= | — | Y | MUST |
| I6 | Arrays of instances `inv u[3:0](…)` | §12.1.2, §7.1.5 | S? | — | P- (no range in `module_instance`, y:606–611) | 0 | Y | SHOULD |
| I7 | Instance of an undefined module (black box) | §12.1.2 | — | B §2.23.1 p.99 | E-: "Can't find module name" (ae:872) | — | Y | MUST (black box, PHASE2 #5 / IR-7b) |

### 4.13 Gate primitives and UDPs

| # | Construct | 1364-2005 | 1364.1 | Books | Odin II | micro/vtr/koios | Yosys | Verdict |
|---|---|---|---|---|---|---|---|---|
| Q1 | `and nand or nor xor xnor` (n inputs), `buf`, `not`, named or not | §7.2–7.3 | S | C p.10–11 Listing 1.5; B §2.22 p.97 | OK= (y:543–576) | 21/0/1 | Y | MUST |
| Q2 | `buf`/`not` with several outputs | §7.3 | S | — | P- (exactly 2 terminals, y:564–567) | 0 | Y | SHOULD |
| Q3 | `bufif0/1`, `notif0/1` | §7.4 | S | B App.A p.194 "Supported" | P- (lexed only) | 0 | Y | SHOULD → D3 |
| Q4 | Gate delays and strengths | §7.1 | I | B App.A p.194–195 | P- | 0 | delay Y / strength N | IGNORE |
| Q5 | Gate instance arrays `and g[3:0](…)` | §7.1.5 | S | — | P- | 0 | N | SHOULD |
| Q6 | MOS, CMOS, pass switches; `pullup`/`pulldown` | §7.5–7.8 | N | B App.A p.194–195 "not supported" | P- | 0 | N | REJECT |
| Q7 | UDPs (combinational and sequential) | §8 | N | C p.11–12 (shown, not used); B App.A p.195 "not supported" | P- (`primitive` lexed, no rule) | 0 | N | REJECT |

### 4.14 `specify`, `config`, system tasks

| # | Construct | 1364-2005 | 1364.1 | Books | Odin II | micro/vtr/koios | Yosys | Verdict |
|---|---|---|---|---|---|---|---|---|
| S1 | `specify`: simple/full paths, edge-sensitive paths, conditional paths, timing checks, `specparam` lists | §14, §15 | I | B App.A p.197 | only `(a => b) = d;` and single `specparam` (y:350–362). Others P-: fails micros `specify_conditional`, `specify_edge_sensitive{,_vector}`, `specparam_inside/outside` (Parmys passes) | 9/0/0; specparam 6 | Y (most) | IGNORE (skip balanced to `endspecify`, warn) |
| S2 | `config…endconfig`, `library`, `liblist`, `design`, `cell`, `use`, `instance` | §13 | N | — | P- | 0 | N | REJECT |
| S3 | System tasks as statements (`$display`, `$write`, `$monitor`, `$finish`, `$stop`) | §17.1, §17.4 | I | B p.3; C p.198–201 | `$display("…")` OK. `$display` with arguments E- (au:1378 `c_display`). `$finish`/`$stop` inside `always` SIGSEGV | ~11/1/4 | Y(w) (`$stop` outside `initial`: N) | IGNORE |
| S4 | `$readmemh/$readmemb` in `initial` | §17.2.9 | ? | C p.200–201 | SIGSEGV | 0/0/4 | Y | MUST (Koios) |

**Counts.** 159 rows: **100 MUST**, 25 SHOULD, 10 IGNORE, 24 REJECT. A row with a split verdict
is counted once, under the verdict for synthesizable use: L9 counts as REJECT, P4 as MUST, F2 and
N15 as SHOULD. Every row needs the lexer and parser to recognise the construct in 2C. REJECT rows
then need a located error naming the construct.

## 5. Gap list (Odin II does not parse or elaborate correctly; in the subset or used)

Ranked by exit-test exposure (the micros and VTR, against the Parmys bar of PHASE2 #4), then by
book centrality. "Evidence" is one line. Rows 1–10 are the top 10.

| # | Gap (Odin II) | Rows | Evidence |
|---|---|---|---|
| 1 | Signedness dropped everywhere: signed nets, ports, parameters, `integer`, function returns | N4, N5, E23, K3 | pma:584/606/650 "Odin does not handle signed …"; *op_signed_compare*, *op_mixed_sign_extend*, *op2_signed_add_ext*, *op2_integer_signed_div* E≠; `signed` in 58 micro files; C §7.3 p.188–191 |
| 2 | Width rules: comparisons cut to the narrower operand; context width of `(a+b)>>1` | E8, E22 | nca:3680; *op_cmp_width_mismatch*, *op2_eq_mixed_width*, *op2_add_shift_ctx* E≠; exactly C §3.2.8 p.45's example |
| 3 | Blocking temporary in a clocked `always` turned into an extra register | T7 | *pr_blocking_in_seq* gives 8 FFs where Yosys gives 4 (E≠); C §7.1.4 attempt 0 p.180–182; B §2.18 p.84–86; ~35 micro files use `=` in posedge blocks |
| 4 | Non-constant bit-select and indexed part-select on vectors | E15, E16 | assert au:671 for `a[s]`; ae:1654 "Part-selects can only contain constant expressions" fails micro `common/shiftx`; B §2.10.2–3 p.34–35 |
| 5 | Division, modulo and power beyond constants or powers of two | E5–E7 | au:977/983, nca:3694/3697; 7 micros fail (`common/div`, `div_by_const`, `mod`, `pow`, `twobits_arithmetic_{div,mod,power}`); Parmys passes all |
| 6 | `casez`/`casex` and `?` digits absent | C5, L7 | no grammar rule (y:740–746; tokens l:230–231); C §3.5.3 p.56; B §2.14.1–2 p.48–49 |
| 7 | Several expressions per case item absent (procedural and generate) | C4, G6 | `case_items` one expression (y:785–788); C Listings 3.6/3.7 p.55 |
| 8 | Generate: one item per region; local declarations, `always` and named-block instances crash | G1, G3, G4, N6 | y:656–658; SIGSEGV / `std::logic_error` (*gw_\**, *ga_\**, *gi_named_lbl*); `generate` in 20 micro + 12 Koios files |
| 9 | ANSI function headers and constant functions | K2, K7, P8 | y:416–420; asserts ae:2006/2074; micro `inside_port` fails (Parmys ok); C §7.4.2 p.192–193 |
| 10 | Ascending or negative ranges and memories `[0:N-1]` | N2, N9 | nca:933…1179 / ae:2248 "arrays declared [m:n] where m < n"; B uses `[0:3]` throughout (p.31, 35, 58) |
| 11 | Implicit nets rejected | M10 | ae:1828 "Missing declaration"; **VTR `spree.v` is the only VTR benchmark Odin fails, for this reason** (golden log `spree.odin.log`) |
| 12 | Block-local declarations in named blocks | T18 | y:790–798; C §3.3.1 p.48; B p.23, 63, 90 |
| 13 | Attributes `(* *)` not lexed | L11, C10 | no rule in `verilog_flex.l`; required by DESIGN §4.1 |
| 14 | `===`/`!==` not elaborated | E9 | nca:3700; micros `operators/binary_equal`, `binary_not_equal` fail (Parmys ok) |
| 15 | Task→task and function→function calls; task in a `for` loop | K6, K11 | pma:461 / nca:~2293; SIGSEGV; micros `task_call_task`, `task_call_function`, `function_call_function` (Parmys ok); B p.90 |
| 16 | `specify` beyond the simple path | S1 | y:350–362; 5 micros fail (Parmys ok) |
| 17 | Several top-level modules | M9 | ae:162; micros `common/adder`, `module_endmodule/multiple_topmodules` (Parmys ok) |
| 18 | Crashes in common code: unary `+`, reduction `^`/`~^` over ≥4 bits, `default` not last, empty case-item statement, output-port concatenation, loops counting down past 0, `$finish`/`$stop` in `always` | E2, E4, C6, I3, F1, S3 | SIGSEGV or asserts (`blif_writer.cpp:597/611`, nca:4165, `string_of_radix_to_bitstring`) |
| 19 | Non-constant case items; `x` in plain-case items | C8, C9 | *pr_case_reverse*, *pr_case_x_item* E≠; B p.58, p.93 |
| 20 | Nested `defparam` not applied | P6 | *par_defparam_nested* E≠ (silent) |
| 21 | `` `default_nettype none `` rejected | L19 | `vNONE` absent from `ieee_filter` (y:950–1125) |
| 22 | Nested `` `ifdef `` inside a skipped region | L16 | no `` `ifdef `` rule in SKIP state (l:163–179) |
| 23 | `initial`-block init values lost; `$readmemh` crashes | T5, S4 | *pr_initial_reg_init* FF init 3 (unknown); SIGSEGV; Koios uses `$readmem*` in 4 files |
| 24 | Typed parameters (`parameter integer`) | P4 | y:246–252, 422–440; Koios 1 file |
| 25 | `$signed`/`$unsigned` in arithmetic | E18 | nca:3700 "Operation not supported" for `$signed(a)+$signed(b)` |
| 26 | `inout` / tri-state | M7, Q3 | pma:554; C p.46–47 (bi-directional pin); B p.93, 143 |
| 27 | `wand`/`wor` resolved as multi-driver `wire` | N14 | y:915–918; *net_wand*, *net_wor* |
| 28 | Escaped identifiers silently mangled | L3 | catch-all rule drops `\` |
| 29 | Black-box (undefined) module instances rejected | I7 | ae:872 |
| 30 | `repeat`, `while` (constant functions), `default` without colon, `supply0/1`, instance and gate arrays, multi-output `buf`/`not`, `bufif`/`notif` | F2, F3, C7, N16, I6, Q2, Q3, Q5 | P- / pma:968 |
| 31 | Signed port connection leaves an undriven bit; string-parameter bit-select undriven | I4, L10 | *inst_signed_port*, *lex_string_param* |
| 32 | `` `begin_keywords `` misspelt with U+2018 | L22 | l:197 |
| 33 | Edge on a derived net (`wire pos_clk = CLK == CLK_POLARITY; always @(posedge pos_clk)`) needs the clock expression folded to a constant | T1 | `blif_writer.cpp:664` assert; 7 `common/*dff*` micros fail (Parmys ok) |
| 34 | `input reg` / `input integer` accepted by Parmys | M12 | pma:476; micros `reg_failure`, `integer_failure` |

## 6. Notes for 2A (AST node kinds this implies)

Every node carries `{file, line, col, end_line, end_col}` and an attribute list (DESIGN §4.1).
The list below adds what the table requires beyond an Odin II-shaped AST.

- **Compilation unit and directives.** Directives that change parsing state are recorded as
  unit-level nodes so a module knows its effective setting: `` `default_nettype `` (wire,
  tri, …, none), `` `timescale `` (kept, ignored), `` `celldefine ``, `` `resetall ``,
  `` `begin_keywords ``. Preprocessor macro expansions keep their origin location (2B).
- **Module**: name; `is_macromodule`; parameter-port list; ports in ANSI or non-ANSI form;
  items. A **Port** has direction, net kind or `reg`/`integer`, `signed`, packed range and
  name. Non-ANSI port expressions (`.x(a)`, `{a,b}`) are a node kind used only for REJECT.
- **Declarations**:
  - `NetDecl` has a kind enum: wire, tri, wand, wor, triand, trior, tri0, tri1, trireg,
    uwire, supply0, supply1. It also has `vectored/scalared`, drive/charge strength, delay,
    `signed`, packed range, and declarators of the form *name, unpacked dims[], optional init*.
  - `VarDecl` has a kind of reg, integer, time, real or realtime (the last three kept for a
    located REJECT).
  - `ParamDecl` has a kind (parameter, localparam, specparam), a type (implicit, signed, range,
    integer, real, realtime, time) and a value.
  - `DefParam` holds a hierarchical path with optional generate-scope indices, and a value.
  - `GenvarDecl`; `EventDecl` (REJECT).
- **Module items**: `ContinuousAssign` (strength, delay, list of lhs/rhs pairs); `Always`;
  `Initial`; `Instance` (module name, ordered or named parameter overrides, instance list each
  with name, optional range for arrays, and connections that are ordered, named or empty);
  `GateInstance` (kind, strength, delay, optional name, optional range, terminals); `UdpDecl`
  (REJECT); `GenerateRegion`; `GenFor`; `GenIf`; `GenCase` (items carry expression lists);
  `GenBlock` (optional name, implicit `genblk<n>` name assigned at elaboration, items);
  `FunctionDecl` and `TaskDecl` (automatic flag; return type range, signed, integer…; ANSI or
  non-ANSI ports; local declarations; body); `SpecifyBlock` (opaque token range, for IGNORE);
  `ConfigDecl` (REJECT).
- **Statements**:
  - `SeqBlock` and `ParBlock` (`fork`, REJECT), each with an optional name and **local
    declarations**.
  - `BlockingAssign` and `NonblockingAssign`, with an optional intra-assignment timing control.
  - `ProcContAssign` (assign, deassign, force, release; REJECT).
  - `If`. `Case` with kind case, casez or casex; items hold **expression lists** plus a
    default.
  - `For`, `While`, `Repeat`, `Forever` (REJECT).
  - `TimingControlStmt` (`#d stmt`, `@(…) stmt`), `Wait` (REJECT), `Disable`, `EventTrigger`.
  - `TaskEnable` (user or system, with an optional argument list), `NullStmt`.
- **Expressions**:
  - `Number`: width or unsized flag, base, signed flag, 4-state value with the `?` digit
    remembered for case patterns.
  - `RealLiteral`, `String`, `Identifier`.
  - `HierName`: components, each with optional constant index for generate scopes.
  - `Select`: base, then a chain of bit / `[msb:lsb]` / `[b+:w]` / `[b-:w]` selects (one node
    covers vectors and every array dimension).
  - `Concat`, `Replicate`, `Unary` (all 11 operators incl. unary `+`), `Binary`, `Ternary`.
  - `Call` (user function, or system function with a name), `MinTypMax` (delays only).
- **Event expressions**: `EventExpr` with an edge (pos, neg or none) and an expression;
  `EventOr` for `or` and `,`; `ImplicitEvent` for `@*`.
- **Symbol table** (DESIGN §4.5): scopes are module, generate block (incl. unnamed), named
  block, function and task. A symbol records net kind or var kind, `signed`, packed and
  unpacked dims with direction (ascending or descending, so `[0:3]` keeps its order),
  implicit-declaration flag, and the list of assigning nodes, so T9, T10 and A4 can be
  reported with both locations.

## 7. Notes for 2D (semantic traps the books call out)

1. **Signedness (§5.5; C §7.3 p.188–191; B §2.4.2 p.23).**
   - An expression is signed only if every operand is signed.
   - Unsized decimal literals are signed 32-bit; based literals are unsigned unless they carry
     `'s`.
   - `integer` is signed 32-bit (C p.49).
   - Sign extension happens to the context width before the operation (C p.190: `a + b + c` with
     one unsigned `c` zero-extends every operand).
   - `$signed`/`$unsigned` change type, not bits.
   - `>>>` sign-fills only for a signed left operand.
   - Relational operators with mixed signedness compare unsigned.
   - Signed `/` truncates toward zero and `%` takes the dividend's sign.
   - Port connections act as continuous assignments with extension per §12.3.11.
   - Odin II gets all of these wrong (gap 1).
2. **Widths (§5.4; C §3.2.8 p.45; B App.A p.194).**
   - The LHS takes part in context-determined widths: `sum = (a+b)>>1` on 8-bit operands drops
     the carry, while `(0+a+b)>>1` keeps it (C p.45).
   - Operands of comparisons are sized to the larger operand, and the result is 1 bit.
   - Shift amounts, concatenation operands, replication counts and `$clog2` arguments are
     self-determined.
   - Truncation on assignment is silent.
   - Width mismatches at ports are extended or truncated with a warning (§12.3.8).
3. **Blocking vs nonblocking (C §7.1 p.175–182; B §2.18 p.84–86, §2.2.4 p.20).**
   - In a clocked block, a variable written with `=` before it is read is a wire. Read before
     written, it is a register (C attempts 0 vs 3 and 5, p.180–182).
   - `<=` reads take the entry value.
   - Assigning one target with both `=` and `<=` is an error (B p.20).
   - Assigning one variable from two `always` blocks is an error (C p.60–61).
4. **Latches and full/parallel case (C §3.7.1 p.61–63; B §2.13.1, §2.14.3–6, §2.15, §5.4, §5.7).**
   - A missing `else`, an output not assigned on some path, or a non-full `case` infers a latch,
     with a warning.
   - Default assignments at the top of the block avoid the latch.
   - `case` is a priority structure (B p.55). A parallel case maps to a mux (C §3.6 p.57–60).
   - `full_case`/`parallel_case` change synthesis semantics, not simulation semantics (B p.183):
     decision D1.
   - A locally declared variable used before it is assigned infers a latch or FF (B p.60–63).
5. **X and Z (B §2.21 p.93–95; C §3.2.9 p.46–47).**
   - `x` on the right-hand side is a don't-care.
   - An `x`/`z` item in a plain `case` never matches.
   - In `casez`, `z` and `?` are wildcards; in `casex`, `x` too. The wildcards also apply to
     x/z bits in the case **expression**, which is a known simulation/synthesis mismatch.
   - `z` is meaningful only as a conditional drive: tri-state, decision D3.
   - An out-of-range index read yields `x` (§5.2.1), which is a don't-care.
6. **Loops (B §2.16 p.66; B App.A p.196).**
   - Unroll `for` with statically known bounds; index updates must be constant steps.
   - `integer` indices go negative (Odin II fails on this, F1).
   - Cap unrolling with a located error.
   - `repeat` takes a constant count.
7. **Case items.** Items may be non-constant (`case (1'b1)` priority encoder, B p.58). Item lists
   are OR-ed (C p.55).
8. **Dynamic indexing (B §2.10.2–3 p.34–35; C p.90).** A non-constant index on the right is a
   mux; on the left it is a decoder plus write enables. On memories it becomes `$memrd`/`$memwr`
   (Phase 4 memory inference).
9. **Parameters (§12.2).**
   - `localparam` cannot be overridden.
   - An untyped parameter takes the width of its final value; a ranged parameter takes its
     range.
   - Precedence between `defparam` and `#()` must follow §12.2. The recollection is that
     `defparam` wins; verify against the text.
   - Nested and generate-scoped `defparam` paths must resolve (Odin II drops them, P6).
   - Constant functions are evaluated at elaboration (C p.192–193) under the §10.4.5 rules,
     including use before declaration.
10. **Generate (§12.4).** `genvar` scope; `genblk<n>` naming of unnamed blocks, so that net
    names match Parmys goldens and `netlist-compare` stays exact; nested scopes; local
    declarations.
11. **Implicit nets (§4.5).** A 1-bit `wire` unless `` `default_nettype none ``, which makes it
    an error. Warn (DESIGN §4.1). VTR `spree.v` needs this.
12. **Initial values (§6.2.1; B App.A p.193/196 vs Yosys practice).** Constant `initial`
    assignments and declaration initialisers become FF/RAM init values; `$readmemh/b` loads
    memory init. Everything else in `initial` is ignored with a warning (decision D2).
13. **Delays, strengths, specify, `$display`.** Parse, drop, warn (B p.20, p.176; App.A
    p.194–197).

## 8. Odin II golden failures that are harness artifacts (not language gaps)

Of the 133 failed Odin II micro runs, many are not Verilog limitations, so 2C should not chase
them:

- **`output_ast_graphs=1` in the oracle `odin_config.xml`.** It makes Odin II assert in
  `graphVizOutputAst_traverse_node` (pma:1884) on every design with a function, e.g.
  `simple_function`, `function_automatic`, `bm_function_1/2`, `*_expression_in_function_port`.
  The same function constructs elaborate equivalently without the flag (*fn_nonansi*,
  *fn2_\**).
- **`` `include `` resolution.** The VTR flow copies only the top `.v` into its temp dir, so
  `` `include "x.vh" `` cannot be found (`keywords/assign/*`, `generate_case_*`,
  `nested_ifdef_*`, `include-syntax`, …). Odin II resolves includes relative to the including
  file when run in place.
- Not an artifact, though it looks like one: the **BLIF clock writer assertion**
  (`blif_writer.cpp:664 define_clock`) on `common/dff`, `adff`, `adffe`, `dffe`, `sdff`,
  `sdffe`, `sdffce`. These micros clock on a derived net, `wire pos_clk = CLK == CLK_POLARITY;
  always @(posedge pos_clk)`. Odin II does not fold the comparison before using the net as a
  clock. Parmys passes all 7, so this is a real 2D/2F gap (gap 33).
- 23 `koios_dummy/*` wrappers include out-of-tree files (excluded from `tests/micro`).

## 9. Decisions for Peter

Numbered; the recommendation comes first.

1. **D1 — `full_case`/`parallel_case` (attributes and `// synopsys` metacomments).**
   Recommendation: honour them like Yosys (warn on each use), because the exit bar compares
   against Parmys (Yosys), which honours them. 11 micro files and 5 VTR files carry them. The
   alternative, following simulation semantics, would create mismatches against Parmys.
2. **D2 — initial values.** Recommendation: constant `initial` assignments, declaration
   initialisers and `$readmemh/b` set FF/RAM init values, as Yosys does; any other `initial`
   content is ignored with a warning. Bhasker App.A and 1364.1 say "ignored". Koios needs
   `$readmem*`.
3. **D3 — tri-state (`inout`, `'bz`, `bufif*`).** Recommendation: Phase 2 parses all of it and
   elaborates `z` drives to `$tribuf` only on top-level `inout`/output ports. A tri-state on
   an internal net is a located error until the IR has a tri-state cell. Only 3 micro files use
   `inout`, and Parmys rejects them.
4. **D4 — `wand/wor/triand/trior/uwire`.** Recommendation: elaborate wired nets as AND/OR of
   their drivers, and `uwire` as `wire` with a single-driver check (SHOULD). There is no corpus
   use, but this costs little once nets track their driver lists.
5. **D5 — Verilog that is illegal but that Parmys accepts** (`input reg`/`input integer`, M12).
   Recommendation: accept with a located warning so the micros match Parmys under PHASE2 #4. The
   alternative is to reject them and list the 2 micros as exceptions.
