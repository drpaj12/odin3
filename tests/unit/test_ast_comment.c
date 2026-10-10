/*
 * test_ast_comment.c — unit tests for the comment side table: FILE-only locations, the sort by
 * loc at finish (an included file's comments arrive between the includer's), comments_in over
 * expansion ranges (never across files), comments_of per buffer, field checks, and OOM.
 */
#include "ast/ast.h"
#include "ast/ast_internal.h"
#include "ast/srcman.h"
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
    TOP_LEN = 100,
    INC_LEN = 40,
    OOM_LIMIT = 16,
    BIG_TEXT = 1 << 17, /* larger than an arena chunk: its own allocation */
    RANDOM_COMMENTS = 500,
};

static odin3_design *design;
static odin3_ast *ast;
static odin3_srcman *sm;
static odin3_srcbuf_id top, inc, exp_buf, arg_buf;
static size_t errors_logged;

static void count_sink(odin3_log_level level, const char *msg, void *user) {
    (void)msg;
    (void)user;
    errors_logged += level == ODIN3_LOG_ERROR;
}

static uint32_t intern(const char *str) {
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_intern(ast, odin3_bytes_cstr(str), &id));
    return id;
}

static uint32_t top_at(uint32_t offset) {
    return odin3_srcman_loc(sm, top, offset).v;
}

static uint32_t inc_at(uint32_t offset) {
    return odin3_srcman_loc(sm, inc, offset).v;
}

/* top.v (100 bytes) includes inc.v (40 bytes) at offset 30; a macro use M(ab) at 40..50. */
static void add_buffers(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_srcman(design, &sm));
    odin3_srcfile_spec file = {.name = intern("top.v"), .len = TOP_LEN};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_add_file(sm, &file, &top));
    file = (odin3_srcfile_spec){.name = intern("inc.v"), .parent = top_at(30), .len = INC_LEN};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_add_file(sm, &file, &inc));
    odin3_expansion_spec exp = {.kind = ODIN3_SRCBUF_EXPANSION,
                                .name = intern("M"),
                                .parent = top_at(40),
                                .parent_end = top_at(50),
                                .def = top_at(2),
                                .len = 5};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_add_expansion(sm, &exp, &exp_buf));
    odin3_expansion_spec arg = {.kind = ODIN3_SRCBUF_MACRO_ARG,
                                .name = intern("M"),
                                .parent = odin3_srcman_loc(sm, exp_buf, 1).v,
                                .def = top_at(43),
                                .len = 2};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_add_expansion(sm, &arg, &arg_buf));
}

void setUp(void) {
    odin3_log_set_sink(count_sink, NULL);
    errors_logged = 0;
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    ast = NULL;
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_ast_create(design, ODIN3_AST_FORM_PARSED, (odin3_passrun_id){1}, &ast));
    add_buffers();
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
    odin3_ast_destroy(ast);
    ast = NULL;
    odin3_design_destroy(design);
    design = NULL;
}

static odin3_ast_comment_spec spec_at(uint32_t loc, const char *text) {
    return (odin3_ast_comment_spec){.loc = loc,
                                    .end = loc + (uint32_t)strlen(text),
                                    .kind = ODIN3_AST_COMMENT_LINE,
                                    .text_ptr = text,
                                    .text_len = (uint32_t)strlen(text)};
}

static void add(uint32_t loc, const char *text) {
    odin3_ast_comment_spec spec = spec_at(loc, text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_comment_add(ast, &spec));
}

/* The comments in the span, by text, joined with '|'. */
static const char *texts(odin3_ast_comment_span span) {
    static char joined[256];
    joined[0] = '\0';
    for (uint32_t i = 0; i < span.n; i++) {
        if (i > 0) {
            strcat(joined, "|");
        }
        strcat(joined, span.items[i].text);
    }
    return joined;
}

static odin3_range range(uint32_t loc, uint32_t end) {
    return (odin3_range){{loc}, {end}};
}

/* The includer's comments, the included file's, then the includer's later ones. */
static void add_stream(void) {
    add(top_at(5), "// a");
    add(top_at(20), "// b");
    add(inc_at(0), "// i0");
    add(inc_at(10), "// i1");
    add(top_at(45), "/*m*/");
    add(top_at(60), "// c");
}

static void test_sorted_at_finish(void) {
    add_stream();
    /* before finish: stream order, and the queries are empty */
    const odin3_ast_comment *raw = ast->comments.data;
    TEST_ASSERT_EQUAL_STRING("// i0", raw[2].text);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_comments_in(ast, range(top_at(0), top_at(99))).n);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_comments_of(ast, top).n);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_finish(ast));
    raw = ast->comments.data;
    for (size_t i = 1; i < ast->comments.len; i++) {
        TEST_ASSERT_TRUE(raw[i - 1].loc.v < raw[i].loc.v);
    }
    TEST_ASSERT_EQUAL_STRING("// a|// b|/*m*/|// c", texts(odin3_ast_comments_of(ast, top)));
    TEST_ASSERT_EQUAL_STRING("// i0|// i1", texts(odin3_ast_comments_of(ast, inc)));
}

static void test_comments_in(void) {
    add_stream();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_finish(ast));
    /* an includer range never covers the included file's comments */
    TEST_ASSERT_EQUAL_STRING("// a|// b|/*m*/|// c",
                             texts(odin3_ast_comments_in(ast, range(top_at(0), top_at(TOP_LEN)))));
    /* [loc, end): a comment starting at end is outside */
    TEST_ASSERT_EQUAL_STRING("// b",
                             texts(odin3_ast_comments_in(ast, range(top_at(20), top_at(45)))));
    TEST_ASSERT_EQUAL_STRING("// b|/*m*/",
                             texts(odin3_ast_comments_in(ast, range(top_at(6), top_at(46)))));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_comments_in(ast, range(top_at(6), top_at(20))).n);
    TEST_ASSERT_EQUAL_STRING("// i1",
                             texts(odin3_ast_comments_in(ast, range(inc_at(1), inc_at(INC_LEN)))));
    /* a range inside the macro use maps to the use site 40..50 */
    uint32_t in_body = odin3_srcman_loc(sm, exp_buf, 0).v;
    TEST_ASSERT_EQUAL_STRING("/*m*/",
                             texts(odin3_ast_comments_in(ast, range(in_body, in_body + 3))));
    /* not in one file: empty */
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_comments_in(ast, range(top_at(0), inc_at(20))).n);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_comments_in(ast, range(top_at(0), 0)).n);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_comments_in(ast, range(top_at(60), top_at(10))).n);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_comments_in(ast, range(0, 0)).n);
}

static void test_comments_of_other_buffers(void) {
    add_stream();
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_finish(ast));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_comments_of(ast, exp_buf).n);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_comments_of(ast, arg_buf).n);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_comments_of(ast, (odin3_srcbuf_id){0}).n);
    TEST_ASSERT_EQUAL_UINT32(0, odin3_ast_comments_of(ast, (odin3_srcbuf_id){99}).n);
}

static void test_fields_and_copy(void) {
    char text[] = "// synopsys full_case";
    odin3_ast_comment_spec spec = spec_at(top_at(70), text);
    spec.flags = ODIN3_AST_COMMENT_METACOMMENT;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_comment_add(ast, &spec));
    text[0] = '#'; /* copied, not borrowed */
    odin3_ast_comment_spec block = {.loc = top_at(80), .kind = ODIN3_AST_COMMENT_BLOCK};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_comment_add(ast, &block)); /* empty, no end */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_finish(ast));
    odin3_ast_comment_span span = odin3_ast_comments_of(ast, top);
    TEST_ASSERT_EQUAL_UINT32(2, span.n);
    TEST_ASSERT_EQUAL_STRING("// synopsys full_case", span.items[0].text);
    TEST_ASSERT_EQUAL_UINT32(strlen(text), span.items[0].len);
    TEST_ASSERT_EQUAL_UINT32(top_at(70), span.items[0].loc.v);
    TEST_ASSERT_EQUAL_UINT32(spec.end, span.items[0].end.v);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_AST_COMMENT_LINE, span.items[0].kind);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_AST_COMMENT_METACOMMENT, span.items[0].flags);
    TEST_ASSERT_EQUAL_STRING("", span.items[1].text);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_AST_COMMENT_BLOCK, span.items[1].kind);
    TEST_ASSERT_EQUAL_UINT32(0, span.items[1].end.v);
}

static void expect_rejected(odin3_ast_comment_spec spec) {
    size_t before = ast->comments.len;
    size_t errors = errors_logged;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_comment_add(ast, &spec));
    TEST_ASSERT_EQUAL_size_t(before, ast->comments.len);
    TEST_ASSERT_EQUAL_size_t(errors + 1, errors_logged);
}

static void test_rejections(void) {
    odin3_ast_comment_spec good = spec_at(top_at(5), "// a");
    odin3_ast_comment_spec spec = good;
    spec.loc = odin3_srcman_loc(sm, arg_buf, 0).v; /* a MACRO_ARG loc */
    spec.end = spec.loc + 1;
    expect_rejected(spec);
    spec.loc = odin3_srcman_loc(sm, exp_buf, 0).v; /* an EXPANSION loc */
    expect_rejected(spec);
    spec = good;
    spec.loc = 0;
    expect_rejected(spec);
    spec.loc = UINT32_MAX - 1; /* past the space allocated so far */
    expect_rejected(spec);
    spec = good;
    spec.end = top_at(4); /* before loc */
    expect_rejected(spec);
    spec.end = inc_at(1); /* another buffer */
    expect_rejected(spec);
    spec = good;
    spec.kind = 0;
    expect_rejected(spec);
    spec.kind = ODIN3_AST_COMMENT_BLOCK + 1;
    expect_rejected(spec);
    spec = good;
    spec.flags = 2;
    expect_rejected(spec);
    spec = good;
    spec.reserved[1] = 1;
    expect_rejected(spec);
    spec = good;
    spec.text_ptr = NULL;
    expect_rejected(spec);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_comment_add(ast, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_comment_add(NULL, &good));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_finish(ast));
    expect_rejected(good); /* sealed */
}

/* A deterministic shuffle of comments over both files sorts by loc; equal records stay. */
static void test_random_order(void) {
    static const char *const names[] = {"// 0", "// 1", "// 2", "// 3"};
    uint64_t rng = 7;
    for (uint32_t i = 0; i < RANDOM_COMMENTS; i++) {
        rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
        uint32_t pick = (uint32_t)(rng >> 33);
        uint32_t loc = pick % 2 == 0 ? top_at(pick % TOP_LEN) : inc_at(pick % INC_LEN);
        odin3_ast_comment_spec spec = spec_at(loc, names[(pick / 3) % 4]);
        spec.end = 0;
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_comment_add(ast, &spec));
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_finish(ast));
    const odin3_ast_comment *items = ast->comments.data;
    TEST_ASSERT_EQUAL_size_t(RANDOM_COMMENTS, ast->comments.len);
    for (size_t i = 1; i < ast->comments.len; i++) {
        TEST_ASSERT_TRUE(items[i - 1].loc.v < items[i].loc.v ||
                         (items[i - 1].loc.v == items[i].loc.v &&
                          strcmp(items[i - 1].text, items[i].text) <= 0));
    }
    uint32_t total = odin3_ast_comments_of(ast, top).n + odin3_ast_comments_of(ast, inc).n;
    TEST_ASSERT_EQUAL_UINT32(RANDOM_COMMENTS, total);
}

/* --- OOM ----------------------------------------------------------------------------------- */

static char big[BIG_TEXT];

static void fill_comments(void) {
    while (ast->comments.len < ast->comments.cap || ast->comments.cap == 0) {
        add(top_at(1), "// f");
    }
}

static void test_oom(void) {
    memset(big, 'c', sizeof big);
    long k = 0;
    for (; k < OOM_LIMIT; k++) {
        tearDown();
        setUp();
        fill_comments();
        size_t comments = ast->comments.len;
        size_t arena = odin3_arena_bytes_used(ast->arena);
        odin3_ast_comment_spec spec = {.loc = top_at(9),
                                       .kind = ODIN3_AST_COMMENT_BLOCK,
                                       .text_ptr = big,
                                       .text_len = BIG_TEXT};
        odin3_util_set_alloc_fail_after(k);
        odin3_status st = odin3_ast_comment_add(ast, &spec);
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            break;
        }
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        TEST_ASSERT_EQUAL_size_t(comments, ast->comments.len);
        TEST_ASSERT_EQUAL_size_t(arena, odin3_arena_bytes_used(ast->arena));
    }
    TEST_ASSERT_EQUAL_INT(2, k); /* comment table, arena block */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_finish(ast));
    TEST_ASSERT_EQUAL_UINT32(BIG_TEXT,
                             odin3_ast_comments_in(ast, range(top_at(9), top_at(10))).items[0].len);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_sorted_at_finish);
    RUN_TEST(test_comments_in);
    RUN_TEST(test_comments_of_other_buffers);
    RUN_TEST(test_fields_and_copy);
    RUN_TEST(test_rejections);
    RUN_TEST(test_random_order);
    RUN_TEST(test_oom);
    return UNITY_END();
}
