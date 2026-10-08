/*
 * ir_internal.h — private IR structs and helpers shared by the src/ir sources (never included
 * outside src/ir and tests/unit).
 */
#ifndef ODIN3_IR_INTERNAL_H
#define ODIN3_IR_INTERNAL_H

#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "odin3/odin3.h"
#include "util/arena.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>

/* One row of a design's cell-type table; the row index is the odin3_celltype_id. */
typedef struct odin3_celltype_entry {
    const odin3_celltype_def *def; /* global: the registered definition; local: in design arena */
    uint32_t name;                 /* strtab ID of def->name; never changes */
    uint32_t instances;            /* live nodes of this type */
    bool local;                    /* added by add_local/declare_blackbox; definition replaceable */
} odin3_celltype_entry;

struct odin3_design {
    odin3_arena *arena;           /* local cell-type definitions */
    odin3_strtab *strtab;         /* design-global names and string values */
    odin3_vec celltypes;          /* odin3_celltype_entry; slot 0 reserved */
    odin3_u64map *celltype_names; /* name strtab ID -> celltype ID */
    odin3_vec declared;           /* odin3_celltype_id, IR-7b declaration order */
};

/* Fills a fresh design's cell-type table with every global definition (design.c calls it). */
odin3_status odin3_celltype_table_init(odin3_design *design);

/* Frees the table's containers (design.c calls it; safe on a partly initialised design). */
void odin3_celltype_table_free(odin3_design *design);

/* Instance counting (node creation and deletion). Require a valid ID; dec requires a count > 0. */
void odin3_celltype_instances_inc(odin3_design *design, odin3_celltype_id id);
void odin3_celltype_instances_dec(odin3_design *design, odin3_celltype_id id);

/*
 * Replaces the definition of local type id with a deep copy of def (module ports, IR-7). def must
 * keep the same name. ODIN3_ERR_INVALID_ARG (logged) when id is not a local type, def is invalid or
 * renamed; ODIN3_ERR_NO_MEMORY on out of memory; the old definition stays on failure. The policy
 * "refuse once instantiated" belongs to the caller (odin3_celltype_instances).
 */
odin3_status odin3_celltype_replace_local(odin3_design *design, odin3_celltype_id id,
                                          const odin3_celltype_def *def);

/* Built-in definitions (src/ir/cells/), listed in cells/builtin.c. */
extern const odin3_celltype_def *const odin3_builtin_celltypes[];
extern const uint32_t odin3_builtin_celltype_count;

extern const odin3_celltype_def odin3_cell_port_in;
extern const odin3_celltype_def odin3_cell_port_out;
extern const odin3_celltype_def odin3_cell_port_inout;
extern const odin3_celltype_def odin3_cell_const0;
extern const odin3_celltype_def odin3_cell_const1;
extern const odin3_celltype_def odin3_cell_constx;
extern const odin3_celltype_def odin3_cell_constz;

#endif
