/*
 * vtr.h — the VTR dialect of the BLIF writer (spec 3E): the preflight that refuses, before any byte
 * is written, every construct VPR would reject or misread, and VB-10's legal names.
 */
#ifndef ODIN3_BACKENDS_BLIF_VTR_H
#define ODIN3_BACKENDS_BLIF_VTR_H

#include "backends/blif/writer.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "util/hash.h"
#include "util/str.h"
#include "util/u64map.h"

#include <stdbool.h>
#include <stdint.h>

/* What the VTR preflight found (spec 3E VB-11); the counts feed the info lines and the tests. */
typedef struct odin3_blif_vtr_report {
    uint32_t errors;                  /* refusals; > 0 means nothing may be written */
    uint32_t names, latches, subckts; /* cells the write will emit */
    uint32_t x_to_gnd;        /* x/undriven bits on .names/.latch/output pins written as gnd */
    uint32_t x_to_unconn;     /* x/undriven/unconnected hard-block input bits written as unconn */
    uint32_t unconn_logic;    /* .names/.latch input pins on a net named 'unconn' (VB-2a) */
    uint32_t renamed;         /* names legalised by VB-10 */
    uint32_t modules_skipped; /* modules other than the top, not written */
} odin3_blif_vtr_report;

/* Checks that the design's top can be written in the VTR dialect with opts (lut_k resolved).
 * Logs each refusal (VB-11, at most ODIN3_BLIF_VTR_MAX_ERRORS, then one count line) and fills
 * report; with no refusal it logs what odin3_blif_vtr_plan_build logs, then the summary line.
 * Returns ODIN3_OK when report->errors == 0, ODIN3_ERR_INVALID_ARG when it is not or an argument
 * is NULL, ODIN3_ERR_NO_MEMORY. Reads only; O(nodes + pins). */
odin3_status odin3_blif_vtr_preflight(const odin3_design *design, const odin3_blif_write_opts *opts,
                                      odin3_blif_vtr_report *report);

/* The source class of an input pin or an output-port bit (spec 3E §9.1). */
typedef enum odin3_blif_src_class {
    ODIN3_BLIF_SRC_SIGNAL = 0,
    ODIN3_BLIF_SRC_0,
    ODIN3_BLIF_SRC_1,
    ODIN3_BLIF_SRC_X,
    ODIN3_BLIF_SRC_UNDRIVEN,
    ODIN3_BLIF_SRC_U
} odin3_blif_src_class;

/*
 * §9.1's class of net in module, by its driving cell's own function only (constants are not
 * traced through buffers): none (an unconnected pin) or a net without a driver → UNDRIVEN;
 * driven by $_CONSTX_ or $_CONSTZ_ → X; a net named `unconn` driven by a zero-input $sop with no
 * rows (Parmys's `unconn`) → U; driven by $_CONST0_ or a zero-input $sop of value 0 → 0; by
 * $_CONST1_ or a zero-input $sop of value 1 → 1 (odin3_net_const_value; a net with several
 * drivers has none); anything else → SIGNAL. O(1).
 */
odin3_blif_src_class odin3_blif_vtr_net_class(const odin3_module *module, odin3_net_id net);

/*
 * What the preflight leaves the emitter (internal): the top, the resolved K, the class of every
 * net of the top whose class is not SIGNAL, and the top's VB-10 names (writer.h), so the write
 * names the top once. Zero-initialise before odin3_blif_vtr_plan_build; free with
 * odin3_blif_vtr_plan_free whatever the build returned.
 */
typedef struct odin3_blif_vtr_plan {
    const odin3_design *design;
    const odin3_module *top;
    uint32_t k;
    odin3_u64map *classes;   /* net ID -> odin3_blif_src_class, for the nets not SIGNAL */
    odin3_blif_names *names; /* NULL until naming succeeded */
    odin3_strbuf summary;    /* the summary info line of a clean plan (odin3_blif_vtr_plan_build) */
} odin3_blif_vtr_plan;

/*
 * The preflight, keeping its plan: checks the options (the preflight's own lines), then E13
 * alone; then the top's nodes in ID order, each with at most one of E1b, E1, E2, E3, E4, E5,
 * E6, E10, E7, E8, E11 (plan 3E point (f)), E3b per clock input of a hard cell, and E9 per
 * $port_inout; then its nets (E12, and the class map); then its names (VB-10;
 * odin3_blif_vtr_name_top logs its warnings and info line only when nothing was refused, and its
 * own refusal only below the cap). Each refusal is `error: write_blif: --vtr: <§11 text>`; past
 * ODIN3_BLIF_VTR_MAX_ERRORS they are counted only, then `write_blif: --vtr: N more errors not
 * shown` follows. report (zeroed first) counts the refusals and what the write will emit:
 * `names` the $sop and gate cells (.names lines of cells; generators and port buffers the writer
 * adds are not counted), `latches` the $_DFF_P_ cells, `subckts` the hard cells; per pin,
 * `x_to_gnd` the X and UNDRIVEN .names inputs (not those of a cell §6 writes as a constant
 * generator, whose inputs are dropped), .latch D pins and output-port bits, `x_to_unconn` the X
 * and UNDRIVEN non-clock inputs of hard cells, `unconn_logic` the class-U .names inputs (as for
 * x_to_gnd) and .latch D pins (of cells not refused); `renamed` (VB-10) and `modules_skipped`.
 *
 * When nothing is refused it then logs, in this order (plan 3E point (k)): the info line
 * `write_blif: --vtr: N modules besides the top '<top>' not written` when N > 0; per node in ID
 * order, the warnings `<loc>: $_DFF_P_ '<cell>': clock is the constant <0|1>; VPR will never
 * clock it` (§7 item 3), `<loc>: <type> '<cell>': clock input '<port>' is the constant <0|1>; VPR
 * will never clock it` (a hard cell's clock input) and `<loc>: <type> '<cell>': clock is
 * 'unconn'; VPR will never clock it` (point (j)), a clock being constant when its class is 0 or 1
 * or its net's driver is a cell §6 writes as a constant generator; and the warning `write_blif:
 * --vtr: net 'unconn' feeds N .names/.latch pins; VPR leaves them open (run abc first)` when
 * unconn_logic N > 0. It builds, but does not log, plan->summary: the info line `write_blif:
 * --vtr: '<top>': N .names, M .latch, S .subckt (adder A, multiply B, single_port_ram C,
 * dual_port_ram D), K=<K>; x bits: X to gnd, Y open (unconn)`, the types with count 0 left out of
 * the parentheses and the parentheses left out when none is left; odin3_blif_vtr_log_summary logs
 * it once the write succeeded (odin3_blif_vtr_preflight: at once). Returns as
 * odin3_blif_vtr_preflight (out of memory logged `write_blif: --vtr: out of memory`).
 */
odin3_status odin3_blif_vtr_plan_build(const odin3_design *design,
                                       const odin3_blif_write_opts *opts, odin3_blif_vtr_plan *plan,
                                       odin3_blif_vtr_report *report);

/* Frees what plan holds and zeroes it. */
void odin3_blif_vtr_plan_free(odin3_blif_vtr_plan *plan);

/* Logs plan->summary (info), when the plan has one. Allocates nothing. */
void odin3_blif_vtr_log_summary(const odin3_blif_vtr_plan *plan);

/* The class plan recorded for net: UNDRIVEN for none, SIGNAL for a net not in its map. */
odin3_blif_src_class odin3_blif_vtr_plan_class(const odin3_blif_vtr_plan *plan, odin3_net_id net);

/* The prefix of every preflight refusal and of the dialect's own lines (spec 3E §11), and the
 * writer's name for itself in the lines its VTR naming logs (`write_blif: --vtr: model …`). */
#define ODIN3_BLIF_VTR_WHO "write_blif: --vtr"
#define ODIN3_BLIF_VTR_PREFIX ODIN3_BLIF_VTR_WHO ": "

/* VB-10: appends the VPR-legal form of name to out (the identity for a legal name).
 *
 * Rules 1-3 of spec 3E §10, in one pass: a whole name that odin3_blif_vtr_reserved accepts gets a
 * `%` prefix; a leading `.` is written `%2E`; every byte outside 0x21-0x7E, and each of `=` `"`
 * `#` `\` `%`, is written `%XX` with two upper-case hex digits; every other byte is kept. `%`
 * followed by two upper-case hex digits is therefore always a byte and `%` followed by anything
 * else the reserved prefix, so the map is injective (`fe` -> `%fe`, the byte 0xFE -> `%FE`) and
 * decodable (plan 3E point (o)). A name containing `%` after the call is exactly a name that
 * changed. The exception of rule 3 for the generators `gnd`/`vcc`/`unconn` is the caller's.
 * ODIN3_ERR_NO_MEMORY leaves out as it was. */
odin3_status odin3_blif_vtr_legal_name(odin3_bytes name, odin3_strbuf *out);

/* VB-10 rules 1 and 2 alone (odin3_blif_vtr_legal_name without rule 3's `%` prefix): the form of
 * the top's `.model` name, which is never a net, so no reserved word applies to it (3E-1 final
 * review M3). ODIN3_ERR_NO_MEMORY leaves out as it was. */
odin3_status odin3_blif_vtr_legal_bytes(odin3_bytes name, odin3_strbuf *out);

/* VB-10 rule 3's reserved names: the whole name is one of `re fe ah al as NIL 0 1 2 3` (the
 * `.latch` tokens, R2) or `gnd vcc unconn` (the generators, VB-9), or it starts with `out:` (VPR's
 * outpad blocks, R12). Case-sensitive. */
bool odin3_blif_vtr_reserved(odin3_bytes name);

/* VB-10 rule 8, for a net used as a `.latch` control or a hard cell's clock input: appends legal
 * (a name odin3_blif_vtr_legal_name gave) to out with each `|` written `%7C` and the `^` of each
 * `_^_` written `%5E` (`x_^_y` -> `x_%5E_y`; overlapping `_^_^_` -> `_%5E_%5E_`), so VTR's
 * blackbox_latches.pl, which builds `latch|<type>|<clk>|<init>` and splits on `_^_`, reads the
 * clock whole. The result stays injective and decodable. ODIN3_ERR_NO_MEMORY leaves out as it
 * was. */
odin3_status odin3_blif_vtr_clock_name(odin3_bytes legal, odin3_strbuf *out);

/* The K the VTR dialect writes with: opts->lut_k, or ODIN3_BLIF_VTR_DEFAULT_K when it is 0. */
uint32_t odin3_blif_vtr_k(const odin3_blif_write_opts *opts);

/* --- emission (spec 3E §6, VB-6) ----------------------------------------------------------- */

/* The cover a `.names` cell is written from: a $sop's rows as stored, or a Yosys gate's rows of
 * §6's table. Rows are inputs + 1 bytes each (no blank): one plane character `0`/`1`/`-` per
 * input, input j in column j, then the output character. The node's first `inputs` pins are its
 * inputs in that order, and its last pin is the output. */
typedef struct odin3_blif_vtr_rows {
    const uint8_t *bits;
    uint32_t len;    /* bytes, a multiple of inputs + 1 */
    uint32_t inputs; /* k */
} odin3_blif_vtr_rows;

/* True and *rows = node's cover when node (a live node of module) is a $sop or one of the gates
 * $_BUF_ $_NOT_ $_AND_ $_OR_ $_XOR_ $_NAND_ $_NOR_ $_XNOR_ $_MUX_ (§6: `1 1`; `0 1`; `11 1`;
 * `1- 1`, `-1 1`; `10 1`, `01 1`; `0- 1`, `-0 1`; `00 1`; `11 1`, `00 1`; `1-0 1`, `-11 1` with
 * pins A B S). False, *rows untouched, for any other cell. The rows are static or the node's. */
bool odin3_blif_vtr_rows_of(const odin3_module *module, odin3_node_id node,
                            odin3_blif_vtr_rows *rows);

/* Words of a truth table of ODIN3_BLIF_VTR_MAX_K inputs (2^16 bits). */
#define ODIN3_BLIF_VTR_TT_WORDS 1024U

/*
 * The truth table of a `.names` cell (odin3_blif_vtr_rows_of) with k <= ODIN3_BLIF_VTR_MAX_K
 * inputs: bit i of tt (word i / 64, bit i % 64) is the output when input j is bit j of i. tt must
 * hold ODIN3_BLIF_VTR_TT_WORDS words; the first max(1, 2^k / 64) are written, bits from 2^k up
 * zero. A cover with rows of output `1` is their OR (rows of output `0` in a mixed cover, E8, are
 * ignored, as the $sop simulator does); one with only `0` rows is the complement of their OR; no
 * rows is constant 0. Returns false, tt untouched, for any other cell or k > 16. O(rows * 2^k).
 */
bool odin3_blif_vtr_eval(const odin3_module *module, odin3_node_id node, uint64_t *tt);

/* What a `.names` cell computes, for §6's normalisation. */
typedef enum odin3_blif_vtr_fn {
    ODIN3_BLIF_VTR_FN_NONE = 0, /* no `.names` cell, or k > 16 */
    ODIN3_BLIF_VTR_FN_COVER,    /* none of the below: written as its rows */
    ODIN3_BLIF_VTR_FN_CONST0,   /* constant 0 (any k, k = 0 included) */
    ODIN3_BLIF_VTR_FN_CONST1,   /* constant 1 */
    ODIN3_BLIF_VTR_FN_BUF,      /* k = 1: the input */
    ODIN3_BLIF_VTR_FN_INV,      /* k = 1: its complement */
} odin3_blif_vtr_fn;

/* The function of node by odin3_blif_vtr_eval. The writer writes a cell with k >= 1 of a CONST
 * function as the zero-input generator `.names <y>` (no rows, or the row `1`), its inputs
 * dropped; of BUF/INV as `.names <a> <y>` with the one row `1 1`/`0 1`; a zero-input $sop as
 * stored. */
odin3_blif_vtr_fn odin3_blif_vtr_function(const odin3_module *module, odin3_node_id node);

/* True when node is a $_CONST0_, $_CONST1_, $_CONSTX_ or $_CONSTZ_ (written as no line; §9). */
bool odin3_blif_vtr_const_cell(const odin3_module *module, odin3_node_id node);

#endif
