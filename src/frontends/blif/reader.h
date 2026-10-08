/*
 * reader.h — BLIF reader: models, ports and black boxes (pass 1), bodies (pass 2), into the IR.
 */
#ifndef ODIN3_FRONTENDS_BLIF_READER_H
#define ODIN3_FRONTENDS_BLIF_READER_H

#include "ir/design.h"
#include "odin3/odin3.h"

/*
 * Attributes the reader sets (IR-10), all of kind STRING:
 * - ODIN3_BLIF_ATTR_CLOCK on a module: the names of its `.clock` directives, in file order,
 *   joined by single spaces;
 * - ODIN3_BLIF_ATTR_PORT_NAME on a port wire whose BLIF name could not be its IR name because a
 *   port of the module already has that name (a net that is both `.inputs x` and `.outputs x`):
 *   the wire gets the first free name `<x>$blif_port`, `<x>$blif_port2`, … and this attribute
 *   holds `x` (the base name for a vector port). Writers print the attribute instead of the wire
 *   name.
 */
#define ODIN3_BLIF_ATTR_CLOCK "blif_clock"
#define ODIN3_BLIF_ATTR_PORT_NAME "blif_name"

/*
 * Reads the BLIF file at path into design, which must be fresh (no modules). One pass run
 * "read_blif"; every module, port node, port wire and port net gets an IMPORTED provenance record
 * {file = path, line, col = 1-based token index of its defining token, end_col}.
 *
 * Pass 1 reads every `.model` header: models become modules in file order (the first is the top),
 * `.inputs`/`.outputs` become ports in file order, and `.blackbox` models become black-box cell
 * types (odin3_celltype_declare_blackbox, IR-7b), also in file order. Port grouping: a run of
 * names `a[0] a[1] … a[w-1]` that appear consecutively, in this order, in one directive is one
 * vector port `a` of width w (written with brackets); every other name is a scalar port with its
 * exact name; ports are never reordered. Every port net is named by its exact BLIF bit name
 * (`x`, or `a[k]` for bit k of vector `a`); a name in both `.inputs` and `.outputs` is one net
 * shared by both ports. Pass 2 reads the model bodies.
 *
 * Errors are logged as "path:line: message". ODIN3_ERR_PARSE for malformed input (a directive
 * outside a model, a duplicate model, a model without `.end`, a port declared twice, a black-box
 * declaration that conflicts with a registered cell type, a black box with a body, an unknown
 * directive); ODIN3_ERR_IO when the file cannot be read; ODIN3_ERR_NO_MEMORY on out of memory;
 * ODIN3_ERR_INVALID_ARG (logged) for a NULL argument or a design that already has modules. Reading
 * stops at the first error; the design may then hold part of the file, so the caller destroys it.
 */
odin3_status odin3_blif_read(odin3_design *design, const char *path);

#endif
