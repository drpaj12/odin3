/*
 * writer.h — Yosys-schema JSON netlist writer (spec 1F, "JSON").
 */
#ifndef ODIN3_BACKENDS_JSON_WRITER_H
#define ODIN3_BACKENDS_JSON_WRITER_H

#include "ir/design.h"
#include "odin3/odin3.h"

/*
 * Writes every module of the design to path in the Yosys JSON netlist schema (read by netlistsvg,
 * nextpnr and `yosys read_json`). Modules, ports, cells and netnames come out in ID order, so the
 * output is deterministic. Constants are the bit strings "0", "1", "x", "z"; every other net is
 * one integer bit, net ID + 1 (so >= 2). Cell and netname keys share one namespace per module
 * (Yosys RTLIL, IR-14): a netname keeps the user's name, a cell named like a net or wire, and a
 * generated key that is taken, get "$u<n>" appended. Names are JSON strings; invalid UTF-8
 * bytes become U+FFFD. $sop becomes $lut (at most 6 inputs) or $sop with
 * DEPTH/TABLE; latch INIT goes to an `init` attribute of the Q net (2 and 3 as "x").
 *
 * Attributes (odin3_attr_foreach order, backends/common/attrs.h) of modules, cells, wires and
 * nets with a netname entry are written to their "attributes" (values as parameters are); BLIF
 * `.attr K` extras become attribute K and `.param K` extras cell parameter K, spelled as Yosys
 * read_blif reads them; `blif_extras` is skipped. A user `src` replaces the provenance `src`;
 * the latch `init` wins over a user `init`; a repeated key keeps the first.
 *
 * After the IR modules, each declared black-box or hard model (odin3_design_declared_model, once,
 * names not starting with `$`) is a module with attribute `blackbox`, its ports (fresh bits from
 * 2; a parameter-sized port at the type's defaults) and no cells, as Yosys write_json writes one.
 *
 * ODIN3_ERR_INVALID_ARG for a NULL argument; ODIN3_ERR_IO (logged "path: reason") when the file
 * cannot be created or written, in which case a partial file is removed; ODIN3_ERR_NO_MEMORY on
 * out of memory. The design is not modified.
 */
odin3_status odin3_json_write(const odin3_design *design, const char *path);

#endif
