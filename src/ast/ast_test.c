/* ast_test.c — hidden test hooks for the AST store: caps, and one corruption per check rule. */
#include "ast/ast_test.h"

#include "ast/ast.h"
#include "ast/ast_internal.h"
#include "ast/kinds.h"
#include "ast/symtab.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "util/log.h"
#include "util/pagevec.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { ATTR_COUNT_SHIFT = 32, FLAG_BITS = 16 };

void odin3_ast_test_set_limits(odin3_ast *ast, odin3_ast_limits limits) {
    ast->max_children = limits.max_children;
    ast->max_nodes = limits.max_nodes;
    ast->max_depth =
        limits.max_depth < ODIN3_AST_MAX_DEPTH ? limits.max_depth : ODIN3_AST_MAX_DEPTH;
}

/* --- node faults ---------------------------------------------------------------------------- */

static odin3_ast_node *node_at(odin3_ast *ast, uint32_t id) {
    return id != 0 && id < odin3_pagevec_len(ast->nodes) ? odin3_pagevec_at(ast->nodes, id) : NULL;
}

static const odin3_ast_kind_info *info_of(const odin3_ast_node *rec) {
    return rec != NULL ? odin3_ast_kind_info_of(rec->kind) : NULL;
}

/* The writable child entry idx of a node with a child span. */
static odin3_ast_id *child_slot(odin3_ast *ast, const odin3_ast_node *rec, uint32_t idx) {
    return (odin3_ast_id *)ast->children.data + rec->child + idx;
}

static bool set_flag(odin3_ast_node *rec) {
    const odin3_ast_kind_info *info = info_of(rec);
    for (uint32_t bit = 0; info != NULL && bit < FLAG_BITS; bit++) {
        if ((info->flags & (1U << bit)) == 0) {
            rec->flags = (uint16_t)(rec->flags | (1U << bit));
            return true;
        }
    }
    return false;
}

static bool scalar_fault(odin3_ast *ast, odin3_ast_test_target target) {
    odin3_ast_node *rec = node_at(ast, target.id);
    const odin3_ast_kind_info *info = info_of(rec);
    if (info == NULL) {
        return false;
    }
    switch (target.fault) {
    case ODIN3_AST_TEST_KIND:
        rec->kind = ODIN3_AST_KIND_COUNT;
        return true;
    case ODIN3_AST_TEST_SUB:
        rec->sub = (uint8_t)(info->sub_count > 0 ? info->sub_count : 1);
        return true;
    case ODIN3_AST_TEST_FLAG:
        return set_flag(rec);
    case ODIN3_AST_TEST_NAME_MISSING:
        if (info->name_rule != ODIN3_AST_NAME_REQ) {
            return false;
        }
        rec->name = 0;
        return true;
    case ODIN3_AST_TEST_NAME_EXTRA:
        if (info->name_rule != ODIN3_AST_NAME_NO || target.arg == 0) {
            return false;
        }
        rec->name = target.arg;
        return true;
    case ODIN3_AST_TEST_NAME_BAD:
        rec->name = (uint32_t)odin3_strtab_count(odin3_design_strtab(ast->design));
        return true;
    default:
        return false;
    }
}

static bool payload_fault(odin3_ast *ast, odin3_ast_test_target target) {
    odin3_ast_node *rec = node_at(ast, target.id);
    const odin3_ast_kind_info *info = info_of(rec);
    if (info == NULL || rec->nchild != 0) {
        return false;
    }
    bool has_payload = info->payload != ODIN3_AST_PAYLOAD_NONE;
    if (target.fault == ODIN3_AST_TEST_PAYLOAD_PLAIN && !has_payload) {
        rec->child = 1;
        return true;
    }
    if (target.fault == ODIN3_AST_TEST_PAYLOAD_INDEX && has_payload) {
        rec->child = (uint32_t)ast->payloads.len + 1;
        return true;
    }
    return false;
}

static bool loc_fault(odin3_ast *ast, odin3_ast_test_target target) {
    odin3_ast_node *rec = node_at(ast, target.id);
    if (rec == NULL) {
        return false;
    }
    if (target.fault == ODIN3_AST_TEST_LOC) {
        rec->loc = UINT32_MAX;
        return true;
    }
    if (rec->loc == 0 || rec->end <= rec->loc) {
        return false;
    }
    uint32_t loc = rec->loc;
    rec->loc = rec->end;
    rec->end = loc;
    return true;
}

/* The first child index the fault applies to, or UINT32_MAX. */
static uint32_t pick_child(const odin3_ast *ast, const odin3_ast_node *rec,
                           odin3_ast_test_fault fault) {
    const odin3_ast_kind_info *info = info_of(rec);
    for (uint32_t i = 0; info != NULL && i < rec->nchild; i++) {
        uint32_t kid = ((const odin3_ast_id *)ast->children.data)[rec->child + i].v;
        bool fits = kid != 0;
        if (fault == ODIN3_AST_TEST_MANDATORY) {
            fits = i < info->nslots && (info->slots[i] & ODIN3_AST_SLOT_OPT) == 0;
        } else if (fault == ODIN3_AST_TEST_TAIL_ZERO) {
            fits = i >= info->nslots;
        }
        if (fits) {
            return i;
        }
    }
    return UINT32_MAX;
}

/* CHILD_ABOVE wants an existing arg ≥ id, CHILD_SHARED one < id; the others no arg. */
static bool arg_ok(odin3_ast *ast, odin3_ast_test_target target) {
    if (target.fault == ODIN3_AST_TEST_CHILD_ABOVE) {
        return node_at(ast, target.arg) != NULL && target.arg >= target.id;
    }
    if (target.fault == ODIN3_AST_TEST_CHILD_SHARED) {
        return node_at(ast, target.arg) != NULL && target.arg < target.id;
    }
    return true;
}

static bool child_fault(odin3_ast *ast, odin3_ast_test_target target) {
    odin3_ast_node *rec = node_at(ast, target.id);
    if (rec == NULL || info_of(rec) == NULL || info_of(rec)->payload != ODIN3_AST_PAYLOAD_NONE) {
        return false;
    }
    if (target.fault == ODIN3_AST_TEST_COUNT) {
        if (rec->nchild == 0) {
            return false;
        }
        rec->nchild--;
        return true;
    }
    uint32_t idx = pick_child(ast, rec, target.fault);
    if (idx == UINT32_MAX || !arg_ok(ast, target)) {
        return false;
    }
    bool zero =
        target.fault == ODIN3_AST_TEST_MANDATORY || target.fault == ODIN3_AST_TEST_TAIL_ZERO;
    child_slot(ast, rec, idx)->v = zero ? 0 : target.arg;
    return true;
}

/* --- table and store faults ----------------------------------------------------------------- */

static bool table_fault(odin3_ast *ast, odin3_ast_test_target target) {
    if (target.fault == ODIN3_AST_TEST_COMMENT_ORDER) {
        if ((uint64_t)target.id + 1 >= ast->comments.len) {
            return false;
        }
        odin3_ast_comment *items = ast->comments.data;
        odin3_ast_comment first = items[target.id];
        if (items[target.id + 1].loc.v == first.loc.v) {
            return false;
        }
        items[target.id] = items[target.id + 1];
        items[target.id + 1] = first;
        return true;
    }
    const odin3_ast_node *rec = node_at(ast, target.id);
    if (rec == NULL || rec->kind != ODIN3_AST_NUMBER) {
        return false;
    }
    const odin3_ast_payload_rec *pay = odin3_ast_payload_rec_of(ast, rec->child);
    if (pay == NULL || pay->kind != ODIN3_AST_PAYLOAD_NUMBER) {
        return false;
    }
    odin3_ast_number *num = (odin3_ast_number *)pay->u.number; /* the arena record is writable */
    num->has_xz = !num->has_xz;
    return true;
}

static odin3_status attr_span_fault(odin3_ast *ast, uint32_t id) {
    uint64_t span = 0;
    if (ast->attrs == NULL || !odin3_u64map_get(ast->attrs, id, &span)) {
        return ODIN3_ERR_INVALID_ARG;
    }
    uint64_t count = ast->children.len + 1;
    return odin3_u64map_put(ast->attrs,
                            (odin3_kv){id, (count << ATTR_COUNT_SHIFT) | (uint32_t)span});
}

static bool symbol_fault(odin3_ast *ast, odin3_ast_test_target target) {
    odin3_symtab *st = ast->symtab;
    if (st == NULL || target.id == 0 || target.id >= odin3_pagevec_len(st->symbols)) {
        return false;
    }
    odin3_symbol *sym = odin3_pagevec_at(st->symbols, target.id);
    if (target.fault == ODIN3_AST_TEST_SYMBOL_SCOPE) {
        sym->scope.v = (uint32_t)odin3_pagevec_len(st->scopes);
    } else {
        sym->name = target.arg;
    }
    return true;
}

static bool store_fault(odin3_ast *ast, odin3_ast_test_target target) {
    if (target.fault == ODIN3_AST_TEST_HEIGHT) {
        ast->max_height++;
    } else {
        ast->form = ODIN3_AST_FORM_ELABORATED;
    }
    return true;
}

typedef bool (*fault_fn)(odin3_ast *ast, odin3_ast_test_target target);

static const fault_fn FAULTS[ODIN3_AST_TEST_FAULT_END] = {
    [ODIN3_AST_TEST_KIND] = scalar_fault,
    [ODIN3_AST_TEST_SUB] = scalar_fault,
    [ODIN3_AST_TEST_FLAG] = scalar_fault,
    [ODIN3_AST_TEST_PAYLOAD_PLAIN] = payload_fault,
    [ODIN3_AST_TEST_PAYLOAD_INDEX] = payload_fault,
    [ODIN3_AST_TEST_LOC] = loc_fault,
    [ODIN3_AST_TEST_LOC_ORDER] = loc_fault,
    [ODIN3_AST_TEST_COUNT] = child_fault,
    [ODIN3_AST_TEST_MANDATORY] = child_fault,
    [ODIN3_AST_TEST_TAIL_ZERO] = child_fault,
    [ODIN3_AST_TEST_CHILD_ABOVE] = child_fault,
    [ODIN3_AST_TEST_CHILD_SHARED] = child_fault,
    [ODIN3_AST_TEST_NAME_MISSING] = scalar_fault,
    [ODIN3_AST_TEST_NAME_BAD] = scalar_fault,
    [ODIN3_AST_TEST_NAME_EXTRA] = scalar_fault,
    [ODIN3_AST_TEST_HEIGHT] = store_fault,
    [ODIN3_AST_TEST_COMMENT_ORDER] = table_fault,
    [ODIN3_AST_TEST_NUMBER_XZ] = table_fault,
    [ODIN3_AST_TEST_FORM] = store_fault,
    [ODIN3_AST_TEST_SYMBOL_SCOPE] = symbol_fault,
    [ODIN3_AST_TEST_SYMBOL_NAME] = symbol_fault,
};

odin3_status odin3_ast_test_corrupt(odin3_ast *ast, odin3_ast_test_target target) {
    if (ast == NULL || target.fault < ODIN3_AST_TEST_KIND ||
        target.fault >= ODIN3_AST_TEST_FAULT_END) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_ast_test_corrupt: NULL store or unknown fault");
        return ODIN3_ERR_INVALID_ARG;
    }
    if (target.fault == ODIN3_AST_TEST_ATTR_SPAN) {
        odin3_status st = attr_span_fault(ast, target.id);
        if (st == ODIN3_ERR_INVALID_ARG) {
            odin3_log(ODIN3_LOG_ERROR, "odin3_ast_test_corrupt: node %u has no attributes",
                      (unsigned)target.id);
        }
        return st;
    }
    if (FAULTS[target.fault] == NULL || !FAULTS[target.fault](ast, target)) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_ast_test_corrupt: precondition of fault %d fails",
                  (int)target.fault);
        return ODIN3_ERR_INVALID_ARG;
    }
    return ODIN3_OK;
}
