/*
 * fnsim_internal.h — the compiled form of a cell's fns, shared by fnsim_compile.c and
 * fnsim_eval.c. Never included elsewhere.
 */
#ifndef ODIN3_TECHLIB_FNSIM_INTERNAL_H
#define ODIN3_TECHLIB_FNSIM_INTERNAL_H

#include "techlib/expr.h"
#include "techlib/fnsim.h"

#include <stdint.h>

/* No output port: the node is not the root of an fn. */
#define ODIN3_FNSIM_NONE UINT32_MAX

typedef enum odin3_fnsim_kind {
    ODIN3_FNSIM_PORT,  /* ref: input port index */
    ODIN3_FNSIM_PARAM, /* ref: parameter index */
    ODIN3_FNSIM_INT,   /* expr->ival */
    ODIN3_FNSIM_SIZED, /* expr->bits, expr->nbits */
    ODIN3_FNSIM_UNARY, /* op; kid[0] */
    ODIN3_FNSIM_BINARY,
    ODIN3_FNSIM_TERNARY, /* kid[0] ? kid[1] : kid[2] */
    ODIN3_FNSIM_BITSEL,  /* kid[0][expr->b] */
    ODIN3_FNSIM_SLICE,   /* kid[0][expr->b : expr->c] */
    ODIN3_FNSIM_CONCAT,  /* items[first .. first+count), most significant first */
    ODIN3_FNSIM_REPL     /* {expr->a {kid[0]}} */
} odin3_fnsim_kind;

/* One expression node, in postorder: a node's operands (kid, items) come before it. */
typedef struct odin3_fnsim_node {
    odin3_fnsim_kind kind;
    odin3_expr_op op;
    uint32_t ref;
    uint32_t kid[3];
    uint32_t first;
    uint32_t count;
    uint32_t out; /* the output port this fn root drives, else ODIN3_FNSIM_NONE */
    const odin3_expr *expr;
} odin3_fnsim_node;

/*
 * Every fn's tree, one after another, each ending with its root. param_ids are the parameters'
 * names (strtab IDs) in definition order, to evaluate constant indices and counts.
 */
struct odin3_fnsim_prog {
    const odin3_fnsim_node *nodes;
    uint32_t n_nodes;
    const uint32_t *items;
    const uint32_t *param_ids;
    uint32_t n_params;
};

#endif
