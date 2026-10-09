/*
 * writer.h — structural Verilog writer (spec 1F "Verilog") and its identifier escaping.
 */
#ifndef ODIN3_BACKENDS_VERILOG_WRITER_H
#define ODIN3_BACKENDS_VERILOG_WRITER_H

#include "ir/design.h"
#include "odin3/odin3.h"
#include "util/hash.h"
#include "util/str.h"

#include <stdbool.h>

/*
 * Writes design as structural Verilog-2005 to path (created or truncated). Nothing in the design
 * changes. Output depends only on the IR (deterministic).
 *
 * One `module` per IR module in creation order, with an ANSI header: ports in port order, as
 * `input`/`output`/`inout`, `signed` when the port wire is, `[msb:lsb]` from the port wire unless
 * the port is scalar. Then declarations, alias assignments, cells in node ID order (port nodes
 * skipped) and `endmodule`. Then one `(* blackbox *)` empty module stub per black-box or hard cell
 * type: the declared models (odin3_design_declared_model) in declaration order, once each, then
 * the other instantiated BLACKBOX/HARD types in type ID order. A stub declares one `parameter` per
 * parameter of the type (its default); a port whose width is a parameter is `[P-1:0]`, a port
 * whose width is a function gets the width for the default parameters.
 *
 * Names. Identifiers are written as they are when they are simple Verilog identifiers and not
 * keywords, else escaped (`\name `); a name holding a blank, a control byte or a non-ASCII byte
 * cannot be written. Each net is written as one canonical reference: the bit of its input/inout
 * port wire; else its primary wire bit; else its first alias wire bit; else the net itself,
 * declared as a scalar `wire` under its own name (or its first bare alias name). A net with none
 * of these whose only driver is a `$_CONST0_`/`$_CONST1_`/`$_CONSTX_`/`$_CONSTZ_` cell is written
 * as the literal `1'b0`/`1'b1`/`1'bx`/`1'bz` and that cell is omitted. Every live non-port wire
 * is declared (`[msb:lsb]` unless it is 1 bit with range [0:0]); each wire bit (outputs and
 * non-port wires) whose net is referenced elsewhere gets `assign w[i] = <reference>;` (runs of
 * bits are grouped). One namespace per module: port names first, then wire names, net names and
 * cell names in ID order keep their own name when it is writable and free; the rest get
 * generated names `$w<ID>` (wires), `$n<ID>` (nets), `$c<ID>` (cells), `$p<ID>` (a dangling
 * output pin), `$gclk` (global clock), with `$1`, `$2`, ... appended while taken.
 *
 * Vectors: a port's nets are written MSB first as a concatenation `{…}`, consecutive bits of one
 * wire as a part-select (`w[3:1]`, or `w` for a whole unsigned wire), consecutive literals as one
 * `N'b…`. An unconnected input bit is `1'bx`; an unconnected output bit of a partly connected
 * port is a dangling wire `$p<pin ID>`; a wholly unconnected instance port is `.P()`.
 *
 * Cells. `$sop`: `assign y = <products>;` over the ON-set rows (output 1), or `~(<products>)`
 * over the OFF-set rows (output 0); a row with only `-` is `1'b1`, no rows is `1'b0`; a cover
 * mixing output values is refused. Gates `$_BUF_` … `$_MUX_` and word cells `$add` … `$tribuf`
 * are `assign`s with Verilog operators (the operands `$signed(…)` when A_SIGNED/B_SIGNED is
 * set, matching Yosys simlib semantics; `$pmux` as a priority chain). Storage cells write
 * `always` blocks on a `reg`: the Q net itself when it is declared as its own scalar net (bit
 * cells), else a reg named after the cell with `assign Q = <reg>;`. `$_DFF_P_`/`$_DFF_N_`:
 * `always @(posedge|negedge C) q <= D;`; `$_DLATCH_P_`/`$_DLATCH_N_`: `always @* if (E|!E) q <=
 * D;`; `$_FF_`: clocked by `(* gclk *) wire $gclk` (Yosys global clock); INIT 0/1 add `initial
 * q = 1'b0|1'b1;`, INIT 2/3 add nothing (undefined start value). `$dff`/`$dffe`/`$adff`/`$sdff`
 * likewise, with enable, async or sync reset. A built-in cell whose outputs are all unconnected is
 * omitted. Any other cell (module, hard, black-box, `$mem*`, plugin types) is a module instance
 * with named port connections in port order and, unless it instantiates a module, every parameter
 * as `#(.P(value))` (INT decimal, BITS `N'b…` MSB first, STRING quoted, COVER rows quoted).
 *
 * Provenance: a declaration or statement whose object has a provenance location ends with
 * `// file:line` (the record's first location; for a DERIVED record, the first leaf of the
 * backward walk).
 *
 * Errors (logged): ODIN3_ERR_INVALID_ARG for a NULL argument, a port or stub name that cannot be
 * written, a net on two input/inout port bits, a `$sop` cover mixing output values;
 * ODIN3_ERR_IO when path cannot be opened or a write fails ("path: cannot open …", "path:line:
 * write failed …"); ODIN3_ERR_NO_MEMORY on out of memory. On any failure after path was opened,
 * the partly written file is removed when it is a regular file.
 */
odin3_status odin3_verilog_write(const odin3_design *design, const char *path);

/* --- identifiers (escape.c) ---------------------------------------------------------------- */

/* How a name is written as a Verilog identifier. */
typedef enum odin3_verilog_ident {
    ODIN3_VERILOG_SIMPLE,     /* [A-Za-z_][A-Za-z0-9_$]* and not a keyword: as is */
    ODIN3_VERILOG_ESCAPED,    /* other printable ASCII without blanks: `\name ` */
    ODIN3_VERILOG_UNWRITABLE, /* empty, or holds a blank, control or non-ASCII byte */
} odin3_verilog_ident;

/* The form of name (Verilog-2005 and SystemVerilog-2017 keywords count as keywords). */
odin3_verilog_ident odin3_verilog_ident_kind(odin3_bytes name);

/* True when name is a Verilog-2005 or SystemVerilog-2017 reserved word. */
bool odin3_verilog_is_keyword(odin3_bytes name);

/*
 * Appends name to buf as a Verilog identifier: as is when simple, else `\` + name + ` ` (the
 * blank ends an escaped identifier). ODIN3_ERR_INVALID_ARG for an unwritable name, NO_MEMORY on
 * out of memory; buf is unchanged on failure.
 */
odin3_status odin3_verilog_append_ident(odin3_strbuf *buf, odin3_bytes name);

#endif
