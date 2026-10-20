/*
 * reader_locs.h — the BLIF reader's `--hdl-locs` half (spec docs/specs/2026-10-20-5.4-hdl-
 * locations-design.md D-4, D-5, D-7); internal to the reader.
 *
 * With the option the reader (a) decodes provenance net names (L-5 after VB-10 decoding) into SHORT
 * with `hier` and an HDL record on the net, (b) restores `.attr src "SRC"` on the previous cell and
 * drops the attribute, (c) gives a cell without an HDL location the first one of its output nets
 * at the end of the model (D-5, counted as `inherited`). Every record is made with odin3_hdl_restore
 * (a DERIVED whose parents are the SOURCE, then the IMPORTED record) in one operation per read.
 */
#ifndef ODIN3_FRONTENDS_BLIF_READER_LOCS_H
#define ODIN3_FRONTENDS_BLIF_READER_LOCS_H

#include "frontends/blif/reader.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "odin3/odin3.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct blif_locs blif_locs;

/* NULL on out of memory. ctx is the reader's pass context (it must outlive the result); stats is
 * zeroed here; path names the file in warnings. */
blif_locs *blif_locs_create(odin3_pass_ctx *ctx, odin3_blif_read_stats *stats, const char *path);
void blif_locs_destroy(blif_locs *locs);

/*
 * The net a body reference to the raw name `str` means in module (mod_key identifies the module
 * in the maps): a decoded net by its original text, else the module's net of that name, except
 * that a decoded net which took that exact name gives it up (renamed to the first free `name$N`),
 * since a raw name is exact. *out is none for no net.
 */
odin3_status blif_locs_find_net(blif_locs *locs, odin3_module *module, uint32_t mod_key,
                                uint32_t str, odin3_net_id *out);

/* Creates the net for the raw name (str is its strtab ID), decoded when it is a provenance name;
 * imported is the net's own record. */
odin3_status blif_locs_create_net(blif_locs *locs, odin3_module *module, uint32_t mod_key,
                                  odin3_bytes name, odin3_prov_id imported, odin3_net_id *out);

/*
 * `.attr src` of node: text is the value as stored (quotes kept). A text that parses (L-2) makes
 * *restored true and the node is rebuilt with the new record (*node is the fresh one: same nets,
 * name and attributes); a text that does not parse gives a WARNING at path:line, *restored false
 * and the node stays (L-6).
 */
odin3_status blif_locs_cell_src(blif_locs *locs, odin3_module *module, odin3_node_id *node,
                                odin3_bytes text, uint32_t line, bool *restored);

/* D-5 for every cell of module without an HDL location of its own. */
odin3_status blif_locs_inherit(blif_locs *locs, odin3_module *module);

/* Copies every attribute of node from to node to. */
odin3_status blif_copy_node_attrs(odin3_module *module, odin3_node_id from, odin3_node_id to);

#endif
