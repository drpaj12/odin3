# 2A — Source manager, AST and symbol table: design

Status: approved (agent default under Peter's overnight rule, 2026-10-10; open questions Q1–Q5
decided as recommended). v4: v2–v4 applied the Opus reviews of a7e3c7f, 5733a29 and f9cdcfa
(the AST is a design rule, PHASE2 #1). Phase 2, sub-project 2A. Spec: `docs/DESIGN.md` §3,
§4.0–4.2, §4.5, §5.3, §6 steps 1–2, §15. Inputs: PHASE2 #6 (coverage D1–D5) and #7 (the
approved AST decisions); the construct table in
`docs/specs/2026-10-09-2C-verilog-coverage.md` (rows cited as `L1`, `M1`, …). Conventions follow
`docs/IR.md`; each decision is numbered **AST-n**. Where this file and PHASE2 #7 disagree, #7
wins and this file is corrected.

## 1. Purpose and scope

2A delivers the data structures every HDL front end shares, with no parser in them:

| Delivered by 2A | Used by |
|---|---|
| **Source manager** (`src/ast/srcman.[ch]`): buffers (files, macro expansions, macro arguments, scratch), line maps, 32-bit locations, expansion and include chains, segment maps for preprocessed streams, printing, located diagnostics | 2B creates buffers and segments; 2C stamps tokens; 2D and every later pass print locations; provenance (IR-12) copies decoded locations |
| **AST store** (`src/ast/ast.[ch]`, `kinds.[ch]`, `number.c`, `attr.c`, `comment.c`, `check.c`, `print.c`, `dump.c`): 24-byte node records, the kind/slot table, payloads, builders with a builder-enforced depth cap, read API, iterative traversal, attributes, comment side table, structural check, Verilog printer and s-expression dump | 2C builds the parsed form; the slang adapter (Phase 5) builds the elaborated form through the C ABI; 2D reads both |
| **Symbol table** (`src/ast/symtab.[ch]`): scopes (library, module, generate with index, block, function, task), symbols, lookup, assignment lists, IR back-links | 2D builds and queries it (PHASE2 #7(4)); the parser records names only |
| **C ABI** (`src/api/ast_abi.c`, `include/odin3/odin3.h`, the next ABI version): builder and read mirrors under the 1D conventions | plugins, the Python binding, `adapters/slang` |

Not in 2A: the preprocessor and project readers (2B), the Bison/Flex grammar (2C), elaboration,
constant evaluation and the symbol-table *filling* (2D), `proc` (2E). 2A ships no Verilog
reader; its tests build ASTs by hand and its benchmark builds a synthetic AST (§11).

Success: every rule below has a unit test under ASan/UBSan; `odin3_ast_check` catches each
violation it lists; the §11 benchmark numbers are recorded in `docs/PHASE2.md`; the gate passes.

## 2. Shape

```
Design ─┬─ strtab (names, paths, string literals)                      design-global (IR-5)
        ├─ srcman ─┬─ buffers (FILE, EXPANSION, MACRO_ARG, SCRATCH)      design-owned (AST-1)
        │          ├─ per-FILE line maps
        │          └─ per-stream segment maps (preprocessed text → loc)
        ├─ asts: at most one per read run (+ the parsed store when kept; AST-15/16)
        │     ├─ nodes      (pagevec, 24-byte records)                   AST-local IDs
        │     ├─ children   (vec of node IDs; each node owns one span)
        │     ├─ payloads   (numbers, reals, opaque text) + arena for bytes
        │     ├─ attr map   (node ID → span of ATTR nodes)
        │     ├─ comments   (vec, sorted by loc at finish)
        │     └─ build scratch: pending stack, height vec (freed at finish)
        └─ symtab per AST (built by 2D)
```

The AST is a sibling of the IR: arena/pagevec storage (IR-18), typed IDs with 0 = none (IR-5;
internally `odin3_ast_id`, `odin3_loc`, `odin3_srcbuf_id`, `odin3_scope_id`, `odin3_sym_id`,
each `struct { uint32_t v; }`; raw `uint32_t` only at the ABI), the design's string table, and
locations that provenance copies. Unlike the IR it is **append-only and immutable once
finished** (no delete, no compact): no ID is ever dead, no tombstones exist.

## 3. Source manager

**AST-1 One source manager per design.** `odin3_design_get_srcman(design, &sm)` creates it on
first use (`NO_MEMORY` possible) and the design destroys it; it outlives every AST because
provenance and every later diagnostic print through it. It stores no source text: buffer bounds,
line starts, chain records and segment maps only.

### 3.1 Locations (AST-2): the Clang model

A location is one `uint32_t` offset into a virtual source space (`odin3_loc`, 0 = unknown).
Every *buffer* occupies `[start, start + len]`; buffers are allocated in creation order from
offset 1, each reserving `len + 1` so that one-past-the-end is a loc of its own buffer and no
two buffers touch. A token's loc is `cursor_loc` of the output offset of its **first** byte
(the buffer it was spelled in), and its `end` is `cursor_loc(output offset of its last byte) + 1`
— the loc of the last byte plus one, in whichever buffer that byte is spelled — never
`loc + length`, because one token can span segments: `` x`S `` with `` `define S _y `` lexes as
the one identifier `x_y`, and a hex literal's digits may come from adjacent macros. A stripped
comment becomes an inserted space (a segment with `loc` 0), so `a/**/b` never merges into `ab`.
A node's range is two independent locs (`odin3_range {loc, end}`; `end` 0 = unknown) that may
lie in different buffers (the `NET_ASSIGN` of `assign `OUT = a;` starts in an expansion and
ends in the file). Chosen over a `{file, line, col}` triple (12 bytes, no chains, no ordering) and
over a hash-consed location table (a lookup per token): 4 bytes, decodes in
O(log buffers + log lines), and the chains come free from the buffer graph.

| field | `FILE` | `EXPANSION` | `MACRO_ARG` | `SCRATCH` |
|---|---|---|---|---|
| range covers | the file's bytes (also the **`<command line>` buffer**, `name` = `<command line>`, `resolved` and `library` 0: one `` `define NAME(args) body `` line per `+define+`/project `define`, in project order, LF-terminated, so every macro has a defining buffer and lines decode) | the macro's **body as spelled in its definition** (offset *k* ↔ `def + k`) | **one contiguous spelled run** of a substituted argument (offset *k* ↔ `def + k`) | pasted/stringified text that exists nowhere (SV `` `` ``/`` `" ``, Phase 5; never made by 2B) |
| `name` | path as given (strtab) | macro name | macro name | 0 |
| `resolved`, `library` | absolute path (0 for `<command line>`); library (`work` default; an included file inherits its includer's) | 0 | 0 | 0 |
| `parent` | loc of the `` `include `` that opened it, 0 for a project file | the use site: loc of the macro name token | loc of the formal parameter's occurrence inside the `EXPANSION` body it was substituted into | loc of the operator inside the `EXPANSION` body |
| `parent_end` | 0 | one past the use (after `)` or the name) | one past the formal | one past the operator |
| `def` | 0 | loc of the body's first byte in the defining buffer | loc of the run's first byte as spelled: in a file, in an expansion body, or in another argument run | 0 |
| `lines` | vec of line-start offsets, line 1 at 0 | — | — | — |

**Argument runs.** After substitution an argument's text may be spelled in several buffers, so
2B makes one `MACRO_ARG` buffer per contiguous spelled run; the runs of one substitution share
`parent`/`parent_end`, and a formal used twice in a body (`(x*x)`) gets its own buffers per
occurrence. Each expansion event costs its body length once and each substitution its argument
length once, so nesting does not multiply the space used. Worked example (`f.v`):

```
1  `define INNER(p) (p + 1)                  INNER body at 1:18
2  `define OUTER(q) `INNER(q * 2)            OUTER body at 2:18; q at 2:25, * at 2:27, 2 at 2:29
3  assign y = `OUTER(xx);                    `OUTER at 3:12, xx at 3:19, ) at 3:21
```

Buffers, in creation order: `E1` = `EXPANSION` of `OUTER` (len 13, `def` 2:18, `parent` 3:12,
`parent_end` 3:22); `A1` = `MACRO_ARG` for `q` ← `xx` (len 2, `def` 3:19, `parent` E1+7 — the
`q`, spelled 2:25 — `parent_end` E1+8); `E2` = `EXPANSION` of `INNER` (len 7, `def` 1:18,
`parent` E1+0, spelled 2:18, `parent_end` E1+13). `INNER`'s argument after substitution is
`xx * 2`, two spelled runs: `A2` = `xx` (len 2, `def` A1+0, `parent` E2+1 — the `p`, spelled
1:19 — `parent_end` E2+2) and `A3` = ` * 2` (len 4, `def` E1+8, spelled 2:26, same
`parent`/`parent_end`). Output text `assign y = (xx * 2 + 1);` with segments: `assign y = `
from the file, `(` from E2+0, `xx` from A2, ` * 2` from A3, ` + 1)` from E2+2, `;` from 3:22.

### 3.2 Decoding

Every walk below is a loop over buffer records, never recursion.

- **Spelling** of `loc` is **one step**: in a `FILE`, itself; in an `EXPANSION` or
  `MACRO_ARG`, `def + (loc − start)`, which may itself lie in a body or in another argument
  run (an argument run's `def` need not be a `FILE` loc); `SCRATCH` has none. The walks below
  take further steps as they need them. Example: `xx` (A2+0) → A1+0; `*` (A3+1) → E1+9.
- **Expansion location** (`odin3_srcman_expansion_loc`, Clang's `getExpansionLoc`): follow
  `parent` while the buffer is not a `FILE`. It is where the outermost macro was invoked:
  `xx`, `*` and `2` all → E2 → E1 → 3:12. Used for range comparison and `locs[0]` ordering.
- **File location** (`odin3_srcman_file_loc`, Clang's `getFileLoc`): in a `MACRO_ARG`, take
  one spelling step; in an `EXPANSION`/`SCRATCH`, go to `parent`; repeat until a `FILE`. A
  token from an argument therefore reports its own spelling, a token from a body the macro's
  use: `xx` → A1+0 → 3:19; `*` → E1+9 → 3:12; `(` (E2+0) → E1+0 → 3:12. Diagnostics use it
  (§3.4).
- **Line and column**: `odin3_srcman_decode(sm, loc, &pos)` gives `{buffer, line, col}` for a
  `FILE` loc: a binary search on `start`, one in `lines`, `col = 1 + offset − line_start` in
  **bytes** (tabs count 1, UTF-8 counts bytes; Clang's rule). A non-`FILE` loc decodes through
  its file location.
- **Ends map through `parent_end`** (`odin3_srcman_expansion_end`): while the buffer is not a
  `FILE`, `end = parent_end` of that buffer. **Ranges** compare only after both ends are
  mapped: `odin3_srcman_expansion_range(sm, odin3_range r, &out)` returns `{expansion_loc(loc),
  expansion_end(end)}` when both are in one `FILE` buffer and ordered, else the start alone
  (`end` 0). `loc ≤ end` is required only when both lie in one buffer (§10, rule 1). Worked
  example, `` `define OUT y `` on line 1 (body at 1:13) and `assign `OUT = a;` on line 2
  (`` `OUT `` at 2:8–2:11, `a` at 2:15, `;` at 2:16); E = the expansion (len 1, `parent` 2:8,
  `parent_end` 2:12): `IDENT y` has `{E+0, E+1}` → expansion range 2:8–2:12; `NET_ASSIGN` has
  `{E+0, 2:16}` → 2:8–2:16; `CONT_ASSIGN` `{2:1, 2:17}` → 2:1–2:17. With `` `define W 8 `` and
  `assign x = `W;`, `NET_ASSIGN` ends at the expansion's end → its `parent_end`, one past
  `` `W ``, so the range covers the macro use. A node wholly inside one expansion gets the
  non-empty range of that use.
- **Provenance**: `odin3_srcloc` (IR-12) gains a `uint32_t loc` field holding the **raw loc**
  of the node — any buffer, exactly as stored on the AST node (amendment, §13) — so the
  expansion chain, the include chain and the macro names are all recoverable from the
  design-owned source manager for the life of the design, and nothing else need be stored.
  `odin3_srcman_srcloc(sm, odin3_range r, &srcloc)` fills `loc` (raw), `file` (given path of
  the file location, strtab), `line`, `col` (of the file location), `end_line`, `end_col` (of
  the expansion range; 0 when it is not in one file). A SOURCE record stores one `odin3_srcloc`
  per node it covers in `locs`; `locs[1..]` are **never** chain entries. The chain is derived
  on demand by `odin3_srcman_chain(sm, loc, visit, user)` (§3.4), which the provenance printer
  and `odin3_diag` call.
- **Twice-included file**: two `FILE` buffers with the same given path and different `parent`s;
  the raw `loc` tells them apart, the `file` string alone does not. The same file in two
  libraries is likewise two buffers (DESIGN §4.0).

### 3.3 The 2B → 2C stream contract (AST-3)

A **stream** is the preprocessed output of one project source file together with everything it
includes; one stream becomes one `UNIT`. For each stream 2B produces: (1) expanded text;
(2) the **segment map**, `odin3_srcman_add_segment(sm, const odin3_segment *s)` with
`odin3_segment {stream, out_offset, loc}`: output byte `out_offset` onward is spelled at `loc`
onward until the next segment. A segment starts at every point where the output stops following
the spelled bytes one for one: each macro expansion, each argument **run** (pointing into its
`MACRO_ARG` buffer), each return to the enclosing buffer (the outer body for a nested
expansion, the file at the end), each line continuation dropped from a multi-line macro body,
each stripped comment, and any bytes 2B inserts (a segment with `loc` 0); (3) buffers and lines
as it reads (`odin3_srcman_add_file`, `odin3_srcman_add_expansion` with a spec struct each,
`odin3_srcman_add_line`), plus the `<command line>` buffer for macros defined by the project.

2B consumes only the text directives (`` `define `undef `ifdef `ifndef `else `elsif `endif
`include `line ``); every other directive (`` `default_nettype `timescale `celldefine
`endcelldefine `resetall `begin_keywords `end_keywords `unconnected_drive
`nounconnected_drive `pragma ``) stays in the text as a directive token line, and 2C makes a
`DIRECTIVE` node wherever it occurs (unit level or inside a module, L18/L21). Comments stay in
the text so that one place, the 2C lexer, records them in the side table with locs from the
segment map and recognises metacomments (§7); comments inside skipped `` `ifdef `` regions
and inside macro definitions (a `//` ends a macro text, §19.3.1) are not in any stream, and
2B strips a comment inside a macro argument to a space (a `loc` 0 segment), so the lexer
records only comments whose `loc` is in a `FILE` buffer. `// synopsys translate_off` …
`translate_on` (L12, D1) is a skip at the same level as `` `ifdef `` and belongs to **2B**:
the region leaves the stream and nothing in
it is recorded. The lexer converts a token's output offset to a loc with a monotone cursor
(`odin3_srcman_cursor_loc`, amortized O(1)); Bison locations are `{loc, end}` pairs. `` `line ``
never changes a loc. The `UNIT` name is the project file's given path; an included file's
tokens carry locs in that file's own buffer.

### 3.4 Printing and diagnostics

`odin3_srcman_format(sm, loc, &strbuf, style)` writes `file:line:col` of the **file location**
(§3.2) with the given path (default; tests compare paths relative to the case directory) or
the resolved path, falling back to the given name when `resolved` is 0 (`<command
line>:3:9`); 0 prints `<unknown>:0:0`. `odin3_srcman_chain(sm, loc, visit, user)` walks
outward from the token's own buffer: at a `MACRO_ARG` it reports
`in argument of macro 'NAME' at <spelling of the formal>` and continues at the run's
spelling, **one `def` step** (`def + (loc − start)`); at an `EXPANSION` it reports
`expanded from macro 'NAME' at <spelling of the current loc in the body>` (one `def` step,
always a `FILE` loc since bodies are defined in files or on the command line) and continues
at `parent`; at a `SCRATCH` it reports
`pasted by macro 'NAME'` and continues at `parent`; it stops at a `FILE`, then reports one
`included from file:line` per include level. `odin3_srcman_format_chain` prints those as
indented lines. For the §3.1 example:

```
f.v:3:19: error: … 'xx' …                    f.v:3:12: error: … '*' …
  in argument of macro 'INNER' at f.v:1:19     in argument of macro 'INNER' at f.v:1:19
  in argument of macro 'OUTER' at f.v:2:25     expanded from macro 'OUTER' at f.v:2:27
```

(`2` reads like `*` with 2:29; `+` reports at 3:12 with `expanded from macro 'INNER' at 1:21`
then `expanded from macro 'OUTER' at 2:18`.)
`odin3_diag(design, level, loc, fmt, ...)` formats `file:line:col: error|warning|info: message`
plus the chain lines and delivers them through `util/log` as one message, so a located message
is never split by another. A reader returns `ODIN3_ERR_PARSE` when it emitted an error.

### 3.5 Limits (each a located `ODIN3_ERR_PARSE`)

`ODIN3_SRC_MAX_FILE_BYTES` = 2^28 per file (so a line map, 4 bytes per line, is at most 1 GB
for a pathological file of newlines and ≈ 100 KB for mcml.v); `ODIN3_SRC_MAX_BUFFERS` = 2^24;
the 2^32 − 1 location space of AST-2 ("source space exhausted"; 2B's expansion caps come first).

## 4. AST store

### 4.1 Node record (AST-4)

```c
typedef struct odin3_ast_node {   /* 24 bytes, pagevec, IDs from 1, slot 0 reserved */
    uint8_t  kind;    /* odin3_ast_kind (§4.3) */
    uint8_t  sub;     /* per-kind sub-kind: operator, net kind, case kind, edge, direction … */
    uint16_t flags;   /* per-kind flag bits (§4.4) */
    uint32_t loc, end;/* source range (§3.1); independent locs; end 0 = unknown */
    uint32_t name;    /* strtab ID, 0 = none */
    uint32_t child;   /* first index of the node's span in the child table; for a payload kind
                         (NUMBER, REAL, and the opaque TEXT kinds) the payload index + 1 */
    uint32_t nchild;  /* span length (0 for payload kinds) */
} odin3_ast_node;
```

The field list is the contract; the layout stays behind accessors (`odin3_ast_payload`
hides the `child` reuse). **Every child ID is smaller than its parent's** (AST-5): nodes are
made bottom-up, so a forward sweep over IDs visits children before parents. A node belongs to
exactly one parent span, or to exactly one attribute span (`ATTR`), or is a root (`UNIT`, or a
subtree abandoned on a syntax error: allowed, unreachable). The child table is one `odin3_vec`
of `uint32_t` (indices survive growth; iteration is contiguous).

### 4.2 Classes and child-ordering conventions

Classes used by the slot table: **E** expression (`NUMBER REAL STRING IDENT HIER_NAME SELECT
CONCAT REPLICATE UNARY BINARY TERNARY CALL MINTYPMAX`); **S** statement (`SEQ_BLOCK` …
`NULL_STMT`, §4.3 group "Statements"); **D** declaration (`NET_DECL VAR_DECL PARAM_DECL
GENVAR_DECL EVENT_DECL`); **I** module/generate item (D plus `PORT_DECL CONT_ASSIGN ALWAYS
INITIAL INSTANTIATION GATE_DECL FUNCTION_DECL TASK_DECL SPECIFY_BLOCK DEFPARAM DIRECTIVE
GENERATE GEN_FOR GEN_IF GEN_CASE GEN_BLOCK NULL_STMT`); **U** unit item (`MODULE DIRECTIVE
UDP_DECL CONFIG_DECL`); **R** `RANGE`; **L** `LIST`; **C** `CONNECTION`; **X** any.

Conventions. (1) Positional slots first, in the listed order; an absent optional slot is ID 0,
so a slot's index never moves. (2) The tail holds items in source order. (3) A kind has a tail
or it does not; a kind needing two lists holds a `LIST` in a slot. (4) Children follow source
order; the one exception is `CASE_ITEM`, whose body precedes its match expressions so that
`default` (no expressions) and `4'd1: ;` (a `NULL_STMT` body) keep one shape. (5) A tail entry
may be 0 only where the table says `E*0` (empty system-task arguments, `$display(a,,b)`, S3).

### 4.3 Slot table (AST-6, normative)

One row per kind; `make` and `check` enforce it from a generated table (`kinds.c`, from an
`ODIN3_AST_KINDS(X)` macro). Columns: **sub** = the sub-kind enum and its count; **name** =
`req` / `opt` / `—` (must be 0); **flags** = the kind's flag names in bit order (§4.4);
**slots** = positional slots, `?` optional; **tail** = class and count (`*` ≥ 0, `+` ≥ 1,
`{a,b}`); **payload** = `NUMBER`, `REAL`, `TEXT` or —. 68 kinds; kind 0 is `NONE`.

| Kind | sub | name | flags | slots | tail | payload | rows |
|---|---|---|---|---|---|---|---|
| *Design units* | | | | | | | |
| `UNIT` | — | req (given path) | — | — | U* | — | one per stream (§3.3) |
| `DIRECTIVE` | directive kind (10): `default_nettype timescale celldefine endcelldefine resetall begin_keywords end_keywords unconnected_drive nounconnected_drive pragma` | opt (argument as written) | — | — | — | — | L18–L22; the effective setting at a point = the last preceding `DIRECTIVE` of that kind in project stream order, across UNITs and inside modules; `resetall` clears |
| `MODULE` | — | req | `MACROMODULE` | L? params (`PARAM_DECL+`), L? ports (`PORT_DECL`/`PORT_REF`*) | I* | — | M1–M9 |
| `UDP_DECL` | — | req | — | — | — | TEXT | Q7 REJECT |
| `CONFIG_DECL` | — | req | — | — | — | TEXT | S2 REJECT |
| `LIST` | — | — | — | — | X* | — | |
| *Ports and declarations* | | | | | | | |
| `PORT_REF` | — | opt (0 for `{a,b}`) | `DOTTED` (`.x(e)`, `.x()`) | E? (present for `.x(e)`/`{…}`) | — | — | M1; M6 REJECT when `DOTTED` or the name is 0 |
| `PORT_DECL` | direction (3) `input output inout` | — | `SIGNED IN_HEADER`, `DT` field (3 bits), `NET` field (4 bits, 0 = unspecified) | R? | `DECLARATOR+` (`input [3:0] a, b` is one node with two) | — | M1–M3, M12 |
| `DECLARATOR` | — | req | — | E? init/value | R* unpacked dims, outermost first | — | N7–N10, N12, P1 |
| `RANGE` | — | — | — | E msb, E lsb | — | — | N2 |
| `NET_DECL` | net kind (12) `wire tri wand wor triand trior tri0 tri1 trireg uwire supply0 supply1` | — | `SIGNED VECTORED SCALARED` | `STRENGTH`?, R?, `DELAY`? (source order, §4.2) | `DECLARATOR+` | — | N1, N11, N14–N16, N19 |
| `VAR_DECL` | var kind (5) `reg integer time real realtime` | — | `SIGNED` | R? | `DECLARATOR+` | — | N3, N4, N9, N17 |
| `PARAM_DECL` | (3) `parameter localparam specparam` | — | `SIGNED IN_HEADER`, `DT` field | R? | `DECLARATOR+` | — | P1–P4, P7 |
| `DEFPARAM` | — | — | — | `HIER_NAME` target, E value | — | — | P6; one node per pair |
| `GENVAR_DECL` | — | — | — | — | `DECLARATOR+` | — | N6 |
| `EVENT_DECL` | — | — | — | — | `DECLARATOR+` | — | N18 REJECT |
| `STRENGTH` | (2) `drive charge` | — | `S0` field (4 bits), `S1` field (4 bits): `highz weak pull strong supply` / `small medium large` | — | — | — | A3, N19, Q4 IGNORE |
| `DELAY` | — | — | — | — | E{1,3} | — | A2, T11, Q4 IGNORE |
| *Module items* | | | | | | | |
| `CONT_ASSIGN` | — | — | — | `STRENGTH`?, `DELAY`? | `NET_ASSIGN+` | — | A1–A3 |
| `NET_ASSIGN` | — | — | — | E lhs, E rhs | — | — | A1 |
| `ALWAYS` | (4) `always`, reserved `always_ff always_comb always_latch` | — | — | S | — | — | T1–T6 (T6: 2D rejects an `ALWAYS` whose S is not `TIMING_STMT`[`EVENT_CONTROL`, S]) |
| `INITIAL` | — | — | — | S | — | — | T5 (D2) |
| `INSTANTIATION` | — | req (module) | — | L? parameter `CONNECTION*` | `INSTANCE+` | — | I1–I7, P5 |
| `INSTANCE` | — | opt (check rule 4: required under `INSTANTIATION`; 0 only under `GATE_DECL`) | — | R? array | C* | — | I5, I6, Q5 |
| `CONNECTION` | — | opt (0 = ordered) | — | E? (absent: `.p()` / empty slot) | — | — | I1–I3; mixing ordered and named: 2D (I2) |
| `GATE_DECL` | gate kind (26) `and nand or nor xor xnor buf not bufif0 bufif1 notif0 notif1`, REJECT `nmos pmos cmos rnmos rpmos rcmos tran rtran tranif0 tranif1 rtranif0 rtranif1 pullup pulldown` | — | — | `STRENGTH`?, `DELAY`? | `INSTANCE+` | — | Q1–Q6 |
| `FUNCTION_DECL` | — | req | `AUTOMATIC SIGNED`, `DT` field (return) | R? return, L ports (`PORT_DECL*`), L locals (D*), S body | — | — | K1–K8 |
| `TASK_DECL` | — | req | `AUTOMATIC` | L ports, L locals, S body | — | — | K9–K12 |
| `SPECIFY_BLOCK` | — | — | — | — | — | TEXT | S1 IGNORE |
| `ATTR` | — | req (key) | `METACOMMENT` | E? value | — | — | L11, C10 (D1); reached only through the attribute map |
| *Generate* | | | | | | | |
| `GENERATE` | — | — | — | — | I* | — | G1 |
| `GEN_FOR` | — | — | — | `BLOCKING_ASSIGN` init, E cond, `BLOCKING_ASSIGN` step, I body | — | — | G2–G4 |
| `GEN_IF` | — | — | — | E, I then, I? else | — | — | G5 (`NULL_STMT` for `;`) |
| `GEN_CASE` | — | — | — | E | `CASE_ITEM+` (bodies I) | — | G6 |
| `GEN_BLOCK` | — | opt | — | `NUMBER`? index (elaborated form only: the loop iteration, §4.6) | I* | — | G2, G3, G7 |
| *Statements* | | | | | | | |
| `SEQ_BLOCK` | — | opt | — | L? locals (D*) | S* | — | T17, T18 |
| `PAR_BLOCK` | — | opt | — | L? locals | S* | — | T15 REJECT |
| `BLOCKING_ASSIGN` | — | — | — | E lhs, (`DELAY`/`EVENT_CONTROL`)? intra-assignment control, E rhs (source order: `a = #1 b`) | — | — | T7, T11 |
| `NONBLOCKING_ASSIGN` | — | — | — | as `BLOCKING_ASSIGN` | — | — | T8 |
| `PROC_CONT_ASSIGN` | (4) `assign deassign force release` | — | — | E lhs, E? rhs | — | — | T16 REJECT |
| `IF` | — | — | — | E, S, S? | — | — | C1, C2 |
| `CASE` | (3) `case casez casex` | — | — | E | `CASE_ITEM+` | — | C3–C10 |
| `CASE_ITEM` | — | — | `DEFAULT` | body: S (I under `GEN_CASE`) | E* (empty iff `DEFAULT`) | — | C4, C6, C7 |
| `FOR` | — | — | — | `BLOCKING_ASSIGN`, E, `BLOCKING_ASSIGN`, S | — | — | F1 |
| `WHILE` | — | — | — | E, S | — | — | F2 |
| `REPEAT` | — | — | — | E, S | — | — | F3 |
| `FOREVER` | — | — | — | S | — | — | F4 REJECT |
| `TIMING_STMT` | — | — | — | `DELAY`/`EVENT_CONTROL`, S? | — | — | T1, T2, T11, T12 |
| `WAIT` | — | — | — | E, S? | — | — | T13 REJECT |
| `DISABLE` | — | — | — | `HIER_NAME` | — | — | T19 REJECT |
| `EVENT_TRIGGER` | — | — | — | `HIER_NAME` | — | — | T14 REJECT |
| `TASK_CALL` | — | req (`$display` keeps `$`; a dotted `u.t` is interned whole with `HIER`) | `SYSTEM HIER` | — | E*0 when `SYSTEM`, else E* | — | K9–K11, S3, S4; E21 REJECT via `HIER` |
| `NULL_STMT` | — | — | — | — | — | — | T20 |
| *Timing and events* | | | | | | | |
| `EVENT_CONTROL` | — | — | `STAR NO_PARENS` | E? repeat count (`repeat (n) @(…)`, intra-assignment only) | `EVENT_EXPR*` (empty iff `STAR`) | — | T1–T4, T11 |
| `EVENT_EXPR` | edge (3) `none posedge negedge` | — | `AFTER_OR` (printer only) | E | — | — | T1 |
| *Expressions* | | | | | | | |
| `NUMBER` | base (4) `dec bin oct hex` | — | `SIGNED` | — | — | NUMBER | L4–L8 |
| `REAL` | — | — | — | — | — | REAL | L9 REJECT in expressions |
| `STRING` | — | req (unescaped bytes) | — | — | — | — | L10 |
| `IDENT` | — | req | — | — | — | — | L2, L3 (escaped identifiers are stored by their bytes without `\`; the printer re-escapes any name that is not a plain identifier) |
| `HIER_NAME` | — | — | — | — | (`IDENT`/`SELECT`(bit over `IDENT`))+ (one component allowed) | — | E21, G7, P6 |
| `SELECT` | (4) `bit part part_plus part_minus` | — | — | E base, E a, E? b | — | — | E14–E16, N13; chains nest, base first |
| `CONCAT` | — | — | — | — | E+ | — | E12 |
| `REPLICATE` | — | — | — | E count, `CONCAT` | — | — | E13 |
| `UNARY` | op (10) `+ - ! ~ & ~& \| ~\| ^ ~^` (`^~` stored as `~^`) | — | — | E | — | — | E1–E4 |
| `BINARY` | op (24) `+ - * / % ** == != === !== && \|\| < <= > >= & \| ^ ~^ << >> <<< >>>` | — | — | E, E | — | — | E1–E10 |
| `TERNARY` | — | — | — | E, E, E | — | — | E11 |
| `CALL` | — | req (`$`-names kept; dotted names whole with `HIER`) | `SYSTEM HIER` | — | E*0 when `SYSTEM`, else E* | — | E18–E21, K6, K7 |
| `MINTYPMAX` | — | — | — | E, E, E | — | — | A2, T11 |

Every MUST and SHOULD row maps to a kind; each IGNORE row is a kind or a `STRENGTH`/`DELAY`/
`DIRECTIVE` child that 2D drops with a located warning; each REJECT row is a kind, sub-kind or
form that 2D reports as a located error naming the construct (`odin3_ast_kind_name`). The parser
builds all of them without judging; a parse-only run is silent on semantics.

Departures from the coverage audit's §6 notes: `SELECT` chains nest; `PORT_REF`'s REJECT form is
its filled slot; `EventOr`/`ImplicitEvent` are the `EVENT_CONTROL` tail and `STAR`.
**Phase 5 appends** the SystemVerilog kinds slang needs (`RETURN`, `BREAK`, `CONTINUE`,
compound assignment, multi-dimensional packed ranges as a `LIST` of `RANGE` in the R slots,
`unique`/`priority` flags) under the same rules; kinds, subs and flags are never renumbered.

### 4.4 Flags (AST-7)

Flags are **per kind**: each kind numbers its own bits from 0 in the order its row lists them
(`ODIN3_AST_F_<KIND>_<NAME>`; fields: `ODIN3_AST_F_<KIND>_<FIELD>_SHIFT/_MASK`), so every kind
keeps headroom to 16 bits for Phase 5 and nothing is shared across kinds. The widest row uses
9 bits (`PORT_DECL`: two flags, a 3-bit and a 4-bit field). The `DT` field's values are
`none reg integer real realtime time` (`ODIN3_AST_DT_*`); the `NET` field's are 0 and the
`NET_DECL` sub-kinds. `check` rejects a set bit the kind does not define.

### 4.5 Payloads (AST-8)

```c
typedef struct odin3_ast_number {
    uint32_t width;       /* declared size, 0 = unsized */
    uint32_t nbits;       /* bits stored: what the digits spell (unsized decimal: 32 min) */
    const uint8_t *bits;  /* nbits odin3_bit values (0 1 x z), LSB first, in the AST arena */
    bool has_xz;          /* any x or z digit (C9) */
    bool has_question;    /* a ? digit was written (stored as z; the printer restores ?) */
    bool warned;          /* number_value has reported this literal's sizing warning */
} odin3_ast_number;       /* base is node.sub, signedness node.flags: no duplicates */
```

Literal text is converted once, in 2A: `odin3_ast_number_parse(ast, loc, pieces, &payload)`
takes `odin3_number_text {size, base, digits}` — three byte views, because `8 'h FF` is three
tokens with blanks or comments between (L8) — and stores the literal **as written** (digits →
bits, no sizing); `odin3_ast_number_new` takes a ready struct (adapters). The §3.5.1 rules —
zero-extend, or extend with `x`/`z` when the leading digit is one, truncate to `width` with a
warning, unsized decimals signed 32-bit — live in `odin3_ast_number_value(ast, node, arena,
&value, &is_signed)`, which writes an IR `odin3_value` of kind `BITS` with its bytes in the
caller's arena and warns once per literal (`warned`), so a generate loop does not repeat it.
`REAL` holds a `double`. **`TEXT`** holds the raw source bytes of an opaque construct
(`SPECIFY_BLOCK`, `UDP_DECL`, `CONFIG_DECL`: from the keyword to its end keyword, copied into
the arena) so these print back verbatim and the round-trip oracle holds on the specify micros.
`STRING` values are interned unescaped; an embedded NUL is a located error.

### 4.6 Parsed and elaborated forms (AST-9)

One format, two subsets, tagged on the store (`odin3_ast_form`: `PARSED`/`ELABORATED`,
fixed at create). The **parsed form** is what 2C builds: every kind. The **elaborated form** is
what the slang adapter emits and what 2D's second stage consumes (§14 Q3); `check` rule 7
enforces:

- **Absent**: `DIRECTIVE UDP_DECL CONFIG_DECL SPECIFY_BLOCK DEFPARAM GENERATE GEN_FOR GEN_IF
  GEN_CASE GENVAR_DECL DELAY STRENGTH MINTYPMAX PROC_CONT_ASSIGN WAIT DISABLE EVENT_TRIGGER
  FOREVER PAR_BLOCK EVENT_DECL`; `PORT_REF`; `VAR_DECL` of `time real realtime`;
  `PARAM_DECL` of `parameter`/`specparam`; `CALL`/`TASK_CALL` with `HIER`.
- **Resolved constants**: every `RANGE` bound, `REPLICATE` count, part-select width, `REPEAT`
  count, instance-array range, `DECLARATOR` value of a `PARAM_DECL` (all `localparam`),
  `SELECT` index inside a `HIER_NAME`, and parameter `CONNECTION` value is a `NUMBER`, `STRING`
  or `REAL` leaf. A genvar is replaced by a `NUMBER` wherever it was read.
- **Expanded hierarchy**: one `MODULE` per parameter specialization, named as IR-7 requires
  (`sub$W=8`); every `INSTANTIATION` names the specialization **and keeps its resolved
  parameters** as `CONNECTION`s (every parameter the instance's module declares, in
  declaration order, named, values resolved; for a black box with no module definition the
  overrides stay as written — ordered ones keep name 0), so IR-7's source-parameter attributes
  and the parameters of black-box and hard-cell instances (`single_port_ram`, `multiply`,
  Altera cells; I7, P5) survive; generate constructs are `GEN_BLOCK`s with non-zero names — an
  unnamed block is `genblk<n>`, and a loop iteration carries the **base name plus its index
  slot** (`g` + `NUMBER 3`, printed `g[3]`), which is what the symbol table stores too (§8) and
  what keeps it distinct from an escaped identifier whose bytes are `g[3]` (a plain name, no
  index) — so `HIER_NAME` with constant indices stays expressible (G7) as `SELECT`(bit) over
  `IDENT`; ports are ANSI: each `PORT_DECL` in the port
  `LIST` has exactly one `DECLARATOR`; implicit nets are declared; `ALWAYS` is `always` over
  `TIMING_STMT`[`EVENT_CONTROL`, S], or a Phase 5 sub.
- **Kept**: exactly one `UNIT`; functions and tasks with bodies (2D inlines), `IDENT` leaves
  (resolution is the symbol table's job, built in 2D for both forms), attributes, `INITIAL`
  (D2), tri-state forms (D3).

A store is never rewritten in place (§2); the elaborated form is a second store built with the
same builders. Which store the design holds: §9.

## 5. Builder API (AST-10)

Internal (`src/ast/ast.h`), used by 2C and 2D stage 1; the ABI mirror (§5.3) wraps it.

```c
odin3_status odin3_ast_create(odin3_design *design, odin3_ast_form form, odin3_passrun_id run,
                              odin3_ast **out);           /* strtab and srcman: the design's */
void         odin3_ast_destroy(odin3_ast *ast);

typedef struct odin3_ast_spec {      /* the record's scalar fields */
    uint8_t kind, sub; uint16_t flags;
    odin3_loc loc, end; uint32_t name, payload;
} odin3_ast_spec;

/* Appends a node whose children are ids[0..n): copies the IDs into the child table. */
odin3_status odin3_ast_make(odin3_ast *ast, const odin3_ast_spec *spec,
                            const odin3_ast_id *ids, uint32_t n, odin3_ast_id *out);

/* List building on the pending stack (Bison list rules, visitor walks). */
uint32_t     odin3_ast_mark(const odin3_ast *ast);                       /* stack height */
odin3_status odin3_ast_push(odin3_ast *ast, odin3_ast_id node);          /* NO_MEMORY possible */
odin3_status odin3_ast_make_marked(odin3_ast *ast, const odin3_ast_spec *spec,
                                   uint32_t mark, odin3_ast_id *out);    /* children = stack[mark..top] */
void         odin3_ast_unwind(odin3_ast *ast, uint32_t mark);            /* error recovery */

odin3_status odin3_ast_number_parse(odin3_ast *ast, odin3_loc loc, odin3_number_text pieces,
                                    uint32_t *payload);
odin3_status odin3_ast_number_new(odin3_ast *ast, odin3_loc loc, const odin3_ast_number *num,
                                  uint32_t *payload);
odin3_status odin3_ast_real_new(odin3_ast *ast, double value, uint32_t *payload);
odin3_status odin3_ast_text_new(odin3_ast *ast, odin3_bytes text, uint32_t *payload);
odin3_status odin3_ast_attach(odin3_ast *ast, odin3_ast_id node, const odin3_ast_id *attrs,
                              uint32_t n);
odin3_status odin3_ast_comment_add(odin3_ast *ast, const odin3_ast_comment_spec *spec);
odin3_status odin3_ast_intern(odin3_ast *ast, odin3_bytes str, uint32_t *name);   /* design strtab */
odin3_status odin3_ast_finish(odin3_ast *ast);   /* seals: sorts comments, frees scratch */
```

Rules:

- **Shape is validated at make** against the slot table: `kind` known; `sub` in range; only
  the kind's flag bits; `n` ≥ the slot count and, for kinds without a tail, equal to it; the
  tail count within its bounds; every child ID made already (hence < the new ID, AST-5), non-zero
  where its slot or tail is not optional; `name` present/absent as the table says and a strtab
  ID; `payload` non-zero exactly for payload kinds and a valid index. Child *classes* are not
  checked at make (`check` rule 2 does). Violations are `ODIN3_ERR_INVALID_ARG` with a logged
  message: builder misuse is a bug in 2C or an adapter, never user input.
- **Depth cap, builder-enforced in every build** (AST-11). The builder keeps a `uint16_t`
  height per node in a side vec during construction: `height = 1 + max(children)`, and a
  `make` that would exceed `ODIN3_AST_MAX_DEPTH` fails with a located `ODIN3_ERR_PARSE`
  ("nesting deeper than N") at `spec->loc`, so a `((((…` or a 100k-term operator chain is
  rejected with a message in Release too. `finish` records the store's maximum height and frees
  the vec (2 bytes per node, build time only).
- **Reserve before mutate.** `make` reserves one node slot, `n` child slots and one height
  entry before it writes anything (`make_marked` pops the pending stack only after that); the
  payload builders allocate the bytes first, then the record. On `ODIN3_ERR_NO_MEMORY` the
  store, the pending stack and every ID handed out are unchanged.
- **Finish.** `odin3_ast_finish` sorts the comment table by expansion loc (stable; §7), frees
  the scratch, and seals the store: a later `make`, `attach` or `comment_add` is
  `INVALID_ARG`. The read API works before and after; `comments_in` only after.
- **Ownership.** The AST owns its nodes, child table, payloads, bytes, comments and attribute
  map; nothing is freed individually; IDs are valid until `odin3_ast_destroy`. Strings live in
  the design's strtab and survive the AST (§14 Q2).
- **Attach once.** `attach` copies the `ATTR` IDs into the child table and records the span
  under the node; a second attach to the same node, or a non-`ATTR` ID, is `INVALID_ARG`. The
  parser collects every `(* *)` instance before an item into one attach.
- **Caps** (§10). Literal caps in `number_parse`/`number_new`, the text cap in `text_new`, the
  child-count cap and the depth cap in `make`, each a located `ODIN3_ERR_PARSE`. Identifier and
  string length are the lexer's to check (it holds the location); `intern` fails only with
  `NO_MEMORY`.
- Single-threaded, as IR-17.

### 5.1 How 2C uses it

A Bison semantic value is an `odin3_ast_id`; `@$` gives `{loc, end}`. A fixed-arity rule calls
`make` with a small array; a list rule does `mark` at its first item, `push` per item,
`make_marked` at the parent; nested lists are safe because LR reductions nest. Left-recursive
operator rules reduce as they go, so a 2,000-term chain does not grow Bison's stack; right-deep
forms (`else if` chains, parentheses) do, at up to ~6 stack symbols per level, so 2C sets
`YYMAXDEPTH` ≥ 8 × `ODIN3_AST_MAX_DEPTH` (≈ 4 MB of heap at the limit, `YYSTACK_USE_ALLOCA 0`)
so that the AST cap fires first for every right-recursive form, and reports a Bison overflow,
should one still occur, as the same located error. On a
syntax error the action unwinds to the enclosing mark; abandoned nodes stay as unreachable roots.
`(* *)` instances reduce to `ATTR` nodes before the item they annotate; the item's rule attaches
them. The lexer turns a metacomment into `ATTR` nodes flagged `METACOMMENT` (§7).

### 5.2 How 2D stage 1 uses it

The elaborated store is built from the parsed one with the same calls while walking it with a
cursor (§6); no clone primitive exists or is needed, since every node is rebuilt with its
resolved children.

### 5.3 C ABI mirror (the next ABI version)

The mirror follows 1D (ABI v3) exactly: every function returns `odin3_status`, results come
through out-pointers, readers are `odin3_ast_get_<field>(const odin3_ast *ast, uint32_t node,
T *out)`, a NULL handle or out-pointer and a node ID that is 0 or ≥ the store's end are
`ODIN3_ERR_INVALID_ARG` logged as `"<function>: invalid argument …"` with outputs unchanged,
strings are `const char *` owned by the design and valid until its next mutation, and the cffi
cdef block stays attribute- and preprocessor-free. Node and buffer IDs are raw `uint32_t` beside
the handle; the kind, sub-kind and per-kind flag enums and the caps move into `odin3.h`
(append-only). Builder calls with more than five parameters take a spec struct
(`odin3_ast_spec` gains `uint32_t reserved[2]`, zero; `odin3_srcfile_spec`,
`odin3_expansion_spec`). The ABI number payload takes and returns bits as a string of
`0 1 x z ?`, most significant first (as `odin3_node_get_param_text` prints `BITS`).
Design-level handles are `odin3_design_get_srcman(design, &sm)` and
`odin3_design_get_ast(design, run, form, &ast)` (both may `NO_MEMORY`/`INVALID_ARG`).
Source-manager builders are exported so an adapter copies slang's locations verbatim
(DESIGN §4.2): slang's files become `FILE` buffers, its macro expansions `EXPANSION`/
`MACRO_ARG` buffers and its pasted tokens `SCRATCH`, so provenance from SystemVerilog prints
the same chains. The Python layer gets read-side helpers only in 2A (`Design.ast()`,
`node.kind`, `node.children()`, `node.loc`).

## 6. Read API and iteration (AST-12)

Internal accessors return values (as the IR's internal accessors do; the ABI wraps them with
status): `odin3_ast_kind`, `_sub`, `_flags`, `_loc`, `_end`, `_name` (strtab ID), `_name_str`,
`_nchild`, `_child(ast, node, i)`, `_children(ast, node) → {const odin3_ast_id *ids, n}` (a view
valid until the next make), `_payload` (index), `_number`, `_real`, `_text`, `_attrs(ast, node)
→ span view`, `_node_end`, `_form`, `_run`, `_max_height`, `_kind_name`, `_sub_name`,
`_slot_count(kind)`, `_has_tail(kind)`, `_root(ast, i)`/`_root_count` (the UNITs in order).
Out-of-range IDs return `NONE`/0/empty, never fault.

**Traversal never recurses** (CLAUDE.md). Three forms: `odin3_ast_walk(ast, root, pre, post,
user)` — iterative pre/post-order over an explicit stack of `{node, next child}` frames, sized
from the store's maximum height at start so `NO_MEMORY` is reported before the first callback,
`pre` may return `SKIP`; `odin3_ast_cursor` (`init`, `next → ENTER/LEAVE`, `skip`, `free`) for
consumers that drive the stack themselves (the printer, 2D); and ID sweeps (`1..end` is
children-first by AST-5; `end..1` parents-first). `odin3_ast_parent_index_build(ast, &index)`
builds an on-demand parent array (4 bytes per node) for provenance-to-AST navigation
(AST-assisted `fsm_detect`) and `odin3_ast_parent_index_destroy` frees it.

`odin3_ast_equal(a, ra, b, rb)` compares two subtrees structurally (kind, sub, flags, name
bytes, payload values including `TEXT`; locations ignored), iteratively — the round-trip oracle
of §11. `odin3_ast_dump(ast, root, &strbuf)` writes the s-expression
`(KIND[.sub][:name] @file:line:col children…)` used by golden tests, iteratively; `odin3_ast_print`
is §11.

## 7. Attributes and comments (AST-13)

**Attributes** are `ATTR` nodes (key, optional value expression) attached to one node through
the attribute map (`u64map`: node ID → packed `{span start, count}`), so they travel with the
node, print back in place and cost nothing on nodes without them. The parser attaches where the
grammar puts them: on the `NET_DECL`/`VAR_DECL`/`PORT_DECL`/`PARAM_DECL`, the `INSTANTIATION` or
`GATE_DECL`, the `MODULE`, the statement, the `CASE`, the `FUNCTION_DECL`/`TASK_DECL`, or an
operator's `UNARY`/`BINARY`/`TERNARY` node.

**Metacomments** (D1): `// synopsys full_case`, `// synopsys parallel_case`, both at once, the
`synthesis` prefix, and the `/* */ form`, placed **after the `case (expr)` header** they modify
(Yosys's rule). The lexer records the comment and emits `ATTR` nodes flagged `METACOMMENT`; the
parser attaches them to the `CASE` whose header they **immediately follow, before its first
item** (so a nested case's metacomment never reaches the outer one; elsewhere they are plain
comments), merged with that case's `(* *)` attributes into its single attach (§5). 2E emits
D1's per-use warning when it honours one. The
printer writes a `METACOMMENT` attribute back as the metacomment after the header, so the
round trip holds.

**Flow to the IR** (applied by 2D; the rule lives here so every front end agrees): a
declaration-level attribute applies to **every declarator** of the declaration and an
instantiation-level one to **every instance**. When 2D creates an IR object from an annotated
node it copies each `ATTR` with `odin3_attr_set` (IR-10): key = the attribute name; value =
`INT 1` when the attribute has none, a `NUMBER` through `odin3_ast_number_value` (`BITS`), a
`STRING` as `STRING`, any other constant expression evaluated by 2D to `BITS`, a non-constant
value a located error. Targets: `MODULE` → the IR module; a declared net/variable/port → its
wire (and port node); an instance → its node; a statement or expression → every node the
elaboration operation that consumed it creates (they share one SOURCE record, IR-13), so
`(* keep *)` on an `assign` lands on its cells; `full_case`/`parallel_case` reach `proc` (2E)
on the process object 2D defines. Attributes on a construct 2D drops (IGNORE) are dropped with
it; the construct's own warning covers them.

**Comments** go to a side table keyed by location (DESIGN §4.1, PHASE2 #7): a vec of
`{loc, end, kind LINE|BLOCK, flags METACOMMENT, text}` with text in the AST arena, appended in
stream order and **sorted once by expansion loc at `finish`** (included files get their buffers
after the includer, so stream order is not loc order). `odin3_ast_comments_in(ast, odin3_range r)`
maps the range to expansion locs and returns the comments inside it by binary search; a
range in an includer never covers an included file's comments (they live in another buffer),
so a consumer that wants them asks per file (`odin3_ast_comments_of(ast, buffer)`). Comments
are not nodes and never reach the IR.

## 8. Symbol table (AST-14)

2A delivers the structure and lookup; 2D fills it. One `odin3_symtab` per AST, owned beside it
(§9).

**Scopes** (pagevec, IDs from 1): `{kind DESIGN|LIBRARY|MODULE|GENERATE|BLOCK|FUNCTION|TASK,
name (0 = unnamed until 2D names it), index (int32, with HAS_INDEX, for a loop iteration),
parent, node (the AST node that opened it), first child, next sibling, same-name next}`. The
tree: `DESIGN` → one `LIBRARY` per project library (so the same module name in two libraries is
two scopes, and DESIGN §4.0's top rules apply) → one `MODULE` per *elaborated module*
(parameter specialization: widths differ) → `GENERATE`/`BLOCK`/`FUNCTION`/`TASK` scopes.
Verilog has one namespace per scope for nets, variables, parameters, genvars, instances, named
blocks, functions and tasks, so one `u64map` keyed `(scope << 32) | name` → symbol serves them
all; named child scopes are found through a second map `(parent << 32) | name` → the first
same-named child, chained in index order, so `odin3_scope_child(st, parent, name, index)`
resolves `g[0]` (G7) and `odin3_scope_child(st, parent, name, NO_INDEX)` a plain block.

**Symbols** (pagevec, IDs from 1):

| field | meaning |
|---|---|
| `name`, `scope` | strtab ID; owning scope |
| `kind` | `NET VAR PARAM LOCALPARAM GENVAR INSTANCE BLOCK FUNCTION TASK MODULE EVENT` |
| `sub` | net kind or var kind (the AST enums) |
| `flags` | `SIGNED`, `IMPLICIT` (M10), `PORT`, `OVERRIDDEN` (set by `#()`/`defparam`), `ASSIGNED_BLOCKING`, `ASSIGNED_NONBLOCKING`, `ASSIGNED_CONTINUOUS` |
| `dir` | port direction or none |
| `decl`, `declarator` | AST nodes (an implicit net: `decl` = the first use) |
| `packed` | `{int32 msb, lsb}` + `HAS_PACKED` (direction preserved: `[0:3]` stays ascending) |
| `unpacked` | span into a dims vec of `{msb, lsb}` pairs, outermost first |
| `value` | `odin3_value` + signedness for parameters (payload in the symtab arena) |
| `assigns` | head/tail of an intrusive list of `{node, kind, next}`: every assigning node in source order, so T9, T10 and A4 report both locations |
| `ir_module`, `ir_wire` | the IR objects the symbol became (0 until 2D creates them); `ir_wire` is module-local and `odin3_module_compact` invalidates it, so the pass manager calls `odin3_symtab_remap(st, module, map)` after each compact of a design that keeps its symtab |

API: `odin3_scope_new`, `odin3_symbol_new` (`INVALID_ARG` on a duplicate `(scope, name)`;
2D makes the located M11 error with both `decl`s), `odin3_symtab_lookup(st, scope, name)`
walking `parent` outward in a loop, `_lookup_local`, `odin3_scope_child`,
`odin3_symbol_add_assign`, `odin3_scope_first_child`/`_next`, `odin3_scope_symbols` (ID order
through a per-scope list), `odin3_symtab_remap`, plain accessors. Fallible calls return
`odin3_status`: misuse `INVALID_ARG`, OOM `NO_MEMORY` with the table unchanged.

## 9. Lifetime (AST-15, AST-16)

**AST-15 One AST per read run.** `read_verilog` parses every Verilog file of the project (all
libraries) into one parsed store — one `UNIT` per stream, in project order, because module
resolution, macro state and directives cross file boundaries — then elaborates it (stage 1 →
an elaborated store, stage 2 → IR). Each store carries the pass-run ID of the read that built
it (`odin3_ast_run`) and its form. The design holds ASTs **per read run**:
`odin3_design_set_ast(design, ast)` attaches, `odin3_design_get_ast(design, run, form, &ast)`
finds. The store the design normally holds for a run is the one stage 2 consumed — the
elaborated store — and `odin3_prov_record.ast` of a SOURCE record made in run *R* is a node ID
in run *R*'s elaborated store (the parsed store when the read had no stage 1, as a BLIF-style
reader would); a consumer resolves it only in a store whose `run` and form match (the generation
tag of the review's I12), never by index alone. The parsed store is destroyed when stage 1 ends,
unless kept. A mixed-language project (Verilog plus SystemVerilog, or VHDL through GHDL) is two
read runs with two ASTs; provenance from each resolves in its own.

**AST-16 ASTs and symbol tables are freed at the end of their read pass** unless kept: the CLI
flag `--keep-ast` (the debug form `--keep-ast=all` also keeps parsed stores, so a run then
holds two), or any pass in the script whose
registry definition (1D's pass registry) sets `wants_ast`, makes the pass manager set
`odin3_design_set_keep_ast(design, true)` before the read. A kept AST lives until
`odin3_design_destroy`. This amends DESIGN §4.5 ("never discarded") per PHASE2 #7.

**What survives into the IR**: `odin3_srcloc`s carrying the raw `loc` in SOURCE provenance
records (§3.2; chains derived on demand through the source manager), the source manager
itself, names and string literals in the strtab, attributes copied
as IR attributes (§7), and `odin3_prov_record.ast` as above. Nothing in the IR holds a pointer
into an AST (IR-5, spec §15.1).

A parse-stage failure (syntax error, cap, OOM) destroys the partial AST and leaves the IR as it
was: the parse stage adds nothing to it (the source manager's buffers and the strtab's new
strings remain; both are harmless). An elaboration-stage failure is 2D's contract.

## 10. Errors and limits

Status use: `ODIN3_ERR_INVALID_ARG` = builder or accessor misuse (a bug in 2C/2D/an adapter,
logged); `ODIN3_ERR_PARSE` = the input is rejected, always with a located diagnostic naming the
construct or the cap; `ODIN3_ERR_NO_MEMORY` = allocation or 32-bit ID exhaustion, nothing
changed (1A convention).

**Depth measurement** (`scratchpad`, lexical proxies over `tests/micro/verilog`, the VTR
benchmarks and Koios, 2026-10-09): the deepest real expression is a left-deep `&` chain of
**2,032** terms in Koios `lenet.v` (HLS output: `~(7'd0 == r) & ~(7'd126 == r) & …`), giving an
AST depth of ≈ 2,050 with its leaf and statement nesting; the longest `else if` chain is 266
(`lenet.v`), the deepest parentheses 33 (`lenet.v`), the deepest block nesting 43
(`bwave_like.float.small.v`); classic VTR and the micros peak at ≈ 150 (`bgm.v`: an 86-operator
`assign` under 8 parentheses; `arm_core.v` 81 operators). Ten times the measured maximum is
≈ 20,500; the cap is the next power of two.

| Cap | Value | Why |
|---|---|---|
| `ODIN3_AST_MAX_DEPTH` | **32768** | ≥ 10 × the measured 2,050 (above); builder-enforced in every build (§5); an explicit stack of 32768 8-byte frames is 256 KB; `uint16_t` heights suffice |
| `ODIN3_AST_MAX_NUMBER_BITS` | `ODIN3_READER_MAX_WIDTH` (2^20) | the literal width the readers cap (PHASE2 #5), so a `1000000'b0` cannot later allocate gigabytes of pins |
| `ODIN3_AST_MAX_DECIMAL_DIGITS` | 4096 | decimal-to-binary is O(n²); 4096 digits (13.6k bits) is already absurd for a decimal |
| `ODIN3_AST_MAX_STRING_BYTES` | 2^17 | a string parameter (8 bits per byte) stays under the width cap (lexer-checked, §5) |
| `ODIN3_AST_MAX_IDENT_BYTES` | 4096 | 1364-2005 §3.7 guarantees 1024 (lexer-checked) |
| `ODIN3_AST_MAX_TEXT_BYTES` | 2^20 | an opaque `specify`/`primitive`/`config` body |
| `ODIN3_AST_MAX_CHILDREN` | 2^24 per node | a 16M-child `CONCAT` or module is a bomb; real designs peak in the thousands |
| node count, child-table length | 2^32 − 2 | ID space; memory runs out first and reports `NO_MEMORY` |
| source limits | §3.5 | |

Replication counts, loop trip counts and generate ranges are elaboration caps (2D).

**OOM policy.** Every builder reserves before it mutates and returns `NO_MEMORY` with the AST
unchanged. 2C stops at the first `NO_MEMORY` (no recovery, which would allocate again),
destroys the AST and returns `NO_MEMORY` from the pass. `walk` reports `NO_MEMORY` before the
first visit. Unit tests inject failures at every allocation point
(`odin3_util_set_alloc_fail_after`) and assert the counts are unchanged.

**`odin3_ast_check(ast, mode)`** (`src/ast/check.c`; run by `read_verilog` after each stage in
Debug builds and on demand, like IR `check`; `ODIN3_ERR_CHECK` on an error; iterative):

1. **E** every record has a known kind, a sub-kind in range, only the kind's flag bits, a
   payload exactly when the kind takes one (and a valid index), locs that decode (or are 0), and
   `loc ≤ end` when both lie in one buffer.
2. **E** the slot table holds: slot count, tail bounds, mandatory slots non-zero, each child of
   its slot's or tail's class (or exact kind), every child ID below the parent's (AST-5), a 0
   tail entry only under `E*0`.
3. **E** every node is referenced by at most one span; an `ATTR` only by the attribute map.
   **I** one count of unreachable non-root nodes.
4. **E** `name` present or absent as the table says (`req`/`—`), plus the parent-dependent
   rule the table notes (an `INSTANCE` under an `INSTANTIATION` has a name); every name a
   strtab ID.
5. **E** the store's recorded maximum height is correct and ≤ `ODIN3_AST_MAX_DEPTH`.
6. **E** after `finish`: comments sorted by expansion loc; attribute spans valid; `NUMBER`
   payload agrees with `sub`/flags (`nbits` ≤ cap; `has_xz` matches the bits).
7. **E** with form `ELABORATED`: the §4.6 subset.
8. **E** symbol table (when present): every scope's parent and every symbol's scope exist,
   AST node IDs are in range, the maps agree with the records, no duplicate `(scope, name)`.

## 11. Testing

Unit tests, one file per area, `tests/unit/test_ast_<area>.c` (Unity, ASan/UBSan):

- `srcman`: buffers of all four kinds; line maps; spelling (one step, run-in-run), expansion
  and file location of every token of the §3.1 example; nested expansion inside an include;
  a file included twice; the same file in two libraries; cross-buffer ranges (`assign `OUT =
  a;`, `W + 1` with `W` a macro) through `expansion_range` and `srcloc`; a token spanning
  segments (`` x`S `` → one identifier ending at `cursor_loc(last byte) + 1`; hex digits from
  two adjacent macros); `a/**/b` staying two tokens (the `loc` 0 space segment); a comment
  inside a macro argument not recorded; the `<command line>` buffer's `def` and its `format`
  output; the chain printed exactly (§3.4's four cases); the segment cursor; comments after an
  include; the three limits; OOM injection; decode and `cursor_loc` throughput (≥ 10M/s in
  Release, recorded).
- `build`: every kind at its minimum and maximum children; every shape rule of §5 rejected with
  `INVALID_ARG`; `make_marked`/`unwind` with nested marks; AST-5 ordering; attach once;
  `finish` sealing; the depth cap hit in a Release build with a located message; every cap of
  §10; OOM at every allocation with unchanged counts.
- `read`: accessors on out-of-range IDs; `walk` and `cursor` produce the ENTER/LEAVE sequence
  of a 20-line recursive reference kept in the test (tests may recurse; `src/` may not);
  pruning; a chain at the cap walks with the pre-sized stack; the parent index.
- `number`: every §3.5.1 case (`4'hFF` truncation, warned once; `8'bx` extension; `'hF`
  unsized; `4'sb1000`; `?`; `_`; `8 'h FF` pieces); a property test over random literals
  (parse → value → print → parse is stable); the ABI's `odin3_ast_number_new_str` bit order.
- `check`: one negative test per rule through a test-only corruption hook
  (`src/ast/ast_test.h`, hidden) as 1B does.
- `symtab`: library/module/generate-with-index scopes, outward lookup, shadowing, duplicates,
  `scope_child` with and without index, assignment lists in order, remap after compact, OOM.
- `attr`, `comment`, `dump` (prints `file:line:col`), `print`, `equal`, and the ABI mirror
  through `odin3.h` only (`test_ast_abi.c`, the 1D conventions: status, out-pointers, logged
  `INVALID_ARG`), plus the Python read helpers in `tests/tools/` (skipped without cffi).

**Verilog printer** (`odin3_ast_print(ast, root, &strbuf)`, iterative, in 2A): emits
Verilog-2005 from any subtree with canonical spacing, full parenthesisation of nested operators,
re-escaping of any name that is not a plain identifier, `?` digits restored, `REAL` as `%.17g`,
`TEXT` payloads verbatim, `METACOMMENT` attributes as metacomments, other comments omitted. Worth
its ~400 lines because 2C then tests every construct as *parse → print → parse →
`odin3_ast_equal`*: a dropped token, a wrong slot, a precedence slip or a lost flag fails without
a hand-written golden, and corpus-scale runs (every micro and VTR file) cost nothing to
maintain. 2A's own tests print hand-built subtrees against literal strings; the printer is also
the `write_verilog_ast` debugging pass.

**Benchmark** (`tests/bench/bench_ast.c`, built, not in CTest): a synthetic 2M-node AST with a
realistic mix (40% `IDENT`, 20% `BINARY`, 15% `NUMBER`, 10% `SELECT`, 15% statements and
declarations, ~1.1 children per node) measuring bytes per node (nodes + child table + payloads
+ bits, strtab excluded), build, `walk`, cursor, `check` and destroy time. Targets: ≤ 40 bytes
per node (2M nodes ≤ 80 MB); build ≤ 1 s, walk ≤ 100 ms, check ≤ 300 ms in Release on the
dev machine. When 2C lands, the same driver runs `read_verilog --parse-only` on the VTR set
smallest first and ends with **mcml.v (636 KB, 24,507 lines)**; target: AST + child table +
payloads + comments + srcman ≤ 20 MB (≈ 32 bytes per source byte; ≈ 200k nodes estimated),
parse-only ≤ 1 s. Numbers go to `docs/PHASE2.md`.

## 12. Review focus

1. **Locations through the preprocessor.** The segment map, the `MACRO_ARG` parent/def pair
   and the expansion-loc loop are where a diagnostic silently lands on the wrong column, on
   the macro definition instead of its use, or on an include's parent. Reviewers should trace one
   `` `define F(x) (x + `W) `` used inside an included file through §3.1–3.2 by hand.
2. **Slot-table drift.** A grammar action that fills `CASE_ITEM` expressions-first, or
   `FUNCTION_DECL`'s four slots out of order, produces a plausible tree 2D misreads. `check`
   rule 2 and the print round-trip catch most of it; reviewers should diff 2C's actions against
   §4.3 row by row.
3. **Literal sizing in two places.** The parser stores digits as written, `number_value`
   sizes; if 2C sizes at lex time, warnings double or vanish and the round trip breaks.
4. **Elaborated subset versus slang.** §4.6 now keeps resolved instance parameters and indexed
   `HIER_NAME`s; what it still forbids (hierarchical calls, `defparam`) must be what slang has
   resolved by the time it hands us the elaborated AST. Reviewers should walk one SystemVerilog
   module with a parameterized instance, a generate loop and a hierarchical reference through
   it.
5. **Memory creep.** The 24-byte record, strings in the design strtab and the build-time height
   vec are cheap for mcml.v; a field added "just for 2D" or comment text in the strtab moves
   the budget silently. Bytes-per-node and the strtab size after `odin3_ast_destroy` are the
   gates; reject record fields a side table or payload could hold.

## 13. Spec amendments made with this document

Under PHASE2 #7: DESIGN §4.5 ("never discarded; IR objects back-point to it" → freed after the
read unless kept, AST-16; the back-pointer is the prov `ast` field resolved by run and form) and
§4.1 (locations are `{loc, end}` offsets decoded through the source manager, not five fields on
every node). `docs/IR.md` §6 / `src/ir/prov.h`: `odin3_srcloc` gains `uint32_t loc` (the
**raw** source-manager location of the node, any buffer, 0 = none; `locs[1..]` never hold
chain entries, which `odin3_srcman_chain` derives) and the `ast` field reads "AST node ID in
the elaborated (else parsed) store of the record's run, 0 = none, meaningful while that store
is held". 1B's design
handle gains the owned source manager, the per-run AST list with symbol tables, and the keep
flag. 1D's pass registry definition gains `wants_ast`. The ABI is bumped once, to the version
after 1D's.

## 14. Open questions (decided as recommended under the overnight rule, 2026-10-10)

Each was Peter's call; the recommendation stands as the decision, the alternative is recorded.

1. **Design owns the source manager, the per-run ASTs and their symbol tables** (AST-1,
   AST-15), and `odin3_srcloc` gains the **raw** `loc` (a 1B struct change; chains are derived
   from it on demand, never stored). Recommend yes: provenance prints macro and include chains
   for the life of the design, and the run tag makes `prov.ast` safe across reads. Alternative:
   the project record owns them, with borrowed pointers and chains copied into every record.
2. **AST strings in the design strtab.** Recommend yes: names flow to IR wires and nodes
   without re-interning and the IR already keeps file paths there (IR-5); cost ≈ 1 MB of
   identifiers and literals retained for mcml.v. Alternative: an AST-local strtab plus a copy per
   name 2D hands to the IR.
3. **2D in two stages with the elaborated store as the seam** (§4.6, §9): stage 1 parsed →
   elaborated store (parameters, `defparam`, generate, constant functions); stage 2 elaborated
   → IR (signedness, widths, processes); the slang adapter enters at stage 2; the design holds
   the elaborated store. Recommend yes, fixed in the 2D spec. Alternative: one-stage 2D and a
   second elaborator over slang's output in Phase 5.
4. **`ODIN3_AST_MAX_DEPTH` = 32768, builder-enforced in every build** (§10: ≥ 10 × the
   measured 2,050 of Koios `lenet.v`, a power of two; 256 KB of explicit stack at the limit).
   Recommend yes. Alternative: 65536 for another 2× of headroom at 512 KB.
5. **Verilog printer in 2A** (~400 lines plus tests; opaque constructs print from their `TEXT`
   payload, metacomments print back). Recommend yes, for the parse-print-parse oracle 2C gets
   for free. Alternative: s-expression dump only, with hand-written goldens in 2C.
