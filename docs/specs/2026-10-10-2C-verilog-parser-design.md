# 2C — Verilog-2005 lexer and parser: design

Status: agent draft for Peter (PHASE2 #1, #3; light review). Phase 2, sub-project 2C. Spec:
`docs/DESIGN.md` §4.1, §6 step 1, §15. Inputs: PHASE2 #6 (coverage, D1–D5), #8 (the 2A AST and
its carry-overs), #9 (the 2B stream contract); the construct table in
`docs/specs/2026-10-09-2C-verilog-coverage.md` (rows cited as `L1`, `M1`, …); the AST in
`docs/specs/2026-10-09-2A-ast-design.md` (§4.3 slot table, §4.4 flags, §5 builders, §7
attributes and comments, §10 caps) and `src/ast/*.h`; the preprocessor in
`docs/specs/2026-10-10-2B-project-preproc-design.md` (§3.3/§7.8 stream contract, §7.5
pass-through directives, §7.6 `translate_off`). Each decision is numbered **VP-n**. Where this
file and the 2A slot table disagree, the slot table wins and this file is corrected.

## 1. Purpose and scope (VP-1)

2C turns the 2B stream of one project source file into one `UNIT` of the **parsed-form** AST
(2A §4.6), for every Verilog file of a read run, in one store. It judges nothing: every MUST,
SHOULD, IGNORE and REJECT row of the coverage table parses to its AST shape, and a parse-only
run is silent on semantics (2A §4.3). What a construct *means* — elaboration, the IGNORE
warnings, the REJECT errors that need a kind to be recognised (`REAL` in an expression,
`FOREVER`, `UDP_DECL`, …), signedness, widths, implicit nets, top selection — is 2D's.

| Delivered by 2C | Where |
|---|---|
| Reentrant Flex lexer over the 2B stream: tokens with `{loc, end}` ranges from the segment cursor, number pieces, escaped identifiers, keyword sets per `` `begin_keywords ``, `(* *)`, comments and metacomments into the 2A side tables, pass-through directives, opaque `specify`/`primitive`/`config` bodies | `src/frontends/verilog/verilog.l`, `lexer_support.[ch]`, `keywords.c` |
| Pure LALR(1) Bison grammar for 1364-2005 Annex A's synthesizable subset plus the REJECT forms, building the AST bottom-up through the 2A builders | `verilog.y`, `actions*.[ch]` |
| Driver: one stream → one `UNIT`; located syntax errors with recovery and an error cap | `parser.[ch]` |
| The `read_verilog` pass and the `--parse-only` CLI flag; library-file resolution (`-v`/`-y`); `duplicate_module`; the unnamed-instance rule | `read_verilog.c`, `src/passes/builtin.c`, `src/cli/main.c` |
| Build: Bison/Flex found by CMake with version floors, generated into the build tree, never committed; the lint gate unchanged | `CMakeLists.txt`, `tools/lint.sh`, `tools/check_rules.py` |
| Tests: one case per coverage row through parse → check → print → re-parse → `equal`; every micro and VTR benchmark under `--parse-only`; negatives; fuzzers; the mcml.v measurement 2A deferred | `tests/verilog/`, `tests/unit/test_verilog_*.c`, `tools/parse-check`, `tools/verilog-gen`, `tests/fuzz/fuzz_verilog.c`, `tests/bench/bench_ast.c` |

Not in 2C: the preprocessor (2B), elaboration and the symbol table filling (2D), SystemVerilog
(Phase 5: slang), VQM's primitive library (Phase 6; the VQM reader reuses this grammar).

**Odin II lineage** (PHASE2 #3). Kept from `verilog_bison.y`/`verilog_flex.l`: the token
vocabulary (`vMODULE`, `voANDAND`, …), the skeleton of the module / item / statement /
expression rules, the shape of the precedence table, `%locations`, `error ';'` recovery at
item level. Dropped: the preprocessor and `` `include `` in the lexer (2B), `ieee_filter`
(replaced by a keyword table with version masks, §2.5), `a.b.c` lexed as one identifier
(§2.4), numbers carried as strings (§2.3), the one-item `generate`, C++ in actions, every
construct the gap list names (coverage §5). The grammar is rewritten against Annex A, not
patched.

## 2. Lexer

### 2.1 Form (VP-2)

Flex ≥ 2.6.4, `%option reentrant bison-bridge bison-locations noyywrap nounput noinput
never-interactive nodefault warn 8bit`, prefix `odin3_vp_` (shared with Bison's `api.prefix`,
so `bison-bridge` finds `odin3_vp_lex`), `yyextra` = `odin3_vl_state` (the
design, the AST under construction, the segment cursor, the byte offset, the keyword-set stack,
the pending metacomments, scratch `odin3_strbuf`s for strings, comments and opaque bodies, the
error count). The stream text (2B PP-1, owned by the preprocessor until the next run) is scanned
with `yy_scan_bytes` (one copy, ≈ 0.7 MB for mcml.v: `yy_scan_buffer` would need two NULs and
a writable buffer, a change to 2B's contract for nothing). The lexer never calls `yymore` or
`unput`, and `yyless` exactly once (§2.3, with the offset put back), so the byte offset is
exact: `YY_USER_ACTION` records `tok_start = offset` and adds `yyleng`. Every byte matches a
rule (`nodefault`): a byte outside the grammar
(control characters, non-ASCII outside comments and strings) is a located error naming it
(`unexpected byte 0x..`).

### 2.2 Locations (VP-3)

`YYLTYPE` is `odin3_range {loc, end}`. A token's `loc` is `odin3_srcman_cursor_loc(cur,
tok_start)` and its `end` is `cursor_loc(tok_start + yyleng − 1) + 1` (2A AST-2: the last
byte's loc plus one, so a token spanning segments — `` x`S `` → `x_y`, hex digits from two
adjacent macros — gets the right ends). `YYLLOC_DEFAULT` gives a rule `@$ = {@1.loc, @N.end}`
and an empty rule `{@0.end, @0.end}`; every node's range is its rule's `@$`. A multi-rule
token (a block comment or an opaque body scanned line by line, §2.6, §2.8) keeps its start in
`yyextra` and takes its end from the closing rule. A token never starts on an inserted byte
(2B's `loc` 0 segments are blanks), so no token has `loc` 0; a token whose last byte is in a
`loc` 0 segment cannot occur for the same reason.

### 2.3 Numbers (VP-4)

Three tokens, so `8 'h FF` with blanks or comments between the pieces (L8) lexes like `8'hFF`:

| token | regex | notes |
|---|---|---|
| `UNSIGNED_NUMBER` | `[0-9][0-9_]*` | also the size of a sized literal and a plain decimal (L5) |
| `BASE` | `'[sS]?[bBoOdDhH]` | enters start condition `BASED`, which skips blanks and comments (comments still recorded) and must find `BASED_DIGITS` |
| `BASED_DIGITS` | `[0-9a-fA-FxXzZ?][0-9a-fA-FxXzZ?_]*` (`<BASED>`, then back to `INITIAL`) | the superset of every base's digits; the per-base check is `odin3_ast_number_parse`'s (a located `PARSE`: `'b12`) |
| `REAL_NUMBER` | `[0-9][0-9_]*\.[0-9][0-9_]*([eE][+-]?[0-9][0-9_]*)?` \| `[0-9][0-9_]*[eE][+-]?[0-9][0-9_]*` | L9; `1.` and `.5` are not reals (1364-2005 §3.5.2); value by `strtod` on the text without `_` |

In `BASED`, anything but blanks, comments and `BASED_DIGITS` — in particular `_` (`'h_F`,
PHASE2 #8 carry-over: `number_parse` stays lenient on `_`, the lexer is not) or end of input —
returns the lexer to `INITIAL` without consuming it (the lexer's one `yyless(0)`); the grammar's
digit-less alternatives `[UNSIGNED_NUMBER] BASE` then report "expected digits after `'h`" at
the `BASE` token and raise `YYERROR`, so the `_F` that follows is recovered quietly (§3.4). The
`number` rule (`UNSIGNED_NUMBER BASE BASED_DIGITS` \| `BASE BASED_DIGITS` \|
`UNSIGNED_NUMBER`) hands the three byte views to
`odin3_ast_number_parse` as `odin3_number_text {size, base, digits}` **as written** (no sizing,
2A review focus 3) and `odin3_ast_number_kind` gives the node's sub and `SIGNED`. `?` is a `z`
digit with `has_question` (L7), so `casez` patterns print back as written.

### 2.4 Identifiers, keywords, system names, strings (VP-5)

- `IDENT`: `[a-zA-Z_][a-zA-Z0-9_$]*` not in the active keyword set, and **escaped identifiers**
  `\` followed by one or more printable non-blank ASCII bytes up to the next blank, tab,
  newline or end of input (L3): the name is the bytes without the `\` (2A: the printer
  re-escapes any name that is not a plain identifier, so `\a+b ` and `\module ` round-trip,
  and `\wire ` is the identifier `wire`, never the keyword). `\` followed by a blank is a located
  error. Longer than `ODIN3_AST_MAX_IDENT_BYTES` (4096): located error, the token is dropped
  to 4096 bytes so the parse continues.
- A dotted name `a.b.c` is `IDENT '.' IDENT '.' IDENT` (Odin II lexed it whole, l:394): the
  grammar builds `HIER_NAME` (§3.6), and a named port connection `.p(e)` needs the `.` as its
  own token anyway.
- `SYSTEM_IDENT`: `\$[a-zA-Z0-9_$]+` (`$signed`, `$display`, `$clog2`, `$readmemh`, …; one token
  kind, the name kept with its `$`, 2A `CALL`/`TASK_CALL`).
- Keywords: the 1364-2005 Annex B set (§2.5), case-sensitive, from a sorted table searched by
  `bsearch` (`keywords.c`, generated from one `ODIN3_VL_KEYWORDS(X)` list with a version mask
  per word; no gperf dependency). `macromodule` is its own token (M8: `MODULE` with the
  `MACROMODULE` flag).
- `STRING`: `"` … `"` on one line with `\n \t \\ \" \ddd` (octal, 1–3 digits) unescaped by the
  lexer (L10; `STRING` is interned unescaped, 2A §4.5); any other `\c` keeps `c` with a located
  warning; a newline before the closing quote is a located error "unterminated string" and the
  string ends there; an unescaped NUL cannot occur (the stream has none) and an escaped `\000` is
  a located error (2A: an embedded NUL is rejected); longer than `ODIN3_AST_MAX_STRING_BYTES`
  (2^17) after unescaping: located error, truncated.
- Operators and punctuation: every 1364-2005 operator token (`<<<`, `>>>`, `===`, `!==`,
  `~^`/`^~` both → `XNOR`, `~&`, `~|`, `**`, `+:`, `-:`, `->`, `=`, `<=` (one token; the grammar
  tells assignment from comparison by position), `@`, `#`, `,`, `;`, `:`, `.`, `?`, `(`, `)`,
  `[`, `]`, `{`, `}`) and the attribute brackets (§2.7).
- Blanks `[ \t\r\n\f]+` are skipped.

### 2.5 Keyword sets and `` `begin_keywords `` (VP-6)

The table carries a mask per word: `1364-1995`, `1364-2001-noconfig`, `1364-2001`,
`1364-2005` (`uwire` is 2005-only; `automatic cell config design endconfig endgenerate generate
genvar incdir include instance liblist library localparam noshowcancelled pulsestyle_ondetect
pulsestyle_onevent showcancelled signed unsigned use` are 2001; `noconfig` drops the config
words). The lexer keeps a stack of active masks (initially 2005; depth cap
`ODIN3_VL_MAX_KEYWORD_DEPTH` = 64, a located error past it): `` `begin_keywords "<v>" ``
pushes `<v>` when it is one of the four (L22 SHOULD); any other version string (`1800-2005`,
…) is a located error naming it (L22 REJECT) and pushes nothing; `` `end_keywords `` pops (a
located warning on an empty stack); `` `resetall `` empties it. The `DIRECTIVE` node is built
for each of them regardless (§2.9), so the unit records what was written. A word that is a
keyword only in a newer set lexes as `IDENT` under an older set — which is the point of the
directive.

### 2.6 Comments and metacomments (VP-7)

`//` to end of line and `/* */` (scanned line by line in a start condition so `yytext` never
holds more than a line) are recorded with `odin3_ast_comment_add` (2A §7: `{loc, end, kind,
flags, text}`) **only when their `loc` is in a `FILE` buffer** (`odin3_srcman_buffer_info`;
2B replaces comments inside macro bodies and arguments by `loc` 0 blanks, so this is a
belt-and-braces check that costs one lookup per comment). Text is stored without the
delimiters; beyond `ODIN3_VL_MAX_COMMENT_BYTES` (2^20) the text is cut (the range stays
exact; no diagnostic — a side table for tooling). An unterminated `/*` is a located error at
its start. The stream never contains `translate_off` regions (2B PP-10).

**Metacomments** (L12, C10, D1; 2A §7): a comment whose text, after blanks, starts with
`synopsys` or `synthesis` is flagged `ODIN3_AST_COMMENT_METACOMMENT`. When the words after the
prefix are a non-empty subset of `{full_case, parallel_case}` (either order; `/* */` form too;
the micro and VTR forms are `// synopsys parallel_case` and `// synopsys full_case
parallel_case`), the lexer also appends `{loc, keys}` to its **pending metacomment list**. The
parser consumes it at each `CASE`/`GEN_CASE` reduction (§3.5): the entries whose `loc` lies in
`[end of the header's ')', loc of the first CASE_ITEM)` become `ATTR` nodes named `full_case`
/ `parallel_case` with the `METACOMMENT` flag and no value, merged into that case's single
attach with its `(* *)` attributes. The list is take-only: an entry no case claims stays until
`endmodule` clears the list, so an inner case (which reduces first) never steals the outer
case's metacomment and a metacomment anywhere else is a plain (flagged) comment. `ATTR` nodes
are made only when taken, so no orphan nodes result. Other `synopsys`/`synthesis` words
(`keep`, …) are flagged comments and nothing more.

### 2.7 Attributes (VP-8)

`(*` is `ATTR_OPEN` and `*)` is `ATTR_CLOSE`, except that `@*`, `@(*)`, `@( * )` and `@ *` lex
as one `AT_STAR` token (Yosys's rule; L11, T2), which is the only place `(*` can follow `@`.
`(*` cannot otherwise start a legal expression and `*)` cannot otherwise end one, so no further
disambiguation is needed. An attribute instance in the grammar is `ATTR_OPEN attr_spec
{, attr_spec} ATTR_CLOSE`, `attr_spec ::= IDENT [= constant_expression]` → one `ATTR` per spec
(name = key, `E?` value). Several consecutive instances before one node merge into one attach
(PHASE2 #8: the printer prints them merged, so the round trip holds). Where attributes may
appear and what they attach to: §3.4.

### 2.8 Opaque bodies (VP-9)

On `specify`, `primitive` or `config` the lexer enters a raw start condition that copies bytes
to the matching `endspecify` / `endprimitive` / `endconfig` word (skipping `//`, `/* */` and
`"…"` so a closing word inside them does not end the body; nested same-kind openers are not
legal and not recognised), then returns one token `OPAQUE_SPECIFY` / `OPAQUE_PRIMITIVE` /
`OPAQUE_CONFIG` whose value is the `TEXT` payload made with `odin3_ast_text_new` from the bytes
**from the keyword to the end keyword inclusive** (2A §4.5), plus, for `primitive` and
`config`, the first identifier after the keyword as the node's name (both `req`). Beyond
`ODIN3_AST_MAX_TEXT_BYTES` (2^20) the lexer reports a located error at the keyword (text_new
has no location) and drops the body; an unterminated body is a located error at the keyword.
Comments inside a body are part of the text and not recorded. S1 `SPECIFY_BLOCK` therefore
prints back verbatim (the specify micros round-trip) and Q7/S2 become `UDP_DECL`/`CONFIG_DECL`
units 2D rejects by kind.

### 2.9 Pass-through directives (VP-10)

A `` `name `` that 2B passes through (PP-8: `default_nettype timescale celldefine endcelldefine
resetall begin_keywords end_keywords unconnected_drive nounconnected_drive pragma`) is one
token `DIRECTIVE` covering the name and the rest of its line minus a trailing `//` comment and
trailing blanks: sub = the directive kind, value = the argument text trimmed (`wire`, `none`,
`1ns / 1ps`, `"1364-2005"` with its quotes, `protect`; empty for `` `resetall ``), interned as
the node's name (`opt`; cap `ODIN3_VL_MAX_DIRECTIVE_BYTES` = 4096, a located error past it).
The parser makes the `DIRECTIVE` node where the token occurs — unit level or as a module or
generate item (class `IU`); inside a statement it is a syntax error. `` `default_nettype `` with
an argument that is not a net kind or `none` is a located error (L19). Any other backtick word
reaching the lexer (2B consumed `define`/`ifdef`/`include`/`line` and errors on SystemVerilog
operators and undefined macros, PP-5/PP-9) is a located error "unknown directive".

## 3. Parser

### 3.1 Form and conflict budget (VP-11)

Bison ≥ 3.8, `%define api.pure full`, `%define api.prefix {odin3_vp_}`, `%locations` with
`YYLTYPE = odin3_range`, `%define parse.error detailed`, `%define parse.lac full` (exact
expected-token lists; the cost is a small table walk per error), `%define lr.type lalr`,
`%expect 0`, `%expect-rr 0`. Semantic values are one union of `odin3_ast_id` (nodes), `uint32_t`
(marks, strtab IDs, enum values) and `odin3_bytes` (token text views into the stream, valid for
the token's life — number pieces are the only values copied before the next `yylex`).

**LALR(1), not GLR.** The builders are stateful at reduction time (`mark`/`push`/
`make_marked` append to one pending stack and to the store, AST-5 orders IDs by creation), so
GLR's deferred and possibly discarded actions would either duplicate nodes or require
side-effect-free actions and a post-pass — the whole bottom-up design of 2A §5.1 assumes a
deterministic parse. Verilog-2005 Annex A is LALR(1) once (a) the dangling `else` is resolved by
precedence (§3.3), (b) `(*` is a token (§2.7), (c) `@*` is a token, (d) ANSI and non-ANSI port
lists are told apart by the first token after `(` (`input`/`output`/`inout`/`(*` vs `IDENT`/
`.`/`{`), (e) a declaration and a statement never start with the same token inside a block, and
(f) `expression_list` and `mintypmax` forms are separate rules (`#(1:2:3)` only under `#(`).
Yosys and Icarus both parse Verilog-2005 with LALR grammars. The budget is **zero**: the CMake
Bison invocation runs with `-Werror` (conflicts, useless rules and precedence warnings all fail
the build, `-Wcounterexamples` for the diagnosis), and a new conflict is removed by a grammar
change or an explicit, commented `%prec`, never by raising `%expect`.

### 3.2 Grammar organisation and AST construction (VP-12)

`verilog.y` is organised by Annex A section, in this order: source text and directives (A.1),
module header and ports (A.1.3–A.1.4), parameters (A.2.1), declarations (A.2), items (A.1.4),
instantiation and gates (A.3–A.4), generate (A.4.2), functions and tasks (A.2.6–A.2.7), UDP /
config / specify opaque tokens (A.5, A.7), statements (A.6), timing (A.6.5), expressions and
primaries (A.8), attributes (A.9). ~130 nonterminals; every action is **one statement** calling
a `vp_*` function in `actions_*.c` (hand-written C under the full lint gate, §6): the grammar
file holds no logic. Conventions:

- A fixed-arity node is `vp_make(ctx, kind, sub, flags, name, @$, ids…)` → `odin3_ast_make`
  (slots in table order; an absent optional slot is 0).
- A list is `mark` at the first item (the list nonterminal's empty alternative returns the mark
  as its semantic value), `push` per item, `make_marked` at the parent (nested lists are safe:
  LR reductions nest, 2A §5.1). A kind with slots **before** a tail pushes the slot IDs first
  (0 for an absent optional slot: `odin3_ast_push` accepts 0 and `make` validates), e.g.
  `SEQ_BLOCK`: push the locals `LIST` (or 0), then the statements.
- `CASE_ITEM` is the one kind whose slot (the body) comes **after** its tail in source order
  (2A §4.2 convention 4). The match expressions are pushed from a mark, the body is parsed,
  then `vp_make_case_item` copies `[body] ++ pending[mark..top]` into a scratch `odin3_vec`
  and calls `odin3_ast_make` + `odin3_ast_unwind(mark)`. This needs one 2A accessor,
  `odin3_ast_pending_span(ast, mark)` → the view `stack[mark..top]` (10 lines; §9 amendment).
- `FUNCTION_DECL`/`TASK_DECL` non-ANSI bodies interleave port and local declarations
  (`input a; reg t; input b;`): ports go on the pending stack, locals into the context's one
  scratch vec (functions and tasks never nest), then `LIST` ports, `LIST` locals, body. The
  printer writes ports then locals, which re-parses to the same two lists.
- Left recursion everywhere a list or operator chain is parsed (Bison's stack stays flat on a
  2,000-term `&` chain, 2A §10); right recursion only where the language is right-recursive
  (`else if`, nested parentheses, nested blocks), bounded by §5.
- The context `odin3_vp_ctx` (one per stream) holds the design, the AST, the lexer state, the
  scratch vec, the error count, the *error mode* flag (§3.4) and the current module's
  metacomment list.

### 3.3 Precedence and the dangling else (VP-13)

1364-2005 §5.1.2 Table 5-4, lowest first, as Bison declarations:

```
%right '?' ':'                                   /* conditional, right-associative      */
%left  OR_OR                                     /* ||                                  */
%left  AND_AND                                   /* &&                                  */
%left  '|'                                       /* binary |                            */
%left  '^' XNOR                                  /* ^  ^~  ~^                           */
%left  '&'                                       /* binary &                            */
%left  EQ NE CASE_EQ CASE_NE                     /* == != === !==                       */
%left  '<' '>' LE GE                             /* < <= > >=                           */
%left  SHL SHR ASHL ASHR                         /* << >> <<< >>>                       */
%left  '+' '-'                                   /* binary                              */
%left  '*' '/' '%'
%left  POW                                       /* ** : left, as every binary operator */
%precedence LOWER_THAN_ELSE
%precedence ELSE
```

Unary operators have no precedence declaration: as in Annex A, `expression ::= unary_operator
{attribute_instance} primary`, so a unary operator binds to a primary only (`-a ** b` is
`(-a) ** b`, `!a && b` is `(!a) && b`, `- -a` needs parentheses). `**` is **left**-associative
because Table 5-4 says every binary operator associates left to right (`2 ** 3 ** 2` = 64);
Odin II's `%right voPOWER` is a divergence from the standard, and §7's Yosys comparison checks
the choice. Concatenation, replication and the conditional are primaries or the top rule, not
operators with precedence. `%prec LOWER_THAN_ELSE` on `IF`/`GEN_IF` without `else` and
`%precedence ELSE` resolve the dangling else as shift (the inner `if` takes the `else`), which is
what Verilog, C and the printer assume (2A: the printer wraps a then-branch that would
otherwise capture an `else`).

### 3.4 Errors and recovery (VP-14)

`yyerror` emits one `odin3_diag(design, ODIN3_LOG_ERROR, @lookahead.loc, "syntax: %s", msg)` with
Bison's detailed message rewritten to name tokens as Verilog spells them (`unexpected 'endmodule',
expected ';' or ','`; the token-name table maps `ATTR_OPEN` → `(*`, `AT_STAR` → `@*`, `IDENT` →
`identifier`, …); the chain lines (`expanded from macro …`, `included from …`) come from
`odin3_diag`. Then the parse **continues in error mode**: three `error` productions —
`module_item: error ';'`, `statement: error ';'`, `description: error ENDMODULE` —
resynchronise at the next `;` or `endmodule`, Bison's three-token rule suppresses cascades, and
every `vp_*` action is a no-op returning 0 while the flag is set, so no builder is ever called
with the half-built children recovery leaves behind (a tree after an error is never used: the
pass destroys the store, 2A §9). The first error also unwinds the pending stack to 0. The run
stops at `ODIN3_VP_MAX_ERRORS` = 20 diagnostics ("too many errors; stopping"), as a compiler's
error limit does, and at the end of the stream returns `ODIN3_ERR_PARSE` when any error was
emitted. Failures raised by actions: `NO_MEMORY` aborts the parse at once (2A §10 OOM policy);
a cap (`PARSE` from `make`, `number_parse`, `text_new`, Bison's stack, §5) is reported once,
located, and aborts the parse (a cap is not recoverable input); a literal error from
`number_parse` (`'b12`, too many digits) is counted and enters error mode. Lexer errors (§2)
count toward the cap and set error mode too; the lexer keeps producing tokens so later errors
are still reported.

### 3.5 Attributes and metacomments in the grammar (VP-15)

`attrs_opt` (zero or more instances, one merged attach) precedes: the module (`MODULE`), every
module and generate item (its node: `NET_DECL VAR_DECL PARAM_DECL GENVAR_DECL EVENT_DECL
PORT_DECL CONT_ASSIGN ALWAYS INITIAL INSTANTIATION GATE_DECL FUNCTION_DECL TASK_DECL DEFPARAM
SPECIFY_BLOCK GENERATE GEN_FOR GEN_IF GEN_CASE`), every ANSI port declaration in the header
(`PORT_DECL`), every parameter in a `#( )` header, every block-item declaration, every
statement (its node, `NULL_STMT` included), and — per Annex A — a unary operator
(`UNARY`), a binary operator (`BINARY`), the `?` of a conditional (`TERNARY`); nowhere else
(a case item, a declarator, a port reference, a connection are syntax errors with an attribute).
Attributes on an `assign` are accepted (Yosys rejects them; nothing in the corpus has them).
`(* full_case, parallel_case *)` before a `case` attaches to the `CASE` like any statement
attribute; the `CASE`/`GEN_CASE` reduction then adds the pending metacomments of §2.6 into the
**same** attach (attach-once). Attribute values are constant expressions parsed by the ordinary
expression rules; 2D evaluates them (2A §7).

### 3.6 Construct → AST map (VP-16)

Grouped by coverage section; the 2A slot table (§4.3) is normative, this lists what the
parser produces for each form. "→" gives kind[sub]{flags}(slots…; tail…).

**Units and directives (L18–L22, M8, M9, Q7, S2).** A stream → `UNIT`(name = the project file's
given path; tail = `MODULE`/`DIRECTIVE`/`UDP_DECL`/`CONFIG_DECL` in source order). `macromodule`
→ `MODULE{MACROMODULE}`. Several modules per unit and per run are simply several `MODULE`s
(M9 is 2D's top selection). Opaque bodies → §2.8.

**Modules and ports (M1–M7, M12).** `module m #(…) (…); items endmodule` → `MODULE`(name;
L? params; L? ports; I* items). `module m;` → ports absent; `module m ();` → an empty `LIST`
(the printer tells them apart: `module m;` vs `module m ();`). Parameter port list → `LIST` of
`PARAM_DECL{IN_HEADER}` (one per `parameter` keyword, several declarators per keyword, `sub`
`parameter`; `localparam` in a header is accepted and stored as `localparam`, 2D decides).
**ANSI** port list → `LIST` of `PORT_DECL{IN_HEADER}`(sub = direction; `SIGNED`; `DT` = `reg`/
`integer`/`time`/`real`/`realtime` or none; `NET` = the net kind written or 0; R? range;
`DECLARATOR+`, one per name, an `E?` initialiser for `output reg q = 0`); a name without a
direction inherits the previous port's direction, kind and range as its own node
(`input [3:0] a, b` is one node with two declarators; `input [3:0] a, output b` two nodes).
**Non-ANSI** port list → `LIST` of `PORT_REF`: `a` → name a; `.x(e)` → name x, `DOTTED`, E;
`.x()` → name x, `DOTTED`, no E; `{a,b}` or `a[3:0]` → name 0 with the expression (M6: 2D
rejects `DOTTED` or name 0). The body's `input [3:0] a;` → a `PORT_DECL` **item** (IN_HEADER
clear; PHASE2 #8: a `PORT_DECL` sits in the port LIST only in the elaborated form — in the
parsed form it is an item for non-ANSI modules and a LIST entry for ANSI ones, never both).
`input reg a` / `input integer n` (M12) → `PORT_DECL[input]{DT=reg|integer}` without comment
(D5 is 2D's warning). `inout` (M7) → sub `inout`.

**Parameters (P1–P8).** `parameter [signed] [range | integer | real | realtime | time] a = e, b
= f;` → `PARAM_DECL[parameter]{SIGNED, DT}`(R?; `DECLARATOR+` each with its value E); `localparam`
→ sub `localparam`; `specparam` outside `specify` → sub `specparam` (P7 IGNORE; inside
`specify` it is opaque text). `defparam a.b.W = 8, c.W = 9;` → one `DEFPARAM`(`HIER_NAME`, E)
per pair (P6). Constant expressions (P8) are ordinary expressions; `$clog2(n)` → `CALL{SYSTEM}`;
a constant function call `f(n)` → `CALL`.

**Nets and variables (N1–N19).** `wire [signed] [range] [#delay] a = e, b [0:3];` →
`NET_DECL[kind]{SIGNED, VECTORED, SCALARED}`(`STRENGTH`?, R?, `DELAY`?; `DECLARATOR+`: name,
E? initialiser (N7), R* unpacked dims outermost first). All twelve net kinds are subs (N14–N16;
`tri0/tri1/trireg` are 2D's REJECT). `(strong0, pull1)` / `(small)` → `STRENGTH[drive|charge]`
with the `S0`/`S1` fields. `reg [signed] [range] r = v, m [0:7] [0:3];` → `VAR_DECL[reg]{SIGNED}`
(R?; `DECLARATOR+`); `integer`, `time`, `real`, `realtime` → their subs (N17 REJECT in 2D);
`event e;` → `EVENT_DECL`(`DECLARATOR+`) (N18); `genvar i, j;` → `GENVAR_DECL`. A `RANGE` is
`(E msb, E lsb)` as written — `[0:3]` stays ascending (N2). `reg a [0:3]` without a packed
range (N10) and `wire [3:0] w [1:0]` (N11), 2-D and deeper arrays (N12) are declarators with
R* dims.

**Expressions (E1–E23, L4–L10).** Literals → `NUMBER[base]{SIGNED}` (§2.3), `REAL`, `STRING`.
`a` → `IDENT`; `a[i]` → `SELECT[bit]`(base, i); `a[m:l]` → `SELECT[part]`(base, m, l);
`a[b+:w]`/`a[b-:w]` → `SELECT[part_plus|part_minus]`(base, b, w); `m[i][j]`, `m[i][3:0]` →
`SELECT` over `SELECT` over `IDENT`, base first (N13; chains nest). `a.b.c` → `HIER_NAME`(`IDENT
a`, `IDENT b`, `IDENT c`); `g[0].t` → `HIER_NAME`(`SELECT[bit]`(`IDENT g`, 0), `IDENT t`) (G7:
a non-final component may carry one bit select); a select on the **last** component wraps the
whole name: `u.w[3]` → `SELECT[bit]`(`HIER_NAME`(u, w), 3), which is how the printer prints both
(`print_hier` joins parts with `.`; `print_select` prints its base first). `{a, b}` → `CONCAT`
(E+); `{n{a, b}}` → `REPLICATE`(E count, `CONCAT`) (a replication's inner list is always a
`CONCAT`, even of one element); unary → `UNARY[op]`(E) (`^~` stored as `~^`; unary `+` kept,
E4); binary → `BINARY[op]`(E, E); `c ? a : b` → `TERNARY`(E, E, E); `f(a, b)` → `CALL`(name f;
E*); `$signed(a)` → `CALL{SYSTEM}`(name `$signed`); `u.f(a)` → `CALL{HIER}`(name `u.f` interned
whole); `$display(a, , b)` → `TASK_CALL{SYSTEM}` with a 0 tail entry for the empty argument
(E*0); `(e)` → `e` itself (parentheses make no node; the printer re-parenthesises every nested
operator, so `a + (b + c)` and `(a + b) + c` print and re-parse as their trees say).
`(1:2:3)` → `MINTYPMAX` only under `#(` (§3.1 f).

**Continuous assignment (A1–A4).** `assign [(strength)] [#d] a = e, b = f;` → `CONT_ASSIGN`
(`STRENGTH`?, `DELAY`?; `NET_ASSIGN+`(E lhs, E rhs)). The lhs is any expression (a `CONCAT`, a
`SELECT`); 2D checks it is an lvalue.

**Procedural blocks and timing (T1–T20).** `always stmt` → `ALWAYS[always]`(S) — the S is a
`TIMING_STMT` for `always @(…) …` and `always #5 …`, the bare statement for `always begin …`
(T6: 2D rejects). `initial stmt` → `INITIAL`(S). `@(posedge clk or negedge rst)` →
`EVENT_CONTROL`(E? = 0; `EVENT_EXPR[posedge]`(clk), `EVENT_EXPR[negedge]{AFTER_OR}`(rst)) —
`AFTER_OR` on every expression that followed `or` rather than `,` (printer only; preserved by
the round trip); `@*`/`@(*)` → `EVENT_CONTROL{STAR}` with no tail; `@clk` → `{NO_PARENS}` with
one `EVENT_EXPR[none]` (T4); `@(a, b)` → two `EVENT_EXPR[none]`. `#5`, `#(5)`, `#D`, `#(a+b)`
→ `DELAY`(one E); `#(1, 2, 3)` → `DELAY`(three E); `#(1:2:3)` → `DELAY`(`MINTYPMAX`) (A2, T11;
canonical print `#(5)`). `#5 stmt` / `@(e) stmt` → `TIMING_STMT`(`DELAY`|`EVENT_CONTROL`, S);
`#5;` → S absent. `a = e;` → `BLOCKING_ASSIGN`(E, 0, E); `a = #1 e;` → `(E, DELAY, E)`; `a <=
@(posedge c) e;` → `NONBLOCKING_ASSIGN`(E, `EVENT_CONTROL`, E); `a <= repeat (n) @(posedge c)
e;` → the `EVENT_CONTROL`'s E? repeat count. `begin [: name] decls stmts end` → `SEQ_BLOCK`
(name; L? locals of D — absent when none; S*) (T17, T18; declarations in an unnamed block are
parsed, 2D judges); `fork … join` → `PAR_BLOCK` (T15). `assign a = e;` inside a statement →
`PROC_CONT_ASSIGN[assign]`(E, E); `deassign a;`, `force a = e;`, `release a;` → their subs
(T16). `wait (e) stmt;` → `WAIT`(E, S?); `disable b;` → `DISABLE`(`HIER_NAME`); `-> e;` →
`EVENT_TRIGGER`(`HIER_NAME`) (T13, T14, T19). `t;` and `t(a, b);` → `TASK_CALL`(name; E*)
(K9–K11; K10 without parentheses); `u.t(a);` → `TASK_CALL{HIER}`. `;` → `NULL_STMT` (T20),
except as the `S?` of `TIMING_STMT`/`WAIT`, where `;` leaves the slot absent (matching the
printer). `@(posedge clk) x; @(…) y;` inside a body (T12) are two `TIMING_STMT`s.

**`if`, `case` (C1–C10).** `if (e) s [else s]` → `IF`(E, S, S?); `else if` → a nested `IF` in
the else slot. `case (e) items endcase` / `casez` / `casex` → `CASE[case|casez|casex]`(E;
`CASE_ITEM+`): `0, 1: s` → `CASE_ITEM`(S; E, E) (C4); `default: s` and `default s` (C7) →
`CASE_ITEM{DEFAULT}`(S) with an empty tail; `4'd1: ;` → body `NULL_STMT` (C6); `default` in any
position (C6). Metacomments and `(* full_case *)` → §3.5.

**Loops (F1–F4).** `for (i = 0; i < n; i = i + 1) s` → `FOR`(`BLOCKING_ASSIGN`, E,
`BLOCKING_ASSIGN`, S); `while (e) s` → `WHILE`(E, S); `repeat (e) s` → `REPEAT`(E, S); `forever
s` → `FOREVER`(S).

**Generate (G1–G7, N6).** `generate items endgenerate` → `GENERATE`(I*) with any number of items,
`genvar` declarations included (G1); a loop/if/case generate outside a region is the item
itself. `for (i = 0; …; …) begin : g items end` → `GEN_FOR`(`BLOCKING_ASSIGN`, E,
`BLOCKING_ASSIGN`, `GEN_BLOCK`(name g; I*)); an unnamed `begin … end` → `GEN_BLOCK` with name 0
and no index (the index slot is elaborated-form only, 2A §4.6); a bare single item as the body
→ that item. `if (e) item [else item]` → `GEN_IF`(E, I, I?) with `;` → `NULL_STMT`; `case (e) …
endcase` → `GEN_CASE`(E; `CASE_ITEM+` whose bodies are items) (G6, several expressions per
item). Declarations, `always`, instances and nested generates inside a block are ordinary
items (G3). References `g[0].t` → `HIER_NAME` (G7).

**Functions and tasks (K1–K12).** `function [automatic] [signed] [range | integer | real |
realtime | time] f; items stmt endfunction` → `FUNCTION_DECL`(name f; `{AUTOMATIC, SIGNED, DT}`;
R? return range; L ports of `PORT_DECL` (from `input [3:0] x;` items, each a node with its
declarators, `DT` for `input integer n`, K2/K7); L locals of D; S body — exactly one statement,
as Annex A says; a `begin … end` for more). The ANSI header `function [3:0] f (input [3:0] x,
input integer n);` fills the same ports `LIST` (`PORT_DECL{IN_HEADER}`); a function may have
both only in illegal code, which 2D reports. `task [automatic] t; items stmt_or_null endtask` and
`task t (input a, output [3:0] b);` → `TASK_DECL`(name; `{AUTOMATIC}`; L ports; L locals; S body
— `NULL_STMT` when empty). Timing controls and nonblocking assignments inside a function (K12)
parse with the one statement grammar; 2D rejects them. A function call is a `CALL`, a task
enable a `TASK_CALL` (§ expressions, statements).

**Instantiation (I1–I7, P5).** `m #(8, .W(4)) u1 (a, .p(b), .q(), , c), u2 [3:0] (…);` →
`INSTANTIATION`(name m; L? parameter `CONNECTION`s: name 0 + E for ordered, name + E? for
named; `INSTANCE+`: `INSTANCE`(name u1; R? array (I6); C*: `CONNECTION`(name 0, E) for an
ordered connection, `(name p, E)` for `.p(b)`, `(name q, no E)` for `.q()`, `(name 0, no E)` for
an empty positional slot)). Ordered and named connections may mix in one list (I2: 2D rejects).
An expression on any port is an expression (I3). An instance without a name is accepted by the
grammar for every module name (a UDP instance may be unnamed); the pass enforces 2A's rule 4
(§4.3).

**Gates (Q1–Q6).** `and [(strength)] [#d] [g1] [[3:0]] (y, a, b), (z, c, d);` → `GATE_DECL[and]`
(`STRENGTH`?, `DELAY`?; `INSTANCE+`(name opt; R? array (Q5); C*: one `CONNECTION`(name 0, E) per
terminal)). `buf`/`not` with several outputs (Q2), `bufif0/1`, `notif0/1` (Q3), the MOS, pass
and pull kinds (Q6; their subs exist for 2D's REJECT) all parse; `pullup (strong1) (n)` and
`pulldown` take the strength and one terminal.

**`specify`, `config`, system tasks (S1–S4).** `specify … endspecify` → `SPECIFY_BLOCK`(TEXT)
(§2.8); `config … endconfig` → `CONFIG_DECL`(name; TEXT); `$display("x", a)`, `$finish;`,
`$readmemh("f", m)` → `TASK_CALL{SYSTEM}`; `$random`, `$time` in an expression → `CALL{SYSTEM}`
(E20).

**IGNORE rows** need nothing from the parser beyond the node (`DELAY`, `STRENGTH`, `DIRECTIVE`,
`SPECIFY_BLOCK`, `PARAM_DECL[specparam]`, `VECTORED`/`SCALARED`, `INITIAL` content, `REAL`
in a delay); **REJECT rows** need the kind, sub or flag 2D reports on (`REAL` in an expression,
`PORT_REF{DOTTED}`/name 0, `PARAM_DECL{DT=real}`, `NET_DECL[tri0|tri1|trireg]`,
`VAR_DECL[time|real|realtime]`, `EVENT_DECL`, `CALL{SYSTEM}` of a name other than `$signed`/
`$unsigned`/`$clog2`, `CALL{HIER}`/`TASK_CALL{HIER}`, `ALWAYS` without a timing statement,
`PROC_CONT_ASSIGN`, `WAIT`, `EVENT_TRIGGER`, `PAR_BLOCK`, `DISABLE`, `FOREVER`, `WHILE` outside a
constant function, `GATE_DECL` of a MOS/pass/pull sub, `UDP_DECL`, `CONFIG_DECL`, an
`INSTANCE` mixing connection forms). The 2C-raised errors are lexical and structural only
(§2, §3.4, §4.3): the only semantic checks in 2C are `duplicate_module` and the unnamed-instance
rule, both of which need the whole run and nothing more.

### 3.7 Carry-overs from PHASE2 #8 (VP-17)

- **Printer canonical forms re-parse to equal trees**: merged attribute groups (§2.7); `#(5)`
  (§3.6 timing: `#5` and `#(5)` both make `DELAY`(one E)); a `METACOMMENT` attribute printed as
  `// synopsys …` after the `case (…)` header lands in `[header end, first item)` and is
  re-attached (§2.6); a system name printed raw (`$foo` is one `SYSTEM_IDENT`); normalised
  literal digits (lower case, `'sd5` as a plain signed decimal — `number_parse` of `5` gives
  `DEC` + `SIGNED`, the same node as `'sd5`); `?` digits restored; escaped names re-escaped
  (§2.4). The §7 round-trip test covers every row, so a printer form the parser cannot read
  fails there, not in 2D.
- **Derived check rules the builders must satisfy**: the `DT`/`NET`/strength fields hold enum
  values (the actions map keywords to the enums, never store token codes); LIST contents per
  slot (`MODULE` params `PARAM_DECL`, ports `PORT_DECL`/`PORT_REF`, `INSTANTIATION` params
  `CONNECTION`, function/task ports `PORT_DECL`, locals D, block locals D); a `CASE_ITEM` body is
  S under `CASE` and I under `GEN_CASE` (two item rules, one per parent); `CASE_ITEM`'s tail is
  empty exactly when `DEFAULT` is set and `EVENT_CONTROL`'s exactly when `STAR` is; a
  `HIER_NAME` part is an `IDENT` or a bit `SELECT` over an `IDENT` (§3.6); no `UNIT` in a span;
  no `GEN_BLOCK` index in the parsed form; `SELECT`'s third slot present exactly for the three
  part forms. `odin3_ast_check` FULL runs on every store 2C builds in Debug (§4.2) and in every
  test, so a drift is a test failure, not a 2D surprise.
- **Rule 4 (unnamed instances)**: §4.3 — the store keeps every `UDP_DECL` of the run and the
  pass errors on an unnamed instance of anything else.
- **`PORT_DECL` position**: §3.6 modules.

## 4. The `read_verilog` pass

### 4.1 Interface (VP-18)

`read_verilog [--parse-only] [<file>…]`, registered in `src/passes/builtin.c` (`docs/PASSES.md`
row added). Without files it reads the project record's design files of language `VERILOG` in
listing order (PRJ-16; a finished record is required: "no project read" otherwise, and "no
Verilog files" when there are none); with files, each path is read as a design file with the
record's defines/incdirs when a record exists and none otherwise (the "one-file project" of
AST-15). A `SYSTEMVERILOG` or `VHDL` file in the record is `unsupported_language` (2B §8); a
missing or unreadable file is the preprocessor's located error (PP-1). CLI sugar in
`src/cli/main.c`: `odin3 --parse-only a.v b.v` ≡ `-p "read_verilog --parse-only a.v b.v"`, and
with `--project`/`-f`, `read_project …; read_verilog --parse-only` is appended; `--parse-only`
with a script is an error ("use the pass option").

### 4.2 Steps (VP-19)

1. `odin3_ast_create(design, PARSED, ctx->run, &ast)`; `odin3_pp_create` from the record
   (defines, incdirs; PP-1).
2. For each design file, in order: `odin3_pp_run` → stream; `odin3_verilog_parse_stream(ctx,
   ast, &stream, &unit)` → one `UNIT` (also on a stream with errors, until the cap); the
   per-stream lexer and parser contexts are created and destroyed here. A `NO_MEMORY` or a
   cap stops the run at once (2A §10 OOM policy).
3. **Library resolution** (DESIGN §4.0; `-v`/`libfile`, `-y`/`libdir`, `libext`): collect the
   module and UDP names defined and the module names instantiated so far; while some
   instantiated name is undefined: if any `libfile` is still unread, read **all** libfiles now
   (each as a stream → `UNIT`; Verilog-XL semantics: a `-v` file's modules are all available,
   first file wins); otherwise look for `<dir>/<name><ext>` over `libdirs` in order and `libext`
   in order (default `.v`), read the first regular file found as a stream, and repeat (a library
   file may instantiate further modules). What stays undefined is left for 2D (I7 black box or
   `unresolved_module`). Library `UNIT`s are appended after every design `UNIT`, and the store
   records where they begin (`odin3_ast_library_units_from`, a `uint32_t` on the store: §9
   amendment) so 2D never picks a library module as top and lets a design unit shadow a library
   module (a library module with a design unit's name is dropped with an info line; a second
   library definition is ignored, first file wins).
4. **`duplicate_module`** (M11, 2B §8): two design units defining one module or UDP name →
   located error at the second definition, names [the module]; counts toward the error cap.
5. **Unnamed instances** (2A rule 4): every `INSTANCE` with name 0 under an `INSTANTIATION`
   whose module name is not a `UDP_DECL` of this store → located error "instance of 'm' has no
   name (only gate and UDP instances may be unnamed)". 1364-2005 §12.1.2 requires the name for
   modules, so this rejects only illegal code; a UDP from a library file is a `UDP_DECL` in the
   store (step 3); a UDP that exists nowhere is an undefined module and errors here, which is
   right for a REJECT construct. Steps 4–5 are one ID sweep each.
6. Any error so far: destroy the store, return `ODIN3_ERR_PARSE` (the IR is untouched, 2A §9).
7. `odin3_ast_finish`; in Debug (or `--check`), `odin3_ast_check(ast, FULL)` — a violation is
   `ODIN3_ERR_CHECK`, a 2C bug, never user input. Log `read_verilog: N units (L library), M
   nodes, K comments, B bytes, T ms`.
8. `--parse-only`: `odin3_design_set_ast` hands the store to the design as the run's store (the
   "parsed store when the read had no stage 1" of AST-15) and it stays held until
   `odin3_design_destroy` whatever the keep level, so `write_verilog_ast`, the bench and the
   tests can read it; return OK. Otherwise call the elaboration entry `odin3_elab_run(ctx,
   design, ast)` (2D's; the parsed store then follows AST-16: freed unless `--keep-ast=all`);
   until 2D lands the entry is a stub that logs "elaboration is not available yet (2D)" and
   returns `ODIN3_ERR_INVALID_ARG`, so no script silently passes.

`preprocess` (PRJ-17) stays the preprocessor's seam; `read_verilog --parse-only` plus
`write_verilog_ast` is the parser's.

## 5. Limits (VP-20)

Each cap is a named constant, a **located** `ODIN3_ERR_PARSE` naming the cap, and has a test
that hits it through a test-only hook that lowers it (`odin3_verilog_test_set_limits`, hidden
in `parser_test.h`, as 2A/2B do).

| Cap | Value | Where | Why |
|---|---|---|---|
| `ODIN3_VP_YYMAXDEPTH` (`YYMAXDEPTH`) | 8 × `ODIN3_AST_MAX_DEPTH` = 262144 states | Bison, heap (`YYSTACK_USE_ALLOCA 0`), `YYINITDEPTH` 256; read through a function so the hook can lower it | 2A §5.1: a right-recursive form pushes ≤ 6 symbols per nesting level (an `else if` level: `if ( expr ) stmt else` ≈ 6), so the AST depth cap (height 32768) fires first on every form that creates nodes; nested parentheses create no nodes and are bounded only here (262144 `(`s). ≈ 6 MB at the limit (state + value + location per entry). Bison's "memory exhausted" is reported as a located error at the current token: "nesting deeper than the parser allows (262144)" |
| `ODIN3_AST_MAX_DEPTH` | 32768 (2A) | `odin3_ast_make`, located at the node | a 32,769-term operator chain or `else if` chain (10 × the measured maximum, 2A §10) |
| `ODIN3_AST_MAX_CHILDREN` | 2^24 (2A) | `make` | a 16M-element concatenation or module |
| `ODIN3_AST_MAX_NUMBER_BITS`, `_DECIMAL_DIGITS` | 2^20, 4096 (2A) | `number_parse` | literal bombs |
| `ODIN3_AST_MAX_IDENT_BYTES` | 4096 (2A, lexer-checked) | §2.4 | 1364-2005 guarantees 1024 |
| `ODIN3_AST_MAX_STRING_BYTES` | 2^17 (2A, lexer-checked) | §2.4 | a string parameter stays under the width cap |
| `ODIN3_AST_MAX_TEXT_BYTES` | 2^20 (2A) | §2.8, checked by the lexer before `text_new` | an opaque body |
| `ODIN3_VL_MAX_COMMENT_BYTES` | 2^20 | §2.6, text cut, no error | a side table for tooling |
| `ODIN3_VL_MAX_DIRECTIVE_BYTES` | 4096 | §2.9 | a directive argument is a few words |
| `ODIN3_VL_MAX_KEYWORD_DEPTH` | 64 | §2.5 | `begin_keywords` nesting |
| `ODIN3_VP_MAX_ERRORS` | 20 | §3.4, per run | a compiler's error limit; also bounds the diagnostic volume a fuzzer can produce |
| token count, node count | none | — | bounded by the stream cap (2B: 2^28 bytes) and the ID space (2A: `NO_MEMORY`) |

Everything else is bounded by these: the pending metacomment list by the comment count, the
scratch vec by the longest case-item list (≤ `MAX_CHILDREN`), the lexer's strbufs by the
string/comment/text caps, Flex's `yytext` by one line (comments and opaque bodies are scanned
line by line) or one token (a token is at most the stream). Memory for mcml.v is measured in
§7.6 against 2A's 20 MB budget.

## 6. Generated code and the lint gate (VP-21)

**Build.** `CMakeLists.txt`:

```cmake
find_package(BISON 3.8 REQUIRED)        # parse.error detailed, parse.lac, %precedence, -Wcex
find_package(FLEX 2.6.4 REQUIRED)       # reentrant + bison-bridge without the 2.6.0–2.6.3 bugs
set(_gen ${CMAKE_CURRENT_BINARY_DIR}/generated/verilog)
bison_target(verilog_parser src/frontends/verilog/verilog.y ${_gen}/verilog_parser.c
             DEFINES_FILE ${_gen}/verilog_parser.h
             COMPILE_FLAGS "-Wall -Wcounterexamples -Werror")
flex_target(verilog_lexer src/frontends/verilog/verilog.l ${_gen}/verilog_lexer.c
            DEFINES_FILE ${_gen}/verilog_lexer.h COMPILE_FLAGS "--warn")
add_flex_bison_dependency(verilog_lexer verilog_parser)
add_custom_target(verilog_generated
                  DEPENDS ${BISON_verilog_parser_OUTPUTS} ${FLEX_verilog_lexer_OUTPUTS})
add_library(odin3_verilog_gen OBJECT ${BISON_verilog_parser_OUTPUTS} ${FLEX_verilog_lexer_OUTPUTS})
target_include_directories(odin3_verilog_gen PRIVATE src include ${_gen})
target_link_libraries(odin3_verilog_gen PRIVATE odin3_flags)
target_compile_options(odin3_verilog_gen PRIVATE ${ODIN3_GENERATED_WNO})   # see below
```

Ubuntu 24.04 (CI) ships Bison 3.8.2 and Flex 2.6.4; the dev machine has the same. `bison flex`
join the apt lines of both CI jobs. The generated files live **only under
`build/<preset>/generated/`** and are never committed: committing them would tie the tree to one
Bison/Flex version, bloat diffs and need lint exclusions; generating them costs one second and
two packages every developer has. `odin3_core` and `libodin3` add `$<TARGET_OBJECTS:
odin3_verilog_gen>`; `actions*.c`, `lexer_support.c` and `parser.c` include the generated
headers from `${_gen}`.

**The narrow exclusion, by construction.** The gate's file lists come from `git ls-files`
(`tools/lint.sh`), `lizard` scans `src/`, and clang-tidy's header filter is anchored on
`include|src|plugins|tests/unit`, so the generated directory is invisible to clang-format, the
rules script, clang-tidy, cppcheck and lizard without any change to their configuration, and
nothing hand-written can hide there (CMake writes only the four Bison/Flex outputs into it).
The one compiler-level concession is `ODIN3_GENERATED_WNO`, applied to `odin3_verilog_gen` and
to nothing else: `-Werror`, `-Wall`, `-Wextra` stay; the list holds exactly the `-Wno-*` flags
the pinned skeletons need, each with a CMake comment naming the generated construct (expected:
`-Wno-conversion -Wno-sign-conversion -Wno-sign-compare` for Flex's `yy_size_t`/`int` buffer
arithmetic, `-Wno-unused-parameter` for Flex's reentrant stubs, `-Wno-missing-prototypes` for
Bison's internal helpers under `api.pure`; the plan measures the minimal list and records it).
Because the generated translation units also compile the `%{ %}` prologues and the actions,
three rules keep hand-written code out of them: (1) prologues contain `#include`s and the
`YY_USER_ACTION`/`YYLLOC_DEFAULT`/`YYMAXDEPTH` macros only; (2) every action is one statement,
a call into `actions*.c`/`lexer_support.c`; (3) `tools/check_rules.py` gains `src/*.y` and
`src/*.l` in its file list (the existing exit/abort/goto rules apply as they are, since the
scanner ignores comments and strings) plus one rule for those two extensions: no rule action
spans more than three lines (a brace-depth scan of the rules section, ≈ 30 lines of Python with
a unit test in `tests/tools/`). clang-format does not read `.y`/`.l`; the actions are too short
to need it. clang-tidy on `actions*.c` needs the generated header: `tidy_step` in
`tools/lint.sh` runs `cmake --build build/lint --target verilog_generated` after configuring
(a second or two; `cmake --build` of the two outputs only). No `.clang-tidy` check, no lizard
threshold and no `NOLINT` changes (CLAUDE.md rule 6).

## 7. Testing

All tests run under ASan/UBSan in Debug; every store built in a test passes `odin3_ast_check`
FULL. CTest labels: `unit`, `verilog` (the corpus tests), `tools`.

### 7.1 Unit tests (Unity)

- `test_verilog_lexer.c`: every token class; numbers (`'h_F` located error at the `_`; `8 'h FF`
  three tokens with a comment between; `?` digit; `'b12` through `number_parse`; reals incl.
  `1e3`, and `1.`/`.5`/`a.5` not reals); escaped identifiers (`\a+b `, `\module `, `\` + blank
  error, 4097 bytes); keyword sets (`generate` is an identifier under `"1364-1995"`, `uwire`
  under `"1364-2001"`, unknown version error, `end_keywords` on an empty stack, `resetall`);
  strings (every escape, `\000` error, unterminated, 2^17 + 1 bytes); `(*`/`*)`/`@*`/`@(*)`/
  `@( * )`; comments to the side table with exact ranges, a comment after an `` `include `` and
  one inside a macro argument (not recorded); every metacomment form and the pending list;
  directives with a trailing comment and over the cap; opaque bodies with `endspecify` inside a
  comment and a string, unterminated, over the cap; locations of a token spanning segments
  (`` x`S ``, hex digits from two macros) and of a token after a `loc` 0 blank; unknown
  directive and unexpected byte errors; OOM at every allocation.
- `test_verilog_parser.c` (+ `_decl`, `_stmt`, `_expr` for the 60-line rule): hand-checked
  `odin3_ast_dump` output for each shape rule of §3.6 and §3.7 — `CASE_ITEM` order, the four
  `FUNCTION_DECL` slots with interleaved items, `EVENT_CONTROL` flags, `PORT_DECL` flags and
  fields for every direction/type/net combination, ANSI vs non-ANSI, shared-range ports,
  `SELECT` chains, the three `HIER_NAME` forms, `REPLICATE` over a one-element `CONCAT`,
  `CONNECTION` forms incl. the empty slot and `.q()`, instance and gate arrays, every delay form,
  intra-assignment controls, `#5;` absent S, every generate form, `module m;` vs `module m ();`,
  attribute placement (every allowed place; a disallowed one is a syntax error) and merging,
  metacomments on nested cases (inner and outer each get their own), precedence (`a + b * c`,
  `a ** b ** c` left, `-a ** b`, `a ? b : c ? d : e` right, `a < b == c`, `!a && b`, `a & b | c`,
  `~^` and `^~` → one sub), dangling else (`if (a) if (b) x; else y;`), the opaque units; error
  recovery (three errors in one module → three diagnostics with the right locations; the cap
  stops at 20; the store is destroyed; no `INVALID_ARG` is logged in error mode, checked with a
  counting log sink); `YYMAXDEPTH` lowered by the hook → a located error on nested parentheses;
  the AST depth cap on a 33k-term chain; every §5 cap; OOM at every allocation (counts
  unchanged, `NO_MEMORY` returned).
- `test_read_verilog.c`: the pass through `odin3_pass_run` on a temporary project: project
  files vs direct paths; `--parse-only` store held and printable by `write_verilog_ast`; without
  the flag the 2D stub error; `duplicate_module` located at the second definition; the
  unnamed-instance rule (unnamed UDP instance passes, unnamed module instance errors, a UDP in
  a `-v` file passes); library resolution: `-v` first file wins, `-y` with two `libext`s, a
  library module instantiating another, a design unit shadowing a library module (info), a
  library module never a top (checked through `library_units_from`); `unsupported_language`;
  a missing file; the error count across files; `odin3_pp_files_read` equals the corpus's
  `resolved.read` for the `lib_v_y` case.

### 7.2 Coverage rows (`verilog_coverage`)

`tests/verilog/coverage/<row>_<slug>.v`, one file per row of the construct table (159, the 2B
rows L13–L17 included: they run through the pass, so the preprocessor is exercised), each a
complete module using the construct in every variant the row names, hand-written now from the
probe descriptions in the coverage audit. The driver `tools/parse-check/odin3-parse-check
[--write] <file>…` (C, beside `blif-roundtrip`; links `odin3_core`) runs per file: parse
(through `read_verilog --parse-only` on a one-file project) → `check` FULL → `odin3_ast_print` →
parse the printed text → `check` FULL → `odin3_ast_equal` on the `UNIT`s → compare
`odin3_ast_dump` against `<row>.ast` when it exists (`--write` creates or refreshes it; the
`.ast` goldens are reviewed by hand against the slot table once and then guard slot drift that a
stable round trip would not catch: a wrong-but-consistent slot order prints and re-parses
equal). Exit 0/1/2 like the other tools. CTest runs the set smallest file first.

### 7.3 Negative tests (`verilog_reject`)

`tests/verilog/reject/<case>.v` with `<case>.err`: the expected `file:line:col: error:` prefix
(relative to the case directory, `--diag-paths resolved`) and a message substring; the
driver runs `odin3 --parse-only` and requires a non-zero exit, the located line and that no
`INVALID_ARG` or check failure was logged. 2C-raised cases: a syntax error of each `error`
production's kind (item, statement, header) with the expected-token text; `'h_F`; `'b12`;
unterminated string, comment, attribute, opaque body; `\` + blank; bad escape; unknown
`begin_keywords` version; `default_nettype foo`; unknown directive; unexpected byte; every §5
cap through a small input where possible (identifier, string, directive, keyword depth) and
through the hook otherwise (`YYMAXDEPTH`, errors cap); `duplicate_module`; an unnamed module
instance; a `DIRECTIVE` inside a statement. For the 24 REJECT rows the construct **parses**:
their coverage files in §7.2 carry an `.ast` golden showing the REJECT form (so 2D has the exact
tree to report on), and `tests/verilog/reject-2d/<row>.v` + `.err` hold the located error 2D
must produce (the construct's name, as the audit's verdict column says), registered as a CTest
that is skipped until `odin3_elab_run` exists — the handover list for 2D.

### 7.4 Corpus (`verilog_micro_parse`, `verilog_vtr_parse`)

`tools/parse-check/parse-corpus` (Python, stdlib, `ruff`/`mypy --strict` clean, its own unit
test over a fake driver): every `.v` under `tests/micro/verilog` (539 files) sorted by size,
smallest first (memory rule), each through `odin3-parse-check` (parse, check, print, re-parse,
equal — no `.ast` goldens for the corpus); a file is expected to fail only when listed in
`tests/verilog/micro_expect_fail.txt` with a one-line reason (micros that Parmys itself rejects
as syntax — the PHASE2 #4 exceptions — the two preprocessor micros 2B declares divergent, files
needing a `.vh` the micro tree lacks; the `syntax/` directory is Odin II's positive syntax set
and must parse), and an unlisted failure or a listed file that passes fails the test (the list
must
shrink, never rot). `verilog_vtr_parse` does the same for `$VTR_ROOT/vtr_flow/benchmarks/
verilog/*.v` (31) and `koios/*.v` (33), largest last (mcml.v, arm_core.v), registered only when
`VTR_ROOT` is set (local and nightly; CI has no VTR checkout). A corpus run is the exit
evidence: every micro and VTR benchmark parses with `--parse-only`, and the printed form
re-parses equal.

### 7.5 Fuzzing

- **Crash fuzz** (`tests/fuzz/fuzz_verilog.c`, libFuzzer under clang with `-DODIN3_FUZZ=ON`,
  as 2B's `fuzz_pp.c`; a 10 s smoke run in CTest, long runs local): the first input byte selects
  the mode — **raw bytes** fed through the preprocessor and parser, or **token stream**: each
  further byte indexes a 64-entry alphabet of token spellings (keywords, operators, `IDENT`s
  from a small pool, literals, `(*`, `@*`, `//` comments, directives) rendered with blanks,
  so the lexer always succeeds and the parser's error paths, recovery and the pending-stack
  discipline are exercised. Invariants on every run: status `OK` or `PARSE` (`NO_MEMORY` only
  under injected failure); on `OK`, `check` FULL passes and print → re-parse → `equal` holds;
  on `PARSE`, at least one diagnostic with a non-zero loc was emitted, at most 20, and no
  `INVALID_ARG` or check failure was logged; no leak, no hang (every cap lowered by the hook so
  the worst case fits libFuzzer's RSS and time limits). Seeds: the coverage files and the
  micros.
- **Grammar-based generation against Yosys** (`tools/verilog-gen/`, Python, stdlib, seeded):
  `verilog_gen.py` generates random **valid** modules over the MUST and SHOULD rows (every name
  declared before use, ranges and widths consistent, weights favouring the common forms;
  attributes, metacomments, every number form, generate and function forms included);
  `compare.py` runs `odin3 --parse-only` and `yosys -q -p "read_verilog <f>"` on each and
  classifies: both accept (expected); **odin3 rejects, Yosys accepts** — a 2C bug, the test
  fails; odin3 accepts, Yosys rejects — reported with the file (Yosys is stricter on a few
  legal forms, e.g. attributes on `assign`, and 2C is lenient by decision D5), not a failure;
  both reject — a generator bug, fails. A **mutation** mode drops, duplicates or swaps one
  token of a valid file and expects a located 2C error whenever Yosys rejects (an accepted
  mutant that Yosys rejects is reported, since `read_verilog` also elaborates and may reject
  semantically). CTest `verilog_gen_vs_yosys` runs 200 seeds when `ODIN3_YOSYS` is found (CI
  installs Yosys already), the nightly workflow 5,000; a unit test in `tests/tools/` checks the
  generator's own well-formedness claims (every generated file passes `odin3 --parse-only`'s
  oracle-free part: parse, print, re-parse, equal).

### 7.6 The mcml.v measurement (2A Task 7's deferral)

`tests/bench/bench_ast.c` gains `--parse <file>…` (built, not in CTest; Release): for each file,
smallest first and ending with `mcml.v` (636 KB, 24,507 lines), `read_verilog --parse-only`
on a one-file project, reporting nodes, comments, `odin3_ast_bytes_reserved` (AST + child table
+ payloads + comments + arena), the source manager's bytes, the strtab growth, parse wall time
(preprocess and parse separately, 2B's §10.6 numbers beside them), `check` FULL time and peak
RSS, against 2A's targets: **mcml.v AST + srcman ≤ 20 MB and parse-only ≤ 1 s** (2A §11; the
estimate was 9.5 MB for 200k nodes), plus the whole VTR set in ≤ 5 s. The output goes verbatim
into `docs/PHASE2.md` under 2C's decisions-log entry, replacing 2A's "an estimate until 2C's
`--parse-only` run"; a missed target is investigated and reported, never relaxed.

## 8. Review focus (the five likeliest failure modes)

1. **Slot drift that round-trips.** An action that fills `CASE_ITEM` expressions-first,
   `FUNCTION_DECL`'s lists swapped, `SELECT`'s `a`/`b` reversed for `-:`, or `AFTER_OR` on the
   wrong `EVENT_EXPR` can print and re-parse to an *equal* tree while being wrong for 2D. The
   `.ast` goldens of §7.2 and `check` rule 2 are the guards; reviewers should diff
   `actions*.c` against the 2A slot table row by row, as 2A's review focus 2 asked.
2. **Locations one token off.** `YYLLOC_DEFAULT` for empty rules, the end of a token spanning
   segments, the comment range for a line-by-line block comment, and the `[header end, first
   item)` window for metacomments each have a plausible off-by-one that no corpus file reveals.
   The lexer unit tests pin exact `{loc, end}` pairs through macros and includes; reviewers
   should trace `` `define F(x) (x + `W) `` used after an include by hand, as 2A §12.1 asked.
3. **Error mode that is not inert.** After a syntax error an action that still calls a builder
   (a list rule's `push`, an `attach`) produces an `INVALID_ARG` log line — a "bug" report on
   user input — or a corrupt pending stack. The fuzz invariant (no `INVALID_ARG` on any `PARSE`
   run) and the counting log sink in the recovery tests exist for this; reviewers should check
   that every `vp_*` entry tests the flag first and that `yyerror` cannot run before the context
   is complete.
4. **A grammar that accepts less than Annex A.** Each dropped alternative (a `localparam` in a
   header, `default` without a colon, an attribute before a block-item declaration, an instance
   array, `@ident`, `#(a+b)`, a nested `` `begin_keywords `` set) is a located syntax error on
   legal code that the corpus never exercises. The coverage files (§7.2) list the variants per
   row and the Yosys comparison (§7.5) samples the rest; reviewers should read `verilog.y`
   beside Annex A section by section.
5. **Caps checked after the damage.** A `YYMAXDEPTH` reported as an unlocated "memory
   exhausted", a string unescaped into a strbuf before its length is checked, a `text_new` call
   whose `PARSE` carries no location, or a comment accumulated without the byte cap turns a
   bomb into an OOM — which kills the WSL VM (memory rule). Every cap test in §7.1 and the fuzz
   RSS limit are the guards; reviewers should find the check-before-allocate for each row of §5.

## 9. Notes for other documents and amendments

- **2A amendments** (`docs/specs/2026-10-09-2A-ast-design.md` §5, §9; `src/ast/ast.h`): (a)
  `odin3_ast_pending_span(const odin3_ast *ast, uint32_t mark)` → `odin3_ast_span` of the
  pending stack from `mark` (a view valid until the next push/make; empty for a mark at or above
  the top), for §3.2's `CASE_ITEM` action; (b) a `uint32_t library_units_from` on the store
  (`odin3_ast_set_library_units_from`/`odin3_ast_library_units_from`; 0 = none), set by the
  pass in §4.2 step 3, so 2D's top selection and module lookup know which `UNIT`s came from
  `-v`/`-y`; (c) the sentence "a parse-only read holds its parsed store as the run's store until
  the design is destroyed" added to AST-15/16.
- **2B**: nothing changes; the stream contract is consumed as written. The `.v` cases of the
  corpus harness that need the front end (2B §10.2 `--with-frontend`) can run once this lands
  (`duplicate_module`, `unknown_top` after 2D).
- **`docs/PASSES.md`**: the `read_verilog` row. **`docs/PHASE2.md`**: 2C's decisions-log entry
  with VP-11 (LALR, zero budget), VP-13 (`**` left-associative per Table 5-4, Odin II differs),
  VP-14 (error cap 20), VP-21 (generated into the build tree; `ODIN3_GENERATED_WNO` list as
  measured), the §7.6 numbers, and the divergence list: attributes on `assign` accepted, `input
  reg` parsed without comment (D5 is 2D's), `localparam` accepted in a parameter port list.
- **`docs/DESIGN.md` §4.1**: the "Bison/Flex grammar" bullet gains "LALR(1), zero conflicts;
  generated at build time (Bison ≥ 3.8, Flex ≥ 2.6.4)".
- **`.github/workflows/ci.yml`**: `bison flex` in both apt lines; `nightly.yml`: the
  `verilog_gen_vs_yosys` 5,000-seed run and `verilog_vtr_parse` when a VTR checkout is cached
  (a follow-up: the nightly is a stub today).

## 10. Open points for Peter (recommendation first; each is applied as the default meanwhile)

1. **Error recovery versus stop-at-first-error.** Recommend: recover at `;`/`endmodule` with
   actions inert after the first error and a cap of 20 (§3.4) — a user fixing a file sees
   several errors per run, and the fuzzers exercise the recovery paths. Alternative: stop at the
   first error (Yosys does); ≈ 40 fewer lines and one failure mode (review focus 3) less, at the
   cost of one error per run.
2. **`**` associativity.** Recommend: left, as 1364-2005 Table 5-4 states for every binary
   operator (§3.3); the Yosys comparison in §7.5 will show whether Yosys agrees, and 2D's
   constant folding follows the tree either way. Alternative: right (Odin II's `%right`,
   mathematical convention); if Yosys turns out to be right-associative the exit bar (Parmys)
   decides and this flips.
3. **Library files read eagerly on the first unresolved name** (§4.2 step 3: all `-v` files at
   once, `-y` lookups per name). Recommend: yes — it matches Verilog-XL/Icarus semantics ("a
   `-v` file's modules are all available"), keeps the UDP rule 4 story simple, and costs
   nothing on the corpus (`lib_v_y` has one file). Alternative: 2D resolves libraries during
   elaboration (as 2B's spec assumed), which would need the store unsealed during 2D stage 1 or a
   second store.
4. **Unnamed module instances are a 2C error** (§4.2 step 5) rather than auto-named. Recommend:
   the error — 1364-2005 §12.1.2 requires the name, no corpus file omits it, and auto-naming
   would invent names that must then be kept stable across runs for `netlist-compare` (the §7.5
   comparison records what Yosys does with one). Alternative: name them `$unnamed$<n>` like
   Odin II's generated names.
5. **Generated files in the build tree, Bison/Flex required to build** (§6). Recommend: yes;
   CI and the dev machine already have the exact versions, and committed output would need a
   lint carve-out. Alternative: commit the generated files under `src/frontends/verilog/gen/`
   with the directory added to every exclusion list in `tools/lint.sh` — the "documented narrow
   exclusion" of the brief, which this design makes unnecessary.
