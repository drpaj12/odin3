/*
 * attrs.h — attribute keys the BLIF reader sets and the BLIF writer reads (IR-10).
 */
#ifndef ODIN3_FRONTENDS_BLIF_ATTRS_H
#define ODIN3_FRONTENDS_BLIF_ATTRS_H

/*
 * Attributes the reader sets (IR-10), all of kind STRING:
 * - ODIN3_BLIF_ATTR_CLOCK on a module: the names of its `.clock` directives, in file order,
 *   joined by single spaces;
 * - ODIN3_BLIF_ATTR_PORT_NAME on a port wire whose BLIF name could not be its IR name because a
 *   port of the module already has that name (a net that is both `.inputs x` and `.outputs x`):
 *   the wire gets the first free name `<x>$blif_port`, `<x>$blif_port2`, … and this attribute
 *   holds `x` (the base name for a vector port). Writers print the attribute instead of the wire
 *   name;
 * - on a cell, for each Yosys `.attr key value` line after it: attribute
 *   ODIN3_BLIF_ATTR_PREFIX "key" (`blif.attr:key`), and for each `.param key value` line:
 *   ODIN3_BLIF_PARAM_PREFIX "key" (`blif.param:key`). `.param` lines are stored as these STRING
 *   attributes, never as IR parameters of the node. The value is the line's tokens after the key
 *   joined by single spaces, quotes kept: the lexer splits on blanks, so a run of blanks (spaces,
 *   tabs) inside a value, even inside quotes, becomes one space. A key repeated on the same cell
 *   keeps its last value;
 * - ODIN3_BLIF_ATTR_EXTRAS on a cell that has any of those: the full attribute keys
 *   (`blif.attr:src blif.param:INIT`) in the order of their first `.attr`/`.param` line, joined
 *   by single spaces (BLIF keys hold no blanks). The IR cannot list a node's attributes, so writers
 *   walk this list and print `.attr key value` or `.param key value` by the key's prefix.
 * A `.cname name` line names the previous cell (odin3_node_name).
 */
#define ODIN3_BLIF_ATTR_CLOCK "blif_clock"
#define ODIN3_BLIF_ATTR_PORT_NAME "blif_name"
#define ODIN3_BLIF_ATTR_PREFIX "blif.attr:"
#define ODIN3_BLIF_PARAM_PREFIX "blif.param:"
#define ODIN3_BLIF_ATTR_EXTRAS "blif_extras"

#endif
