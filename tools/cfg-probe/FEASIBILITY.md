# CFG similarity (Gate-Breaker Algorithm 3) over the Odin III AST: feasibility

Probe, 2026-10-23. Question: can the Gate-Breaker paper's CFG-similarity metric (Algorithm 3:
shortest-path feature vectors per CFG node, node kernel, normalised to [0, 1]) be computed in
Python from Odin III's outputs on the VerilogEval corpus, which the paper's own pipeline builds
from RTL with pyverilog? Verdict: **yes, conditionally** — no core change, no AST JSON dump, one
small Python helper on top of the existing binding, plus the metric itself. Evidence: the
prototype in this directory.

## 1. Representation: the elaborated AST, walked live through the C ABI

Odin III keeps the AST it read (ABI v4) and the released Python binding already walks it:

- `plugins/python/odin3.py`: `Design.ast()` (the latest read run's store, elaborated preferred),
  `Ast.run/form/roots()/node(id)/__len__`, and `AstNode.kind`, `.sub`, `.name`, `.loc`,
  `.children()` (`"file:line:col"`, absent optional slots skipped). The binding generates its cffi
  cdef from `include/odin3/odin3.h`, so it never drifts from the header.
- `include/odin3/odin3.h`: `odin3_design_get_ast_count/_at`, `odin3_ast_get_kind/_sub/_flags/
  _loc/_end/_name/_nchild/_child`, `odin3_ast_get_kind_name/_sub_name`, the number/real/text and
  attribute getters, `odin3_design_set_keep_ast`. `tests/tools/test_python_ast.py` (CTest
  `python_ast`) already proves the walk over a store built through the builders.
- `src/ast/kinds.h` is the slot table: per kind the member class of each child slot and the tail
  (e.g. `IF = SL3(E, S, S_OPT)`, `CASE = SL1(E) + CASE_ITEM tail`, `CASE_ITEM = SL1(BODY) +
  E tail`, `ALWAYS = SL1(S)`, `CONT_ASSIGN = tail NET_ASSIGN`). The CFG builder follows it.
- `src/ast/dump.c` (internal s-expression dump for golden tests) and the `write_verilog_ast` pass
  print the AST as text; `write_json` writes the IR netlist (cells and nets), where control flow
  is already gone. None of them is needed: the Python binding reads the live store through cffi.

Store choice: **elaborated**, for both languages. For Verilog the store is kept with
`odin3_design_set_keep_ast(ODIN3_KEEP_AST_ELABORATED)` (the CLI's `--keep-ast`), and the slang
adapter's SystemVerilog read always hands its elaborated store to the design. Both are
post-parameter and post-generate, so a generated `.v` candidate and a `.sv` reference are
compared in the same form. The parsed store (`--parse-only`, or `--keep-ast=all`) also works for
Verilog and is what a source-faithful metric would use; it does not exist for SystemVerilog.

SystemVerilog from Python needs one thing the binding does not do yet: nothing registers the
slang reader in a plain `libodin3` process (the CLI registers `odin3_read_slang` at start-up,
`src/cli/main.c`). The prototype's `register_sv_adapter()` loads `libodin3_slang.so` with
`ctypes`, takes the address of `odin3_read_slang`, casts it to `odin3_sv_read_fn` and calls
`odin3_sv_reader_register` (ABI v6) — 20 lines, no C or C++ change. Registration is process-wide
and final. **Recommended follow-up:** add this as `Odin3.register_sv_adapter()` in
`plugins/python/odin3.py` (plus a unit test) so a pip-package user gets `.sv` reads too.

## 2. The CFG and the metric (what the prototype implements)

`tools/cfg-probe/odin3_cfg.py`, ~540 lines including its docstring, standard library only (cffi via the binding). One CFG
per module:

| node | meaning |
| --- | --- |
| `Entry`, `Exit` | one each |
| `Block` | one per `always`/`initial`, edge from `Entry` |
| `Assign` | one per continuous assign, edge from `Entry` |
| `If`, `Then`, `Else`, `Join` | per `if`; `Join` merges the branches |
| `Case`, `CaseWhen`, `CaseDefault`, `CaseJoin` | per `case` and per case item |
| `Loop`, `LoopBody`, `LoopExit` | per `for`/`while`/`repeat`/`forever`; the back edge and the exit edge leave the body's end |
| the kind name (`BINARY`, `IDENT`, `NUMBER`, ...) | one per expression-tree node under an assign rhs, a condition, a case selector or a case item expression |

Statement lists are flattened through `SEQ_BLOCK`/`PAR_BLOCK`; a timing control (`always @(...)`)
is unwrapped to its statement; declarations, ports, instances and gates contribute nothing. Every
node with no outgoing edge (except `Entry`) gets an edge to `Exit`, as the pyverilog
implementation in `work/gatebreaker-exp/scripts/metrics.py` did, so the two builders produce the
same node and edge counts for the same statement structure.

Metric: each node's feature vector is the histogram of its shortest-path distances to all CFG
nodes (raw counts); `KSP(A,B) = sum over node pairs exp(-||phi(a)-phi(b)||^2 / (2*sigma^2))` with
sigma = 0.5; `SimCFG = KSP(G,R) / sqrt(KSP(G,G)*KSP(R,R))` in [0, 1]. Labels do not enter the
kernel, so renamed identifiers give exactly 1.0.

One deliberate difference from the earlier pyverilog code: that code padded each graph's feature
vectors to its own longest histogram and then `zip`ed them, silently truncating the longer vector
in the kernel. The prototype pads both graphs to one common width (the paper's common feature
space). Numbers from this script and the earlier experiment are therefore not directly
comparable; the queued run should use the common-width version on both sides.

## 3. Proof-of-concept results

`tools/cfg-probe/test_odin3_cfg.py` (CTest `python_cfg_probe`, label `tools`) reads three
designs, then compares each with itself, with a renamed-identifier variant (whole-word renames
only) and with a hand-restructured variant:

| design | source | same | renamed | restructured |
| --- | --- | --- | --- | --- |
| `bm_DL_74381_ALU.v` | one `always` + 8-item `case` → if/else chain | 1.000000 | 1.000000 | 0.939705 |
| `bm_if_reset.v` | `if`/`else` + an instantiated submodule; top `if`/`else` → `case` | 1.000000 | 1.000000 | 0.966916 |
| `Prob055_conditional_ref.sv` | `always_comb` + three `if`s → nested if/else | 1.000000 | 1.000000 | 0.953421 |
| unrelated pair | ALU top vs `bm_if_reset` top | — | — | 0.951689 |

The direction is right in every case (identity and renaming exactly 1.0, restructuring lower),
but the dynamic range on these small designs is narrow: restructured variants land at 0.94-0.97,
barely below an unrelated pair. The cause is visible in the feature vectors: many expression-leaf
nodes (an `IDENT` whose only reachable node is itself, a `NUMBER`) have identical histograms in
any two graphs, so they dominate the cross and self sums. This is a property of the paper's
metric and matches the earlier experiment's observation that single-assignment designs saturate
at 1.000; the metric is one of several paper features, not a standalone discriminator. Larger
designs (the earlier Q2 values ranged 0.314-1.000) spread out more.

Corpus run (scratch, not committed): all 156 VerilogEval refs through the slang adapter, CFG of
the top module of each, 9.3 s total (~60 ms per design, slang elaboration included), 4408 CFG
nodes, 6800 edges, no empty CFG, no missing module location. 152 read; 4 are refused by the SV
front end before the CFG exists:

- `Prob082_lfsr32_ref.sv`, `Prob086_lfsr5_ref.sv`: `expression kind 26 is not supported`
- `Prob151_review2015_fsm_ref.sv`, `Prob156_review2015_fancytimer_ref.sv`: `an explicit 4-bit cast is not supported`

These are front-end coverage gaps (the refusal list the `veval-coverage` manifest already
tracks), not CFG problems.

## 4. What exists and what the queued Gate-Breaker lane must build

Exists, reused as is:

- Python AST traversal: `plugins/python/odin3.py` (`Design.ast`, `Ast`, `AstNode`), ABI v4, tested
  by `tests/tools/test_python_ast.py`.
- The SystemVerilog front end (`adapters/slang`, registration ABI v6) and the VerilogEval refs
  (`tests/cli/veval/refs/`).
- The CFG builder and Algorithm 3 metric: `tools/cfg-probe/odin3_cfg.py` (this probe).
- The pyverilog Algorithm 2 AST-similarity reference implementation, to port:
  `work/gatebreaker-exp/scripts/metrics.py`.

To build (no C/C++ change, no new Odin release):

1. `Odin3.register_sv_adapter()` in `plugins/python/odin3.py`: ~25 lines + a unit test (lifted
   from `register_sv_adapter()` here).
2. Algorithm 2 AST similarity over the same store: depth-3 unique subtree sets, identifier
   normalisation by kind (`PORT_DECL` sub input/output/inout, `VAR_DECL`/`NET_DECL` declarators,
   `NUMBER`/`REAL`/`STRING`, `IDENT`, `INSTANTIATION`), maximum Jaccard per reference subtree,
   mean — a direct port of the pyverilog code, ~120-200 lines + tests.
3. Experiment driver: for each design read candidate `.v` and reference `.sv`, pick the top
   module (the only module no module instantiates, or `--top`), compute SimAST and SimCFG, write
   per-design rows. ~50 lines; the runner in `work/gatebreaker-exp` already produces the RTL.

Estimated follow-up size: half a day to one day for one lane, dominated by the Algorithm 2 port
and its tests; the CFG half is done.

## 5. Open risks and limits

- **Four refs unreadable** in each language path: 4/156 SystemVerilog refs above. The experiment
  must exclude them (or use the parsed Verilog path for the equivalent `.v`), which the coverage
  manifest already anticipates.
- **Top module only.** The CFG (and the pyverilog metric) compares one module; submodule
  structure is invisible. Multi-module candidates need a per-module average or a union graph if
  the experiment wants it — a metric decision, not a tooling one.
- **Elaborated vs parsed shape.** Constant folding and range resolution in the elaborated store
  change expression node counts relative to a source parse (`[`BITS-1:0`]` ranges become
  constants). Comparing an Odin-elaborated `.v` with a slang-elaborated `.sv` is consistent;
  mixing an elaborated store with a parsed one is not.
- **Recursion.** `_expr_nodes` recurses over the expression tree; a pathological expression
  nested >1000 deep would hit Python's recursion limit. The corpus's largest design is far below
  that; convert to an explicit stack if a big benchmark needs it.
- **Location coverage.** Statement and module `loc`s are present on both paths (`file:line:col`);
  the SystemVerilog `UNIT` root reports `<unknown>:0:0`. The metric never uses `loc`, so this is
  no blocker; it only matters if a later tool reports CFG nodes by source position.
- **Process-wide registration.** `odin3_sv_reader_register` is final per process, so one
  registration for the whole experiment run (the prototype caches it).
- **Discrimination on small designs** (section 3): report the per-design distribution and use
  the metric as one paper feature, not a pass/fail threshold.

## 6. Prototype files and how to run

- `tools/cfg-probe/odin3_cfg.py` — AST → CFG → SimCFG; CLI:
  `python3 tools/cfg-probe/odin3_cfg.py REF GEN [--top NAME] [--sigma S] [--edges] [--json]`.
  It finds `libodin3` through the binding (`$ODIN3_LIB`, else `build/debug`, else
  `build/release`), re-execs itself once with `LD_PRELOAD` for a sanitizer build, and loads
  `libodin3_slang.so` beside the library (`$ODIN3_SLANG_LIB` overrides).
- `tools/cfg-probe/test_odin3_cfg.py` — the evidence table above; standalone or CTest
  (`python_cfg_probe`); skips without cffi.
- Gates run for the commit: debug build (`--preset debug`), `python_cfg_probe` (passes; 1.4 s),
  `ruff check` and `mypy --strict` on both files (clean).
