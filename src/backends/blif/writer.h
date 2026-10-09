/*
 * writer.h — BLIF writer: the inverse of the BLIF reader (src/frontends/blif/reader.h).
 */
#ifndef ODIN3_BACKENDS_BLIF_WRITER_H
#define ODIN3_BACKENDS_BLIF_WRITER_H

#include "ir/design.h"
#include "odin3/odin3.h"

/*
 * Writes design as BLIF to path (created or truncated). Nothing in the design changes.
 *
 * Modules in creation order (the first is the top): `.model`, then the ports in port order, each
 * run of ports of one direction as one `.inputs` or `.outputs` line (a vector port expands to
 * `a[0] … a[w-1]`), then `.clock` from the module's ODIN3_BLIF_ATTR_CLOCK attribute, then the
 * cells in node ID order (port nodes skipped), then `.end`. Then every declared black-box model
 * (odin3_design_declared_model) in declaration order, once each: `.model`, its ports as above,
 * `.blackbox`, `.end`. Implicit black boxes (local BLACKBOX types that are not declared models)
 * get no `.model`.
 *
 * Cells: `$sop` → `.names` (its pins' nets, then the cover rows as stored; a zero-input cover is
 * one output character per row); `$_DFF_P_`/`$_DFF_N_`/`$_DLATCH_P_`/`$_DLATCH_N_` → `.latch in
 * out re|fe|ah|al ctrl init` and `$_FF_` → `.latch in out init` (init always written); any other
 * type → `.subckt type formal=actual …` in port order, unconnected pins skipped, formals by port
 * name (`p[k]` for bit k of a port that is not scalar; a module port's ODIN3_BLIF_ATTR_PORT_NAME
 * when it has one), which needs the node's parameters to equal the type's defaults (BLIF has no
 * way to give them). After each cell: `.cname` with the node's name, then each `.attr`/`.param`
 * listed by its ODIN3_BLIF_ATTR_EXTRAS attribute, in that order.
 *
 * Net names: the net's own name; else its primary wire bit (the wire's ODIN3_BLIF_ATTR_PORT_NAME
 * or name, with `[index]` unless the wire is a scalar port or a 1-bit non-port wire); else
 * `$n<net ID>`. A generated name already used by another net of the module, or by an earlier
 * generated name, falls back to `$n<ID>`, then `$n<ID>$1`, `$n<ID>$2`, … so names are unique and
 * depend only on the IR. An unconnected pin of a `.names` or `.latch` gets a fresh dangling net
 * `$p<pin ID>` (made unique the same way). Generated names are stable until `compact`.
 *
 * A directive line longer than 100 columns wraps between tokens with ` \` (continuation lines
 * are indented by two spaces); a single longer token stays whole, and cover rows never wrap.
 * Output is deterministic.
 *
 * Errors (logged): ODIN3_ERR_IO when path cannot be opened or a write fails ("path: cannot open
 * …", "path:line: write failed …"; the file may then be partly written); ODIN3_ERR_INVALID_ARG
 * for a NULL argument, a port of direction INOUT, or a `.subckt` cell with non-default
 * parameters; ODIN3_ERR_NO_MEMORY on out of memory. Names are written as they are: a name holding
 * blanks or other characters BLIF cannot carry is not escaped.
 */
odin3_status odin3_blif_write(const odin3_design *design, const char *path);

#endif
