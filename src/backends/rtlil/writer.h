/*
 * writer.h — Yosys RTLIL text writer (2D spec §13, EL-48): word-level IR with processes, the
 * bridge that lets Yosys 0.55 read 2D's output (read_rtlil) for the 2D–2F comparisons.
 */
#ifndef ODIN3_BACKENDS_RTLIL_WRITER_H
#define ODIN3_BACKENDS_RTLIL_WRITER_H

#include "ir/design.h"
#include "odin3/odin3.h"
#include "util/str.h"

/*
 * Appends design as RTLIL text to out (the format Yosys 0.55's read_rtlil reads; field order as
 * its rtlil_backend.cc). Nothing in the design changes; the text depends only on the IR (IR-16
 * order), so two calls give the same bytes. Processes are written (no refusal).
 *
 * Per module, in creation order, at no indent: the module's attributes (`attribute \key value`),
 * then `attribute \top 1` when it is the design's top (odin3_design_top) and has no `top`
 * attribute of its own (as Yosys's write_rtlil; read_rtlil takes it as the top), `module \name`,
 * then at indent 2:
 * - each live named wire in ID order, after its attributes: `wire [width W] [upto] [offset O]
 *   [input|output|inout K] [signed] \name` (W when not 1; upto for an ascending range [msb:lsb]
 *   with msb < lsb; O = the lower declared index when not 0; K = the 1-based port index; signed
 *   for a signed wire). Unnamed wires are not written: their nets are spelled as nets.
 * - each live net that odin3_proc_sigspec spells as a net of its own (neither a constant nor a
 *   bit of a named wire; 2D plan point (s), review R4), in ID order, after its attributes:
 *   `wire \name` for a named net, else `wire $n<ID>`;
 * - for each run of unconnected pins of a written cell (consecutive bits of one port), a dangling
 *   wire `wire [width N] $p<pin ID>` named after the run's lowest pin;
 * - each live node in ID order except port nodes, constant nodes (a type whose const_value hook
 *   gives a constant for the node's parameters: $_CONST0_/1/X/Z_, a constant $sop) and $proc
 *   nodes, after its attributes: `cell <type> <name>`, where the type is the cell type's name
 *   (an instance's is its module's name), with `\` prepended unless it starts with `$` or `\`;
 *   the name is `\name`, or `$c<ID>` when the node is unnamed, its name is taken by a written
 *   wire (Yosys keeps cells and wires in one namespace) or RTLIL cannot carry it. Then
 *   `  parameter \P <value>` per parameter in definition order (odin3_proc_value_text: INT
 *   decimal, BITS <n>'<bits> MSB first, STRING quoted with Yosys's escapes; a MEMID string gets
 *   `\` prepended unless it starts with `$` or `\`; on $mem a 0-bit BITS parameter other than
 *   INIT is written 1'0, the max(1, ports) bits Yosys 0.55's cell check wants), `  connect \PORT
 *   <sig>` per port in port order (odin3_proc_sigspec; a run of unconnected pins is its dangling
 *   wire), and `end`;
 * - each live $proc node in ID order as an RTLIL process (odin3_proc_print at indent 2);
 * - for each named wire in ID order, each maximal run of its bits that are not their net's own
 *   spelling (an alias of a net spelled by another wire bit or as a net, or a bit on a constant
 *   net): `connect \w[ [hi:lo]] <sig of the run's nets>`;
 * then `end`.
 *
 * A module's own cell type (an instance) is always written \name, as its `module` line.
 *
 * Limitation (review I1; stubs are 2E-1's): black-box and hard cell types are not IR modules and
 * get no `module` stub, so their instances are cells of a type Yosys does not know. read_rtlil,
 * proc and opt accept that; `hierarchy -check` rejects it, and Yosys counts every connection of
 * such a cell as a user (Cell::known() is false), where the JSON and Verilog writers emit stubs.
 *
 * ODIN3_ERR_INVALID_ARG for a NULL argument (logged "write_rtlil: no design or no output", or
 * "... no path"), and, logged "write_rtlil: module 'm': ...", for:
 * - a name RTLIL cannot carry (empty, or holding a blank, tab, CR, LF or NUL: an RTLIL identifier
 *   ends at whitespace): "<what> '<name>' cannot be written in RTLIL";
 * - a net written as `\name` whose name a wire also has: "net 'n' has the name of a wire";
 * - a cell Yosys 0.55 rejects or asserts on as Odin III defines it (review I2): "cell '<name>' of
 *   type '<t>' has no RTLIL form" for $_DFF_P_, $_DFF_N_, $_DLATCH_P_, $_DLATCH_N_ and $_FF_ (an
 *   INIT parameter Yosys's have not), a non-constant $sop (WIDTH/COVER, not DEPTH/TABLE), $memrd
 *   and $memwr (no `memory` record is written); "cell '<name>' of type '$mem' has an empty MEMID"
 *   (<name> is the node's, else $c<ID>);
 * - a $proc node odin3_proc_print refuses.
 * ODIN3_ERR_NO_MEMORY on out of memory. out is unchanged on failure.
 */
odin3_status odin3_rtlil_text(const odin3_design *design, odin3_strbuf *out);

/*
 * Writes odin3_rtlil_text's text to path atomically (util/file.h): a private temporary beside
 * path, renamed over it on success; on any failure (a refusal above, out of memory, I/O) only the
 * temporary is removed and an existing file at path keeps its contents. Text is streamed a module
 * at a time (and whenever more than 1 MiB is pending), so the whole text is never held. Errors
 * as odin3_rtlil_text, plus ODIN3_ERR_IO (logged "<path>: cannot open ...", "<path>: write
 * failed: ..." or "<path>: cannot rename ...").
 */
odin3_status odin3_rtlil_write(const odin3_design *design, const char *path);

#endif
