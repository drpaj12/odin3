# 1C — BLIF reader and writer; the round-trip exit test

Status: agent-approved under PHASE1 #12 (Peter: continue through 1C without stopping). Phase 1,
sub-project 1C. Semantics of the IR: `docs/IR.md`.

## Goal

Read every golden BLIF into the IR and write it back so that (PHASE1 #2):

1. `tools/netlist-compare` reports the output identical to the input, and
2. every name is kept exactly (the set of net names, cell names and model/port names is equal),
   and model, port and cell order is preserved.

Exit test: all 1846 `status=ok` golden BLIFs in `~/odin3-ws/golden` (both arches, both tools)
pass 1 and 2; CI runs the same check on a committed fixture subset. Byte-identity is a stretch
goal, not a gate.

## Dialect (what the goldens use, plus Yosys's extensions)

`.model`, `.inputs`, `.outputs`, `.clock` (kept as an attribute), `.names` + cover rows,
`.latch in out [type ctrl] [init]`, `.subckt model formal=actual …`, `.blackbox`, `.end`,
`#` comments, `\` line continuation; Yosys extensions `.cname name` (names the previous cell),
`.attr key value` and `.param key value` (on the previous cell) are kept as the cell's name,
attributes and parameters and written back. Any other directive is a located error.

## Mapping to the IR

| BLIF | IR |
|---|---|
| first `.model` | top module (`odin3_design` top); later models in file order |
| `.model … .blackbox` | `odin3_celltype_declare_blackbox` (IR-7b), in file order |
| `.inputs` / `.outputs` names | ports via `odin3_module_add_port`, in file order |
| `.names a b y` + rows | `$sop`, `WIDTH` = input count, `COVER` = rows as written; zero inputs allowed |
| `.latch` `re`/`fe` | `$_DFF_P_` / `$_DFF_N_` (C = ctrl); `ah`/`al` → `$_DLATCH_P_`/`_N_`; no type/ctrl → `$_FF_`; `INIT` 0–3, default 3 |
| `.subckt m f=a …` | node of cell type `m` (a module or a declared/registered black box); unlisted formals stay unconnected |
| net name | net with that exact name (created on first reference) |

**Port grouping (IR-7b).** A run of names `a[0] a[1] … a[w-1]` that appear consecutively, in
order, in one `.inputs`/`.outputs` list (or as consecutive formals of a black-box model) forms
one vector port `a` of width `w` written back with brackets; anything else is a scalar port with
its exact name. Grouping never reorders, so port order round-trips exactly.

**Two passes.** Pass 1 reads every `.model` header and its port lists (modules, ports and black
boxes exist before any body is read, so `.subckt` may reference a model defined later and
`add_port` never meets an instantiated type). Pass 2 builds the bodies.

**Provenance.** One pass run `read_blif`; every node, net and wire gets an `IMPORTED` record whose
location is the BLIF file and line of the directive that created it (IR-12).

## Writer

Per module in creation order: `.model`, `.inputs`, `.outputs` (port order; vector ports expand to
`a[0] … a[w-1]`), cells in node ID order (which is file order for read designs; IR-16), `.end`;
then each declared black-box model in declaration order. Net names: the net's own name, else its
primary wire bit, else a generated `$n<ID>` (stable until `compact`). `.latch` always writes its
init (netlist-compare spells the default too). Long lines wrap with ` \` at 100 columns.

## Errors

A new status `ODIN3_ERR_PARSE` (ABI 1 → 2). Every error is logged as `file:line: message` and
reading stops at the first error; the design is left without the partially read modules
(the reader builds into a fresh design that the caller destroys on failure).

## Files

`src/frontends/blif/lexer.{h,c}` (directives as token lists with line numbers, continuations,
comments), `src/frontends/blif/reader.{h,c}` (`odin3_blif_read(design, path)`),
`src/backends/blif/writer.{h,c}` (`odin3_blif_write(design, path)`), `tests/unit/test_blif_*.c`,
`tests/golden/blif/` (fixtures: a few dozen small goldens covering every construct, both tools),
`tools/blif-roundtrip/` (a C driver `odin3-blif-rt in out` linked against `odin3_core`, and a
script that runs it plus `netlist-compare` and a name-set check over a golden tree, printing a
pass/fail table).

## Out of scope

Reading into an existing design with other content, `.exdc`, `.gate`/`.mlatch` (genlib-mapped
BLIF), `.conn`, hierarchy flattening, tech-library typing of hard blocks (1G; until then the
goldens' own `.blackbox` declarations type them).
