/*
 * ir_internal.h — private IR structs and helpers shared by the src/ir sources (never included
 * outside src/ir and tests/unit).
 */
#ifndef ODIN3_IR_INTERNAL_H
#define ODIN3_IR_INTERNAL_H

#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/pinpool.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "util/arena.h"
#include "util/pagevec.h"
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
    odin3_vec modules;            /* odin3_module *, creation order; slot 0 NULL */
};

/* --- module stores (IR-18) ----------------------------------------------------------------- */

/*
 * An object store: a pagevec plus its logical length. Slots in [len, pagevec length) are zeroed
 * spares that a reservation pushed before its operation failed; the next reservation reuses them.
 * This gives reserve-before-mutate on a pagevec, which has no pop. IDs are slot indices; slot 0 is
 * the reserved dummy.
 */
typedef struct odin3_store {
    odin3_pagevec *pv;
    uint32_t len;
} odin3_store;

/* Creates the pagevec and takes slot 0; ODIN3_ERR_NO_MEMORY on out of memory. */
odin3_status odin3_store_init(odin3_store *store, size_t elem_size);

/* Ensures `count` zeroed slots exist past len; ODIN3_ERR_NO_MEMORY (len unchanged) otherwise. */
odin3_status odin3_store_reserve(odin3_store *store, uint32_t count);

/* Takes the next reserved slot (requires a reservation); returns it, its index is the old len. */
void *odin3_store_take(odin3_store *store);

typedef struct odin3_node_rec {
    odin3_celltype_id type;
    uint32_t name;
    odin3_prov_id prov;
    odin3_pin_id first_pin; /* pins are first_pin .. first_pin + pin_count - 1 */
    uint32_t pin_count;
    uint32_t n_params;
    const odin3_value *params; /* n_params values in the module arena */
    bool dead;
} odin3_node_rec;

typedef struct odin3_pin_rec {
    odin3_node_id node;
    uint32_t port;
    uint32_t bit;
    odin3_net_id net; /* 0: unconnected */
    uint32_t slot;    /* index in the net's pin array while connected */
    uint8_t dir;      /* odin3_dir of the port, fixed at creation */
} odin3_pin_rec;

typedef struct odin3_net_rec {
    odin3_pin_id *pins; /* pinpool block of class cls; NULL when the net has no pins */
    uint32_t name;
    odin3_prov_id prov;
    uint32_t count;        /* pins in the array */
    uint32_t driver_count; /* pins[0 .. driver_count) drive, the rest are sinks */
    odin3_wire_id wire;    /* primary (wire, bit); wires arrive with module ports */
    uint32_t wire_bit;
    uint8_t cls;
    bool dead;
} odin3_net_rec;

struct odin3_module {
    odin3_design *design;
    odin3_module_id id;
    uint32_t name;
    odin3_prov_id prov;
    odin3_celltype_id type;   /* the module's cell type (IR-7) */
    odin3_arena *arena;       /* parameter vectors and other small arrays */
    odin3_store nodes;        /* odin3_node_rec */
    odin3_store pins;         /* odin3_pin_rec */
    odin3_store nets;         /* odin3_net_rec */
    odin3_u64map *node_names; /* name strtab ID -> node ID (live nodes only) */
    odin3_u64map *net_names;  /* name strtab ID -> net ID (live nets only) */
    odin3_pinpool pinpool;    /* net pin arrays */
};

/* Sets up the design's module list (design.c calls it); ODIN3_ERR_NO_MEMORY on out of memory. */
odin3_status odin3_module_table_init(odin3_design *design);

/* Destroys every module and the list (design.c calls it; safe on a partly initialised design). */
void odin3_module_table_free(odin3_design *design);

/* Records by ID: NULL when the ID is 0 or past the store's end (dead records are returned). */
odin3_node_rec *odin3_node_rec_at(odin3_module *module, odin3_node_id node);
const odin3_node_rec *odin3_node_rec_cat(const odin3_module *module, odin3_node_id node);
odin3_pin_rec *odin3_pin_rec_at(odin3_module *module, odin3_pin_id pin);
const odin3_pin_rec *odin3_pin_rec_cat(const odin3_module *module, odin3_pin_id pin);
odin3_net_rec *odin3_net_rec_at(odin3_module *module, odin3_net_id net);
const odin3_net_rec *odin3_net_rec_cat(const odin3_module *module, odin3_net_id net);

/* Live records for an operation named `what`; NULL (and an error logged) for a bad or dead ID. */
odin3_node_rec *odin3_node_live_rec(odin3_module *module, odin3_node_id node, const char *what);
odin3_pin_rec *odin3_pin_live_rec(odin3_module *module, odin3_pin_id pin, const char *what);
odin3_net_rec *odin3_net_live_rec(odin3_module *module, odin3_net_id net, const char *what);

/* A name change in one of the module's name maps (IR-14); 0 means "no name". */
typedef struct odin3_name_change {
    odin3_u64map *map;
    const char *what; /* operation, for the log */
    uint32_t id;      /* the object */
    uint32_t from;    /* its current name */
    uint32_t to;      /* its new name */
} odin3_name_change;

/* True when the change can be made (else an error is logged); never allocates. */
bool odin3_names_available(const odin3_module *module, const odin3_name_change *change);

/*
 * Points `to` at the object and drops `from`. ODIN3_ERR_INVALID_ARG (logged) when `to` is not a
 * strtab ID or names another object; ODIN3_ERR_NO_MEMORY; the map is unchanged on failure.
 */
odin3_status odin3_names_change(const odin3_module *module, const odin3_name_change *change);

/* Removes a pin from its net's array (swap-remove within the partition); never fails. */
void odin3_net_detach(odin3_module *module, odin3_pin_rec *pin);

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
