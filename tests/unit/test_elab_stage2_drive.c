/*
 * test_elab_stage2_drive.c — 2D Task 19: drivers (A4, uwire, supply, wired nets, tri-state D3,
 * child inouts), instances (I4 resizing, parameters, cell types, black boxes), gates (Q1-Q3) and
 * the error cap / top / summary line of stage 2. The whole read_verilog pass runs through the pass
 * manager (elab_test.h's et_read_keep); the IR is read back through the module API and every
 * located diagnostic is asserted as the whole line.
 *
 * Yosys 0.55 (git sha1 60f126cd0) is the oracle for the cells and the resizing; the commands behind
 * the expected values are in the task report:
 *   yosys -q -p "read_verilog t.v; write_rtlil"            (gates, assignments, instance ports)
 *   yosys -q -p "read_verilog t.v; hierarchy -top top"     (the resize warnings of instances)
 *   yosys -q -p "read_verilog t.v; proc; opt; check"       (a net with two drivers)
 */
#include "elab_test.h"

#include "ast/ast.h"
#include "ast/kinds.h"
#include "ast/symtab.h"
#include "frontends/elab/elab.h"
#include "frontends/elab/elab_internal.h"
#include "frontends/elab/elab_test.h"
#include "ir/celltype.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { OOM_LIMIT = 400000, OOM_SETTLE = 32, TEXT_MAX = 1 << 16 };

static et_result rs;

void setUp(void) {
    memset(&rs, 0, sizeof rs);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_elab_test_set_limits(NULL);
    odin3_elab_test_set_hooks(NULL);
    odin3_log_set_sink(NULL, NULL);
    if (rs.design != NULL || rs.dir[0] != '\0') {
        et_free(&rs);
    }
}

/* --- reading --------------------------------------------------------------------------------- */

#define TECHLIB "read_techlib " ODIN3_LIB_DIR "/vtr.o3lib"

/* The whole source text (file t.v) read through the pass at keep level NONE. */
static void read_req(const et_req *req) {
    et_read_keep(req, ODIN3_KEEP_AST_NONE, &rs);
}

static void read_src(const char *text) {
    et_src src = {"t.v", text};
    et_req req = {&src, 1, NULL, NULL, NULL};
    read_req(&req);
}

/* The same with the VTR tech library read first (the registered cell types). */
static void read_techlib_src(const char *text) {
    et_src src = {"t.v", text};
    et_req req = {&src, 1, TECHLIB, NULL, NULL};
    read_req(&req);
}

static void expect_ir_ok(void) {
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_check_design(rs.design, opts), et_log(&rs));
}

/* Errors that are locatable diagnostics: not the pass manager's own "failed" line. */
static uint32_t diag_errors(void) {
    return rs.errors - et_count_with(&rs, ODIN3_LOG_ERROR, "pass read_verilog: failed");
}

/* The warnings of 2D: not the IR check's (an undriven output leaves rule 4's). */
static uint32_t soft_warnings(void) {
    return rs.warnings - et_count_with(&rs, ODIN3_LOG_WARN, "check: ");
}

static void read_clean(const char *text) {
    read_src(text);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, diag_errors(), et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, soft_warnings(), et_log(&rs));
    expect_ir_ok();
}

/* A failed stage 2 leaves valid, incomplete modules and no top (EL-2). */
static void read_failing(const char *text) {
    read_src(text);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_PARSE, rs.status, et_log(&rs));
    expect_ir_ok();
}

/* "t.v:<line>:<col>: <level>: <msg>". */
typedef struct at_loc {
    uint32_t line, col;
} at_loc;

static const char *diag_line(at_loc at, const char *level, const char *msg) {
    static char buf[768];
    int got = snprintf(buf, sizeof buf, "t.v:%u:%u: %s: %s", (unsigned)at.line, (unsigned)at.col,
                       level, msg);
    TEST_ASSERT_TRUE(got > 0 && (size_t)got < sizeof buf);
    return buf;
}

static void expect_warning(at_loc at, const char *msg) {
    et_expect(&rs, ODIN3_LOG_WARN, diag_line(at, "warning", msg));
}

static void expect_error(at_loc at, const char *msg) {
    et_expect(&rs, ODIN3_LOG_ERROR, diag_line(at, "error", msg));
}

/* --- reading the IR back --------------------------------------------------------------------- */

static uint32_t id_of(const char *text) {
    uint32_t id = 0;
    TEST_ASSERT_TRUE_MESSAGE(
        odin3_strtab_find(odin3_design_strtab(rs.design), odin3_bytes_cstr(text), &id), text);
    return id;
}

static odin3_module *module_named(const char *name) {
    uint32_t id = id_of(name);
    for (uint32_t index = 1; index < odin3_design_module_end(rs.design); index++) {
        odin3_module *module = odin3_module_get(rs.design, (odin3_module_id){index});
        if (odin3_module_name(module) == id) {
            return module;
        }
    }
    TEST_FAIL_MESSAGE(name);
    return NULL;
}

static odin3_module *top(void) {
    return module_named("top");
}

static odin3_wire_id wire_in(odin3_module *module, const char *name) {
    odin3_wire_id wire = odin3_module_find_wire(module, id_of(name));
    TEST_ASSERT_NOT_EQUAL_UINT32_MESSAGE(0, wire.v, name);
    return wire;
}

/* The net of bit `bit` of wire `name` of module top. */
static odin3_net_id wnet(const char *name, uint32_t bit) {
    odin3_net_id net = odin3_wire_net(top(), wire_in(top(), name), bit);
    TEST_ASSERT_NOT_EQUAL_UINT32_MESSAGE(0, net.v, name);
    return net;
}

static const odin3_celltype_def *def_in(const odin3_module *module, odin3_node_id node) {
    return odin3_celltype_get(rs.design, odin3_node_type(module, node));
}

/* The nth live node (in creation order) of module whose cell type is named type. */
static odin3_node_id nth_in(const odin3_module *module, const char *type, uint32_t nth) {
    uint32_t seen = 0;
    for (uint32_t id = 1; id < odin3_module_node_end(module); id++) {
        odin3_node_id node = {id};
        if (odin3_node_live(module, node) && strcmp(def_in(module, node)->name, type) == 0 &&
            seen++ == nth) {
            return node;
        }
    }
    return (odin3_node_id){0};
}

static odin3_node_id nth_node(const char *type, uint32_t nth) {
    return nth_in(top(), type, nth);
}

static uint32_t count_of(const char *type) {
    uint32_t count = 0;
    while (nth_node(type, count).v != 0) {
        count++;
    }
    return count;
}

/* The cells 2D made: neither a port nor a constant driver. */
static uint32_t count_logic(void) {
    const odin3_module *module = top();
    uint32_t count = 0;
    for (uint32_t id = 1; id < odin3_module_node_end(module); id++) {
        odin3_node_id node = {id};
        if (!odin3_node_live(module, node)) {
            continue;
        }
        const odin3_celltype_def *def = def_in(module, node);
        bool constant = strncmp(def->name, "$_CONST", 7) == 0;
        count += def->gran == ODIN3_GRAN_PORT || constant ? 0U : 1U;
    }
    return count;
}

static odin3_node_id only_node(const char *type) {
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, count_of(type), type);
    return nth_node(type, 0);
}

static int64_t param_in(const odin3_module *module, odin3_node_id node, const char *name) {
    const odin3_celltype_def *def = def_in(module, node);
    for (uint32_t index = 0; index < def->n_params; index++) {
        if (strcmp(def->params[index].name, name) == 0) {
            const odin3_value *value = odin3_node_param(module, node, index);
            TEST_ASSERT_NOT_NULL(value);
            return value->i;
        }
    }
    TEST_FAIL_MESSAGE(name);
    return 0;
}

static int64_t param_of(odin3_node_id node, const char *name) {
    return param_in(top(), node, name);
}

/* The net on pin `bit` of port `port` of the node of module. */
static odin3_net_id pin_net_in(const odin3_module *module, odin3_node_id node, const char *port,
                               uint32_t bit) {
    const odin3_celltype_def *def = def_in(module, node);
    for (uint32_t index = 0; index < def->n_ports; index++) {
        if (strcmp(def->ports[index].name, port) == 0) {
            odin3_pinslice slice = odin3_node_port(module, node, index);
            TEST_ASSERT_TRUE_MESSAGE(bit < slice.count, port);
            return odin3_pin_net(module, (odin3_pin_id){slice.first.v + bit});
        }
    }
    TEST_FAIL_MESSAGE(port);
    return (odin3_net_id){0};
}

static odin3_net_id pin_net(odin3_node_id node, const char *port, uint32_t bit) {
    return pin_net_in(top(), node, port, bit);
}

/* The shared constant net of bit (the single output of the module's $_CONSTn_ node); 0 if none. */
static odin3_net_id const_net_of(odin3_bit bit) {
    static const char *const k_names[4] = {"$_CONST0_", "$_CONST1_", "$_CONSTX_", "$_CONSTZ_"};
    odin3_node_id node = nth_node(k_names[bit], 0);
    return node.v == 0 ? (odin3_net_id){0} : pin_net(node, "Y", 0);
}

static void expect_const_bit(const char *name, uint32_t bit, odin3_bit want) {
    odin3_net_id net = const_net_of(want);
    TEST_ASSERT_NOT_EQUAL_UINT32_MESSAGE(0, net.v, name);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(net.v, wnet(name, bit).v, name);
}

/* Bits of wire `name` from `first` onwards. */
typedef struct bits_of {
    const char *name;
    uint32_t first;
} bits_of;

static void expect_same_nets(bits_of lhs, bits_of rhs, uint32_t count) {
    for (uint32_t k = 0; k < count; k++) {
        char why[96];
        TEST_ASSERT_TRUE(snprintf(why, sizeof why, "%s[%u] vs %s[%u]", lhs.name,
                                  (unsigned)(lhs.first + k), rhs.name,
                                  (unsigned)(rhs.first + k)) > 0);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(wnet(rhs.name, rhs.first + k).v,
                                         wnet(lhs.name, lhs.first + k).v, why);
    }
}

/* The pins of port `port` of the node are the nets of the wire bits. */
static void expect_pins_are(odin3_node_id node, const char *port, bits_of src, uint32_t count) {
    for (uint32_t k = 0; k < count; k++) {
        char why[96];
        TEST_ASSERT_TRUE(snprintf(why, sizeof why, "%s pin %u vs %s[%u]", port, (unsigned)k,
                                  src.name, (unsigned)(src.first + k)) > 0);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(wnet(src.name, src.first + k).v, pin_net(node, port, k).v,
                                         why);
    }
}

/* --- assignments with drivers (EL-28, A4) ---------------------------------------------------- */

/* assign p = q; assign q = p: the same net, so no error; nothing drives it, and the warning says
 * so once (Yosys makes a self-connection). */
static void test_assign_loop_warning(void) {
    read_src("module top (output o);\n"
             "  wire p, q;\n"
             "  assign p = q;\n"
             "  assign q = p;\n"
             "endmodule\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, diag_errors(), et_log(&rs));
    expect_warning((at_loc){4, 10},
                   "'p' and 'q' are connected only to each other and are never driven");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, soft_warnings(), et_log(&rs));
    expect_ir_ok();
}

/* A wire with sinks and no driver warns once, at its declarator. */
static void test_never_driven_once(void) {
    read_src("module top (output o1, output o2);\n"
             "  wire w;\n"
             "  assign o1 = w;\n"
             "  assign o2 = w;\n"
             "endmodule\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    expect_warning((at_loc){2, 8}, "'w' is never driven");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, soft_warnings(), et_log(&rs));
    expect_ir_ok();
}

#define A4_HEAD "module top (input [7:0] a, input [7:0] b, input [7:0] c, output [7:0] y);\n"

/* A4: a second continuous assignment of a net is an error at the second, naming the first. */
static void test_a4_whole(void) {
    read_failing(A4_HEAD "  wire unused;\n  assign y = a;\n  assign y = b;\nendmodule\n");
    expect_error((at_loc){4, 10}, "'y' has two drivers (also driven at t.v:3:10)");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, diag_errors(), et_log(&rs));
    expect_same_nets((bits_of){"y", 0}, (bits_of){"a", 0}, 8); /* the second was not attached */
    TEST_ASSERT_EQUAL_UINT32(0, odin3_design_top(rs.design).v);
}

/* A whole net then one of its bits: the bit is named. */
static void test_a4_bit(void) {
    read_failing(A4_HEAD "  wire unused;\n  assign y = a;\n  assign y[3] = c[0];\nendmodule\n");
    expect_error((at_loc){4, 10}, "'y[3]' has two drivers (also driven at t.v:3:10)");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, diag_errors(), et_log(&rs));
    expect_same_nets((bits_of){"y", 0}, (bits_of){"a", 0}, 8);
}

/* A cell driving the lhs nets is a driver too. */
static void test_a4_cell_then_assign(void) {
    read_failing(A4_HEAD
                 "  wire unused;\n  assign y = a + b;\n  assign y[7:4] = c[3:0];\nendmodule\n");
    expect_error((at_loc){4, 10}, "'y[4]' has two drivers (also driven at t.v:3:10)");
    TEST_ASSERT_EQUAL_UINT32(1, count_logic());
}

/* An input port is driven from outside: assigning it is a second driver, at the port. */
static void test_a4_input_port(void) {
    read_failing(A4_HEAD "  wire unused;\n  assign a = b;\nendmodule\n");
    expect_error((at_loc){3, 10}, "'a' has two drivers (also driven at t.v:1:25)");
}

/* A reg assigned by assign is Yosys's warning, treated as a net. */
static void test_variable_assigned(void) {
    read_src("module top (input a, output y);\n"
             "  reg r;\n"
             "  assign r = a;\n"
             "  assign y = r;\n"
             "endmodule\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    expect_warning((at_loc){3, 10},
                   "'r' is a variable assigned by a continuous assignment; treated as a net");
    TEST_ASSERT_EQUAL_UINT32(1, soft_warnings());
    expect_ir_ok();
}

/* Carried from Task 18's review: {n4[0], n4[0]} is two Y pins on one net; Yosys warns "multiple
 * conflicting drivers", 2D (A4) rejects it, at the second part. */
static void test_duplicate_concat_bits(void) {
    read_failing("module top (input [3:0] c4, output [3:0] n4);\n"
                 "  assign {n4[0], n4[0]} = c4[1:0] + 2'd1;\n"
                 "endmodule\n");
    expect_error((at_loc){2, 18}, "'n4[0]' has two drivers (also driven at t.v:2:11)");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, diag_errors(), et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32(0, count_logic());
}

/* --- uwire, supply, wired nets (EL-26) ------------------------------------------------------- */

/* The elaborated store the design holds (keep level ELABORATED). */
static const odin3_ast *held_elab(void) {
    for (uint32_t i = 0; i < odin3_design_ast_count(rs.design); i++) {
        const odin3_ast *ast = odin3_design_ast_at(rs.design, i);
        if (odin3_ast_form_of(ast) == ODIN3_AST_FORM_ELABORATED) {
            return ast;
        }
    }
    TEST_FAIL_MESSAGE("no elaborated store is held");
    return NULL;
}

/* The first DECLARATOR of the elaborated store named name. */
static odin3_ast_id declarator_named(const char *name) {
    const odin3_ast *ast = held_elab();
    for (uint32_t id = 1; id < odin3_ast_node_end(ast).v; id++) {
        odin3_ast_id node = {id};
        if (odin3_ast_kind_of(ast, node) == ODIN3_AST_DECLARATOR &&
            strcmp(odin3_ast_name_str(ast, node), name) == 0) {
            return node;
        }
    }
    TEST_FAIL_MESSAGE(name);
    return (odin3_ast_id){0};
}

static void read_keep_elab(const char *text) {
    et_src src = {"t.v", text};
    et_req req = {&src, 1, NULL, NULL, NULL};
    et_read_keep(&req, ODIN3_KEEP_AST_ELABORATED, &rs);
}

static void test_uwire_second_driver(void) {
    read_failing("module top (input a, input b, output y);\n"
                 "  uwire u;\n"
                 "  assign u = a;\n"
                 "  assign u = b;\n"
                 "  assign y = u;\n"
                 "endmodule\n");
    expect_error((at_loc){4, 10}, "'u' is a uwire and has a second driver (first at t.v:3:10)");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, diag_errors(), et_log(&rs));
}

static void test_supply_second_driver(void) {
    read_failing("module top (input a, output y);\n"
                 "supply0 s;\n"
                 "assign s = a;\n"
                 "assign y = s;\n"
                 "endmodule\n");
    expect_error((at_loc){3, 8}, "'s' has two drivers (also driven at t.v:2:9)");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, diag_errors(), et_log(&rs));
    expect_const_bit("s", 0, ODIN3_BIT_0);
}

static void test_supply_nets(void) {
    read_clean("module top (output y, output z);\n"
               "  supply1 v;\n"
               "  supply0 g;\n"
               "  assign y = v;\n"
               "  assign z = g;\n"
               "endmodule\n");
    expect_const_bit("v", 0, ODIN3_BIT_1);
    expect_const_bit("g", 0, ODIN3_BIT_0);
    expect_const_bit("y", 0, ODIN3_BIT_1);
    expect_const_bit("z", 0, ODIN3_BIT_0);
    TEST_ASSERT_EQUAL_UINT32(1, count_of("$_CONST1_"));
    TEST_ASSERT_EQUAL_UINT32(1, count_of("$_CONST0_"));
}

static const char k_wired_head[] =
    "module top (input [7:0] a, input [7:0] b, input [7:0] c, output [7:0] y);\n";

/* A `wand` with three whole-net drivers: two $and cells of the net's width, in source order, each
 * with the declarator of the net as its provenance (review M10). */
static void test_wand_three_drivers(void) {
    char text[512];
    TEST_ASSERT_TRUE(
        snprintf(text, sizeof text,
                 "%s  wand [7:0] w;\n  assign w = a;\n  assign w = b;\n  assign w = c;\n"
                 "  assign y = w;\nendmodule\n",
                 k_wired_head) > 0);
    read_keep_elab(text);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32(2, count_of("$and"));
    TEST_ASSERT_EQUAL_UINT32(2, count_logic());
    odin3_node_id first = nth_node("$and", 0);
    odin3_node_id second = nth_node("$and", 1);
    for (uint32_t k = 0; k < 8; k++) {
        TEST_ASSERT_EQUAL_UINT32(wnet("a", k).v, pin_net(first, "A", k).v);
        TEST_ASSERT_EQUAL_UINT32(wnet("b", k).v, pin_net(first, "B", k).v);
        TEST_ASSERT_EQUAL_UINT32(pin_net(first, "Y", k).v, pin_net(second, "A", k).v);
        TEST_ASSERT_EQUAL_UINT32(wnet("c", k).v, pin_net(second, "B", k).v);
        TEST_ASSERT_EQUAL_UINT32(wnet("w", k).v, pin_net(second, "Y", k).v);
    }
    TEST_ASSERT_EQUAL_INT64(8, param_of(first, "A_WIDTH"));
    TEST_ASSERT_EQUAL_INT64(8, param_of(second, "Y_WIDTH"));
    expect_same_nets((bits_of){"y", 0}, (bits_of){"w", 0}, 8);
    odin3_ast_id decl = declarator_named("w");
    for (uint32_t i = 0; i < 2; i++) {
        const odin3_prov_record *rec =
            odin3_prov_get(rs.design, odin3_node_prov(top(), nth_node("$and", i)));
        TEST_ASSERT_NOT_NULL(rec);
        TEST_ASSERT_EQUAL_UINT32(decl.v, rec->ast);
    }
    expect_ir_ok();
}

/* Drivers of part of the net: one-bit $or chains on the bits two drivers cover; a bit with one
 * driver is that driver's net. */
static void test_wor_partial_drivers(void) {
    char text[512];
    TEST_ASSERT_TRUE(snprintf(text, sizeof text,
                              "%s  wor [7:0] w;\n  assign w[3:0] = a[3:0];\n  assign w[7:0] = b;\n"
                              "  assign y = w;\nendmodule\n",
                              k_wired_head) > 0);
    read_src(text);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32(4, count_of("$or"));
    TEST_ASSERT_EQUAL_UINT32(4, count_logic());
    for (uint32_t k = 0; k < 4; k++) {
        odin3_node_id cell = nth_node("$or", k);
        TEST_ASSERT_EQUAL_INT64(1, param_of(cell, "Y_WIDTH"));
        TEST_ASSERT_EQUAL_UINT32(wnet("a", k).v, pin_net(cell, "A", 0).v);
        TEST_ASSERT_EQUAL_UINT32(wnet("b", k).v, pin_net(cell, "B", 0).v);
        TEST_ASSERT_EQUAL_UINT32(wnet("w", k).v, pin_net(cell, "Y", 0).v);
    }
    for (uint32_t k = 4; k < 8; k++) {
        TEST_ASSERT_EQUAL_UINT32(wnet("b", k).v, wnet("w", k).v);
    }
    expect_ir_ok();
}

static void test_wand_one_driver(void) {
    char text[512];
    TEST_ASSERT_TRUE(snprintf(text, sizeof text,
                              "%s  wand [7:0] w;\n  assign w = a;\n  assign y = w;\nendmodule\n",
                              k_wired_head) > 0);
    read_src(text);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32(0, count_logic());
    expect_same_nets((bits_of){"w", 0}, (bits_of){"a", 0}, 8);
    expect_same_nets((bits_of){"y", 0}, (bits_of){"a", 0}, 8);
    expect_ir_ok();
}

/* A cell's output on a wired net is a driver too. */
static void test_wand_with_cell_driver(void) {
    char text[512];
    TEST_ASSERT_TRUE(snprintf(text, sizeof text,
                              "%s  wand [7:0] w;\n  assign w = a + b;\n  assign w = c;\n"
                              "  assign y = w;\nendmodule\n",
                              k_wired_head) > 0);
    read_src(text);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32(1, count_of("$add"));
    TEST_ASSERT_EQUAL_UINT32(1, count_of("$and"));
    odin3_node_id and_cell = only_node("$and");
    odin3_node_id add = only_node("$add");
    for (uint32_t k = 0; k < 8; k++) {
        TEST_ASSERT_EQUAL_UINT32(pin_net(add, "Y", k).v, pin_net(and_cell, "A", k).v);
        TEST_ASSERT_EQUAL_UINT32(wnet("w", k).v, pin_net(and_cell, "Y", k).v);
    }
    expect_ir_ok();
}

/* A procedural assignment to a wired net is the pre-scan's error (the always block itself is
 * Task 21's, so its pending line follows). */
static void test_wired_always(void) {
    read_failing("module top (input a, output y);\n"
                 "  wand w;\n"
                 "  always @* w = a;\n"
                 "  assign y = w;\n"
                 "endmodule\n");
    expect_error((at_loc){3, 13},
                 "'w' is a wired net (wand) and cannot be assigned in an always block");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(2, diag_errors(), et_log(&rs));
}

/* --- the always-assigned bit set (EL-27's input) ------------------------------------------------
 */

enum { PROBE_R2 = 1, PROBE_R1 = 2, PROBE_R7 = 4, PROBE_S = 8, PROBE_T = 16, PROBE_U = 32 };

typedef struct alw_probe {
    uint32_t calls, mask; /* PROBE_*: the object bits the pre-scan marked */
} alw_probe;

static bool assigned_bit(odin3_elab2 *e2, odin3_ast_id module, const char *name, uint32_t bit) {
    uint32_t key = 0;
    TEST_ASSERT_TRUE(
        odin3_strtab_find(odin3_design_strtab(e2->design), odin3_bytes_cstr(name), &key));
    odin3_sym_id sym = odin3_symtab_lookup(e2->st, odin3_elab_scope_of(&e2->bind, module), key);
    TEST_ASSERT_NOT_EQUAL_UINT32(0, sym.v);
    return odin3_elab2_always_assigned(e2, sym, bit);
}

static void probe_prescan(void *user, odin3_elab2 *e2, odin3_ast_id module) {
    alw_probe *probe = user;
    probe->calls++;
    probe->mask =
        (assigned_bit(e2, module, "r", 2) ? PROBE_R2 : 0U) |
        (assigned_bit(e2, module, "r", 1) ? PROBE_R1 : 0U) |
        (assigned_bit(e2, module, "r", 7) ? PROBE_R7 : 0U) |
        (assigned_bit(e2, module, "s", 0) && assigned_bit(e2, module, "s", 3) ? PROBE_S : 0U) |
        (assigned_bit(e2, module, "t", 0) && assigned_bit(e2, module, "t", 3) ? PROBE_T : 0U) |
        (assigned_bit(e2, module, "u", 0) ? PROBE_U : 0U);
}

/* A constant select marks its bit, a plain assignment all bits, a non-constant index all bits. */
static void test_always_assigned_bits(void) {
    alw_probe probe = {0, 0};
    odin3_elab_test_hooks hooks = {0, NULL, NULL, 0, probe_prescan};
    hooks.user = &probe;
    odin3_elab_test_set_hooks(&hooks);
    read_src("module top (input a, input [1:0] k, output y);\n"
             "  reg [7:0] r;\n  reg [3:0] s;\n  reg [3:0] t;\n  reg u;\n"
             "  always @* begin r[2] = a; r[7:6] = 2'b01; s = 4'd1; t[k] = a; end\n"
             "  assign y = r[0];\nendmodule\n");
    odin3_elab_test_set_hooks(NULL);
    TEST_ASSERT_EQUAL_UINT32(1, probe.calls);
    TEST_ASSERT_EQUAL_HEX32(PROBE_R2 | PROBE_R7 | PROBE_S | PROBE_T, probe.mask);
}

/* --- tri-state (D3) and child inouts (F22) ------------------------------------------------------
 */

#define TRI_HEAD "module top (input [7:0] d, input en, output [7:0] y);\n"

/* c ? d : 'bz on a top-level output is a $tribuf: A = d, EN = en, Y = y, WIDTH 8. */
static void test_tribuf_assign(void) {
    read_clean(TRI_HEAD "  assign y = en ? d : 'bz;\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32(1, count_logic());
    odin3_node_id tri = only_node("$tribuf");
    TEST_ASSERT_EQUAL_INT64(8, param_of(tri, "WIDTH"));
    expect_pins_are(tri, "A", (bits_of){"d", 0}, 8);
    expect_pins_are(tri, "Y", (bits_of){"y", 0}, 8);
    TEST_ASSERT_EQUAL_UINT32(wnet("en", 0).v, pin_net(tri, "EN", 0).v);
}

/* The branches' order sets an inverted enable through $not. */
static void test_tribuf_inverted(void) {
    read_clean(TRI_HEAD "  assign y = en ? 'bz : d;\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32(2, count_logic());
    odin3_node_id tri = only_node("$tribuf");
    odin3_node_id inv = only_node("$not");
    TEST_ASSERT_EQUAL_UINT32(wnet("en", 0).v, pin_net(inv, "A", 0).v);
    TEST_ASSERT_EQUAL_UINT32(pin_net(inv, "Y", 0).v, pin_net(tri, "EN", 0).v);
    expect_pins_are(tri, "A", (bits_of){"d", 0}, 8);
    expect_pins_are(tri, "Y", (bits_of){"y", 0}, 8);
}

/* A wider condition is reduce_bool'd. */
static void test_tribuf_wide_condition(void) {
    read_clean("module top (input [7:0] d, input [1:0] en, output [7:0] y);\n"
               "  assign y = en ? d : 'bz;\nendmodule\n");
    odin3_node_id red = only_node("$reduce_bool");
    odin3_node_id tri = only_node("$tribuf");
    TEST_ASSERT_EQUAL_UINT32(pin_net(red, "Y", 0).v, pin_net(tri, "EN", 0).v);
}

/* bufif0 enables on a low control; notif1 inverts the data (Q3). */
static void test_bufif_notif(void) {
    read_clean("module top (input d1, input e1, output y1, input d2, input e2, output y2);\n"
               "  bufif0 (y1, d1, e1);\n"
               "  notif1 (y2, d2, e2);\n"
               "endmodule\n");
    TEST_ASSERT_EQUAL_UINT32(2, count_of("$tribuf"));
    TEST_ASSERT_EQUAL_UINT32(2, count_of("$not"));
    odin3_node_id first = nth_node("$tribuf", 0);
    odin3_node_id second = nth_node("$tribuf", 1);
    odin3_node_id not_en = nth_node("$not", 0);
    odin3_node_id not_data = nth_node("$not", 1);
    TEST_ASSERT_EQUAL_UINT32(wnet("e1", 0).v, pin_net(not_en, "A", 0).v);
    TEST_ASSERT_EQUAL_UINT32(pin_net(not_en, "Y", 0).v, pin_net(first, "EN", 0).v);
    TEST_ASSERT_EQUAL_UINT32(wnet("d1", 0).v, pin_net(first, "A", 0).v);
    TEST_ASSERT_EQUAL_UINT32(wnet("y1", 0).v, pin_net(first, "Y", 0).v);
    TEST_ASSERT_EQUAL_UINT32(wnet("d2", 0).v, pin_net(not_data, "A", 0).v);
    TEST_ASSERT_EQUAL_UINT32(pin_net(not_data, "Y", 0).v, pin_net(second, "A", 0).v);
    TEST_ASSERT_EQUAL_UINT32(wnet("e2", 0).v, pin_net(second, "EN", 0).v);
    TEST_ASSERT_EQUAL_UINT32(wnet("y2", 0).v, pin_net(second, "Y", 0).v);
}

/* Several tri-state drivers on a top-level port are a bus: legal, and the IR check agrees. */
static void test_two_tribufs(void) {
    read_clean("module top (input [7:0] d, input [7:0] e, input en1, input en2, output [7:0] y);\n"
               "  assign y = en1 ? d : 'bz;\n"
               "  assign y = en2 ? e : 'bz;\n"
               "endmodule\n");
    TEST_ASSERT_EQUAL_UINT32(2, count_of("$tribuf"));
    expect_pins_are(nth_node("$tribuf", 0), "Y", (bits_of){"y", 0}, 8);
    expect_pins_are(nth_node("$tribuf", 1), "Y", (bits_of){"y", 0}, 8);
}

/* An inout port of the top: its own pin and the tribuf share the net (rule 4). */
static void test_tribuf_on_inout(void) {
    read_clean("module top (input d, input en, inout io);\n"
               "  assign io = en ? d : 'bz;\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32(1, count_of("$tribuf"));
}

/* Any other driver together with a $tribuf on one port is A4's error, in either order. */
static void test_assign_and_tribuf(void) {
    read_failing(TRI_HEAD "  assign y = d;\n  assign y = en ? d : 'bz;\nendmodule\n");
    expect_error((at_loc){3, 10}, "'y' has two drivers (also driven at t.v:2:10)");
    et_free(&rs);
    memset(&rs, 0, sizeof rs);
    read_failing(TRI_HEAD "  assign y = en ? d : 'bz;\n  assign y = d;\nendmodule\n");
    expect_error((at_loc){3, 10}, "'y' has two drivers (also driven at t.v:2:10)");
}

/* An internal net cannot be tri-state (D3), and neither can a port of a module below the top. */
static void test_tribuf_internal(void) {
    read_failing(TRI_HEAD
                 "  wire t;\n  assign t = en ? d[0] : 'bz;\n  assign y = {7'b0, t};\nendmodule\n");
    expect_error(
        (at_loc){3, 10},
        "tri-state driver on 't' is supported only on a top-level output or inout port (D3)");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, diag_errors(), et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32(0, count_of("$tribuf"));
    et_free(&rs);
    memset(&rs, 0, sizeof rs);
    read_failing("module sub (input d, input en, output y);\n"
                 "  assign y = en ? d : 'bz;\nendmodule\n"
                 "module top (input d, input en, output y);\n"
                 "  sub u (.d(d), .en(en), .y(y));\nendmodule\n");
    expect_error(
        (at_loc){2, 10},
        "tri-state driver on 'y' is supported only on a top-level output or inout port (D3)");
}

/* A lone 'bz is the constant, wherever. */
static void test_z_alone(void) {
    read_clean("module top (output [3:0] y);\n  assign y = 'bz;\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32(0, count_logic());
    for (uint32_t bit = 0; bit < 4; bit++) {
        expect_const_bit("y", bit, ODIN3_BIT_Z);
    }
}

static const char k_child[] = "module sub (inout p);\nendmodule\n";

/* Two child inout pins on one net are a bus; nothing else drives it. */
static void test_child_inouts(void) {
    read_src(
        "module sub (inout p);\nendmodule\n"
        "module top (input a);\n  wire bus;\n  sub u (.p(bus));\n  sub v (.p(bus));\nendmodule\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32(0, diag_errors());
    expect_ir_ok();
}

/* An inout pin and ordinary logic on one net is an internal tri-state net (F22). */
static void test_child_inout_and_logic(void) {
    char text[256];
    TEST_ASSERT_TRUE(snprintf(text, sizeof text,
                              "%smodule top (input a);\n  wire bus;\n  sub u (.p(bus));\n"
                              "  assign bus = a;\nendmodule\n",
                              k_child) > 0);
    read_failing(text);
    expect_error((at_loc){6, 10},
                 "net 'bus' is driven by inout port 'p' of instance 'u' and by logic at t.v:6:10; "
                 "internal tri-state nets are not supported (D3)");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, diag_errors(), et_log(&rs));
}

/* --- instances (EL-10, EL-11) -------------------------------------------------------------------
 */

#define SUB8 "module sub (input [7:0] a, output [7:0] o);\nendmodule\n"
#define SUB8S "module sub (input [7:0] a, output signed [7:0] o);\nendmodule\n"

/* The instance node named name in top. */
static odin3_node_id instance_node(const char *name) {
    odin3_node_id node = odin3_module_find_node(top(), id_of(name));
    TEST_ASSERT_NOT_EQUAL_UINT32_MESSAGE(0, node.v, name);
    return node;
}

/* Pin `bit` of port `port` of the instance node. */
static odin3_net_id inst_pin(const char *name, const char *port, uint32_t bit) {
    return pin_net(instance_node(name), port, bit);
}

/* An input narrower than its port is zero-extended, with Yosys's warning at the connection. */
static void test_input_extended(void) {
    read_src(SUB8 "module top (input [3:0] n4, output [7:0] y);\n"
                  "  sub u (.a(n4), .o(y));\nendmodule\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    expect_warning((at_loc){4, 10},
                   "port 'a' of instance 'u' (module 'sub') is 8 bits; the connection of 4 bits "
                   "is extended");
    TEST_ASSERT_EQUAL_UINT32(1, soft_warnings());
    for (uint32_t k = 0; k < 4; k++) {
        TEST_ASSERT_EQUAL_UINT32(wnet("n4", k).v, inst_pin("u", "a", k).v);
    }
    for (uint32_t k = 4; k < 8; k++) {
        TEST_ASSERT_EQUAL_UINT32(const_net_of(ODIN3_BIT_0).v, inst_pin("u", "a", k).v);
    }
    TEST_ASSERT_EQUAL_UINT32(wnet("y", 3).v, inst_pin("u", "o", 3).v);
    expect_ir_ok();
}

static void test_input_sign_extended(void) {
    read_src(SUB8 "module top (input signed [3:0] t, output [7:0] y);\n"
                  "  sub u (.a(t), .o(y));\nendmodule\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    for (uint32_t k = 4; k < 8; k++) {
        TEST_ASSERT_EQUAL_UINT32(wnet("t", 3).v, inst_pin("u", "a", k).v);
    }
    expect_ir_ok();
}

static void test_input_truncated(void) {
    read_src(SUB8 "module top (input [11:0] big, output [7:0] y);\n"
                  "  sub u (.a(big), .o(y));\nendmodule\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    expect_warning((at_loc){4, 10},
                   "port 'a' of instance 'u' (module 'sub') is 8 bits; the connection of 12 bits "
                   "is truncated");
    for (uint32_t k = 0; k < 8; k++) {
        TEST_ASSERT_EQUAL_UINT32(wnet("big", k).v, inst_pin("u", "a", k).v);
    }
}

/* A constant input is resized without a warning. */
static void test_input_constant(void) {
    read_src(SUB8 "module top (output [7:0] y);\n  sub u (.a(4'd3), .o(y));\nendmodule\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32(0, soft_warnings());
    TEST_ASSERT_EQUAL_UINT32(const_net_of(ODIN3_BIT_1).v, inst_pin("u", "a", 0).v);
    TEST_ASSERT_EQUAL_UINT32(const_net_of(ODIN3_BIT_1).v, inst_pin("u", "a", 1).v);
    for (uint32_t k = 2; k < 8; k++) {
        TEST_ASSERT_EQUAL_UINT32(const_net_of(ODIN3_BIT_0).v, inst_pin("u", "a", k).v);
    }
}

/* An output into a wider net: the extra bits are the port's top bit when the child declares the
 * port signed, else 0. */
static void test_output_wider(void) {
    read_src(SUB8 "module top (input [7:0] a, output [11:0] y);\n"
                  "  sub u (.a(a), .o(y));\nendmodule\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    expect_warning((at_loc){4, 17},
                   "port 'o' of instance 'u' (module 'sub') is 8 bits; the connection of 12 bits "
                   "is truncated");
    for (uint32_t k = 0; k < 8; k++) {
        TEST_ASSERT_EQUAL_UINT32(wnet("y", k).v, inst_pin("u", "o", k).v);
    }
    for (uint32_t k = 8; k < 12; k++) {
        expect_const_bit("y", k, ODIN3_BIT_0);
    }
    expect_ir_ok();
    et_free(&rs);
    memset(&rs, 0, sizeof rs);
    read_src(SUB8S "module top (input [7:0] a, output [11:0] y);\n"
                   "  sub u (.a(a), .o(y));\nendmodule\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    for (uint32_t k = 8; k < 12; k++) {
        TEST_ASSERT_EQUAL_UINT32(wnet("y", 7).v, wnet("y", k).v);
    }
    expect_ir_ok();
}

/* A narrower output: the extra port bits go to fresh nets this node drives. */
static void test_output_narrower(void) {
    read_src(SUB8 "module top (input [7:0] a, output [3:0] y);\n"
                  "  sub u (.a(a), .o(y));\nendmodule\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    expect_warning((at_loc){4, 17},
                   "port 'o' of instance 'u' (module 'sub') is 8 bits; the connection of 4 bits "
                   "is extended");
    for (uint32_t k = 4; k < 8; k++) {
        odin3_net_id net = inst_pin("u", "o", k);
        TEST_ASSERT_NOT_EQUAL_UINT32(0, net.v);
        TEST_ASSERT_EQUAL_UINT32(0, odin3_net_primary(top(), net).wire.v);
        TEST_ASSERT_EQUAL_UINT32(instance_node("u").v,
                                 odin3_pin_node(top(), odin3_net_driver(top(), net)).v);
    }
    expect_ir_ok();
}

/* An unconnected input is x; an unconnected output has fresh nets. */
static void test_unconnected_ports(void) {
    read_src(SUB8 "module top;\n  sub u (.a(), .o());\nendmodule\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32(0, soft_warnings());
    for (uint32_t k = 0; k < 8; k++) {
        TEST_ASSERT_EQUAL_UINT32(const_net_of(ODIN3_BIT_X).v, inst_pin("u", "a", k).v);
        TEST_ASSERT_EQUAL_UINT32(
            instance_node("u").v,
            odin3_pin_node(top(), odin3_net_driver(top(), inst_pin("u", "o", k))).v);
    }
    expect_ir_ok();
}

/* Ordered connections and an empty one. */
static void test_ordered_connections(void) {
    read_src(SUB8 "module top (input [7:0] a, output [7:0] y);\n  sub u (a, y);\nendmodule\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32(wnet("a", 5).v, inst_pin("u", "a", 5).v);
    TEST_ASSERT_EQUAL_UINT32(wnet("y", 5).v, inst_pin("u", "o", 5).v);
}

/* Two outputs on one net are A4's error; nothing of the instance is made. */
static void test_instance_output_conflict(void) {
    read_failing(SUB8 "module top (input [7:0] a, output [7:0] y);\n"
                      "  sub u (.a(a), .o(y));\n  sub v (.a(a), .o(y));\nendmodule\n");
    expect_error((at_loc){5, 20}, "'y' has two drivers (also driven at t.v:4:20)");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, diag_errors(), et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_module_find_node(top(), id_of("v")).v);
}

/* A module instance keeps its parameters as BITS attributes `parameter:<NAME>`. */
static void test_module_parameters_as_attributes(void) {
    read_src("module sub #(parameter W = 4, parameter S = \"ab\") (input [W-1:0] a);\nendmodule\n"
             "module top (input [7:0] x);\n  sub #(.W(8)) u (.a(x));\nendmodule\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    odin3_objref obj = {ODIN3_OBJ_NODE, instance_node("u").v};
    const odin3_value *width = odin3_attr_get(top(), obj, id_of("parameter:W"));
    TEST_ASSERT_NOT_NULL(width);
    TEST_ASSERT_EQUAL_INT(ODIN3_VAL_BITS, width->kind);
    TEST_ASSERT_EQUAL_UINT32(32, width->len);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_BIT_1, width->bits[3]);
    TEST_ASSERT_EQUAL_UINT8(ODIN3_BIT_0, width->bits[0]);
    const odin3_value *str = odin3_attr_get(top(), obj, id_of("parameter:S"));
    TEST_ASSERT_NOT_NULL(str);
    TEST_ASSERT_EQUAL_INT(ODIN3_VAL_STRING, str->kind);
    expect_ir_ok();
}

/* --- cell types and black boxes (EL-11) ----------------------------------------------------------
 */

static int64_t node_param(odin3_node_id node, const char *name) {
    return param_of(node, name);
}

/* A registered type's parameters not given are inferred from the connection widths. */
static void test_cell_type_inference(void) {
    read_techlib_src(
        "module top (input clk, input we, input [9:0] addr, input [7:0] data, output [7:0] q);\n"
        "  single_port_ram ram (.clk(clk), .we(we), .addr(addr), .data(data), .out(q));\n"
        "endmodule\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    odin3_node_id ram = only_node("single_port_ram");
    TEST_ASSERT_EQUAL_INT64(10, node_param(ram, "ADDR_WIDTH"));
    TEST_ASSERT_EQUAL_INT64(8, node_param(ram, "DATA_WIDTH"));
    expect_pins_are(ram, "addr", (bits_of){"addr", 0}, 10);
    expect_pins_are(ram, "out", (bits_of){"q", 0}, 8);
    expect_ir_ok();
}

/* A given parameter wins over the inferred one; the others are still inferred. */
static void test_cell_type_given_parameter(void) {
    read_techlib_src(
        "module top (input clk, input we, input [9:0] addr, input [7:0] data, output [7:0] q);\n"
        "  single_port_ram #(.ADDR_WIDTH(10)) ram (.clk(clk), .we(we), .addr(addr), "
        ".data(data), .out(q));\nendmodule\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    odin3_node_id ram = only_node("single_port_ram");
    TEST_ASSERT_EQUAL_INT64(10, node_param(ram, "ADDR_WIDTH"));
    TEST_ASSERT_EQUAL_INT64(8, node_param(ram, "DATA_WIDTH"));
}

static uint32_t blackbox_types(void) {
    return odin3_design_declared_model_count(rs.design);
}

/* A `(* blackbox *)` definition: one cell type under its base name, declared once, each cell with
 * its INSTANTIATION's values and the declared widths; never an IR module. */
static void test_blackbox_base_name(void) {
    read_src("(* blackbox *) module bb #(parameter W = 4) (input [W-1:0] a, output [W-1:0] o);\n"
             "endmodule\n"
             "module top (input [3:0] x, input [3:0] y, output [3:0] ox, output [3:0] oy);\n"
             "  bb #(.W(4)) u1 (.a(x), .o(ox));\n"
             "  bb u2 (.a(y), .o(oy));\nendmodule\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32(0, soft_warnings());
    TEST_ASSERT_EQUAL_UINT32(2, odin3_design_module_end(rs.design)); /* top only */
    TEST_ASSERT_EQUAL_UINT32(1, blackbox_types());
    odin3_celltype_id type = odin3_design_declared_model(rs.design, 0);
    const odin3_celltype_def *def = odin3_celltype_get(rs.design, type);
    TEST_ASSERT_EQUAL_STRING("bb", def->name);
    TEST_ASSERT_EQUAL_INT(ODIN3_GRAN_BLACKBOX, def->gran);
    odin3_node_id u1 = instance_node("u1");
    odin3_node_id u2 = instance_node("u2");
    TEST_ASSERT_EQUAL_UINT32(type.v, odin3_node_type(top(), u1).v);
    TEST_ASSERT_EQUAL_INT64(4, node_param(u1, "W"));
    TEST_ASSERT_EQUAL_INT64(4, node_param(u2, "W"));
    expect_pins_are(u1, "a", (bits_of){"x", 0}, 4);
    expect_pins_are(u2, "o", (bits_of){"oy", 0}, 4);
    expect_ir_ok();
}

/* Ports without a range cannot depend on a parameter, and a string value does not touch a range:
 * those overrides are exact. */
static void test_blackbox_override_exact_cases(void) {
    read_src("(* blackbox *) module fx #(parameter MODE = 1) (input a, output o);\nendmodule\n"
             "(* blackbox *) module st #(parameter NAME = \"x\") (input [7:0] a, output [7:0] o);\n"
             "endmodule\n"
             "module top (input [7:0] x, output o1, output [7:0] o2);\n"
             "  fx #(.MODE(2)) u1 (.a(x[0]), .o(o1));\n"
             "  st #(.NAME(\"y\")) u2 (.a(x), .o(o2));\nendmodule\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    TEST_ASSERT_EQUAL_INT64(2, node_param(instance_node("u1"), "MODE"));
    expect_pins_are(instance_node("u2"), "a", (bits_of){"x", 0}, 8);
    expect_ir_ok();
}

#define BB_TOP(ports, params, conns)                                                               \
    "(* blackbox *) module bb " params " (" ports ");\nendmodule\n"                                \
    "module top (input [15:0] x, input [7:0] kk, output [15:0] y, output [7:0] y8);\n" conns       \
    "endmodule\n"

/* --- exact black-box port widths per parameter set (Peter decision 3, spec v4.19) --------------
 *
 * Yosys 0.55 keeps one `module \bb` (the defaults' widths) and cells `\bb` whose connections have
 * the widths of their own parameter set: `hierarchy` derives the module for the width check and
 * drops it (write_rtlil: `cell \bb \u1` with `parameter signed \W 16` and `connect \a \x` of 16
 * bits beside `module \bb` with `wire width 8 input 1 \a`). Commands behind the expected widths:
 *   yosys -q -p "read_verilog t.v; hierarchy -top top; write_rtlil"
 * with the t.v of each test below.
 */

static void slurp(const char *path, char *out, size_t cap);

/* The pins of port `port` of the node: how many. */
static uint32_t port_width(odin3_node_id node, const char *port) {
    const odin3_celltype_def *def = def_in(top(), node);
    for (uint32_t index = 0; index < def->n_ports; index++) {
        if (strcmp(def->ports[index].name, port) == 0) {
            return odin3_node_port(top(), node, index).count;
        }
    }
    TEST_FAIL_MESSAGE(port);
    return 0;
}

static void expect_widths(const char *inst, const char *port_a, uint32_t wa, const char *port_b,
                          uint32_t wb) {
    odin3_node_id node = instance_node(inst);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(wa, port_width(node, port_a), inst);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(wb, port_width(node, port_b), inst);
}

/* (default) an instance without an override and one with `.W(16)`, a `[W-1:0]` port. */
static void test_bbw_range_param(void) {
    read_clean(BB_TOP("input [W-1:0] a, output [W-1:0] o", "#(parameter W = 8)",
                      "  bb u1 (.a(x[7:0]), .o(y8));\n  bb #(.W(16)) u2 (.a(x), .o(y));\n"));
    TEST_ASSERT_EQUAL_UINT32(1, blackbox_types());
    expect_widths("u1", "a", 8, "o", 8);
    expect_widths("u2", "a", 16, "o", 16);
    TEST_ASSERT_EQUAL_INT64(8, node_param(instance_node("u1"), "W"));
    TEST_ASSERT_EQUAL_INT64(16, node_param(instance_node("u2"), "W"));
    expect_pins_are(instance_node("u2"), "a", (bits_of){"x", 0}, 16);
    expect_pins_are(instance_node("u2"), "o", (bits_of){"y", 0}, 16);
}

/* `[W:0]` and `[2*W-1:0]` follow the override; the fixed `[7:0]` port beside them stays 8 bits. */
static void test_bbw_formulas_and_fixed_port(void) {
    read_clean("(* blackbox *) module bb #(parameter W = 8) (input [W:0] a, output [2*W-1:0] o,\n"
               "                                             input [7:0] k);\nendmodule\n"
               "module top (input [16:0] x, output [31:0] y, input [7:0] k, output [15:0] y8);\n"
               "  bb #(.W(16)) u1 (.a(x), .o(y), .k(k));\n"
               "  bb u2 (.a(x[8:0]), .o(y8), .k(k));\nendmodule\n");
    expect_widths("u1", "a", 17, "o", 32);
    TEST_ASSERT_EQUAL_UINT32(8, port_width(instance_node("u1"), "k"));
    expect_widths("u2", "a", 9, "o", 16);
    TEST_ASSERT_EQUAL_UINT32(8, port_width(instance_node("u2"), "k"));
}

/* Two parameters with one default, only one overridden: each port follows its own parameter. */
static void test_bbw_shared_default(void) {
    read_clean(BB_TOP("input [WA-1:0] a, input [2*WB-1:0] b, output o",
                      "#(parameter WA = 8, WB = 8)",
                      "  bb #(.WB(16)) u1 (.a(x[7:0]), .b({x, x}), .o(y[0]));\n"
                      "  bb u2 (.a(x[7:0]), .b(x), .o(y[1]));\n"));
    expect_widths("u1", "a", 8, "b", 32);
    expect_widths("u2", "a", 8, "b", 16);
}

/* Three instances of different widths in one module: no cross-talk between them. */
static void test_bbw_no_cross_talk(void) {
    read_clean(BB_TOP("input [W-1:0] a, output [W-1:0] o", "#(parameter W = 8)",
                      "  bb #(.W(4)) u1 (.a(x[3:0]), .o(y[3:0]));\n"
                      "  bb #(.W(16)) u2 (.a(x), .o());\n"
                      "  bb #(.W(8)) u3 (.a(kk), .o(y8));\n"));
    expect_widths("u1", "a", 4, "o", 4);
    expect_widths("u2", "a", 16, "o", 16);
    expect_widths("u3", "a", 8, "o", 8);
}

/* A string override does not touch the ranges; another parameter's override does. */
static void test_bbw_string_override(void) {
    read_clean("(* blackbox *) module bb #(parameter W = 8, parameter S = \"x\")\n"
               "  (input [W-1:0] a, output [7:0] o);\nendmodule\n"
               "module top (input [7:0] x, output [7:0] y, output [7:0] y2);\n"
               "  bb #(.S(\"y\")) u1 (.a(x), .o(y));\n"
               "  bb #(.W(4), .S(\"z\")) u2 (.a(x[3:0]), .o(y2));\nendmodule\n");
    expect_widths("u1", "a", 8, "o", 8);
    expect_widths("u2", "a", 4, "o", 8);
}

/* An instance array: the default's elements are 8 bits each (as Yosys); an overridden array
 * slices by its own widths (Yosys slices by the default module's, spec 17 #46). */
static void test_bbw_instance_array(void) {
    read_clean("(* blackbox *) module bb #(parameter W = 8) (input [W-1:0] a, output [W-1:0] o);\n"
               "endmodule\n"
               "module top (input [31:0] x, output [31:0] y, input [31:0] x2, output [31:0] y2);\n"
               "  bb u [3:0] (.a(x), .o(y));\n"
               "  bb #(.W(16)) v [1:0] (.a(x2), .o(y2));\nendmodule\n");
    expect_widths("u[0]", "a", 8, "o", 8);
    expect_widths("u[3]", "a", 8, "o", 8);
    expect_widths("v[0]", "a", 16, "o", 16);
    expect_pins_are(instance_node("v[1]"), "a", (bits_of){"x2", 16}, 16);
}

/* Two parameter sets with equal values (4'd8 and 5'd8) whose port range depends on the literal's
 * size (`~W` is as wide as W) give the port two widths: the cell's parameter cannot tell them
 * apart, so it is a located error, not a guess. */
static void test_bbw_equal_values_other_width(void) {
    read_failing("(* blackbox *) module bb #(parameter W = 4'd6) (input [(~W)>>2:0] a);\n"
                 "endmodule\n"
                 "module top (input [7:0] x);\n"
                 "  bb #(.W(4'd8)) u1 (.a(x[1:0]));\n  bb #(.W(5'd8)) u2 (.a(x[5:0]));\n"
                 "endmodule\n");
    expect_error((at_loc){1, 16}, "black box 'bb': port 'a' has different widths in parameter sets "
                                  "with equal values");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, diag_errors(), et_log(&rs));
}

/* A connection narrower than the overridden port is extended to the port's exact width, with
 * the resize warning (Yosys: "Resizing cell port top.u1.a from 8 bits to 16 bits."). */
static void test_bbw_connection_resized(void) {
    read_src(BB_TOP("input [W-1:0] a, output [W-1:0] o", "#(parameter W = 8)",
                    "  bb #(.W(16)) u1 (.a(x[7:0]), .o(y));\n"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    expect_warning((at_loc){4, 20},
                   "port 'a' of instance 'u1' (module 'bb') is 16 bits; the connection of 8 bits "
                   "is extended");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, soft_warnings(), et_log(&rs));
    expect_widths("u1", "a", 16, "o", 16);
    expect_ir_ok();
}

/* Two parameterised black boxes in one source, each with a default and an overridden instance
 * (a type per definition; widths per set, no cross-talk between the two definitions).
 *   yosys -q -p "read_verilog t.v; hierarchy -top top; write_rtlil"  (the t.v of this test):
 * cells \bb \u1, \bb \u2 (`parameter signed \W 16`), \cc \v1, \cc \v2 (`\V 16`) with
 * connections of 8, 16, 5, 17 (a) and 8, 16, 8, 32 (o) bits. */
static void test_bbw_two_definitions(void) {
    read_clean("(* blackbox *) module bb #(parameter W = 8) (input [W-1:0] a, output [W-1:0] o);\n"
               "endmodule\n"
               "(* blackbox *) module cc #(parameter V = 4) (input [V:0] a, output [2*V-1:0] o);\n"
               "endmodule\n"
               "module top (input [16:0] x, output [15:0] y, output [31:0] z, output [7:0] y8,\n"
               "            output [7:0] z8);\n"
               "  bb u1 (.a(x[7:0]), .o(y8));\n  bb #(.W(16)) u2 (.a(x[15:0]), .o(y));\n"
               "  cc v1 (.a(x[4:0]), .o(z8));\n  cc #(.V(16)) v2 (.a(x), .o(z));\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32(2, blackbox_types());
    expect_widths("u1", "a", 8, "o", 8);
    expect_widths("u2", "a", 16, "o", 16);
    expect_widths("v1", "a", 5, "o", 8);
    expect_widths("v2", "a", 17, "o", 32);
}

/* A black box inside a parameterised parent: two specializations of the parent, each with its
 * own black-box set (W = 8 and W = 16), one cell type.
 *   yosys -q -p "read_verilog t.v; hierarchy -top top; write_rtlil" gives
 * `$paramod\par\P=s32'...1000` with `cell \bb \u` (`\W 8`, a/o of 8 bits) and
 * `$paramod\par\P=s32'...10000` with `cell \bb \u` (`\W 16`, 16 bits), one `module \bb`. */
static void test_bbw_parameterised_parent(void) {
    read_clean("(* blackbox *) module bb #(parameter W = 8) (input [W-1:0] a, output [W-1:0] o);\n"
               "endmodule\n"
               "module par #(parameter P = 8) (input [15:0] x, output [15:0] y);\n"
               "  bb #(.W(P)) u (.a(x[P-1:0]), .o(y[P-1:0]));\nendmodule\n"
               "module top (input [15:0] x, output [15:0] y, output [15:0] y2);\n"
               "  par #(.P(8)) p1 (.x(x), .y(y));\n  par #(.P(16)) p2 (.x(x), .y(y2));\n"
               "endmodule\n");
    TEST_ASSERT_EQUAL_UINT32(1, blackbox_types());
    uint32_t seen = 0;
    for (uint32_t index = 1; index < odin3_design_module_end(rs.design); index++) {
        odin3_module *module = odin3_module_get(rs.design, (odin3_module_id){index});
        odin3_node_id node = odin3_module_find_node(module, id_of("u"));
        if (node.v == 0) {
            continue;
        }
        int64_t width = param_in(module, node, "W");
        TEST_ASSERT_TRUE(width == 8 || width == 16);
        TEST_ASSERT_EQUAL_UINT32((uint32_t)width, odin3_node_port(module, node, 0).count);
        TEST_ASSERT_EQUAL_UINT32((uint32_t)width, odin3_node_port(module, node, 1).count);
        seen |= width == 8 ? 1U : 2U;
    }
    TEST_ASSERT_EQUAL_UINT32(3, seen);
}

/* write_rtlil carries each cell's own widths and the module of the defaults, twice the same. */
static void test_bbw_rtlil(void) {
    static char first[TEXT_MAX];
    static char second[TEXT_MAX];
    static const char text[] =
        "(* blackbox *) module bb #(parameter W = 8) (input [W-1:0] a, output [W:0] o);\n"
        "endmodule\n"
        "module top (input [15:0] x, output [16:0] y, input [7:0] x8, output [8:0] y8);\n"
        "  bb u1 (.a(x8), .o(y8));\n  bb #(.W(16)) u2 (.a(x), .o(y));\nendmodule\n";
    for (int round = 0; round < 2; round++) {
        read_src(text);
        TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_pass_run(rs.design, "write_rtlil", odin3_bytes_cstr("bbw.il")));
        slurp("bbw.il", round == 0 ? first : second, TEXT_MAX);
        TEST_ASSERT_EQUAL_INT(0, remove("bbw.il"));
        et_free(&rs);
        memset(&rs, 0, sizeof rs);
    }
    TEST_ASSERT_EQUAL_STRING(first, second);
    static const char *const k_wanted[] = {
        "cell \\bb \\u2", "parameter \\W 16\n",
        /* Yosys writes `parameter signed \W 16` and a \src attribute on each; the writer does not
         * (a backend difference, not a width one: the widths are what these checks pin). */
        "attribute \\dynports 1\nmodule \\bb\n  parameter \\W 8\n", "wire width 8 input 1 \\a",
        "wire width 9 output 2 \\o", "connect \\o \\y\n", "connect \\o \\y8\n"};
    for (size_t k = 0; k < sizeof k_wanted / sizeof *k_wanted; k++) {
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(first, k_wanted[k]), k_wanted[k]);
    }
}

/* An override that does not evaluate is reported, not read as "not overridden". */
static void test_blackbox_override_unevaluable(void) {
    read_failing(BB_TOP("input [W-1:0] a, output o", "#(parameter W = 8)",
                        "  bb #(.W(4'bx010)) u1 (.a(x[7:0]), .o(y[0]));\n"));
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(1, diag_errors());
}

/* A source black box named like a registered type reuses it, whatever its parameters. */
static void test_blackbox_reuses_registered_type(void) {
    read_techlib_src(
        "(* blackbox *) module single_port_ram #(parameter ADDR_WIDTH = 1) (input clk, "
        "input [7:0] data, input [9:0] addr, input we, output [7:0] out);\nendmodule\n"
        "module top (input clk, input we, input [9:0] addr, input [7:0] data, output [7:0] q);\n"
        "  single_port_ram #(.ADDR_WIDTH(10)) ram (.clk(clk), .we(we), .addr(addr), "
        ".data(data), .out(q));\nendmodule\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    odin3_node_id ram = only_node("single_port_ram");
    TEST_ASSERT_EQUAL_INT(ODIN3_GRAN_HARD,
                          odin3_celltype_get(rs.design, odin3_node_type(top(), ram))->gran);
    TEST_ASSERT_EQUAL_INT64(10, node_param(ram, "ADDR_WIDTH"));
    TEST_ASSERT_EQUAL_INT64(8, node_param(ram, "DATA_WIDTH"));
}

/* A black box that does not fit the registered type of its name is a located error. */
static void test_blackbox_mismatch(void) {
    read_techlib_src(
        "(* blackbox *) module single_port_ram (input clk, input [7:0] data, output [7:0] "
        "dout);\nendmodule\n"
        "module top (input clk, input [7:0] d, output [7:0] q);\n"
        "  single_port_ram ram (.clk(clk), .data(d), .dout(q));\nendmodule\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_PARSE, rs.status, et_log(&rs));
    expect_error((at_loc){1, 16},
                 "black box 'single_port_ram' does not match the registered cell type: the cell "
                 "type has no port 'dout'");
    expect_ir_ok();
}

/* --- gates (Q1-Q3)
 * -------------------------------------------------------------------------------- */

#define GATE_HEAD                                                                                  \
    "module top (input a1, input b1, input c1, input i1, output y1, output y2, output o1, output " \
    "o2);\n"

/* and g (y1, a1, b1, c1): a left fold of two 1-bit $and cells, the second driving y1. */
static void test_gate_and(void) {
    read_clean(GATE_HEAD "  and g (y1, a1, b1, c1);\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32(2, count_of("$and"));
    TEST_ASSERT_EQUAL_UINT32(2, count_logic());
    odin3_node_id first = nth_node("$and", 0);
    odin3_node_id second = nth_node("$and", 1);
    TEST_ASSERT_EQUAL_INT64(1, param_of(first, "A_WIDTH"));
    TEST_ASSERT_EQUAL_INT64(1, param_of(first, "B_WIDTH"));
    TEST_ASSERT_EQUAL_INT64(1, param_of(first, "Y_WIDTH"));
    TEST_ASSERT_EQUAL_UINT32(wnet("a1", 0).v, pin_net(first, "A", 0).v);
    TEST_ASSERT_EQUAL_UINT32(wnet("b1", 0).v, pin_net(first, "B", 0).v);
    TEST_ASSERT_EQUAL_UINT32(pin_net(first, "Y", 0).v, pin_net(second, "A", 0).v);
    TEST_ASSERT_EQUAL_UINT32(wnet("c1", 0).v, pin_net(second, "B", 0).v);
    TEST_ASSERT_EQUAL_UINT32(wnet("y1", 0).v, pin_net(second, "Y", 0).v);
}

/* nand: the fold, then $not; xnor: $xor then $not. */
static void test_gate_inverting(void) {
    read_clean(GATE_HEAD "  nand (y1, a1, b1);\n  xnor (y2, a1, b1);\nendmodule\n");
    odin3_node_id and_cell = only_node("$and");
    odin3_node_id xor_cell = only_node("$xor");
    TEST_ASSERT_EQUAL_UINT32(2, count_of("$not"));
    odin3_node_id not_and = nth_node("$not", 0);
    odin3_node_id not_xor = nth_node("$not", 1);
    TEST_ASSERT_EQUAL_UINT32(pin_net(and_cell, "Y", 0).v, pin_net(not_and, "A", 0).v);
    TEST_ASSERT_EQUAL_UINT32(wnet("y1", 0).v, pin_net(not_and, "Y", 0).v);
    TEST_ASSERT_EQUAL_UINT32(pin_net(xor_cell, "Y", 0).v, pin_net(not_xor, "A", 0).v);
    TEST_ASSERT_EQUAL_UINT32(wnet("y2", 0).v, pin_net(not_xor, "Y", 0).v);
    TEST_ASSERT_EQUAL_INT64(1, param_of(not_and, "Y_WIDTH"));
}

/* buf: one assignment per output, here both merged with the input. */
static void test_gate_buf(void) {
    read_clean(GATE_HEAD "  buf (o1, o2, i1);\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32(0, count_logic());
    TEST_ASSERT_EQUAL_UINT32(wnet("i1", 0).v, wnet("o1", 0).v);
    TEST_ASSERT_EQUAL_UINT32(wnet("i1", 0).v, wnet("o2", 0).v);
}

/* not: one $not per output. */
static void test_gate_not(void) {
    read_clean(GATE_HEAD "  not (o1, o2, i1);\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32(2, count_of("$not"));
    for (uint32_t i = 0; i < 2; i++) {
        odin3_node_id cell = nth_node("$not", i);
        TEST_ASSERT_EQUAL_UINT32(wnet("i1", 0).v, pin_net(cell, "A", 0).v);
    }
    TEST_ASSERT_EQUAL_UINT32(wnet("o1", 0).v, pin_net(nth_node("$not", 0), "Y", 0).v);
    TEST_ASSERT_EQUAL_UINT32(wnet("o2", 0).v, pin_net(nth_node("$not", 1), "Y", 0).v);
}

/* Several instances of one gate, and a wider terminal. */
static void test_gate_instances_and_widths(void) {
    read_clean("module top (input [7:0] a, input [7:0] b, output [7:0] y, output z);\n"
               "  and g1 (y, a, b), g2 (z, a[0], b[0]);\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32(2, count_of("$and"));
    odin3_node_id wide = nth_node("$and", 0);
    TEST_ASSERT_EQUAL_INT64(8, param_of(wide, "Y_WIDTH"));
    expect_pins_are(wide, "Y", (bits_of){"y", 0}, 8);
    TEST_ASSERT_EQUAL_INT64(1, param_of(nth_node("$and", 1), "Y_WIDTH"));
}

/* Two gates on one output are A4's error. */
static void test_gate_output_conflict(void) {
    read_failing(GATE_HEAD "  and (y1, a1, b1);\n  or (y1, a1, c1);\nendmodule\n");
    expect_error((at_loc){3, 7}, "'y1' has two drivers (also driven at t.v:2:8)");
    TEST_ASSERT_EQUAL_UINT32(1, count_of("$and"));
    TEST_ASSERT_EQUAL_UINT32(0, count_of("$or"));
}

/* --- the error cap, the top and the summary ------------------------------------------------------
 */

/* 25 two-driver assignments: 20 errors, then the stop line; PARSE, no top, a valid IR. */
static void test_error_cap(void) {
    char text[4096];
    size_t len =
        (size_t)snprintf(text, sizeof text, "module top (input a, input b, output [24:0] y);\n");
    for (int i = 0; i < 25; i++) {
        int got = snprintf(text + len, sizeof text - len,
                           "  assign y[%d] = a;\n  assign y[%d] = b;\n", i, i);
        TEST_ASSERT_TRUE(got > 0);
        len += (size_t)got;
    }
    TEST_ASSERT_TRUE(snprintf(text + len, sizeof text - len, "endmodule\n") > 0);
    read_failing(text);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(21, diag_errors(), et_log(&rs));
    expect_error((at_loc){3, 10}, "'y[0]' has two drivers (also driven at t.v:2:10)");
    expect_error((at_loc){41, 10}, "'y[19]' has two drivers (also driven at t.v:40:10)");
    expect_error((at_loc){43, 10}, "too many errors; elaboration stopped after 20");
    TEST_ASSERT_EQUAL_UINT32(0, odin3_design_top(rs.design).v);
}

/* A clean three-module design with one specialization: the top is set, the summary line says so. */
static void test_three_module_summary(void) {
    read_clean("module leaf (input a, output y);\n  assign y = a;\nendmodule\n"
               "module mid #(parameter W = 1) (input [W-1:0] a, output [W-1:0] y);\n"
               "  leaf l (.a(a[0]), .y(y[0]));\n  assign y[W-1:1] = a[W-1:1];\nendmodule\n"
               "module top (input [7:0] a, output [7:0] y);\n  mid #(.W(8)) m (.a(a), .y(y));\n"
               "endmodule\n");
    TEST_ASSERT_EQUAL_UINT32(odin3_module_id_of(top()).v, odin3_design_top(rs.design).v);
    TEST_ASSERT_EQUAL_UINT32(1, et_count_with(&rs, ODIN3_LOG_INFO,
                                              "read_verilog: elaborated top 'top': 3 modules (1 "
                                              "specializations), 0 processes, "));
}

/* A '$' inside a module's own (escaped) name is not a specialization (Task 17's minor). */
static void test_escaped_name_is_not_a_specialization(void) {
    read_clean("module \\a$b (input x, output y);\n  assign y = x;\nendmodule\n"
               "module top (input x, output y);\n  \\a$b u (.x(x), .y(y));\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32(1, et_count_with(&rs, ODIN3_LOG_INFO,
                                              "read_verilog: elaborated top 'top': 2 modules (0 "
                                              "specializations), 0 processes, "));
}

/* --- the claim API itself
 * ------------------------------------------------------------------------- */

typedef struct claim_probe {
    bool first, second;
} claim_probe;

static void probe_claim(void *user, odin3_elab2 *e2, odin3_ast_id module) {
    claim_probe *probe = user;
    odin3_wire_id wire = odin3_module_find_wire(e2->mod, id_of("w"));
    odin3_net_id nets[2] = {odin3_wire_net(e2->mod, wire, 0), odin3_wire_net(e2->mod, wire, 1)};
    odin3_elab2_claim_req req = {nets, 2, ODIN3_DRV_CELL, module};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_elab2_claim(e2, &req, &probe->first));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_elab2_claim(e2, &req, &probe->second));
    odin3_elab2_claim_req bad = {nets, 2, (uint8_t)(ODIN3_DRV_TRIBUF + 1), module};
    bool granted = true;
    odin3_log_set_sink(NULL, NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_elab2_claim(e2, &bad, &granted));
}

/* The net-only claim records a driver and refuses the second, naming the bit. */
static void test_claim_api(void) {
    claim_probe probe = {false, true};
    odin3_elab_test_hooks hooks = {0, NULL, NULL, 0, NULL};
    hooks.user = &probe;
    hooks.after_prescan = probe_claim;
    odin3_elab_test_set_hooks(&hooks);
    read_src("module top (output o);\n  wire [1:0] w;\nendmodule\n");
    odin3_elab_test_set_hooks(NULL);
    TEST_ASSERT_TRUE(probe.first);
    TEST_ASSERT_FALSE(probe.second);
    expect_error((at_loc){1, 1}, "'w[0]' has two drivers (also driven at t.v:1:1)");
}

static void probe_wired_claim(void *user, odin3_elab2 *e2, odin3_ast_id module) {
    bool *granted = user;
    odin3_wire_id wire = odin3_module_find_wire(e2->mod, id_of("w"));
    odin3_net_id net = odin3_wire_net(e2->mod, wire, 0);
    odin3_elab2_claim_req req = {&net, 1, ODIN3_DRV_CELL, module};
    *granted = true;
    odin3_log_set_sink(NULL, NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_elab2_claim(e2, &req, granted));
}

/* The net-only claim refuses a wired net (it cannot redirect the driver). */
static void test_claim_api_refuses_wired(void) {
    bool granted = true;
    odin3_elab_test_hooks hooks = {0, NULL, NULL, 0, NULL};
    hooks.user = &granted;
    hooks.after_prescan = probe_wired_claim;
    odin3_elab_test_set_hooks(&hooks);
    read_src("module top (output o);\n  wand w;\nendmodule\n");
    odin3_elab_test_set_hooks(NULL);
    TEST_ASSERT_FALSE(granted);
}

/* --- a design with all of it, determinism, out of memory ----------------------------------------
 */

static const char k_rich[] =
    "module sub #(parameter W = 4) (input [W-1:0] a, output [W-1:0] o, inout p);\n"
    "  assign o = a;\nendmodule\n"
    "(* blackbox *) module bb #(parameter W = 8) (input [W-1:0] a, output [W-1:0] o);\nendmodule\n"
    "module top (input [7:0] a, input [7:0] b, input en, input c, inout io, output [7:0] y,\n"
    "            output [7:0] z, output g1, output g2, output t1, output [7:0] w2);\n"
    "  wire [7:0] s1, s2;\n  wand [7:0] w;\n  wire bus;\n  supply1 hi;\n  uwire u;\n"
    "  sub #(.W(8)) m (.a(a), .o(s1), .p(bus));\n"
    "  sub #(.W(4)) n (.a(b[3:0]), .o(), .p(bus));\n"
    "  bb #(.W(8)) k (.a(a), .o(s2));\n"
    "  and (g1, c, en, a[0]);\n  nand (g2, c, en);\n  bufif1 (t1, c, hi);\n"
    "  assign w = a;\n  assign w = b;\n  assign u = en;\n"
    "  assign y = en ? s1 : 8'bz;\n  assign z = s2 + w;\n  assign w2 = w;\n"
    "endmodule\n";

static void test_rich_design(void) {
    read_src(k_rich);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, diag_errors(), et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32(4, count_of("$and")); /* the wand's, the gate's two, the nand's */
    expect_ir_ok();
}

static void slurp(const char *path, char *out, size_t cap) {
    FILE *file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(file, path);
    size_t got = fread(out, 1, cap - 1, file);
    out[got] = '\0';
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static void rtlil_of_rich(char *out, size_t cap) {
    read_src(k_rich);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_pass_run(rs.design, "write_rtlil", odin3_bytes_cstr("rich.il")));
    slurp("rich.il", out, cap);
    TEST_ASSERT_EQUAL_INT(0, remove("rich.il"));
}

/* Two runs of the same input write the same RTLIL (byte-identical). */
static void test_deterministic(void) {
    static char first[TEXT_MAX];
    static char second[TEXT_MAX];
    rtlil_of_rich(first, sizeof first);
    et_free(&rs);
    memset(&rs, 0, sizeof rs);
    rtlil_of_rich(second, sizeof second);
    TEST_ASSERT_NOT_NULL(strstr(first, "cell $tribuf"));
    TEST_ASSERT_EQUAL_STRING(first, second);
}

/* A small design that still makes every kind of object (the sweep is quadratic in its size). */
static const char k_sweep[] =
    "module sub (input [3:0] a, output [3:0] o, inout p);\n  assign o = a;\nendmodule\n"
    "(* blackbox *) module bb (input a, output o);\nendmodule\n"
    "module top (input [3:0] a, input en, inout io, output [3:0] y, output g, output t,\n"
    "            output [3:0] z);\n"
    "  wand [3:0] w;\n  wire bus;\n"
    "  sub m (.a(a), .o(y), .p(bus));\n  bb k (.a(en), .o());\n"
    "  and (g, en, a[0]);\n  bufif1 (t, en, a[1]);\n"
    "  assign w = a;\n  assign w = {a[0], a[3:1]};\n  assign z = en ? w : 4'bz;\nendmodule\n";

static bool g_saw_missing_file;

static void quiet_sink(odin3_log_level level, const char *msg, void *user) {
    (void)level;
    (void)user;
    g_saw_missing_file |= strstr(msg, "missing_file:") != NULL;
}

static void fresh_quiet_design(void) {
    odin3_design_destroy(rs.design);
    rs.design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(rs.design);
    odin3_log_set_sink(quiet_sink, NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_log_set_level(ODIN3_LOG_ERROR));
}

static uint32_t g_sweep_modules = 3; /* module_end of a complete read of the swept source */

static bool read_complete(void) {
    return odin3_design_module_end(rs.design) == g_sweep_modules &&
           odin3_design_top(rs.design).v != 0;
}

/* One run of the whole read with the limit-th allocation failing. */
static odin3_status read_failing_at(long limit) {
    fresh_quiet_design();
    g_saw_missing_file = false;
    odin3_util_set_alloc_fail_after(limit);
    odin3_status st = odin3_pass_run(rs.design, "read_verilog", odin3_bytes_cstr("t.v"));
    odin3_util_set_alloc_fail_after(-1);
    /* an injected allocation failure is NO_MEMORY, never a user error and never a missing file */
    char why[64];
    TEST_ASSERT_TRUE(snprintf(why, sizeof why, "limit %ld status %d", limit, (int)st) > 0);
    TEST_ASSERT_TRUE_MESSAGE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY, why);
    TEST_ASSERT_FALSE_MESSAGE(g_saw_missing_file, why);
    odin3_check_opts full = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(rs.design, full));
    return st;
}

/* Every allocation of the read fails in turn: NO_MEMORY, the IR check passes, no leak (ASan), until
 * the read completes OOM_SETTLE runs in a row. */
static uint32_t oom_sweep_of(const char *text, uint32_t modules) {
    et_src src = {"t.v", text};
    et_req req = {&src, 1, NULL, NULL, NULL};
    et_setup(&req, &rs);
    g_sweep_modules = modules;
    uint32_t failures = 0;
    uint32_t settled = 0;
    for (long limit = 0; limit < OOM_LIMIT && settled < OOM_SETTLE; limit++) {
        odin3_status st = read_failing_at(limit);
        failures += st == ODIN3_ERR_NO_MEMORY ? 1U : 0U;
        settled = st == ODIN3_OK && read_complete() ? settled + 1 : 0;
    }
    g_sweep_modules = 3;
    TEST_ASSERT_EQUAL_UINT32(OOM_SETTLE, settled);
    return failures;
}

static void test_oom_sweep(void) {
    TEST_ASSERT_GREATER_THAN_UINT32(300, oom_sweep_of(k_sweep, 3));
}

/* A black-box-heavy source: parameter sets (carriers, a table, width rules), an array, a string
 * parameter and an implicit-width formula, all read under every allocation failure. */
static const char k_bbsweep[] =
    "(* blackbox *) module bb #(parameter W = 4, parameter S = \"x\") (input [W-1:0] a,\n"
    "                                                       output [2*W-1:0] o, input [3:0] k);\n"
    "endmodule\n"
    "module top (input [7:0] x, output [15:0] y, input [3:0] k, output [7:0] z,\n"
    "            output [3:0] w, output [31:0] y2, input [7:0] k2);\n"
    "  bb #(.W(8)) u1 (.a(x), .o(y), .k(k));\n"
    "  bb #(.W(2), .S(\"y\")) u2 (.a(x[1:0]), .o(w), .k(k));\n"
    "  bb u3 (.a(k), .o(z), .k(k));\n"
    "  bb #(.W(8)) u4 [1:0] (.a({x, x}), .o(y2), .k(k2));\nendmodule\n";

static void test_oom_sweep_blackbox_sets(void) {
    TEST_ASSERT_GREATER_THAN_UINT32(300, oom_sweep_of(k_bbsweep, 2));
}

/* Every test, in order (an X-macro keeps main short). */
#define ALL_TESTS(X)                                                                               \
    X(test_assign_loop_warning)                                                                    \
    X(test_never_driven_once)                                                                      \
    X(test_a4_whole)                                                                               \
    X(test_a4_bit)                                                                                 \
    X(test_a4_cell_then_assign)                                                                    \
    X(test_a4_input_port)                                                                          \
    X(test_variable_assigned)                                                                      \
    X(test_duplicate_concat_bits)                                                                  \
    X(test_uwire_second_driver)                                                                    \
    X(test_supply_second_driver)                                                                   \
    X(test_supply_nets)                                                                            \
    X(test_wand_three_drivers)                                                                     \
    X(test_wor_partial_drivers)                                                                    \
    X(test_wand_one_driver)                                                                        \
    X(test_wand_with_cell_driver)                                                                  \
    X(test_wired_always)                                                                           \
    X(test_always_assigned_bits)                                                                   \
    X(test_tribuf_assign)                                                                          \
    X(test_tribuf_inverted)                                                                        \
    X(test_tribuf_wide_condition)                                                                  \
    X(test_bufif_notif)                                                                            \
    X(test_two_tribufs)                                                                            \
    X(test_tribuf_on_inout)                                                                        \
    X(test_assign_and_tribuf)                                                                      \
    X(test_tribuf_internal)                                                                        \
    X(test_z_alone)                                                                                \
    X(test_child_inouts)                                                                           \
    X(test_child_inout_and_logic)                                                                  \
    X(test_input_extended)                                                                         \
    X(test_input_sign_extended)                                                                    \
    X(test_input_truncated)                                                                        \
    X(test_input_constant)                                                                         \
    X(test_output_wider)                                                                           \
    X(test_output_narrower)                                                                        \
    X(test_unconnected_ports)                                                                      \
    X(test_ordered_connections)                                                                    \
    X(test_instance_output_conflict)                                                               \
    X(test_module_parameters_as_attributes)                                                        \
    X(test_cell_type_inference)                                                                    \
    X(test_cell_type_given_parameter)                                                              \
    X(test_blackbox_base_name)                                                                     \
    X(test_blackbox_override_exact_cases)                                                          \
    X(test_bbw_range_param)                                                                        \
    X(test_bbw_formulas_and_fixed_port)                                                            \
    X(test_bbw_shared_default)                                                                     \
    X(test_bbw_no_cross_talk)                                                                      \
    X(test_bbw_string_override)                                                                    \
    X(test_bbw_instance_array)                                                                     \
    X(test_bbw_equal_values_other_width)                                                           \
    X(test_bbw_connection_resized)                                                                 \
    X(test_bbw_two_definitions)                                                                    \
    X(test_bbw_parameterised_parent)                                                               \
    X(test_bbw_rtlil)                                                                              \
    X(test_blackbox_override_unevaluable)                                                          \
    X(test_blackbox_reuses_registered_type)                                                        \
    X(test_blackbox_mismatch)                                                                      \
    X(test_gate_and)                                                                               \
    X(test_gate_inverting)                                                                         \
    X(test_gate_buf)                                                                               \
    X(test_gate_not)                                                                               \
    X(test_gate_instances_and_widths)                                                              \
    X(test_gate_output_conflict)                                                                   \
    X(test_error_cap)                                                                              \
    X(test_three_module_summary)                                                                   \
    X(test_escaped_name_is_not_a_specialization)                                                   \
    X(test_claim_api)                                                                              \
    X(test_claim_api_refuses_wired)                                                                \
    X(test_rich_design)                                                                            \
    X(test_deterministic)                                                                          \
    X(test_oom_sweep)                                                                              \
    X(test_oom_sweep_blackbox_sets)

#define RUN_ONE(name) RUN_TEST(name);

int main(void) {
    UNITY_BEGIN();
    ALL_TESTS(RUN_ONE)
    return UNITY_END();
}
