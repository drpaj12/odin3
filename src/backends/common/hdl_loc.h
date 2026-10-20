/*
 * hdl_loc.h — the text of an HDL location and what the writers print (spec
 * docs/specs/2026-10-20-5.4-hdl-locations-design.md, rules L-1 to L-6, W-1 to W-5).
 *
 * An HDL location is the {file, line, col, end_line, end_col} of a SOURCE provenance leaf (IR.md
 * section 6); a netlist location (an IMPORTED leaf) is not one. This module is pure format code
 * plus one backward provenance walk: the grammar and its parser and printer, the filter that
 * picks the printable HDL locations of an object, and the decoder of the provenance names
 * `hier/short@file:line`. It changes no IR field, ID or invariant.
 */
#ifndef ODIN3_BACKENDS_COMMON_HDL_LOC_H
#define ODIN3_BACKENDS_COMMON_HDL_LOC_H

#include "ir/design.h"
#include "ir/ids.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "util/str.h"
#include "util/u64map.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* W-3: the most locations one `src` text holds. */
#define ODIN3_HDL_LOC_MAX 8U

/* --- the text (L-1 to L-3, W-4) --------------------------------------------------------------- */

/*
 * One parsed LOC: file is a view into the parsed text (never resolved, checked or unescaped);
 * line >= 1; col, end_line and end_col are 0 when the text has no such part. A form with an end
 * has end_line and end_col as written.
 */
typedef struct odin3_hdl_loc {
    odin3_bytes file;
    uint32_t line, col, end_line, end_col;
} odin3_hdl_loc;

/*
 * Parses text as SRC (L-1): `LOC { '|' LOC }`, LOC = `FILE ':' LINE [ '.' COL [ '-' ENDLINE '.'
 * ENDCOL ] ]`. Each LOC is parsed from the right (L-2): the digits form that ends the LOC, with the
 * last `:` that makes it match, and everything before it, byte for byte, is the file. A `|` splits
 * only where the text before it ends in such a form and a byte follows, so a file name holding `|`
 * reads whole in a single LOC. The text is all or nothing: one malformed LOC, an empty file, a
 * line of 0, a number over UINT32_MAX (L-3) or any trailing byte gives 0 (never an error, L-6).
 * Returns the number of LOCs in the text; the first min(count, cap) are stored in out (out may be
 * NULL when cap is 0), so a count over cap tells the caller how many it dropped (D-7). `end` before
 * `start` is accepted and kept.
 */
uint32_t odin3_hdl_loc_parse(odin3_bytes text, odin3_hdl_loc *out, uint32_t cap);

/*
 * Appends the SRC text of locs[0..n) to out, separated by `|` (W-4): `file:L.C-EL.EC` when
 * end_line != 0, else `file:L.C` when col != 0, else `file:L`. Printing then parsing is the
 * identity for every location the parser can produce with end_line != 0 or end_col == 0. No byte
 * of a file is escaped or checked (the writers filter with odin3_hdl_locs). NO_MEMORY leaves out
 * as it was.
 */
odin3_status odin3_hdl_loc_print(const odin3_hdl_loc *locs, uint32_t n, odin3_strbuf *out);

/* --- names (L-5) ------------------------------------------------------------------------------ */

/* A decoded provenance name; every view points into the decoded text. */
typedef struct odin3_hdl_name {
    odin3_bytes hier;       /* up to the last `/` before the last `@`; empty for none */
    odin3_bytes short_name; /* after that `/`, before the `@`; never empty */
    odin3_hdl_loc loc;      /* file and line only */
    uint32_t suffix;        /* the writer's `$N` uniqueness suffix, valid when has_suffix */
    bool has_suffix;
} odin3_hdl_name;

/*
 * Decodes NAME = `[HIER '/'] SHORT '@' FILE ':' LINE [ '$' N ]` (L-5). HIER and SHORT are split at
 * the last `/` before the last `@`, so a `/` inside the writer's short name moves its front part
 * into hier; the tail is FILE ':' LINE in the line form only, then an optional `$` and decimal
 * digits, which are the writer's suffix and not part of the location. False for a name that does
 * not match (no `@`, an empty short name or file, no digits, a column form, a number over
 * UINT32_MAX); *out is untouched then and the name is kept as it is (L-6).
 */
bool odin3_hdl_name_decode(odin3_bytes name, odin3_hdl_name *out);

/*
 * VB-10's inverse (backends/blif/vtr.h odin3_blif_vtr_legal_name, one step): a leading `%` that is
 * not followed by two upper-case hex digits is the reserved prefix and is dropped, then every `%`
 * with two upper-case hex digits is that byte. This is one step of `vb10_decode` in
 * tools/netlist-compare/netlist_compare.py, which repeats it to a fixed point to compare names
 * written twice; a reader undoes exactly one write. Appends the decoded text to buf (cleared
 * first), then decodes it as odin3_hdl_name_decode; *out views buf, which must outlive it.
 * ODIN3_OK with *matched false when the decoded text is not a provenance name. NO_MEMORY.
 */
odin3_status odin3_hdl_name_vtr_decode(odin3_bytes name, odin3_strbuf *buf, odin3_hdl_name *out,
                                       bool *matched);

/* --- the HDL locations of an object (W-1 to W-5) ---------------------------------------------- */

/*
 * The state of one writer's use of this module: a cache of "prov ID -> first HDL leaf" and the
 * files already warned about. Create one per write; destroy it when done.
 */
typedef struct odin3_hdl_ctx odin3_hdl_ctx;

/* who is the writer's name for its warnings (`write_json`); it must outlive the context. NULL on
 * out of memory. */
odin3_hdl_ctx *odin3_hdl_ctx_create(const odin3_design *design, const char *who);
void odin3_hdl_ctx_destroy(odin3_hdl_ctx *ctx);

/* The result of odin3_hdl_locs. */
typedef struct odin3_hdl_locs_out {
    odin3_srcloc locs[ODIN3_HDL_LOC_MAX]; /* distinct, in walk order */
    uint32_t n;
    bool capped; /* more than ODIN3_HDL_LOC_MAX distinct locations (X-3): the first are kept */
} odin3_hdl_locs_out;

/*
 * The HDL locations of the object with provenance prov (W-2, W-3): the SOURCE leaves of the
 * backward walk (odin3_prov_sources, iterative), in parent order. A leaf counts when its first
 * location has a file whose bytes are all in 0x20..0x7e; IMPORTED leaves never count. Each
 * location of a counted leaf with such a file is taken once (equal file, line, col, end_line and
 * end_col are one), up to ODIN3_HDL_LOC_MAX. A file with a byte outside that range is skipped with
 * one WARNING per file (`<who>: source file name '<name>' cannot be written; its locations are
 * skipped`, X-4). *out is cleared first; prov 0 or unknown gives n = 0. NO_MEMORY or the walk's
 * status.
 */
odin3_status odin3_hdl_locs(odin3_hdl_ctx *ctx, odin3_prov_id prov, odin3_hdl_locs_out *out);

/*
 * The first HDL location of prov, the one names and comments carry (W-3): locs[0] of
 * odin3_hdl_locs. *found is false for none. Cached per prov ID. NO_MEMORY or the walk's status.
 */
odin3_status odin3_hdl_first(odin3_hdl_ctx *ctx, odin3_prov_id prov, odin3_srcloc *loc,
                             bool *found);

/*
 * Appends the SRC text (W-4) of the locations in locs to out; the files are the design's strings.
 * Locations whose file holds `|` (or, when quote_free is true, `"` or `\`) are skipped, with one
 * WARNING per file (W-5); *n_written is the number printed. A text of none is left empty.
 * out is cleared first. NO_MEMORY.
 */
typedef struct odin3_hdl_print_req {
    const odin3_hdl_locs_out *locs;
    bool quote_free; /* the target cannot escape `"` and `\` in a file name (BLIF) */
} odin3_hdl_print_req;
odin3_status odin3_hdl_src_text(odin3_hdl_ctx *ctx, const odin3_hdl_print_req *req,
                                odin3_strbuf *out, uint32_t *n_written);

/* True when file (a strtab ID) is a non-empty name of bytes 0x20..0x7e only. */
bool odin3_hdl_file_printable(const odin3_design *design, uint32_t file);

#endif
