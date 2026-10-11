/*
 * test_ast_dump.c — unit tests for the s-expression dump: goldens with file:line:col, absent
 * slots, sub-kinds, names, payloads and attribute groups; the depth-cap chain (iterative, with
 * capped indentation); abandoned roots; misuse; OOM.
 */
#include "ast/ast.h"
#include "ast/ast_internal.h"
#include "ast/kinds.h"
#include "ast/print.h"
#include "ast/srcman.h"
#include "ir/design.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { OOM_LIMIT = 200, FILE_LEN = 100 };

static odin3_design *design;
static odin3_ast *ast;
static odin3_srcman *sm;
static odin3_srcbuf_id file;
static odin3_strbuf out;
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

/* t.v: 100 bytes, lines starting at 0, 20 and 40. */
static void add_file(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_srcman(design, &sm));
    odin3_srcfile_spec spec = {.name = intern("t.v"), .len = FILE_LEN};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_add_file(sm, &spec, &file));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_add_line(sm, file, 20));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_add_line(sm, file, 40));
}

void setUp(void) {
    odin3_log_set_sink(count_sink, NULL);
    errors_logged = 0;
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    ast = NULL;
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_ast_create(design, ODIN3_AST_FORM_PARSED, (odin3_passrun_id){1}, &ast));
    add_file();
    odin3_strbuf_init(&out);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
    odin3_strbuf_free(&out);
    odin3_ast_destroy(ast);
    ast = NULL;
    odin3_design_destroy(design);
    design = NULL;
}

static const odin3_ast_id NO = {0};

/* The loc of 1-based line:col in t.v (lines 20 bytes apart). */
static odin3_loc at(uint32_t line, uint32_t col) {
    return odin3_srcman_loc(sm, file, (line - 1) * 20 + col - 1);
}

static odin3_ast_spec spec_at(odin3_ast_kind kind, odin3_loc loc) {
    return (odin3_ast_spec){.kind = (uint8_t)kind, .loc = loc.v};
}

static odin3_ast_spec named_at(odin3_ast_kind kind, const char *name, odin3_loc loc) {
    odin3_ast_spec spec = spec_at(kind, loc);
    spec.name = intern(name);
    return spec;
}

static odin3_ast_id mk(odin3_ast_spec spec, const odin3_ast_id *ids, uint32_t n) {
    odin3_ast_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_make(ast, &spec, ids, n, &id));
    return id;
}

static odin3_ast_id hex_ff(odin3_loc loc) {
    odin3_number_text pieces = {odin3_bytes_cstr("4"), odin3_bytes_cstr("'h"),
                                odin3_bytes_cstr("FF")};
    odin3_ast_spec spec = spec_at(ODIN3_AST_NUMBER, loc);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_number_kind(pieces, &spec.sub, &spec.flags));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_number_parse(ast, loc, pieces, &spec.payload));
    return mk(spec, NULL, 0);
}

static odin3_ast_id three(void) {
    odin3_number_text pieces = {{NULL, 0}, {NULL, 0}, odin3_bytes_cstr("3")};
    odin3_ast_spec spec = spec_at(ODIN3_AST_NUMBER, (odin3_loc){0});
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_number_kind(pieces, &spec.sub, &spec.flags));
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_ast_number_parse(ast, (odin3_loc){spec.loc}, pieces, &spec.payload));
    return mk(spec, NULL, 0);
}

static void expect(odin3_ast_id root, const char *want) {
    odin3_strbuf_clear(&out);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_dump(ast, root, &out));
    TEST_ASSERT_EQUAL_STRING(want, out.data != NULL ? out.data : "");
}

/* (* keep *) wire w;  — absent slots, a sub-kind, a name and an attribute group. */
static void test_declaration_golden(void) {
    odin3_ast_id decl = mk(named_at(ODIN3_AST_DECLARATOR, "w", at(1, 17)), &NO, 1);
    odin3_ast_id wire =
        mk(spec_at(ODIN3_AST_NET_DECL, at(1, 12)), (odin3_ast_id[]){NO, NO, NO, decl}, 4);
    odin3_ast_id keep = mk(named_at(ODIN3_AST_ATTR, "keep", at(1, 4)), &NO, 1);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_attach(ast, wire, &keep, 1));
    expect(wire, "(NET_DECL.wire @t.v:1:12\n"
                 "  _\n"
                 "  _\n"
                 "  _\n"
                 "  (DECLARATOR:w @t.v:1:17\n"
                 "    _)\n"
                 "  {(ATTR:keep @t.v:1:4\n"
                 "     _)})");
}

/* assign y = 4'hFF + (* keep, max = 3 *) 1.5;  — payloads and two attributes. */
static void test_expression_golden(void) {
    odin3_ast_spec real = spec_at(ODIN3_AST_REAL, at(2, 20));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_real_new(ast, 1.5, &real.payload));
    odin3_ast_spec add = spec_at(ODIN3_AST_BINARY, at(2, 16));
    add.sub = ODIN3_AST_BINARY_ADD;
    odin3_ast_id sum = mk(add, (odin3_ast_id[]){hex_ff(at(2, 12)), mk(real, NULL, 0)}, 2);
    odin3_ast_id attrs[2] = {
        mk(named_at(ODIN3_AST_ATTR, "keep", (odin3_loc){0}), &NO, 1),
        mk(named_at(ODIN3_AST_ATTR, "max", (odin3_loc){0}), (odin3_ast_id[]){three()}, 1)};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_attach(ast, sum, attrs, 2));
    odin3_ast_id lhs = mk(named_at(ODIN3_AST_IDENT, "y", at(2, 8)), NULL, 0);
    odin3_ast_id net = mk(spec_at(ODIN3_AST_NET_ASSIGN, at(2, 8)), (odin3_ast_id[]){lhs, sum}, 2);
    odin3_ast_id cont =
        mk(spec_at(ODIN3_AST_CONT_ASSIGN, at(2, 1)), (odin3_ast_id[]){NO, NO, net}, 3);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_finish(ast));
    expect(cont, "(CONT_ASSIGN @t.v:2:1\n"
                 "  _\n"
                 "  _\n"
                 "  (NET_ASSIGN @t.v:2:8\n"
                 "    (IDENT:y @t.v:2:8)\n"
                 "    (BINARY.+ @t.v:2:16\n"
                 "      (NUMBER.hex @t.v:2:12 4'hff)\n"
                 "      (REAL @t.v:2:20 1.5)\n"
                 "      {(ATTR:keep @<unknown>:0:0\n"
                 "         _)\n"
                 "       (ATTR:max @<unknown>:0:0\n"
                 "         (NUMBER.dec [SIGNED] @<unknown>:0:0 3))})))");
}

static void test_strings_and_text(void) {
    odin3_ast_id str = mk(named_at(ODIN3_AST_STRING, "a \"b\"\n", at(3, 1)), NULL, 0);
    expect(str, "(STRING:\"a \\\"b\\\"\\n\" @t.v:3:1)");
    odin3_ast_spec empty = spec_at(ODIN3_AST_STRING, at(3, 9));
    empty.flags = ODIN3_AST_F_STRING_EMPTY;
    expect(mk(empty, NULL, 0), "(STRING:\"\" [EMPTY] @t.v:3:9)");
    odin3_ast_spec spec = spec_at(ODIN3_AST_SPECIFY_BLOCK, at(3, 5));
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK,
        odin3_ast_text_new(ast, odin3_bytes_cstr("specify\n\tendspecify"), &spec.payload));
    expect(mk(spec, NULL, 0), "(SPECIFY_BLOCK @t.v:3:5 \"specify\\n\\tendspecify\")");
    odin3_ast_spec gen = named_at(ODIN3_AST_GEN_BLOCK, "g", (odin3_loc){0});
    expect(mk(gen, (odin3_ast_id[]){three()}, 1),
           "(GEN_BLOCK:g @<unknown>:0:0\n  (NUMBER.dec [SIGNED] @<unknown>:0:0 3))");
}

static odin3_ast_spec flagged(odin3_ast_spec spec, uint32_t flags) {
    spec.flags = (uint16_t)flags;
    return spec;
}

/* Flags by name in bit order, fields as NAME=value, undefined bits as one 0x token. */
static void test_flags(void) {
    uint32_t dt_reg = (uint32_t)ODIN3_AST_DT_REG << ODIN3_AST_F_PORT_DECL_DT_SHIFT;
    uint32_t net_wire = (1U + ODIN3_AST_NET_DECL_WIRE) << ODIN3_AST_F_PORT_DECL_NET_SHIFT;
    odin3_ast_id decl = mk(named_at(ODIN3_AST_DECLARATOR, "q", (odin3_loc){0}), &NO, 1);
    odin3_ast_spec port =
        flagged(spec_at(ODIN3_AST_PORT_DECL, at(1, 1)),
                ODIN3_AST_F_PORT_DECL_SIGNED | ODIN3_AST_F_PORT_DECL_IN_HEADER | dt_reg | net_wire);
    port.sub = ODIN3_AST_PORT_DECL_OUTPUT;
    expect(mk(port, (odin3_ast_id[]){NO, decl}, 2),
           "(PORT_DECL.output [SIGNED IN_HEADER DT=1 NET=1] @t.v:1:1\n"
           "  _\n"
           "  (DECLARATOR:q @<unknown>:0:0\n"
           "    _))");
    odin3_ast_spec str = flagged(spec_at(ODIN3_AST_STRENGTH, (odin3_loc){0}),
                                 (ODIN3_AST_DRIVE_STRONG << ODIN3_AST_F_STRENGTH_S0_SHIFT) |
                                     (ODIN3_AST_DRIVE_WEAK << ODIN3_AST_F_STRENGTH_S1_SHIFT));
    expect(mk(str, NULL, 0), "(STRENGTH.drive [S0=4 S1=2] @<unknown>:0:0)");
    odin3_ast_id null_stmt = mk(spec_at(ODIN3_AST_NULL_STMT, (odin3_loc){0}), NULL, 0);
    odin3_ast_spec item =
        flagged(spec_at(ODIN3_AST_CASE_ITEM, (odin3_loc){0}), ODIN3_AST_F_CASE_ITEM_DEFAULT);
    expect(mk(item, &null_stmt, 1),
           "(CASE_ITEM [DEFAULT] @<unknown>:0:0\n  (NULL_STMT @<unknown>:0:0))");
    odin3_ast_spec star =
        flagged(spec_at(ODIN3_AST_EVENT_CONTROL, (odin3_loc){0}),
                ODIN3_AST_F_EVENT_CONTROL_STAR | ODIN3_AST_F_EVENT_CONTROL_NO_PARENS);
    odin3_ast_id ctl = mk(star, &NO, 1);
    expect(ctl, "(EVENT_CONTROL [STAR NO_PARENS] @<unknown>:0:0\n  _)");
    odin3_ast_node *rec = (odin3_ast_node *)odin3_ast_rec(ast, ctl); /* corrupt: bit 4 */
    rec->flags |= 0x10U;
    expect(ctl, "(EVENT_CONTROL [STAR NO_PARENS 0x10] @<unknown>:0:0\n  _)");
    rec->flags = 0x20U;
    expect(ctl, "(EVENT_CONTROL [0x20] @<unknown>:0:0\n  _)");
}

/* Review Focus 5: an abandoned root dumps like any other subtree; a UNIT with no items. */
static void test_abandoned_root_and_empty_unit(void) {
    odin3_ast_id lost = mk(named_at(ODIN3_AST_IDENT, "lost", at(1, 1)), NULL, 0);
    odin3_ast_id unit = mk(named_at(ODIN3_AST_UNIT, "t.v", (odin3_loc){0}), NULL, 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_finish(ast));
    expect(lost, "(IDENT:lost @t.v:1:1)");
    expect(unit, "(UNIT:t.v @<unknown>:0:0)");
}

static void test_chain_at_cap(void) {
    odin3_ast_spec unary = spec_at(ODIN3_AST_UNARY, (odin3_loc){0});
    unary.sub = ODIN3_AST_UNARY_NOT;
    odin3_ast_id top = mk(named_at(ODIN3_AST_IDENT, "x", (odin3_loc){0}), NULL, 0);
    for (uint32_t height = 2; height <= ODIN3_AST_MAX_DEPTH; height++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_make(ast, &unary, &top, 1, &top));
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_finish(ast));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_dump(ast, top, &out));
    size_t lines = 1;
    for (size_t i = 0; i < out.len; i++) {
        lines += out.data[i] == '\n';
    }
    TEST_ASSERT_EQUAL_size_t(ODIN3_AST_MAX_DEPTH, lines);
    const char *last = strrchr(out.data, '\n') + 1; /* indentation capped */
    size_t blanks = strspn(last, " ");
    TEST_ASSERT_EQUAL_size_t(2 * ODIN3_PR_MAX_INDENT, blanks);
    TEST_ASSERT_EQUAL_STRING_LEN("(IDENT:x @<unknown>:0:0)))", last + blanks, 26);
}

static void test_misuse(void) {
    odin3_ast_id node = mk(named_at(ODIN3_AST_IDENT, "a", (odin3_loc){0}), NULL, 0);
    size_t errors = errors_logged;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_dump(NULL, node, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_dump(ast, node, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ast_dump(ast, NO, &out));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_ast_dump(ast, (odin3_ast_id){node.v + 1}, &out));
    TEST_ASSERT_EQUAL_size_t(errors + 4, errors_logged);
}

static void test_oom_sweep(void) {
    odin3_ast_spec add = spec_at(ODIN3_AST_BINARY, at(2, 16));
    odin3_ast_id sum = mk(add, (odin3_ast_id[]){hex_ff(at(2, 12)), three()}, 2);
    odin3_ast_id keep =
        mk(named_at(ODIN3_AST_ATTR, "keep", at(1, 4)), (odin3_ast_id[]){three()}, 1);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_attach(ast, sum, &keep, 1));
    odin3_ast_id str = mk(named_at(ODIN3_AST_STRING, "s\n", at(3, 1)), NULL, 0);
    odin3_ast_id call = mk(named_at(ODIN3_AST_CALL, "f", at(3, 1)), (odin3_ast_id[]){sum, str}, 2);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_finish(ast));
    bool done = false;
    for (long k = 0; k < OOM_LIMIT && !done; k++) {
        odin3_strbuf buf;
        odin3_strbuf_init(&buf);
        odin3_util_set_alloc_fail_after(k);
        odin3_status st = odin3_ast_dump(ast, call, &buf);
        odin3_util_set_alloc_fail_after(-1);
        odin3_strbuf_free(&buf);
        done = st == ODIN3_OK;
        if (!done) {
            TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        }
    }
    TEST_ASSERT_TRUE(done);
}

/* A design whose source manager does not exist yet: creating it is an allocation point. */
static void test_oom_creating_the_source_manager(void) {
    odin3_design *fresh = odin3_design_create();
    TEST_ASSERT_NOT_NULL(fresh);
    odin3_ast *store = NULL;
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_ast_create(fresh, ODIN3_AST_FORM_PARSED, (odin3_passrun_id){1}, &store));
    odin3_ast_spec spec = {.kind = ODIN3_AST_NULL_STMT};
    odin3_ast_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_make(store, &spec, NULL, 0, &node));
    odin3_util_set_alloc_fail_after(0);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, odin3_ast_dump(store, node, &out));
    odin3_util_set_alloc_fail_after(-1);
    odin3_strbuf_clear(&out);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_dump(store, node, &out));
    TEST_ASSERT_EQUAL_STRING("(NULL_STMT @<unknown>:0:0)", out.data);
    odin3_ast_destroy(store);
    odin3_design_destroy(fresh);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_declaration_golden);
    RUN_TEST(test_expression_golden);
    RUN_TEST(test_strings_and_text);
    RUN_TEST(test_flags);
    RUN_TEST(test_abandoned_root_and_empty_unit);
    RUN_TEST(test_chain_at_cap);
    RUN_TEST(test_misuse);
    RUN_TEST(test_oom_sweep);
    RUN_TEST(test_oom_creating_the_source_manager);
    return UNITY_END();
}
