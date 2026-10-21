# 3E — `write_blif --vtr`: design

Status: v2.4 (2026-10-12): v2.3 plus the 3E-2 final review's amendments (§24: `--writer-log`
pairs C-S2 with the writer's class-U warnings in §16.4's oracle; §15.1's C7 and C8/C9 notes on
repeats VPR accepts). v2.3 (2026-10-12): v2.2 plus the §24 erratum of the 3E Task 7 review (I1:
VPR makes the data ports of memory-class pb_types 1 bit wide; §2's table, R11, §15.2 and §16.4
corrected).
v2.2 (2026-10-12): v2.1 plus the errata of the 3E-1 final review (§23; Peter's ruling on
the round trip, §16.3 step 1). v2.1 (2026-10-11), approved in re-review 2 with touch-ups applied (§14.1 item 6 marked a
3H request; constant LUT objects return as `$_CONST0_`/`$_CONST1_`, §4.2; constants are not traced
through buffers, §9.1). v2: v2 applies review 1 (`3EH-review1`: 5 important, 12 minor; dispositions
in §22) and the controller's rulings: constants as zero-input generators plus checker rule C5b
(E1/E2); the 3D exchange text matched to PHASE3 decisions 9, 12 and 13, with hard-block inputs on
`unconn`/x passing through `abc` unchanged as a requirement on 3D (E3/E4, §14.1); the `$lut`/ISOP
PR split to land right after 3D's `$lut` registry PR, with `$lut`'s interface left to 3D and only
3E's need stated (E5); the declared-index rationale corrected (E7). v1 was the agent draft for
light review (PHASE3 3E row: light; Opus `high`). Phase
3, sub-project 3E, lane 1 (back end), first in 3E → 3D → 3F. Spec: `docs/DESIGN.md` §9 (BLIF, VTR
dialect; black boxes; `.latch` init), §5.4 (netlist view), §13 #6 (name style). Inputs:
`docs/PHASE3.md` (the "3E `write_blif --vtr` rules" paragraph; decisions 2, 5, 6, 9, 12, 14, 18,
19, 20; risks 3, 8, 9), the 1C BLIF spec and the existing writer (`src/backends/blif/writer.[ch]`),
the 1G tech-library spec and `lib/vtr.o3lib`, `docs/IR.md` (IR-3, IR-4, IR-7b, IR-12), the 2D
spec §14.6 (x and `unconn`), the 1F Verilog writer's `--name-style`. **Oracle: VTR `3c9a4d23`**
(`docs/ORACLES.md`): libblifparse (`libs/EXTERNAL/libblifparse/src/blif_lexer.l`,
`blif_parser.y`), `vpr/src/base/read_blif.cpp`, `vpr/src/base/read_circuit.cpp`,
`libs/libarchfpga/src/arch_util.cpp`, and the arch
`vtr_flow/arch/timing/k6_frac_N10_frac_chain_mem32K_40nm.xml`; Parmys's conventions are read from
the goldens under `~/odin3-ws/golden`. External trees are read-only. Decisions are numbered
**VB-n**. Where this file and `docs/IR.md` disagree, IR.md wins.

## 1. Purpose and scope (VB-1)

`write_blif --vtr` writes the design's top module as a BLIF netlist that VPR reads with
`run_vtr_flow.py <design>.blif <arch> -start vpr` (post-ABC netlists: the O and P′ legs) and that
VTR's ABC stage reads with `-start abc` (pre-ABC netlists: the O′ leg and the 3E golden
round trip). It refuses, with located errors and before any byte is written, every construct VPR
would reject or misread. A VPR-free checker gives CI the same verdicts.

| Delivered by 3E | Where |
|---|---|
| `--vtr` dialect of the BLIF writer: options struct, preflight with every refusal, constants and `unconn`, gate cells as `.names`, `.latch … re <clk> <init>`, `.subckt` formals and `.blackbox` stubs from the tech library, VPR-legal names, header comment | `src/backends/blif/vtr.[ch]`, `src/backends/blif/writer.[ch]` |
| Pass arguments `write_blif [--vtr] [--lut-k K] [--name-style short\|provenance] <file>` | `src/passes/builtin.c`, `docs/PASSES.md` |
| The plain writer's constant convention (`$_CONST*_` → `gnd`/`vcc`/`unconn`; PHASE3 #20, 2D §14.6 item 3) | `writer.c` |
| `$lut` → irredundant `.names` cover (after 3D registers `$lut`, decision 9) | `src/backends/blif/isop.[ch]` |
| VPR-free structural checker for CI | `tools/blif-vtr-check/` |
| The arch's `<models>` with port widths, committed for CI | `tools/vtr-models/`, `tests/vtr/arch/*.models.json` |
| Round-trip and VPR-read evidence over the goldens | `tools/blif-roundtrip --vtr`, `tools/vtr-read-check/`, `netlist-compare --vtr-normalize` |

Not in 3E: decomposing a `.names` wider than K (that is `abc`'s or `lower`'s job; 3E refuses
it); flattening (3A); mapping to hard blocks (3C); clock inversion for falling edges (3B,
decision 19); `.eblif` output (`.conn`/`.attr`/`.param`/`.cname`; VPR accepts them only with
`--circuit_format eblif`, and Parmys never writes them for this flow); writing more than one
non-black-box model; reading the arch XML in C (decision 5: Phase 3 needs no C XML reader).

Success: every `status=ok` k6_frac golden (467 Parmys + 456 Odin II) either becomes a `--vtr`
file that VPR reads (`vpr --exit_before_pack on`, §16.4) or is refused with a VB-11 error whose
class VPR also rejects in the original; every Parmys k6_frac golden that is written is
`netlist-compare`-identical to its source; `--vtr` is idempotent through `read_blif` (byte for
byte when no name is legalised, else under `--vtr-normalize`; §16.3 step 1); the checker
agrees with VPR on the whole corpus; the 19 route through `-start abc` (the R leg of 3H); the
gate passes.

## 2. What VPR accepts (VB-2)

Read at `3c9a4d23`. VPR parses BLIF with libblifparse, then builds the atom netlist in
`read_blif.cpp`, then cleans it in `read_circuit.cpp`. Each rule below is a refusal in the writer
(§11) or a convention (§5–§10), and a check in `blif-vtr-check` (§15).

| # | Rule | Source (VTR) | VPR's text |
|---|---|---|---|
| R1 | A name is `([^ \t\r\n\\="]\|\\)*[^ \t\r\n\\="]`, or a `"…"` string. `#` starts a comment only where a token would start (`<*>#.*{ENDL}` loses to the longer STRING match inside a word), so `a#b` is one name to VPR; ABC, however, strips `#` to the end of the line anywhere (relevant to the `-start abc` legs) | `blif_lexer.l` `ID_SET` | `Unrecognized character` / parse error |
| R2 | Inside a `.latch` line the words `re fe ah al as NIL 0 1 2 3` are tokens, not names (lexer state `LATCH`); a net with such a name cannot appear on a `.latch` line | `blif_lexer.l` `<LATCH>` | parse error |
| R3 | Directives: `.model .inputs .outputs .names .latch .subckt .blackbox .end`; `.conn .attr .param .cname` only in eblif mode; anything else (`.clock`, `.gate`, `.exdc`, …) is a syntax error | `blif_lexer.l`, `read_blif.cpp` `conn`/`attr`/`param`/`cname` | `Supported only in extended BLIF format` |
| R4 | `.names`: at most K inputs, K = the arch's `.names` pb_type width (6 here) | `read_blif.cpp` `names()` | `BLIF .names input size (%zu) greater than .names model input size (%d)` |
| R5 | A cover row has one plane character per input and one output character | `blif_parser.y` | `Mismatched .names single-output cover row. names connected to %zu net(s), but cover row has %zu element(s)` |
| R6 | `.latch`: type `re`; any other type is fatal. The untyped forms (`.latch D Q`, `.latch D Q init`) carry no control, so they always fail R7; none is ever "treated as `re`" in practice | `read_blif.cpp` `latch()` | `Only rising edge latches supported` |
| R7 | `.latch`: a control (clock) net is required; `NIL` counts as none | `read_blif.cpp` `latch()` | `Latch must have a clock` |
| R5b | A `.names` with one connected input and one connected output must have exactly one cover row: `absorb_buffer_luts` (on by default) calls `is_buffer_lut`, which asserts it | `read_circuit.cpp`, `atom_netlist_utils.cpp` `is_buffer_lut` | assertion `One truth-table row` |
| R8 | `.latch` init ∈ {0, 1, 2, 3} (2 = don't care, 3 = unknown) | `blif_parser.y` `latch_init` | parse error |
| R9 | `.subckt` model must be an arch `<model>` | `read_blif.cpp` `subckt()` | `Subckt instantiates model '%s', but no such model exists in the architecture file.` |
| R10 | `.subckt` formal `p` or `p[i]` (index split from the end, `p` = bit 0) must be a port of that model with `i` < the port's size | `read_blif.cpp` `find_model_port`, `split_index` | `Found no matching port '%s' on architecture model '%s'` / `Port '%s' on architecture model '%s' exceeds port width (%d bits)` |
| R11 | A model port's size is the largest `num_pins` among the pb_types whose `blif_model` is `.subckt <model>`, except that the `port_class="data*"` ports of a `class="memory"` pb_type count 1: `ProcessMemoryClass` moves its `blif_model` to a `memory_slice` child whose data pins it forces to one bit, before `SyncModelsPbTypes` sizes the model (§24) | `arch_util.cpp` (`ProcessMemoryClass`; `SyncModelsPbTypes`: model_port->size = max) | — |
| R12 | Block names: `.names` and `.latch` blocks take their output net's name, a `.subckt` the net of its first **output formal in written order**, outpads `out:<name>`; so formal order decides `.subckt` block names; two blocks with one name are an error | `read_blif.cpp` `subckt()`, `outputs()` | `Duplicate blocks named '%s' found in netlist. Existing block of type '%s' conflicts with subckt of type '%s'.` |
| R13 | Exactly one non-black-box model (the first one found is the top); every `.blackbox` model must be an arch model whose declared ports exist | `read_blif.cpp` `determine_main_netlist_index`, `blackbox()`, `verify_blackbox_model` | `Found multiple models with primitives. Only one model can contain primitives, the others must be blackboxes.` / `Blackbox BLIF model '%s' has no equivalent architecture model.` |
| R14 | The net named `unconn` and the block named `unconn` are deleted after reading; their pins stay with no net (open) | `read_circuit.cpp` `process_circuit` | warning at verbosity > 1 |
| R15 | A zero-input `.names` with no rows (or row `0`) is a constant-0 generator, with row `1` constant 1; VPR merges and sweeps constant generators. `truth_table_constant_value` also flags an empty table *with* inputs as constant 0, but not `-- 1` over inputs; 3E therefore writes every constant as a zero-input generator (§6) | `read_blif.cpp` `names()`, `read_circuit.cpp` | info |
| R16 | One driver per net: a second driver aborts while the netlist is built | `netlist.tpp` `associate_pin_with_net` | assertion `Must be no existing net driver` |

The arch's `<models>` (k6_frac and EArch alike) and the port sizes R11 derives (computed
2026-10-11 from the k6_frac XML; EArch gives the same set; the RAM data ports corrected to 1 on
2026-10-12, §24, from the Task 7 review; `tests/vtr/arch/*.models.json` hold the extracted sizes):

| Model | Inputs (size) | Outputs (size) | Clock |
|---|---|---|---|
| `adder` | `a` 1, `b` 1, `cin` 1 | `cout` 1, `sumout` 1 | — |
| `multiply` | `a` 36, `b` 36 | `out` 72 | — |

The `multiply` row is the arch's model (its widest instance). The library cell `lib/vtr.o3lib`
`multiply` is VTR's square cell: one `WIDTH` parameter, `a`/`b` of `WIDTH` bits, `out` of `2*WIDTH`.
An instance is written at its own `WIDTH`; only the stub is written at the largest `WIDTH` used
(PHASE3 decision 29; the "36 bits" wording elsewhere is the arch cap, not a fixed cell). The
upper-bit `unconn` padding of §9.2 applies only to a `WIDTH`-sized instance whose operands are
narrower than `WIDTH`.

| `single_port_ram` | `we` 1, `addr` 15, `data` 1, `clk` 1 | `out` 1 | `clk` |
| `dual_port_ram` | `we1` 1, `we2` 1, `addr1` 15, `addr2` 15, `data1` 1, `data2` 1, `clk` 1 | `out1` 1, `out2` 1 | `clk` |

K is 6 on both arches (`determine_lut_size`; EArch's largest `.names` pb_type is also 6).

Consequence of R14 for 3E (VB-2a): `unconn` means "open pin" to VPR but "constant 0" to ABC
(it is a zero-row `.names`). Parmys relies on both: hard-block inputs on `unconn` stay open in
VPR, and `unconn` on logic disappears into constants in VTR's ABC stage before VPR sees it. A
post-ABC netlist that still feeds `unconn` to a `.names` or `.latch` pin gives VPR an open LUT or
FF input, whose function is undefined. §9 therefore uses `unconn` only on hard-block inputs.

## 3. Parmys's conventions, as the goldens show them (VB-3)

Survey of the 21 `vtr/` Parmys goldens on k6_frac (the 19 plus `arm_core` and `spree`) and of all
923 `status=ok` k6_frac goldens, 2026-10-11 (streaming `awk`, read-only):

1. **Header**: `# Generated by Yosys 0.55 (…)`, a blank line, then `.model <top>`.
2. **Constants first**: `.names gnd` (no rows), `.names vcc` with row `1`, `.names unconn` (no
   rows), always all three, even when unused. A constant reaching an output is a buffer
   (`.names unconn d` + `1 1`; `.names vcc <net>` + `1 1`).
3. **Latches**: every one of the 21 has only `.latch <D> <Q> re <clk> 2` (PHASE3 #12 counts
   172,301 such latches over the 31 `vtr/` files). `stereovision3` has two clocks, the rest one.
   Across all 467 Parmys k6 goldens, 4 files hold `fe` latches (micros:
   `keywords/negedge/negedge`, `keywords/begin_end/negedge/negedge`, `common/register/register`,
   `syntax/bm_jk_rtl/bm_jk_rtl`); VPR rejects those (R6).
4. **`.names`**: at most 3 inputs in every Parmys golden (pre-ABC gates); on-set covers.
5. **`.subckt`**: every formal bit is written, formals sorted by name (`addr[0]` … `clk` `data`
   `out` `we`); unused hard-block inputs are `unconn` (upper `multiply` bits, RAM address bits
   above the used depth, unused `adder` operands, `data2`/`we2` on `gnd`); every output bit gets
   its own net, used or not. 1-bit ports are scalar (`data=`, `we=`, `out=` on 1-bit RAM slices),
   wider ones indexed (`addr[14]`).
6. **Adder chains** start with a dummy head `.subckt adder a=vcc b=gnd cin=gnd cout=… sumout=…`
   (decision 4a).
7. **Stubs**: after the top's `.end`, one `.model <m>` / `.inputs …` / `.outputs …` / `.blackbox`
   / `.end` per hard model used, separated by blank lines, port order varying per design (IR-7b
   accepts any order).
8. **No** `.conn`, `.attr`, `.param`, `.cname`, `.clock` in any k6 golden; no line continuations
   in Parmys files; one non-black-box model everywhere.
9. **Names**: `$auto$rtlil.cc:3203:MuxGate$1084`, `memory_controller_address~26`,
   `single_port_ram^MEM~0-7^out~0`: all VPR-legal; port bits are Yosys's `name~index` scalars.
   **Erratum (3E-1 final review I1):** 25 of the 467 also hold a mid-name `\`
   (`$techmap$auto$hard_block.cc:122:cell_hard_block$54.$2\buffer[7:0][0]`, from shift
   lowering; `shiftx` and the `*_variable_*_wide` operators). VPR reads it (R1), but VB-10 rule
   1 escapes it (§10), so these files are renamed and their round trip is compared under
   `--vtr-normalize` (§16.3 step 1).

Odin II goldens (456 on k6): latch init `3` (211 files), `0`/`1` (2 each), `fe` in 6 files;
92 files have a `.names` wider than 6 (VPR rejects them, R4); `\` continuations without a space
before the backslash (legal under R1).

## 4. Interface (VB-4)

### 4.1 C

In `src/backends/blif/writer.h` (internal header; no public ABI change):

```c
/* BLIF dialects (spec 3E VB-4). */
typedef enum odin3_blif_dialect {
    ODIN3_BLIF_DIALECT_PLAIN = 0, /* 1C: every module, IR round trip */
    ODIN3_BLIF_DIALECT_VTR = 1,   /* 3E: the top only, what VPR and VTR's ABC stage read */
} odin3_blif_dialect;

/* Net and cell names in the VTR dialect (spec 3E VB-10; the 1F Verilog writer's styles). */
typedef enum odin3_blif_name_style {
    ODIN3_BLIF_NAMES_SHORT = 0,      /* own name, else generated (default) */
    ODIN3_BLIF_NAMES_PROVENANCE = 1, /* hier/<short name>@file:line where provenance has one */
} odin3_blif_name_style;

typedef struct odin3_blif_write_opts {
    odin3_blif_dialect dialect;
    odin3_blif_name_style name_style; /* VTR only; PLAIN requires SHORT */
    uint32_t lut_k;                   /* VTR only: max .names inputs; 0 = the default */
} odin3_blif_write_opts;

#define ODIN3_BLIF_VTR_DEFAULT_K 6U   /* k6_frac_N10_frac_chain_mem32K_40nm and EArch */
#define ODIN3_BLIF_VTR_MIN_K 2U
#define ODIN3_BLIF_VTR_MAX_K 16U      /* VB-12 */
#define ODIN3_BLIF_VTR_MAX_ERRORS 20U /* refusals logged before "N more" */

/* Writes design to path in opts' dialect; opts NULL = PLAIN with defaults. odin3_blif_write is
 * odin3_blif_write_with(design, path, NULL). VTR: runs odin3_blif_vtr_preflight first and opens
 * path only when it passes, so a refused design leaves no file. Errors: as odin3_blif_write,
 * plus ODIN3_ERR_INVALID_ARG for a preflight refusal or an option out of range. */
odin3_status odin3_blif_write_with(const odin3_design *design, const char *path,
                                   const odin3_blif_write_opts *opts);
```

Amendment (3E-1 Task 1): the constants' suffixes are upper case (`6U`, not `6u`), as the lint
gate's clang-tidy check `readability-uppercase-literal-suffix` requires.

In `src/backends/blif/vtr.h`:

```c
/* What the VTR preflight found (spec 3E VB-11); the counts feed the info lines and the tests. */
typedef struct odin3_blif_vtr_report {
    uint32_t errors;           /* refusals; > 0 means nothing may be written */
    uint32_t names, latches, subckts; /* cells the write will emit */
    uint32_t x_to_gnd;         /* x/undriven bits on .names/.latch/output pins written as gnd */
    uint32_t x_to_unconn;      /* x/undriven/unconnected hard-block input bits written as unconn */
    uint32_t unconn_logic;     /* .names/.latch input pins on a net named 'unconn' (VB-2a) */
    uint32_t renamed;          /* names legalised by VB-10 */
    uint32_t modules_skipped;  /* modules other than the top, not written */
} odin3_blif_vtr_report;

/* Checks that the design's top can be written in the VTR dialect with opts (lut_k resolved).
 * Logs each refusal (VB-11, at most ODIN3_BLIF_VTR_MAX_ERRORS, then one count line) and fills
 * report. When nothing is refused it also logs the clean run's lines, in this order: §5's
 * modules-skipped info line, the constant and `unconn` clock warnings (§7 item 3, §8.2), the
 * `unconn`-on-logic warning (§9.2), the renaming warnings and info line (§10 items 5 and 9) and
 * the summary line (§11); the write logs none of them again (3E-1 final review M6). Returns ODIN3_OK when report->errors == 0, ODIN3_ERR_INVALID_ARG when it is not or an
 * argument is NULL, ODIN3_ERR_NO_MEMORY. Reads only; O(nodes + pins). */
odin3_status odin3_blif_vtr_preflight(const odin3_design *design,
                                      const odin3_blif_write_opts *opts,
                                      odin3_blif_vtr_report *report);

/* VB-10: appends the VPR-legal form of name to out (the identity for a legal name). */
odin3_status odin3_blif_vtr_legal_name(odin3_bytes name, odin3_strbuf *out);
```

In `src/backends/blif/isop.h` (3E-5):

```c
/* An irredundant sum-of-products of the k-input function whose truth table is tt (bit i = f(i),
 * where bit j of the index i is the value of input j), by the Minato-Morreale ISOP on an explicit
 * stack (no recursion), for on-set and off-set; the shorter is chosen (on-set on a tie). Rows are
 * appended to cover as BLIF plane text (k characters of 0/1/-, character j for input j, then the
 * output character), in the deterministic order the algorithm emits them. A constant function
 * gives *rows = 0 and *constant = 0 or 1 (the caller writes a zero-input generator, §6); otherwise
 * *constant = -1. k <= ODIN3_BLIF_VTR_MAX_K. */
odin3_status odin3_blif_isop(const uint64_t *tt, uint32_t k, odin3_strbuf *cover,
                             uint32_t *rows, int *constant);
```

### 4.2 What 3E needs from 3D's `$lut` (a requirement, no more)

The `$lut` cell's name, parameter names, port names and mask encoding are 3D's (PHASE3 decision
9; DESIGN §5.2 also mentions `$_LUT_K_`, and 3D's spec settles the spelling). 3E-5 requires only:

1. a bit-granularity registry cell with k inputs (1 ≤ k ≤ 16) and one output; a **constant**
   object (0 fan-in, which MiniLUT can return) comes back as `$_CONST0_`/`$_CONST1_`, never as
   a k = 0 `$lut` (§9 then writes it as `gnd`/`vcc`);
2. a way to read its function as a 2^k-bit truth table, with a stated correspondence between
   truth-table index bits and input pins.

3E writes the `.names` inputs in that pin order and maps index bit j to cover column j; nothing
else about the cell is assumed.

### 4.3 Pass and CLI

```
write_blif [--vtr] [--lut-k <K>] [--name-style short|provenance] <file>
```

Options and the path in any order (like `write_verilog`). Amendment (3E-1 Task 1 review): only a word starting with `--` (and longer) is an option, and a
lone `--` ends the options, so a plain path starting with one `-` is written as before 3E. `--lut-k` and `--name-style` need
`--vtr`. `--lut-k` default `ODIN3_BLIF_VTR_DEFAULT_K`; 3F's wrapper and 3H pass VTR's own
`determine_lut_size(arch)`. Help string: `write_blif [--vtr] [--lut-k K] [--name-style
short|provenance] <file>: write the design as BLIF (the top first); --vtr writes the top alone in
the dialect VPR reads`. `docs/PASSES.md`'s `write_blif` row is rewritten accordingly (§20).

The VTR flow script (decision 3, 3F's wrapper) ends `… abc --lut 6; write_blif --vtr
<design>.blif`; the O′ leg ends `… lower; write_blif --vtr <design>.blif`.

## 5. Output layout (VB-5)

In this order, byte-deterministic (§13):

1. **Header comment**, one line per item, no dates and no host names (byte-identical reruns):
   - `# Odin III <odin3_version_string()> write_blif --vtr (K=<K>, names <short|provenance>)`
   - one `# abc: <facts>` per `abc` pass run in pass-run order, `<facts>` being what 3D's spec
     records in the pass-run table (decision 14: script, its SHA-256, ABC commit, K; the exact
     string format is 3D's), with the script path printed as given or repo-relative, never
     resolved to an absolute path (§13: no working-directory path in the file); none before 3D
     lands.
   - a blank line.
2. `.model <top>`, the name in VB-10 rules 1 and 2 (§10 item 10).
3. `.inputs` and `.outputs` exactly as the plain writer (port order; one line per run of one
   direction; ` \` wrapping at 100 columns), with VB-10 names. **No `.clock`** (R3).
4. **Constant generators** (§9): `.names gnd`, then `.names vcc` + `1`, then `.names unconn`, each
   emitted only when referenced and not already present as a written `$sop` (§9.3).
5. **Cells** in node ID order, port nodes skipped (§6).
6. **Port buffers** as the plain writer, plus §9.2's constant-to-output buffers.
7. `.end`.
8. **Stubs** (§8.3): for each hard cell type used by a live node of the top, sorted by name in
   byte order, a blank line, `.model <m>`, `.inputs …`, `.outputs …`, `.blackbox`, `.end`.

**No eblif directives**: `.cname`, `.attr`, `.param` and `.conn` are never written (VPR reads
them only with `--circuit_format eblif`, §1). The `.cname`/`.attr`/`.param` lines the plain
dialect writes for a cell are dropped under `--vtr` (agent default, PHASE3 #21; 3E-1 final review
M5).

Modules other than the top are not written (R13); when there are any, the info line
`write_blif: --vtr: N modules besides the top '<top>' not written` is logged. An instance of one
of them in the top is a refusal (§11, E10).

## 6. Cells (VB-6)

| IR cell (registry name) | `--vtr` output | Notes |
|---|---|---|
| `$port_in`, `$port_out` | `.inputs` / `.outputs` | IR-3 |
| `$port_inout` | refused: E9 | VPR has no bidirectional top pins |
| `$sop` (N inputs, cover) | `.names` with the cover as stored, after the normalisation below | N ≤ K (E7); every row the same output character (E8); a zero-input `$sop` is a constant generator kept as written |
| `$_BUF_` A Y | `.names A Y` / `1 1` | |
| `$_NOT_` A Y | `.names A Y` / `0 1` | Yosys `write_blif` covers (backends/blif/blif.cc), verified 2026-10-11 |
| `$_AND_` A B Y | `11 1` | |
| `$_OR_` | `1- 1`, `-1 1` | |
| `$_XOR_` | `10 1`, `01 1` | |
| `$_NAND_` | `0- 1`, `-0 1` | |
| `$_NOR_` | `00 1` | |
| `$_XNOR_` | `11 1`, `00 1` | |
| `$_MUX_` A B S Y | `.names A B S Y` / `1-0 1`, `-11 1` | needs K ≥ 3 (E7 otherwise) |
| `$lut` (3D's cell, §4.2) | `.names` with `odin3_blif_isop`'s cover | 3E-5; k ≤ K (E7); a constant LUT is written as a zero-input generator (below) |
| `$_CONST0_`, `$_CONST1_`, `$_CONSTX_`, `$_CONSTZ_` | no line of their own: their nets become `gnd`/`vcc`/`unconn` per §9 | |
| `$_DFF_P_` C D Q, `INIT` | `.latch D Q re C <init>` | §7 |
| `$_DFF_N_` | refused: E1 | `lower` inverts the clock (decision 19) |
| `$_DFFSR_*` and any flip-flop with asynchronous set, reset or load (registry or IR change D) | refused: E1b | `lower` makes it synchronous (decision 19) |
| `$_DLATCH_P_`, `$_DLATCH_N_` | refused: E2 | |
| `$_FF_` (no clock) | refused: E3 | R7 |
| `$tribuf` and any registry cell with a `z`-capable output | refused: E4 | |
| any other registry (word) cell: `$add`, `$mux`, `$dff`, `$mem`, … | refused: E5 | the netlist view (IR-9) does not hold |
| a library cell of kind `hard` (1G) | `.subckt <m> …` | §8 |
| a library cell of kind `gate` (`generic_gates.o3lib`) | refused: E6 | no arch model of that name |
| an implicit black box (no library cell, no declared model) | refused: E6 | |
| a declared black-box model (from `read_blif`) that is a library hard cell | `.subckt`, stub as declared (§8.3) | the golden round trip |
| a declared black-box model that is not a library hard cell | refused: E6 | unless `--vtr` is given `read_techlib lib/vtr.o3lib` first |
| a module (hierarchy not flattened) | refused: E10 | 3A `flatten` runs first |

Unconnected and undriven input pins (§9.1) are never written as a dangling generated name: a
dangling `$p<pin ID>` net has no driver in VPR and sweeps unpredictably.

**Constant and 1-input normalisation** (R5b, R15; review E1). Before writing, each `$sop`,
gate and `$lut` with at least one input is evaluated on its truth table (k ≤ 16):

1. A **constant** function (including a `$sop` with inputs and no rows, and `-- 1` covers) is
   written as a **zero-input generator** on its output net: `.names <y>` with no rows for 0, or
   with the row `1` for 1. Its input pins are dropped. When that net is a reserved constant name
   or an output-port bit, §9.3 and §9.2 apply (one driver, no extra buffer).
2. A **1-input** non-constant function is written with exactly one row: `1 1` (buffer) or `0 1`
   (inverter), whatever the stored cover held (0 rows is case 1; two or more rows are reduced).
3. Every other cover is written as stored (`$sop`) or as `odin3_blif_isop` gives it (`$lut`).

Unit tests cover a 1-input `$sop` with 0, 1, 2 and 3 rows, a constant 2-input `$sop` (`-- 1`, no
rows), a constant `$lut` of each width class, and a 1-input `$lut`.

E7 counts a cell's **stored** inputs and runs before this normalisation (plan 3E point (g)), so a
cell with more than K inputs whose function is constant is refused rather than written as a
generator. `abc` never produces one, so this is noted, not changed (3E-1 final review M10).

## 7. Latches (VB-7)

1. **Form**: `.latch <D> <Q> re <C> <init>`; all five fields always (VPR's warning for an untyped
   latch is avoided, R6).
2. **Init**: the node's `INIT` parameter: 0 → `0`, 1 → `1`, 2 or 3 → `2`. Parmys writes `2` for
   every latch, and VTR's ABC rejects `3` (PHASE3's 3E rules); VPR accepts all four. Odin III's
   own latches carry 0/1 only from an `initial` value, else 2 (2D EL-40, 2F §9.4).
3. **Clock**: the `C` pin's net. Unconnected or undriven C is E3 (VPR "Latch must have a clock").
   A constant C (`$_CONST*_` or a constant `$sop`) is written (VPR accepts it) with the warning
   `write_blif: --vtr: <loc>: $_DFF_P_ '<cell>': clock is the constant <0|1|x>; VPR will never
   clock it`. A C on `unconn` (class U, §9.1) is **E3b** (amended in 3E-4, PHASE3 #34; it was a warning
   with `re unconn` under plan point (j)). VPR's `latch()` accepts it and `process_circuit`
   removes the `unconn` net (R14), so `--exit_before_pack` reads it, but the full flow (pack,
   place, route) aborts: `timing_graph_builder.cpp:546 … Assertion 'clk_pin' failed` (exit 134),
   as it does for a hard block with `clk=unconn`. A written file could never be routed, so the
   writer refuses it with the E3b text (`clock input 'C' is unconnected or x`). VPR accepts G at
   read time, so the refusal is `listed` in §16.4.
4. **D**: an x, unconnected or undriven D is written as `gnd` (counted in `x_to_gnd`), the value
   ABC assigns it (decision 12: "DC treated as 0").
5. **Q**: an unconnected Q gets its own net (`$p<pin ID>`-style generated name, VB-10), since a
   latch block is named after its output net (R12).
6. **R2**: the D, Q and C names never lex as latch tokens; VB-10 item 3 renames any net called
   `re fe ah al as NIL 0 1 2 3`.

## 8. Hard blocks: `.subckt` and stubs (VB-8)

### 8.1 Which types

A `.subckt` type must be a tech-library cell of kind `hard` registered by `read_techlib` (1G),
i.e. a cell of `lib/vtr.o3lib` for the VTR flow, or a declared black-box model (from
`read_blif`) whose name is such a cell (IR-7b's compatibility check already ties the two).
Whether the library cell exists in the arch's `<models>` is not the writer's to know (no arch in
C): `tools/vtr-models --check-lib` (§15.2, run by 3H and nightly) checks `lib/vtr.o3lib` against
the arch with VTR's own `determine_*` readers, and `blif-vtr-check` checks every written file.

### 8.2 Formals

Formals are written **sorted as Parmys sorts them**: by port name in byte order, then by bit
index numerically (`addr[0]` … `addr[14]` `clk` `data` `out` `we`). This keeps `.subckt` block
names (R12: the first output formal in written order) and line contents equal to Parmys's on a
round-tripped golden, so the R leg (§16.5) measures no avoidable order noise. Stubs keep their
own order (§8.3). For each bit `k` in `0 … width−1` of each port of the node (the width the
node's parameters give):

- the formal is the port name when the port's width expression is the literal `1` **or** the
  node's width for that port is 1 (Parmys's scalar `data=`, §3 item 5), else `<port>[k]`; a
  declared model's scalar flags win when the type is declared (round trip); **erratum (Task 5
  review I1):** for an undeclared type the choice is per type and port, not per node: `p` only
  when the widest width any live node gives the port is 1 (the stub's rule, §8.3), so every
  instance agrees with the stub and our reader (IR-7b) reads the file back;
- an **input** bit's actual is its net's VB-10 name; when the pin is unconnected, its net
  undriven, or its net x (`$_CONSTX_`, `$_CONSTZ_`), the actual is `unconn` (counted in
  `x_to_unconn`): VPR leaves the pin open (R14), as Parmys does;
- an **output** bit's actual is its net's name; an unconnected output pin gets a fresh generated
  net, so every output bit is written (Parmys's form, and the block's VPR name is stable: R12);
- a **clock** input (a port with the `.o3lib` `clock` flag, e.g. the RAMs' `clk`) follows the
  latch C rule of §7 item 3, not the hard-block input rule: x, unconnected, undriven or `unconn` is E3b, a
  constant is written with the §7 warning (a sequential block with an open clock would be
  meaningless).

Bits beyond the node's width are not written: VPR leaves them open, and the node's parameters are
recoverable from the written formals (1C's rule; the writer's existing
`odin3_celltype_infer_params` check stays, E11). Widths above the arch's (R10) are caught by the
checker, not here (§8.1).

### 8.3 Stubs

One stub per hard type used by a live node of the top, sorted by type name:

- a **declared** type is written as declared (its port order, widths and scalar flags), the 1C
  writer's `write_blackboxes` per declaration — only the used ones (R13 verifies every stub
  against the arch, so an unused stub for a model the arch lacks would be fatal);
- an **undeclared** library cell gets a stub with its library port order, each port at the
  largest width any live instance uses (per port, the maximum over nodes of the node's width),
  scalar exactly when that maximum is 1, else `p[0] … p[w−1]`: the same per-type choice every
  instance's formals make (the §8.2 erratum; a port whose width expression is the literal `1`
  always has maximum 1).

VPR checks the stub's ports against the arch (R13), never against the instances; 1C's reader
checks the instances against the stub (IR-7b), which the maximum-width rule satisfies.

## 9. Constants, x and `unconn` (VB-9)

### 9.1 Classification of an input pin's source

Each input pin of a written cell, and each output-port bit, has one source class, from its net:

| Source | Class |
|---|---|
| driven by `$_CONST0_`, or a constant-0 `$sop` (`odin3_net_const_value` 0) not named `unconn` | **0** |
| driven by `$_CONST1_`, or a constant-1 `$sop` | **1** |
| driven by `$_CONSTX_` or `$_CONSTZ_` (Yosys `write_blif -undef` treats z as undef too) | **x** |
| no net (unconnected pin), or a net with no driver that is not a `$port_in` bit | **x** (undriven) |
| a net named `unconn` driven by a zero-row `$sop` (read from a Parmys golden) | **U** |
| anything else | **signal** |

Only the **driving cell's own function** counts: a net driven by a buffer (or any non-constant
cell) whose input is a constant is class *signal*, not 0/1. Constants are not traced through
buffers, so Parmys's `.names vcc d` / `1 1` stays a buffer and is not rewritten as `.names d` /
`1` (`netlist-compare` identity on the goldens, §16.3).

### 9.2 What each class is written as

| Class | `.names` input, `.latch` D | `.latch` C | `.subckt` input | output-port bit |
|---|---|---|---|---|
| 0 | `gnd` | `gnd` + warning (§7) | `gnd` | buffer `.names gnd <port bit>` / `1 1` |
| 1 | `vcc` | `vcc` + warning | `vcc` | buffer from `vcc` |
| x | `gnd` (`x_to_gnd`) | E3 | `unconn` (`x_to_unconn`); a clock input: E3b | buffer from `gnd` (`x_to_gnd`) |
| U | `unconn` as read, counted in `unconn_logic`, one warning (below) | E3b (3E-4) | `unconn` (a clock input: E3b) | buffer from `unconn` as read |
| signal | its name | its name | its name | 1C's port-bit naming |

An output-port bit whose net is itself driven by a constant `$sop` or a normalised constant cell
(§6) is written as that generator under the port bit's name (`.names <port bit>`, no rows or row
`1`), with no buffer: emitting both would give the port net two drivers (R16). Buffers from
`gnd`/`vcc` are used only when the constant comes from a `$_CONST*_` node shared with other
nets.

An output-port bit that a hard cell's (`.subckt`) output pin drives is written through a buffer:
the box output gets a net of its own (`$n<ID>`) and `.names <net> <port bit>` / `1 1` joins it to
the port (the existing port buffer, no second mechanism). VTR's ABC step (`strash; ifraig; scorr;
...; write_hie`) drops a primary output that a box drives directly, and the boxes behind it with it
(mkPktMerge: 153 of 156 outputs, all 459 RAMs). Parmys has logic between box and port, so it never
hits this. The info line's `.names` count includes these buffers.

The 0/1 rows keep Parmys's buffer form (§3 item 2). The x rows are the VPR-safe form of 2D
§14.6's convention: x is 0 wherever logic sees it (what VTR's ABC makes of Parmys's `unconn`), and
an open pin on hard blocks, the only place Parmys's `unconn` reaches VPR.

Class U is faithful to the input: a Parmys golden read back writes its own `unconn` wiring
unchanged, so the golden round trip stays identical (§16.3); one warning per write, if any:
`write_blif: --vtr: net 'unconn' feeds N .names/.latch pins; VPR leaves them open (run abc
first)`. After `abc`, ABC has folded those pins (it reads `unconn` as constant 0), so the O and P′
legs never warn.

### 9.3 The generators

`gnd`, `vcc`, `unconn` are the reserved constant names (VB-10). A written zero-input `$sop`
whose own net is named `gnd` (resp. `vcc`, `unconn`) and whose value is 0 (resp. 1, 0) *is* that
generator: it is written as stored and §5 item 4 emits nothing more for that name. Otherwise the
generator is emitted at §5 item 4 iff some pin or buffer uses the name. A net that carries a
reserved name but is not such a generator is renamed by VB-10 item 3 (info line). Each name thus
has exactly one driver (R16).

### 9.4 Plain mode (PHASE3 #20, 2D §14.6 item 3)

This lands in its own PR, 3E-3 (§18): 2F's spec (line "Odin III's BLIF writer adopts `unconn`
when `lower` lands … not 2F's change") confirms 2F has not done it, 2H depends on it, and it
touches 1C/1F fixtures. The plain writer changes only here: a `$_CONST0_`/`$_CONST1_` net is
written as `gnd`/`vcc` and
a `$_CONSTX_`/`$_CONSTZ_` net as `unconn` everywhere (not only on hard-block inputs), with the
generators of §9.3, instead of today's `.subckt $_CONST0_ Y=…`. That is Parmys's `write_blif -true
+ vcc -false + gnd -undef + unconn`, which the Phase 2 exit comparison (2H) needs. Goldens
contain no `$_CONST*_` cell (the reader keeps constants as `$sop`), so 1C's round trip is
unaffected; Odin III-built fixtures that expected `.subckt $_CONST0_` are updated in 3E-3.

## 10. Names (VB-10)

1. **Legal bytes**: `0x21`–`0x7E` except `=` `"` `#` `\` `%` (R1; `%` is the escape). Any other
   byte (blank, control, non-ASCII, the five) is written `%XX` (two upper-case hex digits). `#`
   is escaped although VPR would read it mid-name (R1), because VTR's ABC stage and our own
   `abc --via-blif` exchange strip `#` to the end of the line. `\` is escaped everywhere too,
   although VPR reads it mid-name (R1; only a `\` before the end of the line is a continuation);
   the escape set stays as it is (Peter, 2026-10-12, 3E-1 final review I1). The cost: rule 1
   escapes the `%` of its own escapes on a second write (`\` → `%5C` → `%255C`), so a renamed
   design is not byte-idempotent through `read_blif` (§16.3 step 1).
2. **Leading `.`** is written `%2E` (a name equal to a directive would lex as one).
3. **Reserved whole names**: `re fe ah al as NIL 0 1 2 3` (R2), `gnd vcc unconn` (VB-9, unless
   the net is that generator), and any name starting `out:` (R12: outpad blocks are
   `out:<output>`) are prefixed `%` (`%re`, `%unconn`, `%out:x`). `%` followed by anything but two
   hex digits never comes from rule 1, so the mapping stays injective.
4. **Uniqueness**: legalisation runs before 1C's uniqueness pass, so a legalised name that
   collides with another net's name falls back to `$n<ID>`, `$n<ID>$1`, … as today.
5. **Port bit names** use the wire's declared index (IR wire spec `{msb, lsb}`: bit k is
   `lsb + k` descending, `lsb − k` ascending; 2D EL-21), so `a[8:1]` writes `a[1] … a[8]`. The
   reason is parity with Yosys and Parmys, whose `write_blif` names bits by the wire's
   `start_offset` index, so O and P netlists name the same port bits alike. It does *not* help
   Verilator: `mosaic/verilator_check/blif_to_verilog.py` (`pack_bit_blasted_top_ports`) declares
   every packed port `[max:0]` and the testbench drives RTL-width vectors positionally, so a top
   port whose lsb is not 0 would be off by its lsb on P and O alike (silently, under
   `-Wno-WIDTHEXPAND`). None of the 19 has such a top port (or1200's `[31:2]` ports are on
   submodules; its top is `or1200_flat`). A BLIF-read design's wires are `[w−1:0]`, so goldens
   are unaffected. A port's own legalised name is reported once per
   port: `write_blif: --vtr: port '<p>' written as '<legal>' (not a legal name for VTR's BLIF
   readers)` (3E-1 final review M12: VPR itself reads `re` off a `.latch` line and a mid-name
   `\`, and `gnd`/`vcc`/`unconn`/`out:` are our conventions, so the text names VPR and VTR's ABC
   stage together; a port that only rule 8 renames gets the clock-domain text of item 8).
6. **Short** (default): the net's own name, else its primary wire bit, else `$n<net ID>` (1C).
7. **Provenance** (`--name-style provenance`, 3E-6): `hier/<short name>@file:line` from the
   net's provenance record, as the 1F Verilog writer builds it, then rules 1–4 (blanks in paths
   become `%20`). Rules: top-level **port bits keep their short names** (pads, `.inputs`/
   `.outputs` and Verilator's port matcher depend on them); the location is the one the
   **deterministic "first location" rule of 1F/3D** picks among the record's sources (for a
   DERIVED LUT with many parents, 3D's spec must define it; 3E-6 cites it and adds no rule of its
   own); the file is printed **as given** (never `--diag-paths resolved`, which would put `/home/…`
   into VPR names and break byte identity across checkouts). Objects without a location keep
   their short name. DESIGN §13 #6: 3H measures VPR time and memory with both styles; short stays
   the default until then.
8. **Clock nets on the pre-ABC legs** (O′, R): VTR's `blackbox_latches.pl` builds clock-domain
   ids as `latch|<type>|<clk>|<init>` and splits them on `_^_`, so a latch clock net's name must
   contain neither `|` nor `_^_`. On a net used as a latch or hard-block clock, `|` is written
   `%7C` and the `^` of `_^_` as `%5E` (`_%5E_`); checker rule C-S5 enforces it.
9. **Renaming info**: one line, `write_blif: --vtr: N names legalised (first: '<orig>' ->
   '<legal>')`, when N > 0. N is the report's `renamed`: the `.model` name (item 10), port bit
   names and net names that VB-10 changed; the `.model` name, when it changed, is the `first`.
10. **The top's `.model` name** gets rules 1 and 2, never rule 3 (a model name is no net, so no
   reserved word applies; `.model NIL` stays). VPR tokenises it with the same lexer and uses it
   for its output file names. The 1C reader reads the escaped name back unchanged (3E-1 final
   review M3).

## 11. Preflight and errors (VB-11)

`odin3_blif_vtr_preflight` walks the top once (nodes in ID order, then nets for R16), logs every
refusal up to `ODIN3_BLIF_VTR_MAX_ERRORS`, then `write_blif: --vtr: N more errors not shown`, and
the pass returns `ODIN3_ERR_INVALID_ARG` without opening the file. Style as 2D §9 (lower case,
names in `'…'`, no final period). `<loc>` is the `file:line` of the first location
`odin3_prov_sources` yields for the node (or the net), else the module name; `<cell>` is the
node's name or `$c<ID>`; `<type>` the registry or library type name.

| # | Exact text (after `write_blif: --vtr: `) | When |
|---|---|---|
| E1 | `<loc>: $_DFF_N_ '<cell>': falling-edge flip-flop; VPR accepts only rising-edge latches (lower inverts the clock)` | R6 |
| E1b | `<loc>: <type> '<cell>': asynchronous set/reset flip-flop; VPR has none (lower makes it synchronous, decision 19)` | `$_DFFSR_*`, async word FFs |
| E2 | `<loc>: <type> '<cell>': level-sensitive latch; VPR accepts only rising-edge latches` | `$_DLATCH_P_`/`_N_` |
| E3 | `<loc>: <type> '<cell>': flip-flop without a clock; VPR requires a clock on every latch` | `$_FF_`, or `$_DFF_P_` with C unconnected, undriven or x |
| E3b | `<loc>: <type> '<cell>': clock input '<port>' is unconnected or x` | a hard cell's `clock`-flagged input (§8.2) that is unconnected, undriven, x or `unconn`; a `$_DFF_P_` whose C is `unconn` (`<port>` is `C`) |
| E4 | `<loc>: <type> '<cell>': tri-state cell; VPR has no tri-state` | `$tribuf` and z-capable cells |
| E5 | `<loc>: <type> '<cell>': not a netlist cell (run lower)` | any other registry word cell |
| E6 | `<loc>: <type> '<cell>': '<type>' is not a hard cell of a loaded tech library (read_techlib lib/vtr.o3lib)` | gate-kind library cell, implicit or undeclared black box |
| E7 | `<loc>: <type> '<cell>': <N> inputs exceed K=<K> (run abc, or pass --lut-k)` | R4 |
| E8 | `<loc>: $sop '<cell>': cover mixes on-set and off-set rows` | rows with both output `0` and `1` |
| E9 | `<module>: port '<p>': inout port; VPR has no bidirectional pins` | `$port_inout` |
| E10 | `<loc>: instance '<cell>' of module '<m>': hierarchy not flattened (run flatten); VPR reads one model` | a module-typed node in the top |
| E11 | `<loc>: <type> '<cell>': parameters <list> cannot be recovered from BLIF formals` | 1C's existing check, kept |
| E12 | `<module>: net '<net>' has <N> drivers ('<a>', '<b>'…)` | R16 (the reader can produce multi-driver nets from Odin II goldens, PHASE1 #15) |
| E13 | `no top module (read_blif sets one; or run hierarchy)` | `odin3_design_top` is 0 |

Argument errors (no `--vtr: ` prefix, `ODIN3_ERR_INVALID_ARG`, the 1D style):

| Exact text | When |
|---|---|
| `write_blif: unknown argument '<arg>'` | an unknown option |
| `write_blif: expects one path` | none or two paths (existing `one_path`) |
| `write_blif: --lut-k expects an integer in 2..16, got '<v>'` | not a decimal in range |
| `write_blif: --name-style expects short or provenance, got '<v>'` | other value |
| `write_blif: --lut-k and --name-style need --vtr` | either without `--vtr` |
| `write_blif: option '<opt>' given twice` | repeated option |

Warnings and info (logged, `ODIN3_OK`): §7 item 3 (constant clock), §9.2 (`unconn` on logic),
§10 items 5 and 9 (renames), §5 (modules skipped), and the summary line `write_blif: --vtr:
'<top>': N .names, M .latch, S .subckt (adder A, multiply B, single_port_ram C, dual_port_ram D),
K=<K>; x bits: X to gnd, Y open (unconn)`; types with count 0 are omitted from the parentheses.

## 12. Caps (VB-12)

| Cap | Value | Effect |
|---|---|---|
| K | `ODIN3_BLIF_VTR_MIN_K` 2 … `ODIN3_BLIF_VTR_MAX_K` 16 | argument error outside; 16 bounds the ISOP truth table (1,024 words) and its cover (≤ 2^15 rows) |
| refusals logged | `ODIN3_BLIF_VTR_MAX_ERRORS` 20 | then one count line; the count is exact |
| line width | 100 columns, ` \` continuation (1C) | cover rows never wrap; a `.latch` line may wrap (libblifparse's `<*>\\{ENDL}` rule applies in the `LATCH` state too) |
| ISOP stack | depth ≤ K frames, explicit (no recursion) | CLAUDE.md |
| memory | preflight: two `u64map`s (net → class, net → legal name ID) and a `vec` of driver counts, freed on return; the writer's existing maps | O(nets + pins), about 40 B per net |
| design size | none beyond the IR's | mcml (51 MB golden, 209 k `.names`) is the largest input |

## 13. Determinism and provenance (VB-13)

Output is a pure function of the IR (node and net IDs, names, parameters, provenance records) and
the options: no hash-table iteration order, time, path of the working directory or host reaches
the file. Stubs are sorted by name; cells follow node ID order; generated names depend only on
IDs. Two runs of the same script give byte-identical files (PHASE3 exit #3); 3H checks it on the
19. `write_blif` creates no IR objects, so it adds no provenance records; the header carries the
`abc` runs' facts (decision 14), and `--name-style provenance` carries locations into VPR's block
and net names.

## 14. Interaction with the other sub-projects (VB-14)

- **3A `flatten`**: E10 enforces it. **3B `lower`**: E1/E2/E5 enforce the netlist view and
  decision 19's clock inversion. **3C `hardmap`**: produces library hard cells (§8), with unused
  RAM address bits as x on the IR side (decision 6 step 3), which §9.2 writes as `unconn`.
- **3D `abc`**: see §14.1. After `abc`, LUTs come back as 3D's `$lut` cell whichever exchange
  3D uses (decision 9: "either way"); §6 also accepts `$sop`, so a BLIF read back by 1C is
  writable too. §5 prints the facts 3D records for each `abc` run.
- **3F**: the wrapper's script ends with `write_blif --vtr`; it passes `--lut-k` from
  `determine_lut_size`.
- **3H**: legs O, O′, P′ and R (§16.5) consume `--vtr` files; 3H's `check-lib` uses §15.2.

### 14.1 Requirements on 3D (passed to the 3D author; review E3/E4)

3E does not choose 3D's exchange mechanism (decision 9: MiniAIG/MiniLUT in production, BLIF via a
temporary directory as `--via-blif`). Whatever 3D chooses, 3E's output is only correct if:

1. **The exchange matches PHASE3.** `--via-blif` is "exactly VTR's stage" (decision 9b: `read`,
   the script, `write_hie`); the current clock domain's latches go into ABC (decisions 12a and
   13a: `scorr` needs them), other domains' latches and every hard block are cut as boxes
   (hard-block outputs are ABC primary inputs, inputs are primary outputs). Any BLIF handed to
   ABC follows the `--vtr` value rules: latch init 0/1/2 (never 3, which ABC rejects), constants
   as `gnd`/`vcc`/`unconn`, no `.attr`/`.param`/`.cname`, `#` escaped (§10 item 1).
2. **Hard-block inputs on `unconn`, x or a constant pass through `abc` unchanged.** A cut-point
   net (a hard-block input that becomes an ABC primary output) whose only driver is a constant,
   x or `unconn` generator bypasses ABC and keeps its IR net. Otherwise ABC's constant 0 comes
   back on RAM address bits and upper `multiply` bits, §9.2 writes them as `gnd` (tied) instead
   of `unconn` (open), and P′ would differ structurally from P (`ch_intrinsics` has 10 `unconn`
   address bits per slice): a QoR confound in the leg meant to measure ABC alone. VTR avoids it
   because `write_hie` keeps the PO net named `unconn`. Test (3D, with 3E-5): on P′ for
   `ch_intrinsics`, every `addr[5..14]=unconn` survives in the `--vtr` file.
3. **Latches return as `$_DFF_P_`** with C re-stitched to the original clock net and INIT 0, 1
   or 2 (DC → 2), so §7 writes them unchanged.
4. **No `.names`/`.latch` pin is left on `unconn` after `abc`** (every in-domain cone goes through
   ABC, which folds `unconn` as constant 0), so class U (§9.2) never warns on the O and P′ legs.
5. **The recorded script path is as given or repo-relative**, with its SHA-256 (§5).
6. *Request from 3H, not a correctness requirement:* a fixed log line with ABC's final depth
   (e.g. `abc: final: lev = <N>`; spelling 3D's), so 3H can fill `abc_depth` for every leg (3H
   review H11).

## 15. Tools (VB-15, VB-16)

### 15.1 `tools/blif-vtr-check` — the VPR-free checker (decision 18)

`tools/blif-vtr-check/blif-vtr-check` (bash shim) → `blif_vtr_check.py` (stdlib only, `ruff` and
`mypy --strict` clean). It does not reuse `tools/blif/blif.py`'s permissive reader: it carries its
own lexer that mirrors libblifparse's token rules (R1–R3, R5, R8), so it rejects what VPR rejects.

```
blif-vtr-check [--models FILE] [--lut-k K] [--strict] [--eblif] [--max-errors N]
               [--writer-log LOG] FILE...
```

- `--models` default `tests/vtr/arch/k6_frac_N10_frac_chain_mem32K_40nm.models.json`; `--lut-k`
  default the JSON's `lut_size` (6).
- `--strict`: also Odin III's own conventions (C-S rows below).
- `--eblif`: accept `.conn .attr .param .cname` (VPR eblif mode); default off, as for `.blif`.
- `--writer-log LOG` (with `--strict` and one FILE; added 2026-10-12, §24): LOG is the stderr of
  the `write_blif --vtr` run that wrote FILE. C-S2's lines on `.names` inputs and `.latch` D pins
  are dropped when their pin count equals the N of the writer's `net 'unconn' feeds N
  .names/.latch pins` warning (0 when absent), and those on `.latch` controls when the `.latch`
  and hard-block clock pins on `unconn` number exactly the writer's `clock is 'unconn'` warnings
  (one per such pin, §7 item 3, plan point (j); since 3E-4 the writer refuses such a clock
  instead, so a W with one always differs from the count 0 and is flagged). A count that differs keeps every C-S2 line of
  its kind and adds `net 'unconn' feeds <M> .names inputs and .latch D pins, but the writer warned
  of <N>` (or `… <M> clock pins …`) on line 1. Every other rule stays as `--strict` has it. The
  pairing is by count, since the writer's §9.2 warning is one count per write.
- Exit: 0 every file legal, 1 any violation (located `file:line: message` lines on stdout, at
  most `--max-errors` per file, default 50), 2 usage or I/O.

| Check | Mirrors | Message (`blif-vtr-check: ` + `file:line: `) |
|---|---|---|
| C1 | R1 | `illegal character <0xHH> in name` / `unterminated quoted string`. The rule mirrors the lexer exactly (longest match): `#` ends the line only where a token would start, so `a#b` is accepted as VPR accepts it |
| C2 | R3 | `directive '<d>' is not read by VPR` / `'<d>' needs --circuit_format eblif` |
| C3 | R13 | `second model '<m>' with primitives; VPR reads one` / `black-box model '<m>' is not an arch model` / `stub port '<p>' of '<m>': <reason>` |
| C4 | R4 | `.names with <N> inputs exceeds K=<K>` |
| C5 | R5 | `cover row has <N> columns, expected <M>` / `cover row character '<c>'` |
| C5b | R5b | `1-input .names '<y>' has <N> cover rows; VPR asserts exactly one (absorb_buffer_luts)` |
| C6 | R6, R7, R8, R2 | `latch type '<t>' (VPR accepts only re)` / `latch without a clock` (also for a `NIL` control and for every untyped form, which carries none) / `latch init '<v>'` / `'<w>' on a .latch line lexes as a keyword` |
| C7 | R9, R10 | `.subckt model '<m>' is not an arch model` / `no port '<p>' on model '<m>'` / `port '<p>[<i>]' exceeds width <W> of '<m>'` / `formal '<p>' given twice` (any repeated formal, input or output: a second pin on one port bit. VPR itself accepts a repeat on the *same* net, since `Netlist::create_pin` finds the pin and asserts only that the net is equal, and fails one on a different net; the checker refuses both, which no writer produces; noted 2026-10-12, Task 6 review M4, §24) |
| C8 | R12 | `duplicate block name '<b>' (<kind> and <kind>)` |
| C9 | R16 | `net '<n>' has <N> drivers` (`.inputs` pads count as drivers, so a `.names` driving a primary input is caught; a name repeated on `.inputs`/`.outputs` is one pad, as VPR's `inputs()`/`outputs()` reuse its block, port and pin, so `.inputs a a` is legal: amended 2026-10-12, §24) |
| C-S1 | VB-7 | `latch init 3 (write 2; VTR's ABC rejects 3)` |
| C-S2 | VB-2a | `net 'unconn' feeds a .names/.latch pin (VPR leaves it open)` |
| C-S3 | VB-9 | `'<gnd\|vcc\|unconn>' is not a constant generator of the right value` |
| C-S4 | VB-5 | `missing header` / `.clock` present / `stub '<m>' unused` |
| C-S5 | VB-10 item 8 | `clock net '<n>' contains '\|' or '_^_' (breaks VTR's blackbox_latches.pl)` |

### 15.2 `tools/vtr-models` — the arch's `<models>` for CI (decision 18) and the library check (decision 5)

`tools/vtr-models/vtr-models` → `vtr_models.py` (stdlib; `xml.etree.ElementTree`).

```
vtr-models extract ARCH.xml [--out FILE]      # JSON below
vtr-models check FILE [--arch ARCH.xml]       # FILE still matches ARCH (sha256 and content)
vtr-models check-lib LIB.o3lib [--models FILE] [--vtr-root DIR]
```

JSON (`tests/vtr/arch/<arch>.models.json`, committed for k6_frac and EArch):

```json
{"schema": 1, "arch": "k6_frac_N10_frac_chain_mem32K_40nm.xml", "arch_sha256": "<hex>",
 "vtr_commit": "3c9a4d23b27d187bd67c79dc01a18ff1ffc21d9b", "lut_size": 6, "ram_addr_width": 15,
 "models": {"adder": {"inputs": {"a": 1, "b": 1, "cin": 1}, "outputs": {"cout": 1, "sumout": 1},
                      "clocks": []}, "...": {}}}
```

Widths follow R11 (the largest `num_pins` over the model's pb_types, memory-class data ports
1); `lut_size` and
`ram_addr_width` come from VTR's own `vtr.log_parse.determine_lut_size` /
`determine_memory_addr_width`, run as `$VTR_ROOT/.venv/bin/python` with
`PYTHONPATH=$VTR_ROOT/vtr_flow/scripts/python_libs` (decision 5: VTR's readers, not ours); `extract`
refuses if they disagree with its own count (`vtr-models: lut_size: VTR says <a>, the pb_types
give <b>`). `check-lib` parses the `cell`/`param`/`in`/`out`/`end` lines of a `.o3lib` (the
other statements are skipped) and requires, for each `hard` cell: a model of that name; the same
port names and directions; `clock` flags equal to the model's `is_clock`; the default width of
every port ≤ the model's size. Messages: `vtr-models: check-lib: <cell>: <reason>`; exit 0/1/2.
CI runs `check` without an arch (schema only) and `check-lib` against the committed JSON; the
freshness `check --arch` runs nightly and in 3H.

## 16. Test plan and oracles (VB-17)

### 16.1 Unit (Unity, `tests/unit/test_blif_vtr.c`, CI)

Hand-built IR per row of §6, §7, §8, §9.2, §10 and per E1–E13: exact output text (byte strings)
or exact log line and `ODIN3_ERR_INVALID_ARG` with no file created (`access(path) == -1`).
Includes: every gate cover; latch init mapping 0/1/2/3 → 0/1/2/2; class U vs x vs undriven on
each pin kind; reserved and illegal names (`#`, blank, `=`, `\`, `%`, UTF-8, `re`, `0`, `.end`,
`out:x`, `unconn` as a signal); stub widths as the maximum over instances; declared stub order
kept; unused declared stub dropped; option parsing and every argument error; E-cap at 21 refusals
(20 lines + `1 more errors not shown`); `odin3_blif_isop` against an exhaustive enumeration for
k ≤ 4 (cover evaluates to the truth table; no row removable — irredundance; on/off choice) and
random tables for k = 5…16 (evaluation only), 3E-5. Also: the §6 normalisation cases (1-input
covers with 0, 2 and 3 rows; constant covers with inputs; constant `$lut`; a constant `$sop` on an
output-port net, which must give one driver), formals sorted as Parmys sorts them, a RAM `clk` that
is x (E3b) or constant (warning), `$_DFFSR_*` (E1b), and a clock net named `a|b` and `x_^_y`
(§10 item 8).

### 16.2 Fixtures (CI, `tests/golden/blif-vtr/`)

`<name>.in.blif` (or `<name>.o3` script on hand IR) → `<name>.vtr.blif` expected bytes, run by a
CTest driver `blif_vtr_fixtures` with `odin3 -p "read_techlib lib/vtr.o3lib; read_blif
<in>; write_blif --vtr <out>"`, then `blif-vtr-check --strict` on every expected file (the checker
and the writer agree). Fixtures: the committed `tests/golden/blif/*.parmys.blif` sample (adder
chain, `ff`, `dffsre`, `pow`), a two-clock design, a RAM slice with `unconn` addresses, a
`multiply` with narrow inputs, an Odin II file with init 3 and `\` continuations, a constant
output, an x output, a Parmys k6 golden with a mid-name `\` (`shiftx.parmys`, copied in as
`shiftx.parmys.in.blif` since `tests/golden/blif/` is not 3E's; §16.3 step 1's renamed case), and
one negative fixture per E-class with its expected log (`<name>.err`). E4 and E9 have no BLIF
form, and E11 and E13 need hand IR or an `.o3` script the driver does not run, so those four are
pinned by §16.1 only (3E-1 final review M7). The driver then runs §16.3 step 1 on every written
case. `tests/tools/test_blif_vtr_check.py` covers C1–C9 and C-S1–4 on inline texts.

### 16.3 Round trip with our BLIF reader (local; CI on fixtures)

`tools/blif-roundtrip --vtr` (a new mode of the 1C script) on a golden tree, smallest first:

1. `W1 = write_blif --vtr (read_blif G)`; `W2 = write_blif --vtr (read_blif W1)`.
   **Erratum (3E-1 final review I1; Peter's ruling, 2026-10-12).** Rule 1 of §10 escapes `%`,
   so a name the write of W1 legalised comes back one level deeper in W2 (`$2\buffer` →
   `$2%5Cbuffer` → `$2%255Cbuffer`; `NIL` → `%NIL` → `%25NIL`), and the reader does not decode.
   The oracle is therefore split on the report's `renamed` count of the W1 write (the `names
   legalised` info line, §10 item 9):
   - `renamed == 0`: **W1 == W2 byte for byte** (idempotence; the header line is identical
     because nothing in it depends on the run);
   - `renamed > 0` (the 25 Parmys k6 goldens with a mid-name `\`, §3 item 9, and any Odin III
     design with a reserved or illegal name): **W1 and W2 identical under `--vtr-normalize`**,
     whose VB-10 decode runs to a fixed point (one decoding step inverts one write exactly,
     point (o); repeating it until nothing changes decodes `%255C` and `%5C` alike, to `\`);
     the byte comparison is not applied. Such files are listed `decoded`.

   The escape set stays as it is (§10 item 1). CI runs this step on every written fixture
   (`blif_vtr_fixtures`; the decode is `vtr_normalized` in `tools/blif-vtr-check/vtr_fixtures.py`
   until 3E-4's `--vtr-normalize` lands, and must agree with it), and `test_vtr_fixtures` pins
   it on `shiftx.parmys` (renamed 8: byte comparison not applied, decoded comparison passes) and
   `two_clock` (renamed 0: bytes).
2. **Parmys k6 goldens**: `netlist-compare G W1` identical (exit 0) for every written file. The
   survey (§3) predicts all 463 non-refused files; the 4 `fe` files must be refused with E1. A file
   that holds a constant cover with inputs or a 1-input cover without exactly one row (§6
   normalisation; none found in the goldens under 8 MB, review E1) is compared with
   `--vtr-normalize` instead and listed.
3. **Odin II k6 goldens**: `netlist-compare --vtr-normalize G W1` identical. `--vtr-normalize`
   (new flag, 3E-4) applies the writer's documented rewrites to both sides before
   canonicalising: latch init 3 → 2; an omitted `.subckt` input formal bit ≡ `=unconn`; `.clock`
   lines dropped; VB-10 names decoded to a fixed point (step 1); §6's constant and 1-input
   normalisation. The 6 `fe` files → E1, the 92 files with `.names` wider than
   6 → E7, the 2 multi-driver files → E12; each refused file's original is also rejected by VPR
   (§16.4).
4. `equiv-check G W1` on every Odin II golden written in step 3 that has no RAM (PHASE0 #8) and
   no `multiply` (equiv-check inlines a 36×36 multiplier as logic, which dsec cannot finish), as a
   second, semantic oracle for the normalisation itself; 600 s per file, a `timeout` verdict is
   listed, not a failure; designs with more than one clock get `multiclock` (dsec has one
   implicit clock) and are skipped.
5. CI: steps 1–2 on `tests/golden/blif/*.parmys.blif` and the 16.2 fixtures.

### 16.4 VPR reads every golden-equivalent BLIF (local, heavy-run rule)

`tools/vtr-read-check` (bash + a small Python report): for each `status=ok` k6 golden G
(923 files), smallest first:

```
odin3 -p "read_techlib lib/vtr.o3lib; read_blif G; write_blif --vtr W"      # W or a refusal
$VTR_ROOT/vpr/vpr <arch> W --circuit_format blif --exit_before_pack on       # in a temp dir
$VTR_ROOT/vpr/vpr <arch> G --circuit_format blif --exit_before_pack on       # the original
blif-vtr-check --strict --writer-log <the write's stderr> W ; blif-vtr-check G
```

`--exit_before_pack on` makes VPR read, clean (R14, R15) and verify the netlist and print its
statistics, then stop: VPR's whole reader at a fraction of a route's cost. **Oracles**:

- every written W: VPR exits 0 after printing its netlist statistics (the atom-netlist block
  counts; no pack output), and `blif-vtr-check --strict --writer-log <the write's stderr> W`
  exits 0: a class-U `unconn` on a `.names`/`.latch` pin (§9.2; ≥ 39 goldens, 35 Parmys and 4
  Odin II under 3 MB) is allowed only when its C-S2 lines pair with the writer's class-U warnings
  (§15.1 `--writer-log`), and every other `--strict` rule holds (amended 2026-10-12, §24, from
  the 3E-2 final review I1);
- every refused G: the refusal's class and VPR's verdict on G pair as below; any other pairing
  is a failure:

  | Refusal | VPR on G |
  |---|---|
  | E1 (`fe`) | `Only rising edge latches supported` |
  | E1b (async FF) | cannot occur in a BLIF golden (no such `.latch`); a hit is a failure |
  | E2 (`ah`/`al`) | `Only rising edge latches supported` |
  | E3 (no clock) | `Latch must have a clock` |
  | E3b (hard-block clock open) | accepted by VPR (open pin); listed as an intended refusal |
  | E4 (tri-state) | cannot occur (no BLIF tri-state); failure |
  | E5, E6 | `Subckt instantiates model …` / `Blackbox BLIF model … has no equivalent architecture model.` |
  | E7 (`.names` > K) | `BLIF .names input size … greater than .names model input size …` |
  | E8 (mixed cover) | accepted by VPR (undefined function); listed as an intended refusal |
  | E9 (inout) | cannot occur in BLIF; failure |
  | E10 (hierarchy) | `Found multiple models with primitives …` |
  | E11 (parameters) | depends; listed per file |
  | E12 (multi-driver) | the R16 assertion |
  | E13 (no top) | `No non-blackbox models found …` |

- **checker agreement**: `blif-vtr-check G`'s verdict equals VPR's on G for all 923, and on every
  W; and on `tests/vtr/blif-bad/*.blif` (one file per C-check, committed with the VPR message it
  draws, verified by this script) VPR rejects exactly the files the checker rejects.

Scope: `--exit_before_pack` checks reading only. Whether a hard block can be *packed* (R10/R11
check each port's size alone: a RAM `.subckt` is a 1-bit data slice, §24, and the slices of one
memory must share their address and control nets to fill a memory mode) is 3C's to guarantee and
3H's runs to show, not the checker's. (Corrected 2026-10-12, §24: the earlier text had a slice
with 64 data bits passing R10/R11, which VPR refuses.)

The sweep is a VTR-set sweep under the standing rules. It uses 3H's shared heavy convention
(`tools/vtr-qor/qor_heavy.py`, imported, 3H §7.1): it re-executes itself as `flock
/tmp/claude-1000/odin3-heavy.lock systemd-run --user --scope -p MemoryMax=6G -p MemorySwapMax=0 …`
unless `ODIN3_HEAVY=1` is already set (then it only verifies the scope), so `vtr-qor nightly` can
call it without deadlock. One file at a time, run directories under
`~/odin3-ws/work/vtr-read-check/` deleted after each file passes. Report:
`~/odin3-ws/work/vtr-read-check/report.tsv` (`golden, tool, written|refused, error class, vpr_W,
vpr_G, checker_W, checker_G, verdict`) and a one-paragraph PHASE3 checklist entry. **Time
estimate** (unmeasured): about 900 small files at ≈ 1–2 s each (odin3 read/write plus two VPR
reads) ≈ 25 min, plus the 31 `vtr/` files on each tool, dominated by the four largest (≈ 1–2 min
each for two VPR reads of a 17–60 MB BLIF) ≈ 30 min: **≈ 1 h** for §16.4, plus ≈ 1 h for §16.3
(netlist-compare's canonicalisation of the large files dominates). The first sweep records the
real figure.

### 16.5 Route through `-start abc` (local, 3H's R leg)

PHASE3's checklist item: "Parmys goldens round-trip through it and route via `-start abc`". The R
leg of 3H runs, for the 19 (+2 extras): `W1` of §16.3 through `run_vtr_flow.py W1 <arch> -start
abc -track_memory_usage -crit_path_router_iterations 100`. **Oracle**: `vpr_status=success` for
all 19; QoR reported against the P leg (same netlist up to names and line order, so differences
are VPR's sensitivity to names and order; report-only, recorded in the 3H table).

### 16.6 What runs where

| Check | CI (`ci`) | Local, on demand | Nightly (WSL, 3H) |
|---|---|---|---|
| 16.1 unit, 16.2 fixtures, checker unit tests | yes | yes | — |
| 16.3 round trip on fixtures | yes | — | — |
| 16.3 round trip on all k6 goldens | — | yes (3E-4) | yes, when odin3 changed |
| `vtr-models check` (schema) / `check-lib` | yes | yes | yes, plus `check --arch` |
| 16.4 VPR read sweep | — | yes, heavy (3E-4) | yes, heavy |
| 16.5 R leg | — | 3H | yes, heavy |

## 17. Files

| File | Content |
|---|---|
| `src/backends/blif/writer.h` | `odin3_blif_dialect`, `odin3_blif_name_style`, `odin3_blif_write_opts`, the constants, `odin3_blif_write_with` |
| `src/backends/blif/writer.c` | dialect switch: model selection, `.clock` omission, gate covers, latch init map, formal rules, stubs, constant generators (both dialects), legal names hook |
| `src/backends/blif/vtr.[ch]` | `odin3_blif_vtr_report`, `odin3_blif_vtr_preflight`, source classes (§9.1), `odin3_blif_vtr_legal_name`, E-texts |
| `src/backends/blif/isop.[ch]` | `odin3_blif_isop` (3E-5) |
| `src/passes/builtin.c` | `write_blif` options (`--vtr`, `--lut-k`, `--name-style`), help text |
| `tests/unit/test_blif_vtr.c`, `tests/unit/test_blif_isop.c` | §16.1 |
| `tests/golden/blif-vtr/` | §16.2 fixtures, a README listing each |
| `tests/vtr/arch/{k6_frac_N10_frac_chain_mem32K_40nm,EArch}.models.json` | §15.2 |
| `tests/vtr/blif-bad/` | one file per checker rule, with the VPR message in a `#` comment |
| `tools/blif-vtr-check/`, `tools/vtr-models/`, `tools/vtr-read-check/` | §15, §16.4 |
| `tools/blif-roundtrip/blif-roundtrip` | `--vtr` mode (§16.3) |
| `tools/netlist-compare/netlist_compare.py` | `--vtr-normalize` |
| `tests/tools/test_blif_vtr_check.py`, `test_vtr_models.py` | unittest, CI |

No public ABI change (`include/odin3/odin3.h` untouched): the writer is reached through the pass.

## 18. Implementation split (VB-18)

PHASE3's 3E row: light review, Opus `high`. Each PR: tests first, `/lint`, gates green
(`cmake --build --preset debug -j6 && ctest --preset debug -j4`, `tools/lint.sh`), a read-only
critique (PHASE0 §A.1).

| PR | Content | Depends on | Model/effort |
|---|---|---|---|
| 3E-1 writer | opts struct and `odin3_blif_write_with`; pass options; preflight E1–E13 (with E1b, E3b) and the exact texts; §6 cells (gate covers, `$sop` checks, constant and 1-input normalisation); §7 latches; §8 formals (sorted), clock pins and stubs; §9 classes and generators in the VTR dialect; §10 rules 1–6, 8, 9; header; §16.1 unit tests; §16.2 fixtures; PASSES.md row | 1C, 1G (merged) | Opus `high` |
| 3E-2 checker | `blif-vtr-check` C1–C9 with C5b, C-S1–5; `vtr-models extract/check/check-lib`; the two committed JSON files; `tests/vtr/blif-bad/` (one file per rule, C5b included); unittest; CI wiring (the checker over 16.2's expected files) | none (parallel with 3E-1) | Opus `high`; Sonnet `high` for the negative-fixture corpus |
| 3E-3 plain constants | §9.4 alone: the plain dialect's `$_CONST*_` → `gnd`/`vcc`/`unconn` (PHASE3 #20; 2F confirms it is not 2F's change), the 1C/1F fixture updates; 2H depends on it | 3E-1 (shares the generator code) | Opus `high` |
| 3E-4 evidence | `blif-roundtrip --vtr`, `netlist-compare --vtr-normalize`, `vtr-read-check` (on 3H's `qor_heavy`); the full k6 sweeps of §16.3–16.4 run by the controller under the heavy lock, smallest first; report and PHASE3 checklist entry | 3E-1, 3E-2, 3H-1 (`qor_heavy`) | Opus `high` for the scripts; Sonnet `high` to run the sweeps (in the background, watched with Monitor) and write the report |
| 3E-5 `$lut` | `odin3_blif_isop` and the `$lut` row of §6, to §4.2's requirement only; reads `$lut`'s mask directly as the truth table and caches each node's `odin3_blif_vtr_fn` in the plan (3E-1 rebuilds a table by scanning 2^k minterms per row, up to four times per cell: harmless at K = 6, 2^31 steps at K = 16; final review M9), and resolves cell kinds by type ID once instead of `strcmp` per node (M11); lands **right after 3D's `$lut` registry PR and before 3D's `abc` pass PR**, so P′/M1 never waits on it | 3D's `$lut` registry PR | Opus `high` |
| 3E-6 provenance names | `--name-style provenance` (§10 item 7) | 1F's provenance naming, 3D's first-location rule; needed only for Oprov at M3 | Opus `high` |

Lanes: 3E-1 ‖ 3E-2 → 3E-3 and 3E-4; 3E-5 directly after 3D's `$lut` registry PR; 3E-6 any time
before M3. 3D can start once 3E-1 lands (its P′ leg writes `--vtr`). The agent says when to
switch: Opus `high` for 3E-1/2/3/5/6 sessions, Sonnet `high` for the 3E-4 sweep sessions (PHASE0
§9).

## 19. Review focus and risks

1. **`unconn` semantics** (VB-2a, §9): the split between "open" (hard-block inputs) and "0"
   (logic) is the one place 3E departs from Parmys's text. It follows what VPR and ABC each do with
   Parmys's own output; §16.5 and 3H's functional check are its evidence.
2. **Scalar vs indexed formals** (§8.2): VPR accepts both; IR-7b's reader check decides which
   stub matches; an error here shows as a 1C read failure in §16.3 step 1.
3. **R12 block names**: generated names for unconnected outputs keep block names unique; the
   checker's C8 guards it.
4. **Stub widths** (§8.3) bigger than the arch's (R10) are only caught by the checker, by design
   (no C arch reader).
5. **The plain-mode change** (§9.4) touches 1F/1C fixtures built from Odin III IR; 3E-3 updates
   them in the same PR.

## 20. Amendments to other documents (when the PRs land)

- `docs/PASSES.md`: the `write_blif` row gains the options and the dialect (3E-1).
- `docs/PHASE3.md` checklist: 3E ticked with the §16.3/16.4 report figures (3E-4); decision 20's
  sub-bullet (x → `unconn`) marked done by 3E-3.
- `docs/DESIGN.md` §9 BLIF row: "VTR dialect (`write_blif --vtr`): top only, `.latch … re`
  init 0/1/2, `unconn` on open hard-block inputs".
- `docs/ORACLES.md`: note that `vpr --exit_before_pack on` is the VPR-read oracle.

## 21. Decisions for Peter (agent defaults; none blocks 3E-1)

1. **x on logic pins is `gnd`, `unconn` only on hard-block inputs** (§9.2). *Recommended*: as
   specified; VPR leaves `unconn` pins open (R14), so `unconn` on a LUT input after ABC would be
   undefined. Alternative: Parmys's text everywhere, relying on ABC always running first (false for
   the O and P′ legs, whose ABC is ours and runs before the writer).
2. **Hard-block stubs sorted by name; only used types** (§8.3). *Recommended*: VPR verifies every
   stub against the arch, so an unused stub can only hurt.
3. **Port bits by declared Verilog index** (§10 item 5; ruled). The reason is parity with
   Yosys/Parmys naming (`start_offset`), not Verilator, whose wrapper packs ports as `[max:0]`; a
   top port with a non-zero lsb would fail the functional check on P and O alike, and none of the
   19 has one. The plain writer keeps 1C's 0-based index (no golden is affected either way).
4. **`--lut-k` default 6** rather than reading K from a design record. *Recommended*: both arches
   have K = 6, and 3F passes VTR's `determine_lut_size` explicitly.

## 22. Review dispositions (v2, `3EH-review1`)

| # | Finding | Disposition |
|---|---|---|
| E1 | 1-input `.names` must have one row; constants with inputs | applied (ruling): every constant written as a zero-input generator, 1-input covers reduced to one row (§6), R5b and R15 in §2 |
| E2 | checker vs VPR gaps | applied: C5b (ruling), every repeated formal (C7), `.inputs` as drivers (C9), `NIL` and untyped latches as no clock (C6), C1 mirrors the lexer (`a#b` legal) |
| E3 | 3D exchange text | applied (ruling): §14 and §14.1 item 1 follow decisions 9, 12 and 13; mechanism left to 3D |
| E4 | `unconn`/x hard-block inputs through `abc` | applied (ruling): §14.1 item 2, a requirement on 3D with a `ch_intrinsics` test |
| E5 | `$lut` contract and PR placement | applied (ruling): 3E-5 lands right after 3D's `$lut` registry PR; provenance names split to 3E-6; §4.2 states only what 3E needs |
| E6 | absolute script path in the header | applied: as given or repo-relative, format cited from 3D (§5) |
| E7 | declared-index rationale | applied (ruling): §10 item 5 and §21 item 3 |
| E8 | R1, R6, R12 accuracy | applied (§2) |
| E9 | formal order | applied: sorted as Parmys sorts them (§8.2) |
| E10 | hard-block clock pins | applied: E3b and the §7 warning (§8.2, §9.2) |
| E11 | `$_DFFSR_*` text | applied: E1b |
| E12 | sweep pairings, equiv cost, estimate | applied: full pairing table, multiplier/multiclock exclusion with a 600 s timeout verdict, ≈ 1 h + 1 h estimate (§16.3, §16.4) |
| E13 | provenance names | applied: ports keep short names, 3D/1F's first-location rule cited, paths as given (§10 item 7) |
| E14 | constant `$sop` on an output-port net | applied: generator under the port name, no buffer (§9.2) |
| E15 | packing feasibility | applied: noted as 3C/3H's (§16.4) |
| E16 | `\|` and `_^_` in clock names | applied: §10 item 8 and C-S5 |
| E17 | 3E-1 size; plain-mode constants | applied: 3E-3 is its own PR; 2F checked (not 2F's change) |

No minor finding was rejected.

## 23. Errata after the 3E-1 final review (v2.2, 2026-10-12)

| # | Finding | Disposition |
|---|---|---|
| I1 | `--vtr` not byte-idempotent for a renamed name (§10 rule 1 against §16.3 step 1) | applied (Peter's ruling): §16.3 step 1 split on `renamed`; `--vtr-normalize` decodes to a fixed point; escape set kept (§10 item 1); §1 and §3 item 9 amended; `shiftx.parmys` fixture and `test_vtr_fixtures` |
| M3 | the top's `.model` name written raw | applied: rules 1 and 2 (§10 item 10, §5 item 2), counted in `renamed` |
| M5 | `--vtr` drops `.cname`/`.attr`/`.param` unstated | applied: §5; the Task 3–5 agent defaults logged in PHASE3 #21 |
| M6 | §8.3 and §4.1 stale | applied |
| M7 | E4, E9, E11, E13 without a fixture | applied: stated in §16.2 and the fixture README |
| M8 | a class-U clock leaves a latch with no clock net in VPR | applied: §7 item 3, a full VPR run in 3E-4 Task 11 |
| M9, M11 | truth tables rebuilt per use; kinds by `strcmp` | applied as 3E-5 requirements (§18) |
| M10 | E7 before the constant normalisation | noted in §6 |
| M12 | port warning overstated VPR | applied: §10 item 5's new text |
| M2, M13 | `PASSES.md` row; `wr_refuse`'s comment | applied (no spec text) |
| M4 | `modules_skipped` = `module_end − 2` counts dead modules | kept: the IR cannot remove a module (every ID below `module_end` is live), so the count is exact; the code comment now says so |

## 24. Errata after the 3E Task 7 review (v2.3) and the 3E-2 final review (v2.4), 2026-10-12

| # | Finding | Disposition |
|---|---|---|
| I1 | §2's RAM data widths (`data` 64/`out` 64; `data1`/`data2`/`out1`/`out2` 32) are not VPR's model sizes. libarchfpga's `ProcessMemoryClass` (`arch_util.cpp`, run on every `class="memory"` pb_type before `SyncModelsPbTypes`) moves the `blif_model` to a `memory_slice` child and forces each `port_class="data*"` port to 1 pin, so VPR refuses `data[1]` with `Port 'data[1]' on architecture model 'single_port_ram' exceeds port width (1 bits)` | applied (controller's ruling): R11 states the memory-class rule; §2's table gives the data ports 1; §15.2 and §16.4 corrected; `vtr-models extract` mirrors the rule and the committed `tests/vtr/arch/*.models.json` were regenerated; corpus file `tests/vtr/blif-bad/c7_ram_data_bit.blif`. `lib/vtr.o3lib` (`DATA_WIDTH` default 1) still passes `check-lib`. |
| M3 | `extract` did not refuse a pb_type port whose direction or `is_clock` differs from its model, as `SyncModelsPbTypes` does | applied: `extract` refuses both (exit 1) |
| I1 (3E-2 final) | §16.4's `--strict W` oracle fails by design on every golden with class-U `unconn` on a `.names`/`.latch` pin (§9.2 keeps it; C-S2 flags it): ≥ 39 goldens | applied (controller's ruling): checker option `--writer-log` (§15.1) pairs C-S2 with the writer's class-U warnings by count; §16.4 and plan Task 11 use it; every other `--strict` rule unchanged |
| M1 (3E-2 final) | §15.1 C7 stricter than VPR for a formal repeated on the same net (Task 6 review M4) | applied: note in C7 (the checker keeps refusing both) |
| M3 (3E-2 final) | `.inputs a a` gave C8 and C9; VPR accepts it | applied: a repeated `.inputs`/`.outputs` name is one pad (C9 note) |

## Note 2026-10-19 (5.1)

The 1C amendment of 2026-10-19 lets the PLAIN dialect write `.param` lines for a tech-library
`.subckt`. The VTR dialect is unchanged: its preflight (E11) still refuses a hard cell whose
parameters the reader would not derive, and `write_blif --vtr` writes no `.param`.
