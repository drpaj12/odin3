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
 * report. Returns ODIN3_OK when report->errors == 0, ODIN3_ERR_INVALID_ARG when it is not or an
 * argument is NULL, ODIN3_ERR_NO_MEMORY. Reads only; O(nodes + pins). */
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
 * `x_to_gnd` the X and UNDRIVEN .names inputs, .latch D pins and output-port bits,
 * `x_to_unconn` the X and UNDRIVEN non-clock inputs of hard cells, `unconn_logic` the class-U
 * .names inputs and .latch D pins (of cells not refused); `renamed` (VB-10) and
 * `modules_skipped`. Returns as odin3_blif_vtr_preflight (out of memory logged `write_blif:
 * --vtr: out of memory`).
 */
odin3_status odin3_blif_vtr_plan_build(const odin3_design *design,
                                       const odin3_blif_write_opts *opts, odin3_blif_vtr_plan *plan,
                                       odin3_blif_vtr_report *report);

/* Frees what plan holds and zeroes it. */
void odin3_blif_vtr_plan_free(odin3_blif_vtr_plan *plan);

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

#endif
