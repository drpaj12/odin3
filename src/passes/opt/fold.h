/*
 * fold.h — constant folding of the opt family (2F spec OP-5, §5; plan Task 5): a word cell whose
 * input bits are all constant is evaluated on 4-state values with 2D's evaluator (Yosys 0.55
 * RTLIL::const_<type>, bit-exact, EL-14) and its output connected to the constant nets. Task 6's
 * opt_expr calls odin3_opt_fold on every cell it visits; the fold table (tests/opt/fold_table.txt,
 * tools/opt-check/gen-fold-table) is its test against Yosys 0.55.
 */
#ifndef ODIN3_PASSES_OPT_FOLD_H
#define ODIN3_PASSES_OPT_FOLD_H

#include "frontends/elab/cval.h"
#include "ir/celltype.h"
#include "ir/ids.h"
#include "odin3/odin3.h"
#include "passes/opt/kit.h"
#include "passes/opt/opt.h"
#include "util/arena.h"

#include <stdbool.h>
#include <stdint.h>

/* True when type is one of the ODIN3_OPT_FOLD_TYPES foldable registry types. */
bool odin3_opt_foldable(const odin3_opt_ctx *oc, odin3_celltype_id type);

/* The counter name of fold_type[index], "const.$<type>"; NULL for an index past the types. */
const char *odin3_opt_fold_name(uint32_t index);

/* odin3_opt_load's request: port `port` of node, read as a value signed when is_signed. */
typedef struct odin3_opt_load_req {
    odin3_node_id node;
    uint32_t port;
    bool is_signed;
    odin3_arena *arena;
} odin3_opt_load_req;

/*
 * Reads the pins of port `port` of a live node as a value in req->arena (LSB first; SIGNED when
 * is_signed). A pin on a constant net (odin3_net_const_value: a $_CONST*_ node's, a constant
 * $sop's) is that bit. *all_const is true and *out set when every pin is on a constant net; it is
 * false (out untouched) for an unconnected pin, a pin on any other net, and a port of no pins or
 * of ODIN3_ELAB_MAX_WIDTH or more bits (the evaluator needs room for one bit more). INVALID_ARG
 * (logged "opt: ...") for a NULL argument or a node that is not live; NO_MEMORY.
 */
odin3_status odin3_opt_load(const odin3_opt_ctx *oc, const odin3_opt_load_req *req, odin3_cval *out,
                            bool *all_const);

/*
 * odin3_opt_eval_cell's request: the foldable cell `node` and its operands as values at the port
 * widths (a, b and s; NULL for a port the type lacks), in `arena`, which also receives the result.
 * The sign flags of the values are ignored: the cell's parameters decide, as for a fold.
 */
typedef struct odin3_opt_eval_req {
    odin3_node_id node;
    const odin3_cval *a, *b, *s;
    odin3_arena *arena;
} odin3_opt_eval_req;

/*
 * Evaluates the foldable cell req->node on the operands of req (the evaluator of odin3_opt_fold,
 * Yosys 0.55's CellTypes::eval, 2F spec OP-5) and stores Y at Y's width in *out; the IR is not
 * touched. *evaluable is false (and *out untouched) for a node that is not a foldable cell, for
 * a missing operand, and for widths past the evaluator's cap. oc supplies the module, its design
 * and its options (opts->keepdc). INVALID_ARG (logged) for a NULL argument or a node that is
 * not live; NO_MEMORY.
 */
odin3_status odin3_opt_eval_cell(odin3_opt_ctx *oc, const odin3_opt_eval_req *req, odin3_cval *out,
                                 bool *evaluable);

/*
 * Folds node when it is a foldable cell with every input bit constant: its Y bits take the result
 * (Yosys const_<type> at Y's width, spec §5) through kit_connect on the canonical constant nets,
 * the node is deleted, *folded is true, and counts->expr.folded and counts->fold_type[<type>]
 * grow by one. Any other node (not foldable, a non-constant input bit, widths past the
 * evaluator's cap) is left alone with *folded false. NO_MEMORY (the IR as kit_connect leaves it);
 * INVALID_ARG (logged) for a NULL argument or a node that is not live.
 */
odin3_status odin3_opt_fold(odin3_opt_ctx *oc, odin3_node_id node, bool *folded);

#endif
