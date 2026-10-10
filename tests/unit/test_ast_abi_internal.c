/*
 * test_ast_abi_internal.c — what the ABI test cannot see through odin3.h alone: the stored form
 * of odin3_ast_number_new_str, keep levels through odin3_design_drop_ast, and OOM sweeps of every
 * allocating ABI call (the failure hook is hidden from libodin3.so).
 */
#include "ast/ast.h"
#include "ast/ast_internal.h"
#include "ast/srcman.h"
#include "ir/design.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/arena.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { OOM_LIMIT = 64, RUN = 3, FILE_BYTES = 40 };

static odin3_design *design;
static odin3_srcman *sm;
static odin3_ast *ast;
static uint32_t file_buf;

void setUp(void) {
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_srcman(design, &sm));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_new_ast(design, ODIN3_AST_FORM_PARSED, RUN, &ast));
    uint32_t name = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_build_intern(ast, "f.v", &name));
    odin3_srcfile_spec spec = {.name = name, .len = FILE_BYTES};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_build_file(sm, &spec, &file_buf));
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_design_destroy(design);
    design = NULL;
}

static uint32_t ident(const char *name) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_build_intern(ast, name, &str));
    odin3_ast_spec spec = {.kind = ODIN3_AST_IDENT, .name = str};
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_build_node(ast, &spec, NULL, 0, &id));
    return id;
}

/* --- stored forms ---------------------------------------------------------------------------- */

/* The NUMBER payload of a node made from bits (MSB first) and width. */
static const odin3_ast_number *number_of(const char *bits, uint32_t width) {
    uint32_t payload = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_number_new_str(ast, 1, width, bits, &payload));
    odin3_ast_spec spec = {
        .kind = ODIN3_AST_NUMBER, .sub = ODIN3_AST_NUMBER_BIN, .payload = payload};
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_build_node(ast, &spec, NULL, 0, &id));
    const odin3_ast_number *num = odin3_ast_number_of(ast, (odin3_ast_id){id});
    TEST_ASSERT_NOT_NULL(num);
    return num;
}

static void test_number_new_str_stores_lsb_first(void) {
    const odin3_ast_number *num = number_of("10x?", 4);
    TEST_ASSERT_EQUAL_UINT32(4, num->width);
    TEST_ASSERT_EQUAL_UINT32(4, num->nbits);
    static const uint8_t want[] = {ODIN3_BIT_Z, ODIN3_BIT_X, ODIN3_BIT_0, ODIN3_BIT_1};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, num->bits, 4);
    TEST_ASSERT_TRUE(num->has_xz);
    TEST_ASSERT_TRUE(num->has_question);
    TEST_ASSERT_FALSE(num->warned);
}

static void test_number_new_str_plain_bits(void) {
    const odin3_ast_number *num = number_of("0110", 0);
    TEST_ASSERT_FALSE(num->has_xz);
    TEST_ASSERT_FALSE(num->has_question);
    TEST_ASSERT_EQUAL_UINT32(0, num->width);
    TEST_ASSERT_EQUAL_UINT32(4, num->nbits);
}

/* --keep-ast=all is ODIN3_KEEP_AST_ALL through the ABI setter: both forms survive the drop. */
static void test_keep_all_keeps_both_forms_through_drop(void) {
    odin3_ast *elab = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_new_ast(design, ODIN3_AST_FORM_ELABORATED, RUN, &elab));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_set_keep_ast(design, ODIN3_KEEP_AST_ALL));
    odin3_design_drop_ast(design, (odin3_passrun_id){RUN}, ODIN3_AST_FORM_PARSED);
    odin3_design_drop_ast(design, (odin3_passrun_id){RUN}, ODIN3_AST_FORM_ELABORATED);
    odin3_ast *found = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_get_ast(design, RUN, ODIN3_AST_FORM_PARSED, &found));
    TEST_ASSERT_EQUAL_PTR(ast, found);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_get_ast(design, RUN, ODIN3_AST_FORM_ELABORATED, &found));
    TEST_ASSERT_EQUAL_PTR(elab, found);
    /* --keep-ast (ELABORATED): the parsed store goes */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_set_keep_ast(design, ODIN3_KEEP_AST_ELABORATED));
    odin3_design_drop_ast(design, (odin3_passrun_id){RUN}, ODIN3_AST_FORM_PARSED);
    odin3_design_drop_ast(design, (odin3_passrun_id){RUN}, ODIN3_AST_FORM_ELABORATED);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_get_ast(design, RUN, ODIN3_AST_FORM_PARSED, &found));
    TEST_ASSERT_NULL(found);
    uint32_t count = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_ast_count(design, &count));
    TEST_ASSERT_EQUAL_UINT32(1, count);
}

/* --- OOM ------------------------------------------------------------------------------------- */

/* What a failed call must leave as it was. */
typedef struct snapshot {
    uint32_t node_end, mark, asts, attrs_on_1;
    size_t children, payloads, comments, arena, bufs, lines, segs;
} snapshot;

static snapshot snap(void) {
    const odin3_srcfile_rec *file = odin3_vec_cat(&sm->files, 1);
    return (snapshot){.node_end = odin3_ast_node_end(ast).v,
                      .mark = odin3_ast_mark(ast),
                      .asts = odin3_design_ast_count(design),
                      .attrs_on_1 = odin3_ast_attrs(ast, (odin3_ast_id){1}).n,
                      .children = ast->children.len,
                      .payloads = ast->payloads.len,
                      .comments = ast->comments.len,
                      .arena = odin3_arena_bytes_used(ast->arena),
                      .bufs = sm->bufs.len,
                      .lines = file->lines.len,
                      .segs = file->segs.len};
}

static void assert_same(snapshot want) {
    snapshot got = snap();
    TEST_ASSERT_EQUAL_UINT32(want.node_end, got.node_end);
    TEST_ASSERT_EQUAL_UINT32(want.mark, got.mark);
    TEST_ASSERT_EQUAL_UINT32(want.asts, got.asts);
    TEST_ASSERT_EQUAL_UINT32(want.attrs_on_1, got.attrs_on_1);
    TEST_ASSERT_EQUAL_size_t(want.children, got.children);
    TEST_ASSERT_EQUAL_size_t(want.payloads, got.payloads);
    TEST_ASSERT_EQUAL_size_t(want.comments, got.comments);
    TEST_ASSERT_EQUAL_size_t(want.arena, got.arena);
    TEST_ASSERT_EQUAL_size_t(want.bufs, got.bufs);
    TEST_ASSERT_EQUAL_size_t(want.lines, got.lines);
    TEST_ASSERT_EQUAL_size_t(want.segs, got.segs);
}

typedef odin3_status (*oom_step)(void);

static void no_prep(void) {
}

static void prep_leaves(void) {
    (void)ident("a");
    (void)ident("b");
}

/* An ATTR node (ID 3) to attach to node 1. */
static void prep_attr(void) {
    prep_leaves();
    uint32_t key = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_build_intern(ast, "keep", &key));
    odin3_ast_spec spec = {.kind = ODIN3_AST_ATTR, .name = key};
    uint32_t none[] = {0};
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_build_node(ast, &spec, none, 1, &id));
    TEST_ASSERT_EQUAL_UINT32(3, id);
}

/* A NUMBER node (ID 1) whose bits get_number_str reads. */
static void prep_number(void) {
    uint32_t payload = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_number_new_str(ast, 1, 0, "1x0z1", &payload));
    odin3_ast_spec spec = {.kind = ODIN3_AST_NUMBER, .payload = payload};
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_build_node(ast, &spec, NULL, 0, &id));
}

static odin3_status step_new_ast(void) {
    odin3_ast *made = NULL;
    return odin3_design_new_ast(design, ODIN3_AST_FORM_ELABORATED, RUN, &made);
}

static odin3_status step_build_node(void) {
    odin3_ast_spec spec = {.kind = ODIN3_AST_BINARY, .sub = ODIN3_AST_BINARY_ADD};
    uint32_t kids[] = {1, 2};
    uint32_t id = 0;
    return odin3_ast_build_node(ast, &spec, kids, 2, &id);
}

static odin3_status step_build_marked(void) {
    odin3_status st = odin3_ast_build_push(ast, 1);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_ast_spec spec = {.kind = ODIN3_AST_CONCAT};
    uint32_t id = 0;
    st = odin3_ast_build_marked(ast, &spec, 0, &id);
    if (st != ODIN3_OK) {
        (void)odin3_ast_build_unwind(ast, 0); /* the push succeeded: undo it for the snapshot */
    }
    return st;
}

static odin3_status step_number(void) {
    uint32_t payload = 0;
    return odin3_ast_number_new_str(ast, 1, 8, "1010x?zz", &payload);
}

static odin3_status step_number_str(void) {
    const char *bits = NULL;
    uint32_t width = 0;
    return odin3_ast_get_number_str(ast, 1, &bits, &width);
}

static odin3_status step_attach(void) {
    uint32_t attrs[] = {3};
    return odin3_ast_build_attach(ast, 1, attrs, 1);
}

static odin3_status step_text(void) {
    static const char big[1U << 17] = {0}; /* larger than an arena chunk: its own allocation */
    uint32_t payload = 0;
    return odin3_ast_build_text(ast, big, sizeof big, &payload);
}

static odin3_status step_real(void) {
    uint32_t payload = 0;
    return odin3_ast_build_real(ast, 0.5, &payload);
}

static odin3_status step_intern(void) {
    static char big[1U << 17]; /* larger than a strtab arena chunk: its own allocation */
    if (big[0] == '\0') {
        memset(big, 'n', sizeof big - 1);
    }
    uint32_t name = 0;
    return odin3_ast_build_intern(ast, big, &name);
}

static odin3_status step_comment(void) {
    uint32_t loc = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_get_loc(sm, file_buf, 2, &loc));
    odin3_ast_comment_spec spec = {
        .loc = loc, .kind = ODIN3_AST_COMMENT_LINE, .text_ptr = "// c", .text_len = 4};
    return odin3_ast_build_comment(ast, &spec);
}

/* Fills the buffer table to its capacity: the next buffer must grow it. */
static void prep_full_bufs(void) {
    uint32_t parent = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_get_loc(sm, file_buf, 1, &parent));
    while (sm->bufs.len < sm->bufs.cap) {
        odin3_expansion_spec spec = {
            .kind = ODIN3_SRCBUF_EXPANSION, .name = 1, .parent = parent, .def = parent, .len = 1};
        uint32_t buf = 0;
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_build_expansion(sm, &spec, &buf));
    }
}

static odin3_status step_file(void) {
    odin3_srcfile_spec spec = {.name = 1, .len = 5};
    uint32_t buf = 0;
    return odin3_srcman_build_file(sm, &spec, &buf);
}

static odin3_status step_expansion(void) {
    uint32_t parent = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_get_loc(sm, file_buf, 3, &parent));
    odin3_expansion_spec spec = {
        .kind = ODIN3_SRCBUF_EXPANSION, .name = 1, .parent = parent, .def = parent, .len = 2};
    uint32_t buf = 0;
    return odin3_srcman_build_expansion(sm, &spec, &buf);
}

static odin3_status step_line(void) {
    return odin3_srcman_build_line(sm, file_buf, 7);
}

static odin3_status step_segment(void) {
    odin3_segment seg = {.stream = file_buf, .out_offset = 0, .loc = 1};
    return odin3_srcman_build_segment(sm, &seg);
}

static odin3_status step_format(void) {
    const char *text = NULL;
    return odin3_srcman_get_format(sm, 1, &text);
}

/*
 * Runs step at every failure point, each on a fresh design prepared without failures: every
 * failure is NO_MEMORY and changes nothing; returns the number of failure points.
 */
static long oom_sweep(oom_step step, void (*prepare)(void)) {
    for (long k = 0; k < OOM_LIMIT; k++) {
        tearDown();
        setUp();
        prepare();
        snapshot before = snap();
        odin3_util_set_alloc_fail_after(k);
        odin3_status st = step();
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            return k;
        }
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        assert_same(before);
    }
    TEST_FAIL_MESSAGE("no success within OOM_LIMIT");
    return 0;
}

static void test_oom_ast_builders(void) {
    TEST_ASSERT_GREATER_OR_EQUAL(1, oom_sweep(step_new_ast, no_prep));
    TEST_ASSERT_GREATER_OR_EQUAL(1, oom_sweep(step_build_node, prep_leaves));
    TEST_ASSERT_GREATER_OR_EQUAL(1, oom_sweep(step_build_marked, prep_leaves));
    TEST_ASSERT_GREATER_OR_EQUAL(2, oom_sweep(step_number, no_prep));   /* bits buffer, payload */
    TEST_ASSERT_GREATER_OR_EQUAL(2, oom_sweep(step_attach, prep_attr)); /* ID copy, map */
    TEST_ASSERT_GREATER_OR_EQUAL(1, oom_sweep(step_text, no_prep));
    TEST_ASSERT_GREATER_OR_EQUAL(1, oom_sweep(step_real, no_prep));
    TEST_ASSERT_GREATER_OR_EQUAL(1, oom_sweep(step_intern, no_prep));
    TEST_ASSERT_GREATER_OR_EQUAL(1, oom_sweep(step_comment, no_prep));
    TEST_ASSERT_GREATER_OR_EQUAL(1, oom_sweep(step_number_str, prep_number)); /* text, intern */
}

static void test_oom_srcman(void) {
    TEST_ASSERT_GREATER_OR_EQUAL(1, oom_sweep(step_file, prep_full_bufs));
    TEST_ASSERT_GREATER_OR_EQUAL(1, oom_sweep(step_expansion, prep_full_bufs));
    TEST_ASSERT_GREATER_OR_EQUAL(1, oom_sweep(step_line, no_prep));
    TEST_ASSERT_GREATER_OR_EQUAL(1, oom_sweep(step_segment, no_prep));
    TEST_ASSERT_GREATER_OR_EQUAL(1, oom_sweep(step_format, no_prep));
}

static void test_oom_get_srcman(void) {
    long points = 0;
    for (; points < OOM_LIMIT; points++) {
        odin3_design *fresh = odin3_design_create();
        TEST_ASSERT_NOT_NULL(fresh);
        odin3_srcman *got = NULL;
        odin3_util_set_alloc_fail_after(points);
        odin3_status st = odin3_design_get_srcman(fresh, &got);
        odin3_util_set_alloc_fail_after(-1);
        odin3_design_destroy(fresh);
        if (st == ODIN3_OK) {
            break;
        }
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        TEST_ASSERT_NULL(got);
    }
    TEST_ASSERT_GREATER_OR_EQUAL(1, points);
    TEST_ASSERT_LESS_THAN(OOM_LIMIT, points);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_number_new_str_stores_lsb_first);
    RUN_TEST(test_number_new_str_plain_bits);
    RUN_TEST(test_keep_all_keeps_both_forms_through_drop);
    RUN_TEST(test_oom_ast_builders);
    RUN_TEST(test_oom_srcman);
    RUN_TEST(test_oom_get_srcman);
    return UNITY_END();
}
