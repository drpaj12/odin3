/*
 * hm_kit.h — hardmap's local kit (3C plan Task 2, agent defaults (b), (g)): the context, the hard
 * cell types found by name (H-3), one transaction per source cell (H-7, H-9), node creation with
 * the names and `hardmap` attributes of agent default (b), the canonical constant nets of H-5, and
 * the 1-bit glue (H-6). It does not depend on opt_ctx (as 3B's lower_connect does not).
 *
 * A transaction is hm_begin_cell, any number of hm_new_node / hm_fresh_nets / glue calls (all on
 * fresh nets, the source untouched), then hm_commit: reserve, delete the source, rename the first
 * node to the source's name, merge the ties. A failure before the delete is undone by hm_abort
 * (best effort: a node that cannot be deleted stays as a dead-weight cell on unread nets), leaving
 * the IR valid and the source cell intact (H-9). A failure after it (a merge that needs an alias
 * record) logs "out of memory in the middle of a mapping ... discard it" and leaves the IR valid.
 *
 * Naming (agent default (b), approved in PHASE3 #33): the first node a transaction creates takes
 * the source's name and attributes at commit; every other node is named `<cell>.<role>` (`$c<ID>`
 * for an unnamed source), made unique by `_1`, `_2` ...; every node carries the string attribute
 * `hardmap` = `<cell type>:<role>`. The source is reached through the DERIVED record.
 */
#ifndef ODIN3_PASSES_HARDMAP_HM_KIT_H
#define ODIN3_PASSES_HARDMAP_HM_KIT_H

#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "passes/hardmap/hardmap.h"
#include "passes/manager.h"
#include "util/attr.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>

/* The hard cell types hardmap uses, found by name and never created (H-3). */
typedef enum hm_hard { HM_ADDER, HM_MULTIPLY, HM_SP_RAM, HM_DP_RAM, HM_N_HARD } hm_hard;

/* The word-level glue cells (H-6), all of width 1. */
typedef enum hm_glue { HM_G_AND, HM_G_NOT, HM_G_MUX, HM_G_DFF, HM_N_GLUE } hm_glue;

/* The constants of H-5: gnd, vcc and the pad (`$_CONSTX_`, which the writers print as `unconn`). */
typedef enum hm_bit { HM_GND, HM_VCC, HM_PAD, HM_N_CONST } hm_bit;

/* One merge of a commit: `drop` is merged into `keep` (odin3_net_merge). */
typedef struct hm_tie {
    odin3_net_id keep;
    odin3_net_id drop;
} hm_tie;

typedef struct hm_ctx {
    odin3_pass_ctx *pass;
    odin3_design *design;
    odin3_module *module;
    odin3_hardmap_stats stats;
    odin3_celltype_id hard[HM_N_HARD];   /* found by hm_need; {0} until then */
    odin3_celltype_id glue[HM_N_GLUE];   /* found on first use */
    odin3_celltype_id konst[HM_N_CONST]; /* $_CONST0_, $_CONST1_, $_CONSTX_ */
    odin3_net_id consts[HM_N_CONST]; /* the canonical nets of the current module; {0}: unknown */
    odin3_node_id src;               /* the cell of the transaction */
    odin3_prov_id prov;              /* its DERIVED record (0 when the source has none) */
    odin3_node_id first;             /* the first node the transaction made */
    uint32_t src_name;               /* the source's name (strtab ID, 0: unnamed) */
    uint64_t pins_created;           /* the budget's counter (H-9) */
    odin3_vec made_nodes;            /* odin3_node_id: undone by hm_abort */
    odin3_vec made_nets;             /* odin3_net_id: the fresh nets, likewise */
    odin3_vec merged;                /* odin3_net_pair: the merges of a commit */
    odin3_strbuf text;               /* scratch for names and attribute values */
} hm_ctx;

/* The request of hm_new_node: type, parameters (NULL: the defaults), one netvec per port and
 * the role. */
typedef struct hm_node_req {
    odin3_celltype_id type;
    const odin3_value *params;
    uint32_t n_params;
    const odin3_netvec *ports;
    const char *role;
} hm_node_req;

/* The registered cell type named name, false when the design has none; no allocation, no log. */
bool hm_find_type(const odin3_design *design, const char *name, odin3_celltype_id *out);

/* Zeroes *hc and fills it; allocates nothing. */
void hm_init(hm_ctx *hc, odin3_pass_ctx *pass, odin3_design *design);
void hm_free(hm_ctx *hc);

/* Makes module the module hm_new_node, hm_const and the transactions work on. */
void hm_set_module(hm_ctx *hc, odin3_module *module);

/*
 * The cell type of hard cell `which` in *out, checked once per context: it must be registered,
 * of granularity HARD, with the ports and parameters hardmap indexes by position. Otherwise the
 * error "hardmap: cell type 'multiply' is not registered; read_techlib lib/vtr.o3lib first" (or
 * "... is not a hard block ..." / "... does not have the expected ports ...") is logged and
 * ODIN3_ERR_INVALID_ARG returned, nothing changed. The error is located at node `at` (none: bare).
 */
odin3_status hm_need(hm_ctx *hc, hm_hard which, odin3_node_id at, odin3_celltype_id *out);

/*
 * Starts the transaction for source cell `src` (live, in the current module): begins one
 * provenance operation and makes its DERIVED record with the source's record as parent (H-7).
 * NO_MEMORY with nothing made.
 */
odin3_status hm_begin_cell(hm_ctx *hc, odin3_node_id src);

/* n fresh unnamed nets with the transaction's record in out[0 .. n). NO_MEMORY: the ones made are
 * undone by hm_abort. */
odin3_status hm_fresh_nets(hm_ctx *hc, uint32_t n, odin3_net_id *out);

/*
 * Creates a node on the given nets with the transaction's record, its name and `hardmap`
 * attribute (see the top of this file). Every 2^16 created pins, and before the first, the design
 * budget is checked: over it, the budget's message is logged and ODIN3_ERR_PARSE returned.
 * INVALID_ARG/NO_MEMORY as odin3_node_create_connected, nothing made by this call.
 */
odin3_status hm_new_node(hm_ctx *hc, const hm_node_req *req, odin3_node_id *out);

/*
 * The canonical net of constant `which` in the current module: the net of the lowest-ID live
 * constant node of that value, else a new constant node (with the module's record). NO_MEMORY.
 */
odin3_status hm_const(hm_ctx *hc, hm_bit which, odin3_net_id *out);

/* Glue cells of width 1 (H-6) on fresh nets; *out is the output net. role as hm_new_node's. */
odin3_status hm_not(hm_ctx *hc, const char *role, odin3_net_id in, odin3_net_id *out);
odin3_status hm_and(hm_ctx *hc, const char *role, const odin3_net_id in[2], odin3_net_id *out);
/* sel ? in[1] : in[0]. */
odin3_status hm_mux(hm_ctx *hc, const char *role, odin3_net_id sel, const odin3_net_id in[2],
                    odin3_net_id *out);
/* A rising-edge flip-flop. */
odin3_status hm_dff(hm_ctx *hc, const char *role, const odin3_net_id in[2], odin3_net_id *out);

/*
 * The destructive step: deletes the source cell, renames the first node to the source's name, then
 * makes the ties in order (a net an earlier tie merged away stands for the net it went into; a tie
 * with a net that is none or whose two sides are one net does nothing). A failure before the delete
 * undoes the transaction (hm_abort) and returns its status.
 */
odin3_status hm_commit(hm_ctx *hc, const hm_tie *ties, size_t n_ties);

/* Undoes what the transaction made (best effort); the source is untouched. */
void hm_abort(hm_ctx *hc);

/* Checks the design budget: OK, else logs the budget's message and returns ODIN3_ERR_PARSE. */
odin3_status hm_budget_check(const hm_ctx *hc);

/*
 * Logs fmt at level, located at the source of node (its first source leaf: "file:line:col: error:
 * ..." or the source manager's form) or, without a location, as the bare text. The texts carry
 * their own "hardmap: " prefix.
 */
void hm_diag(const hm_ctx *hc, odin3_log_level level, odin3_node_id node, const char *fmt, ...)
    ODIN3_PRINTF(4, 5);

/* The label of node for messages: its name, else `$c<ID>`. */
void hm_label(const hm_ctx *hc, odin3_node_id node, char *out, size_t size);

/* The `$mul` rule (mul.c, M-1..M-7): maps src or leaves it soft with a warning. */
odin3_status hm_map_mul(hm_ctx *hc, odin3_node_id src);

/* The `$add` / `$sub` rule (adder.c, A-1..A-9): maps src to an `adder` chain or leaves it soft with
 * a warning (Y_WIDTH above max(A_WIDTH, B_WIDTH) + 1); a cell with both operand widths 0 is left
 * alone. */
odin3_status hm_map_adder(hm_ctx *hc, odin3_node_id src);

/* The explicit-instance check of M-6 for node (a `multiply`): the type's shape (H-3), then WIDTH
 * above 36: OK, or the located error. */
odin3_status hm_check_multiply(hm_ctx *hc, odin3_node_id node);

/* The explicit `multiply` rule (M-6 as amended): treat src like the equivalent `$mul`, with a
 * warning; WIDTH 36 is left as it is. */
odin3_status hm_map_explicit(hm_ctx *hc, odin3_node_id src);

#endif
