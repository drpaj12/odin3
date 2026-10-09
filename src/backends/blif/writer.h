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
 * run of ports of one direction as one `.inputs` or `.outputs` line, then `.clock` from the
 * module's ODIN3_BLIF_ATTR_CLOCK attribute, then the cells in node ID order (port nodes skipped),
 * then the port buffers (below), then `.end`. Then every declared black-box model
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
 * out re|fe|ah|al ctrl init` and `$_FF_` → `.latch in out init` (init always written); any other
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
 * Errors (logged): ODIN3_ERR_IO when path cannot be opened or a write fails ("path: cannot open
 * …", "path:line: write failed …"); ODIN3_ERR_INVALID_ARG for a NULL argument, a port of direction
 * INOUT, two ports of one BLIF name on different nets, a `.subckt` cell with parameters the
 * reader would not derive or with both ports of one BLIF name connected; ODIN3_ERR_NO_MEMORY on out
 * of memory. On any failure after path was opened, the partly written file is removed when it is a
 * regular file (a device such as /dev/full is left alone). Names are written as they are: a name
 * holding blanks or other characters BLIF cannot carry is not escaped.
 */
odin3_status odin3_blif_write(const odin3_design *design, const char *path);

#endif
