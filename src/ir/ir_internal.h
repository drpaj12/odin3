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
#include "ir/prov.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "util/arena.h"
#include "util/idindex.h"
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
    const odin3_techlib_cell *lib; /* tech-library data (local types only), or NULL */
} odin3_celltype_entry;

/* One entry of a design's declared-model list (IR-7b). */
typedef struct odin3_declared_entry {
    odin3_celltype_id type;
    const odin3_value *params;      /* one per parameter of type (design arena), NULL when none */
    const odin3_celltype_def *decl; /* the declaration as written (design arena) */
} odin3_declared_entry;

/* The design's provenance store (prov.c; IR-12, IR-13, IR-6 tombstones). */
typedef struct odin3_prov_store {
    odin3_pagevec *records; /* odin3_prov_record; slot 0 reserved */
    odin3_arena *arena;     /* locs and parents arrays of the records */
    odin3_idindex *index;   /* hash-consing: record identity -> ID */
    odin3_vec runs;         /* uint32_t pass name strtab ID per run; slot 0 reserved */
    odin3_vec tombstones;   /* odin3_tombstone; slot 0 reserved */
    odin3_vec marks;        /* uint32_t per record: walk generation that visited it */
    uint32_t gen;           /* current walk generation */
    odin3_vec stack;        /* uint32_t walk worklist */
    odin3_vec scratch;      /* odin3_prov_id: derive's de-duplicated parents */
} odin3_prov_store;

/* Creates the design's provenance store (design.c calls it); ODIN3_ERR_NO_MEMORY on OOM. */
odin3_status odin3_prov_store_init(odin3_design *design);

/* Frees the provenance store (design.c calls it; safe on a partly initialised design). */
void odin3_prov_store_free(odin3_design *design);

/*
 * Points tomb's params and pin_nets at deep copies in the design's provenance arena (NULL when
 * the count is 0). NO_MEMORY on out of memory, with tomb unchanged (arena bytes of a failure stay
 * unused). Does not validate.
 */
odin3_status odin3_tombstone_own(odin3_design *design, odin3_tombstone *tomb);

/*
 * odin3_tombstone_add for a tombstone whose arrays the design already owns (odin3_tombstone_own):
 * validates the same way and appends without copying. Cannot fail on memory once room for it was
 * reserved in the tombstone table (compact reserves room for all of its tombstones).
 */
odin3_status odin3_tombstone_append(odin3_design *design, const odin3_tombstone *tomb);

struct odin3_design {
    odin3_prov_store *prov;       /* provenance records, pass runs, tombstones */
    odin3_arena *arena;           /* local cell-type definitions */
    odin3_strtab *strtab;         /* design-global names and string values */
    odin3_vec celltypes;          /* odin3_celltype_entry; slot 0 reserved */
    odin3_u64map *celltype_names; /* name strtab ID -> celltype ID */
    odin3_vec declared;           /* odin3_declared_entry, IR-7b declaration order */
    odin3_vec modules;            /* odin3_module *, creation order; slot 0 NULL */
    struct odin3_srcman *srcman;  /* owned; NULL until odin3_design_get_srcman (AST-1) */
};

/* --- module stores (IR-18) ----------------------------------------------------------------- */

/*
 * Each module keeps its nodes, pins and nets in pagevecs of 1 << ODIN3_MODULE_PAGE_SHIFT records,
 * so a small module costs tens of KiB. IDs are indices; index 0 is the reserved dummy.
 */
enum { ODIN3_MODULE_PAGE_SHIFT = 8 };

/* Wires are fewer than nets; smaller pages keep an empty module small. */
enum { ODIN3_WIRE_PAGE_SHIFT = 6 };

/* Chunk size of a module's arena: small chunks keep an empty module cheap. */
enum { ODIN3_MODULE_ARENA_CHUNK_BYTES = 2048 };

/*
 * Makes the next `count` pushes onto a module store unable to fail (reserve before mutate).
 * ODIN3_ERR_NO_MEMORY on out of memory or when the IDs would pass UINT32_MAX; length unchanged.
 */
odin3_status odin3_module_reserve(odin3_pagevec *store, uint32_t count);

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
    odin3_wire_id wire;    /* primary (wire, bit); none when it has none */
    uint32_t wire_bit;
    uint32_t alias_head; /* newest alias record (tail of a circular chain); 0: none */
    uint8_t cls;
    bool dead;
} odin3_net_rec;

typedef struct odin3_wire_rec {
    odin3_net_id *nets; /* width entries in the module arena, LSB first */
    uint32_t name;
    odin3_prov_id prov;
    int32_t msb;
    int32_t lsb;
    uint32_t width;
    odin3_node_id port_node; /* none unless the wire is a module port */
    bool is_signed;
    bool dead;
} odin3_wire_rec;

/* One alias of a net (IR-2): a (wire, bit) membership, or a bare name when wire is none. */
typedef struct odin3_alias_rec {
    odin3_net_id net;
    odin3_wire_id wire;
    uint32_t bit;
    uint32_t name;
    uint32_t next; /* next (older-to-newer) alias of the same net; the tail's next is the head */
} odin3_alias_rec;

/* A module port in declaration order: its node and its wire. */
typedef struct odin3_port_rec {
    odin3_node_id node;
    odin3_wire_id wire;
} odin3_port_rec;

/* The attr_heads key of an object: kind in the high 32 bits, ID in the low. */
uint64_t odin3_attr_key(odin3_objref obj);

/* One attribute of an object (IR-10); the value lives in the module arena. */
typedef struct odin3_attr_rec {
    uint32_t key;
    uint32_t next; /* next attribute of the same object; 0 ends the chain */
    const odin3_value *value;
} odin3_attr_rec;

struct odin3_module {
    odin3_design *design;
    odin3_module_id id;
    uint32_t name;
    odin3_prov_id prov;
    odin3_celltype_id type;      /* the module's cell type (IR-7) */
    odin3_arena *arena;          /* parameter vectors and other small arrays */
    odin3_pagevec *nodes;        /* odin3_node_rec */
    odin3_pagevec *pins;         /* odin3_pin_rec */
    odin3_pagevec *nets;         /* odin3_net_rec */
    odin3_u64map *node_names;    /* name strtab ID -> node ID (live nodes only) */
    odin3_u64map *net_names;     /* name and alias-name strtab ID -> net ID (live nets only) */
    odin3_pinpool pinpool;       /* net pin arrays */
    odin3_pagevec *wires;        /* odin3_wire_rec */
    odin3_u64map *wire_names;    /* name strtab ID -> wire ID (live wires only) */
    odin3_vec aliases;           /* odin3_alias_rec; slot 0 reserved once the table is used */
    odin3_vec ports;             /* odin3_port_rec, declaration order */
    odin3_vec port_defs;         /* odin3_port_def per port: type_def.ports points here */
    odin3_celltype_def type_def; /* the module cell type's definition, updated in place (IR-7) */
    odin3_u64map *attr_heads;    /* (kind << 32 | ID) -> first record in attrs */
    odin3_vec attrs;             /* odin3_attr_rec; slot 0 reserved once the table is used */
    /*
     * Pin net names of the nodes deleted since the last compact, for their tombstones (IR-6,
     * PHASE1 #14): dead node ID -> index of its first name in dead_pin_names, which holds one
     * strtab ID per pin in pin order (0: unconnected or unnamed). dead_pins is NULL until the
     * first delete; compact drops both.
     */
    odin3_u64map *dead_pins;
    odin3_vec dead_pin_names; /* uint32_t */
};

/*
 * Records node's pin net names in the module's dead-pin table (before its pins are disconnected
 * or handed over). NO_MEMORY on out of memory, with nothing recorded; nothing to record for a
 * node without pins.
 */
odin3_status odin3_node_record_pin_nets(odin3_module *module, odin3_node_id node);

/* Drops the dead-pin table (compact, after writing the tombstones; module destruction). */
void odin3_module_dead_pins_free(odin3_module *module);

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
odin3_wire_rec *odin3_wire_rec_at(odin3_module *module, odin3_wire_id wire);
const odin3_wire_rec *odin3_wire_rec_cat(const odin3_module *module, odin3_wire_id wire);
odin3_wire_rec *odin3_wire_live_rec(odin3_module *module, odin3_wire_id wire, const char *what);

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

/* odin3_node_create without the port-type refusal (module_add_port makes port nodes). */
odin3_status odin3_node_create_any(odin3_module *module, const odin3_node_spec *spec,
                                   odin3_node_id *out);

/* Removes a pin from its net's array (swap-remove within the partition); never fails. */
void odin3_net_detach(odin3_module *module, odin3_pin_rec *pin);

/*
 * Puts pin `to` in the place of pin `from` on from's net (same slot; the two pins must have the
 * same direction, so the partition holds); from ends unconnected, to must be unconnected.
 */
void odin3_net_handover(odin3_module *module, odin3_pin_rec *from, odin3_pin_id to);

/* --- wires and aliases (wire.c) ------------------------------------------------------------ */

/* A wire about to be created: odin3_wire_prepare fills it, odin3_wire_commit uses it. */
typedef struct odin3_wire_plan {
    const odin3_wire_spec *spec;
    const odin3_net_id *given; /* NULL: create new nets */
    odin3_net_id *nets;        /* width entries in the module arena */
    uint32_t width;
    odin3_wire_id id; /* the ID the wire will get */
} odin3_wire_plan;

/*
 * Validates the spec and the given nets (INVALID_ARG, logged, for `what`) and reserves
 * everything the commit needs except the name-map entry; nothing observable changes.
 */
odin3_status odin3_wire_prepare(odin3_module *module, odin3_wire_plan *plan, const char *what);

/* Creates the planned wire (and its nets); never fails. The caller has put the name in the map. */
void odin3_wire_commit(odin3_module *module, const odin3_wire_plan *plan);

/* Makes the next `count` alias records unable to fail; ODIN3_ERR_NO_MEMORY otherwise. */
odin3_status odin3_alias_reserve(odin3_module *module, uint32_t count);

/*
 * Merge's alias step (IR-15): every wire entry and name that pointed at drop now points at keep,
 * drop's aliases join keep's, and drop's name and primary become aliases of keep. Needs two
 * reserved alias records; never fails.
 */
void odin3_alias_absorb(odin3_module *module, odin3_net_id keep, odin3_net_id drop);

/*
 * Points local cell type id at def without copying (module types, IR-7): the caller owns def,
 * keeps it valid by the definition rules and alive as long as the design, and may grow its port
 * array in place. def must keep the type's name.
 */
void odin3_celltype_bind_local(odin3_design *design, odin3_celltype_id id,
                               const odin3_celltype_def *def);

/* Fills a fresh design's cell-type table with every global definition (design.c calls it). */
odin3_status odin3_celltype_table_init(odin3_design *design);

/* Frees the table's containers (design.c calls it; safe on a partly initialised design). */
void odin3_celltype_table_free(odin3_design *design);

/* Instance counting (node creation and deletion). Require a valid ID; dec requires a count > 0. */
void odin3_celltype_instances_inc(odin3_design *design, odin3_celltype_id id);
void odin3_celltype_instances_dec(odin3_design *design, odin3_celltype_id id);

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
