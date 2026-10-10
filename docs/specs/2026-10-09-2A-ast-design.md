# 2A — Source manager, AST and symbol table: design

Status: draft for Peter's review (the AST is a design rule, PHASE2 #1). Phase 2, sub-project 2A.
Spec: `docs/DESIGN.md` §3, §4.0–4.2, §4.5, §5.3, §6 steps 1–2, §15. Inputs: PHASE2 decisions #6
(coverage D1–D5) and #7 (the approved AST decisions); the construct table in
`docs/specs/2026-10-09-2C-verilog-coverage.md` (cited below as rows `L1`, `M1`, …, and its §6
"Notes for 2A"). Style and conventions follow `docs/IR.md`; each decision is numbered **AST-n**
so later documents can cite it. Where this document and PHASE2 #7 disagree, #7 wins and this
file is corrected.

## 1. Purpose and scope

2A delivers the three data structures every HDL front end shares, with no parser in them:

| Delivered by 2A | Used by |
|---|---|
| **Source manager** (`src/ast/srcman.[ch]`): file table, line maps, 32-bit locations, macro-expansion and include chains, location printing, located diagnostics | 2B creates buffers and expansion records; 2C stamps tokens; 2D and the IR print locations; provenance (IR-12) copies decoded locations |
| **AST store** (`src/ast/ast.[ch]`, `kinds.[ch]`, `number.c`, `attr.c`, `comment.c`, `check.c`, `print.c`, `dump.c`): node records, kind catalogue, payloads, builders, read API, iterative traversal, attributes, comment side table, structural check, Verilog printer and s-expression dump | 2C builds the parsed form; the slang adapter (Phase 5) builds the elaborated form through the C ABI; 2D reads either |
| **Symbol table** (`src/ast/symtab.[ch]`): scopes, symbols, lookup, assignment lists | 2D builds and queries it during elaboration (PHASE2 #7(4)); the parser records names only |
| **C ABI** (`src/api/ast_abi.c`, `include/odin3/odin3.h`, ABI 3 → 4): builder and read mirrors following the 1D conventions | plugins, the Python binding, `adapters/slang` |

Not in 2A: the preprocessor and project readers (2B), the Bison/Flex grammar (2C), elaboration,
constant evaluation and the symbol-table *filling* (2D), `proc` (2E). 2A ships no Verilog
reader; its tests build ASTs by hand and its benchmark builds a synthetic AST (§11).

Success: every rule below has a unit test under ASan/UBSan; `odin3_ast_check` catches each
violation it lists; the benchmark numbers in §11 are recorded in `docs/PHASE2.md`; the lint
gate passes.

## 2. Shape

```
Design ─┬─ strtab (names, paths, string literals)                         design-global (IR-5)
        ├─ srcman ─┬─ buffer table (files and macro expansions)             design-owned (AST-1)
        │          ├─ per-file line maps
        │          └─ per-stream segment maps (preprocessed text → loc)
        ├─ ast (at most one, freed after elaboration unless kept; AST-14)
        │     ├─ nodes      (pagevec, 28-byte records)                      AST-local IDs
        │     ├─ children   (vec of node IDs; each node owns one span)
        │     ├─ numbers / reals (pagevec payloads) + arena for literal bits
        │     ├─ attr map   (node ID → span of ATTR nodes)
        │     ├─ comments   (vec sorted by loc, text in the arena)
        │     └─ pending stack (builder scratch)
        └─ symtab (built by 2D; scopes, symbols, (scope,name) → symbol map)
```

The AST is a sibling of the IR: arena/pagevec storage (IR-18), typed `uint32_t` IDs with 0 =
none (IR-5), the design's string table, and locations that provenance copies (IR-6, IR-12).
Unlike the IR it is **append-only and immutable once built** (no delete, no compact), so no ID
is ever dead and no tombstones exist.

## 3. Source manager

**AST-1 One source manager per design.** `odin3_design_srcman(design)` creates it on first use
and the design destroys it; it outlives the AST (§9) because provenance and every later
diagnostic print through it. It stores no source text: only buffer bounds, line starts and chain
records, so keeping it costs a few hundred KB for the largest VTR benchmark.

**AST-2 A location is one `uint32_t` offset into a virtual source space** (`odin3_loc`,
0 = unknown), as in Clang. Every *buffer* — a file, or one macro expansion — occupies a
contiguous range `[start, start + len]` of that space, allocated in creation order. A token's
location is its spelling offset inside its buffer; a node's range is two offsets `{loc, end}`
(`end` one past the last byte). Chosen over a `{file, line, col}` triple (12 bytes, no room for
chains, costs a table lookup to compare) and over an index into a location table (needs
hash-consing per token): an offset is 4 bytes, ordered, comparable, and decodes in
O(log buffers + log lines). Space: 2^32 − 1 bytes of files plus expansion text; a design that
exceeds it (only an expansion bomb does) gets a located `ODIN3_ERR_PARSE`
"source space exhausted" from the call that crosses the limit, after 2B's own expansion caps.

**Buffer record** (`odin3_srcbuf`, pagevec, IDs from 1):

| field | FILE buffer | EXPANSION buffer |
|---|---|---|
| `kind` | `ODIN3_SRC_FILE` | `ODIN3_SRC_EXPANSION` |
| `name` | path as given in the project (strtab) | macro name (strtab) |
| `resolved` | absolute path (strtab) | 0 |
| `library` | library name (strtab; `work` default) | 0 |
| `parent` | loc of the `` `include `` directive that opened it, 0 for a project file | loc of the macro use (the expansion site; inside another expansion when nested) |
| `def` | 0 | loc of the macro's definition body |
| `start`, `len` | range in the location space | range; `len` = bytes of expanded text |
| `lines` | vec of line-start offsets (relative to `start`), line 1 at 0 | — |

Decoding `loc` → buffer is a binary search on `start`; line is a binary search in `lines`; the
column is `1 + offset − line_start` in bytes (tabs count 1, UTF-8 counts bytes; the same rule
Yosys and GCC use for column 1 = first byte). **Presumed location**: follow `parent` while the
buffer is an EXPANSION (a loop, not recursion); the first FILE buffer reached is where the user
wrote the text. `odin3_srcloc` for provenance (IR-12) is filled by
`odin3_srcman_srcloc(sm, loc, end)`: `file` = the presumed file's *given* path (strtab), line and
column of the presumed location, `end_line`/`end_col` from `end` (0 when `end` is 0). 2D fills
a SOURCE record's `locs[0]` with that, then one entry per expansion level inward (the spelling
inside each macro body), so a record prints its own "expanded from" chain after the AST is gone;
the include chain is per file and read from the source manager when printing.

**Interaction with 2B.** The preprocessor owns macro tables and include resolution; the source
manager owns locations. 2B calls `odin3_srcman_add_file` when it opens a file (project file or
include, with the include site), `odin3_srcman_add_expansion` once per macro expansion event
(macro name, site, definition loc, expanded length), and appends line starts as it scans
(`odin3_srcman_add_line`). 2B emits expanded text plus a **segment map** per compilation stream:
`odin3_srcman_add_segment(stream, out_offset, loc)` records that output byte `out_offset` onward
is spelled at `loc` onward, until the next segment. Granularity: at least one segment per
expansion and per return to the file; text substituted from a macro *argument* keeps a segment
pointing at its file spelling, so an error inside an argument reports the argument, not the
macro (Clang's behaviour). The 2C lexer converts a token's output offset to a loc with a monotone
cursor (`odin3_srcman_cursor_loc`, amortized O(1)); the Bison locations are `{loc, end}` pairs.
`` `line `` is ignored (L21): it never changes a loc.

**Printing.** `odin3_srcman_format(sm, loc, strbuf, style)` writes `file:line:col` with the
given path (default; tests compare paths relative to the case directory, DESIGN §4.0) or the
resolved path; unknown locations print `<unknown>:0:0`. `odin3_srcman_format_chain` appends one
line per level, inner to outer, `  expanded from macro 'NAME' at file:line:col`, then one per
include level `  included from file:line`. **Located diagnostics**: `odin3_diag(design, level,
loc, fmt, ...)` formats `file:line:col: error|warning|info: message` followed by the chain lines
and delivers it through `util/log` in one call (one sink message), so a located message is never
split by another. A reader returns `ODIN3_ERR_PARSE` when it emitted at least one error.

**Limits** (every one a located `ODIN3_ERR_PARSE`): `ODIN3_SRC_MAX_FILE_BYTES` = 2^30 per file;
`ODIN3_SRC_MAX_BUFFERS` = 2^24; the location space of AST-2.

## 4. AST store

### 4.1 Node record (AST-3)

```c
typedef struct odin3_ast_node {       /* 28 bytes, pagevec, IDs from 1, slot 0 reserved */
    uint8_t  kind;    /* odin3_ast_kind (§4.2) */
    uint8_t  sub;     /* per-kind sub-kind: operator, net kind, case kind, edge, direction … */
    uint16_t flags;   /* per-kind flag bits (§4.3) */
    uint32_t loc, end;/* source range (AST-2); end 0 = unknown */
    uint32_t name;    /* strtab ID, 0 = none */
    uint32_t child;   /* first index of the node's span in the child table */
    uint32_t nchild;  /* span length */
    uint32_t payload; /* index + 1 into the kind's payload store, 0 = none (NUMBER, REAL only) */
} odin3_ast_node;
```

The field list is the contract; the layout stays behind accessors (as IR-15). **Every child ID
is smaller than its parent's** (AST-4): nodes are made bottom-up, so a forward sweep over IDs
visits children before parents and a backward sweep visits parents first; `check` enforces it.
A node belongs to exactly one parent span, or to exactly one attribute span (`ATTR` nodes), or
is a root (`UNIT`, or a subtree the builder abandoned on a syntax error — allowed, unreachable).

The child table is one `odin3_vec` of `uint32_t` (indices survive growth; iteration is
contiguous). Node IDs are design-independent indices; the ABI hands out the raw `uint32_t` with
the AST handle, as 1D does for IR IDs.

### 4.2 Kind catalogue (AST-5)

Legend. Children: **P** positional slots (in order; `|0` = may be absent, ID 0) then a **tail**
(`…`, any count). Classes: E expression (`NUMBER REAL STRING IDENT HIER_NAME SELECT CONCAT
REPLICATE UNARY BINARY TERNARY CALL MINTYPMAX`), S statement (`SEQ_BLOCK` … `NULL_STMT`),
D declaration (`NET_DECL VAR_DECL PARAM_DECL GENVAR_DECL EVENT_DECL`), I module/generate item
(D plus `PORT_DECL CONT_ASSIGN ALWAYS INITIAL INSTANTIATION GATE_DECL FUNCTION_DECL TASK_DECL
SPECIFY_BLOCK DEFPARAM GENERATE GEN_FOR GEN_IF GEN_CASE GEN_BLOCK`), R `RANGE`, L `LIST`.
"name" says what the `name` field holds. Rows cite the coverage table. Every kind name is
`ODIN3_AST_<KIND>`; kind 0 is `NONE` (never stored).

**Child-ordering conventions.** (1) Positional slots come first, in the order the catalogue
lists, and an absent optional slot is ID 0, so a slot's index never moves. (2) The tail holds
items in source order. (3) A kind has a tail or it does not; a kind that needs two lists holds a
`LIST` node in a positional slot. (4) Nested operands put the thing operated on first (`SELECT`
base, `REPLICATE` count, `TIMING_STMT` control). (5) The body of a `CASE_ITEM` precedes its
match expressions so that `default` (no expressions) and a `4'd1: ;` (empty body) both keep
one shape.

**Design units**

| Kind | sub | name | children | rows |
|---|---|---|---|---|
| `UNIT` | — | path as given | tail: `MODULE`, `DIRECTIVE`, `UDP_DECL`, `CONFIG_DECL` in file order | one per project source file; a stream of several files is several UNITs in project order |
| `DIRECTIVE` | directive kind: `default_nettype timescale celldefine endcelldefine resetall begin_keywords end_keywords unconnected_drive nounconnected_drive pragma` | argument text as written (`none`, `1ns/1ps`, `"1364-2005"`) | none | L18–L22; effective setting for a module = the last preceding `DIRECTIVE` of that kind in UNIT order, `resetall` clears |
| `MODULE` | — | module name | P: `LIST`\|0 of `PARAM_DECL` (the `#(…)` header), `LIST`\|0 of ports (`PORT_DECL` ANSI, or `PORT_REF` non-ANSI); tail: I | M1–M9; flag `MACROMODULE` (M8) |
| `UDP_DECL` | — | name | none (opaque range) | Q7 REJECT |
| `CONFIG_DECL` | — | name | none (opaque range) | S2 REJECT |
| `LIST` | — | — | tail: any | a bare list in a positional slot |

**Ports and declarations**

| Kind | sub | name | children | rows |
|---|---|---|---|---|
| `PORT_REF` | — | port name (0 for `{a,b}`) | P: E\|0 — absent for a bare name; present for `.x(e)` and `{…}` | M1; M6 REJECT when the child is present |
| `PORT_DECL` | direction `input output inout` | — | P: R\|0; tail: `DECLARATOR` (exactly one in an ANSI header) | M1–M3, M12; flags `SIGNED`, `IN_HEADER`, data type (`REG`, `INTEGER`), net kind field (`output wire`, `output wor`; 0 = unspecified) |
| `DECLARATOR` | — | declared name | P: E\|0 (initialiser or parameter value); tail: R (unpacked dimensions, outermost first) | N7–N10, N12, P1; flag `ESCAPED` |
| `RANGE` | — | — | P: E msb, E lsb | N2 (either direction kept as written) |
| `NET_DECL` | net kind `wire tri wand wor triand trior tri0 tri1 trireg uwire supply0 supply1` | — | P: R\|0, `DELAY`\|0, `STRENGTH`\|0; tail: `DECLARATOR` | N1, N11, N14–N16, N19; flags `SIGNED VECTORED SCALARED` |
| `VAR_DECL` | var kind `reg integer time real realtime` | — | P: R\|0; tail: `DECLARATOR` | N3, N4, N9, N17; flag `SIGNED` |
| `PARAM_DECL` | `parameter localparam specparam` | — | P: R\|0; tail: `DECLARATOR` (each with its value) | P1–P4, P7; flags `SIGNED`, `IN_HEADER`, data type (`INTEGER REAL REALTIME TIME`) |
| `DEFPARAM` | — | — | P: `HIER_NAME` target, E value | P6; one node per `a.b = v` pair |
| `GENVAR_DECL` | — | — | tail: `DECLARATOR` | N6 |
| `EVENT_DECL` | — | — | tail: `DECLARATOR` | N18 REJECT |
| `STRENGTH` | `drive` / `charge` | — | none; flags bits 0–3 strength0, 4–7 strength1 (`highz weak pull strong supply`; charge `small medium large`) | A3, N19, Q4 IGNORE |
| `DELAY` | — | — | tail: 1–3 E (each may be `MINTYPMAX`) | A2, T11, Q4 IGNORE |

**Module items**

| Kind | sub | name | children | rows |
|---|---|---|---|---|
| `CONT_ASSIGN` | — | — | P: `STRENGTH`\|0, `DELAY`\|0; tail: `NET_ASSIGN` | A1–A3 |
| `NET_ASSIGN` | — | — | P: E lhs, E rhs | A1 |
| `ALWAYS` | `always` (`always_ff always_comb always_latch` reserved for Phase 5) | — | P: S | T1–T6; T6 is detected by 2D: the statement is not a `TIMING_STMT` whose control is an `EVENT_CONTROL` |
| `INITIAL` | — | — | P: S | T5 (D2) |
| `INSTANTIATION` | — | module name | P: `LIST`\|0 of `CONNECTION` (parameter overrides, ordered or named); tail: `INSTANCE` | I1–I7, P5 |
| `INSTANCE` | — | instance name (0 for an unnamed gate) | P: R\|0 (instance array); tail: `CONNECTION` | I5, I6, Q5 |
| `CONNECTION` | — | port or parameter name (0 = ordered) | P: E\|0 (absent: `.p()` or an empty positional slot) | I1–I3; mixing ordered and named is detected by 2D (I2 REJECT) |
| `GATE_DECL` | gate kind `and nand or nor xor xnor buf not bufif0 bufif1 notif0 notif1`, then the REJECT kinds `nmos pmos cmos rnmos rpmos rcmos tran rtran tranif0 tranif1 rtranif0 rtranif1 pullup pulldown` | — | P: `STRENGTH`\|0, `DELAY`\|0; tail: `INSTANCE` (ordered `CONNECTION`s) | Q1–Q6 |
| `FUNCTION_DECL` | — | function name | P: R\|0 return range, L ports (`PORT_DECL`, ANSI or collected non-ANSI, in order), L local D, S body | K1–K8; flags `AUTOMATIC`, `SIGNED`, data type of the return (`INTEGER REAL REALTIME TIME`) |
| `TASK_DECL` | — | task name | P: L ports, L local D, S body | K9–K12; flag `AUTOMATIC` |
| `SPECIFY_BLOCK` | — | — | none (opaque range to `endspecify`) | S1 IGNORE |
| `ATTR` | — | attribute key | P: E\|0 value | L11, C10; flag `METACOMMENT` when made from `// synopsys …` (D1); attached to its node through the attribute map (§7), never a child |

**Generate**

| Kind | sub | name | children | rows |
|---|---|---|---|---|
| `GENERATE` | — | — | tail: I (a region may hold several items and `GENVAR_DECL`s) | G1 |
| `GEN_FOR` | — | — | P: `BLOCKING_ASSIGN` init, E cond, `BLOCKING_ASSIGN` step, I body (usually a `GEN_BLOCK`) | G2–G4 |
| `GEN_IF` | — | — | P: E cond, I then, I\|0 else | G5 |
| `GEN_CASE` | — | — | P: E; tail: `CASE_ITEM` whose bodies are I | G6 |
| `GEN_BLOCK` | — | block name (0 = unnamed; 2D assigns `genblk<n>` in the symbol table, G7) | tail: I | G2, G3, G7 |

**Statements**

| Kind | sub | name | children | rows |
|---|---|---|---|---|
| `SEQ_BLOCK` | — | block name (0 = unnamed) | P: L\|0 local D; tail: S | T17, T18 |
| `PAR_BLOCK` | — | block name | as `SEQ_BLOCK` | T15 REJECT |
| `BLOCKING_ASSIGN` | — | — | P: E lhs, E rhs, (`DELAY`\|`EVENT_CONTROL`)\|0 intra-assignment control | T7, T11 |
| `NONBLOCKING_ASSIGN` | — | — | as `BLOCKING_ASSIGN` | T8 |
| `PROC_CONT_ASSIGN` | `assign deassign force release` | — | P: E lhs, E\|0 rhs | T16 REJECT |
| `IF` | — | — | P: E cond, S then, S\|0 else | C1, C2 |
| `CASE` | `case casez casex` | — | P: E; tail: `CASE_ITEM` | C3–C10 |
| `CASE_ITEM` | — | — | P: body (S, or I under `GEN_CASE`; `NULL_STMT` for `4'd1: ;`); tail: E (empty with flag `DEFAULT`) | C4, C6, C7 |
| `FOR` | — | — | P: `BLOCKING_ASSIGN` init, E cond, `BLOCKING_ASSIGN` step, S body | F1 |
| `WHILE` | — | — | P: E cond, S body | F2 |
| `REPEAT` | — | — | P: E count, S body | F3 |
| `FOREVER` | — | — | P: S body | F4 REJECT |
| `TIMING_STMT` | — | — | P: `DELAY`\|`EVENT_CONTROL`, S\|0 | T1, T2, T11, T12 |
| `WAIT` | — | — | P: E, S\|0 | T13 REJECT |
| `DISABLE` | — | — | P: `HIER_NAME` | T19 REJECT |
| `EVENT_TRIGGER` | — | — | P: `HIER_NAME` | T14 REJECT |
| `TASK_CALL` | — | task name (`$display` keeps its `$`) | tail: E arguments | K9–K11, S3, S4; flag `SYSTEM` |
| `NULL_STMT` | — | — | none | T20 |

**Timing and events**

| Kind | sub | name | children | rows |
|---|---|---|---|---|
| `EVENT_CONTROL` | — | — | tail: `EVENT_EXPR` (empty with flag `STAR` for `@*`/`@(*)`) | T1–T4; flag `NO_PARENS` for `@ident` |
| `EVENT_EXPR` | edge `none posedge negedge` | — | P: E | T1; flag `AFTER_OR` when `or` rather than `,` preceded it (printer only) |

**Expressions**

| Kind | sub | name | children / payload | rows |
|---|---|---|---|---|
| `NUMBER` | base `dec bin oct hex` | — | payload §4.4; flag `SIGNED` for `'s` | L4–L7, E17 |
| `REAL` | — | — | payload: `double` | L9 REJECT in expressions |
| `STRING` | — | unescaped bytes (strtab) | none | L10 |
| `IDENT` | — | identifier | none; flag `ESCAPED` (printed with `\` and a trailing space) | L2, L3 |
| `HIER_NAME` | — | — | tail: components, each `IDENT` or `SELECT`(bit) over an `IDENT` (`g[0].t`) | E21, G7, P6 |
| `SELECT` | `bit part part_plus part_minus` | — | P: E base, E a, E\|0 b (`[a]`, `[a:b]`, `[a+:b]`, `[a-:b]`); chains nest, base first (`m[i][3:0]` = part over bit) | E14–E16, N13 |
| `CONCAT` | — | — | tail: E | E12 |
| `REPLICATE` | — | — | P: E count, `CONCAT` | E13 |
| `UNARY` | op `+ - ! ~ & ~& \| ~\| ^ ~^` (`^~` is stored as `~^`) | — | P: E | E1–E4 |
| `BINARY` | op `+ - * / % ** == != === !== && \|\| < <= > >= & \| ^ ~^ << >> <<< >>>` (`^~` as `~^`) | — | P: E lhs, E rhs | E1–E10 |
| `TERNARY` | — | — | P: E cond, E, E | E11 |
| `CALL` | — | function name (`$signed`, `$clog2` keep `$`) | tail: E arguments; flag `SYSTEM` | E18–E20, K6, K7 |
| `MINTYPMAX` | — | — | P: E, E, E | A2, T11 (delays only) |

68 kinds. Every MUST and SHOULD row of the coverage table maps to a kind above; each IGNORE
row is a kind or a `STRENGTH`/`DELAY`/`DIRECTIVE` child that 2D drops with a located warning;
each REJECT row is a kind, a sub-kind or a form that 2D reports as a located error naming the
construct (`odin3_ast_kind_name`). The parser builds every one of them without judging it, so
a parse-only run is silent on semantics and a slang-built AST never contains them (§4.5).

Deliberate departures from the coverage audit's §6 notes: `SELECT` chains nest instead of one
node holding a list of selects (one shape for vectors and arrays, no variable-length payload);
`PORT_REF` carries no separate "port expression" kind (the presence of a child marks the REJECT
form); `EventOr`/`ImplicitEvent` are the `EVENT_CONTROL` tail and its `STAR` flag.

### 4.3 Flags (AST-6)

`flags` bits are **per kind**; the catalogue says which apply. Named bits, defined once:
`SIGNED` 0, `ESCAPED` 1, `DEFAULT` 2, `AUTOMATIC` 3, `SYSTEM` 4, `STAR` 5, `NO_PARENS` 6,
`AFTER_OR` 7, `MACROMODULE` 8, `IN_HEADER` 9, `VECTORED` 10, `SCALARED` 11, `METACOMMENT` 12,
data type field bits 13–15 (`ODIN3_AST_DT_NONE REG INTEGER REAL REALTIME TIME`). Two
documented exceptions reuse bits whose names do not apply to them: `PORT_DECL` holds its net
kind in bits 2–5 (`ODIN3_AST_F_PORT_NET_SHIFT`), and `STRENGTH` holds its two strengths in
bits 0–3 and 4–7. No other kind reuses a bit with a different meaning. `check` rejects a set bit
the kind does not define.

### 4.4 Payloads (AST-7)

Only `NUMBER` and `REAL` carry payloads; everything else fits the record (`sub`, `flags`,
`name`) or is children (`RANGE`, `DELAY`, `STRENGTH`, `MINTYPMAX`, case items, event edges).

```c
typedef struct odin3_ast_number {
    uint32_t width;       /* declared size, 0 = unsized */
    uint32_t nbits;       /* bits stored: what the digits spell (unsized decimal: 32 min) */
    const uint8_t *bits;  /* nbits odin3_bit values (0 1 x z), LSB first, in the AST arena */
    uint8_t base;         /* dec bin oct hex (also in node.sub) */
    bool    is_signed;    /* 's */
    bool    has_xz;       /* any x or z digit (C9: never matches in a plain case) */
    bool    has_question; /* a ? digit was written (stored as z; printer restores ?) */
} odin3_ast_number;
```

The literal text is converted once, in 2A: `odin3_ast_number_parse(ast, loc, text, &payload)`
accepts the §3.5.1 syntax (`[size]'[s]base digits`, `_`, `?`, spaces inside the number, L8) and
stores the literal **as written** (digits → bits, no sizing), so the printer round-trips it;
`odin3_ast_number_new` takes a ready struct (for the adapter). The §3.5.1 rules — zero-extend,
or extend with `x`/`z` when the leading digit is `x`/`z`, truncate to `width` with a warning,
unsized decimals signed 32-bit — live in one helper,
`odin3_ast_number_value(ast, node, &odin3_value, &is_signed)`, which returns an IR `odin3_value`
of kind `BITS`; 2D and the attribute flow (§7) use it, so the rule exists once. `STRING` values
are interned unescaped; a string with an embedded NUL is a located error (the strtab cannot hold
it, and no synthesizable use exists).

### 4.5 Parsed and elaborated forms (AST-8)

One node format, two subsets. The **parsed form** is what 2C builds: every kind above. The
**elaborated form** is what the slang adapter emits and what 2D's second stage consumes
(Phase 5 enters there; §14 Q3). A store carries `ODIN3_AST_ELABORATED` when its builder says so,
and `odin3_ast_check` enforces the subset:

- **Absent kinds**: `DIRECTIVE UDP_DECL CONFIG_DECL SPECIFY_BLOCK DEFPARAM GENERATE GEN_FOR
  GEN_IF GEN_CASE DELAY STRENGTH MINTYPMAX PROC_CONT_ASSIGN WAIT DISABLE EVENT_TRIGGER FOREVER
  PAR_BLOCK EVENT_DECL HIER_NAME`, `PORT_REF` with a child, `VAR_DECL` of `time real realtime`,
  `PARAM_DECL` of `specparam`.
- **Resolved constants**: every `RANGE` bound, `REPLICATE` count, part-select width,
  `REPEAT` count, instance-array range and `DECLARATOR` value of a `PARAM_DECL` is a `NUMBER`,
  `STRING` or `REAL` leaf. Every `PARAM_DECL` is `localparam`.
- **Expanded hierarchy**: one `MODULE` per parameter specialization, named as IR-7 requires
  (`sub$W=8`); `INSTANTIATION` has no parameter list and names the specialization; generate
  constructs are `GEN_BLOCK`s with non-zero names (`genblk<n>` included); ports are ANSI
  (`PORT_DECL` with one `DECLARATOR` in the port `LIST`, and no `PORT_REF`); implicit nets are
  declared; `ALWAYS` is `always` over `TIMING_STMT`[`EVENT_CONTROL`, S], or one of the
  Phase 5 subs.
- **Kept**: exactly one `UNIT` holding every specialization; functions and tasks with bodies
  (2D inlines them), `IDENT` leaves (resolution is the symbol table's job, built in 2D for both
  forms), attributes, `INITIAL` (D2), tri-state forms (D3).

Elaboration never rewrites a store in place (§2); the elaborated form is a second store built
with the same builders.

## 5. Builder API (AST-9)

Internal (`src/ast/ast.h`), used by 2C; the ABI mirror (§5.2) is the same calls with raw
`uint32_t`s and NULL checks.

```c
odin3_ast *odin3_ast_create(odin3_design *design, bool elaborated);   /* strtab, srcman: the design's */
void       odin3_ast_destroy(odin3_ast *ast);

typedef struct odin3_ast_spec {      /* what to make; the record's scalar fields */
    uint8_t kind, sub; uint16_t flags;
    uint32_t loc, end, name, payload;
} odin3_ast_spec;

/* Appends a node whose children are ids[0..n): copies the IDs into the child table. */
odin3_status odin3_ast_make(odin3_ast *ast, const odin3_ast_spec *spec,
                            const uint32_t *ids, uint32_t n, uint32_t *out);

/* List building on the pending stack (Bison list rules, visitor walks). */
uint32_t     odin3_ast_mark(const odin3_ast *ast);                       /* stack height */
odin3_status odin3_ast_push(odin3_ast *ast, uint32_t node);              /* NO_MEMORY possible */
odin3_status odin3_ast_make_marked(odin3_ast *ast, const odin3_ast_spec *spec,
                                   uint32_t mark, uint32_t *out);        /* children = stack[mark..top], popped on success */
void         odin3_ast_unwind(odin3_ast *ast, uint32_t mark);            /* error recovery */

odin3_status odin3_ast_number_parse(odin3_ast *ast, uint32_t loc, odin3_bytes text, uint32_t *payload);
odin3_status odin3_ast_number_new(odin3_ast *ast, uint32_t loc, const odin3_ast_number *num,
                                  uint32_t *payload);
odin3_status odin3_ast_real_new(odin3_ast *ast, double value, uint32_t *payload);
odin3_status odin3_ast_attach(odin3_ast *ast, uint32_t node, const uint32_t *attrs, uint32_t n);
odin3_status odin3_ast_comment_add(odin3_ast *ast, odin3_ast_comment_spec spec);
odin3_status odin3_ast_intern(odin3_ast *ast, odin3_bytes str, uint32_t *name);  /* design strtab */
```

Rules:

- **Shape is validated at make.** `kind` known; `sub` within the kind's range; `flags` only the
  kind's bits; `n` ≥ the kind's positional count and equal to it for kinds without a tail;
  every non-zero child ID < the new node's ID and already made; a mandatory slot non-zero;
  `name` a strtab ID (non-zero where the catalogue requires one); `payload` non-zero exactly
  for `NUMBER`/`REAL` and a valid index. Violations are `ODIN3_ERR_INVALID_ARG` with a logged
  message: builder misuse is a programming error in 2C or the adapter, never user input. Child
  *classes* (E/S/I) are not checked at make (that needs the children's kinds; `check` does it).
- **Reserve before mutate.** `make` reserves one node slot and `n` child slots before it
  writes anything (`make_marked` pops the pending stack only after both succeed); `number_new`
  and `number_parse` allocate the bits first, then the record. On `ODIN3_ERR_NO_MEMORY` the
  store, the pending stack and every ID handed out are unchanged, so the parser can abort
  cleanly (the design has no IR yet: the parse stage adds nothing to it, §9).
- **Ownership.** The AST owns its nodes, child table, payloads, bits, comments and attribute
  map; nothing is freed individually; IDs are valid until `odin3_ast_destroy`. Strings live in
  the design's strtab and survive the AST (§14 Q2).
- **Attach once.** `odin3_ast_attach` copies the `ATTR` IDs into the child table and records the
  span under the node; a second attach to the same node, or an ID that is not an `ATTR`, is
  `INVALID_ARG`. The parser collects every `(* *)` instance before an item into one attach.
- **Caps** (§10). The builder checks the literal caps in `number_parse`/`number_new` (at the
  `loc` they take) and the child-count cap in `make` (at `spec->loc`), each a located
  `ODIN3_ERR_PARSE`. Identifier and string length are the lexer's to check (it holds the
  location and the text); `intern` itself only fails with `NO_MEMORY`.
- **Single-threaded**, as IR-17.

### 5.1 How 2C uses it

A Bison semantic value is a `uint32_t` node ID; `@$` gives `{loc, end}`. A fixed-arity rule
calls `make` with a small array; a list rule does `mark` at its first item, `push` per item,
and `make_marked` at the parent. Nested lists are safe because LR reductions nest. On a syntax
error the action unwinds to the enclosing mark; abandoned nodes stay in the store as unreachable
roots (they are counted by `check` as info, never an error). A `(* *)` instance reduces to
`ATTR` nodes before the item it annotates; the item's rule attaches them.

### 5.2 C ABI mirror (ABI 3 → 4)

Following 1D: `odin3_` names only, opaque `odin3_ast *` and `odin3_srcman *`, raw `uint32_t`
IDs, `const char *` valid until the design changes, a NULL handle or out-pointer is
`ODIN3_ERR_INVALID_ARG` at the boundary, the cffi cdef block stays attribute- and
preprocessor-free. The kind, sub-kind and flag enums move into `odin3.h` (append-only; a kind
is never renumbered), and `odin3_ast_spec` gains `uint32_t reserved[2]` (zero) so a later field
does not break the ABI. The ABI versions of the number payload take the bits as a string of
`0 1 x z ?` (`odin3_ast_number_new_str`) and return it the same way. Source-manager builders
(`add_file`, `add_expansion`, `add_line`, `add_segment`, `cursor_loc`) are exported so an
adapter can copy slang's locations verbatim (DESIGN §4.2): slang's files become FILE buffers
and its macro expansions EXPANSION buffers, so provenance from SystemVerilog prints the same
chains. The Python layer gets read-side helpers only in 2A (`Design.ast()`, `node.kind`,
`node.children()`, `node.loc`); builders from Python are not needed.

## 6. Read API and iteration (AST-10)

Accessors, all O(1): `odin3_ast_kind`, `_sub`, `_flags`, `_loc`, `_end`, `_name`
(strtab ID), `_name_str` (`const char *`), `_nchild`, `_child(ast, node, i)`,
`_children(ast, node) → {const uint32_t *ids, n}` (a view into the child table, valid until the
next make), `_number(ast, node) → const odin3_ast_number *`, `_real`, `_attrs(ast, node) →
span view`, `_node_end(ast)` (one past the last ID), `_kind_name(kind)`, `_sub_name(kind, sub)`,
`_positional(kind) → count`, `_has_tail(kind)`, `_root(ast, i)`/`_root_count` (the UNITs in
order). Out-of-range IDs return `NONE`/0/empty, never fault (as the IR accessors).

**Traversal never recurses** (CLAUDE.md; the parser can produce a 4096-deep tree, §10, and
plugins must not have to guess). Two forms:

- `odin3_ast_walk(ast, root, pre, post, user)`: iterative pre/post-order over an explicit stack
  of `{node, next child}` frames held in a vec; `pre` may return `SKIP` to prune. `NO_MEMORY`
  before the first callback if the stack cannot grow.
- `odin3_ast_cursor`: `cursor_init(cur, ast, root)`, `cursor_next(cur, &node, &event)` yields
  `ENTER`/`LEAVE` events so a consumer (the printer, 2D's elaborator) drives the stack itself
  without callbacks; `cursor_skip` prunes; `cursor_free`.

ID sweeps are the third form: `for (id = 1; id < end; ++id)` visits children before parents
(AST-4), which is what bottom-up analyses (constant folding, width inference) need; the reverse
sweep visits every parent before its subtree.

`odin3_ast_equal(a, ra, b, rb)` compares two subtrees structurally (kind, sub, flags, name
bytes, payload values, children; locations ignored) — the round-trip oracle of §11.
`odin3_ast_dump(ast, root, strbuf)` writes the s-expression `(KIND[.sub][:name] @loc-end
children…)` used by golden tests; `odin3_ast_print` is §11.

## 7. Attributes and comments (AST-11)

**Attributes** are `ATTR` nodes (key, optional value expression) attached to one node through
the attribute map (`u64map`: node ID → packed `{span start, count}` in the child table), so they
travel with the node, print back in place, and cost nothing on nodes without them. Metacomments
`// synopsys full_case`, `parallel_case` (D1) are turned into `ATTR` nodes flagged `METACOMMENT`
by 2C and attached to the next `CASE`; the lexer records the comment in the side table as well.

**Flow to the IR** (2D applies it; the rule lives here so every front end agrees): when 2D
creates an IR object from an annotated node it copies each `ATTR` with `odin3_attr_set`
(IR-10), key = the attribute name, value = `INT 1` when the attribute has no value; a `NUMBER`
through `odin3_ast_number_value` (`BITS`); a `STRING` as `STRING`; any other constant expression
evaluated by 2D's constant evaluator to `BITS`; a non-constant value is a located error. Targets:
`MODULE` → the IR module; a `DECLARATOR` of a net/variable/port → its wire (and its port node);
an `INSTANCE` → the instance node; a statement or expression → every node the elaboration
operation that consumed it creates (they share one SOURCE record, IR-13), so `(* keep *)` on an
`assign` lands on its cells; `full_case`/`parallel_case` on a `CASE` reach `proc` (2E) on the
process object 2D defines for `always` blocks. Attributes on a construct 2D drops (an IGNORE
kind) are dropped with it, silently: the construct's own warning covers them.

**Comments** go to a side table keyed by location (DESIGN §4.1, PHASE2 #7): a vec of
`{loc, end, kind LINE|BLOCK, flags METACOMMENT, text}` sorted by `loc` (2C adds them in stream
order, so insertion is sorted; the builder verifies monotonic `loc` per UNIT), text copied into
the AST arena. `odin3_ast_comments_in(ast, loc, end)` returns the span of comments inside a
range by binary search. Comments are not nodes and never reach the IR.

## 8. Symbol table (AST-12)

2A delivers the structure and lookup; 2D fills it (PHASE2 #7(4)). One `odin3_symtab` per
design, owned by the design beside the AST and freed with it (§9).

**Scopes** (pagevec, IDs from 1): `{kind DESIGN|MODULE|GENERATE|BLOCK|FUNCTION|TASK, name
(0 = unnamed; `genblk<n>` once 2D names it), parent, node (the AST node that opened it), first
child scope, next sibling}` — a tree per *elaborated module* (one `MODULE` scope per parameter
specialization, since widths differ), all under the `DESIGN` root that holds module names.
Verilog has one namespace per scope for nets, variables, parameters, genvars, instances, named
blocks, functions and tasks, so one map serves them all: `u64map` keyed
`(scope << 32) | name` → symbol ID.

**Symbols** (pagevec, IDs from 1):

| field | meaning |
|---|---|
| `name`, `scope` | strtab ID; owning scope |
| `kind` | `NET VAR PARAM LOCALPARAM GENVAR INSTANCE BLOCK FUNCTION TASK MODULE EVENT` |
| `sub` | net kind or var kind (the AST enums) |
| `flags` | `SIGNED`, `IMPLICIT` (M10), `PORT`, `OVERRIDDEN` (parameter set by `#()`/`defparam`), `ASSIGNED_BLOCKING`, `ASSIGNED_NONBLOCKING`, `ASSIGNED_CONTINUOUS` |
| `dir` | port direction or none |
| `decl`, `declarator` | AST nodes (0 for an implicit net: `decl` = the first use) |
| `packed` | `{int32 msb, lsb}` + `HAS_PACKED` (direction preserved, `[0:3]` stays ascending) |
| `unpacked` | span into a dims vec of `{msb, lsb}` pairs, outermost first |
| `value` | `odin3_value` + signedness for parameters (payload in the symtab arena) |
| `assigns` | head/tail of an intrusive list of `{node, kind, next}` records: every assigning node, in source order, so T9, T10 and A4 report both locations |
| `ir_module`, `ir_wire` | the IR object the symbol became (0 until 2D creates it) |

API: `odin3_scope_new`, `odin3_symbol_new` (`INVALID_ARG` on a duplicate name in the scope —
2D turns it into the located M11 error with both `decl`s), `odin3_symtab_lookup(st, scope,
name)` walks `parent` outward in a loop, `_lookup_local`, `odin3_scope_child(st, scope, name)`
for hierarchical names (G7, P6), `odin3_symbol_add_assign`, `odin3_scope_first_child/next`,
`odin3_scope_symbols` (ID-order iteration of a scope's symbols through a per-scope intrusive
list), plain accessors. Everything fallible returns `odin3_status`, misuse `INVALID_ARG`, OOM
`NO_MEMORY` with the table unchanged.

## 9. Lifetime (AST-13, AST-14)

**AST-13 One AST per design per read.** `read_verilog` parses every Verilog file of the project
(all libraries) into one store — one `UNIT` per file, in project order, because module
resolution, `` `define `` state and directives (L19) cross file boundaries — then elaborates it.
The design holds at most one AST and one symbol table (`odin3_design_ast`,
`odin3_design_symtab`; the reader attaches them with `odin3_design_set_ast`, which destroys any
previous one).

**AST-14 The AST and symbol table are freed at the end of the read pass** unless kept: the CLI
flag `--keep-ast`, or any pass in the script whose `odin3_pass_def` has flag
`ODIN3_PASS_WANTS_AST` (AST-assisted `fsm_detect`, DESIGN §6 step 5; plugins), makes the pass
manager call `odin3_design_set_keep_ast(design, true)` before the read. A kept AST lives until
`odin3_design_destroy` or the next read. This amends DESIGN §4.5 ("never discarded") per
PHASE2 #7.

**What survives into the IR**: decoded locations in SOURCE provenance records (IR-12,
`odin3_srcloc` with the expansion chain, §3), the source manager (chains, line maps), names and
string literals in the strtab, attributes copied as IR attributes (§7), and the AST node ID in
`odin3_prov_record.ast` — meaningful only while the AST lives (`odin3_design_ast` returns NULL
after freeing; an ID into a freed store is never dereferenced because every AST accessor takes
the handle). Nothing in the IR holds a pointer into the AST (IR-5, spec §15.1).

A parse-stage failure (syntax error, cap, OOM) destroys the partial AST and leaves the design
as it was: the parse stage adds nothing to the IR. An elaboration-stage failure is 2D's contract.

## 10. Errors and limits

Status use: `ODIN3_ERR_INVALID_ARG` = builder or accessor misuse (a bug in 2C/2D/an adapter,
logged); `ODIN3_ERR_PARSE` = the input is rejected, always with a located diagnostic (§3) naming
the construct or the cap; `ODIN3_ERR_NO_MEMORY` = allocation or 32-bit ID exhaustion, nothing
changed (1A convention).

Caps against adversarial input (each a located `ODIN3_ERR_PARSE`; named constants in
`src/ast/ast.h`, mirrored in `odin3.h` for the ABI):

| Cap | Value | Why |
|---|---|---|
| `ODIN3_AST_MAX_DEPTH` | 4096 | Bounds every explicit stack and the parser's (2C sets `YYMAXDEPTH` ≥ 4× it and reports overflow as this error); left-deep operator chains reach depth n for n operands, and 4096 is beyond any real or generated design we have seen (mcml.v stays under 40). `check` verifies it; the builder cannot (depth is not known bottom-up) |
| `ODIN3_AST_MAX_NUMBER_BITS` | `ODIN3_READER_MAX_WIDTH` (2^20) | The same literal width the readers cap (PHASE2 #5), so a `1000000'b0` cannot allocate gigabytes of pins later |
| `ODIN3_AST_MAX_DECIMAL_DIGITS` | 4096 | Decimal-to-binary is O(n²); 4096 digits (13.6k bits) is already absurd for a decimal literal |
| `ODIN3_AST_MAX_STRING_BYTES` | 2^17 | So a string parameter (8 bits per byte) stays under the width cap (checked by the lexer, §5) |
| `ODIN3_AST_MAX_IDENT_BYTES` | 4096 | 1364-2005 §3.7 guarantees 1024; nothing real exceeds it (checked by the lexer, §5) |
| `ODIN3_AST_MAX_CHILDREN` | 2^24 per node | A `CONCAT` or module with 16M children is a bomb; real designs peak in the thousands |
| node count, child-table length | 2^32 − 2 | ID space; memory runs out first (28 bytes per node) and reports `NO_MEMORY` |
| source limits | §3 | |

Replication counts, loop trip counts and generate ranges are elaboration caps (2D; the
coverage table's §7.6 "cap unrolling").

**OOM policy.** Every builder reserves before it mutates and returns `NO_MEMORY` with the AST
unchanged. 2C stops parsing at the first `NO_MEMORY` (no recovery attempts, which would allocate
again), destroys the AST and returns `NO_MEMORY` from the pass; the design is unchanged. The
walk and cursor report `NO_MEMORY` before the first visit. The unit tests inject failures at
every allocation point (`odin3_util_set_alloc_fail_after`) and assert the counts are unchanged.

**`odin3_ast_check(ast, mode)`** (`src/ast/check.c`, run by `read_verilog` after the parse
stage in Debug builds and on demand, like IR `check`; `ODIN3_ERR_CHECK` on an error):

1. **E** every record has a known kind, a sub-kind in range, only the kind's flag bits, a
   payload exactly when the kind takes one, `loc ≤ end`, and both decode in the source manager
   (or are 0).
2. **E** child count ≥ the positional count (= for kinds without a tail); mandatory slots
   non-zero; every child ID is non-zero-or-optional, below the parent's ID (AST-4), and of the
   slot's class (E/S/I/D/R/L and the exact kinds the catalogue names).
3. **E** every node is referenced by at most one span (parent or attribute); an `ATTR` only by
   the attribute map. **I** unreachable non-root nodes (abandoned on a syntax error), one count.
4. **E** `name` non-zero where the catalogue requires it (`MODULE`, `IDENT`, `DECLARATOR`,
   `INSTANTIATION`, `TASK_CALL`, `CALL`, `ATTR`, `UNIT`, `DIRECTIVE`); every name is a strtab ID.
5. **E** depth ≤ `ODIN3_AST_MAX_DEPTH` (iterative DFS).
6. **E** comments sorted by `loc` within each UNIT; attribute-map spans valid.
7. **E** with `ODIN3_AST_ELABORATED`: the §4.5 subset (absent kinds, resolved constants, ANSI
   ports, named generate blocks, one parameter-free `INSTANTIATION` form).
8. **E** symbol table (when present): every symbol's scope exists, `decl` IDs are in range,
   the map agrees with the records, no duplicate `(scope, name)`.

## 11. Testing

Unit tests, one file per area under `tests/unit/test_ast_<area>.c` (Unity, ASan/UBSan):

- `srcman`: buffers, line maps, decode/presumed location, nested expansion and include chains
  printed exactly (`expanded from … / included from …`), segment-map cursor, srcloc for
  provenance, the three source limits, OOM injection.
- `build`: every kind made with its minimum and maximum children; every shape rule of §5
  rejected with `INVALID_ARG`; `make_marked`/`unwind` with nested marks; AST-4 ordering; attach
  once; caps of §10; OOM at every allocation with unchanged counts.
- `read`: accessors on out-of-range IDs; `walk` and `cursor` produce the same ENTER/LEAVE
  sequence as a 20-line recursive reference kept in the test (tests may recurse; `src/` may
  not); pruning; a 5000-deep chain walks without stack growth (ASan stack check).
- `number`: every §3.5.1 case (`4'hFF` truncation warning, `8'bx` extension, `'hF` unsized,
  `4'sb1000`, `?`, `_`), round-trip of the payload through the printer.
- `check`: one negative test per rule, by corrupting a valid store through a test-only hook
  (`src/ast/ast_test.h`, hidden) as 1B does.
- `symtab`: nested scopes, outward lookup, shadowing, duplicate detection, hierarchical child
  lookup, assignment lists in order, OOM.
- `attr`, `comment`, `dump`, `print`, `equal`, and the ABI mirror through `odin3.h` only
  (`test_ast_abi.c`), including `odin3_abi_version() == 4`.

**Verilog printer** (`odin3_ast_print(ast, root, strbuf)`, in 2A): emits Verilog-2005 from any
parsed-form subtree with canonical spacing, full parenthesisation of nested operators, escaped
identifiers, `?` digits and comments omitted. Worth its ~400 lines because 2C then tests every
construct as *parse → print → parse → `odin3_ast_equal`*: a dropped token, a wrong child order,
a precedence or associativity slip or a lost flag fails without a hand-written golden, and
corpus-scale runs (every micro and VTR file) cost nothing to maintain. The printer's own 2A
tests print hand-built subtrees against literal strings; it is also the `write_verilog_ast`
debugging pass. The elaborated form prints the same way (it is a subset).

**Benchmark** (`tests/bench/bench_ast.c`, built, not in CTest): a synthetic 2M-node AST with a
realistic kind mix (40% `IDENT`, 20% `BINARY`, 15% `NUMBER`, 10% `SELECT`, 15% statements and
declarations, ~1.1 children per node), measuring bytes per node (nodes + child table + payloads
+ bits, strtab excluded), build time, `walk` and cursor time, `check` time, destroy time.
Target: ≤ 48 bytes per node, so 2M nodes ≤ 96 MB; the 2 GB budget of spec §5.1 is the IR's.
When 2C lands, the same driver runs `read_verilog --parse-only` on the VTR set smallest first
(memory rule) and ends with **mcml.v (636 KB, 24,507 lines)**; target for mcml.v: AST + child
table + payloads + comments + srcman ≤ 20 MB (≈ 32 bytes per source byte; the estimate is
~200k nodes), parse-only wall time ≤ 1 s in Release. Numbers go to `docs/PHASE2.md`.

## 12. Review focus

The five failure modes most likely to bite, in order:

1. **Locations through the preprocessor.** An off-by-one between 2B's segment map and 2C's
   token offsets points every diagnostic at the wrong column, or at the macro definition instead
   of the use; and the presumed-location walk must stop at the first FILE buffer, never at an
   include's parent. The srcman tests with nested expansion inside an include are the guard;
   reviewers should trace one `` `define `` with arguments by hand through §3.
2. **Child-slot conventions drifting.** `CASE_ITEM` is body-first, `SELECT` is base-first,
   `FUNCTION_DECL` has four positional slots, `GEN_CASE` reuses `CASE_ITEM`; a parser rule that
   gets one wrong produces a plausible tree 2D misreads. `check` rule 2's class table and the
   print round-trip catch most of it; reviewers should diff the grammar actions against §4.2.
3. **Literal sizing split across two places.** The parser stores digits as written, the helper
   applies §3.5.1; if 2C "helpfully" sizes at lex time, `4'hFF` warns twice or not at all and
   the printer stops round-tripping. Reviewers should confirm 2C never touches `bits`.
4. **Elaborated subset too loose or too tight for slang.** Phase 5 is far off; if the subset
   forbids something slang must emit (e.g. hierarchical references slang resolves but we list
   `HIER_NAME` as absent) or allows something 2D's second stage then has to re-elaborate, the
   adapter forks the format. Reviewers should walk one SystemVerilog module with a parameterized
   instance and a generate loop through §4.5.
5. **Memory creep.** The 28-byte record and the "strings in the design strtab" choice are
   cheap for mcml.v but a field added "just for 2D" or comment text landing in the strtab
   changes the budget silently; the benchmark's bytes-per-node and the strtab size after
   `odin3_ast_destroy` are the gates. Reviewers should reject record fields that a side table
   or a payload could hold.

## 13. Spec amendments made with this document

Under PHASE2 #7: DESIGN §4.5 ("never discarded; IR objects back-point to it" → freed after
elaboration unless kept, AST-14; the back-pointer is the prov `ast` field, valid while the AST
lives) and §4.1 (locations are `{loc, end}` offsets decoded through the source manager, not a
five-field record on every node; provenance still receives the five fields, §3). `docs/IR.md`
§6: the `ast` field is "AST node ID in the design's AST, 0 = none, meaningful while
`odin3_design_ast` is non-NULL". 1B's design handle gains three owned pointers (source
manager, AST, symbol table) and a keep flag; 1D's `odin3_pass_def` carries a `flags` field
with `ODIN3_PASS_WANTS_AST` (1D has not landed: it adds the field, or 2A does if it lands
first). ABI bumped once, from the value 1D leaves: 3 → 4.

## 14. Open questions for Peter

Everything above is decided; each item here changes a 1B/1D contract or a 2D shape, so it is
his call. Recommendation first.

1. **Design owns the source manager, AST and symbol table** (AST-1, AST-13; three pointers on
   `odin3_design`). Recommend yes: provenance prints through the source manager for the life of
   the design, and "one AST per design" needs one owner. Alternative: the project record (§4.0)
   owns them, and the design keeps a borrowed pointer — two lifetimes to get wrong.
2. **AST strings in the design strtab** (§5, ownership). Recommend yes: names flow to IR wires
   and nodes without re-interning, and the IR already holds file paths there (IR-5). Cost: the
   identifiers and string literals of a freed AST stay (≈1 MB for mcml.v). Alternative: an
   AST-local strtab plus a copy per name 2D hands to the IR.
3. **2D in two stages, the elaborated AST being the seam** (§4.5): stage 1 parsed form →
   elaborated store (parameters, `defparam`, generate, constant functions), stage 2 elaborated
   store → IR (signedness, widths, processes); the slang adapter enters at stage 2. Recommend
   yes, and that the 2D spec fixes it; 2A only defines the subset and `check` rule 7 either way.
   Alternative: 2D goes parsed AST → IR in one stage and Phase 5 writes a second elaborator
   over slang's output.
4. **`ODIN3_AST_MAX_DEPTH` = 4096.** Recommend keeping a cap (adversarial `((((…` input must
   fail with a located error, not with Bison's "memory exhausted" or a stack overflow in a
   plugin); 4096 leaves 100× headroom over the corpus. Alternative: 65536, if generated
   left-deep operator chains worry you more than plugin recursion.
5. **Verilog printer in 2A** (§11, ~400 lines plus tests). Recommend yes, for the
   parse-print-parse oracle 2C gets for free. Alternative: s-expression dump only, and 2C writes
   per-construct goldens by hand.
