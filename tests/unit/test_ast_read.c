/*
 * test_ast_read.c — unit tests for the AST read side: out-of-range accessors, walk and cursor
 * against a recursive reference (absent slots, abandoned roots), pruning, the depth-cap chain
 * with its single pre-sized allocation, and the parent index.
 */
#include "ast/ast.h"
#include "ast/ast_internal.h"
#include "ast/kinds.h"
#include "ir/design.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/arena.h"
#include "util/hash.h"
#include "util/log.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
    MAX_EVENTS = 1 << 16,
    TREE_NODES = 30,
    RANDOM_NODES = 3000,
    OOM_LIMIT = 16,
};

static odin3_design *design;
static odin3_ast *ast;
static size_t errors_logged;

static void count_sink(odin3_log_level level, const char *msg, void *user) {
    (void)msg;
    (void)user;
    if (level == ODIN3_LOG_ERROR) {
        errors_logged++;
    }
}

void setUp(void) {
    odin3_log_set_sink(count_sink, NULL);
    errors_logged = 0;
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    ast = NULL;
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_ast_create(design, ODIN3_AST_FORM_PARSED, (odin3_passrun_id){1}, &ast));
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
    odin3_ast_destroy(ast);
    ast = NULL;
    odin3_design_destroy(design);
    design = NULL;
}

/* --- builders ------------------------------------------------------------------------------ */

static uint32_t intern(const char *str) {
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_intern(ast, odin3_bytes_cstr(str), &id));
    return id;
}

static odin3_ast_id make(uint8_t kind, uint32_t name, const odin3_ast_id *ids, uint32_t n) {
    odin3_ast_spec spec = {.kind = kind, .name = name};
    odin3_ast_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_make(ast, &spec, ids, n, &id));
    return id;
}

static odin3_ast_id ident(const char *name) {
    return make(ODIN3_AST_IDENT, intern(name), NULL, 0);
}

static odin3_ast_id binary(odin3_ast_id lhs, odin3_ast_id rhs) {
    odin3_ast_id kids[2] = {lhs, rhs};
    return make(ODIN3_AST_BINARY, 0, kids, 2);
}

static odin3_ast_id attr(const char *key, odin3_ast_id value) {
    return make(ODIN3_AST_ATTR, intern(key), &value, 1);
}

/*
 * --- the reference model --------------------------------------------------------------------
 * The brief asks for a recursive reference; the lint gate forbids recursion in tests too
 * (misc-no-recursion), so the reference is a different iterative algorithm: a to-do stack of
 * (node, phase) items with children pushed in reverse, rather than the cursor's frames with a
 * next-child index.
 */

typedef struct event_log {
    uint32_t n;
    uint32_t ev[MAX_EVENTS];
    uint32_t node[MAX_EVENTS];
    uint32_t skip_node; /* pre returns SKIP for this node (0: never) */
} event_log;

static event_log want, got;

static void log_event(event_log *log, odin3_ast_event ev, odin3_ast_id node) {
    TEST_ASSERT_LESS_THAN_UINT32(MAX_EVENTS, log->n);
    log->ev[log->n] = (uint32_t)ev;
    log->node[log->n] = node.v;
    log->n++;
}

typedef struct todo {
    uint32_t node;
    odin3_ast_event phase;
} todo;

static todo todos[MAX_EVENTS];

/* Pushes node's non-zero children, last first, so the first is popped first. */
static uint32_t push_children(odin3_ast_id node, uint32_t top) {
    for (uint32_t i = odin3_ast_nchild(ast, node); i-- > 0;) {
        odin3_ast_id child = odin3_ast_child(ast, node, i);
        TEST_ASSERT_LESS_THAN_UINT32(MAX_EVENTS, top);
        todos[top] = (todo){child.v, ODIN3_AST_ENTER};
        top += child.v != 0 ? 1 : 0;
    }
    return top;
}

static void reference(odin3_ast_id root, event_log *log) {
    uint32_t top = 0;
    todos[top++] = (todo){root.v, ODIN3_AST_ENTER};
    while (top > 0) {
        todo item = todos[--top];
        log_event(log, item.phase, (odin3_ast_id){item.node});
        if (item.phase == ODIN3_AST_LEAVE) {
            continue;
        }
        todos[top++] = (todo){item.node, ODIN3_AST_LEAVE};
        if (item.node != log->skip_node) {
            top = push_children((odin3_ast_id){item.node}, top);
        }
    }
}

static odin3_ast_walk_action on_pre(const odin3_ast *store, odin3_ast_id node, void *user) {
    TEST_ASSERT_TRUE(store == ast);
    event_log *log = user;
    log_event(log, ODIN3_AST_ENTER, node);
    return node.v == log->skip_node ? ODIN3_AST_WALK_SKIP : ODIN3_AST_WALK_CONTINUE;
}

static void on_post(const odin3_ast *store, odin3_ast_id node, void *user) {
    TEST_ASSERT_TRUE(store == ast);
    log_event(user, ODIN3_AST_LEAVE, node);
}

static void assert_logs_equal(void) {
    TEST_ASSERT_EQUAL_UINT32(want.n, got.n);
    TEST_ASSERT_EQUAL_UINT32_ARRAY(want.ev, got.ev, want.n);
    TEST_ASSERT_EQUAL_UINT32_ARRAY(want.node, got.node, want.n);
}

/* walk and cursor from root both match the reference, with pre returning SKIP at skip. */
static void check_root(odin3_ast_id root, uint32_t skip) {
    memset(&want, 0, sizeof want);
    memset(&got, 0, sizeof got);
    want.skip_node = skip;
    got.skip_node = skip;
    reference(root, &want);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_walk(ast, root, on_pre, on_post, &got));
    assert_logs_equal();

    memset(&got, 0, sizeof got);
    odin3_ast_cursor cur;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_cursor_init(&cur, ast, root));
    odin3_ast_event ev = 0;
    odin3_ast_id node = {0};
    while (odin3_ast_cursor_next(&cur, &ev, &node)) {
        log_event(&got, ev, node);
        if (ev == ODIN3_AST_ENTER && node.v == skip) {
            odin3_ast_cursor_skip(&cur);
        }
    }
    TEST_ASSERT_FALSE(odin3_ast_cursor_next(&cur, &ev, &node)); /* stays done */
    odin3_ast_cursor_free(&cur);
    odin3_ast_cursor_free(&cur); /* idempotent */
    assert_logs_equal();
}

/*
 * A 30-node tree under one UNIT with absent slots (MODULE ports, SELECT's third index, IF's
 * else, CONT_ASSIGN's strength and delay), an attribute, and an abandoned root beside it.
 */
typedef struct tree {
    odin3_ast_id unit, module, abandoned, if_stmt, sel, attr, cont;
} tree;

static tree build_tree(void) {
    tree tr;
    odin3_ast_id leaf_a = ident("a");
    odin3_ast_id idx = ident("i");
    odin3_ast_id hi = ident("h");
    odin3_ast_id sel_kids[3] = {leaf_a, idx, {0}};
    tr.sel = make(ODIN3_AST_SELECT, 0, sel_kids, 3); /* 4 */
    odin3_ast_id sum = binary(tr.sel, binary(hi, ident("c")));
    odin3_ast_id assign_kids[2] = {ident("y"), sum};
    odin3_ast_id net_assign = make(ODIN3_AST_NET_ASSIGN, 0, assign_kids, 2);
    odin3_ast_id cont_kids[3] = {{0}, {0}, net_assign};
    tr.cont = make(ODIN3_AST_CONT_ASSIGN, 0, cont_kids, 3); /* 10 */
    tr.attr = attr("keep", ident("one"));                   /* 12 */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_attach(ast, tr.cont, &tr.attr, 1));
    /* abandoned on a syntax error: unreachable from the UNIT */
    odin3_ast_id leaf_p = ident("p");
    odin3_ast_id leaf_q = ident("q");
    tr.abandoned = binary(leaf_p, binary(leaf_q, ident("r"))); /* 17 */
    odin3_ast_id lhs = ident("z");
    odin3_ast_id ba_kids[3] = {lhs, {0}, ident("w")};
    odin3_ast_id then = make(ODIN3_AST_BLOCKING_ASSIGN, 0, ba_kids, 3);
    odin3_ast_id if_kids[3] = {ident("en"), then, {0}};
    tr.if_stmt = make(ODIN3_AST_IF, 0, if_kids, 3);
    odin3_ast_id body_kids[3] = {{0}, tr.if_stmt, make(ODIN3_AST_NULL_STMT, 0, NULL, 0)};
    odin3_ast_id block = make(ODIN3_AST_SEQ_BLOCK, 0, body_kids, 3);
    odin3_ast_id always = make(ODIN3_AST_ALWAYS, 0, &block, 1);
    odin3_ast_id null_stmt = make(ODIN3_AST_NULL_STMT, 0, NULL, 0);
    odin3_ast_id gen_kids[3] = {{0}, null_stmt, make(ODIN3_AST_NULL_STMT, 0, NULL, 0)};
    odin3_ast_id gen = make(ODIN3_AST_GEN_BLOCK, 0, gen_kids, 3);
    odin3_ast_id mod_kids[5] = {{0}, {0}, tr.cont, always, gen};
    tr.module = make(ODIN3_AST_MODULE, intern("m"), mod_kids, 5);
    tr.unit = make(ODIN3_AST_UNIT, intern("f.v"), &tr.module, 1);
    TEST_ASSERT_EQUAL_UINT32(TREE_NODES, tr.unit.v); /* the 30-node store of the brief */
    return tr;
}

/* --- tests --------------------------------------------------------------------------------- */

/* Every accessor on an ID that is 0 or not made: NONE / 0 / empty, never a fault. */
static void assert_out_of_range(odin3_ast_id bad) {
    TEST_ASSERT_EQUAL_PTR(NULL, odin3_ast_number_of(ast, bad));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_attrs(ast, bad).n);
    TEST_ASSERT_EQUAL_PTR(NULL, odin3_ast_attrs(ast, bad).ids);
    TEST_ASSERT_EQUAL_INT(ODIN3_AST_NONE, odin3_ast_kind_of(ast, bad));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_children(ast, bad).n);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_child(ast, bad, 0).v);
    TEST_ASSERT_EQUAL_STRING("", odin3_ast_name_str(ast, bad));
    TEST_ASSERT_EQUAL_STRING("", odin3_ast_sub_name(ast, bad));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_payload(ast, bad));
    TEST_ASSERT_EQUAL_INT(1, odin3_ast_real(ast, bad) == 0.0);
    TEST_ASSERT_EQUAL_PTR(NULL, odin3_ast_text(ast, bad).ptr);
}

static void test_out_of_range_accessors(void) {
    build_tree();
    assert_out_of_range((odin3_ast_id){0});
    assert_out_of_range((odin3_ast_id){TREE_NODES + 1});
    assert_out_of_range((odin3_ast_id){UINT32_MAX});
    TEST_ASSERT_NULL(odin3_ast_number_of(NULL, (odin3_ast_id){1}));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_attrs(NULL, (odin3_ast_id){1}).n);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_comments_of(NULL, (odin3_srcbuf_id){1}).n);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_comments_in(NULL, (odin3_range){{1}, {2}}).n);
    /* a non-NUMBER node has no number; a node without attributes an empty view */
    TEST_ASSERT_NULL(odin3_ast_number_of(ast, (odin3_ast_id){1}));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_attrs(ast, (odin3_ast_id){1}).n);
}

/* How many events of log name node. */
static uint32_t occurrences(const event_log *log, uint32_t node) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < log->n; i++) {
        count += log->node[i] == node ? 1 : 0;
    }
    return count;
}

static void test_walk_and_cursor_match_reference(void) {
    tree tr = build_tree();
    for (uint32_t id = 1; id <= TREE_NODES; id++) { /* every node as a root, abandoned too */
        check_root((odin3_ast_id){id}, 0);
    }
    /* the UNIT's walk never reaches the abandoned subtree, nor the attribute */
    check_root(tr.unit, 0);
    TEST_ASSERT_EQUAL_UINT32(0, occurrences(&want, tr.abandoned.v));
    TEST_ASSERT_EQUAL_UINT32(0, occurrences(&want, tr.attr.v));
    TEST_ASSERT_EQUAL_UINT32(0, occurrences(&want, 0));
    TEST_ASSERT_EQUAL_UINT32(2 * (TREE_NODES - 7), want.n); /* 5 abandoned, attr and its value */
    TEST_ASSERT_EQUAL_UINT32(ODIN3_AST_ENTER, got.ev[0]);
    TEST_ASSERT_EQUAL_UINT32(tr.unit.v, got.node[0]);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_AST_LEAVE, got.ev[got.n - 1]);
    TEST_ASSERT_EQUAL_UINT32(tr.unit.v, got.node[got.n - 1]);
    /* the same after finish */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_finish(ast));
    check_root(tr.unit, 0);
    check_root(tr.abandoned, 0);
}

static void test_skip_prunes(void) {
    tree tr = build_tree();
    check_root(tr.unit, tr.if_stmt.v);
    TEST_ASSERT_EQUAL_UINT32(2, occurrences(&got, tr.if_stmt.v));
    for (uint32_t below = tr.if_stmt.v - 4; below < tr.if_stmt.v; below++) { /* its subtree */
        TEST_ASSERT_EQUAL_UINT32(0, occurrences(&got, below));
    }
    check_root(tr.unit, tr.module.v); /* ENTER UNIT, ENTER m, LEAVE m, LEAVE UNIT */
    TEST_ASSERT_EQUAL_UINT32(4, got.n);
    check_root(tr.unit, tr.unit.v);
    TEST_ASSERT_EQUAL_UINT32(2, got.n);
    check_root(tr.sel, tr.sel.v);
}

/* The cursor's next event must be want; returns its node. */
static odin3_ast_id expect_event(odin3_ast_cursor *cur, odin3_ast_event want_ev) {
    odin3_ast_event ev = 0;
    odin3_ast_id node = {0};
    TEST_ASSERT_EQUAL_INT(1, odin3_ast_cursor_next(cur, &ev, &node));
    TEST_ASSERT_EQUAL_INT(want_ev, ev);
    return node;
}

static void test_cursor_skip_only_after_enter(void) {
    tree tr = build_tree();
    odin3_ast_cursor cur;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_cursor_init(&cur, ast, tr.cont));
    odin3_ast_cursor_skip(&cur); /* before the first event: no-op */
    TEST_ASSERT_EQUAL_UINT32(tr.cont.v, expect_event(&cur, ODIN3_AST_ENTER).v);
    expect_event(&cur, ODIN3_AST_ENTER); /* NET_ASSIGN */
    expect_event(&cur, ODIN3_AST_ENTER); /* y */
    expect_event(&cur, ODIN3_AST_LEAVE); /* y */
    odin3_ast_cursor_skip(&cur);         /* after a LEAVE: no-op, the sum is still visited */
    odin3_ast_id sum = expect_event(&cur, ODIN3_AST_ENTER);
    TEST_ASSERT_EQUAL_INT(ODIN3_AST_BINARY, odin3_ast_kind_of(ast, sum));
    odin3_ast_cursor_skip(&cur); /* after an ENTER: its LEAVE comes next */
    TEST_ASSERT_EQUAL_UINT32(sum.v, expect_event(&cur, ODIN3_AST_LEAVE).v);
    odin3_ast_cursor_free(&cur);
    odin3_ast_event ev = 0;
    odin3_ast_id node = {0};
    TEST_ASSERT_EQUAL_INT(0, odin3_ast_cursor_next(&cur, &ev, &node));
    odin3_ast_cursor_skip(&cur);
    odin3_ast_cursor_skip(NULL);
    odin3_ast_cursor_free(NULL);
}

static void test_misuse(void) {
    tree tr = build_tree();
    odin3_ast_cursor cur;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_walk(NULL, tr.unit, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_ast_walk(ast, (odin3_ast_id){0}, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_ast_walk(ast, (odin3_ast_id){TREE_NODES + 1}, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_ast_cursor_init(&cur, ast, (odin3_ast_id){TREE_NODES + 1}));
    odin3_ast_event ev = 0;
    odin3_ast_id node = {7};
    TEST_ASSERT_EQUAL_INT(0, odin3_ast_cursor_next(&cur, &ev, &node)); /* an empty cursor */
    TEST_ASSERT_EQUAL_UINT32(7, node.v);
    odin3_ast_cursor_free(&cur);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_cursor_init(NULL, ast, tr.unit));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_cursor_init(&cur, NULL, tr.unit));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_cursor_init(&cur, ast, tr.unit));
    TEST_ASSERT_EQUAL_INT(0, odin3_ast_cursor_next(&cur, NULL, &node));
    TEST_ASSERT_EQUAL_INT(0, odin3_ast_cursor_next(&cur, &ev, NULL));
    TEST_ASSERT_EQUAL_INT(0, odin3_ast_cursor_next(NULL, &ev, &node));
    odin3_ast_cursor_free(&cur);
    TEST_ASSERT_EQUAL_size_t(6, errors_logged);
    /* walks with no callbacks are fine */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_walk(ast, tr.unit, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_walk(ast, tr.unit, on_pre, NULL, &got));
}

static void test_corrupt_height_reported(void) {
    tree tr = build_tree();
    ast->max_height = 2; /* a corrupted record: the tree is deeper */
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_walk(ast, tr.unit, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_size_t(1, errors_logged);
    odin3_ast_cursor cur;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_cursor_init(&cur, ast, tr.unit));
    odin3_ast_event ev = 0;
    odin3_ast_id node = {0};
    uint32_t events = 0;
    while (odin3_ast_cursor_next(&cur, &ev, &node)) {
        events++;
    }
    TEST_ASSERT_TRUE(cur.corrupt);
    TEST_ASSERT_EQUAL_UINT32(2, events); /* UNIT, MODULE entered; then stopped */
    odin3_ast_cursor_free(&cur);
}

/* A deterministic pseudo-random forest: walk and cursor match the reference on every root. */
static void test_random_forest(void) {
    uint64_t seed = 0x2a;
    for (uint32_t i = 0; i < RANDOM_NODES; i++) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        uint32_t end = odin3_ast_node_end(ast).v;
        uint32_t pick = (uint32_t)(seed >> 33);
        if (end < 3 || pick % 4 == 0) {
            ident("v");
            continue;
        }
        odin3_ast_id kids[3] = {{1 + pick % (end - 1)}, {1 + (pick / 7) % (end - 1)}, {0}};
        /* children may be shared: walks follow spans and don't care */
        make(pick % 3 == 0 ? ODIN3_AST_SELECT : ODIN3_AST_LIST, 0, kids, pick % 3 == 0 ? 3 : 2);
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_finish(ast));
    for (uint32_t id = RANDOM_NODES - 50; id <= RANDOM_NODES; id++) {
        check_root((odin3_ast_id){id}, 0);
        check_root((odin3_ast_id){id}, id - 1);
    }
}

/* The depth-cap chain: the walk's only allocation is its pre-sized stack. */
static uint32_t visits;

static odin3_ast_walk_action count_pre(const odin3_ast *store, odin3_ast_id node, void *user) {
    (void)store;
    (void)node;
    (void)user;
    visits++;
    return ODIN3_AST_WALK_CONTINUE;
}

static void test_chain_at_cap(void) {
    odin3_ast_spec unary = {.kind = ODIN3_AST_UNARY, .sub = ODIN3_AST_UNARY_MINUS};
    odin3_ast_id top = ident("x");
    for (uint32_t height = 2; height <= ODIN3_AST_MAX_DEPTH; height++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_make(ast, &unary, &top, 1, &top));
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_finish(ast));
    visits = 0;
    odin3_util_set_alloc_fail_after(0);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, odin3_ast_walk(ast, top, count_pre, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(0, visits); /* reported before any callback */
    odin3_util_set_alloc_fail_after(1);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_walk(ast, top, count_pre, NULL, NULL));
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_AST_MAX_DEPTH, visits);
    /* the cursor likewise */
    odin3_ast_cursor cur;
    odin3_util_set_alloc_fail_after(0);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, odin3_ast_cursor_init(&cur, ast, top));
    odin3_ast_cursor_free(&cur);
    odin3_util_set_alloc_fail_after(1);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_cursor_init(&cur, ast, top));
    odin3_ast_event ev = 0;
    odin3_ast_id node = {0};
    uint32_t leaves = 0;
    while (odin3_ast_cursor_next(&cur, &ev, &node)) {
        leaves += ev == ODIN3_AST_LEAVE;
    }
    odin3_util_set_alloc_fail_after(-1);
    odin3_ast_cursor_free(&cur);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_AST_MAX_DEPTH, leaves);
    TEST_ASSERT_EQUAL_UINT32(top.v, node.v); /* the root leaves last */
}

static void test_parent_index(void) {
    tree tr = build_tree();
    odin3_ast_parent_index ix = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_parent_index_build(ast, &ix));
    for (uint32_t id = 1; id <= TREE_NODES; id++) { /* every node, against the spans */
        odin3_ast_id node = {id};
        for (uint32_t i = 0; i < odin3_ast_nchild(ast, node); i++) {
            odin3_ast_id child = odin3_ast_child(ast, node, i);
            if (child.v != 0) {
                TEST_ASSERT_EQUAL_UINT32(id, odin3_ast_parent(&ix, child).v);
            }
        }
    }
    TEST_ASSERT_EQUAL_UINT32(tr.cont.v, odin3_ast_parent(&ix, tr.attr).v); /* the annotated node */
    TEST_ASSERT_EQUAL_UINT32(tr.attr.v, odin3_ast_parent(&ix, (odin3_ast_id){tr.attr.v - 1}).v);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_parent(&ix, tr.unit).v);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_parent(&ix, tr.abandoned).v);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_parent(&ix, (odin3_ast_id){0}).v);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_parent(&ix, (odin3_ast_id){TREE_NODES + 1}).v);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_parent(NULL, tr.attr).v);
    odin3_ast_parent_index_destroy(&ix);
    TEST_ASSERT_NULL(ix.parents);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_parent(&ix, tr.attr).v);
    odin3_ast_parent_index_destroy(&ix);
    odin3_ast_parent_index_destroy(NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_parent_index_build(NULL, &ix));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_parent_index_build(ast, NULL));
}

static void test_parent_index_oom(void) {
    build_tree();
    odin3_ast_parent_index ix = {(odin3_ast_id *)&ix, 77}; /* sentinel: unchanged on failure */
    long points = 0;
    for (; points < OOM_LIMIT; points++) {
        odin3_util_set_alloc_fail_after(points);
        odin3_status st = odin3_ast_parent_index_build(ast, &ix);
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            break;
        }
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        TEST_ASSERT_EQUAL_UINT32(77, ix.n);
    }
    TEST_ASSERT_EQUAL_INT(1, points); /* one array */
    TEST_ASSERT_EQUAL_UINT32(TREE_NODES + 1, ix.n);
    odin3_ast_parent_index_destroy(&ix);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_out_of_range_accessors);
    RUN_TEST(test_walk_and_cursor_match_reference);
    RUN_TEST(test_skip_prunes);
    RUN_TEST(test_cursor_skip_only_after_enter);
    RUN_TEST(test_misuse);
    RUN_TEST(test_corrupt_height_reported);
    RUN_TEST(test_random_forest);
    RUN_TEST(test_chain_at_cap);
    RUN_TEST(test_parent_index);
    RUN_TEST(test_parent_index_oom);
    return UNITY_END();
}
