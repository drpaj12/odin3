# 3E — `write_blif --vtr`: design

Status: v1 (2026-10-11), agent draft for light review (PHASE3 3E row: light; Opus `high`). Phase
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
`netlist-compare`-identical to its source; `--vtr` is idempotent through `read_blif`; the checker
agrees with VPR on the whole corpus; the 19 route through `-start abc` (the R leg of 3H); the
gate passes.

## 2. What VPR accepts (VB-2)

Read at `3c9a4d23`. VPR parses BLIF with libblifparse, then builds the atom netlist in
`read_blif.cpp`, then cleans it in `read_circuit.cpp`. Each rule below is a refusal in the writer
(§11) or a convention (§5–§10), and a check in `blif-vtr-check` (§15).

| # | Rule | Source (VTR) | VPR's text |
|---|---|---|---|
| R1 | A name is `([^ \t\r\n\\="]\|\\)*[^ \t\r\n\\="]`, or a `"…"` string; `#` starts a comment anywhere (`<*>#.*{ENDL}`), so a name cannot hold `#` | `blif_lexer.l` `ID_SET` | `Unrecognized character` / parse error |
| R2 | Inside a `.latch` line the words `re fe ah al as NIL 0 1 2 3` are tokens, not names (lexer state `LATCH`); a net with such a name cannot appear on a `.latch` line | `blif_lexer.l` `<LATCH>` | parse error |
| R3 | Directives: `.model .inputs .outputs .names .latch .subckt .blackbox .end`; `.conn .attr .param .cname` only in eblif mode; anything else (`.clock`, `.gate`, `.exdc`, …) is a syntax error | `blif_lexer.l`, `read_blif.cpp` `conn`/`attr`/`param`/`cname` | `Supported only in extended BLIF format` |
| R4 | `.names`: at most K inputs, K = the arch's `.names` pb_type width (6 here) | `read_blif.cpp` `names()` | `BLIF .names input size (%zu) greater than .names model input size (%d)` |
| R5 | A cover row has one plane character per input and one output character | `blif_parser.y` | `Mismatched .names single-output cover row. names connected to %zu net(s), but cover row has %zu element(s)` |
| R6 | `.latch`: type `re`, or no type (a warning; treated as `re`) | `read_blif.cpp` `latch()` | `Only rising edge latches supported` |
| R7 | `.latch`: a control (clock) net is required | `read_blif.cpp` `latch()` | `Latch must have a clock` |
| R8 | `.latch` init ∈ {0, 1, 2, 3} (2 = don't care, 3 = unknown) | `blif_parser.y` `latch_init` | parse error |
| R9 | `.subckt` model must be an arch `<model>` | `read_blif.cpp` `subckt()` | `Subckt instantiates model '%s', but no such model exists in the architecture file.` |
| R10 | `.subckt` formal `p` or `p[i]` (index split from the end, `p` = bit 0) must be a port of that model with `i` < the port's size | `read_blif.cpp` `find_model_port`, `split_index` | `Found no matching port '%s' on architecture model '%s'` / `Port '%s' on architecture model '%s' exceeds port width (%d bits)` |
| R11 | A model port's size is the largest `num_pins` among the pb_types whose `blif_model` is `.subckt <model>` | `arch_util.cpp` (model_port->size = max) | — |
| R12 | Block names: `.names` and `.latch` blocks take their output net's name, a `.subckt` its first connected output's, outpads `out:<name>`; two blocks with one name are an error | `read_blif.cpp` `subckt()`, `outputs()` | `Duplicate blocks named '%s' found in netlist. Existing block of type '%s' conflicts with subckt of type '%s'.` |
| R13 | Exactly one non-black-box model (the first one found is the top); every `.blackbox` model must be an arch model whose declared ports exist | `read_blif.cpp` `determine_main_netlist_index`, `blackbox()`, `verify_blackbox_model` | `Found multiple models with primitives. Only one model can contain primitives, the others must be blackboxes.` / `Blackbox BLIF model '%s' has no equivalent architecture model.` |
| R14 | The net named `unconn` and the block named `unconn` are deleted after reading; their pins stay with no net (open) | `read_circuit.cpp` `process_circuit` | warning at verbosity > 1 |
| R15 | A zero-input `.names` with no rows (or row `0`) is a constant-0 generator, with row `1` constant 1; VPR merges and sweeps constant generators | `read_blif.cpp` `names()`, `read_circuit.cpp` | info |
| R16 | One driver per net: a second driver aborts while the netlist is built | `netlist.tpp` `associate_pin_with_net` | assertion `Must be no existing net driver` |

The arch's `<models>` (k6_frac and EArch alike) and the port sizes R11 derives (computed
2026-10-11 from the k6_frac XML; EArch gives the same set):

| Model | Inputs (size) | Outputs (size) | Clock |
|---|---|---|---|
| `adder` | `a` 1, `b` 1, `cin` 1 | `cout` 1, `sumout` 1 | — |
| `multiply` | `a` 36, `b` 36 | `out` 72 | — |
| `single_port_ram` | `we` 1, `addr` 15, `data` 64, `clk` 1 | `out` 64 | `clk` |
| `dual_port_ram` | `we1` 1, `we2` 1, `addr1` 15, `addr2` 15, `data1` 32, `data2` 32, `clk` 1 | `out1` 32, `out2` 32 | `clk` |

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

#define ODIN3_BLIF_VTR_DEFAULT_K 6u   /* k6_frac_N10_frac_chain_mem32K_40nm and EArch */
#define ODIN3_BLIF_VTR_MIN_K 2u
#define ODIN3_BLIF_VTR_MAX_K 16u      /* VB-12 */
#define ODIN3_BLIF_VTR_MAX_ERRORS 20u /* refusals logged before "N more" */

/* Writes design to path in opts' dialect; opts NULL = PLAIN with defaults. odin3_blif_write is
 * odin3_blif_write_with(design, path, NULL). VTR: runs odin3_blif_vtr_preflight first and opens
 * path only when it passes, so a refused design leaves no file. Errors: as odin3_blif_write,
 * plus ODIN3_ERR_INVALID_ARG for a preflight refusal or an option out of range. */
odin3_status odin3_blif_write_with(const odin3_design *design, const char *path,
                                   const odin3_blif_write_opts *opts);
```

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
 * report. Returns ODIN3_OK when report->errors == 0, ODIN3_ERR_INVALID_ARG when it is not or an
 * argument is NULL, ODIN3_ERR_NO_MEMORY. Reads only; O(nodes + pins). */
odin3_status odin3_blif_vtr_preflight(const odin3_design *design,
                                      const odin3_blif_write_opts *opts,
                                      odin3_blif_vtr_report *report);

/* VB-10: appends the VPR-legal form of name to out (the identity for a legal name). */
odin3_status odin3_blif_vtr_legal_name(odin3_bytes name, odin3_strbuf *out);
```

In `src/backends/blif/isop.h` (3E-4):

```c
/* An irredundant sum-of-products of the k-input function whose truth table is tt (bit i = f(i),
 * input 0 the least significant index bit), by the Minato-Morreale ISOP on an explicit stack
 * (no recursion), for on-set and off-set; the shorter is chosen (on-set on a tie). Rows are
 * appended to cover as BLIF plane text (k characters of 0/1/-, then the output character), in
 * the deterministic order the algorithm emits them. k <= ODIN3_BLIF_VTR_MAX_K. */
odin3_status odin3_blif_isop(const uint64_t *tt, uint32_t k, odin3_strbuf *cover,
                             uint32_t *rows);
```

### 4.2 Pass and CLI

```
write_blif [--vtr] [--lut-k <K>] [--name-style short|provenance] <file>
```

Options and the path in any order (like `write_verilog`). `--lut-k` and `--name-style` need
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
   - one `# abc: <args>` per `abc` pass run in pass-run order, `<args>` being the pass-run
     argument string decision 14 records (`script=<path> sha256=<hex> abc=<commit> K=<K>`); none
     before 3D lands.
   - a blank line.
2. `.model <top>`.
3. `.inputs` and `.outputs` exactly as the plain writer (port order; one line per run of one
   direction; ` \` wrapping at 100 columns), with VB-10 names. **No `.clock`** (R3).
4. **Constant generators** (§9): `.names gnd`, then `.names vcc` + `1`, then `.names unconn`, each
   emitted only when referenced and not already present as a written `$sop` (§9.3).
5. **Cells** in node ID order, port nodes skipped (§6).
6. **Port buffers** as the plain writer, plus §9.2's constant-to-output buffers.
7. `.end`.
8. **Stubs** (§8.3): for each hard cell type used by a live node of the top, sorted by name in
   byte order, a blank line, `.model <m>`, `.inputs …`, `.outputs …`, `.blackbox`, `.end`.

Modules other than the top are not written (R13); when there are any, the info line
`write_blif: --vtr: N modules besides the top '<top>' not written` is logged. An instance of one
of them in the top is a refusal (§11, E10).

## 6. Cells (VB-6)

| IR cell (registry name) | `--vtr` output | Notes |
|---|---|---|
| `$port_in`, `$port_out` | `.inputs` / `.outputs` | IR-3 |
| `$port_inout` | refused: E9 | VPR has no bidirectional top pins |
| `$sop` (N inputs, cover) | `.names` with the cover as stored | N ≤ K (E7); every row the same output character (E8); a zero-input `$sop` is a constant generator kept as written |
| `$_BUF_` A Y | `.names A Y` / `1 1` | |
| `$_NOT_` A Y | `.names A Y` / `0 1` | Yosys `write_blif` covers (backends/blif/blif.cc), verified 2026-10-11 |
| `$_AND_` A B Y | `11 1` | |
| `$_OR_` | `1- 1`, `-1 1` | |
| `$_XOR_` | `10 1`, `01 1` | |
| `$_NAND_` | `0- 1`, `-0 1` | |
| `$_NOR_` | `00 1` | |
| `$_XNOR_` | `11 1`, `00 1` | |
| `$_MUX_` A B S Y | `.names A B S Y` / `1-0 1`, `-11 1` | needs K ≥ 3 (E7 otherwise) |
| `$lut` (3D, decision 9: `WIDTH`, `LUT` mask, Yosys semantics) | `.names` with `odin3_blif_isop`'s cover | 3E-4; WIDTH ≤ K; a constant LUT becomes a zero-input-like constant `.names` over its inputs (`.names a b y` with no rows for 0; `-- 1` for 1) |
| `$_CONST0_`, `$_CONST1_`, `$_CONSTX_`, `$_CONSTZ_` | no line of their own: their nets become `gnd`/`vcc`/`unconn` per §9 | |
| `$_DFF_P_` C D Q, `INIT` | `.latch D Q re C <init>` | §7 |
| `$_DFF_N_` | refused: E1 | `lower` inverts the clock (decision 19) |
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

## 7. Latches (VB-7)

1. **Form**: `.latch <D> <Q> re <C> <init>`; all five fields always (VPR's warning for an untyped
   latch is avoided, R6).
2. **Init**: the node's `INIT` parameter: 0 → `0`, 1 → `1`, 2 or 3 → `2`. Parmys writes `2` for
   every latch, and VTR's ABC rejects `3` (PHASE3's 3E rules); VPR accepts all four. Odin III's
   own latches carry 0/1 only from an `initial` value, else 2 (2D EL-40, 2F §9.4).
3. **Clock**: the `C` pin's net. Unconnected or undriven C is E3 (VPR "Latch must have a clock").
   A constant C (`$_CONST*_` or a constant `$sop`) is written (VPR accepts it) with the warning
   `write_blif: --vtr: <loc>: $_DFF_P_ '<cell>': clock is the constant <0|1|x>; VPR will never
   clock it`.
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

For each port of the type in the library's port order (a declared model's order when the design
declares the type; 1C's rule), and for each bit `k` in `0 … width−1` of the node's port (the
width the node's parameters give):

- the formal is the port name when the port's width expression is the literal `1` **or** the
  node's width for that port is 1 (Parmys's scalar `data=`, §3 item 5), else `<port>[k]`; a
  declared model's scalar flags win when the type is declared (round trip);
- an **input** bit's actual is its net's VB-10 name; when the pin is unconnected, its net
  undriven, or its net x (`$_CONSTX_`, `$_CONSTZ_`), the actual is `unconn` (counted in
  `x_to_unconn`): VPR leaves the pin open (R14), as Parmys does;
- an **output** bit's actual is its net's name; an unconnected output pin gets a fresh generated
  net, so every output bit is written (Parmys's form, and the block's VPR name is stable: R12).

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
  scalar when that maximum is 1 and the library width expression is the literal `1` or the
  instance formals are scalar (8.2), else `p[0] … p[w−1]`.

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

### 9.2 What each class is written as

| Class | `.names` input, `.latch` D | `.latch` C | `.subckt` input | output-port bit |
|---|---|---|---|---|
| 0 | `gnd` | `gnd` + warning (§7) | `gnd` | buffer `.names gnd <port bit>` / `1 1` |
| 1 | `vcc` | `vcc` + warning | `vcc` | buffer from `vcc` |
| x | `gnd` (`x_to_gnd`) | E3 | `unconn` (`x_to_unconn`) | buffer from `gnd` (`x_to_gnd`) |
| U | `unconn` as read, counted in `unconn_logic`, one warning (below) | `unconn` + warning | `unconn` | buffer from `unconn` as read |
| signal | its name | its name | its name | 1C's port-bit naming |

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

The plain writer changes only here: a `$_CONST0_`/`$_CONST1_` net is written as `gnd`/`vcc` and
a `$_CONSTX_`/`$_CONSTZ_` net as `unconn` everywhere (not only on hard-block inputs), with the
generators of §9.3, instead of today's `.subckt $_CONST0_ Y=…`. That is Parmys's `write_blif -true
+ vcc -false + gnd -undef + unconn`, which the Phase 2 exit comparison (2H) needs. Goldens
contain no `$_CONST*_` cell (the reader keeps constants as `$sop`), so 1C's round trip is
unaffected; Odin III-built fixtures that expected `.subckt $_CONST0_` are updated in 3E-1.

## 10. Names (VB-10)

1. **Legal bytes**: `0x21`–`0x7E` except `=` `"` `#` `\` `%` (R1; `%` is the escape). Any other
   byte (blank, control, non-ASCII, the five) is written `%XX` (two upper-case hex digits).
2. **Leading `.`** is written `%2E` (a name equal to a directive would lex as one).
3. **Reserved whole names**: `re fe ah al as NIL 0 1 2 3` (R2), `gnd vcc unconn` (VB-9, unless
   the net is that generator), and any name starting `out:` (R12: outpad blocks are
   `out:<output>`) are prefixed `%` (`%re`, `%unconn`, `%out:x`). `%` followed by anything but two
   hex digits never comes from rule 1, so the mapping stays injective.
4. **Uniqueness**: legalisation runs before 1C's uniqueness pass, so a legalised name that
   collides with another net's name falls back to `$n<ID>`, `$n<ID>$1`, … as today.
5. **Port bit names** use the wire's declared index (IR wire spec `{msb, lsb}`: bit k is
   `lsb + k` descending, `lsb − k` ascending; 2D EL-21), so `a[8:1]` writes `a[1] … a[8]` and
   Verilator's port matcher (3H, decision 16) pairs them with the RTL. A BLIF-read design's wires
   are `[w−1:0]`, so goldens are unaffected. A port's own legalised name is reported once per
   port: `write_blif: --vtr: port '<p>' written as '<legal>' (VPR cannot read '<p>')`.
6. **Short** (default): the net's own name, else its primary wire bit, else `$n<net ID>` (1C).
7. **Provenance** (`--name-style provenance`, 3E-4): `hier/<short name>@file:line` from the
   net's provenance record, as the 1F Verilog writer builds it, then rules 1–4 (blanks in paths
   become `%20`). Objects without a location keep their short name. DESIGN §13 #6: 3H measures
   VPR time and memory with both styles; short stays the default until then.
8. **Renaming info**: one line, `write_blif: --vtr: N names legalised (first: '<orig>' ->
   '<legal>')`, when N > 0.

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
| E2 | `<loc>: <type> '<cell>': level-sensitive latch; VPR accepts only rising-edge latches` | `$_DLATCH_P_`/`_N_` |
| E3 | `<loc>: <type> '<cell>': flip-flop without a clock; VPR requires a clock on every latch` | `$_FF_`, or `$_DFF_P_` with C unconnected, undriven or x |
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
§10 items 5 and 8 (renames), §5 (modules skipped), and the summary line `write_blif: --vtr:
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
- **3D `abc`**: `--via-blif` exchanges plain-dialect BLIF through 1C, not `--vtr` (ABC reads
  `.subckt`-free combinational cones there). After `abc`, LUTs are `$sop` (BLIF exchange) or
  `$lut` (MiniLUT); both are written by §6. 3D's pass-run argument string is what §5 prints.
- **3F**: the wrapper's script ends with `write_blif --vtr`; it passes `--lut-k` from
  `determine_lut_size`.
- **3H**: legs O, O′, P′ and R (§16.5) consume `--vtr` files; 3H's `check-lib` uses §15.2.

## 15. Tools (VB-15, VB-16)

### 15.1 `tools/blif-vtr-check` — the VPR-free checker (decision 18)

`tools/blif-vtr-check/blif-vtr-check` (bash shim) → `blif_vtr_check.py` (stdlib only, `ruff` and
`mypy --strict` clean). It does not reuse `tools/blif/blif.py`'s permissive reader: it carries its
own lexer that mirrors libblifparse's token rules (R1–R3, R5, R8), so it rejects what VPR rejects.

```
blif-vtr-check [--models FILE] [--lut-k K] [--strict] [--eblif] [--max-errors N] FILE...
```

- `--models` default `tests/vtr/arch/k6_frac_N10_frac_chain_mem32K_40nm.models.json`; `--lut-k`
  default the JSON's `lut_size` (6).
- `--strict`: also Odin III's own conventions (C-S rows below).
- `--eblif`: accept `.conn .attr .param .cname` (VPR eblif mode); default off, as for `.blif`.
- Exit: 0 every file legal, 1 any violation (located `file:line: message` lines on stdout, at
  most `--max-errors` per file, default 50), 2 usage or I/O.

| Check | Mirrors | Message (`blif-vtr-check: ` + `file:line: `) |
|---|---|---|
| C1 | R1 | `illegal character <0xHH> in name` / `unterminated quoted string` |
| C2 | R3 | `directive '<d>' is not read by VPR` / `'<d>' needs --circuit_format eblif` |
| C3 | R13 | `second model '<m>' with primitives; VPR reads one` / `black-box model '<m>' is not an arch model` / `stub port '<p>' of '<m>': <reason>` |
| C4 | R4 | `.names with <N> inputs exceeds K=<K>` |
| C5 | R5 | `cover row has <N> columns, expected <M>` / `cover row character '<c>'` |
| C6 | R6, R7, R8, R2 | `latch type '<t>' (VPR accepts only re)` / `latch without a clock` / `latch init '<v>'` / `'<w>' on a .latch line lexes as a keyword` |
| C7 | R9, R10 | `.subckt model '<m>' is not an arch model` / `no port '<p>' on model '<m>'` / `port '<p>[<i>]' exceeds width <W> of '<m>'` / `output formal '<p>' connected twice` |
| C8 | R12 | `duplicate block name '<b>' (<kind> and <kind>)` |
| C9 | R16 | `net '<n>' has <N> drivers` |
| C-S1 | VB-7 | `latch init 3 (write 2; VTR's ABC rejects 3)` |
| C-S2 | VB-2a | `net 'unconn' feeds a .names/.latch pin (VPR leaves it open)` |
| C-S3 | VB-9 | `'<gnd\|vcc\|unconn>' is not a constant generator of the right value` |
| C-S4 | VB-5 | `missing header` / `.clock` present / `stub '<m>' unused` |

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

Widths follow R11 (the largest `num_pins` over the model's pb_types); `lut_size` and
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
random tables for k = 5…16 (evaluation only), 3E-4.

### 16.2 Fixtures (CI, `tests/golden/blif-vtr/`)

`<name>.in.blif` (or `<name>.o3` script on hand IR) → `<name>.vtr.blif` expected bytes, run by a
CTest driver `blif_vtr_fixtures` with `odin3 -p "read_techlib lib/vtr.o3lib; read_blif
<in>; write_blif --vtr <out>"`, then `blif-vtr-check --strict` on every expected file (the checker
and the writer agree). Fixtures: the committed `tests/golden/blif/*.parmys.blif` sample (adder
chain, `ff`, `dffsre`, `pow`), a two-clock design, a RAM slice with `unconn` addresses, a
`multiply` with narrow inputs, an Odin II file with init 3 and `\` continuations, a constant
output, an x output, and one negative fixture per E-class with its expected log
(`<name>.err`). `tests/tools/test_blif_vtr_check.py` covers C1–C9 and C-S1–4 on inline texts.

### 16.3 Round trip with our BLIF reader (local; CI on fixtures)

`tools/blif-roundtrip --vtr` (a new mode of the 1C script) on a golden tree, smallest first:

1. `W1 = write_blif --vtr (read_blif G)`; `W2 = write_blif --vtr (read_blif W1)`. **Oracle:
   W1 == W2 byte for byte** (idempotence; the header line is identical because nothing in it
   depends on the run).
2. **Parmys k6 goldens**: `netlist-compare G W1` identical (exit 0) for every written file. The
   survey (§3) predicts all 463 non-refused files; the 4 `fe` files must be refused with E1.
3. **Odin II k6 goldens**: `netlist-compare --vtr-normalize G W1` identical. `--vtr-normalize`
   (new flag, 3E-3) applies the writer's documented rewrites to both sides before
   canonicalising: latch init 3 → 2; an omitted `.subckt` input formal bit ≡ `=unconn`; `.clock`
   lines dropped; VB-10 names decoded. The 6 `fe` files → E1, the 92 files with `.names` wider than
   6 → E7, the 2 multi-driver files → E12; each refused file's original is also rejected by VPR
   (§16.4).
4. `equiv-check G W1` on every Odin II golden written in step 3 that has no RAM (PHASE0 #8), as a
   second, semantic oracle for the normalisation itself.
5. CI: steps 1–2 on `tests/golden/blif/*.parmys.blif` and the 16.2 fixtures.

### 16.4 VPR reads every golden-equivalent BLIF (local, heavy-run rule)

`tools/vtr-read-check` (bash + a small Python report): for each `status=ok` k6 golden G
(923 files), smallest first:

```
odin3 -p "read_techlib lib/vtr.o3lib; read_blif G; write_blif --vtr W"      # W or a refusal
$VTR_ROOT/vpr/vpr <arch> W --circuit_format blif --exit_before_pack on       # in a temp dir
$VTR_ROOT/vpr/vpr <arch> G --circuit_format blif --exit_before_pack on       # the original
blif-vtr-check --strict W ; blif-vtr-check G
```

`--exit_before_pack on` makes VPR read, clean (R14, R15) and verify the netlist and print its
statistics, then stop: VPR's whole reader at a fraction of a route's cost. **Oracles**:

- every written W: VPR exits 0 after printing its netlist statistics (the atom-netlist block
  counts; no pack output), and `blif-vtr-check --strict W` exits 0;
- every refused G: the refusal is E1, E7 or E12 and VPR also rejects G (`Only rising edge latches
  supported`, `BLIF .names input size …`, or the R16 assertion); any other pairing is a failure;
- **checker agreement**: `blif-vtr-check G`'s verdict equals VPR's on G for all 923, and on every
  W; and on `tests/vtr/blif-bad/*.blif` (one file per C-check, committed with the VPR message it
  draws, verified by this script) VPR rejects exactly the files the checker rejects.

The sweep is a VTR-set sweep under the standing rules: it runs as
`flock /tmp/claude-1000/odin3-heavy.lock systemd-run --user --scope -p MemoryMax=6G -p
MemorySwapMax=0 tools/vtr-read-check …`, one file at a time, run directories under
`~/odin3-ws/work/vtr-read-check/` deleted after each file passes. Report:
`~/odin3-ws/work/vtr-read-check/report.tsv` (`golden, tool, written|refused, error class, vpr_W,
vpr_G, checker_W, checker_G, verdict`) and a one-paragraph PHASE3 checklist entry.

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
| 16.3 round trip on all k6 goldens | — | yes (3E-3) | yes, when odin3 changed |
| `vtr-models check` (schema) / `check-lib` | yes | yes | yes, plus `check --arch` |
| 16.4 VPR read sweep | — | yes, heavy (3E-3) | yes, heavy |
| 16.5 R leg | — | 3H | yes, heavy |

## 17. Files

| File | Content |
|---|---|
| `src/backends/blif/writer.h` | `odin3_blif_dialect`, `odin3_blif_name_style`, `odin3_blif_write_opts`, the constants, `odin3_blif_write_with` |
| `src/backends/blif/writer.c` | dialect switch: model selection, `.clock` omission, gate covers, latch init map, formal rules, stubs, constant generators (both dialects), legal names hook |
| `src/backends/blif/vtr.[ch]` | `odin3_blif_vtr_report`, `odin3_blif_vtr_preflight`, source classes (§9.1), `odin3_blif_vtr_legal_name`, E-texts |
| `src/backends/blif/isop.[ch]` | `odin3_blif_isop` (3E-4) |
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
| 3E-1 writer | opts struct and `odin3_blif_write_with`; pass options; preflight E1–E13 with the exact texts; §6 cells (gate covers, `$sop` checks); §7 latches; §8 formals and stubs; §9 classes and generators in both dialects (plain-mode x → `unconn`, PHASE3 #20); §10 rules 1–6 and 8; header; §16.1 unit tests; §16.2 fixtures; PASSES.md row | 1C, 1G (merged) | Opus `high` |
| 3E-2 checker | `blif-vtr-check` C1–C9, C-S1–4; `vtr-models extract/check/check-lib`; the two committed JSON files; `tests/vtr/blif-bad/`; unittest; CI wiring (the checker over 16.2's expected files) | none (parallel with 3E-1) | Opus `high`; Sonnet `high` for the negative-fixture corpus |
| 3E-3 evidence | `blif-roundtrip --vtr`, `netlist-compare --vtr-normalize`, `vtr-read-check`; the full k6 sweeps of §16.3–16.4 run by the controller under the heavy lock, smallest first; report and PHASE3 checklist entry | 3E-1, 3E-2 | Opus `high` for the scripts; Sonnet `high` to run the sweeps and write the report |
| 3E-4 LUTs and names | `odin3_blif_isop` and the `$lut` row of §6; `--name-style provenance` (§10 item 7) | 3D's `$lut` cell (decision 9); 1F's provenance naming | Opus `high` |

Lanes: 3E-1 ‖ 3E-2 → 3E-3; 3E-4 after 3D-`$lut`. 3D can start once 3E-1 lands (its P′ leg
writes `--vtr`). The agent says when to switch: Opus `high` for 3E-1/2/4 sessions, Sonnet for the
3E-3 sweep sessions (PHASE0 §9).

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
5. **The plain-mode change** (§9.4) touches 1F/1C fixtures built from Odin III IR; 3E-1 updates
   them in the same PR.

## 20. Amendments to other documents (when the PRs land)

- `docs/PASSES.md`: the `write_blif` row gains the options and the dialect (3E-1).
- `docs/PHASE3.md` checklist: 3E ticked with the §16.3/16.4 report figures (3E-3); decision 20's
  sub-bullet (x → `unconn`) marked done by 3E-1.
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
3. **Port bits by declared Verilog index** (§10 item 5). *Recommended*: Verilator's port matcher
   (decision 16) needs it for ranges not ending at 0; the plain writer keeps 1C's 0-based index
   (no golden is affected either way).
4. **`--lut-k` default 6** rather than reading K from a design record. *Recommended*: both arches
   have K = 6, and 3F passes VTR's `determine_lut_size` explicitly.
