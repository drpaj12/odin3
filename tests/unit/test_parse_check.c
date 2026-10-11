/*
 * test_parse_check.c — the location oracle of odin3-parse-check (2C spec §7.2, plan Task 9;
 * tools/parse-check/parse_check.c) on a one-file read: a clean file passes, and seeded faults
 * fail with the node named — a node shifted by one token (an identifier and a keyword-led
 * item), a child whose range leaves its parent's, and the end regressions the Task 7 review
 * named (an INSTANTIATION ending at its first token, a parameter LIST before its ')', a
 * GEN_BLOCK before its 'end', a DEFPARAM over its ';', a GATE_DECL before its ';').
 */
#include "ast/ast.h"
#include "ast/ast_test.h"
#include "ast/srcman.h"
#include "frontends/verilog/parser.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "parse_check.h"
#include "unity.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { PATH_BUF = 1024 };

/* Line 6 holds `assign w = a & b;`; every construct the faults below touch has its own line. */
static const char k_source[] = "// the oracle's fixture\n"
                               "module leaf #(parameter W = 1) (input a, output y);\n"
                               "  assign y = a;\n"
                               "endmodule\n"
                               "module t (input a, b, output y, z);\n"
                               "  assign z = a & b;\n"
                               "  leaf #(2) u (a, z);\n"
                               "  generate\n"
                               "    if (1) begin : g\n"
                               "      wire v;\n"
                               "    end\n"
                               "  endgenerate\n"
                               "  defparam u.W = 3;\n"
                               "  and g1 (y, a, b);\n"
                               "endmodule\n";

static char cwd[PATH_BUF];
static char tmp[PATH_BUF / 4];
static odin3_strbuf logbuf;
static pc_read rd;

static void sink(odin3_log_level level, const char *msg, void *user) {
    (void)level;
    (void)user;
    (void)odin3_strbuf_appendf(&logbuf, "%s\n", msg);
}

static const char *logged(void) {
    return logbuf.data != NULL ? logbuf.data : "";
}

void setUp(void) {
    odin3_strbuf_init(&logbuf);
    TEST_ASSERT_NOT_NULL(getcwd(cwd, sizeof cwd));
    const char *base = getenv("TMPDIR");
    (void)snprintf(tmp, sizeof tmp, "%s/odin3-parse-check.XXXXXX",
                   base != NULL && base[0] != '\0' ? base : "/tmp");
    TEST_ASSERT_NOT_NULL(mkdtemp(tmp));
    TEST_ASSERT_EQUAL_INT(0, chdir(tmp));
    FILE *fp = fopen("t.v", "w");
    TEST_ASSERT_NOT_NULL(fp);
    TEST_ASSERT_EQUAL_size_t(strlen(k_source), fwrite(k_source, 1, strlen(k_source), fp));
    TEST_ASSERT_EQUAL_INT(0, fclose(fp));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_log_set_level(ODIN3_LOG_WARN));
    odin3_log_set_sink(sink, NULL);
    odin3_status st = pc_read_file("t.v", &rd);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, st, logged());
    odin3_strbuf_clear(&logbuf);
}

void tearDown(void) {
    pc_read_free(&rd);
    odin3_log_set_sink(NULL, NULL);
    (void)odin3_log_set_level(ODIN3_LOG_INFO);
    (void)unlink("t.v");
    TEST_ASSERT_EQUAL_INT(0, chdir(cwd));
    (void)rmdir(tmp);
    odin3_strbuf_free(&logbuf);
}

/* --- helpers -------------------------------------------------------------------------------- */

/* The nth (from 1) node of kind, named name when name is not NULL. */
static odin3_ast_id find(odin3_ast_kind kind, const char *name, uint32_t nth) {
    odin3_ast_id end = odin3_ast_node_end(rd.ast);
    for (odin3_ast_id node = {1}; node.v < end.v; node.v++) {
        if (odin3_ast_kind_of(rd.ast, node) == kind &&
            (name == NULL || strcmp(odin3_ast_name_str(rd.ast, node), name) == 0) && --nth == 0) {
            return node;
        }
    }
    TEST_FAIL_MESSAGE("no such node");
    return (odin3_ast_id){0};
}

/* The token map's index of the token at loc. */
static uint32_t token_at(odin3_loc loc) {
    const odin3_vp_loc *toks = rd.tokens.data;
    for (uint32_t i = 0; i < rd.tokens.len; i++) {
        if (toks[i].loc.v == loc.v) {
            return i;
        }
    }
    TEST_FAIL_MESSAGE("no token at the loc");
    return 0;
}

static const odin3_vp_loc *token(uint32_t idx) {
    TEST_ASSERT_TRUE(idx < rd.tokens.len);
    return (const odin3_vp_loc *)rd.tokens.data + idx;
}

static odin3_range range_of(odin3_ast_id node) {
    return (odin3_range){odin3_ast_loc(rd.ast, node), odin3_ast_end(rd.ast, node)};
}

static void set_range(odin3_ast_id node, odin3_loc loc, odin3_loc end) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_ast_test_set_range(rd.ast, node, (odin3_range){loc, end}));
}

/* The oracle fails, logging every needle (NULL-terminated). */
static void expect_fail(const char *const *needles) {
    odin3_strbuf_clear(&logbuf);
    uint32_t failures = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pc_loc_oracle(&rd, &failures));
    TEST_ASSERT_TRUE_MESSAGE(failures > 0, "the seeded fault was not caught");
    for (const char *const *needle = needles; *needle != NULL; needle++) {
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(logged(), *needle), logged());
    }
}

static void expect_pass(void) {
    odin3_strbuf_clear(&logbuf);
    uint32_t failures = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pc_loc_oracle(&rd, &failures));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, failures, logged());
}

/* " (node N)" of node, for the needles. */
static const char *node_tag(odin3_ast_id node, char *buf, size_t cap) {
    (void)snprintf(buf, cap, " (node %u)", (unsigned)node.v);
    return buf;
}

/* node's end becomes the end of the token `delta` tokens from the one ending there now. */
static void move_end(odin3_ast_id node, int delta) {
    odin3_range range = range_of(node);
    const odin3_vp_loc *toks = rd.tokens.data;
    for (uint32_t i = 0; i < rd.tokens.len; i++) {
        if (toks[i].end.v == range.end.v) {
            set_range(node, range.loc, token((uint32_t)((int)i + delta))->end);
            return;
        }
    }
    TEST_FAIL_MESSAGE("no token ends at the node's end");
}

/* --- tests ---------------------------------------------------------------------------------- */

static void test_clean_file_passes(void) {
    TEST_ASSERT_EQUAL_UINT32(1, odin3_ast_root_count(rd.ast));
    TEST_ASSERT_TRUE(rd.tokens.len > 0);
    TEST_ASSERT_TRUE(rd.stream.len == strlen(k_source));
    expect_pass();
}

/* `a` of `a & b` moved one token right: it now starts at '&', which does not spell it. */
static void test_ident_shifted_by_one_token(void) {
    odin3_ast_id ident = find(ODIN3_AST_IDENT, "a", 2);
    uint32_t i = token_at(odin3_ast_loc(rd.ast, ident));
    set_range(ident, token(i + 1)->loc, token(i + 1)->end);
    char tag[64];
    const char *const needles[] = {"t.v:6:16: error: location oracle: IDENT:a",
                                   node_tag(ident, tag, sizeof tag),
                                   "starts at '&', which does not spell it", NULL};
    expect_fail(needles);
}

/* A keyword-led item moved one token right: CONT_ASSIGN at `z`. */
static void test_keyword_item_shifted_by_one_token(void) {
    odin3_ast_id assign = find(ODIN3_AST_CONT_ASSIGN, NULL, 2);
    uint32_t i = token_at(odin3_ast_loc(rd.ast, assign));
    set_range(assign, token(i + 1)->loc, odin3_ast_end(rd.ast, assign));
    const char *const needles[] = {"location oracle: CONT_ASSIGN",
                                   "starts at 'z', which does not spell it", NULL};
    expect_fail(needles);
}

/* `a & b` stretched over the `;` its NET_ASSIGN parent does not cover. */
static void test_child_outside_parent(void) {
    odin3_ast_id binary = find(ODIN3_AST_BINARY, NULL, 1);
    move_end(binary, 1);
    char tag[64];
    const char *const needles[] = {"location oracle: BINARY", "lies outside its parent",
                                   node_tag(binary, tag, sizeof tag), NULL};
    expect_fail(needles);
}

/* An INSTANTIATION located with its first token's range only (@1 instead of @$). */
static void test_instantiation_end_at_first_token(void) {
    odin3_ast_id inst = find(ODIN3_AST_INSTANTIATION, "leaf", 1);
    odin3_range range = range_of(inst);
    set_range(inst, range.loc, token(token_at(range.loc))->end);
    const char *const needles[] = {"location oracle: INSTANCE:u", "lies outside its parent", NULL};
    expect_fail(needles);
}

/* The parameter LIST `#(2)` ending at `2`, not at its ')'. */
static void test_parameter_list_end(void) {
    odin3_ast_id inst = find(ODIN3_AST_INSTANTIATION, "leaf", 1);
    odin3_ast_id params = odin3_ast_child(rd.ast, inst, 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_AST_LIST, odin3_ast_kind_of(rd.ast, params));
    move_end(params, -1);
    const char *const needles[] = {"t.v:7:8: error: location oracle: LIST",
                                   "ends at '2', not at ')'", NULL};
    expect_fail(needles);
}

/* A GEN_BLOCK ending at the `;` of its item, not at its 'end'. */
static void test_generate_block_end(void) {
    move_end(find(ODIN3_AST_GEN_BLOCK, "g", 1), -1);
    const char *const needles[] = {"location oracle: GEN_BLOCK:g", "ends at ';', not at 'end'",
                                   NULL};
    expect_fail(needles);
}

/* A DEFPARAM pair stretched over the statement's ';'. */
static void test_defparam_end(void) {
    move_end(find(ODIN3_AST_DEFPARAM, NULL, 1), 1);
    const char *const needles[] = {"t.v:13:12: error: location oracle: DEFPARAM",
                                   "ends at ';', not where its last child ends", NULL};
    expect_fail(needles);
}

/* A GATE_DECL ending at its instance's ')', not at its ';'. */
static void test_gate_end(void) {
    move_end(find(ODIN3_AST_GATE_DECL, NULL, 1), -1);
    const char *const needles[] = {"t.v:14:3: error: location oracle: GATE_DECL",
                                   "ends at ')', not at ';'", NULL};
    expect_fail(needles);
}

/* Restoring a seeded range makes the oracle pass again (the faults are the only failures). */
static void test_restored_range_passes(void) {
    odin3_ast_id gate = find(ODIN3_AST_GATE_DECL, NULL, 1);
    odin3_range range = range_of(gate);
    move_end(gate, -1);
    uint32_t failures = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pc_loc_oracle(&rd, &failures));
    TEST_ASSERT_TRUE(failures > 0);
    set_range(gate, range.loc, range.end);
    expect_pass();
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_clean_file_passes);
    RUN_TEST(test_ident_shifted_by_one_token);
    RUN_TEST(test_keyword_item_shifted_by_one_token);
    RUN_TEST(test_child_outside_parent);
    RUN_TEST(test_instantiation_end_at_first_token);
    RUN_TEST(test_parameter_list_end);
    RUN_TEST(test_generate_block_end);
    RUN_TEST(test_defparam_end);
    RUN_TEST(test_gate_end);
    RUN_TEST(test_restored_range_passes);
    return UNITY_END();
}
