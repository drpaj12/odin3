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
 * nets with a netname entry are written to their "attributes" (values as parameters are); a
 * `parameter:K` attribute (an instance parameter) is cell parameter K; BLIF `.attr K` extras
 * become attribute K and `.param K` extras cell parameter K, spelled as Yosys read_blif reads them;
 * `blif_extras` is skipped. A user `src` replaces the provenance `src`; the latch `init` wins over
 * a user `init`; a repeated key keeps the first.
 *
 * `src` (spec 5.4 W-1 to W-5; backends/common/hdl_loc.h) is on modules, cells and netnames, from
 * the object's HDL locations only: the SOURCE leaves of its provenance walk, in parent order, each
 * distinct location once, at most 8 joined by `|`, each as Yosys writes it (`file:L.C-EL.EC`;
 * `file:L.C` and `file:L` for a record with fewer parts). An IMPORTED leaf (a netlist's own line)
 * writes none. A file name with a byte outside 0x20..0x7e or a `|` is skipped with one WARNING per
 * file.
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

/* Which names cell and netname keys get (5.4f, spec section 13; the write_json --name-style). */
typedef enum odin3_json_name_style {
    ODIN3_JSON_NAMES_SHORT,      /* the IR names, else generated (default; odin3_json_write) */
    ODIN3_JSON_NAMES_PROVENANCE, /* hier/<short name>@file:line where provenance has one */
    ODIN3_JSON_NAMES_HASH,       /* those names as `h` + 10 base32 characters */
} odin3_json_name_style;

typedef struct odin3_json_write_opts {
    odin3_json_name_style name_style;
    const char *name_map; /* HASH only: the name map file; NULL means `<path>.names` (H-4) */
} odin3_json_write_opts;

/*
 * odin3_json_write with name options; opts NULL is odin3_json_write. PROVENANCE: a cell, a
 * non-port wire's netname and a named net's netname that has an HDL location (spec 5.4 W-2) take
 * the key `hier/<key>@file:line` (backends/prov_name.h), where <key> is the key they would have
 * (a generated `$c5`, `$n7`, a `$sop` suffix included); the "$u<n>" uniqueness suffix follows when
 * the key is taken, and `hide_name` says whether the key before this renaming started with `$`.
 * Ports (the "ports" entries and the netnames of port wires) and objects without an HDL location
 * keep their keys. HASH: those objects get `h` and 10 lower-case base32 characters of the SHA-256
 * of the PROVENANCE key (backends/common/hash_name.h; cells and netnames share one namespace, so a
 * collision extends the names of both), and the name map (H-4) is written to opts->name_map, else
 * `<path>.names`, as a private temporary opened before the netlist and renamed after it: a map
 * that cannot be created refuses the write (ODIN3_ERR_IO), a path of "-" with no name_map is
 * ODIN3_ERR_INVALID_ARG. The writer runs twice (the first pass to a null sink). The netlist is
 * otherwise the PROVENANCE one.
 */
odin3_status odin3_json_write_with(const odin3_design *design, const char *path,
                                   const odin3_json_write_opts *opts);

#endif
