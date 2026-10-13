/*
 * test_read_verilog_lib.c — 2D-7b: the library-unit read (`read_verilog --lib`, spec 3.1a and
 * 17 #80): modules kept as library units for a later read, each elaborated under the flags of the
 * read that supplied it, never a top candidate, never a black box, and the precedence of a
 * library unit over a techlib hard cell of the same name. The whole read_verilog pass runs
 * through the pass manager; the IR and the elaborated store are read back.
 */
#include "elab_test.h"

#include "ast/ast.h"
#include "ast/kinds.h"
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "unity.h"
#include "util/log.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static et_result rs;

void setUp(void) {
    memset(&rs, 0, sizeof rs);
}

void tearDown(void) {
    odin3_log_set_sink(NULL, NULL);
    if (rs.design != NULL || rs.dir[0] != '\0') {
        et_free(&rs);
    }
}

/* Files are written but never read as design files: their names do not end in ".v". */
static void setup_files(const et_src *srcs, uint32_t n, const char *pre) {
    et_req req = {srcs, n, pre, NULL, NULL};
    et_setup(&req, &rs);
}

static odin3_status run(const char *pass, const char *args) {
    return odin3_pass_run(rs.design, pass, odin3_bytes_cstr(args));
}

/* The first IR module whose name starts with prefix (NULL: none). */
static const odin3_module *module_like(const char *prefix) {
    uint32_t end = odin3_design_module_end(rs.design);
    for (uint32_t id = 1; id < end; id++) {
        const odin3_module *module = odin3_module_get(rs.design, (odin3_module_id){id});
        const char *name = module != NULL ? odin3_strtab_get(odin3_design_strtab(rs.design),
                                                             odin3_module_name(module))
                                          : NULL;
        if (name != NULL && strncmp(name, prefix, strlen(prefix)) == 0) {
            return module;
        }
    }
    return NULL;
}

/* The live nodes of cell type `type` over every module. */
static uint32_t cells_of(const char *type) {
    uint32_t count = 0;
    uint32_t end = odin3_design_module_end(rs.design);
    for (uint32_t id = 1; id < end; id++) {
        const odin3_module *module = odin3_module_get(rs.design, (odin3_module_id){id});
        for (uint32_t n = 1; module != NULL && n < odin3_module_node_end(module); n++) {
            odin3_node_id node = {n};
            count += odin3_node_live(module, node) &&
                             strcmp(odin3_celltype_get(rs.design, odin3_node_type(module, node))->name,
                                    type) == 0
                         ? 1U
                         : 0U;
        }
    }
    return count;
}

static const char k_sub[] = "module sub #(parameter W = 1) (input [W-1:0] a, output [W-1:0] y);\n"
                            "  assign y = ~a;\nendmodule\n"
                            "module unused_a (input a, output y);\n  assign y = a;\nendmodule\n"
                            "module unused_b (input a, output y);\n  assign y = ~a;\nendmodule\n";
static const char k_top[] = "module top (input [3:0] a, output [3:0] y);\n"
                            "  sub #(4) u (.a(a), .y(y));\nendmodule\n";

/* A file of several uninstantiated modules reads alone (no ambiguous_top) and makes no IR. */
static void test_lib_read_alone(void) {
    et_src srcs[] = {{"p.vl", k_sub}};
    setup_files(srcs, 1, NULL);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run("read_verilog", "--lib p.vl"), et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, rs.errors, et_log(&rs));
    TEST_ASSERT_EQUAL_INT(-1, et_find(&rs, ODIN3_LOG_ERROR, "ambiguous_top"));
    TEST_ASSERT_NULL(module_like("sub"));
    TEST_ASSERT_NULL(module_like("unused"));
}

/* A later read elaborates the instantiated unit, drops the others without a word. */
static void test_lib_unit_elaborated_where_used(void) {
    et_src srcs[] = {{"p.vl", k_sub}, {"t.v", k_top}};
    setup_files(srcs, 2, NULL);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run("read_verilog", "--lib p.vl"), et_log(&rs));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run("read_verilog", "t.v"), et_log(&rs));
    TEST_ASSERT_NOT_NULL_MESSAGE(module_like("top"), et_log(&rs));
    const odin3_module *sub = module_like("sub");
    TEST_ASSERT_NOT_NULL_MESSAGE(sub, et_log(&rs));
    TEST_ASSERT_TRUE_MESSAGE(odin3_module_node_end(sub) > 3, "the unit has a body, not a black box");
    TEST_ASSERT_NULL(module_like("unused"));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, rs.warnings, et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, et_count_with(&rs, ODIN3_LOG_INFO, "not instantiated"),
                                     et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, et_count_with(&rs, ODIN3_LOG_INFO, "unused"), et_log(&rs));
}

/* Library units are never top candidates: with one design module the top is that one. */
static void test_lib_units_not_top_candidates(void) {
    et_src srcs[] = {{"p.vl", k_sub}, {"t.v", "module only (input a, output y);\n  assign y = a;\n"
                                              "endmodule\n"}};
    setup_files(srcs, 2, NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("read_verilog", "--lib p.vl"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run("read_verilog", "t.v"), et_log(&rs));
    TEST_ASSERT_TRUE_MESSAGE(
        et_find(&rs, ODIN3_LOG_INFO,
                "top module 'work.only' chosen: the only module no other module instantiates") >=
            0,
        et_log(&rs));
}

/* A unit chain across two library reads: mid (second read) instantiates leaf (first read). */
static void test_lib_units_chain_across_reads(void) {
    et_src srcs[] = {{"p1.vl", "module leaf (input a, output y);\n  assign y = ~a;\nendmodule\n"},
                     {"p2.vl", "module mid (input a, output y);\n  leaf l (.a(a), .y(y));\n"
                               "endmodule\n"},
                     {"t.v", "module top (input a, output y);\n  mid m (.a(a), .y(y));\nendmodule\n"}};
    setup_files(srcs, 3, NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("read_verilog", "--lib p1.vl"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("read_verilog", "--lib p2.vl"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run("read_verilog", "t.v"), et_log(&rs));
    TEST_ASSERT_NOT_NULL(module_like("mid"));
    TEST_ASSERT_NOT_NULL(module_like("leaf"));
}

/* A design unit of the same name wins over a library unit. */
static void test_design_unit_wins_over_lib_unit(void) {
    et_src srcs[] = {{"p.vl", "module m (input a, output y);\n  assign y = ~a;\nendmodule\n"},
                     {"t.v", "module m (input a, output y);\n  assign y = a;\nendmodule\n"
                             "module top (input a, output y);\n  m u (.a(a), .y(y));\nendmodule\n"}};
    setup_files(srcs, 2, NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("read_verilog", "--lib p.vl"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run("read_verilog", "t.v"), et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, cells_of("$not"), et_log(&rs));
}

/* A syntax error in the library read is reported at that read. */
static void test_lib_syntax_error_at_lib_read(void) {
    et_src srcs[] = {{"p.vl", "module bad (input a;\nendmodule\n"}};
    setup_files(srcs, 1, NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, run("read_verilog", "--lib p.vl"));
}

static void test_lib_rejects_auto_blackbox(void) {
    et_src srcs[] = {{"p.vl", k_sub}};
    setup_files(srcs, 1, NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run("read_verilog", "--lib --auto-blackbox p.vl"));
    TEST_ASSERT_TRUE_MESSAGE(et_count_with(&rs, ODIN3_LOG_ERROR, "--lib") > 0, et_log(&rs));
}

/* --- per-unit flags ----------------------------------------------------------------------- */

#define NOMEM_ERROR                                                                                \
    "array 'w' cannot be a memory (it is written in a combinational block) and "                   \
    "--nomem2reg is set"

static const char k_arr[] = "module arr (input [3:0] a, output [3:0] y);\n"
                            "  wire [3:0] w [0:3];\n  assign w[1] = a;\n  assign y = w[1];\n"
                            "endmodule\n";
static const char k_arr_top[] = "module top (input [3:0] a, output [3:0] y);\n"
                                "  arr u (.a(a), .y(y));\nendmodule\n";

/* The unit read with --nomem2reg is elaborated under it though the design read has no flag. */
static void test_unit_flag_nomem2reg_from_lib_read(void) {
    et_src srcs[] = {{"p.vl", k_arr}, {"t.v", k_arr_top}};
    setup_files(srcs, 2, NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("read_verilog", "--lib --nomem2reg p.vl"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_PARSE, run("read_verilog", "t.v"), et_log(&rs));
    TEST_ASSERT_TRUE_MESSAGE(et_count_with(&rs, ODIN3_LOG_ERROR, NOMEM_ERROR) > 0, et_log(&rs));
}

/* The design read's --nomem2reg does not reach a unit read without it. */
static void test_unit_flag_not_inherited_from_design_read(void) {
    et_src srcs[] = {{"p.vl", k_arr}, {"t.v", k_arr_top}};
    setup_files(srcs, 2, NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("read_verilog", "--lib p.vl"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run("read_verilog", "--nomem2reg t.v"), et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, rs.errors, et_log(&rs));
}

/* Control: a design-unit module of the same text does fail under the design read's flag. */
static void test_design_flag_applies_to_design_units(void) {
    et_src srcs[] = {{"t.v", k_arr}};
    setup_files(srcs, 1, NULL);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_PARSE, run("read_verilog", "--nomem2reg t.v"),
                                  et_log(&rs));
}

/* --nolatches of the library read is recorded on the unit's module in the elaborated store. */
static bool module_has_attr(const odin3_ast *elab, const char *attr) {
    uint32_t key = 0;
    if (!odin3_strtab_find(odin3_design_strtab(rs.design), odin3_bytes_cstr(attr), &key)) {
        return false;
    }
    odin3_ast_id unit = odin3_ast_root(elab, 0);
    odin3_ast_span kids = odin3_ast_children(elab, unit);
    for (uint32_t i = 0; i < kids.n; i++) {
        odin3_ast_span attrs = odin3_ast_attrs(elab, kids.ids[i]);
        for (uint32_t k = 0; odin3_ast_kind_of(elab, kids.ids[i]) == ODIN3_AST_MODULE && k < attrs.n;
             k++) {
            if (odin3_ast_name(elab, attrs.ids[k]) == key) {
                return true;
            }
        }
    }
    return false;
}

static const odin3_ast *kept_elaborated(void) {
    for (uint32_t i = 0; i < odin3_design_ast_count(rs.design); i++) {
        const odin3_ast *ast = odin3_design_ast_at(rs.design, i);
        if (odin3_ast_form_of(ast) == ODIN3_AST_FORM_ELABORATED) {
            return ast;
        }
    }
    return NULL;
}

static void test_unit_flag_nolatches_recorded(void) {
    et_src srcs[] = {{"p.vl", k_sub}, {"t.v", k_top}};
    setup_files(srcs, 2, NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_set_keep_ast(rs.design, ODIN3_KEEP_AST_ELABORATED));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("read_verilog", "--lib --nolatches p.vl"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run("read_verilog", "t.v"), et_log(&rs));
    const odin3_ast *elab = kept_elaborated();
    TEST_ASSERT_NOT_NULL(elab);
    TEST_ASSERT_TRUE(module_has_attr(elab, "odin3_lib_nolatches"));
    TEST_ASSERT_FALSE(module_has_attr(elab, "odin3_lib_nomem2reg"));
}

/* --- precedence over a techlib hard cell --------------------------------------------------- */

#define TECHLIB "read_techlib " ODIN3_LIB_DIR "/vtr.o3lib"

static const char k_adder[] =
    "module adder #(parameter WIDTH = 1) (input [WIDTH-1:0] a, input [WIDTH-1:0] b, input cin,\n"
    "                                     output cout, output [WIDTH-1:0] sumout);\n"
    "  assign {cout, sumout} = a + b + cin;\nendmodule\n";

/* A library unit named like a hard cell is not "has the name of cell type"; an instance whose
 * parameters do not fit the hard cell takes the unit, with a warning. */
static void test_lib_unit_over_hard_cell_when_params_do_not_fit(void) {
    et_src srcs[] = {{"p.vl", k_adder},
                     {"t.v", "module top (input [3:0] a, b, input c, output [3:0] s, output o);\n"
                             "  adder #(4) u (.a(a), .b(b), .cin(c), .sumout(s), .cout(o));\n"
                             "endmodule\n"}};
    setup_files(srcs, 2, TECHLIB);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run("read_verilog", "--lib p.vl"), et_log(&rs));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run("read_verilog", "t.v"), et_log(&rs));
    TEST_ASSERT_NOT_NULL_MESSAGE(module_like("adder"), et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32(0, cells_of("adder"));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, et_count_with(&rs, ODIN3_LOG_WARN, "library unit 'adder'"),
                                     et_log(&rs));
}

/* An instance that fits the hard cell keeps it, with an info line naming the choice. */
static void test_hard_cell_kept_when_params_fit(void) {
    et_src srcs[] = {{"p.vl", k_adder},
                     {"t.v", "module top (input a, b, c, output s, output o);\n"
                             "  adder u (.a(a), .b(b), .cin(c), .sumout(s), .cout(o));\n"
                             "endmodule\n"}};
    setup_files(srcs, 2, TECHLIB);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("read_verilog", "--lib p.vl"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run("read_verilog", "t.v"), et_log(&rs));
    TEST_ASSERT_NULL(module_like("adder"));
    TEST_ASSERT_EQUAL_UINT32(1, cells_of("adder"));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, rs.warnings, et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, et_count_with(&rs, ODIN3_LOG_INFO, "hard cell 'adder'"),
                                     et_log(&rs));
}

/* A named parameter the hard cell lacks does not fit either. */
static void test_named_parameter_not_on_hard_cell(void) {
    et_src srcs[] = {{"p.vl", k_adder},
                     {"t.v", "module top (input [1:0] a, b, input c, output [1:0] s, output o);\n"
                             "  adder #(.WIDTH(2)) u (.a(a), .b(b), .cin(c), .sumout(s), .cout(o));\n"
                             "endmodule\n"}};
    setup_files(srcs, 2, TECHLIB);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run("read_verilog", "--lib p.vl"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, run("read_verilog", "t.v"), et_log(&rs));
    TEST_ASSERT_NOT_NULL(module_like("adder"));
    TEST_ASSERT_EQUAL_UINT32(0, cells_of("adder"));
}

/* Without the library read the hard cell's own error stands (spec 17 #78 for a plain design). */
static void test_no_lib_read_keeps_error(void) {
    et_src srcs[] = {{"t.v", "module top (input [3:0] a, b, input c, output [3:0] s, output o);\n"
                             "  adder #(4) u (.a(a), .b(b), .cin(c), .sumout(s), .cout(o));\n"
                             "endmodule\n"}};
    setup_files(srcs, 1, TECHLIB);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, run("read_verilog", "t.v"));
    TEST_ASSERT_TRUE(et_count_with(&rs, ODIN3_LOG_ERROR, "too many parameter values") > 0);
}

#define ALL_TESTS(X)                                                                               \
    X(test_lib_read_alone)                                                                         \
    X(test_lib_unit_elaborated_where_used)                                                         \
    X(test_lib_units_not_top_candidates)                                                           \
    X(test_lib_units_chain_across_reads)                                                           \
    X(test_design_unit_wins_over_lib_unit)                                                         \
    X(test_lib_syntax_error_at_lib_read)                                                           \
    X(test_lib_rejects_auto_blackbox)                                                              \
    X(test_unit_flag_nomem2reg_from_lib_read)                                                      \
    X(test_unit_flag_not_inherited_from_design_read)                                               \
    X(test_design_flag_applies_to_design_units)                                                    \
    X(test_unit_flag_nolatches_recorded)                                                           \
    X(test_lib_unit_over_hard_cell_when_params_do_not_fit)                                         \
    X(test_hard_cell_kept_when_params_fit)                                                         \
    X(test_named_parameter_not_on_hard_cell)                                                       \
    X(test_no_lib_read_keeps_error)

#define RUN_ONE(name) RUN_TEST(name);

int main(void) {
    UNITY_BEGIN();
    ALL_TESTS(RUN_ONE)
    return UNITY_END();
}
