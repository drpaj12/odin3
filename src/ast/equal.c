/*
 * equal.c — structural equality of two subtrees (§6), the round-trip oracle of §11: iterative
 * over an explicit stack of node pairs, across stores and designs.
 */
#include "ast/ast.h"
#include "ast/kinds.h"
#include "ast/print.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* One store's side of a comparison. */
typedef struct side {
    const odin3_ast *ast;
    odin3_ast_id node;
} side;

typedef struct node_pair {
    uint32_t left;
    uint32_t right;
} node_pair;

static bool same_bytes(odin3_bytes lhs, odin3_bytes rhs) {
    return lhs.len == rhs.len && (lhs.len == 0 || memcmp(lhs.ptr, rhs.ptr, lhs.len) == 0);
}

static bool same_number(const odin3_ast_number *lhs, const odin3_ast_number *rhs) {
    if (lhs == NULL || rhs == NULL) {
        return lhs == rhs;
    }
    return lhs->width == rhs->width && lhs->nbits == rhs->nbits && lhs->has_xz == rhs->has_xz &&
           lhs->has_question == rhs->has_question && memcmp(lhs->bits, rhs->bits, lhs->nbits) == 0;
}

static bool same_payload(side lhs, side rhs, uint32_t payload) {
    switch (payload) {
    case ODIN3_AST_PAYLOAD_NUMBER:
        return same_number(odin3_ast_number_of(lhs.ast, lhs.node),
                           odin3_ast_number_of(rhs.ast, rhs.node));
    case ODIN3_AST_PAYLOAD_REAL: {
        double vx = odin3_ast_real(lhs.ast, lhs.node);
        double vy = odin3_ast_real(rhs.ast, rhs.node);
        /* bit-exact in value: -0.0 differs from 0.0 (the printer writes them apart) */
        return (vx == vy && signbit(vx) == signbit(vy)) || (isnan(vx) && isnan(vy));
    }
    case ODIN3_AST_PAYLOAD_TEXT:
        return same_bytes(odin3_ast_text(lhs.ast, lhs.node), odin3_ast_text(rhs.ast, rhs.node));
    default:
        return true;
    }
}

/* The records agree on everything but locations (children and attributes are pushed). */
static bool same_record(side lhs, side rhs) {
    odin3_ast_kind kind = odin3_ast_kind_of(lhs.ast, lhs.node);
    const odin3_ast_kind_info *info = odin3_ast_kind_info_of(kind);
    if (info == NULL || kind != odin3_ast_kind_of(rhs.ast, rhs.node) ||
        odin3_ast_sub(lhs.ast, lhs.node) != odin3_ast_sub(rhs.ast, rhs.node) ||
        odin3_ast_flags(lhs.ast, lhs.node) != odin3_ast_flags(rhs.ast, rhs.node) ||
        odin3_ast_nchild(lhs.ast, lhs.node) != odin3_ast_nchild(rhs.ast, rhs.node) ||
        odin3_ast_attrs(lhs.ast, lhs.node).n != odin3_ast_attrs(rhs.ast, rhs.node).n) {
        return false;
    }
    return same_bytes(odin3_pr_name_bytes(lhs.ast, lhs.node),
                      odin3_pr_name_bytes(rhs.ast, rhs.node)) &&
           same_payload(lhs, rhs, info->payload);
}

/* Pushes the pairs (xs[i], ys[i]); both spans have n entries. */
static odin3_status push_pairs(odin3_vec *stack, odin3_ast_span xs, odin3_ast_span ys) {
    if (odin3_vec_reserve(stack, stack->len + xs.n) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < xs.n && i < ys.n; i++) {
        *(node_pair *)odin3_vec_push(stack) = (node_pair){xs.ids[i].v, ys.ids[i].v};
    }
    return ODIN3_OK;
}

/* Compares the pair on top of the stack; pushes its children and attributes when equal. */
static odin3_status step(odin3_vec *stack, side lhs, side rhs, bool *equal) {
    node_pair pair = *(const node_pair *)odin3_vec_cat(stack, stack->len - 1);
    odin3_vec_pop(stack);
    lhs.node = (odin3_ast_id){pair.left};
    rhs.node = (odin3_ast_id){pair.right};
    if (pair.left == 0 || pair.right == 0) { /* absent slots: equal only when both are */
        *equal = pair.left == pair.right;
        return ODIN3_OK;
    }
    if (!same_record(lhs, rhs)) {
        *equal = false;
        return ODIN3_OK;
    }
    odin3_status st =
        push_pairs(stack, odin3_ast_attrs(lhs.ast, lhs.node), odin3_ast_attrs(rhs.ast, rhs.node));
    if (st == ODIN3_OK) {
        st = push_pairs(stack, odin3_ast_children(lhs.ast, lhs.node),
                        odin3_ast_children(rhs.ast, rhs.node));
    }
    return st;
}

odin3_status odin3_ast_equal(const odin3_ast *ast_a, odin3_ast_id ra, const odin3_ast *ast_b,
                             odin3_ast_id rb, bool *equal) {
    if (ast_a == NULL || ast_b == NULL || equal == NULL ||
        odin3_ast_kind_of(ast_a, ra) == ODIN3_AST_NONE ||
        odin3_ast_kind_of(ast_b, rb) == ODIN3_AST_NONE) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_ast_equal: %s",
                  ast_a == NULL || ast_b == NULL || equal == NULL ? "NULL argument"
                                                                  : "a root is 0 or not made");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_vec stack;
    odin3_vec_init(&stack, sizeof(node_pair));
    side lhs = {ast_a, ra};
    side rhs = {ast_b, rb};
    odin3_ast_span first_a = {&ra, 1};
    odin3_ast_span first_b = {&rb, 1};
    odin3_status st = push_pairs(&stack, first_a, first_b);
    bool same = true;
    while (st == ODIN3_OK && same && stack.len > 0) {
        st = step(&stack, lhs, rhs, &same);
    }
    odin3_vec_free(&stack);
    if (st == ODIN3_OK) {
        *equal = same;
    }
    return st;
}
