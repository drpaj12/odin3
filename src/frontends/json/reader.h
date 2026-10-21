/*
 * reader.h — Yosys-schema JSON netlist reader (spec 5.3:
 * docs/specs/2026-10-20-5.3-read-json-design.md).
 */
#ifndef ODIN3_FRONTENDS_JSON_READER_H
#define ODIN3_FRONTENDS_JSON_READER_H

#include "ir/design.h"
#include "ir/prov.h"
#include "odin3/odin3.h"

#include <stdbool.h>

/*
 * Options of a read. top: the name of the module to make the top, NULL for the choice of M-1 (an
 * explicit name wins over a `top` attribute of the file; a name that is no module is a located
 * error). auto_blackbox: a cell type that is neither a module of the file nor a registered cell
 * type becomes an implicit black box with a warning instead of an error (M-5). keep_src_attr
 * (5.4 L-8, `read_json --keep-src-attr`): by default a `src` attribute that parses as HDL
 * locations (L-1) becomes a provenance record (D-1, D-2) and is not kept as an attribute; with
 * keep_src_attr every `src` stays a STRING attribute and no record is made (the 5.3 behaviour).
 * The field is the inverse of the spec's `src_to_prov` so that a zero-filled struct and a NULL
 * opts both mean the 5.4 default.
 */
typedef struct odin3_json_opts {
    const char *top;
    bool auto_blackbox;
    bool keep_src_attr;
} odin3_json_opts;

/*
 * Reads the Yosys 0.55 JSON netlist (or Odin III's own `write_json` output) at path into design,
 * which must be fresh (no modules). One pass run "read_json" (odin3_json_read_in: the caller's
 * run instead); a port, wire (netname), cell and module gets an IMPORTED provenance record
 * {file = path, line, col} of its key in the file, and every net and constant node of a module the
 * record of the module.
 *
 * The file is parsed once into a value tree (json_parse.h: iterative, limits E-3) and walked
 * twice (M-0). Pass 1 makes every module with its ports and attributes, and every module with a
 * non-zero `blackbox` attribute and no cells a black-box cell type (as `read_blif` makes one from
 * `.blackbox`: odin3_celltype_declare_blackbox, a registered type of the name must match). Pass 2
 * reads, per module in file order, the bits, nets, netnames, cells and memories: nets are created
 * by ascending bit number, then the constants in the order 0, 1, x, z of their first use (a
 * constant on an output port bit takes that port's net), so the net IDs depend on the file's bit
 * numbers alone and write_json, read_json, write_json reaches a fixpoint from the second to the
 * third file (R-1). Cells are nodes in file order, named by their key, with their connections
 * made at creation; widths are checked, never changed. A cell type is a module of the file, a
 * registered cell type or a black-box module of the file (M-5); anything else is a located error,
 * or with opts->auto_blackbox an implicit black box (a port without `port_directions` takes its
 * direction from its bits: output on bits nothing else drives, inout on a module inout or when two
 * unknown ports drive one bit, input otherwise; EL-11; `port_directions` gives it when a cell has
 * one; the widths the connections give) and a WARNING per type listing the directions. `$scopeinfo`
 * cells are dropped. Yosys `$sop` (DEPTH, TABLE, WIDTH) is read as the `$sop` cover cell and a
 * `$lut` without inputs as a constant `$sop`; other parameters are typed by the registry (M-6) and
 * for an instance of a module or black box of the file they are attributes `parameter:<NAME>`.
 * Attributes are typed as Yosys types them (J-10). A `src` of a module, netname or cell that
 * parses (odin3_hdl_loc_parse, L-1) is restored (D-1, D-2): the object takes one DERIVED record,
 * parents the restored SOURCE then its own IMPORTED record, made in one operation of the run
 * (begun at the first restore); a port takes its netname's location, a one-bit netname also its
 * net; the other nets of a module keep the module's IMPORTED record (X-1). The attribute is then
 * not kept (L-8). A `src` that does not parse stays an attribute (L-6); one WARNING at the end
 * reports the count and the first example, and one INFO line the texts of over 8 locations (D-7).
 * An `init` attribute of a netname whose bits a bit flip-flop or latch Q drives sets that cell's
 * INIT. Members the schema does not list, and `odin3_`-prefixed members, are skipped and counted in
 * one INFO line (J-8, J-9); `odin3_version` above 1 is an error. A net with a second ordinary
 * driver is refused as read_blif refuses it. The top is chosen as in M-1.
 *
 * Errors are logged as "path:line:col: modules.<module>.<section>.<key>: message" (the JSON path
 * of the value, E-4). ODIN3_ERR_PARSE for malformed JSON, a schema violation, an unknown cell
 * type, unknown port or parameter, a bad value or a width mismatch; ODIN3_ERR_IO when the file
 * cannot be read; ODIN3_ERR_NO_MEMORY on out of memory; ODIN3_ERR_INVALID_ARG (logged) for a NULL
 * argument or a design that already has modules. Reading stops at the first error; the design may
 * then hold part of the file, so the caller destroys it. opts NULL means the defaults.
 */
odin3_status odin3_json_read(odin3_design *design, const char *path, const odin3_json_opts *opts);

/*
 * odin3_json_read into ctx->design inside the caller's pass run ctx (the read_json pass), so the
 * records belong to that run and no second run is opened; ctx->op becomes the last operation the
 * reader began. INVALID_ARG (logged) also for a NULL ctx or a ctx whose run is not a run of its
 * design.
 */
odin3_status odin3_json_read_in(odin3_pass_ctx *ctx, const char *path, const odin3_json_opts *opts);

#endif
