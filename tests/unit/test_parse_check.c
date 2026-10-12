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
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { PATH_BUF = 1024 };

/* Line 6 holds `assign z = a & b;`; every construct the faults below touch has its own line.
 * Module mc (lines 16-24) adds a macro use (line 18: a BINARY whose first operand lies in the
 * expansion buffer) and a metacomment (line 20). Module nn (lines 25-28) holds a binary `~&` (line
 * 26: UNARY[~] over BINARY[&], both at `a`) beside a written `~(a | b)` (line 27). */
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
                               "endmodule\n"
                               "`define PC_A a\n"
                               "module mc (input [1:0] s, input a, b, output reg y, output q);\n"
                               "  assign q = `PC_A & b;\n"
                               "  always @*\n"
                               "    case (s) // synopsys full_case\n"
                               "      0: y = a;\n"
                               "      default: y = b;\n"
                               "    endcase\n"
                               "endmodule\n"
                               "module nn (input a, b, output y, z);\n"
                               "  assign y = a ~& b;\n"
                               "  assign z = ~(a | b);\n"
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

/* The node of kind, named name, on line `line` of t.v (its file location). */
static odin3_ast_id find_at(odin3_ast_kind kind, const char *name, uint32_t line) {
    odin3_srcman *sm = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_srcman(rd.design, &sm));
    odin3_ast_id end = odin3_ast_node_end(rd.ast);
    for (odin3_ast_id node = {1}; node.v < end.v; node.v++) {
        odin3_srcpos pos;
        if (odin3_ast_kind_of(rd.ast, node) == kind &&
            strcmp(odin3_ast_name_str(rd.ast, node), name) == 0 &&
            odin3_srcman_decode(sm, odin3_ast_loc(rd.ast, node), &pos) && pos.line == line) {
            return node;
        }
    }
    TEST_FAIL_MESSAGE("no such node on the line");
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
    TEST_ASSERT_TRUE(rd.stream.len > 0);
    expect_pass();
}

/* A binary `~&` reads as UNARY[~] over BINARY[&] at the left operand (2C Task 10): it passes as
 * its operand; moved one token right, to the `~&`, it no longer does. */
static void test_binary_nand_and_its_shifted_unary(void) {
    odin3_ast_id unary = find(ODIN3_AST_UNARY, NULL, 1);
    odin3_ast_id binary = odin3_ast_child(rd.ast, unary, 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_AST_BINARY, odin3_ast_kind_of(rd.ast, binary));
    TEST_ASSERT_EQUAL_UINT32(odin3_ast_loc(rd.ast, binary).v, odin3_ast_loc(rd.ast, unary).v);
    expect_pass();
    uint32_t i = token_at(odin3_ast_loc(rd.ast, unary));
    set_range(unary, token(i + 1)->loc, odin3_ast_end(rd.ast, unary));
    const char *const needles[] = {"t.v:26:16: error: location oracle: UNARY",
                                   "starts at '~&', which does not spell it", NULL};
    expect_fail(needles);
}

/* Review M2: the exception for a binary `~&` does not cover a written `~(a | b)` (line 27)
 * whose UNARY is moved onto its operand: the operator after `a` is `|`, not `~|`. */
static void test_written_not_moved_onto_its_operand(void) {
    odin3_ast_id unary = find(ODIN3_AST_UNARY, NULL, 2);
    odin3_ast_id binary = odin3_ast_child(rd.ast, unary, 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_AST_BINARY, odin3_ast_kind_of(rd.ast, binary));
    TEST_ASSERT_NOT_EQUAL_UINT32(odin3_ast_loc(rd.ast, binary).v, odin3_ast_loc(rd.ast, unary).v);
    expect_pass();
    set_range(unary, odin3_ast_loc(rd.ast, binary), odin3_ast_end(rd.ast, binary));
    const char *const needles[] = {"t.v:27:16: error: location oracle: UNARY",
                                   "starts at 'a', which does not spell it", NULL};
    expect_fail(needles);
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

/* The left edge (review I1): a BINARY moved to start at the '=' before its first operand. */
static void test_binary_starts_at_equals(void) {
    odin3_ast_id binary = find(ODIN3_AST_BINARY, NULL, 1);
    odin3_range range = range_of(binary);
    set_range(binary, token(token_at(range.loc) - 1)->loc, range.end);
    const char *const needles[] = {"t.v:6:12: error: location oracle: BINARY",
                                   "starts before its first child at '='", NULL};
    expect_fail(needles);
}

/* A NET_ASSIGN moved to start at its CONT_ASSIGN's 'assign'. */
static void test_net_assign_starts_at_assign(void) {
    odin3_ast_id assign = find(ODIN3_AST_CONT_ASSIGN, NULL, 2);
    odin3_ast_id net = odin3_ast_child(rd.ast, assign, 2);
    TEST_ASSERT_EQUAL_INT(ODIN3_AST_NET_ASSIGN, odin3_ast_kind_of(rd.ast, net));
    set_range(net, odin3_ast_loc(rd.ast, assign), odin3_ast_end(rd.ast, net));
    const char *const needles[] = {"t.v:6:3: error: location oracle: NET_ASSIGN",
                                   "starts before its first child at 'assign'", NULL};
    expect_fail(needles);
}

/* Spec §7.2's I2 class: an item starting at the previous ';' (the end of line 5). */
static void test_item_starts_at_previous_semicolon(void) {
    odin3_ast_id assign = find(ODIN3_AST_CONT_ASSIGN, NULL, 2);
    odin3_range range = range_of(assign);
    set_range(assign, token(token_at(range.loc) - 1)->loc, range.end);
    const char *const needles[] = {"t.v:5:35: error: location oracle: CONT_ASSIGN",
                                   "starts at the closer ';'", NULL};
    expect_fail(needles);
}

/* Tightness: a header PORT_DECL stretched over the ',' after its last declarator. */
static void test_ends_past_last_child(void) {
    move_end(find(ODIN3_AST_PORT_DECL, NULL, 3), 1);
    const char *const needles[] = {"t.v:5:11: error: location oracle: PORT_DECL",
                                   "ends past its last child at ','", NULL};
    expect_fail(needles);
}

/* A loc inside a token: MODULE t one byte into 'module'. */
static void test_loc_inside_a_token(void) {
    odin3_ast_id module = find(ODIN3_AST_MODULE, "t", 1);
    odin3_range range = range_of(module);
    set_range(module, (odin3_loc){range.loc.v + 1}, range.end);
    const char *const needles[] = {"t.v:5:2: error: location oracle: MODULE:t",
                                   "does not start at a token", NULL};
    expect_fail(needles);
}

/* A zero-width node inside a token ('wire' of line 10). */
static void test_empty_node_off_a_boundary(void) {
    odin3_ast_id decl = find(ODIN3_AST_NET_DECL, NULL, 1);
    odin3_ast_id declarator = odin3_ast_child(rd.ast, decl, 3);
    odin3_loc inside = {odin3_ast_loc(rd.ast, decl).v + 1};
    set_range(declarator, inside, inside);
    const char *const needles[] = {"t.v:10:8: error: location oracle: DECLARATOR:v",
                                   "is empty but not at a token boundary", NULL};
    expect_fail(needles);
}

/* A metacomment's ATTR moved one byte off its comment. */
static void test_metacomment_attr_off_its_comment(void) {
    odin3_ast_id attr = find(ODIN3_AST_ATTR, "full_case", 1);
    odin3_range range = range_of(attr);
    set_range(attr, (odin3_loc){range.loc.v + 1}, (odin3_loc){range.end.v + 1});
    const char *const needles[] = {"t.v:20:15: error: location oracle: ATTR:full_case",
                                   "is not at a metacomment", NULL};
    expect_fail(needles);
}

/* Containment in stream order: the BINARY of line 18 starts in the macro's expansion buffer,
 * so only the token map can see `b` stretched over the ';'. */
static void test_child_outside_parent_in_the_stream(void) {
    move_end(find_at(ODIN3_AST_IDENT, "b", 18), 1);
    const char *const needles[] = {"location oracle: IDENT:b", "lies outside its parent",
                                   "in the stream", NULL};
    expect_fail(needles);
}

/* The token map and the stream bytes disagree on the stream's length. */
static void test_stream_length_mismatch(void) {
    rd.stream.len--;
    const char *const needles[] = {"the token map does not end at the stream's end", NULL};
    expect_fail(needles);
    rd.stream.len++;
}

/* A token whose loc is not the stream map's loc at its offset (its end is still right). */
static void test_token_loc_off_its_offset(void) {
    odin3_ast_id assign = find(ODIN3_AST_CONT_ASSIGN, NULL, 2);
    odin3_vp_loc *tok = odin3_vec_at(&rd.tokens, token_at(odin3_ast_loc(rd.ast, assign)));
    tok->loc.v--;
    const char *const needles[] = {"t.v:6:2: error: location oracle: token", "not the map's", NULL};
    expect_fail(needles);
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
    RUN_TEST(test_binary_starts_at_equals);
    RUN_TEST(test_net_assign_starts_at_assign);
    RUN_TEST(test_item_starts_at_previous_semicolon);
    RUN_TEST(test_ends_past_last_child);
    RUN_TEST(test_loc_inside_a_token);
    RUN_TEST(test_empty_node_off_a_boundary);
    RUN_TEST(test_metacomment_attr_off_its_comment);
    RUN_TEST(test_child_outside_parent_in_the_stream);
    RUN_TEST(test_stream_length_mismatch);
    RUN_TEST(test_token_loc_off_its_offset);
    RUN_TEST(test_binary_nand_and_its_shifted_unary);
    RUN_TEST(test_written_not_moved_onto_its_operand);
    return UNITY_END();
}
