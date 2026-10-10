/* check_elab.c — check rule 7: the elaborated subset of the AST format (§4.6, AST-9). */
#include "ast/check.h"

#include "ast/ast.h"
#include "ast/kinds.h"
#include "odin3/odin3.h"

#include <stdbool.h>
#include <stdint.h>

enum { RULE_ELAB = 7 };

/* Kinds the elaborated form never holds (§4.6 "Absent"). */
static const bool ABSENT[ODIN3_AST_KIND_COUNT] = {
    [ODIN3_AST_DIRECTIVE] = true,     [ODIN3_AST_UDP_DECL] = true,
    [ODIN3_AST_CONFIG_DECL] = true,   [ODIN3_AST_SPECIFY_BLOCK] = true,
    [ODIN3_AST_DEFPARAM] = true,      [ODIN3_AST_GENERATE] = true,
    [ODIN3_AST_GEN_FOR] = true,       [ODIN3_AST_GEN_IF] = true,
    [ODIN3_AST_GEN_CASE] = true,      [ODIN3_AST_GENVAR_DECL] = true,
    [ODIN3_AST_DELAY] = true,         [ODIN3_AST_STRENGTH] = true,
    [ODIN3_AST_MINTYPMAX] = true,     [ODIN3_AST_PROC_CONT_ASSIGN] = true,
    [ODIN3_AST_WAIT] = true,          [ODIN3_AST_DISABLE] = true,
    [ODIN3_AST_EVENT_TRIGGER] = true, [ODIN3_AST_FOREVER] = true,
    [ODIN3_AST_PAR_BLOCK] = true,     [ODIN3_AST_EVENT_DECL] = true,
    [ODIN3_AST_PORT_REF] = true,
};

static void fail(odin3_ast_checker *cx, const char *what) {
    odin3_ast_check_node_fail(cx, RULE_ELAB, what);
}

/* A resolved constant: a NUMBER, STRING or REAL leaf. */
static bool is_const(const odin3_ast_checker *cx, odin3_ast_id node) {
    odin3_ast_kind kind = odin3_ast_kind_of(cx->ast, node);
    return kind == ODIN3_AST_NUMBER || kind == ODIN3_AST_STRING || kind == ODIN3_AST_REAL;
}

/* Slot idx of the current node, when present, is a resolved constant. */
static void const_slot(odin3_ast_checker *cx, uint32_t idx) {
    odin3_ast_id kid = odin3_ast_check_kid(cx, cx->id, idx);
    if (kid.v != 0 && !is_const(cx, kid)) {
        fail(cx, "a constant here must be a NUMBER, STRING or REAL leaf");
    }
}

/* Every entry of the current node's tail (from slot `from`) passes `ok`. */
typedef bool (*entry_pred)(const odin3_ast_checker *cx, odin3_ast_id entry);

static void each_entry(odin3_ast_checker *cx, odin3_ast_span entries, entry_pred ok,
                       const char *what) {
    for (uint32_t i = 0; i < entries.n; i++) {
        if (entries.ids[i].v != 0 && entries.ids[i].v < cx->id.v && !ok(cx, entries.ids[i])) {
            fail(cx, what);
        }
    }
}

/* The current node's children from index `from` on. */
static odin3_ast_span tail_from(const odin3_ast_checker *cx, uint32_t from) {
    odin3_ast_span kids = odin3_ast_check_span(cx, cx->id);
    return from < kids.n ? (odin3_ast_span){kids.ids + from, kids.n - from}
                         : (odin3_ast_span){NULL, 0};
}

static bool param_value_ok(const odin3_ast_checker *cx, odin3_ast_id declarator) {
    return is_const(cx, odin3_ast_check_kid(cx, declarator, 0));
}

static bool hier_index_ok(const odin3_ast_checker *cx, odin3_ast_id part) {
    return odin3_ast_kind_of(cx->ast, part) != ODIN3_AST_SELECT ||
           is_const(cx, odin3_ast_check_kid(cx, part, 1));
}

static bool connection_ok(const odin3_ast_checker *cx, odin3_ast_id conn) {
    odin3_ast_id value = odin3_ast_check_kid(cx, conn, 0);
    return value.v == 0 || is_const(cx, value);
}

static bool one_declarator(const odin3_ast_checker *cx, odin3_ast_id port) {
    return odin3_ast_kind_of(cx->ast, port) != ODIN3_AST_PORT_DECL ||
           odin3_ast_nchild(cx->ast, port) == odin3_ast_slot_count(ODIN3_AST_PORT_DECL) + 1;
}

/* The entries of the LIST in slot idx of the current node. */
static odin3_ast_span list_in_slot(const odin3_ast_checker *cx, uint32_t idx) {
    odin3_ast_id list = odin3_ast_check_kid(cx, cx->id, idx);
    if (list.v >= cx->id.v || odin3_ast_kind_of(cx->ast, list) != ODIN3_AST_LIST) {
        return (odin3_ast_span){NULL, 0};
    }
    return odin3_ast_check_span(cx, list);
}

/* ALWAYS of sub `always` is over TIMING_STMT[EVENT_CONTROL, S]; Phase 5 subs are free. */
static void check_always(odin3_ast_checker *cx) {
    if (cx->rec->sub != ODIN3_AST_ALWAYS_ALWAYS) {
        return;
    }
    odin3_ast_id timing = odin3_ast_check_kid(cx, cx->id, 0);
    bool ok =
        timing.v < cx->id.v && odin3_ast_kind_of(cx->ast, timing) == ODIN3_AST_TIMING_STMT &&
        odin3_ast_kind_of(cx->ast, odin3_ast_check_kid(cx, timing, 0)) == ODIN3_AST_EVENT_CONTROL &&
        odin3_ast_check_kid(cx, timing, 1).v != 0;
    if (!ok) {
        fail(cx, "always is not over TIMING_STMT[EVENT_CONTROL, S]");
    }
}

static void check_param_decl(odin3_ast_checker *cx) {
    if (cx->rec->sub != ODIN3_AST_PARAM_DECL_LOCALPARAM) {
        fail(cx, "parameter or specparam in the elaborated form");
    }
    each_entry(cx, tail_from(cx, odin3_ast_slot_count(ODIN3_AST_PARAM_DECL)), param_value_ok,
               "a localparam value must be a NUMBER, STRING or REAL leaf");
}

static void check_select(odin3_ast_checker *cx) {
    if (cx->rec->sub == ODIN3_AST_SELECT_PART) {
        const_slot(cx, 1);
    }
    if (cx->rec->sub != ODIN3_AST_SELECT_BIT) {
        const_slot(cx, 2);
    }
}

/* The resolved-constant positions (§4.6 "Resolved constants"). */
static void check_constants(odin3_ast_checker *cx) {
    switch (cx->rec->kind) {
    case ODIN3_AST_RANGE:
        const_slot(cx, 0);
        const_slot(cx, 1);
        break;
    case ODIN3_AST_REPLICATE:
    case ODIN3_AST_REPEAT:
        const_slot(cx, 0);
        break;
    case ODIN3_AST_SELECT:
        check_select(cx);
        break;
    case ODIN3_AST_HIER_NAME:
        each_entry(cx, tail_from(cx, 0), hier_index_ok, "a HIER_NAME index must be constant");
        break;
    case ODIN3_AST_INSTANTIATION:
        each_entry(cx, list_in_slot(cx, 0), connection_ok,
                   "a parameter CONNECTION value must be a constant leaf");
        break;
    default:
        break;
    }
}

/* Kind-specific forms (§4.6 "Absent" sub-kinds and flags, "Expanded hierarchy"). */
static void check_forms(odin3_ast_checker *cx) {
    const odin3_ast_node *rec = cx->rec;
    switch (rec->kind) {
    case ODIN3_AST_VAR_DECL:
        if (rec->sub != ODIN3_AST_VAR_DECL_REG && rec->sub != ODIN3_AST_VAR_DECL_INTEGER) {
            fail(cx, "time, real or realtime variable in the elaborated form");
        }
        break;
    case ODIN3_AST_PARAM_DECL:
        check_param_decl(cx);
        break;
    case ODIN3_AST_CALL:
    case ODIN3_AST_TASK_CALL:
        if ((rec->flags & ODIN3_AST_F_CALL_HIER) != 0) { /* the same bit in TASK_CALL */
            fail(cx, "hierarchical call in the elaborated form");
        }
        break;
    case ODIN3_AST_GEN_BLOCK:
        if (rec->name == 0) {
            fail(cx, "unnamed GEN_BLOCK in the elaborated form");
        }
        break;
    case ODIN3_AST_MODULE:
        each_entry(cx, list_in_slot(cx, 1), one_declarator,
                   "a port PORT_DECL must have exactly one DECLARATOR");
        break;
    case ODIN3_AST_ALWAYS:
        check_always(cx);
        break;
    default:
        check_constants(cx);
        break;
    }
}

_Static_assert(ODIN3_AST_F_CALL_HIER == ODIN3_AST_F_TASK_CALL_HIER,
               "CALL and TASK_CALL share the HIER bit");

void odin3_ast_check_elab_node(odin3_ast_checker *cx) {
    if (ABSENT[cx->rec->kind]) {
        fail(cx, "kind absent from the elaborated form");
        return;
    }
    check_forms(cx);
}

void odin3_ast_check_elab_store(odin3_ast_checker *cx) {
    if (odin3_ast_root_count(cx->ast) != 1) {
        odin3_ast_check_site site = {RULE_ELAB, "store", odin3_ast_root_count(cx->ast)};
        odin3_ast_check_fail(cx, site, "the elaborated form holds exactly one UNIT");
    }
}
