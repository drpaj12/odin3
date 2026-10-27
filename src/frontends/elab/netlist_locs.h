/*
 * netlist_locs.h — read_verilog -netlist (spec docs/specs/2026-10-20-5.4-hdl-locations-design.md
 * D-3, L-5, L-6, D-7): the HDL locations a netlist written by write_verilog (or Yosys) carries.
 * Internal to the Verilog reader (stages 1 and 2).
 *
 * Three sources, all only under `-netlist`: (a) a `(* src = "SRC" *)` attribute on a module, port,
 * wire, instance or `always`; (b) a trailing `// LOC` comment on the line where a continuous
 * assignment ends, found by stage 1 through the comment side table (binary search by position) and
 * handed to stage 2 as an internal attribute of the CONT_ASSIGN; (c) an identifier `\hier/short@
 * file:line `, split into SHORT and `hier`. Each restored object gets odin3_hdl_restore's record
 * (DERIVED, parents SOURCE then the IMPORTED record the reader made), in the one operation the
 * read begins.
 */
#ifndef ODIN3_FRONTENDS_ELAB_NETLIST_LOCS_H
#define ODIN3_FRONTENDS_ELAB_NETLIST_LOCS_H

#include "ast/ast.h"
#include "backends/common/hdl_loc.h"
#include "frontends/elab/elab_internal.h"
#include "ir/design.h"
#include "odin3/odin3.h"

#include <stdbool.h>
#include <stdint.h>

/* The name of the internal attribute stage 1 puts on a CONT_ASSIGN with a location comment. */
#define ODIN3_NETLIST_ASSIGN_KEY "odin3_netlist_loc"

/* --- stage 1 -----------------------------------------------------------------------------------
 */

typedef struct odin3_netlist_assign_src {
    odin3_ast_id item; /* the parsed CONT_ASSIGN */
    odin3_ast_id next; /* the item after it in the same body, 0 for none */
} odin3_netlist_assign_src;

/*
 * The text of the `// LOC` comment on the line where the parsed CONT_ASSIGN item ends: the first
 * line comment that starts at or after the item's end (a binary search of the file's comment
 * table) and on that line, whose text after `//` and blanks is exactly one LOC (L-1). *text views
 * the comment (valid while the parsed store lives). False for none, a block comment, a text that
 * is not one LOC, an item whose end is unknown or inside a macro expansion, or an item that is not
 * the last on its line before the comment (next, the item after it in the same body or 0, starts
 * between them).
 */
bool odin3_netlist_assign_comment(odin3_design *design, const odin3_ast *parsed,
                                  const odin3_netlist_assign_src *src, odin3_bytes *text);

/* --- stage 2 -----------------------------------------------------------------------------------
 */

/* Begins the read's one operation (D-1) when -netlist is on and none is begun. */
void odin3_netlist_begin(odin3_elab2 *e2);

/* A decoded L-5 name (interned in the design). */
typedef struct odin3_netlist_name {
    bool matched;
    uint32_t short_str; /* strtab ID of SHORT */
    uint32_t hier_str;  /* strtab ID of HIER, 0 for none */
    odin3_hdl_loc loc;  /* file and line; the file views e2->scratch */
    uint32_t rec; /* version 2 map: the record the map binds to the name (O entry or N line) */
} odin3_netlist_name;

/*
 * Decodes the identifier raw_str (strtab ID), declared at `at`, as L-5. With -name-map (H-6) a hash
 * name of the current module that the map holds is first replaced by its full name; one that it
 * lacks is counted for the end-of-run warning. *out->matched is false for a name that does not
 * match (kept as it is). ODIN3_ERR_NO_MEMORY.
 */
odin3_status odin3_netlist_name_decode(odin3_elab2 *e2, uint32_t raw_str, odin3_loc at,
                                       odin3_netlist_name *out);

/*
 * Version 2 map (V1.2.0 4.5): true when -name-map is of version 2, so that odin3_netlist_prov binds
 * a declarator to the record its O or N line names (odin3_netlist_name.rec, 0 when the map does not
 * list it) and makes no restored-location record. Only module-level declarators (wires and nets)
 * are named by the elaborator; the other objects keep the reader's own record.
 */
bool odin3_netlist_restored(const odin3_elab2 *e2);

/* Notes that a declarator of the current module is spelled str: a decoded name never takes it. */
odin3_status odin3_netlist_note_raw(odin3_elab2 *e2, uint32_t str);

/*
 * The name a decoded wire takes in the current module: SHORT, or SHORT$N with the first free N when
 * the module already has a wire of that name or a declarator spelled so (IR-14: names are unique
 * per kind; the raw name always wins, as in the BLIF reader).
 */
odin3_status odin3_netlist_unique_wire(odin3_elab2 *e2, uint32_t short_str, uint32_t *name);

/*
 * True when attr (an ATTR of the elaborated store) is a `src` attribute whose string value is a
 * restorable location (L-2): the attribute is then consumed (not stored on the object). A `src`
 * that is not (a string that does not parse) is counted for the end-of-run warning and kept (L-6).
 */
bool odin3_netlist_src_consumed(odin3_elab2 *e2, odin3_ast_id attr);

/*
 * The record for an object whose IMPORTED record is base: the restored one (odin3_hdl_restore)
 * when the CONT_ASSIGN being emitted has a comment, or req->node's then nl->attrs's `src`
 * attribute parses, or nl->name matched; else base. ODIN3_ERR_NO_MEMORY.
 */
odin3_status odin3_netlist_prov(odin3_elab2 *e2, const odin3_elab2_prov_req *req,
                                const odin3_elab2_nl_extra *nl, odin3_prov_id base,
                                odin3_prov_id *out);

/* The CONT_ASSIGN node's comment location for the assignments emitted until _end (asg_loc). */
void odin3_netlist_assign_begin(odin3_elab2 *e2, odin3_ast_id cont_assign);
void odin3_netlist_assign_end(odin3_elab2 *e2);

/* The one WARNING of L-6: how many `src` attributes were not locations and the first. */
void odin3_netlist_finish(odin3_elab2 *e2);

#endif
