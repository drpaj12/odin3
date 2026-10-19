/*
 * lm_kit.h — lpmmap's local kit (5.1 plan Task 4): the context, the lookup of the four LPM types
 * (spec P-2), the transaction (begin, build on fresh nets, commit or abort) with the names and
 * attributes of spec H-6/H-7, canonical constants (H-3), operand extension (H-2) and located
 * diagnostics. It mirrors the shape of hardmap's hm_kit but is not tied to the four VTR types.
 */
#ifndef ODIN3_PASSES_LPMMAP_LM_KIT_H
#define ODIN3_PASSES_LPMMAP_LM_KIT_H

#include "ir/design.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "passes/lpmmap/lpmmap.h"
#include "passes/manager.h"
#include "util/attr.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The LPM cell types lpmmap uses, found by name and never created (P-2). */
typedef enum lm_type { LM_ADD_SUB, LM_MULT, LM_COMPARE, LM_ALTSYNCRAM, LM_N_TYPES } lm_type;

/* The constants of H-3: gnd and vcc. */
typedef enum lm_bit { LM_GND, LM_VCC, LM_N_CONST } lm_bit;

/* One merge of a commit: `drop` is merged into `keep` (odin3_net_merge). */
typedef struct lm_tie {
    odin3_net_id keep;
    odin3_net_id drop;
} lm_tie;

typedef struct lm_ctx {
    odin3_pass_ctx *pass;
    odin3_design *design;
    odin3_module *module;
    const char *family; /* intended_device_family */
    odin3_lpmmap_stats stats;
    odin3_celltype_id types[LM_N_TYPES]; /* found by lm_need; {0} until then */
    odin3_celltype_id konst[LM_N_CONST]; /* $_CONST0_, $_CONST1_ */
    odin3_net_id consts[LM_N_CONST];     /* the canonical nets of the current module */
    odin3_node_id src;                   /* the cell of the transaction */
    odin3_prov_id prov;                  /* its DERIVED record (0 when the source has none) */
    odin3_node_id first;                 /* the first node the transaction made */
    uint32_t src_name;                   /* the source's name (strtab ID, 0: unnamed) */
    uint64_t pins_created;               /* the budget's counter */
    odin3_vec made_nodes;                /* odin3_node_id: undone by lm_abort */
    odin3_vec made_nets;                 /* odin3_net_id: the fresh nets, likewise */
    odin3_vec merged;                    /* odin3_net_pair: the merges of a commit */
    odin3_vec doomed;  /* odin3_node_id: released at the commit, after the source */
    odin3_vec scratch; /* odin3_net_id: the port nets of the node being made */
    odin3_strbuf text; /* scratch for names and attribute values */
} lm_ctx;

/* A parameter of a node to make, by name: an INT value, or a STRING when sval is not NULL. A
 * parameter not listed keeps its library default. */
typedef struct lm_pset {
    const char *name;
    int64_t ival;
    const char *sval;
} lm_pset;

/* The nets of one port of a node to make, by name, LSB first: count entries, which may be fewer
 * than the port's width (the rest of its pins stay open) but not more. A port not listed is open.
 */
typedef struct lm_pconn {
    const char *name;
    const odin3_net_id *nets;
    uint32_t count;
} lm_pconn;

/* The request of lm_new_node. */
typedef struct lm_node_req {
    lm_type type;
    const lm_pset *params;
    size_t n_params;
    const lm_pconn *ports;
    size_t n_ports;
    const char *role;
} lm_node_req;

/* The registered cell type named name, false when the design has none; no allocation, no log. */
bool lm_find_type(const odin3_design *design, const char *name, odin3_celltype_id *out);

/* Zeroes *lc and fills it; allocates nothing. */
void lm_init(lm_ctx *lc, odin3_pass_ctx *pass, odin3_design *design, const char *family);
void lm_free(lm_ctx *lc);

/* Makes module the module lm_new_node, lm_const and the transactions work on. */
void lm_set_module(lm_ctx *lc, odin3_module *module);

/*
 * The cell type `which` in *out, checked once per context: it must be registered and of
 * granularity BLACKBOX. Otherwise the error "lpmmap: cell type 'lpm_add_sub' is not registered;
 * read_techlib lib/lpm.o3lib first" (or "... is not a black box ...") is logged, located at node
 * `at` (none: bare), and ODIN3_ERR_INVALID_ARG returned, nothing changed.
 */
odin3_status lm_need(lm_ctx *lc, lm_type which, odin3_node_id at, odin3_celltype_id *out);

/* The name of the type `which` ("lpm_add_sub", ...). */
const char *lm_type_name(lm_type which);

/*
 * Starts the transaction for source cell `src` (live, in the current module): begins one
 * provenance operation and makes its DERIVED record with the source's record as parent (H-6).
 * NO_MEMORY with nothing made.
 */
odin3_status lm_begin_cell(lm_ctx *lc, odin3_node_id src);

/* n fresh unnamed nets with the transaction's record in out[0 .. n). NO_MEMORY: the ones made are
 * undone by lm_abort. */
odin3_status lm_fresh_nets(lm_ctx *lc, uint32_t n, odin3_net_id *out);

/* Adds node (live, in the current module) to the cells lm_commit releases after the source: it
 * disconnects every pin, then deletes the node if memory allows. The only allocation of that step
 * is here, so a failure leaves the design intact. */
odin3_status lm_doom(lm_ctx *lc, odin3_node_id node);

/*
 * Creates a node of type req->type with the transaction's record, the source's name (the first
 * node: taken at the commit; later ones `<source>.<role>`), the source's attributes (first node)
 * and the `lpmmap` attribute `<type>:<role>`. INVALID_ARG (logged) for a port or parameter the
 * type does not have, or more nets than a port's width; NO_MEMORY; ODIN3_ERR_PARSE when the design
 * budget is exceeded. Nothing is made by a failing call.
 */
odin3_status lm_new_node(lm_ctx *lc, const lm_node_req *req, odin3_node_id *out);

/* The canonical net of constant `which` in the current module: the net of the lowest-ID live
 * constant node of that value, else a new constant node. NO_MEMORY. */
odin3_status lm_const(lm_ctx *lc, lm_bit which, odin3_net_id *out);

/*
 * H-2: the nets of operand port `port` (a pinslice of the source) resized to `width` bits into
 * out[0 .. width): bits below the operand's width are its nets; bits at or above it are the sign
 * net (the operand's last bit) when is_signed, else gnd; a longer operand is truncated.
 * An operand with no pins is all gnd. NO_MEMORY from the gnd net.
 */
odin3_status lm_extend(lm_ctx *lc, odin3_pinslice port, uint32_t width, bool is_signed,
                       odin3_net_id *out);

/*
 * The destructive step: deletes the source cell, renames the first node to the source's name, then
 * makes the ties in order (a net an earlier tie merged away stands for the net it went into; a tie
 * with a net that is none or whose two sides are one net does nothing). A failure before the delete
 * undoes the transaction (lm_abort) and returns its status.
 */
odin3_status lm_commit(lm_ctx *lc, const lm_tie *ties, size_t n_ties);

/* Undoes what the transaction made (best effort); the source is untouched. */
void lm_abort(lm_ctx *lc);

/* Checks the design budget: OK, else logs the budget's message and returns ODIN3_ERR_PARSE. */
odin3_status lm_budget_check(const lm_ctx *lc);

/*
 * Logs fmt at level, located at the source of node (its first source leaf) or, without a
 * location, as the bare text. The texts carry their own "lpmmap: " prefix.
 */
void lm_diag(const lm_ctx *lc, odin3_log_level level, odin3_node_id node, const char *fmt, ...)
    ODIN3_PRINTF(4, 5);

/* The label of node for messages: its name, else `$c<ID>`. */
void lm_label(const lm_ctx *lc, odin3_node_id node, char *out, size_t size);

/* The rules (one file each). Each maps src or leaves it soft with a warning. */
odin3_status lm_map_addsub(lm_ctx *lc, odin3_node_id src);
odin3_status lm_map_mult(lm_ctx *lc, odin3_node_id src);
odin3_status lm_map_compare(lm_ctx *lc, odin3_node_id src);
odin3_status lm_map_mem(lm_ctx *lc, odin3_node_id src);

/* Appends count open (none) nets to vec and puts the address of the first in *first; the pointer is
 * valid until vec grows again, so reserve all of a builder's arrays before taking any pointer.
 * NO_MEMORY.
 */
odin3_status lm_net_array(odin3_vec *vec, uint32_t count, odin3_net_id **first);

/* Runs build(lc, src, ties) as one transaction: builds, then commits, or aborts on failure. */
typedef odin3_status (*lm_build_fn)(lm_ctx *lc, odin3_node_id src, odin3_vec *ties);
odin3_status lm_transact(lm_ctx *lc, odin3_node_id src, lm_build_fn build);

#endif
