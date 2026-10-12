/*
 * writer.h — BLIF writer: the inverse of the BLIF reader (src/frontends/blif/reader.h).
 */
#ifndef ODIN3_BACKENDS_BLIF_WRITER_H
#define ODIN3_BACKENDS_BLIF_WRITER_H

#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>

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

#define ODIN3_BLIF_VTR_DEFAULT_K 6U /* k6_frac_N10_frac_chain_mem32K_40nm and EArch */
#define ODIN3_BLIF_VTR_MIN_K 2U
#define ODIN3_BLIF_VTR_MAX_K 16U      /* VB-12 */
#define ODIN3_BLIF_VTR_MAX_ERRORS 20U /* refusals logged before "N more" */

/*
 * Writes design as BLIF to path (created or truncated). Nothing in the design changes.
 *
 * Modules: the design's top (odin3_design_top) first, since BLIF's first model is the top, then
 * the others in creation order (all in creation order when no top is set): `.model`, then the ports
 * in port order, each run of ports of one direction as one `.inputs` or `.outputs` line, then
 * `.clock` from the module's ODIN3_BLIF_ATTR_CLOCK attribute, then the cells in node ID order (port
 * nodes skipped), then the port buffers (below), then `.end`. Then every declared black-box model
 * (odin3_design_declared_model) in declaration order, once each even when listed twice (the first
 * declaration): `.model`, the ports of the declaration as written
 * (odin3_design_declared_model_decl: its order, widths and scalar flags) as above, `.blackbox`,
 * `.end`. Implicit black boxes (local BLACKBOX types that are not
 * declared models) get no `.model`.
 *
 * Ports keep their names: bit k of a port is written as its port bit name, the port wire's
 * ODIN3_BLIF_ATTR_PORT_NAME (frontends/blif/attrs.h) or else the port's name, followed by `[k]`
 * unless the port is scalar. Port bit names are reserved in their module: the net on a port bit is
 * written under that name, except when the net has another name of its own or is on several port
 * bits with different names (an input wired to an output, two outputs on one net). Then the net
 * keeps its own name (or the first port bit name in port order) and each other port bit gets a
 * buffer `.names <port bit> <net>` (input) or `.names <net> <port bit>` (output) with the row
 * `1 1`, written after the cells, so the interface and the output's driver read back. Two ports
 * of one BLIF name share their net (a name in both `.inputs` and `.outputs`); on different nets
 * the write is refused.
 *
 * Cells: `$sop` → `.names` (its pins' nets, then the cover rows as stored; a zero-input cover is
 * one output character per row); `$_DFF_P_`/`$_DFF_N_`/`$_DLATCH_P_`/`$_DLATCH_N_` → `.latch in
 * out re|fe|ah|al ctrl init` and `$_FF_` → `.latch in out init` (init always written); the eleven
 * word flip-flops and latches of 2E's IR change D (`$adffe $sdffe $sdffce $aldff $aldffe $dffsr
 * $dffsre $dlatch $adlatch $dlatchsr $sr`) are refused, `path: model 'm': word flip-flops and
 * latches have no BLIF form; cannot write a cell of '$dlatch'`; any other
 * type → `.subckt type formal=actual …` in port order (a declared type's in its declaration's
 * order), unconnected pins skipped, formals by port
 * name (`p[k]` for bit k of a port that is not scalar — as the type's declaration writes it when
 * the design declares the type; a module port's ODIN3_BLIF_ATTR_PORT_NAME when it has one). BLIF
 * has no way to give parameters, so the node's must be those the reader derives (reader.h): the
 * declared-model parameters of a declared type, else those the connected formals imply
 * (odin3_celltype_infer_params; the defaults for a type no port of which a parameter sizes). A
 * model with a name in both `.inputs` and `.outputs` has two ports of one BLIF name; BLIF can
 * connect only one of them per instance (the reader binds the formal to the input), so a cell with
 * both connected is refused. After each cell: `.cname` with the node's name, then each
 * `.attr`/`.param` listed by its ODIN3_BLIF_ATTR_EXTRAS attribute, in that order; a listed key
 * without a `blif.attr:`/`blif.param:` prefix or without a STRING attribute on the cell is skipped
 * with a warning.
 *
 * Other net names: the net's own name, unless it is a port bit name of another net; else its
 * primary wire bit (the wire's ODIN3_BLIF_ATTR_PORT_NAME or name, with `[index]` unless the wire
 * is a scalar port or a 1-bit non-port wire); else `$n<net ID>`. A generated name already used by
 * another net or port bit of the module, or by an earlier generated name, falls back to
 * `$n<ID>`, then `$n<ID>$1`, `$n<ID>$2`, … so names are unique and depend only on the IR. An
 * unconnected pin of a `.names` or `.latch` gets a fresh dangling net `$p<pin ID>` (made unique
 * the same way). Generated names are stable until `compact`.
 *
 * A directive line longer than 100 columns wraps between tokens with ` \` (continuation lines
 * are indented by two spaces); a single longer token stays whole, and cover rows never wrap.
 * Output is deterministic.
 *
 * Errors (logged): ODIN3_ERR_IO when the temporary cannot be created, a write fails or the rename
 * fails ("path: cannot open …", "path:line: write failed …", "path: cannot rename …");
 * ODIN3_ERR_INVALID_ARG for a NULL argument, a port of direction INOUT, two ports of one BLIF name
 * on different nets, an IR change D flip-flop or latch, a `.subckt` cell with parameters the reader
 * would not derive or with both ports of one BLIF name connected; ODIN3_ERR_NO_MEMORY on out of
 * memory. The text goes to a private temporary beside path (util/file.h), renamed over path on
 * success; on any failure only the temporary is removed, so an existing file at path keeps its old
 * contents (a write into a directory that refuses the temporary, such as /dev, is "cannot open").
 * Names are written as they are: a name holding blanks or other characters BLIF cannot carry is not
 * escaped.
 */
odin3_status odin3_blif_write(const odin3_design *design, const char *path);

/* Writes design to path in opts' dialect; opts NULL = PLAIN with defaults. odin3_blif_write is
 * odin3_blif_write_with(design, path, NULL). VTR: runs odin3_blif_vtr_preflight first and opens
 * path only when it passes, so a refused design leaves no file. Errors: as odin3_blif_write,
 * plus ODIN3_ERR_INVALID_ARG for a preflight refusal or an option out of range.
 *
 * opts is read during the call only. The refusals come in this order, each logged and
 * ODIN3_ERR_INVALID_ARG with nothing opened: a NULL design or path; a design with processes;
 * a dialect or name style out of its enum ("odin3_blif_write_with: unknown dialect" / "unknown
 * name_style"); PLAIN with a non-zero lut_k or PROVENANCE ("odin3_blif_write_with: lut_k and
 * name_style apply to the VTR dialect only"); a non-zero lut_k outside 2..16
 * ("odin3_blif_write_with: lut_k <k> is outside 2..16"); VTR with PROVENANCE ("write_blif: --vtr:
 * --name-style provenance is not available yet (3E-6)"); then the preflight's refusals.
 *
 * VTR names (spec 3E VB-10): every net, port bit and generated name is written in its
 * odin3_blif_vtr_legal_name form, and a net used as a `.latch` control or a hard cell's clock
 * input in its odin3_blif_vtr_clock_name form, before 1C's uniqueness rule (a form another net or
 * port bit already has falls back to `$n<ID>`, `$n<ID>$1`, …; nets in ID order). A net named
 * `gnd`/`vcc`/`unconn` that a zero-input $sop of value 0/1/0 drives keeps its name. Port bits are
 * named by their wire's declared index, as in the plain dialect. The top is named once, by the
 * preflight (odin3_blif_vtr_plan_build), and written under the names its plan holds.
 *
 * VTR output (spec 3E §5-§9): the header line `# Odin III <version> write_blif --vtr (K=<K>,
 * names short|provenance)` and a blank line; `.model <top>`; `.inputs`/`.outputs` (no `.clock`);
 * the generators `.names gnd`, `.names vcc` / `1`, `.names unconn` that some pin or buffer reads,
 * in that order, each left out when a written zero-input $sop already is it; the cells in node ID
 * order; the port buffers; `.end`; then the hard-block stubs (§8.3): for each hard type a live
 * node of the top uses, sorted by name in byte order, a blank line, `.model <m>`, its ports,
 * `.blackbox`, `.end`, a declared type as declared, an undeclared library cell in library port
 * order with each port at the widest width a live node gives it (`p` when that is 1). No other
 * module, black-box model, `.cname`, `.attr` or `.param` is written. Cells: a $sop or a Yosys
 * gate is a `.names` (§6's gate rows; a cell with inputs whose function is constant is the
 * zero-input generator on its output, a 1-input one has the one row `1 1` or `0 1`, any other
 * keeps its rows); a $_CONST*_ writes no line; $_DFF_P_ is `.latch <D> <Q> re <C> <init>` with
 * init 0 1 2 2 for INIT 0 1 2 3; a hard cell is a `.subckt` with every formal bit at the node's
 * widths, sorted by port name in byte order then by bit (§8.2): `p` for a 1-bit port (as declared
 * when the type is), else `p[k]`; an unconnected output reads its own generated `$p<pin ID>`. An
 * input pin reads, by its net's class (odin3_blif_vtr_net_class): 0 `gnd`, 1 `vcc`, x or undriven
 * (an unconnected pin included) `gnd` (`unconn` on a `.subckt` input), Parmys's `unconn`
 * `unconn`, else the net's name. An output-port bit on a $_CONST*_, undriven or `unconn` net gets
 * a buffer from `gnd`/`vcc`/`gnd`/`unconn`; one on a net a constant $sop or cell drives under
 * another name gets that generator again under its own name (one driver per net). The summary
 * info line is logged once the file is written. */
odin3_status odin3_blif_write_with(const odin3_design *design, const char *path,
                                   const odin3_blif_write_opts *opts);

/*
 * E11 (spec 3E §11; 1C's rule, both dialects): *ok when every parameter of node (a live node of
 * module, of a type with parameters or none) equals the value the BLIF reader would give its
 * `.subckt`: the declared model's parameters when the design declares the type (its first
 * declaration), else those its connected formals imply (odin3_celltype_infer_params). When not,
 * appends to bad the parameters that differ, in definition order, each in `'…'`, joined by `, `
 * (plan 3E point (w)): `'A_WIDTH', 'B_WIDTH'`. ODIN3_ERR_NO_MEMORY (not logged; bad may hold part
 * of the list) when scratch memory runs out; ODIN3_ERR_INVALID_ARG (logged by the IR) when the
 * type cannot infer its parameters.
 */
odin3_status odin3_blif_params_check(const odin3_design *design, const odin3_module *module,
                                     odin3_node_id node, odin3_strbuf *bad, bool *ok);

/* The VTR dialect's names of the design's top (spec 3E VB-10), as the writer's naming machinery
 * chose them: built once by the preflight (odin3_blif_vtr_name_top), then handed to the writer
 * through the plan (vtr.h), so naming runs once per write. Opaque; odin3_blif_names_free frees. */
typedef struct odin3_blif_names odin3_blif_names;

/* What odin3_blif_vtr_name_top names and logs. */
typedef struct odin3_blif_names_req {
    const odin3_design *design;        /* its top must exist (E13 is the caller's) */
    const odin3_blif_write_opts *opts; /* the VTR dialect */
    bool log_names;   /* log the port warnings and the info line (no refusal so far) */
    bool log_refusal; /* log a refusal (the preflight's cap is not reached) */
} odin3_blif_names_req;

/*
 * Names the design's top as the VTR dialect writes it (VB-10: rules 1-3 and 8 before 1C's
 * uniqueness fallback; spec 3E §10), without writing anything, and stores the result in *names
 * (NULL on failure; the caller frees it). When log_names, logs, once per port whose bit names
 * changed, the warning `write_blif: --vtr: port '<p>' written as '<legal>' (not a legal name
 * for VTR's BLIF readers)`, or, when rule 8 alone changed them (a clock port), `write_blif: --vtr:
 * clock port '<p>' written as '<legal>' (VTR's latch clock domains cannot hold '|' or '_^_')`;
 * and, when N > 0, the info line `write_blif: --vtr: N names legalised (first: '<orig>' ->
 * '<legal>')` for the top's `.model` name when rules 1-2 changed it, else the name of the lowest
 * net ID (an unconnected port bit sorts last; on a tie, the port bit). N, set in *renamed, counts
 * the written names VB-10 changed: the `.model` name, each port bit name, and each net's own or
 * wire bit name (a net written under its own port bit's name counts once). Returns ODIN3_OK;
 * ODIN3_ERR_INVALID_ARG when two ports on different nets end up with one BLIF name (logged as
 * `write_blif: --vtr: model '<top>': two ports on different nets have the BLIF name '<n>'` when
 * log_refusal); ODIN3_ERR_NO_MEMORY (logged `write_blif: --vtr: out of memory`).
 */
odin3_status odin3_blif_vtr_name_top(const odin3_blif_names_req *req, odin3_blif_names **names,
                                     uint32_t *renamed);

/* Frees names (NULL is a no-op). */
void odin3_blif_names_free(odin3_blif_names *names);

#endif
