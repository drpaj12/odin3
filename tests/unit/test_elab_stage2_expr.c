/*
 * test_elab_stage2_expr.c — 2D Task 18: expressions and selects to word cells, continuous
 * assignments and net declaration assignments (EL-28 by construction and by merge, EL-30, EL-31,
 * EL-32 reads), with Yosys 0.55's genrtlil as the oracle for every cell and parameter. The whole
 * read_verilog pass runs through the pass manager (elab_test.h's et_read); the IR is read back
 * through the module API (cells by type, parameters by name, pins by port name) and every located
 * diagnostic is asserted as the whole line.
 *
 * Yosys commands behind the expected values (0.55, git sha1 60f126cd0):
 *   yosys -q -p "read_verilog t.v; write_rtlil"   (the cells of a module before any pass)
 * run on assigns of each row below (see the task report for the files).
 */
#include "elab_test.h"

/* elab_test_tree.h (hand-built stores) defines its own et_free: rename it for this file. */
#define et_free et_tree_free
#include "elab_test_tree.h"
#undef et_free

#include "ast/ast.h"
#include "ast/check.h"
#include "ast/kinds.h"
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

enum { OOM_LIMIT = 400000, OOM_SETTLE = 32, TEXT_MAX = 1 << 16, CHAIN_TERMS = 2000 };

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

/* --- the module under test ------------------------------------------------------------------ */

/* The signals of the task's table: 8-bit a, b, y; signed [7:0] s; signed [3:0] t; [3:0] u; 64-bit
 * y64; and the other inputs and outputs the rows need. The body starts at line HEAD_LINES + 1. */
static const char k_head[] =
    "module top (input [7:0] a, input [7:0] b, input signed [7:0] s, input signed [3:0] t,\n"
    "            input [3:0] u, input c, input [3:0] c4, input [3:0] a4, input [3:0] d4,\n"
    "            input [7:0] d, input [7:0] w, input [8:1] v, input [1:8] x,\n"
    "            input signed [31:0] i, input [7:0] k, output [7:0] y, output [7:0] y1,\n"
    "            output [7:0] y2, output [7:0] y3, output o1, output o2, output [3:0] n4,\n"
    "            output [15:0] y16, output [31:0] y32, output [63:0] y64, output [63:0] q64,\n"
    "            output [63:0] r64);\n";
enum { HEAD_LINES = 7 };

static char g_text[TEXT_MAX];

/* head + body + endmodule, read through the whole pass at keep level NONE. */
static void read_body(const char *body) {
    int got = snprintf(g_text, sizeof g_text,
                       "%s%s"
                       "endmodule\n",
                       k_head, body);
    TEST_ASSERT_TRUE(got > 0 && (size_t)got < sizeof g_text);
    et_src src = {"t.v", g_text};
    et_req req = {&src, 1, NULL, NULL, NULL};
    et_read_keep(&req, ODIN3_KEEP_AST_NONE, &rs);
}

static void expect_ir_ok(void) {
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_check_design(rs.design, opts), et_log(&rs));
}

/* Errors that are locatable diagnostics: not the pass manager's own "failed" line. */
static uint32_t diag_errors(void) {
    return rs.errors - et_count_with(&rs, ODIN3_LOG_ERROR, "pass read_verilog: failed");
}

/* The warnings of 2D: not the IR check's (the wide head leaves outputs undriven: rule 4). */
static uint32_t soft_warnings(void) {
    return rs.warnings - et_count_with(&rs, ODIN3_LOG_WARN, "check: ");
}

/* The body read clean: OK, no error, no warning, the IR passes check FULL. */
static void read_clean(const char *body) {
    read_body(body);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, diag_errors(), et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, soft_warnings(), et_log(&rs));
    expect_ir_ok();
}

/* The IR is valid whatever the outcome (EL-2: a failed stage 2 leaves valid, incomplete modules).
 */
static void read_failing(const char *body) {
    read_body(body);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_PARSE, rs.status, et_log(&rs));
    expect_ir_ok();
}

/* "t.v:<line>:<col>: <level>: <msg>" (body line numbers count from the head's end). */
typedef struct at_loc {
    uint32_t line, col;
} at_loc;

static const char *diag_line(at_loc at, const char *level, const char *msg) {
    static char buf[512];
    int got = snprintf(buf, sizeof buf, "t.v:%u:%u: %s: %s", (unsigned)(HEAD_LINES + at.line),
                       (unsigned)at.col, level, msg);
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

static odin3_module *top(void) {
    uint32_t id = id_of("top");
    for (uint32_t index = 1; index < odin3_design_module_end(rs.design); index++) {
        odin3_module *module = odin3_module_get(rs.design, (odin3_module_id){index});
        if (odin3_module_name(module) == id) {
            return module;
        }
    }
    TEST_FAIL_MESSAGE("no module top");
    return NULL;
}

static odin3_wire_id wire_of(const char *name) {
    odin3_wire_id wire = odin3_module_find_wire(top(), id_of(name));
    TEST_ASSERT_NOT_EQUAL_UINT32_MESSAGE(0, wire.v, name);
    return wire;
}

/* The net of bit `bit` of wire `name`. */
static odin3_net_id wnet(const char *name, uint32_t bit) {
    odin3_net_id net = odin3_wire_net(top(), wire_of(name), bit);
    TEST_ASSERT_NOT_EQUAL_UINT32_MESSAGE(0, net.v, name);
    return net;
}

static const odin3_celltype_def *def_of(odin3_node_id node) {
    return odin3_celltype_get(rs.design, odin3_node_type(top(), node));
}

static const char *type_name(odin3_node_id node) {
    return def_of(node)->name;
}

/* The nth live node (in creation order) of cell type `type`; none (0) when there are fewer. */
static odin3_node_id nth_node(const char *type, uint32_t nth) {
    const odin3_module *module = top();
    uint32_t seen = 0;
    for (uint32_t id = 1; id < odin3_module_node_end(module); id++) {
        odin3_node_id node = {id};
        if (odin3_node_live(module, node) && strcmp(type_name(node), type) == 0 && seen++ == nth) {
            return node;
        }
    }
    return (odin3_node_id){0};
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
        const odin3_celltype_def *def = def_of(node);
        bool constant = strncmp(def->name, "$_CONST", 7) == 0;
        count += def->gran == ODIN3_GRAN_PORT || constant ? 0U : 1U;
    }
    return count;
}

static odin3_node_id only_node(const char *type) {
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, count_of(type), type);
    return nth_node(type, 0);
}

static int64_t param_of(odin3_node_id node, const char *name) {
    const odin3_celltype_def *def = def_of(node);
    for (uint32_t index = 0; index < def->n_params; index++) {
        if (strcmp(def->params[index].name, name) == 0) {
            const odin3_value *value = odin3_node_param(top(), node, index);
            TEST_ASSERT_NOT_NULL(value);
            return value->i;
        }
    }
    TEST_FAIL_MESSAGE(name);
    return 0;
}

/* The net on pin `bit` of port `port` of the node. */
static odin3_net_id pin_net(odin3_node_id node, const char *port, uint32_t bit) {
    const odin3_celltype_def *def = def_of(node);
    for (uint32_t index = 0; index < def->n_ports; index++) {
        if (strcmp(def->ports[index].name, port) == 0) {
            odin3_pinslice slice = odin3_node_port(top(), node, index);
            TEST_ASSERT_TRUE_MESSAGE(bit < slice.count, port);
            return odin3_pin_net(top(), (odin3_pin_id){slice.first.v + bit});
        }
    }
    TEST_FAIL_MESSAGE(port);
    return (odin3_net_id){0};
}

/* The node whose output pin drives bit `bit` of wire `name` (none when the net has no driver). */
static odin3_node_id driver_of(const char *name, uint32_t bit) {
    odin3_pin_id pin = odin3_net_driver(top(), wnet(name, bit));
    return pin.v == 0 ? (odin3_node_id){0} : odin3_pin_node(top(), pin);
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
    char why[64];
    TEST_ASSERT_TRUE(snprintf(why, sizeof why, "%s[%u]", name, (unsigned)bit) > 0);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(net.v, wnet(name, bit).v, why);
}

/* A run of bits [first, end). */
typedef struct run_span {
    uint32_t first, end;
} run_span;

/* Bits of a wire in the run all on constant `want`. */
static void expect_const_run(const char *name, run_span run, odin3_bit want) {
    for (uint32_t bit = run.first; bit < run.end; bit++) {
        expect_const_bit(name, bit, want);
    }
}

/* Bits of wire `name` from `first` onwards are the nets of wire `other` from `other_first`. */
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

/* Pins [from, to) of port `port` on constant `want`. */
static void expect_pins_const(odin3_node_id node, const char *port, run_span run, odin3_bit want) {
    for (uint32_t k = run.first; k < run.end; k++) {
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(const_net_of(want).v, pin_net(node, port, k).v, port);
    }
}

typedef struct bin_want {
    const char *type;
    int64_t a_signed, b_signed, a_width, b_width, y_width;
} bin_want;

/* Every parameter of a binary word cell, and its type. */
static void expect_bin(odin3_node_id node, bin_want want) {
    TEST_ASSERT_NOT_EQUAL_UINT32_MESSAGE(0, node.v, want.type);
    TEST_ASSERT_EQUAL_STRING(want.type, type_name(node));
    TEST_ASSERT_EQUAL_INT64_MESSAGE(want.a_signed, param_of(node, "A_SIGNED"), want.type);
    TEST_ASSERT_EQUAL_INT64_MESSAGE(want.b_signed, param_of(node, "B_SIGNED"), want.type);
    TEST_ASSERT_EQUAL_INT64_MESSAGE(want.a_width, param_of(node, "A_WIDTH"), want.type);
    TEST_ASSERT_EQUAL_INT64_MESSAGE(want.b_width, param_of(node, "B_WIDTH"), want.type);
    TEST_ASSERT_EQUAL_INT64_MESSAGE(want.y_width, param_of(node, "Y_WIDTH"), want.type);
}

typedef struct un_want {
    const char *type;
    int64_t a_signed, a_width, y_width;
} un_want;

static void expect_un(odin3_node_id node, un_want want) {
    TEST_ASSERT_NOT_EQUAL_UINT32_MESSAGE(0, node.v, want.type);
    TEST_ASSERT_EQUAL_STRING(want.type, type_name(node));
    TEST_ASSERT_EQUAL_INT64_MESSAGE(want.a_signed, param_of(node, "A_SIGNED"), want.type);
    TEST_ASSERT_EQUAL_INT64_MESSAGE(want.a_width, param_of(node, "A_WIDTH"), want.type);
    TEST_ASSERT_EQUAL_INT64_MESSAGE(want.y_width, param_of(node, "Y_WIDTH"), want.type);
}

/* --- assignments (EL-28; review R1) ---------------------------------------------------------- */

/* y = a + b: one $add, its Y pins on y's nets, nothing merged. */
static void test_assign_by_construction(void) {
    read_clean("  assign y = a + b;\n");
    TEST_ASSERT_EQUAL_UINT32(1, count_logic());
    odin3_node_id add = only_node("$add");
    expect_bin(add, (bin_want){"$add", 0, 0, 8, 8, 8});
    expect_pins_are(add, "A", (bits_of){"a", 0}, 8);
    expect_pins_are(add, "B", (bits_of){"b", 0}, 8);
    expect_pins_are(add, "Y", (bits_of){"y", 0}, 8);
    for (uint32_t bit = 0; bit < 8; bit++) {
        TEST_ASSERT_EQUAL_UINT32(0, odin3_net_alias_count(top(), wnet("y", bit)));
        TEST_ASSERT_EQUAL_UINT32(0, odin3_net_alias_count(top(), wnet("a", bit)));
    }
}

/* assign b2 = a: one net, alias count 1; assign c = 1'b1: c on the $_CONST1_ net. */
static void test_assign_alias_and_const(void) {
    read_clean("  wire b2;\n  wire c1;\n  assign b2 = c;\n  assign c1 = 1'b1;\n"
               "  assign o1 = c1;\n");
    TEST_ASSERT_EQUAL_UINT32(0, count_logic());
    TEST_ASSERT_EQUAL_UINT32(wnet("c", 0).v, wnet("b2", 0).v);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_net_alias_count(top(), wnet("c", 0)));
    expect_const_bit("c1", 0, ODIN3_BIT_1);
    expect_const_bit("o1", 0, ODIN3_BIT_1);
    TEST_ASSERT_EQUAL_UINT32(1, count_of("$_CONST1_"));
}

/* wire w = a & b (N7): one $and driving w, through the same path as an assign. */
static void test_net_declaration_assignment(void) {
    read_clean("  wire [7:0] w2 = a & b;\n  assign y = w2;\n");
    odin3_node_id and_cell = only_node("$and");
    expect_bin(and_cell, (bin_want){"$and", 0, 0, 8, 8, 8});
    expect_pins_are(and_cell, "Y", (bits_of){"w2", 0}, 8);
    TEST_ASSERT_EQUAL_UINT32(1, count_logic());
    expect_same_nets((bits_of){"y", 0}, (bits_of){"w2", 0}, 8);
}

/* A port's declaration assignment (v4.15) drives the port's net. */
static void test_port_declaration_assignment(void) {
    et_src src = {"t.v", "module top (input [3:0] a, input [3:0] b, output wire [3:0] y = a ^ b);\n"
                         "endmodule\n"};
    et_req req = {&src, 1, NULL, NULL, NULL};
    et_read_keep(&req, ODIN3_KEEP_AST_NONE, &rs);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    odin3_node_id xor_cell = only_node("$xor");
    expect_bin(xor_cell, (bin_want){"$xor", 0, 0, 4, 4, 4});
    expect_pins_are(xor_cell, "Y", (bits_of){"y", 0}, 4);
    expect_ir_ok();
}

/* A select target that is not constant is EL-28's error; nothing is attached for it. */
static void test_assign_non_constant_target(void) {
    read_failing("  assign y[s] = d;\n  assign y1 = a;\n");
    expect_error((at_loc){1, 10},
                 "continuous assignment to 'y' with a non-constant index is not supported");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, diag_errors(), et_log(&rs));
    expect_same_nets((bits_of){"y1", 0}, (bits_of){"a", 0}, 8); /* the next item is emitted */
}

/* Out-of-range target bits are dropped with Yosys's warning; the bits inside are driven. */
static void test_assign_out_of_range_bits(void) {
    read_body("  assign y[9:6] = d4;\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    expect_warning((at_loc){1, 10}, "assignment to bits outside 'y' [7:0] is ignored");
    expect_same_nets((bits_of){"y", 6}, (bits_of){"d4", 0}, 2);
    TEST_ASSERT_EQUAL_UINT32(1, soft_warnings());
    expect_ir_ok();
}

/* Constant selects and concatenations as targets; the lsb of a concatenation is its last part. */
static void test_assign_select_and_concat_targets(void) {
    read_body("  wire [15:0] y16_src;\n  assign y[3:0] = a4;\n  assign y[4] = c;\n  assign "
              "{y[7:6], y[5]} = {c4[1:0], c};\n"
              "  assign {y2, y1} = y16_src;\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    expect_warning((at_loc){1, 15}, "'y16_src' is never driven"); /* Task 19: it has sinks */
    TEST_ASSERT_EQUAL_UINT32(1, soft_warnings());
    expect_ir_ok();
    TEST_ASSERT_EQUAL_UINT32(0, count_logic());
    expect_same_nets((bits_of){"y", 0}, (bits_of){"a4", 0}, 4);
    TEST_ASSERT_EQUAL_UINT32(wnet("c", 0).v, wnet("y", 4).v);
    TEST_ASSERT_EQUAL_UINT32(wnet("c", 0).v, wnet("y", 5).v);
    TEST_ASSERT_EQUAL_UINT32(wnet("c4", 0).v, wnet("y", 6).v);
    TEST_ASSERT_EQUAL_UINT32(wnet("c4", 1).v, wnet("y", 7).v);
    expect_same_nets((bits_of){"y1", 0}, (bits_of){"y16_src", 0}, 8);
    expect_same_nets((bits_of){"y2", 0}, (bits_of){"y16_src", 8}, 8);
}

/* assign a = b; assign b = a: the same net, so the second is a no-op (no merge, no error); nothing
 * drives it, which Task 19 warns about once. */
static void test_same_net_assignment_noop(void) {
    read_body("  wire p, q;\n  assign p = q;\n  assign q = p;\n  assign o1 = p;\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    expect_warning((at_loc){3, 10},
                   "'p' and 'q' are connected only to each other and are never driven");
    TEST_ASSERT_EQUAL_UINT32(1, soft_warnings());
    expect_ir_ok();
    TEST_ASSERT_EQUAL_UINT32(wnet("p", 0).v, wnet("q", 0).v);
    TEST_ASSERT_EQUAL_UINT32(wnet("o1", 0).v, wnet("q", 0).v);
}

/* --- E22, unsized constants, E23 ------------------------------------------------------------- */

/* (a + b) >> 1: the add keeps its 8 bits, the shift's amount is the 32-bit constant 1. */
static void test_context_width_e22(void) {
    read_clean("  assign y = (a + b) >> 1;\n");
    TEST_ASSERT_EQUAL_UINT32(2, count_logic());
    odin3_node_id add = only_node("$add");
    odin3_node_id shr = only_node("$shr");
    expect_bin(add, (bin_want){"$add", 0, 0, 8, 8, 8});
    expect_bin(shr, (bin_want){"$shr", 0, 0, 8, 32, 8});
    expect_pins_are(shr, "Y", (bits_of){"y", 0}, 8);
    for (uint32_t bit = 0; bit < 8; bit++) {
        TEST_ASSERT_EQUAL_UINT32(pin_net(add, "Y", bit).v, pin_net(shr, "A", bit).v);
    }
    expect_pins_const(shr, "B", (run_span){0, 1}, ODIN3_BIT_1);
    expect_pins_const(shr, "B", (run_span){1, 32}, ODIN3_BIT_0);
}

/* (0 + a + b) >> 1: the 32-bit constant widens the whole expression; y takes the low 8 bits. */
static void test_context_width_wide(void) {
    read_clean("  assign y = (0 + a + b) >> 1;\n");
    TEST_ASSERT_EQUAL_UINT32(3, count_logic());
    expect_bin(nth_node("$add", 0), (bin_want){"$add", 0, 0, 32, 8, 32});
    expect_bin(nth_node("$add", 1), (bin_want){"$add", 0, 0, 32, 8, 32});
    odin3_node_id shr = only_node("$shr");
    expect_bin(shr, (bin_want){"$shr", 0, 0, 32, 32, 32});
    for (uint32_t bit = 0; bit < 8; bit++) {
        TEST_ASSERT_EQUAL_UINT32(pin_net(shr, "Y", bit).v, wnet("y", bit).v);
    }
    expect_pins_const(nth_node("$add", 0), "A", (run_span){0, 32}, ODIN3_BIT_0);
}

/* An unsized parameter keeps UNSIZED (review I11): P and Q fill 64 bits with ones, R does not. */
static void test_unsized_constants(void) {
    read_clean("  localparam P = 'hFFFFFFFF;\n  localparam [31:0] Q = 'hFFFFFFFF;\n"
               "  localparam [35:0] R = 'hFFFFFFFF;\n  assign y64 = P;\n  assign q64 = Q;\n"
               "  assign r64 = R;\n");
    TEST_ASSERT_EQUAL_UINT32(0, count_logic());
    expect_const_run("y64", (run_span){0, 64}, ODIN3_BIT_1);
    expect_const_run("q64", (run_span){0, 64}, ODIN3_BIT_1);
    expect_const_run("r64", (run_span){0, 32}, ODIN3_BIT_1);
    expect_const_run("r64", (run_span){32, 64}, ODIN3_BIT_0);
}

/* E23: s + u is unsigned (one operand is), s + t is signed; operands keep their own widths. */
static void test_signedness_e23(void) {
    read_clean("  assign y = s + u;\n  assign y1 = s + t;\n");
    odin3_node_id first = nth_node("$add", 0);
    odin3_node_id second = nth_node("$add", 1);
    expect_bin(first, (bin_want){"$add", 0, 0, 8, 4, 8});
    expect_bin(second, (bin_want){"$add", 1, 1, 8, 4, 8});
    expect_pins_are(first, "A", (bits_of){"s", 0}, 8);
    expect_pins_are(first, "B", (bits_of){"u", 0}, 4);
}

/* --- every EL-30 row ------------------------------------------------------------------------- */

static void test_arithmetic_rows(void) {
    read_clean("  assign y = a - b;\n  assign y1 = a * b;\n  assign y2 = a / b;\n"
               "  assign y3 = a % b;\n");
    expect_bin(driver_of("y", 0), (bin_want){"$sub", 0, 0, 8, 8, 8});
    expect_bin(driver_of("y1", 0), (bin_want){"$mul", 0, 0, 8, 8, 8});
    expect_bin(driver_of("y2", 0), (bin_want){"$div", 0, 0, 8, 8, 8});
    expect_bin(driver_of("y3", 0), (bin_want){"$mod", 0, 0, 8, 8, 8});
}

/* -a and ~a extend their operand to the context by wiring first; +a is no cell. */
static void test_unary_rows(void) {
    read_clean("  assign y32 = -a;\n  assign y = ~a;\n  assign y1 = +a;\n");
    odin3_node_id neg = only_node("$neg");
    expect_un(neg, (un_want){"$neg", 0, 32, 32});
    expect_pins_are(neg, "A", (bits_of){"a", 0}, 8);
    expect_pins_const(neg, "A", (run_span){8, 32}, ODIN3_BIT_0);
    expect_pins_are(neg, "Y", (bits_of){"y32", 0}, 32);
    odin3_node_id not_cell = only_node("$not");
    expect_un(not_cell, (un_want){"$not", 0, 8, 8});
    expect_pins_are(not_cell, "Y", (bits_of){"y", 0}, 8);
    TEST_ASSERT_EQUAL_UINT32(2, count_logic()); /* +a made none */
    expect_same_nets((bits_of){"y1", 0}, (bits_of){"a", 0}, 8);
}

/* A signed operand of a signed context is sign-extended by wiring, with A_SIGNED 1. */
static void test_unary_signed_extension(void) {
    read_clean("  assign y32 = -s;\n");
    odin3_node_id neg = only_node("$neg");
    expect_un(neg, (un_want){"$neg", 1, 32, 32});
    expect_pins_are(neg, "A", (bits_of){"s", 0}, 8);
    for (uint32_t bit = 8; bit < 32; bit++) {
        TEST_ASSERT_EQUAL_UINT32(wnet("s", 7).v, pin_net(neg, "A", bit).v);
    }
}

static void test_reduction_rows(void) {
    read_clean("  assign y = &a;\n  assign y16 = {&b, d};\n  assign y1 = |a;\n  assign y2 = ^a;\n"
               "  assign y3 = ~^a;\n");
    expect_un(driver_of("y", 0), (un_want){"$reduce_and", 0, 8, 8});
    expect_un(driver_of("y16", 8), (un_want){"$reduce_and", 0, 8, 1});
    expect_un(driver_of("y1", 0), (un_want){"$reduce_or", 0, 8, 8});
    expect_un(driver_of("y2", 0), (un_want){"$reduce_xor", 0, 8, 8});
    expect_un(driver_of("y3", 0), (un_want){"$reduce_xnor", 0, 8, 8});
}

/* A signed operand of a reduction is unsigned to its cell (genrtlil generates it with no hint);
 * a compound one keeps the sign of its operator (Yosys 0.55: `&s` A_SIGNED 0, `&(s + t)` 1). */
static void test_reduction_signed_operands(void) {
    read_clean("  assign y = &s;\n  assign y1 = |s;\n  assign y2 = ^s;\n  assign y3 = ~^s;\n"
               "  assign y32 = &(s + t);\n  assign o1 = ~|s;\n  assign o2 = ~&s;\n");
    expect_un(driver_of("y", 0), (un_want){"$reduce_and", 0, 8, 8});
    expect_un(driver_of("y1", 0), (un_want){"$reduce_or", 0, 8, 8});
    expect_un(driver_of("y2", 0), (un_want){"$reduce_xor", 0, 8, 8});
    expect_un(driver_of("y3", 0), (un_want){"$reduce_xnor", 0, 8, 8});
    expect_un(driver_of("y32", 0), (un_want){"$reduce_and", 1, 8, 32});
    expect_un(nth_node("$reduce_or", 1), (un_want){"$reduce_or", 0, 8, 1});
    expect_un(nth_node("$reduce_and", 2), (un_want){"$reduce_and", 0, 8, 1});
}

/* ~&a and ~|a are the reduction (1 bit) and a $logic_not of it (Yosys's parser form, v4). */
static void test_nand_nor_rows(void) {
    read_clean("  assign y = ~&a;\n  assign y1 = ~|a;\n");
    odin3_node_id not_and = driver_of("y", 0);
    expect_un(not_and, (un_want){"$logic_not", 0, 1, 8});
    odin3_node_id red_and = only_node("$reduce_and");
    expect_un(red_and, (un_want){"$reduce_and", 0, 8, 1});
    TEST_ASSERT_EQUAL_UINT32(pin_net(red_and, "Y", 0).v, pin_net(not_and, "A", 0).v);
    odin3_node_id not_or = driver_of("y1", 0);
    expect_un(not_or, (un_want){"$logic_not", 0, 1, 8});
    odin3_node_id red_or = only_node("$reduce_or");
    expect_un(red_or, (un_want){"$reduce_or", 0, 8, 1});
    TEST_ASSERT_EQUAL_UINT32(pin_net(red_or, "Y", 0).v, pin_net(not_or, "A", 0).v);
    TEST_ASSERT_EQUAL_UINT32(4, count_logic());
}

static void test_logic_rows(void) {
    read_clean("  assign y = !a;\n  assign y1 = a && b;\n  assign y2 = a || b;\n");
    expect_un(driver_of("y", 0), (un_want){"$logic_not", 0, 8, 8});
    expect_bin(driver_of("y1", 0), (bin_want){"$logic_and", 0, 0, 8, 8, 8});
    expect_bin(driver_of("y2", 0), (bin_want){"$logic_or", 0, 0, 8, 8, 8});
}

static void test_logic_signed_operands(void) {
    read_clean("  assign y = !s;\n  assign y1 = s && t;\n  assign y2 = s || a;\n"
               "  assign y3 = a && (s + t);\n  assign y32 = !(-s);\n");
    expect_un(driver_of("y", 0), (un_want){"$logic_not", 0, 8, 8});
    expect_bin(driver_of("y1", 0), (bin_want){"$logic_and", 0, 0, 8, 4, 8});
    expect_bin(driver_of("y2", 0), (bin_want){"$logic_or", 0, 0, 8, 8, 8});
    expect_bin(driver_of("y3", 0), (bin_want){"$logic_and", 0, 1, 8, 8, 8});
    expect_un(driver_of("y32", 0), (un_want){"$logic_not", 0, 8, 32});
}

static void test_bitwise_rows(void) {
    read_clean("  assign y = a ~^ b;\n  assign y1 = a | b;\n  assign y2 = a ^ b;\n");
    expect_bin(driver_of("y", 0), (bin_want){"$xnor", 0, 0, 8, 8, 8});
    expect_bin(driver_of("y1", 0), (bin_want){"$or", 0, 0, 8, 8, 8});
    expect_bin(driver_of("y2", 0), (bin_want){"$xor", 0, 0, 8, 8, 8});
}

/* The shift amount is self-determined and unsigned: B_SIGNED 0 throughout. */
static void test_shift_rows(void) {
    read_clean("  assign y = a << b;\n  assign y1 = a <<< b;\n  assign y2 = a >> b;\n"
               "  assign y3 = s >>> b;\n  assign y32 = a << i;\n");
    expect_bin(driver_of("y", 0), (bin_want){"$shl", 0, 0, 8, 8, 8});
    expect_bin(driver_of("y1", 0), (bin_want){"$shl", 0, 0, 8, 8, 8});
    expect_bin(driver_of("y2", 0), (bin_want){"$shr", 0, 0, 8, 8, 8});
    expect_bin(driver_of("y3", 0), (bin_want){"$sshr", 1, 0, 8, 8, 8});
    expect_bin(driver_of("y32", 0), (bin_want){"$shl", 0, 0, 8, 32, 32});
}

/* a ** b is $pow (B_SIGNED from b); 2 ** b is Yosys's rewrite to $shl with A = 1. */
static void test_pow_rows(void) {
    read_clean("  assign y = a ** b;\n  assign y1 = 2 ** b;\n  assign y2 = a ** i;\n");
    expect_bin(driver_of("y", 0), (bin_want){"$pow", 0, 0, 8, 8, 8});
    odin3_node_id shl = driver_of("y1", 0);
    expect_bin(shl,
               (bin_want){"$shl", 1, 0, 32, 8, 32}); /* Yosys: A_SIGNED 1 (the signed decimal 2) */
    expect_pins_const(shl, "A", (run_span){0, 1}, ODIN3_BIT_1);
    expect_pins_const(shl, "A", (run_span){1, 32}, ODIN3_BIT_0);
    expect_bin(driver_of("y2", 0), (bin_want){"$pow", 0, 1, 8, 32, 8});
}

/* Comparisons: operands as generated (an identifier at its own width), Y_WIDTH the context's. */
static void test_compare_rows(void) {
    read_clean("  assign y = a < t;\n  assign y1 = s < t;\n  assign y2 = a == b;\n"
               "  assign y3 = a >= b;\n  assign o1 = a != b;\n");
    expect_bin(driver_of("y", 0), (bin_want){"$lt", 0, 0, 8, 4, 8});
    expect_bin(driver_of("y1", 0), (bin_want){"$lt", 1, 1, 8, 4, 8});
    expect_bin(driver_of("y2", 0), (bin_want){"$eq", 0, 0, 8, 8, 8});
    expect_bin(driver_of("y3", 0), (bin_want){"$ge", 0, 0, 8, 8, 8});
    expect_bin(driver_of("o1", 0), (bin_want){"$ne", 0, 0, 8, 8, 1});
}

/* c ? a : b is $mux(A = b, B = a, S = c); a wider select goes through $reduce_bool first. */
static void test_ternary_rows(void) {
    read_clean("  assign y = c ? a : b;\n  assign y1 = c4 ? a : b;\n  assign y2 = 1 ? a : b;\n"
               "  assign y3 = 1'b0 ? a : b;\n");
    odin3_node_id mux = driver_of("y", 0);
    TEST_ASSERT_EQUAL_STRING("$mux", type_name(mux));
    TEST_ASSERT_EQUAL_INT64(8, param_of(mux, "WIDTH"));
    expect_pins_are(mux, "A", (bits_of){"b", 0}, 8);
    expect_pins_are(mux, "B", (bits_of){"a", 0}, 8);
    expect_pins_are(mux, "S", (bits_of){"c", 0}, 1);
    odin3_node_id wide = driver_of("y1", 0);
    TEST_ASSERT_EQUAL_STRING("$mux", type_name(wide));
    odin3_node_id reduce = only_node("$reduce_bool");
    expect_un(reduce, (un_want){"$reduce_bool", 0, 4, 1});
    expect_pins_are(reduce, "A", (bits_of){"c4", 0}, 4);
    TEST_ASSERT_EQUAL_UINT32(pin_net(reduce, "Y", 0).v, pin_net(wide, "S", 0).v);
    TEST_ASSERT_EQUAL_UINT32(2, count_of("$mux"));
    expect_same_nets((bits_of){"y2", 0}, (bits_of){"a", 0}, 8); /* a constant condition: a only */
    expect_same_nets((bits_of){"y3", 0}, (bits_of){"b", 0}, 8);
}

/* The $reduce_bool of a wider condition reads it as genrtlil does: a signed name is unsigned. */
static void test_ternary_signed_condition(void) {
    read_clean("  assign y = s ? a : b;\n  assign y1 = (s + t) ? a : b;\n");
    odin3_node_id plain = nth_node("$reduce_bool", 0);
    odin3_node_id compound = nth_node("$reduce_bool", 1);
    expect_un(plain, (un_want){"$reduce_bool", 0, 8, 1});
    expect_un(compound, (un_want){"$reduce_bool", 1, 8, 1});
}

/* Yosys makes the mux as wide as its branches as generated and extends the result (agent default
 * for EL-30's WIDTH: the spec's "final width" and genrtlil differ only here). */
static void test_ternary_wide_context(void) {
    read_clean("  assign y32 = c ? a : b;\n");
    odin3_node_id mux = only_node("$mux");
    TEST_ASSERT_EQUAL_INT64(8, param_of(mux, "WIDTH"));
    for (uint32_t bit = 0; bit < 8; bit++) {
        TEST_ASSERT_EQUAL_UINT32(pin_net(mux, "Y", bit).v, wnet("y32", bit).v);
    }
    expect_const_run("y32", (run_span){8, 32}, ODIN3_BIT_0);
}

/* In a context wider than 32 bits a plain decimal keeps its 32 bits (Yosys 0.55); the apostrophe
 * forms are widened to the context (spec 17 #41). */
static void test_plain_decimal_in_wide_context(void) {
    read_clean("  assign y64 = a + 1;\n  assign q64 = a + 'd1;\n  assign r64 = c ? a : 5;\n"
               "  assign y32 = c ? a : 'd5;\n");
    odin3_node_id plain = driver_of("y64", 0);
    expect_bin(plain, (bin_want){"$add", 0, 0, 8, 32, 64});
    expect_pins_const(plain, "B", (run_span){0, 1}, ODIN3_BIT_1);
    expect_bin(driver_of("q64", 0), (bin_want){"$add", 0, 0, 8, 64, 64});
    odin3_node_id mux = nth_node("$mux", 0);
    TEST_ASSERT_EQUAL_INT64(32, param_of(mux, "WIDTH"));
    expect_const_run("r64", (run_span){32, 64}, ODIN3_BIT_0);
    TEST_ASSERT_EQUAL_INT64(32, param_of(nth_node("$mux", 1), "WIDTH"));
}

/* Concatenation, replication, $signed and $unsigned are wiring only. */
static void test_wiring_rows(void) {
    read_clean("  assign y16 = {a, b};\n  assign y64 = {2{a}};\n  assign y32 = $signed(a);\n"
               "  assign q64 = $unsigned(s);\n  assign r64 = $signed(s);\n");
    TEST_ASSERT_EQUAL_UINT32(0, count_logic());
    expect_same_nets((bits_of){"y16", 0}, (bits_of){"b", 0}, 8);
    expect_same_nets((bits_of){"y16", 8}, (bits_of){"a", 0}, 8);
    expect_same_nets((bits_of){"y64", 0}, (bits_of){"a", 0}, 8);
    expect_same_nets((bits_of){"y64", 8}, (bits_of){"a", 0}, 8);
    expect_const_run("y64", (run_span){16, 64}, ODIN3_BIT_0);
    /* $signed(a) is signed whatever a is: sign-extended into the 32-bit context. */
    expect_same_nets((bits_of){"y32", 0}, (bits_of){"a", 0}, 8);
    for (uint32_t bit = 8; bit < 32; bit++) {
        TEST_ASSERT_EQUAL_UINT32(wnet("a", 7).v, wnet("y32", bit).v);
    }
    expect_same_nets((bits_of){"q64", 0}, (bits_of){"s", 0}, 8);
    expect_const_run("q64", (run_span){8, 64}, ODIN3_BIT_0);
    for (uint32_t bit = 8; bit < 64; bit++) {
        TEST_ASSERT_EQUAL_UINT32(wnet("s", 7).v, wnet("r64", bit).v);
    }
}

/* --- === and !== (E9) ------------------------------------------------------------------------ */

static void test_case_equality_constants(void) {
    read_body("  assign o1 = 4'b1x00 === 4'b1x00;\n  assign o2 = a4 === 4'bxxxx;\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32(0, count_logic());
    expect_const_bit("o1", 0, ODIN3_BIT_1);
    expect_const_bit("o2", 0, ODIN3_BIT_1); /* no 0/1 position left: always matches */
    expect_warning((at_loc){2, 15}, "'===' compares against x/z bits; they match any value");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, soft_warnings(), et_log(&rs));
    expect_ir_ok();
}

/* a4 !== 4'b1x00: a $ne over the three positions the constant decides (a4[3], a4[1], a4[0]). */
static void test_case_inequality_wildcard(void) {
    read_body("  assign o1 = a4 !== 4'b1x00;\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    expect_warning((at_loc){1, 15}, "'===' compares against x/z bits; they match any value");
    odin3_node_id ne = only_node("$ne");
    TEST_ASSERT_EQUAL_UINT32(1, count_logic());
    TEST_ASSERT_EQUAL_INT64(3, param_of(ne, "A_WIDTH"));
    TEST_ASSERT_EQUAL_INT64(3, param_of(ne, "B_WIDTH"));
    TEST_ASSERT_EQUAL_INT64(1, param_of(ne, "Y_WIDTH"));
    TEST_ASSERT_EQUAL_UINT32(wnet("a4", 0).v, pin_net(ne, "A", 0).v);
    TEST_ASSERT_EQUAL_UINT32(wnet("a4", 1).v, pin_net(ne, "A", 1).v);
    TEST_ASSERT_EQUAL_UINT32(wnet("a4", 3).v, pin_net(ne, "A", 2).v);
    expect_pins_const(ne, "B", (run_span){0, 2}, ODIN3_BIT_0);
    expect_pins_const(ne, "B", (run_span){2, 3}, ODIN3_BIT_1);
    expect_pins_are(ne, "Y", (bits_of){"o1", 0}, 1);
    expect_ir_ok();
}

static void test_case_equality_plain(void) {
    read_body("  assign o1 = a === b;\n  assign o2 = a !== b;\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    expect_warning((at_loc){1, 15}, "'===' is treated as '==' (synthesis has no x or z)");
    expect_warning((at_loc){2, 15}, "'===' is treated as '==' (synthesis has no x or z)");
    expect_bin(driver_of("o1", 0), (bin_want){"$eq", 0, 0, 8, 8, 1});
    expect_bin(driver_of("o2", 0), (bin_want){"$ne", 0, 0, 8, 8, 1});
    expect_ir_ok();
}

/* --- selects (EL-32) ------------------------------------------------------------------------- */

static void test_constant_selects(void) {
    read_clean("  assign o1 = w[2];\n  assign n4 = w[5:2];\n  assign o2 = v[8];\n"
               "  assign y1 = x[1:8];\n  assign y2 = {w[0], w[7:1]};\n");
    TEST_ASSERT_EQUAL_UINT32(0, count_logic());
    TEST_ASSERT_EQUAL_UINT32(wnet("w", 2).v, wnet("o1", 0).v);
    expect_same_nets((bits_of){"n4", 0}, (bits_of){"w", 2}, 4);
    TEST_ASSERT_EQUAL_UINT32(wnet("v", 7).v, wnet("o2", 0).v);  /* v[8] is the MSB of [8:1] */
    expect_same_nets((bits_of){"y1", 0}, (bits_of){"x", 0}, 8); /* x[1:8] is all of x */
    TEST_ASSERT_EQUAL_UINT32(wnet("w", 1).v, wnet("y2", 0).v);
    TEST_ASSERT_EQUAL_UINT32(wnet("w", 0).v, wnet("y2", 7).v);
}

/* An ascending vector's bit k is index lsb - k: x[1] is its MSB. */
static void test_ascending_constant_selects(void) {
    read_clean("  assign o1 = x[1];\n  assign o2 = x[8];\n  assign n4 = x[2:5];\n");
    TEST_ASSERT_EQUAL_UINT32(wnet("x", 7).v, wnet("o1", 0).v);
    TEST_ASSERT_EQUAL_UINT32(wnet("x", 0).v, wnet("o2", 0).v);
    /* x[2:5]: MSB x[2] (wire bit 6) ... LSB x[5] (wire bit 3) */
    expect_same_nets((bits_of){"n4", 0}, (bits_of){"x", 3}, 4);
}

static void test_constant_plus_minus_selects(void) {
    read_clean("  assign n4 = w[2 +: 4];\n  assign y16 = {w[5 -: 2], x[2 +: 2], x[6 -: 4]};\n"
               "  assign o1 = w[3 +: 1];\n");
    expect_same_nets((bits_of){"n4", 0}, (bits_of){"w", 2}, 4);
    /* w[5 -: 2] = w[5:4]; x[2 +: 2] = x[2:3]; x[6 -: 4] = x[3:6] on [1:8] */
    expect_same_nets((bits_of){"y16", 0}, (bits_of){"x", 2}, 4);
    expect_same_nets((bits_of){"y16", 4}, (bits_of){"x", 5}, 2);
    expect_same_nets((bits_of){"y16", 6}, (bits_of){"w", 4}, 2);
    TEST_ASSERT_EQUAL_UINT32(wnet("w", 3).v, wnet("o1", 0).v);
}

/* A constant index from parameters folds: no cell. */
static void test_parameter_index_select(void) {
    read_clean("  localparam W = 4;\n  assign n4 = w[W-1:0];\n  assign o1 = w[W];\n");
    TEST_ASSERT_EQUAL_UINT32(0, count_logic());
    expect_same_nets((bits_of){"n4", 0}, (bits_of){"w", 0}, 4);
    TEST_ASSERT_EQUAL_UINT32(wnet("w", 4).v, wnet("o1", 0).v);
}

/* A part select out of range: the bits outside read x, with the warning (E17). */
static void test_out_of_range_part_select(void) {
    read_body("  assign n4 = w[9:6];\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    expect_warning((at_loc){1, 15},
                   "range select [9:6] of 'w' is out of bounds; bits outside [7:0] are x");
    TEST_ASSERT_EQUAL_UINT32(wnet("w", 6).v, wnet("n4", 0).v);
    TEST_ASSERT_EQUAL_UINT32(wnet("w", 7).v, wnet("n4", 1).v);
    expect_const_run("n4", (run_span){2, 4}, ODIN3_BIT_X);
    expect_ir_ok();
}

static void test_out_of_range_bit_select(void) {
    read_body("  assign o1 = w[9];\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    expect_warning((at_loc){1, 15}, "bit select [9] of 'w' is out of bounds; the bit is x");
    expect_const_bit("o1", 0, ODIN3_BIT_X);
    expect_ir_ok();
}

/* Extreme constant indices (review I1): no signed overflow, the select reads x with the
 * out-of-bounds warning. A base beyond +-2^40 is clamped to that edge, so the warning shows the
 * edge. One test per overflowing line of elab2_select.c. */
static void test_extreme_plus_base(void) { /* sel_orient: hi = lo + (w - 1) */
    read_body("  assign n4 = v[64'h7fffffffffffffff +: 4];\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    expect_warning((at_loc){1, 15}, "range select [1099511627779:1099511627776] of 'v' is out of "
                                    "bounds; bits outside [8:1] are x");
    expect_const_run("n4", (run_span){0, 4}, ODIN3_BIT_X);
    expect_ir_ok();
}

static void test_extreme_minus_base(void) { /* sel_orient: lo = base - (w - 1) */
    read_body("  assign n4 = v[-64'sd9223372036854775807 -: 4];\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    expect_warning((at_loc){1, 15}, "range select [-1099511627776:-1099511627779] of 'v' is out "
                                    "of bounds; bits outside [8:1] are x");
    expect_const_run("n4", (run_span){0, 4}, ODIN3_BIT_X);
    expect_ir_ok();
}

static void test_extreme_bit_index(void) { /* a bit select at INT64_MIN */
    read_body("  assign o1 = v[-64'sd9223372036854775808];\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    expect_warning((at_loc){1, 15},
                   "bit select [-1099511627776] of 'v' is out of bounds; the bit is x");
    expect_const_bit("o1", 0, ODIN3_BIT_X);
    expect_ir_ok();
}

static void test_extreme_bit_index_ascending(void) { /* sel_pos: lsb - idx on [1:8] */
    read_body("  assign o1 = x[-64'sd9223372036854775808];\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    expect_warning((at_loc){1, 15},
                   "bit select [-1099511627776] of 'x' is out of bounds; the bit is x");
    expect_const_bit("o1", 0, ODIN3_BIT_X);
    expect_ir_ok();
}

static void test_reversed_part_select(void) {
    read_failing("  assign n4 = w[0:3];\n");
    expect_error((at_loc){1, 15},
                 "part-select [0:3] of 'w' is reversed relative to its declaration [7:0]");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, diag_errors(), et_log(&rs));
}

/* w[i] reads through $shiftx: A = the whole wire, B = the index, signed for a 32-bit index. */
static void test_nonconstant_bit_read(void) {
    read_clean("  assign o1 = w[i];\n  assign o2 = w[c4];\n  assign y1 = w[s[2:0]] ? a : b;\n");
    odin3_node_id wide = driver_of("o1", 0);
    expect_bin(wide, (bin_want){"$shiftx", 0, 1, 8, 32, 1});
    expect_pins_are(wide, "A", (bits_of){"w", 0}, 8);
    expect_pins_are(wide, "B", (bits_of){"i", 0}, 32);
    expect_bin(driver_of("o2", 0), (bin_want){"$shiftx", 0, 0, 8, 4, 1}); /* unsigned 4-bit index */
    TEST_ASSERT_EQUAL_UINT32(0, count_of("$shr"));                        /* E17: x fill, no $shr */
}

/* v[i] of [8:1]: $sub(i, 1) signed first; x[i] of the ascending [1:8]: the two $subs. */
static void test_offset_reads(void) {
    read_clean("  assign o1 = v[i];\n  assign o2 = x[i];\n");
    odin3_node_id first = driver_of("o1", 0);
    expect_bin(first, (bin_want){"$shiftx", 0, 1, 8, 32, 1});
    odin3_node_id offset = nth_node("$sub", 0);
    expect_bin(offset, (bin_want){"$sub", 1, 1, 32, 32, 32});
    expect_pins_are(offset, "A", (bits_of){"i", 0}, 32);
    expect_pins_const(offset, "B", (run_span){0, 1}, ODIN3_BIT_1);
    expect_pins_const(offset, "B", (run_span){1, 32}, ODIN3_BIT_0);
    for (uint32_t bit = 0; bit < 32; bit++) {
        TEST_ASSERT_EQUAL_UINT32(pin_net(offset, "Y", bit).v, pin_net(first, "B", bit).v);
    }
    /* x[i] on [1:8]: shift = 7 - (i - 1) */
    odin3_node_id second = driver_of("o2", 0);
    expect_bin(second, (bin_want){"$shiftx", 0, 1, 8, 32, 1});
    TEST_ASSERT_EQUAL_UINT32(3, count_of("$sub"));
    odin3_node_id flip = nth_node("$sub", 2);
    expect_bin(flip, (bin_want){"$sub", 1, 1, 32, 32, 32});
    expect_pins_const(flip, "A", (run_span){0, 3}, ODIN3_BIT_1); /* 7 */
    expect_pins_const(flip, "A", (run_span){3, 32}, ODIN3_BIT_0);
    for (uint32_t bit = 0; bit < 32; bit++) {
        TEST_ASSERT_EQUAL_UINT32(pin_net(nth_node("$sub", 1), "Y", bit).v,
                                 pin_net(flip, "B", bit).v);
        TEST_ASSERT_EQUAL_UINT32(pin_net(flip, "Y", bit).v, pin_net(second, "B", bit).v);
    }
}

/* w[k -: 4]: the base is k - 3 (the select's lowest index), then the shift. */
static void test_minus_colon_read(void) {
    read_clean("  assign n4 = w[k -: 4];\n  assign y1 = w[k +: 4];\n");
    odin3_node_id sub = only_node("$sub");
    expect_bin(sub, (bin_want){"$sub", 0, 0, 8, 32, 32});
    expect_pins_are(sub, "A", (bits_of){"k", 0}, 8);
    expect_pins_const(sub, "B", (run_span){0, 2}, ODIN3_BIT_1); /* 3 */
    expect_pins_const(sub, "B", (run_span){2, 32}, ODIN3_BIT_0);
    odin3_node_id minus = driver_of("n4", 0);
    expect_bin(minus, (bin_want){"$shiftx", 0, 1, 8, 32, 4});
    for (uint32_t bit = 0; bit < 32; bit++) {
        TEST_ASSERT_EQUAL_UINT32(pin_net(sub, "Y", bit).v, pin_net(minus, "B", bit).v);
    }
    expect_bin(driver_of("y1", 0), (bin_want){"$shiftx", 0, 0, 8, 8, 4}); /* +: the base as is */
}

/* A parameter's bits can be selected by a variable too. */
static void test_parameter_variable_select(void) {
    read_clean("  localparam [7:0] K = 8'h5A;\n  assign o1 = K[c4];\n");
    odin3_node_id shiftx = driver_of("o1", 0);
    expect_bin(shiftx, (bin_want){"$shiftx", 0, 0, 8, 4, 1});
    for (uint32_t bit = 0; bit < 8; bit++) {
        bool one = ((0x5AU >> bit) & 1U) != 0;
        TEST_ASSERT_EQUAL_UINT32(const_net_of(one ? ODIN3_BIT_1 : ODIN3_BIT_0).v,
                                 pin_net(shiftx, "A", bit).v);
    }
}

/* --- attributes, folding, provenance ---------------------------------------------------------- */

static void test_attributes_and_folding(void) {
    read_clean(
        "  assign y = a + (* keep *) b;\n  assign y1 = 3 + 4;\n  assign y2 = 8'hF0 & 8'h3C;\n"
        "  assign y3 = ~(8'sh0F);\n");
    odin3_node_id add = only_node("$add");
    uint32_t keep = id_of("keep");
    const odin3_value *value = odin3_attr_get(top(), (odin3_objref){ODIN3_OBJ_NODE, add.v}, keep);
    TEST_ASSERT_NOT_NULL(value);
    TEST_ASSERT_EQUAL_INT(ODIN3_VAL_INT, value->kind);
    TEST_ASSERT_EQUAL_INT64(1, value->i);
    TEST_ASSERT_EQUAL_UINT32(1, count_logic());            /* the other three folded */
    expect_const_run("y1", (run_span){0, 3}, ODIN3_BIT_1); /* 7 */
    expect_const_run("y1", (run_span){3, 8}, ODIN3_BIT_0);
    expect_const_run("y2", (run_span){0, 2}, ODIN3_BIT_0); /* 0x30 */
    expect_const_run("y2", (run_span){4, 6}, ODIN3_BIT_1);
    expect_const_run("y3", (run_span){0, 4}, ODIN3_BIT_0); /* ~0x0F = 0xF0 */
    expect_const_run("y3", (run_span){4, 8}, ODIN3_BIT_1);
}

/* A keep on an operator reaches every cell that operator makes (~& makes two). */
static void test_attributes_on_every_cell(void) {
    read_clean("  assign y = ~& (* keep *) a;\n");
    uint32_t keep = id_of("keep");
    odin3_node_id first = only_node("$reduce_and");
    odin3_node_id second = only_node("$logic_not");
    TEST_ASSERT_NOT_NULL(odin3_attr_get(top(), (odin3_objref){ODIN3_OBJ_NODE, first.v}, keep));
    TEST_ASSERT_NOT_NULL(odin3_attr_get(top(), (odin3_objref){ODIN3_OBJ_NODE, second.v}, keep));
}

/* A cell's SOURCE record: the operator's location and the scope path of its generate block. */
static void test_cell_provenance(void) {
    read_clean("  assign y = a + b;\n  genvar g;\n"
               "  for (g = 0; g < 2; g = g + 1) begin : blk\n    wire [7:0] z = a & b;\n  end\n");
    odin3_node_id add = only_node("$add");
    const odin3_prov_record *rec = odin3_prov_get(rs.design, odin3_node_prov(top(), add));
    TEST_ASSERT_NOT_NULL(rec);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_SOURCE, rec->kind);
    TEST_ASSERT_EQUAL_UINT32(HEAD_LINES + 1, rec->locs[0].line);
    TEST_ASSERT_EQUAL_UINT32(14, rec->locs[0].col);
    TEST_ASSERT_EQUAL_STRING("top", odin3_strtab_get(odin3_design_strtab(rs.design), rec->hier));
    odin3_node_id inner = nth_node("$and", 1);
    TEST_ASSERT_NOT_EQUAL_UINT32(0, inner.v);
    const odin3_prov_record *scoped = odin3_prov_get(rs.design, odin3_node_prov(top(), inner));
    TEST_ASSERT_EQUAL_STRING("top/blk[1]",
                             odin3_strtab_get(odin3_design_strtab(rs.design), scoped->hier));
}

/* --- names that are not values, memories, calls ---------------------------------------------- */

/* A generate block's name read as a value is an error (Yosys: an implicit net; §17). */
static void test_block_name_is_not_a_value(void) {
    read_failing("  if (1) begin : gb\n    wire q;\n  end\n  assign o1 = gb;\n");
    expect_error((at_loc){4, 15}, "'gb' is not a net, variable or parameter");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, diag_errors(), et_log(&rs));
}

/* A memory word read is a $mem read port (Task 20): the rest of the module is emitted as well. */
static void test_memory_read_port(void) {
    read_clean("  reg [7:0] mem [0:3];\n  assign y = mem[1];\n  assign y1 = a;\n");
    TEST_ASSERT_EQUAL_UINT32(1, count_of("$mem"));
    TEST_ASSERT_EQUAL_UINT32(0, count_of("$memrd"));
    expect_same_nets((bits_of){"y1", 0}, (bits_of){"a", 0}, 8);
    odin3_node_id mem = nth_node("$mem", 0);
    TEST_ASSERT_EQUAL_INT64(1, param_of(mem, "RD_PORTS"));
    expect_pins_are(mem, "RD_DATA", (bits_of){"y", 0}, 8);
}

/* A user function call is the pending continuous-context call until Task 23. */
static void test_function_call_pending(void) {
    read_failing("  function [7:0] f;\n    input [7:0] p;\n    f = p;\n  endfunction\n"
                 "  assign y = f(a);\n  assign y1 = b;\n");
    expect_error((at_loc){5, 14},
                 "elaboration is not available yet (2D): 'continuous-context call'");
    expect_same_nets((bits_of){"y1", 0}, (bits_of){"b", 0}, 8);
}

/* A zero-width part of a concatenation is wiring that adds nothing (stage 1 rejects it alone). */
static void test_zero_width_part(void) {
    read_clean("  assign y = {a, {0{b}}};\n");
    TEST_ASSERT_EQUAL_UINT32(0, count_logic());
    expect_same_nets((bits_of){"y", 0}, (bits_of){"a", 0}, 8);
}

/* --- caps ------------------------------------------------------------------------------------ */

static void cell_pins_16(odin3_elab_limits *limits) {
    limits->max_cell_pins = 16;
}

static void test_cell_pins_cap(void) {
    odin3_elab_limits limits = *odin3_elab_limits_get();
    cell_pins_16(&limits);
    odin3_elab_test_set_limits(&limits);
    et_src src = {"t.v", "module top (input [8:0] p, input [8:0] q, output [8:0] r, output o);\n"
                         "  assign o = 1'b0;\n  assign r = p + q;\n  wire after;\nendmodule\n"};
    et_req req = {&src, 1, NULL, NULL, NULL};
    et_read_keep(&req, ODIN3_KEEP_AST_NONE, &rs);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_PARSE, rs.status, et_log(&rs));
    et_expect(&rs, ODIN3_LOG_ERROR,
              "t.v:3:14: error: cell of 27 pins exceeds the limit of 16 pins");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, diag_errors(), et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_design_top(rs.design).v);
    TEST_ASSERT_EQUAL_UINT32(0, count_of("$add"));
    expect_ir_ok();
}

/* A long & chain is 1999 cells, walked with explicit stacks. */
static void test_chain_depth(void) {
    static char body[CHAIN_TERMS * 4 + 64];
    size_t len = (size_t)snprintf(body, sizeof body, "  assign y = a");
    for (uint32_t term = 1; term < CHAIN_TERMS; term++) {
        len += (size_t)snprintf(body + len, sizeof body - len, " & a");
    }
    (void)snprintf(body + len, sizeof body - len, ";\n");
    read_clean(body);
    TEST_ASSERT_EQUAL_UINT32(CHAIN_TERMS - 1, count_of("$and"));
    expect_pins_are(driver_of("y", 0), "Y", (bits_of){"y", 0}, 8);
}

/* A deep right-nested expression too. */
static void test_nested_depth(void) {
    static char body[CHAIN_TERMS * 8 + 64];
    size_t len = (size_t)snprintf(body, sizeof body, "  assign y = a");
    for (uint32_t term = 1; term < CHAIN_TERMS; term++) {
        len += (size_t)snprintf(body + len, sizeof body - len, " ^ (b");
    }
    for (uint32_t term = 1; term < CHAIN_TERMS; term++) {
        len += (size_t)snprintf(body + len, sizeof body - len, ")");
    }
    (void)snprintf(body + len, sizeof body - len, ";\n");
    read_clean(body);
    TEST_ASSERT_EQUAL_UINT32(CHAIN_TERMS - 1, count_of("$xor"));
}

/* --- the helpers, direct --------------------------------------------------------------------- */

static odin3_elab2 *context_over(const char *text) {
    et_src src = {"t.v", text};
    et_req req = {&src, 1, NULL, NULL, NULL};
    et_stage1(&req, &rs);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    odin3_elab2 *e2 = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_elab2_create(&rs.ctx, rs.elab, &rs.opts, &e2));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_elab_bind(rs.elab, &e2->bind));
    e2->st = odin3_ast_symtab(rs.elab);
    odin3_ast_id module = odin3_ast_child(rs.elab, odin3_ast_root(rs.elab, 0), 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_elab2_ports(e2, module));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_elab2_enter(e2, module));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_elab2_wires(e2, module));
    return e2;
}

/* The sig of a wire's nets. */
static odin3_sig sig_of_wire(odin3_elab2 *e2, const char *name, bool is_signed) {
    odin3_wire_id wire = wire_of(name);
    uint32_t width = odin3_wire_width(top(), wire);
    odin3_sig sig = {NULL, 0, is_signed};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_elab2_sig_new(e2, width, &sig));
    sig.is_signed = is_signed;
    for (uint32_t bit = 0; bit < width; bit++) {
        sig.bits[bit] = odin3_wire_net(top(), wire, bit).v;
    }
    return sig;
}

/* extend is wiring: zero or sign repeat, truncation keeps the low bits; to_nets makes constants. */
static void test_extend_and_to_nets(void) {
    odin3_elab2 *e2 =
        context_over("module top (input [3:0] a, input signed [3:0] s);\nendmodule\n");
    odin3_sig unsigned_sig = sig_of_wire(e2, "a", false);
    odin3_sig signed_sig = sig_of_wire(e2, "s", true);
    uint32_t nodes = odin3_module_node_end(top());
    odin3_sig out = {NULL, 0, false};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_elab2_extend(e2, &unsigned_sig, 6, false, &out));
    TEST_ASSERT_EQUAL_UINT32(6, out.width);
    TEST_ASSERT_EQUAL_UINT32(unsigned_sig.bits[3], out.bits[3]);
    TEST_ASSERT_EQUAL_UINT32(ODIN3_SB_CONST | ODIN3_BIT_0, out.bits[4]);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_elab2_extend(e2, &signed_sig, 6, true, &out));
    TEST_ASSERT_EQUAL_UINT32(signed_sig.bits[3], out.bits[4]);
    TEST_ASSERT_EQUAL_UINT32(signed_sig.bits[3], out.bits[5]);
    TEST_ASSERT_TRUE(out.is_signed);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_elab2_extend(e2, &signed_sig, 2, true, &out));
    TEST_ASSERT_EQUAL_UINT32(2, out.width);
    TEST_ASSERT_EQUAL_UINT32(signed_sig.bits[1], out.bits[1]);
    TEST_ASSERT_EQUAL_UINT32(nodes, odin3_module_node_end(top())); /* wiring: no cell, no net */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_elab2_extend(e2, &unsigned_sig, 6, false, &out));
    odin3_net_id nets[6];
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_elab2_to_nets(e2, &out, nets));
    TEST_ASSERT_EQUAL_UINT32(unsigned_sig.bits[0], nets[0].v);
    odin3_net_id zero = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_elab2_const_net(e2, ODIN3_BIT_0, &zero));
    TEST_ASSERT_EQUAL_UINT32(zero.v, nets[4].v);
    TEST_ASSERT_EQUAL_UINT32(zero.v, nets[5].v);
    expect_ir_ok();
    odin3_elab2_destroy(e2);
}

/* odin3_elab2_cell by hand: parameters from the operands, Y on fresh nets or on dest. */
static void test_cell_direct(void) {
    odin3_elab2 *e2 = context_over("module top (input [3:0] a, input signed [1:0] s, "
                                   "output [4:0] y);\nendmodule\n");
    odin3_sig lhs = sig_of_wire(e2, "a", false);
    odin3_sig rhs = sig_of_wire(e2, "s", true);
    odin3_sig out = {NULL, 0, false};
    odin3_elab2_cell_req req = {ODIN3_E2T_ADD, {0}, &lhs, &rhs, 5, NULL, NULL};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_elab2_cell(e2, &req, &out));
    TEST_ASSERT_EQUAL_UINT32(5, out.width);
    odin3_node_id add = only_node("$add");
    expect_bin(add, (bin_want){"$add", 0, 1, 4, 2, 5});
    for (uint32_t bit = 0; bit < 5; bit++) {
        TEST_ASSERT_EQUAL_UINT32(pin_net(add, "Y", bit).v, out.bits[bit]);
    }
    odin3_net_id dest[5];
    for (uint32_t bit = 0; bit < 5; bit++) {
        dest[bit] = wnet("y", bit);
    }
    req = (odin3_elab2_cell_req){ODIN3_E2T_SUB, {0}, &lhs, &rhs, 5, dest, NULL};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_elab2_cell(e2, &req, &out));
    expect_pins_are(only_node("$sub"), "Y", (bits_of){"y", 0}, 5);
    TEST_ASSERT_EQUAL_UINT32(dest[0].v, out.bits[0]);
    expect_ir_ok();
    odin3_elab2_destroy(e2);
}

/* A hook-lowered cap through the direct call: fatal, nothing attached. */
static void test_cell_direct_cap(void) {
    odin3_elab2 *e2 = context_over("module top (input [3:0] a, output [3:0] y);\nendmodule\n");
    odin3_elab_limits limits = *odin3_elab_limits_get();
    cell_pins_16(&limits);
    odin3_elab_test_set_limits(&limits);
    e2->limits = odin3_elab_limits_get();
    odin3_sig lhs = sig_of_wire(e2, "a", false);
    odin3_sig out = {NULL, 0, false};
    odin3_elab2_cell_req req = {ODIN3_E2T_ADD, {0}, &lhs, &lhs, 9, NULL, NULL};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_elab2_cell(e2, &req, &out)); /* 17 pins */
    et_expect(&rs, ODIN3_LOG_ERROR, "cell of 17 pins exceeds the limit of 16 pins");
    TEST_ASSERT_TRUE(odin3_elab2_stopped(e2));
    TEST_ASSERT_EQUAL_UINT32(0, count_of("$add")); /* nothing attached */
    req.y_width = 8;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_elab2_cell(e2, &req, &out)); /* exactly 16: allowed */
    TEST_ASSERT_EQUAL_UINT32(1, count_of("$add"));
    expect_ir_ok();
    odin3_elab2_destroy(e2);
}

/* --- more rows: folding, constant conditions, targets and names ------------------------------ */

/* The bits [0, width) of wire `name` are the constants of value, exactly (no x or z). */
typedef struct value_want {
    uint64_t value;
    uint32_t width;
} value_want;

static void expect_value(const char *name, value_want want_value) {
    for (uint32_t bit = 0; bit < want_value.width; bit++) {
        odin3_bit want = ((want_value.value >> bit) & 1U) != 0 ? ODIN3_BIT_1 : ODIN3_BIT_0;
        expect_const_bit(name, bit, want);
    }
}

/* Operators on constants fold through odin3_cval_op: no cell, the value Yosys's calc.cc gives. */
static void test_folding_table(void) {
    read_clean("  assign y = 8'd200 + 8'd100;\n  assign y1 = 8'sh81 >>> 1;\n"
               "  assign y2 = ~8'h0F ^ 8'hFF;\n  assign y3 = -8'sd5;\n  assign o1 = 8'd5 < 8'd7;\n"
               "  assign o2 = &4'b1111 && |4'b0100;\n  assign n4 = {2'b10, 2'b01};\n"
               "  assign y16 = 2 ** 4;\n  assign y32 = 7 / 2 + 7 % 4;\n"
               "  assign q64 = (4'b1010 == 4'b1010) ? 8'hAA : 8'h55;\n");
    TEST_ASSERT_EQUAL_UINT32(0, count_logic());
    expect_value("y", (value_want){44, 8});
    expect_value("y1", (value_want){0xC0, 8});
    expect_value("y2", (value_want){0x0F, 8});
    expect_value("y3", (value_want){0xFB, 8});
    expect_value("o1", (value_want){1, 1});
    expect_value("o2", (value_want){1, 1});
    expect_value("n4", (value_want){9, 4});
    expect_value("y16", (value_want){16, 16});
    expect_value("y32", (value_want){6, 32});
    expect_value("q64", (value_want){0xAA, 64});
}

/* A constant x condition merges the branches bit by bit; with other branches it is a $mux on x. */
static void test_ternary_x_condition(void) {
    read_clean("  assign n4 = 1'bx ? 4'b0101 : 4'b0111;\n  assign y = 1'bx ? a : b;\n");
    expect_const_bit("n4", 0, ODIN3_BIT_1);
    expect_const_bit("n4", 1, ODIN3_BIT_X);
    expect_const_bit("n4", 2, ODIN3_BIT_1);
    expect_const_bit("n4", 3, ODIN3_BIT_0);
    odin3_node_id mux = only_node("$mux");
    TEST_ASSERT_EQUAL_UINT32(const_net_of(ODIN3_BIT_X).v, pin_net(mux, "S", 0).v);
    expect_pins_are(mux, "Y", (bits_of){"y", 0}, 8);
}

/* A constant condition picks a branch without generating the other one. */
static void test_ternary_constant_condition_skips_branch(void) {
    read_clean("  assign y = 1'b1 ? a : (b + d);\n  assign y1 = 1'b0 ? (a * b) : d;\n");
    TEST_ASSERT_EQUAL_UINT32(0, count_logic());
    expect_same_nets((bits_of){"y", 0}, (bits_of){"a", 0}, 8);
    expect_same_nets((bits_of){"y1", 0}, (bits_of){"d", 0}, 8);
}

/* A constant and wires side by side: the constant bits are the shared nets, the rest the wire. */
static void test_constant_in_concatenation(void) {
    read_clean("  assign y = {4'hA, a4};\n");
    expect_same_nets((bits_of){"y", 0}, (bits_of){"a4", 0}, 4);
    expect_const_bit("y", 4, ODIN3_BIT_0);
    expect_const_bit("y", 5, ODIN3_BIT_1);
    expect_const_bit("y", 6, ODIN3_BIT_0);
    expect_const_bit("y", 7, ODIN3_BIT_1);
}

/* x[i +: 3] of the ascending [1:8]: (8 - 3) - (i - 1). */
static void test_ascending_plus_read(void) {
    read_clean("  assign n4 = x[i +: 3];\n");
    TEST_ASSERT_EQUAL_UINT32(2, count_of("$sub"));
    odin3_node_id first = nth_node("$sub", 0);
    odin3_node_id flip = nth_node("$sub", 1);
    expect_bin(first, (bin_want){"$sub", 1, 1, 32, 32, 32});
    expect_bin(flip, (bin_want){"$sub", 1, 1, 32, 32, 32});
    expect_pins_const(flip, "A", (run_span){0, 1}, ODIN3_BIT_1); /* 5 = 8 - 3 */
    expect_pins_const(flip, "A", (run_span){1, 2}, ODIN3_BIT_0);
    expect_pins_const(flip, "A", (run_span){2, 3}, ODIN3_BIT_1);
    expect_pins_const(flip, "A", (run_span){3, 32}, ODIN3_BIT_0);
    odin3_node_id shiftx = driver_of("n4", 0);
    expect_bin(shiftx, (bin_want){"$shiftx", 0, 1, 8, 32, 3});
    for (uint32_t bit = 0; bit < 32; bit++) {
        TEST_ASSERT_EQUAL_UINT32(pin_net(flip, "Y", bit).v, pin_net(shiftx, "B", bit).v);
    }
}

/* A whole memory is not a value; the diagnostic names the fix. */
static void test_memory_name_is_not_a_value(void) {
    read_failing("  reg [7:0] mem [0:3];\n  assign y = mem;\n");
    expect_error((at_loc){2, 14}, "'mem' is a memory; select a word");
}

static void test_assign_to_parameter(void) {
    read_failing("  localparam P = 1;\n  assign P = a;\n");
    expect_error((at_loc){2, 10}, "'P' is not a net or variable");
}

/* References into a generate block's scope as targets and as values. */
static void test_hierarchical_names(void) {
    read_clean("  if (1) begin : gb\n    wire [7:0] z;\n  end\n  assign gb.z = a;\n"
               "  assign y = gb.z;\n");
    expect_same_nets((bits_of){"gb.z", 0}, (bits_of){"a", 0}, 8);
    expect_same_nets((bits_of){"y", 0}, (bits_of){"a", 0}, 8);
}

static void test_assign_reversed_target(void) {
    read_failing("  assign y[0:3] = d4;\n");
    expect_error((at_loc){1, 10},
                 "part-select [0:3] of 'y' is reversed relative to its declaration [7:0]");
}

/* An index with an x bit is not a constant target either; a negative one is outside the net. */
static void test_assign_unknown_and_negative_index(void) {
    read_failing("  assign y[1'bx] = c;\n");
    expect_error((at_loc){1, 10},
                 "continuous assignment to 'y' with a non-constant index is not supported");
    et_free(&rs);
    memset(&rs, 0, sizeof rs);
    read_body("  assign y[-1] = c;\n  assign y[4 +: 8] = a;\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    expect_warning((at_loc){1, 10}, "assignment to bits outside 'y' [7:0] is ignored");
    expect_warning((at_loc){2, 10}, "assignment to bits outside 'y' [7:0] is ignored");
    expect_same_nets((bits_of){"y", 4}, (bits_of){"a", 0}, 4);
    expect_ir_ok();
}

/* The fresh nets of a cell count against the net-bits cap, at the expression (fatal). */
static void net_bits_28(odin3_elab_limits *limits) {
    limits->max_net_bits = 28;
}

static void test_net_bits_cap_for_cells(void) {
    odin3_elab_limits limits = *odin3_elab_limits_get();
    net_bits_28(&limits);
    odin3_elab_test_set_limits(&limits);
    et_src src = {"t.v", "module top (input [7:0] a, input [7:0] b, output [7:0] y);\n"
                         "  assign y = (a + b) >> 1;\nendmodule\n"};
    et_req req = {&src, 1, NULL, NULL, NULL};
    et_read_keep(&req, ODIN3_KEEP_AST_NONE, &rs);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_PARSE, rs.status, et_log(&rs));
    et_expect(&rs, ODIN3_LOG_ERROR, "t.v:2:15: error: elaboration needs more than 28 net bits");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, diag_errors(), et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32(0, count_of("$add"));
    expect_ir_ok();
}

/* --- the hooks, direct ------------------------------------------------------------------------ */

static odin3_ast_id find_kind(odin3_ast_kind kind) {
    for (uint32_t id = 1; id < odin3_ast_node_end(rs.elab).v; id++) {
        if (odin3_ast_kind_of(rs.elab, (odin3_ast_id){id}) == kind) {
            return (odin3_ast_id){id};
        }
    }
    TEST_FAIL_MESSAGE("no such node");
    return (odin3_ast_id){0};
}

/* The rv hook: nets of wire a read as the constant 0, the others as they are. */
typedef struct reads_ctx {
    odin3_net_id a[4];
    uint32_t calls;
} reads_ctx;

static odin3_status reads_zero_a(void *user, odin3_net_id net, odin3_sigbit *out) {
    reads_ctx *ctx = user;
    ctx->calls++;
    for (unsigned i = 0; i < 4; i++) {
        if (ctx->a[i].v == net.v) {
            *out = ODIN3_SB_CONST | ODIN3_BIT_0;
        }
    }
    return ODIN3_OK;
}

static void test_reads_hook(void) {
    odin3_elab2 *e2 = context_over("module top (input [3:0] a, input [3:0] b, output [3:0] y);\n"
                                   "  assign y = a + b;\nendmodule\n");
    reads_ctx ctx = {{wnet("a", 0), wnet("a", 1), wnet("a", 2), wnet("a", 3)}, 0};
    odin3_elab2_reads reads = {reads_zero_a, &ctx};
    odin3_elab2_expr_req req = {find_kind(ODIN3_AST_BINARY), {4, true, false}, NULL, &reads};
    odin3_sig sig;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_elab2_expr(e2, &req, &sig));
    TEST_ASSERT_EQUAL_UINT32(8, ctx.calls); /* every net of a and of b, once */
    odin3_node_id add = only_node("$add");
    expect_bin(add, (bin_want){"$add", 0, 0, 4, 4, 4});
    expect_pins_const(add, "A", (run_span){0, 4}, ODIN3_BIT_0);
    expect_pins_are(add, "B", (bits_of){"b", 0}, 4);
    TEST_ASSERT_EQUAL_UINT32(4, sig.width);
    expect_ir_ok();
    odin3_elab2_destroy(e2);
}

static void test_select_wrapper_and_misuse(void) {
    odin3_elab2 *e2 = context_over("module top (input [3:0] a, output [1:0] y);\n"
                                   "  assign y = a[2:1];\nendmodule\n");
    odin3_elab2_expr_req req = {find_kind(ODIN3_AST_SELECT), {2, true, false}, NULL, NULL};
    odin3_sig sig;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_elab2_select(e2, &req, &sig));
    TEST_ASSERT_EQUAL_UINT32(2, sig.width);
    TEST_ASSERT_EQUAL_UINT32(wnet("a", 1).v, sig.bits[0]);
    TEST_ASSERT_EQUAL_UINT32(wnet("a", 2).v, sig.bits[1]);
    req.expr = find_kind(ODIN3_AST_IDENT);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_elab2_select(e2, &req, &sig));
    et_expect(&rs, ODIN3_LOG_ERROR, "odin3_elab2_select: not a SELECT node");
    req.expr = find_kind(ODIN3_AST_MODULE); /* not an expression at all */
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_elab2_expr(e2, &req, &sig));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_elab2_assign(e2, req.expr));
    odin3_elab2_destroy(e2);
}

/* An any-value bit (the process generator's `-`) is the $_CONSTX_ net. */
static void test_to_nets_any_value(void) {
    odin3_elab2 *e2 = context_over("module top (input a);\nendmodule\n");
    odin3_sig sig;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_elab2_sig_new(e2, 2, &sig));
    sig.bits[0] = ODIN3_SB_ANY;
    sig.bits[1] = ODIN3_SB_CONST | ODIN3_BIT_Z;
    odin3_net_id nets[2];
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_elab2_to_nets(e2, &sig, nets));
    TEST_ASSERT_EQUAL_UINT32(const_net_of(ODIN3_BIT_X).v, nets[0].v);
    TEST_ASSERT_EQUAL_UINT32(const_net_of(ODIN3_BIT_Z).v, nets[1].v);
    expect_ir_ok();
    odin3_elab2_destroy(e2);
}

/* --- the hand-built store (review I10) ------------------------------------------------------- */

/* A text summary of a module's cells in creation order: type and every INT parameter. */
static void summarize(odin3_design *design, odin3_module *module, char *out, size_t cap) {
    size_t len = 0;
    out[0] = '\0';
    for (uint32_t id = 1; id < odin3_module_node_end(module); id++) {
        odin3_node_id node = {id};
        if (!odin3_node_live(module, node)) {
            continue;
        }
        const odin3_celltype_def *def = odin3_celltype_get(design, odin3_node_type(module, node));
        if (def->gran == ODIN3_GRAN_PORT || strncmp(def->name, "$_CONST", 7) == 0) {
            continue;
        }
        len += (size_t)snprintf(out + len, cap - len, "%s", def->name);
        for (uint32_t param = 0; param < def->n_params; param++) {
            len += (size_t)snprintf(out + len, cap - len, " %lld",
                                    (long long)odin3_node_param(module, node, param)->i);
        }
        len += (size_t)snprintf(out + len, cap - len, ";");
    }
}

/* A new elaborated store held by `design` over a FILE buffer, for the eb_* builders. */
static void open_held_store(et_tree *tree, odin3_design *design) {
    *tree = (et_tree){.design = design, .line = 1, .lines = ET_LINES};
    odin3_pass_ctx ctx;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(design, et_intern(&rs, "hand"), &ctx));
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_design_new_ast(design, ODIN3_AST_FORM_ELABORATED, ctx.run.v, &tree->ast));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_srcman(design, &tree->sm));
    uint32_t name = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_ast_intern(tree->ast, odin3_bytes_cstr("hand.sv"), &name));
    odin3_srcfile_spec spec = {.name = name, .len = ET_LINE_BYTES * ET_LINES};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_add_file(tree->sm, &spec, &tree->buf));
    for (uint32_t line = 1; line < ET_LINES; line++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_srcman_add_line(tree->sm, tree->buf, line * ET_LINE_BYTES));
    }
    tree->at = odin3_srcman_loc(tree->sm, tree->buf, 0);
}

static odin3_ast_id hand_port(et_tree *tree, uint8_t dir, const char *name) {
    odin3_ast_id msb = eb_int(tree, 7);
    odin3_ast_id lsb = eb_int(tree, 0);
    eb_bounds bounds = {msb, lsb};
    eb_decl decl = {
        .sub = dir, .flags = eb_wire_port(), .range = eb_range(tree, bounds), .name = name};
    return eb_port(tree, &decl);
}

/* module top (input [7:0] a, b, output [7:0] y); assign y = (a + b) >> 1; endmodule */
static void test_hand_built_store(void) {
    read_clean("  assign y = (a + b) >> 1;\n");
    char parsed[512];
    summarize(rs.design, top(), parsed, sizeof parsed);
    et_free(&rs);
    memset(&rs, 0, sizeof rs);

    et_req req = {NULL, 0, NULL, NULL, NULL};
    et_setup(&req, &rs);
    et_tree tree;
    open_held_store(&tree, rs.design);
    odin3_ast_id ports[3] = {hand_port(&tree, ODIN3_AST_PORT_DECL_INPUT, "a"),
                             hand_port(&tree, ODIN3_AST_PORT_DECL_INPUT, "b"),
                             hand_port(&tree, ODIN3_AST_PORT_DECL_OUTPUT, "y")};
    odin3_ast_id sum =
        et_binary(&tree, ODIN3_AST_BINARY_ADD, et_ident(&tree, "a"), et_ident(&tree, "b"));
    odin3_ast_id shifted = et_binary(&tree, ODIN3_AST_BINARY_SHR, sum, eb_int(&tree, 1));
    odin3_ast_id assign = eb_assign(&tree, ODIN3_AST_CONT_ASSIGN, et_ident(&tree, "y"), shifted);
    eb_mod mod = {"top", {0}, eb_list(&tree, ports, 3), &assign, 1};
    odin3_ast_id module = eb_module(&tree, &mod);
    (void)eb_unit(&tree, &module, 1);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_ast_elaborate(rs.design, tree.ast, NULL),
                                  et_log(&rs));
    char built[512];
    summarize(rs.design, top(), built, sizeof built);
    TEST_ASSERT_EQUAL_STRING(parsed, built);
    TEST_ASSERT_EQUAL_UINT32(2, count_logic());
    expect_ir_ok();
}

/* --- determinism ------------------------------------------------------------------------------ */

static void slurp(const char *path, char *out, size_t cap) {
    FILE *file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(file, path);
    size_t got = fread(out, 1, cap - 1, file);
    out[got] = '\0';
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static const char k_rich_body[] =
    "  assign y = (a + b) >> 1;\n  assign y1 = c ? a : b;\n  assign y2 = ~&a;\n"
    "  assign y3 = w[i] ? s : {a[3:0], d4};\n  assign y16 = {a, b} ** 2;\n"
    "  assign o1 = a4 !== 4'b1x00;\n  assign n4 = w[k -: 4];\n"
    "  assign {o2, y32[2:0]} = w[k +: 4];\n  assign q64 = {2{a}} + (s >>> 2);\n"
    "  assign r64 = x[i +: 3] ? $signed(t) : {c4, c4};\n"
    "  assign y64 = 8'hFF ^ {a4, 4'b1x0z};\n";

static void rtlil_of_rich(char *out, size_t cap) {
    read_body(k_rich_body);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_pass_run(rs.design, "write_rtlil", odin3_bytes_cstr("rich.il")));
    slurp("rich.il", out, cap);
    TEST_ASSERT_EQUAL_INT(0, remove("rich.il"));
}

static void test_deterministic_rtlil(void) {
    static char first[TEXT_MAX];
    static char second[TEXT_MAX];
    rtlil_of_rich(first, sizeof first);
    et_free(&rs);
    memset(&rs, 0, sizeof rs);
    rtlil_of_rich(second, sizeof second);
    TEST_ASSERT_NOT_NULL(strstr(first, "cell $shiftx"));
    TEST_ASSERT_EQUAL_STRING(first, second);
}

/* --- out of memory ---------------------------------------------------------------------------- */

static bool g_saw_missing_file;
static bool g_saw_error; /* any ERROR-level line of the run */

static void quiet_sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    g_saw_error |= level == ODIN3_LOG_ERROR;
    g_saw_missing_file |= strstr(msg, "missing_file:") != NULL;
}

static void fresh_quiet_design(void) {
    odin3_design_destroy(rs.design);
    rs.design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(rs.design);
    odin3_log_set_sink(quiet_sink, NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_log_set_level(ODIN3_LOG_ERROR));
}

static bool read_complete(void) {
    return odin3_design_module_end(rs.design) == 2 && odin3_design_top(rs.design).v != 0;
}

/* A swallowed failure is an OK read that is incomplete or that logged an error. */
static void check_not_swallowed(odin3_status st) {
    if (st == ODIN3_OK) {
        TEST_ASSERT_FALSE_MESSAGE(g_saw_error, "an OK read logged an error");
        TEST_ASSERT_TRUE_MESSAGE(read_complete(), "an OK read is incomplete");
    }
}

/* What a run with an injected allocation failure may leave: the status, no missing file, the
 * IR whole, and no swallowed failure (an OK read is complete and logged no error). */
static void check_run(odin3_status st) {
    /* an injected allocation failure is NO_MEMORY, never a user error and never a missing file */
    TEST_ASSERT_TRUE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY);
    TEST_ASSERT_FALSE(g_saw_missing_file);
    check_not_swallowed(st);
    odin3_check_opts full = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(rs.design, full));
}

/* One run of the whole read with the limit-th allocation failing. */
static odin3_status read_failing_at(long limit) {
    fresh_quiet_design();
    g_saw_missing_file = false;
    g_saw_error = false;
    odin3_util_set_alloc_fail_after(limit);
    odin3_status st = odin3_pass_run(rs.design, "read_verilog", odin3_bytes_cstr("t.v"));
    odin3_util_set_alloc_fail_after(-1);
    check_run(st);
    return st;
}

/* Every allocation of the read fails in turn: NO_MEMORY, the IR check passes, no leak (ASan),
 * until the read completes OOM_SETTLE runs in a row. */
static void test_oom_sweep(void) {
    int got = snprintf(g_text, sizeof g_text,
                       "%s%s"
                       "endmodule\n",
                       k_head, k_rich_body);
    TEST_ASSERT_TRUE(got > 0 && (size_t)got < sizeof g_text);
    et_src src = {"t.v", g_text};
    et_req req = {&src, 1, NULL, NULL, NULL};
    et_setup(&req, &rs);
    uint32_t failures = 0;
    uint32_t settled = 0;
    for (long limit = 0; limit < OOM_LIMIT && settled < OOM_SETTLE; limit++) {
        odin3_status st = read_failing_at(limit);
        failures += st == ODIN3_ERR_NO_MEMORY ? 1U : 0U;
        settled = st == ODIN3_OK && read_complete() ? settled + 1 : 0;
    }
    TEST_ASSERT_EQUAL_UINT32(OOM_SETTLE, settled);
    TEST_ASSERT_GREATER_THAN_UINT32(200, failures);
}

/* Every test, in order (an X-macro keeps main short). */
#define ALL_TESTS(X)                                                                               \
    X(test_assign_by_construction)                                                                 \
    X(test_assign_alias_and_const)                                                                 \
    X(test_net_declaration_assignment)                                                             \
    X(test_port_declaration_assignment)                                                            \
    X(test_assign_non_constant_target)                                                             \
    X(test_assign_out_of_range_bits)                                                               \
    X(test_assign_select_and_concat_targets)                                                       \
    X(test_same_net_assignment_noop)                                                               \
    X(test_context_width_e22)                                                                      \
    X(test_context_width_wide)                                                                     \
    X(test_unsized_constants)                                                                      \
    X(test_signedness_e23)                                                                         \
    X(test_arithmetic_rows)                                                                        \
    X(test_unary_rows)                                                                             \
    X(test_unary_signed_extension)                                                                 \
    X(test_reduction_rows)                                                                         \
    X(test_nand_nor_rows)                                                                          \
    X(test_logic_rows)                                                                             \
    X(test_bitwise_rows)                                                                           \
    X(test_shift_rows)                                                                             \
    X(test_pow_rows)                                                                               \
    X(test_compare_rows)                                                                           \
    X(test_ternary_rows)                                                                           \
    X(test_ternary_wide_context)                                                                   \
    X(test_wiring_rows)                                                                            \
    X(test_case_equality_constants)                                                                \
    X(test_case_inequality_wildcard)                                                               \
    X(test_case_equality_plain)                                                                    \
    X(test_constant_selects)                                                                       \
    X(test_ascending_constant_selects)                                                             \
    X(test_constant_plus_minus_selects)                                                            \
    X(test_parameter_index_select)                                                                 \
    X(test_out_of_range_part_select)                                                               \
    X(test_out_of_range_bit_select)                                                                \
    X(test_extreme_plus_base)                                                                      \
    X(test_extreme_minus_base)                                                                     \
    X(test_extreme_bit_index)                                                                      \
    X(test_extreme_bit_index_ascending)                                                            \
    X(test_reversed_part_select)                                                                   \
    X(test_nonconstant_bit_read)                                                                   \
    X(test_offset_reads)                                                                           \
    X(test_minus_colon_read)                                                                       \
    X(test_parameter_variable_select)                                                              \
    X(test_attributes_and_folding)                                                                 \
    X(test_attributes_on_every_cell)                                                               \
    X(test_cell_provenance)                                                                        \
    X(test_block_name_is_not_a_value)                                                              \
    X(test_memory_read_port)                                                                       \
    X(test_function_call_pending)                                                                  \
    X(test_zero_width_part)                                                                        \
    X(test_cell_pins_cap)                                                                          \
    X(test_chain_depth)                                                                            \
    X(test_nested_depth)                                                                           \
    X(test_extend_and_to_nets)                                                                     \
    X(test_cell_direct)                                                                            \
    X(test_cell_direct_cap)                                                                        \
    X(test_folding_table)                                                                          \
    X(test_ternary_x_condition)                                                                    \
    X(test_ternary_constant_condition_skips_branch)                                                \
    X(test_constant_in_concatenation)                                                              \
    X(test_ascending_plus_read)                                                                    \
    X(test_memory_name_is_not_a_value)                                                             \
    X(test_assign_to_parameter)                                                                    \
    X(test_hierarchical_names)                                                                     \
    X(test_assign_reversed_target)                                                                 \
    X(test_assign_unknown_and_negative_index)                                                      \
    X(test_net_bits_cap_for_cells)                                                                 \
    X(test_reads_hook)                                                                             \
    X(test_select_wrapper_and_misuse)                                                              \
    X(test_to_nets_any_value)                                                                      \
    X(test_reduction_signed_operands)                                                              \
    X(test_logic_signed_operands)                                                                  \
    X(test_ternary_signed_condition)                                                               \
    X(test_plain_decimal_in_wide_context)                                                          \
    X(test_hand_built_store)                                                                       \
    X(test_deterministic_rtlil)                                                                    \
    X(test_oom_sweep)

#define RUN_ONE(name) RUN_TEST(name);

int main(void) {
    UNITY_BEGIN();
    ALL_TESTS(RUN_ONE)
    return UNITY_END();
}
