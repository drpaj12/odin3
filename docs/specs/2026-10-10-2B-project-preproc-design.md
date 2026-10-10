# 2B — Project readers and the Verilog-2005 preprocessor: design

Status: draft for Peter's light review (PHASE2 #1: 2B is "light"); written under the overnight
rule, every decision below is an agent default he may override, and §12 lists the points that
are genuinely his. Phase 2, sub-project 2B. Spec: `docs/DESIGN.md` §4.0 (the normative project
grammar), §4.1 (the preprocessor), §6 step 1, §12, §15. Inputs: the approved 2A spec
(`docs/specs/2026-10-09-2A-ast-design.md`, cited as AST-n and §3.x), the 2A plan's source-manager
interface (`docs/plans/2026-10-10-2A-ast-plan.md`, Task 1), the coverage rows L1–L22
(`docs/specs/2026-10-09-2C-verilog-coverage.md`), the corpus contract
(`tests/golden/projects/README.md`) and its oracle (`tools/project-fixtures/project_fixtures.py`),
1D (`docs/specs/2026-10-09-1D-abi-design.md`, branch `feat/1D-abi`). Conventions follow
`docs/IR.md`; project decisions are numbered **PRJ-n**, preprocessor decisions **PP-n**.

## 1. Purpose and scope

2B turns a project description into the one record every reader shares, and turns a Verilog
source file into the expanded stream 2C's lexer consumes. It contains no parser of Verilog
beyond what the preprocessor needs (identifiers, strings, comments, bracket nesting).

| Delivered by 2B | Used by |
|---|---|
| **Project record** (`src/frontends/project/project.[ch]`): the C mirror of the corpus's normalized record, owned by the design; JSON dump | `read_verilog` (2C/2D: files, incdirs, defines, top, libraries), Phase 4 (arch, rules, inventory, overrides: stored, not interpreted), the CLI |
| **Readers**: `.o3proj` (`o3proj.c`), `-f`/`-F` lists (`flist.c`), Quartus `.qpf`/`.qsf`/`.qip` (`tcl.c`, `quartus.c`), Odin II XML (`xml.c`, `odin2.c`, planned last, §5.4); shared word splitter (`words.c`) and path resolution (`paths.c`) | the `read_project` pass |
| **Passes and CLI**: `read_project`, `dump_project`, `preprocess` (debug); CLI `--project`, `-f`, `--revision`, `--top` (1D's, applied) | users, the corpus harness, the differential and fuzz tools |
| **Preprocessor** (`src/frontends/verilog/preproc.[ch]`, `macro.c`, `cond.c`, `include.c`, `pp_check.c`): one compilation unit over the project's Verilog files; macro table; expansion with 2A's buffers and segment map; conditionals; includes; `translate_off`; caps | 2C (one stream per project file → one `UNIT`), 2D (library files on demand), the tools above |

Not in 2B: the source manager and `odin3_diag` (2A §3; 2B calls them), the lexer and parser
(2C; comments, metacomments and the pass-through directives are lexed there, §3.3), design-unit
indexing and the errors that need parsed modules (`duplicate_module`, `unresolved_module`,
`no_top`, `ambiguous_top`, `unknown_top`, `dependency_cycle`: 2C/2D raise them from the record's
fields, §4.6), the semantics of mapping rules, inventory and overrides (Phase 4), the Altera
primitive library (Phase 6), SystemVerilog macro operators (Phase 5, §6.5).

Success: every Phase 2 case and format of `tests/golden/projects` produces its expected record
byte-identically through `read_project; dump_project` (§10.2); every L13–L22 verdict has a unit
test; `preprocess` agrees with Icarus and Yosys on every micro and VTR file where those two
agree (§10.4); the §9 caps each have a test; mcml.v preprocesses within the §7 budget; the gate
passes.

## 2. Shape

```
Design ─┬─ strtab                                   paths, names, values (IR-5; AST Q2 precedent)
        ├─ srcman (2A)                              FILE buffers for project files AND sources
        ├─ project (PRJ-1, this spec)               one per design, filled once by read_project
        │     ├─ files, libfiles, libdirs, libext, incdirs, defines, params
        │     ├─ top (+ loc), arch, rules (+ cells), inventory, overrides (+ params)
        │     ├─ flow, output, ignored, duplicates   (ignored/duplicates sorted at finish)
        │     └─ entry {path, format}, options {revision}
        └─ preprocessor state (PP-1)                 one per read_verilog run: macro table,
              │                                      <command line> buffer, include stack
              └─ streams: text + segments in srcman  one per project Verilog file (AST-3)
```

Source layout: `src/frontends/project/` (`project.[ch]`, `words.[ch]`, `paths.[ch]`,
`o3proj.c`, `flist.c`, `tcl.[ch]`, `quartus.c`, `xml.[ch]`, `odin2.c`, `dump.c`),
`src/frontends/verilog/` (`preproc.[ch]`, `macro.c`, `cond.c`, `include.c`, `pp_check.c`;
2C adds the lexer and grammar beside them), `src/passes/builtin.c` (the three passes),
`src/cli/main.c` (the flags), `tools/project-fixtures/` (the harness gains `compare`),
`tools/pp-diff/` (differential), `tests/unit/test_project_*.c`, `tests/unit/test_pp_*.c`,
`tests/fuzz/fuzz_pp.c`, `tests/bench/bench_pp.c`.

## 3. The project record (PRJ-1 … PRJ-4)

**PRJ-1 The design owns one project record.** `odin3_design_get_project(design, &proj)` creates
it on first use (`NO_MEMORY` possible) and the design destroys it; `read_project` fills it and
marks it finished; a second `read_project` on a finished record is a located error ("project
already read from <entry>", `ODIN3_ERR_PARSE`). A failed `read_project` drops the record
(`odin3_design_drop_project`), so the next attempt starts clean. Readers that follow
(`read_verilog`, `read_blif` via the project, `read_arch`) only read it. This mirrors AST Q1:
provenance (which rule chose a mapping, Phase 4) and diagnostics print through design-owned
state for the life of the design.

**PRJ-2 Strings in the design strtab, paths twice.** Every name, value and path is a strtab ID.
A path is `odin3_proj_path {given, resolved, at}`: the text as written, the absolute normalized
path (§4.1), and the `odin3_loc` of the word that named it. Locations are real source-manager
locations: every project file 2B reads (`.o3proj`, each `-f` list, `.qpf`, `.qsf`, `.qip`,
`odin2.xml`) becomes a `FILE` buffer (AST-2: `name` = path as given, `resolved`, `library` 0,
`parent` = the loc of the `-f`/`-F`, `import` or `QIP_FILE` word that reached it, 0 for the
entry; lines registered as the file is split), so `odin3_diag` prints `file:line:col` and the
include chain (`included from files.f:3`) for free, and Phase 4 can cite `rule at top.o3proj:12`.

**PRJ-3 The record mirrors the corpus's normalized record field for field.** Internal header
(`project.h`); the ABI exposes only `dump` in 2B (§5.3):

```c
typedef enum odin3_lang { ODIN3_LANG_VERILOG = 1, ODIN3_LANG_SYSTEMVERILOG, ODIN3_LANG_VHDL,
                          ODIN3_LANG_BLIF, ODIN3_LANG_VQM, ODIN3_LANG_EDIF } odin3_lang;
typedef struct odin3_proj_path   { uint32_t given, resolved; odin3_loc at; } odin3_proj_path;
typedef struct odin3_proj_file   { odin3_proj_path path; uint8_t lang; uint32_t library; }
    odin3_proj_file;
typedef struct odin3_proj_define { uint32_t name, value; odin3_loc at; } odin3_proj_define;
                                                       /* value 0 = none */
typedef struct odin3_proj_param  { uint32_t scope, name, value; uint8_t type; odin3_loc at; }
    odin3_proj_param;                                  /* type: ODIN3_PARAM_NUMBER | _STRING */
typedef struct odin3_proj_arch   { uint8_t kind; odin3_proj_path path; uint32_t family, device; }
    odin3_proj_arch;                                   /* kind: O3LIB | VPR_XML | DEVICE */
typedef struct odin3_proj_rule {                       /* stored only; Phase 4 interprets */
    uint8_t kind;                                      /* MAP | SPLIT | LIMIT | PATTERNS */
    odin3_loc at;
    union {
        struct { uint32_t scope, subject; uint8_t action;        /* TO | SOFT | KEEP */
                 uint32_t cells, ncells;                         /* span in proj->rule_cells */
                 uint8_t has;                                    /* bit per threshold given */
                 uint32_t min_width, max_width, min_depth; } map;
        struct { uint32_t subject; bool width; uint8_t depth_kind; uint32_t depth; } split;
                                                       /* depth_kind: NONE | MIN | MAX | N */
        struct { uint32_t cell, count; } limit;
        struct { odin3_proj_path path; } patterns;
    } u;
} odin3_proj_rule;
typedef struct odin3_proj_inventory { uint32_t cell, count; odin3_loc at; } odin3_proj_inventory;
typedef struct odin3_proj_override  { uint8_t kind; uint32_t name, from;       /* HIDE | CELL */
                                      uint32_t params, nparams; odin3_loc at; }
    odin3_proj_override;                     /* params: span in override_params {key, value} */
typedef struct odin3_project {
    odin3_design *design;
    odin3_proj_path entry; uint8_t format;             /* O3PROJ | FLIST | QUARTUS | ODIN2 */
    uint32_t revision;                                 /* --revision, 0 = none */
    odin3_vec files, libfiles;                         /* odin3_proj_file; libfiles: library 0 */
    odin3_vec libdirs, incdirs;                        /* odin3_proj_path */
    odin3_vec libext;                                  /* uint32_t; empty = ".v" at lookup */
    odin3_vec defines, params, arch, rules, rule_cells, inventory, overrides, override_params;
    uint32_t top; odin3_loc top_at;                    /* "mod" or "lib.mod" as given; 0 = auto */
    odin3_proj_path flow;                              /* given 0 = none */
    struct { uint32_t path, format; } output;          /* Odin II <output>; 0 = none */
    odin3_vec ignored, duplicates;                     /* uint32_t strtab IDs, sorted at finish */
    bool finished;
} odin3_project;
```

Values stay text (a `param` value `8'hFF`, a define value `"AB"` with its quotes): the record
reports what was written; 2D converts when it applies them. Counts (`available`, `limit`,
`min_width`…, `split … depth <n>`) are `uint32_t`; a count that does not fit is a `syntax` error
("count too large") — the oracle accepts any digit string, the corpus has none above 2^32, and
the oracle gains the same cap (§10.1).

**PRJ-4 `dump` is the test seam.** `odin3_project_dump(proj, &strbuf)` writes the record as JSON
with exactly the README's keys in the README's order, one key per line, arrays and objects on
one line each, paths as their **resolved** absolute form (the harness relativizes, as the README
says), `ignored`/`duplicates` sorted by bytes (Python's `sorted` on `str` orders code points,
which is byte order for UTF-8), `null` for an absent top/flow/output, `value: null` for a bare
define. `read_project` also emits the two info lines through `odin3_log` exactly as the README
spells them (`info: <entry as given>: ignored: A, B, …`, `info: <path>: listed more than once,
read once: …`), only when non-empty.

## 4. Reading rules common to every format

### 4.1 Files, lines, paths (PRJ-5, PRJ-6)

**PRJ-5 Whole file, bytes.** A project file is read whole (`odin3_file_read_all`, added to
`util/file` and shared with the preprocessor; `ODIN3_SRC_MAX_FILE_BYTES` applies), kept as bytes
(no encoding check; the oracle decodes UTF-8 and would crash, not report, on bad input — no case
has any), and split at `\n`; a `\r` before the `\n` is dropped (LF and CRLF). A lone `\r` is an
ordinary character. The oracle's `splitlines()` also splits at `\r`, `\v`, `\f` and the Unicode
separators; no case contains them and the oracle is tightened to the two endings (§10.1).

**PRJ-6 Path resolution is lexical.** `resolve(base, given)`: an absolute `given` stands; else
`base + "/" + given`; then normalize like `os.path.normpath` (collapse `//`, drop `.`, fold `..`
against the preceding component, never consult the file system, never resolve symlinks). The
result must exist with the right kind (`stat`: regular file, or directory where a directory is
expected; a symlink to one counts) else `missing_file` at the naming word, `names` = [the
resolved path; the harness relativizes it, §8]. `base` is the directory of the file that names
the path, except in file lists (§5.2) and Quartus files (§5.3). Nothing ever resolves against the working directory: the
harness runs every case from an unrelated directory (the oracle's `elsewhere()`).

Language from the extension, case-insensitive: `.v .vh` Verilog, `.sv .svh` SystemVerilog,
`.vhd .vhdl` VHDL, `.blif`, `.vqm`, `.edf .edif`; else `unknown_file_type`, `names` = [the
text as given]. `libext` entries are checked the same way (`+libext+.foo` is an error). Arch
files: `.o3lib` → `o3lib`, `.xml` → `vpr_xml`, else `unknown_file_type`.

### 4.2 Words (`.o3proj` and `-f`) (PRJ-7)

Exactly the oracle's `split_words`: blanks are space and tab; a word is bare, or wholly inside
double quotes with `\"` and `\\` the only escapes (any other backslash inside quotes: `syntax`);
a bare word's backslashes are ordinary; a character other than blank right after the closing
quote, an unterminated quote, and `""` are `syntax`; `#` at the start of a word begins a comment
to the end of the line, and so does `//` in a file list; inside a word both are ordinary; a `"`
inside a bare word is `syntax` in `.o3proj` and ordinary in a file list. Each word remembers
whether it was quoted (a quoted first word is `unknown_key`; a quoted `param` value is a
string). `names` on a `syntax` error is the offending word where the oracle gives one, else
empty; the harness compares `names` only when the expected list is non-empty for that format.

### 4.3 Duplicates and ignored names (PRJ-8)

`files`: the same resolved path in the same library a second time is not added; its path goes
into `duplicates`. `libfiles`: the same resolved path twice, likewise (no library). `ignored`
and `duplicates` are sets of strtab IDs, sorted by bytes at finish. A diamond of nested lists is
only a duplicate listing, never a duplicate module (PHASE1 #19).

### 4.4 Top and parameters (PRJ-9)

`set_top(name, at)`: `name` must match `IDENT` or `IDENT.IDENT` (`[A-Za-z_][A-Za-z0-9_$]*`),
else `syntax`; a second top is `syntax` ("top given twice (a, b)"). The CLI's `--top NAME`
(1D's option) is applied by `read_project` after the file is read: it **replaces** the
project's top, with an info line when they differ (§12 #1). `param` lines are checked once the
file is read: the scope's first component must equal the top's module name (library stripped),
else `syntax` at the `param` line.

## 5. One reader per format

Each reader is one function `odin3_status read_<fmt>(odin3_project *proj, const
odin3_proj_path *file, <state>)`; all stop at the first error (the oracle raises), returning
`ODIN3_ERR_PARSE` after one `odin3_diag` (§8). Tables below restate DESIGN §4.0 where a C
implementer needs an arity or an order; where this file and §4.0 or the oracle differ, §4.0
then the oracle wins and this file is corrected.

### 5.1 `.o3proj` (PRJ-10)

One statement per line; the first (bare) word is the key; `unknown_key` otherwise, `names` =
[key]. Arity is checked before anything else (`syntax`, `names` empty unless noted):

| Key | Words after the key | Effect |
|---|---|---|
| `file` | `<lang> <path> [library <lib>]` (2 or 4) | `files` += {path, lang (one of the six names), lib or `work`} |
| `libfile` | `<lang> <path>` | `libfiles` += (unknown lang: `syntax`, names [lang]) |
| `libdir` | `<dir>` | `libdirs` += directory |
| `libext` | `<ext>…` (≥ 1) | each checked (§4.1), appended |
| `incdir` | `<dir>` | `incdirs` += directory |
| `define` | `<name>[=<value>]` (1) | name must match `IDENT` (else `syntax`, names [word]); value is everything after the first `=`, absent when there is none |
| `top` | `[<lib>.]<mod>` (1) | §4.4 |
| `param` | `<top>[.<inst>…].<name> <value>` (2) | scope = everything before the last `.`, must be `IDENT(.IDENT)*`; quoted value → `string`, else it must match the oracle's `NUMBER` (`-?[0-9][0-9_]*` or `([0-9][0-9_]*)?'[sS]?[bBoOdDhH][0-9a-fA-FxXzZ_?]+`, whole word) → `number`, else `syntax` names [value] |
| `arch` | `<file>` | `arch` += {kind by extension, path} |
| `device` | `<family> [<part>]` | `arch` += {DEVICE, family, part or none} |
| `available` | `<cell> <n>` | `inventory` += |
| `hide` | `<cell>` | `overrides` += HIDE |
| `cell` | `<name> from <libcell> [param <k>=<v>…]` (3, or ≥ 5 with word 4 = `param`) | `overrides` += CELL; each `k=v` needs a non-empty `k`, an `=`, and no repeated `k` (`syntax`, names [word]) |
| `map` | `<scope> <subject> (to <cells…> \| soft \| keep) [<thr> <n>]…` | cells: the words up to the first threshold word, joined by one space and split at `,`, each trimmed, none empty or containing a blank; thresholds `min_width max_width min_depth`, each at most once, pairs only |
| `split` | `<subject> [width] [depth min\|max\|<n>]` (2–4) | at least one of `width`/`depth`; `depth` keeps `min`/`max`/the digits as written |
| `limit` | `<cell> <n>` | `rules` += LIMIT |
| `patterns` | `<file.o3lib>` | `rules` += PATTERNS (path must exist) |
| `flow` | `<script>` (once; else `syntax` names [`flow`]) | `flow` = path (must exist) |
| `import` | `<file.qpf\|.qsf> [revision <r>]` (1 or 3) | extension must be `.qpf`/`.qsf` (`unknown_file_type`, names [word]); file must exist; then §5.3 with the `import` word's loc as the revision's location; the Quartus top goes through `set_top` (a second top is `syntax`) |

### 5.2 `-f` / `-F` file lists (PRJ-11)

Words of the whole list are collected first (§4.2, file-list mode), then consumed left to
right; `$NAME` / `${NAME}` (`NAME` an `IDENT`) are expanded from the process environment **when a
word is consumed** (so a `$X` after a comment marker never errors): an unset name is
`undefined_variable` (names [NAME]); after expansion of the known forms, any `$` left in the
word is `syntax`. The harness runs `odin3` under `env -i` plus the case's `env` (§10.2), which
is how the oracle's "the harness passes nothing else" is honoured in C.

Resolution base: `f_root` = the directory of the outermost list for the entries of that list and
of every list reached through `-f`; a list reached through `-F` resolves its entries (and what it
reaches through `-f`) against its own directory. Implementation: `read_flist(proj, file, base,
chain, &sv)`; `-f <list>` recurses with the same `base`, `-F <list>` with the nested list's
directory; the nested list's existence is checked at the option (`missing_file`), and a list
already in `chain` is `f_cycle` with `names` = the chain from that list to the end plus the list
again (resolved paths). `-sv` sets `sv` for the rest of the list that says it, including nested
lists, and is restored on return. Options:

| Word | Effect |
|---|---|
| bare word | source file; language by extension, `systemverilog` when `sv` and the extension says Verilog; library `work` |
| `-f` / `-F <list>` | above |
| `-v <file>` | `libfiles` += (language by extension) |
| `-y <dir>` | `libdirs` += |
| `-top` / `--top-module <name>` | `set_top` |
| `-sv` | above |
| `+incdir+<d>[+<d>…]`, `+define+<n>[=<v>][+…]`, `+libext+<e>[+…]` | split at `+`, empty parts dropped; no part left is `syntax` (names [word]); other `+key+` is `unknown_option` |
| any other `-…` | `unknown_option`, names [word] |

An option needing an argument at the end of the list is `syntax` (names [option]). The
argument is expanded before use.

### 5.3 Quartus `.qpf` / `.qsf` / `.qip` (PRJ-12 … PRJ-14)

**PRJ-12 `.qpf`.** Lines that are blank or start (after blanks) with `#` are skipped; every
other line must be `NAME = "value"` (`[A-Z_]+`, blanks allowed around `=`, nothing after the
closing quote) else `syntax`. The revisions are the `PROJECT_REVISION` values in order, or the
`.qpf`'s stem when there are none. With a wanted revision (`--revision`, or `import … revision`)
not among them: `unknown_revision` (located at the `import` word; unlocated for `--revision`),
names [wanted]. With several and none wanted: `ambiguous_revision` at the first
`PROJECT_REVISION` line, names = the revisions in file order. The chosen revision's
`<revision>.qsf` beside the `.qpf` must exist: `missing_file` located at the `.qpf` as a whole
(a file-level location, §8; the oracle's `at` is the file name without a line). A `.qsf` given
directly is revision = its stem; a wanted revision that differs is `unknown_revision`.

**PRJ-13 Tcl words.** `tcl.c` is the oracle's `TclReader` in C, byte for byte in behaviour: a
command is words up to a newline or `;`; blanks are space, tab, CR; `\`-newline is a
continuation anywhere a blank may be; `#` where a command may start comments to the end of the
line; a word is `"…"` (escapes `\"`, `\\` only; `$` inside is `syntax`; newline inside is
`syntax` "unterminated quote" located at the line the quote opened), `{…}` (nested, verbatim),
`[…]` (only the `.qip` idiom `[file join $::quartus(qip_path) "<path>"]` or its bare-word form;
the word becomes the normalized path of `<qip dir>/<path>`; anything else is `syntax`), or bare
(`$` or `\` inside: `syntax`; `[` and `]` inside are ordinary). Any character other than a
blank, `;`, newline or continuation right after a closing quote, brace or bracket is `syntax`.
A command's location is the line its first word starts on. The reader works on the whole file
with CRLF folded to LF first.

**PRJ-14 Commands.** All plain relative paths in a `.qsf` and in every `.qip` resolve against the
**project directory** (the `.qsf`'s), as Quartus does; only the idiom resolves against the `.qip`.
Assignment names are compared upper-cased; enumerated rule values upper-cased; everything else
exact.

| Command | Handling |
|---|---|
| `set_global_assignment`, `set_instance_assignment`, `set_io_assignment` | exactly one `-name N` (else `syntax`); options allowed by `N` (below) else `unknown_option` names [opt]; flags `-disable`/`-remove` take no value, other options one (`syntax` at the end); exactly one positional value (`syntax`) |
| `set_location_assignment` | options validated against the full Quartus set; `ignored` += `LOCATION` |
| `set_parameter` | options `-name -entity -to` only; `-name` required; queued (below) |
| anything else | `ignored` += the command word |

Assignment names: `VERILOG_FILE SYSTEMVERILOG_FILE VHDL_FILE VQM_FILE EDIF_FILE` (options
`-name -library -hdl_version`; a design file with the named language, library `-library` or
`work`; `-hdl_version` present → `ignored` += `-hdl_version`); `TOP_LEVEL_ENTITY` (last wins,
its location kept); `SEARCH_PATH` (`;`-separated: each is an `incdirs` and a `libdirs` entry, in
that order per directory) and `USER_LIBRARIES` (`libdirs` only); `VERILOG_MACRO` (a define, as
`.o3proj` `define` parses it); `FAMILY` / `DEVICE` (one `arch` DEVICE entry created at the first
of either, in `arch` load order, then updated); `QIP_FILE` (must exist; a `.qip` already on the
qip stack is `qip_cycle`, names = the stack from it plus itself; read now with the same state,
with `qip` = true so the idiom is allowed); mapping names `AUTO_DSP_RECOGNITION` (OFF → `$mul`),
`AUTO_RAM_RECOGNITION` (OFF → `$mem`), `DSP_BLOCK_BALANCING` (`LOGIC ELEMENTS` → `$mul`),
`MULTSTYLE` (LOGIC → `$mul`), `RAMSTYLE` (LOGIC → `$mem`), with `ON`/`AUTO` the default (no rule)
and any other value `ignored` += the name (options `-name -to -entity`); `VERILOG_INCLUDE_FILE`
and every other name: `ignored` += the name (any Quartus option allowed). These single-value
names allow only `-name`.

After the file: top = `TOP_LEVEL_ENTITY` or the revision, through `set_top` (located at the
assignment, else at the `.qsf` as a whole — a file-level location, §8). Then each queued
`set_parameter`: `-entity` other than the top's module name is `syntax` (names [entity]); value
typing: whole-word `NUMBER` → `number`; `"…"` with literal quotes → `string` without them; else
`string` as written; scope = `anchored(entity, to)` when `-to` is given, else the top's module
name. Then each queued rule: `{map, anchored(entity, to), subject, soft, cells []}`.
`anchored`: no `-to` → `-entity` or `*`; with `-to a|b:c` → components after the last `:` of
each `|` part (empty parts dropped), prefixed with the anchor (`-entity`, else the top's module
name) unless the first component already is the anchor; joined with `.`.

### 5.4 Odin II XML (PRJ-15, planned last)

Specified now so the record and the harness need no change when it lands; implemented as the
last 2B PR (§11) because DESIGN §4.0 and the README put the format "after Phase 2" and the 11
corpus cases that carry `odin2.xml` are gated on `format_phase`. A ~300-line subset XML reader
(`xml.c`): prolog and comments skipped, elements with attributes (either quote), text, CDATA,
the five predefined entities and numeric references; anything else (DTD, processing
instructions, unbalanced tags) is `syntax` at the line. An element's line is the line of its
`<` (expat's `CurrentLineNumber` at the start tag). Root must be `config` (`syntax`, names
[root]). Children: `verilog_files` (each `verilog_file` a Verilog design file in `work`,
relative to the XML's directory; other children `ignored` += `verilog_files/<tag>`); `inputs`
(at most one `input_type`, lower-cased, `verilog` default, `verilog`/`blif` else
`unsupported_input_type` names [as written]; `input_path_and_name` files; other children
`ignored` += `inputs/<tag>`); `output` (`output_type` lower-cased → format; `output_path_and_name`
→ `output.path` normalized but **not** required to exist; `target/arch_file` → `arch`; other
`target` children `ignored` += `output/target/<tag>`; other children `output/<tag>`; `output` is
recorded only when a path was given); `optimizations` (`multiply@size` → `map * $mul to multiply
min_width N`; `adder@threshold_size` → `map * $add to adder min_width N`; `memory` with
`split_memory_width` 0|1 and `split_memory_depth` min|max|digits (else `syntax`) → `split $mem
[width] [depth …]` when either is set; other attributes `ignored` += `optimizations/<tag>@<attr>`;
other elements `optimizations/<tag>`); any other root child `ignored` += `<tag>`. No top.

### 5.5 What each format cannot say

A `-f` list has no parameters, libraries, arch, rules or flow; a `.qsf` always yields a top; Odin
II XML yields only files, an arch, an output and rules. The corpus's `format_overrides` lists
exactly the fields a format states differently (`top`, `ignored`, `libdirs` for `qsf`; `top`,
`output`, `ignored` for `odin2`), and the harness applies them (§10.2).

## 6. Passes and CLI (PRJ-16 … PRJ-18)

**PRJ-16 `read_project [--format o3proj|f|qsf|odin2] [--revision <r>] <entry>`.** Format from
the extension: `.o3proj`; `.qpf`/`.qsf` (a `.qip` is not an entry: `unknown_file_type`); `.xml`
(until the reader lands: `unknown_file_type` "no Odin II XML reader yet", names [entry]);
anything else is a file list
(`.f`, `.vc`, …); `--format` overrides. The entry must exist (`missing_file`, unlocated, names
[entry as given]). The pass fills the record and runs **no** front end: scripts read
`read_project top.o3proj; read_verilog; …` (AST-15: `read_verilog` without arguments reads the
project's Verilog files; with a path it reads that file as a one-file project — 2C's call). A
project whose `flow` is set is still not executed (DESIGN §4.0: stored). `--revision` is also a
1D pass option (`odin3_pass_options.revision`), so the CLI flag reaches a `read_project` inside
a script.

**PRJ-17 `dump_project [<path>]`** writes §3's JSON to stdout or `path`; `INVALID_ARG` when no
project was read. **`preprocess [--out <dir>] [--segments]`** (debug) runs the preprocessor over
the project's Verilog design files in listing order and writes `<dir>/<n>.v` per stream (the
expanded text; stdout when no `--out`) and, with `--segments`, `<n>.segments.json` (`[{out_offset,
file, line, col, buffer_kind, macro}]` decoded through the source manager). It is the seam for
the corpus's include negatives before 2C exists, for `tools/pp-diff` and for the fuzzers.

**PRJ-18 CLI.** `--project FILE` and `-f LIST` prepend `read_project FILE` (with `--format f` for
`-f`) to the script list; `--revision R` sets the option; `--top` is 1D's (applied by
`read_project` per §4.4, and still by `read_blif`/`hierarchy` as today, so 1D's "`--top` was not
applied" check also counts `read_project`); `--dump-project` appends `dump_project`. A
`--project`/`-f` without any script runs `read_project` alone and exits with its status.

**Ordering against 1D and 2A.** Neither is on `main` today: `feat/1D-abi` holds the pass
manager and ABI v3 (T1, T2 done, PR not yet open); `feat/2A-ast` holds the approved spec and
plan, no code. 2B's work is cut so nothing waits that need not:

1. **PR 2B-1 record + readers** (`.o3proj`, `-f`, Quartus; `dump`; unit tests): needs 2A Task 1
   (`srcman`, `odin3_diag`) for `odin3_loc` and located diagnostics — the first 2A merge. Until
   it merges, 2B-1 is developed on a branch from `feat/2A-ast` after Task 1 lands there, and
   rebased onto `main` when 2A-T1 merges.
2. **PR 2B-2 preprocessor** (§6–§9, unit tests, `pp_check`, bench): needs 2A Task 1 only;
   parallel with 2B-1 (disjoint files).
3. **PR 2B-3 passes, CLI, corpus CTest, pp-diff, fuzz**: needs 1D merged (pass registry, CLI
   script path) and 2B-1/2B-2. If 1D is still unmerged when 2B-3 is ready, 2B-3 branches from
   `feat/1D-abi` and its PR targets `main` after 1D's.
4. **PR 2B-4 Odin II XML** (§5.4): after 2B-3; scheduled per §12 #5.

## 7. The preprocessor (PP-1 … PP-12)

### 7.1 Unit, state, interface (PP-1)

One `odin3_pp` per `read_verilog` run, created from the project: `odin3_pp_create(design, proj,
&pp)` builds the macro table from `proj->defines` by writing the `<command line>` `FILE` buffer
(AST-2: one line per define, `` `define NAME `` or `` `define NAME value `` with the value bytes
as written, in project order, LF-terminated; `name` = `<command line>`, `resolved` and `library`
0) and running the definition scanner over it, so a bad project define is a located error
(`<command line>:3:9: error: …`). `odin3_pp_run(pp, file_index, &stream)` preprocesses one
project file (design file or, when 2D resolves a `-v`/`-y` module, a library file given by path
and library: `odin3_pp_run_path`) and returns `odin3_pp_stream {odin3_bytes text; odin3_srcbuf_id
stream; uint32_t nsegments}`; the text is owned by `pp` until the next run or destroy (2C
scans it with `yy_scan_bytes` or a custom `YY_INPUT`). Macros defined in one file stay defined
for the next (DESIGN §4.0: one compilation unit); the conditional stack and the include stack
are per file and must be empty at its end. `odin3_pp_destroy` frees the table and the text;
buffers and segments live on in the source manager (AST-1).

**Whole file, not streaming.** The output of one project file plus everything it includes is
one contiguous buffer: Flex wants a buffer anyway, the segment cursor (`odin3_srcman_cursor_loc`)
assumes forward motion over one stream, and the input is capped at 2^28 bytes. Memory for
mcml.v: ≈ 0.65 MB in, ≈ 0.7 MB out, 2,953 directive lines, under 1 MB of srcman records.

### 7.2 Scanning model (PP-2)

The preprocessor is a byte scanner over an **input stack of frames** `{text, len, pos, runs}`,
where `runs` maps offsets of the frame's text to locs (the file frame has one run; an expansion
frame lists its body and argument runs). The top frame is scanned; what is not a directive, a
macro use or a skipped region is copied to the output, and each byte copied starts a new
segment whenever its loc is not the previous byte's loc + 1 — that single rule produces every
segment AST-3 lists (expansion entry, each argument run, return to the enclosing buffer,
dropped continuation, stripped comment, inserted byte). The scanner recognises, outside skipped
regions: `//` and `/* */` comments (copied through verbatim in file text — the lexer records
them (§3.3) — and checked for `translate_off`/`on`, §7.6); string literals `"…"` with `\"` and
`\\` escapes, ended by an unescaped newline (the lexer reports the unterminated string; the
preprocessor only refuses to expand inside it); escaped identifiers `\` + non-blanks to the next
blank (no expansion inside: `` \a`b `` is one identifier); a backtick followed by an `IDENT`
anywhere else — including glued to the preceding token (`` x`S ``, `` 8'h`W ``), which AST-2's
token-end rule exists for. A backtick followed by `"` or `\` or another backtick is §7.5; a
backtick followed by anything else (a blank, a digit, end of line) is `syntax` "stray backtick".

### 7.3 Directives consumed by 2B (PP-3, PP-4)

**PP-3 Token-based, not line-based.** Conditionals and `include` may appear anywhere a blank may
(`wire a = 1 `ifdef X + 1 `else + 2 `endif;` is legal and Icarus accepts it): a directive consumes
the backtick, its name and its argument (one `IDENT` for `ifdef ifndef elsif undef`; nothing for
`else endif`; one `"…"` for `include`; number, string and 0–2 for `line`) and nothing more;
`define` alone consumes to the end of its logical line (continuations included). A consumed
directive emits nothing itself; the bytes around it keep their own locs, so `a `ifdef X b
`endif c` yields `a  c` with a segment either side of the gap. When the output byte before a
consumed directive or skipped region is not a blank (`` a`ifdef X `` …), 2B inserts one space
(a `loc` 0 segment) so that tokens never merge across it.

| Directive | Rule |
|---|---|
| `` `define NAME[(formals)] text `` | name an `IDENT` (else `syntax`); formals only when `(` **immediately** follows the name: `IDENT [= default]` separated by `,`, blanks allowed, at most `ODIN3_PP_MAX_MACRO_ARGS`; a default is the text to the next top-level `,`/`)` (parentheses, brackets, braces and strings nest), trimmed; text = from the first non-blank after the name/formals to the end of the logical line: `\`-newline continues it (CRLF: `\` `\r` `\n` too), a `//` comment ends it (and a `\` inside that comment continues nothing), a `/* */` inside it is part of the text and is dropped at expansion (§7.4), an unterminated `/*` on the logical line is `syntax` "unterminated comment in macro text", a `"` string inside it protects `//`. Stored: name, formals (strtab IDs) with default ranges, the body range `[def, def + len)` in the defining buffer (the bytes as spelled, continuations included), the definition loc. Redefinition replaces silently (L13: allowed; Yosys is silent). Defining a name that is a compiler directive (`define`, `timescale`, …) is `syntax`. |
| `` `undef NAME `` | removes; an unknown name is silent (Yosys); a directive name is `syntax` |
| `` `ifdef NAME `` / `` `ifndef NAME `` | pushes `{taken, seen_true, loc}`; in a skipped region the argument is still consumed and the frame pushed as skipped (L16, gap 22) |
| `` `elsif NAME `` | without an open frame: `syntax`; else as the oracle: taken iff no branch yet and defined |
| `` `else `` | likewise; a second `else` is `syntax` |
| `` `endif `` | pops; without a frame: `syntax` "`endif` without `ifdef`" |
| `` `include "file" `` | §7.7; `<file>` and a macro as argument are `syntax` ("`include` takes a double-quoted file name") |
| `` `line n "file" level `` | syntax-checked, dropped: locations come from the segment map (AST-3: "`line` never changes a loc") |

**PP-4 Skipped regions emit nothing and interpret only conditionals.** Inside a false branch the
scanner still tracks comments and strings (so a `` `endif `` inside a comment or string does not
close anything) and nested conditionals; `define`, `undef`, `include`, macro uses and other
directives are ignored; comments there reach no stream (§3.3). At the end of a file every frame
it opened must be closed: an open one is `syntax` "unterminated `ifdef`" at the `ifdef`; an
`endif` that would close a frame opened by the including file is `syntax` (conditionals do not
cross file boundaries).

### 7.4 Macro expansion (PP-5 … PP-7)

**PP-5 Use.** On `` `NAME `` outside strings, comments and escaped identifiers: a pass-through
directive name (§7.5) is copied; a macro with no formals expands; a macro with formals requires
`(` after optional blanks (newlines included); absent `(` is `syntax` "macro 'NAME' takes
arguments". Actuals are collected from the **current frame** up to the matching `)`: `,` at
nesting depth 0 separates; `(`/`)`, `[`/`]`, `{`/`}` nest; strings are opaque; comments inside
actuals are replaced by one space (a `loc` 0 segment, §3.3); newlines are kept. Running off the
frame is `syntax` "unterminated arguments of macro 'NAME'". Each actual is trimmed of leading and
trailing blanks (space, tab, CR, LF). More actuals than formals: `syntax`; fewer: the missing
ones take their defaults, an empty actual (`(a,,b)`) takes its default too, and a missing or
empty actual without a default is `syntax` "macro 'NAME' expects N arguments, got M" (1800 §22.5.1
semantics for defaults, which Peter's brief asks for; everything else is 1364-2005 §19.3.1).
Any other backtick word is `syntax` "undefined macro 'NAME'" (L14 REJECT: Yosys errors; Icarus
warns and substitutes nothing — see §10.4 for the differential consequence). Undefined macros
inside skipped regions are not reported.

**PP-6 Substitution then rescan, with 2A's buffers.** The expansion of a use is a new frame
whose text is the body with every formal occurrence — an `IDENT` token of the body equal to a
formal, outside strings and comments, and not immediately after a backtick (`` `ADDITION_num `` is
the macro name `ADDITION_num`, never `ADDITION` + `_num`: Yosys and Icarus agree, and the two
micros that rely on textual splicing, `preprocessor_complex_define` and
`preprocessor_define_with_comment`, are Parmys-rejected exceptions under PHASE2 #4) — replaced by
the actual's bytes **as collected**, not pre-expanded; `/* */` comments in the body become one
space (`loc` 0). Buffers are created exactly as AST-2/§3.1 prescribe: one `EXPANSION` buffer per
use (`def` = the body's first byte in its defining buffer, `len` = the body's spelled length,
`parent` = the loc of the backtick, `parent_end` = one past the `)` or the name); one
`MACRO_ARG` buffer per **contiguous spelled run** of each substituted actual (an actual spelled
across several runs of the enclosing frame — itself an expansion — yields one buffer per run;
`def` = the run's first byte wherever it is spelled, `parent`/`parent_end` = the formal's
occurrence in the body). The body's `\`-newline pairs are dropped (one segment each, AST-3).
The new frame's `runs` list is the body/argument interleaving, so nested uses
inside it get their own buffers and the 2A worked example (`OUTER`/`INNER`, buffers E1 A1 E2 A2
A3) is reproduced byte for byte — its segment list is a unit test. The frame is then scanned
like any other (rescan): macros in the result expand; a conditional, `define`, `undef` or
`include` directive inside an expansion is `syntax` "directive inside macro text" (no corpus
file has one; accepting them would make the conditional stack span frames).

**PP-7 Recursion is an error, not a hang.** A use of a macro whose name is already on the
expansion stack (directly, or through an argument that is expanded inside the body — the stack
holds every live `EXPANSION` frame) is `syntax` "recursive macro 'NAME'" with the expansion
chain printed (§8): both Icarus and Yosys loop forever on `` `define R `R + 1 `` (measured
2026-10-10), so this is stricter than both oracles and never disagrees with a terminating one.
Repeated non-recursive use is unaffected (a formal used twice in a body gets its buffers per
occurrence, AST-2).

### 7.5 Pass-through directives and SystemVerilog forms (PP-8, PP-9)

**PP-8 Pass-through.** `` `default_nettype `timescale `celldefine `endcelldefine `resetall
`begin_keywords `end_keywords `unconnected_drive `nounconnected_drive `pragma `` are copied to the
output with their backtick (one segment, since they are spelled bytes) and nothing else is
done; 2C lexes the directive and its argument on that line and builds the `DIRECTIVE` node
(L18–L22). `resetall` leaves the macro table alone (1364-2005 §19.6 lists no text macros; Icarus
keeps them, measured). Inside a macro body they pass through likewise. `` `line `` is the one
directive with no node: consumed (§7.3).

**PP-9 SystemVerilog macro operators are errors in 2B.** ``` `` ``` (paste), `` `" `` and
`` `\`" `` (strings with substitution) in a macro body, and `` `__FILE__ ``/`` `__LINE__ ``
anywhere, are `syntax` naming the operator and saying "SystemVerilog macro operator; not
Verilog-2005 (Phase 5)". The `SCRATCH` buffer kind exists for Phase 5 and 2B never creates one
(AST-2). Measured: no VTR benchmark or micro uses them outside comments (the four grep hits are
`` ``AS IS'' `` in licence headers). Formals inside a plain `"…"` string of a body are **not**
substituted (1800 §22.5.1's reason for `` `" ``; Yosys treats the string as one token; Icarus
differs and substitutes — the differential tool reports, not fails, where the oracles disagree).

### 7.6 `translate_off` / `translate_on` (PP-10)

Belongs to 2B (§3.3, L12, D1). A comment whose text, after the `//` or `/*` and blanks, is
`synopsys translate_off`, `synthesis translate_off` or `pragma translate_off` (exact case, as
Yosys's lexer spells them; optional blanks and, for `/* */`, the closing `*/`) opens a skipped
region ending after the matching `… translate_on` comment; the comments and everything between
leave the stream and nothing inside is interpreted (not even conditionals: the region is
opaque text, so a `` `endif `` inside it is not seen and the enclosing `` `ifdef `` reports
"unterminated" at end of file — documented, not a target of support). Unterminated: `syntax`
at the `translate_off`; a stray `translate_on`: warning, ignored. Inside a skipped `ifdef` branch
the comment is skipped text and never seen. Six VTR benchmarks carry the pair (`LU*PEEng`,
`arm_core`, `spree`, `or1200`); the micros' uses enclose only comments.

### 7.7 `include` (PP-11)

`` `include "name" ``: an absolute `name` stands; else the including file's directory, then
`proj->incdirs` in order; the first regular file wins (lexical normalization as §4.1). None:
`include_not_found`, names [name as written]. A candidate already on this stream's include stack
(resolved path): `include_cycle` at the `include`, names [name as written]. The file gets a `FILE`
buffer (`name` = the text as written, `resolved`, `library` = the includer's, `parent` = the loc
of the directive's backtick), its lines are registered as it is loaded, it is pushed as a new
file frame with its own conditional stack, and when it ends the includer continues after the
directive. The same file included twice (not nested) is two buffers (§3.2).
`ODIN3_SRC_MAX_FILE_BYTES` and `ODIN3_PP_MAX_INCLUDE_DEPTH` apply. The set of files read is reported to 2D for the corpus's
`resolved.read` (`odin3_pp_files_read(pp)`: resolved paths in first-read order).

### 7.8 Output and the lexer contract (PP-12)

For each stream 2B delivers exactly AST-3's three things: (1) the text; (2) the segment map,
`odin3_srcman_add_segment` per segment with `stream` = the project file's `FILE` buffer ID and
strictly increasing `out_offset`; (3) the buffers and lines. Comments in file text are in the
stream verbatim (the lexer records only those whose loc is in a `FILE` buffer: comments from
macro bodies and arguments have been replaced by `loc` 0 spaces, comments in skipped regions
are gone). Every newline of text that reaches the stream is preserved (the newline ending a
`define` line is not part of the macro text and is emitted; skipped regions and consumed
directives contribute nothing), so humans reading `preprocess` output and `iverilog -E` see the
file's lines; nothing else is inserted (except PP-3's protective space) or reordered. The
stream of a file with no directive, macro use or `translate_off` is byte-identical to the file
with one segment.

**`odin3_pp_check(pp, stream)`** (Debug builds, after every run; `pp_check.c`; also run by the
fuzzers): segments strictly increasing and starting at 0; every segment loc decodes or is 0;
for every output byte whose loc is not 0, the byte equals the byte at
`odin3_srcman_spelling` chased to its `FILE` buffer (the preprocessor keeps file bytes until the
run ends, so the check can read them); every `loc` 0 byte is a blank; every `EXPANSION`/`MACRO_ARG`
buffer created in the run is referenced by at least one segment or is the parent of one that is.
Violations are `ODIN3_ERR_CHECK`, logged.

## 8. Errors and diagnostics

Every rejection is `ODIN3_ERR_PARSE` after one `odin3_diag(design, ODIN3_LOG_ERROR, loc, …)`
with the message `<kind>: <detail>` where `<kind>` is a DESIGN §4.0 error kind (`missing_file`,
`unknown_key`, `unknown_option`, `unknown_file_type`, `unsupported_input_type`, `syntax`,
`undefined_variable`, `f_cycle`, `qip_cycle`, `ambiguous_revision`, `unknown_revision`,
`include_not_found`, `include_cycle`) and `<detail>` names what §4.0 says the error names (the
corpus's `names`, each quoted once, in order). `odin3_diag` prints `file:line:col: error:` with
the file location and then the chain (`included from files.f:3`, `expanded from macro …`), so a
preprocessor error inside a macro argument inside an include prints the 2A §3.4 chain. A
**file-level** error (a `.qpf`'s revision `.qsf` missing; a `.qsf` default top that is not an
identifier) is logged through `odin3_log` as `<file as given>: error: <kind>: <detail>`; an
**unlocated** one (`unknown_revision` from `--revision`, a missing entry) as `<entry as given>:
error: <kind>: <detail>`. The harness (§10.2) parses `kind` and the location prefix (`file:line`
with the column dropped, `file` alone, or none — matching the oracle's `at` string, relative
path or null), relativizes any absolute path in the detail to the case directory, and checks
that every expected name appears in the detail. Warnings (stray `translate_on`) and infos
(ignored, duplicates, `--top` replacing the project top) use the same function at their level. `ODIN3_ERR_IO` only for a file
that exists but cannot be read (permissions, read error); `ODIN3_ERR_NO_MEMORY` leaves the
record dropped and the preprocessor's run abandoned (buffers already added to the source
manager stay, harmlessly, as AST-15 says for a parse failure); `ODIN3_ERR_INVALID_ARG` for
misuse (`dump_project` without a project, a second `read_project`, NULL handles).

Error-kind owners by phase: 2B raises the thirteen kinds above; `unsupported_language` is
raised by `read_verilog` when a design file's language has no reader yet (EDIF before Phase 6);
`duplicate_module`, `unresolved_module`, `dependency_cycle`, `no_top`, `ambiguous_top`,
`unknown_top` by 2C/2D from the record's `top`/`top_at`, `libfiles`, `libdirs`, `libext`
and the units they parse, with the oracle's `names` and locations (`unknown_top`/`ambiguous_top`
at `top_at`; `no_top`/automatic `ambiguous_top` unlocated; `duplicate_module` at the second
definition). The §10.2 harness therefore runs those cases only when 2C/2D are present.

## 9. Limits

Each cap is a named constant in `preproc.h` (`ODIN3_PP_*`) or `project.h` (`ODIN3_PROJ_*`), a
located `ODIN3_ERR_PARSE` naming the cap, and has a test that hits it (through a test-only hook
that lowers it, as 2A's `odin3_srcman_test_set_limits` does, so the test runs in milliseconds).

| Cap | Value | Why |
|---|---|---|
| `ODIN3_PP_MAX_INCLUDE_DEPTH` | 64 | real designs nest 2–3; a cycle is caught earlier by name |
| `ODIN3_PP_MAX_EXPANSION_DEPTH` | 1024 | live `EXPANSION` frames; recursion is caught by name (PP-7), so depth measures only distinct-macro chains; 1024 bounds the input stack (≈ 64 KB of frames) |
| `ODIN3_PP_MAX_COND_DEPTH` | 1024 | per file; `nested_ifdef` micros nest 3 |
| `ODIN3_PP_MAX_MACRO_ARGS` | 256 | formals per macro |
| `ODIN3_PP_MAX_MACRO_TEXT` | 2^20 | body bytes; the largest VTR body is under 1 KB |
| `ODIN3_PP_MAX_MACROS` | 2^20 | table entries; mcml.v defines 51 |
| `ODIN3_PP_MAX_STREAM_BYTES` | 2^28 | output bytes per stream, same as the input cap: an expansion bomb (`` `define A B B B B `` …) stops here or at the srcman caps, whichever first; each expansion also costs `len + 1` of the 2^32 location space (AST-2) and one of the 2^24 buffers |
| `ODIN3_PP_MAX_EXPANSIONS` | 2^22 per stream | buffers per stream (the srcman cap is 2^24 for the design); mcml.v uses ≈ 5,000 |
| `ODIN3_PROJ_MAX_FLIST_DEPTH`, `_QIP_DEPTH` | 64 | nested lists; cycles are caught by name first |
| `ODIN3_PROJ_MAX_WORDS` | 2^24 | words per file list (a 2^28-byte list of one-byte words) |
| source limits | AST §3.5 | file size 2^28, buffers 2^24, location space |

Everything else is bounded by these (the macro table is at most `MAX_MACROS` × a bounded
record; an actual is at most the frame's length ≤ `MAX_STREAM_BYTES`).

## 10. Testing

### 10.1 The oracle stays the oracle

`tools/project-fixtures/project_fixtures.py` is normative for the record; the C readers are
checked against it byte for byte (§10.2). Where this spec restricts the oracle's accidental
leniency outside the corpus (line endings, §4.1; counts above 2^32, §3), the oracle is changed
in the same PR (2B-1) so the two agree, and `tests/tools/test_project_fixtures.py` gains a test
per change. No corpus case changes.

### 10.2 Corpus CTest (`project_corpus`)

`project-fixtures compare --odin3 <bin> [--case <c>] [--format <f>] [--with-frontend]`
(stdlib, ruff/mypy clean, its own unit test in `tests/tools/test_project_compare.py` over a
fake `odin3` script): for every case and every format in its `formats` except `odin2` (until
`format_phase` says otherwise), from a temporary working directory, with `env -i PATH=… ` plus the
case's `env`, run `odin3 read_project <entry> [--revision r] ; dump_project` (`--format` from
the corpus format name), relativize every path in the output JSON to the case directory, apply
the format's `format_overrides`, and require equality with `project`, plus the two info lines
when `ignored`/`duplicates` are non-empty. Negative cases: non-zero exit, and stderr's error
matches `kind`, `at` (per format where it is an object) and `names` (§8). Cases whose error
needs the front end (§8's second list, and `neg_include_*`) run through `read_project;
preprocess` for the include kinds (2B-3) and `read_project; read_verilog` for the rest once 2C
lands (`--with-frontend`; skipped, not failed, before). Phase-5 and phase-6 cases are included:
their records need no front end (README "Phases"). Smallest case first (memory rule). CTest
registers it as `project_corpus` beside `project_fixtures`; CI runs it (no external tools
needed).

### 10.3 Unit tests (Unity, ASan/UBSan)

- `test_project_words`: every §4.2 rule, both modes; `test_project_paths`: normalization table
  (`a/./b`, `a/../../b`, `//`, absolute, symlinked directory), missing file and directory.
- `test_project_o3proj`, `_flist`, `_tcl`, `_quartus`: one test per table row of §5, every error
  kind with its `names` and location, `$VAR` timing (a `$X` after `#`), `-sv` scoping across
  `-f` and `-F`, `f_root` for `-f` under `-F`, the Tcl reader on the oracle's docstring cases,
  `.qip` idiom in both forms, `anchored` on `a|b:c` with and without `-entity`, revision
  selection, `import` into an `.o3proj` with a top already set; dump against a literal JSON
  string; OOM injection (`odin3_util_set_alloc_fail_after`) with the record dropped.
- `test_pp_define`: L13's forms (plain, args, defaults, multi-line, nested use, redefinition,
  `//` ending the text, `/* */` inside the text, strings protecting `//`, CRLF continuation);
  `test_pp_expand`: the 2A §3.1 example's buffers and segments exactly (E1 A1 E2 A2 A3; the
  listed `def`/`parent`/`parent_end`); `` x`S `` and `` 8'h`W ``; argument runs across a nested
  body; comments in arguments → `loc` 0; a formal in a string untouched; `` `ADDITION_num ``
  undefined; recursion direct and via argument (PP-7) with the printed chain; too few/many
  actuals; `test_pp_cond`: every `nested_ifdef_*` micro's branch, mid-line conditionals, `elsif`
  chains, nested `ifdef` in a skipped region (gap 22), unbalanced and cross-file `endif`,
  `translate_off` forms and the documented opaque interaction; `test_pp_include`: search order
  (`include_order`: first incdir wins over the second; the includer's directory first), cycle,
  not found, twice-included file, chain printing, `files_read`; `test_pp_directives`: each
  L18–L22 directive passes through verbatim, `resetall` keeps macros, `line` dropped, SV
  operators rejected (PP-9); `test_pp_limits`: every §9 cap through the hook; `test_pp_check`:
  the §7.8 invariants on every micro under `tests/micro/verilog/preprocessor` and `syntax/*define*`,
  plus one corruption per rule through a hidden hook.
- Every test with a located error asserts the exact `file:line:col: error: kind: detail`
  string through a capturing log sink.

### 10.4 Differential tests (`tools/pp-diff`)

`pp-diff [--odin3 bin] [--iverilog /usr/bin/iverilog] [--yosys ~/odin3-ws/external/yosys/build/yosys] files…`
(stdlib; local, not CI; CTest label `differential`, skipped when a tool is absent): for each
file, smallest first, run `odin3 --project <generated .o3proj> preprocess`, `iverilog -E -g2005
[-I…] [-D…]` and Yosys `read_verilog -ppdump` (the dump is in Yosys's log; the tool cuts it out),
tokenize all three with one Verilog tokenizer (identifiers, numbers, strings, operators;
whitespace and comments dropped; `` `line ``, Yosys's `` `file_push``/`` `file_pop `` and
Icarus's `` `line `` lines dropped), and compare Odin III against the two oracles **where they
agree with each other**; where they disagree (Icarus substitutes formals inside strings, Icarus
tolerates undefined macros, Yosys's `-ppdump` drops some pass-through directives) the file is
listed under "oracles disagree" and not failed. Inputs: the preprocessor micros, every micro
with a backtick (`syntax/*`, `keywords/*/*.vh` through their includers), the VTR set ending with
mcml.v; expected: zero disagreements with an agreeing pair, the list of excluded files recorded
in `docs/PHASE2.md` under 2B's results.

### 10.5 Fuzz and property tests

- **Model-based** (`tools/pp-fuzz`, stdlib, seeded): generates random projects (macros with up to
  4 formals and defaults, nesting ≤ 6, conditionals ≤ 4 deep, includes in a temporary tree, the
  pass-through directives, comments and strings sprinkled) together with the expected output of
  a 150-line reference expander written independently in Python, runs `preprocess --segments`
  and compares tokens and, for every output byte with a non-zero loc, that the decoded
  `file:line:col` points at the same byte in the sources (the §7.8 invariant, checked from the
  outside). 1,000 cases in CI (seconds), 100,000 locally.
- **Crash fuzz** (`tests/fuzz/fuzz_pp.c`, libFuzzer, built with `-DODIN3_FUZZ=ON` under
  clang; a 10 s smoke run in CTest, long runs local): the input is a file plus a one-byte
  options header; the harness runs `odin3_pp_run` and `odin3_pp_check` under ASan/UBSan and
  asserts the status is `OK` or `PARSE` (`NO_MEMORY` only under injected allocation failure)
  and `pp_check` passes on every `OK` run; a crash, a leak or a hang (each run is bounded by the
  §9 caps; the fuzzer's timeout is the test) is a finding. Seeds: the micros.
- **Properties**: a file without backticks or `translate_off` preprocesses to itself with one
  segment; `pp(x)` with every macro use replaced by its expansion text re-preprocesses to the
  same tokens (idempotence on directive-free output); the oracle corpus's `resolved.read` equals
  `odin3_pp_files_read` for every Phase 2 positive case.

### 10.6 Benchmark (`tests/bench/bench_pp.c`, built, not in CTest)

Preprocess every VTR benchmark smallest first and mcml.v last (636 KB, 24,507 lines, 51
`define`s, ≈ 2,950 directive lines); report bytes out, segments, buffers, srcman bytes, and time.
Targets in Release on the dev machine: mcml.v ≤ 50 ms (the 2A parse-only budget is 1 s; the
preprocessor should be a twentieth of it), the whole VTR set ≤ 0.5 s, peak memory ≤ 3 × input
bytes plus the srcman's records. Numbers go to `docs/PHASE2.md`.

## 11. Review focus

1. **Segment and run bookkeeping.** A `MACRO_ARG` run that should split at a buffer boundary
   but does not, or a return-to-body segment one byte off, puts a diagnostic on the wrong
   column with no test failing unless the §3.1 example and the §7.8 check are both exact.
   Reviewers should trace `` `OUTER(xx) `` through PP-6 by hand against the 2A table.
2. **Oracle drift in the small.** Word splitting, Tcl continuation, `..` folding, `;` lists in
   `SEARCH_PATH`, the `anchored` rule, `names` order — each is one line in Python and one
   function in C; a divergence outside the corpus is invisible until a user hits it. The §10.5
   generator should also generate project files (a follow-up if 2B-1's review asks).
3. **Directive boundaries.** Token-based conditionals (PP-3), the end of a `define` text with
   comments, strings and continuations, CRLF inside bodies, and `translate_off` as opaque text:
   each has a plausible line-based implementation that passes the micros and fails Icarus's
   mid-line case.
4. **Bombs and caps.** An exponential macro must stop at `MAX_STREAM_BYTES` or `MAX_EXPANSIONS`
   within the fuzzer's timeout, and recursion must be caught by name before any cap; a cap
   checked after the allocation rather than before turns the bomb into an OOM.
5. **Error locations and ordering.** Which word a `-f` cycle, a `qip_cycle`, an
   `unknown_revision` or a `set_parameter -entity` error is located at, and that readers stop
   at the first error in the oracle's order (arity before semantics), decide whether `at`
   matches; every negative case runs in every format for exactly this reason.

## 12. Open points for Peter (recommendation first; each is applied as the default meanwhile)

1. **`--top` versus the project's `top`.** Recommend: the command line replaces the project's
   top with an info line (what every EDA tool does). Alternative: an error when both are given
   and differ.
2. **SystemVerilog macro operators in `.v` files** (PP-9). Recommend: a located error pointing
   at Phase 5. Alternative: implement `` `" `` and ``` `` ``` now with `SCRATCH` buffers (≈ 200
   lines) — nothing in the Phase 2 test space needs them.
3. **Macro argument defaults** (1800 §22.5.1) in Verilog-2005 mode (PP-5). Recommend: accept,
   as the brief asks; it is cheap and rejects nothing legal. Alternative: reject in `.v` files
   and accept only in `.sv`.
4. **Undefined macro** (L14, PP-5). Recommend: error, as Yosys (and L14's REJECT). Alternative:
   Icarus's warning and empty substitution, which hides typos in `ifdef`-heavy code.
5. **When the Odin II XML reader lands** (§5.4, PR 2B-4). Recommend: inside 2B, right after
   2B-3 — the corpus already carries 11 `odin2.xml` cases and the reader is ≈ 600 lines
   including the XML subset; DESIGN §4.0's "after Phase 2" would then be amended to "with 2B".
   Alternative: leave it after Phase 2 as written.
6. **Oracle amendments** (§10.1: LF/CRLF only, counts ≤ 2^32 − 1). Recommend: tighten the oracle
   to match the C where the corpus is silent. Alternative: make the C accept the oracle's
   leniency (Python `splitlines` semantics, big integers), which costs more and matches no tool.
