/*
 * flatten_int.h — the expansion state of `flatten` shared by expand.c, copy.c, names.c and prov.c
 * (3A spec §3, §4, §6; plan Task 3, points (b)-(e)). Internal.
 */
#ifndef ODIN3_PASSES_FLATTEN_FLATTEN_INT_H
#define ODIN3_PASSES_FLATTEN_FLATTEN_INT_H

#include "passes/flatten/flatten.h"

#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { FLAT_N_CONST = 4, FLAT_BUDGET_EVERY = 1 << 16, FLAT_KEEP_LOG_CAP = 20 };

/* One instance node of the root waiting for expansion: the string ID of its parent's path, the
 * instance's own IR name in the module that held it (0 = unnamed) and its original record. */
typedef struct flat_work {
    uint32_t node;
    uint32_t parent_path;
    uint32_t orig_name;
    odin3_prov_id prov;
} flat_work;

/* A net of the child (mapped to the root by odin3_flatten_mapped) that a port bit joined, for the
 * checks of spec §3.7. */
typedef struct flat_joined {
    uint32_t net;
    uint32_t port;
} flat_joined;

typedef enum flat_kind { FLAT_NODE, FLAT_NET, FLAT_WIRE } flat_kind;

/* A name to make in the root: the object kind and the original name (strtab ID). */
typedef struct flat_key {
    flat_kind kind;
    uint32_t str;
} flat_key;

/* The label of an instance for its children's prefix: its flat name (0: unnamed) and node ID. */
typedef struct flat_label {
    uint32_t name;
    uint32_t node;
} flat_label;

typedef struct odin3_flatten_ctx {
    odin3_pass_ctx *pass;
    odin3_design *design;
    const odin3_flatten_graph *graph;
    const char *sep;
    odin3_module *root;
    uint32_t root_name;
    odin3_celltype_id const_type[FLAT_N_CONST];
    odin3_net_id const_net[FLAT_N_CONST];
    odin3_vec work; /* flat_work, FIFO with head */
    size_t head;
    odin3_u64map *redirect; /* dead root net -> kept root net */
    odin3_u64map *memids;   /* strtab IDs of the root's MEMIDs */
    odin3_u64map *memmap;   /* this instance: the child's MEMID -> its new MEMID (one per memory) */
    /* the instance being expanded */
    const odin3_module *child;
    uint32_t child_id;
    odin3_vec pnets;  /* odin3_net_id per instance pin */
    odin3_vec map;    /* odin3_net_id per child net ID */
    odin3_vec joined; /* uint8_t per child net ID: 1 when a port bit or a constant joined it */
    odin3_vec bits;   /* flat_joined */
    odin3_strbuf prefix;
    odin3_strbuf scratch;
    odin3_vec tmp_nets;  /* odin3_net_id */
    odin3_vec tmp_ports; /* odin3_netvec */
    odin3_vec tmp_vals;  /* odin3_value */
    uint32_t path_str;
    odin3_prov_id inst_prov;
    uint32_t cache_from, cache_to; /* the last original hier string and its derived one */
    /* counters */
    uint64_t instances, kept, nodes, renamed, created;
} odin3_flatten_ctx;

/* names.c */
/* prefix + name (the prefix already ends with the separator), made unique in the root for the
 * kind (`_1`, `_2`, ...). orig 0 gives 0 (unnamed objects stay unnamed). */
odin3_status odin3_flatten_name(odin3_flatten_ctx *fc, flat_key key, uint32_t *out);
/* The MEMID rewritten the same way against the root's MEMID set (which it extends). Once per
 * (instance, original MEMID): $mem, $memrd and $memwr cells sharing a MEMID keep sharing it. */
odin3_status odin3_flatten_memid(odin3_flatten_ctx *fc, uint32_t orig, uint32_t *out);
/* "<text><sep>" as the prefix of the children of an instance named str (0: "$c<node>"). */
odin3_status odin3_flatten_set_prefix(odin3_flatten_ctx *fc, flat_label label);
/* Interns the NUL-terminated text of fc->scratch. */
odin3_status odin3_flatten_intern_scratch(odin3_flatten_ctx *fc, uint32_t *out);

/* prov.c */
/* fc->path_str for the work item: parent path + "/" + the instance's own name (or $c<node>). */
odin3_status odin3_flatten_path(odin3_flatten_ctx *fc, const flat_work *item);
/* The record of a copied object: DERIVED with parents [orig, instance] and hier = the path plus
 * orig's hier minus its first component. Both zero gives 0. */
odin3_status odin3_flatten_derive(odin3_flatten_ctx *fc, odin3_prov_id orig, odin3_prov_id *out);
/* An error located at the instance's source (or unlocated), logged; always ERR_INVALID_ARG. */
odin3_status odin3_flatten_error(const odin3_flatten_ctx *fc, const char *fmt, ...)
    ODIN3_PRINTF(2, 3);

/* expand.c */
/* Sets up fc for a run (all scratch empty); odin3_flatten_ctx_free releases it, also after a
 * failed init. */
odin3_status odin3_flatten_ctx_init(odin3_flatten_ctx *fc, odin3_pass_ctx *pass,
                                    const odin3_flatten_graph *graph, const char *sep);
void odin3_flatten_ctx_free(odin3_flatten_ctx *fc);
odin3_status odin3_flatten_expand_root(odin3_flatten_ctx *fc, odin3_module *root);
/* The root net a net ID stands for after the merges so far. */
odin3_net_id odin3_flatten_resolve(const odin3_flatten_ctx *fc, odin3_net_id net);
/* The mapped, resolved root net of child net `net` (none when unmapped). */
odin3_net_id odin3_flatten_mapped(const odin3_flatten_ctx *fc, odin3_net_id net);

/* copy.c */
odin3_status odin3_flatten_copy_nodes(odin3_flatten_ctx *fc);
odin3_status odin3_flatten_copy_wires(odin3_flatten_ctx *fc);
odin3_status odin3_flatten_check_joins(odin3_flatten_ctx *fc);

#endif
