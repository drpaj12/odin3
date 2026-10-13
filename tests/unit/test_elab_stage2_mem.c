/*
 * test_elab_stage2_mem.c — 2D Task 20: memories (EL-27, EL-39 reads). The mem2reg decision
 * (Yosys 0.55's mem2reg_as_needed rules 1-5, `(* nomem2reg *)`, `--nomem2reg`), registers per
 * word, the `$mem` cell with its asynchronous read ports (raw-index addresses, simlib applies
 * OFFSET; the row-major address of a multi-dimensional array), the memory-bits cap, the write
 * port API (used from Task 24) and an OOM sweep. The whole read_verilog pass runs through the
 * pass manager (elab_test.h's et_read_keep); the IR is read back through the module API and every
 * located diagnostic is asserted as the whole line.
 *
 * Yosys 0.55 (git sha1 60f126cd0) is the oracle; the commands behind the expected values are in
 * the task report:
 *   yosys -q -p "read_verilog t.v; write_rtlil"                  (OFFSET, $memrd addresses,
 * flatten) yosys -q -p "read_verilog t.v; proc; memory_collect; write_rtlil"   (the collected $mem
 * ports) yosys -q -p "read_verilog t.v"                                (the "Replacing memory"
 * warnings)
 */
#include "elab_test.h"

#include "ast/ast.h"
#include "ast/kinds.h"
#include "frontends/elab/elab.h"
#include "frontends/elab/elab_internal.h"
#include "frontends/elab/elab_test.h"
#include "ir/celltype.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { OOM_LIMIT = 400000, OOM_SETTLE = 32, TEXT_MAX = 1 << 16, ABITS_32 = 32 };

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

/* The inputs the rows need; the body starts at line HEAD_LINES + 1 of t.v. */
static const char k_head[] =
    "module top (input [7:0] a, input [7:0] d, input [3:0] a4, input [3:0] ra,\n"
    "            input [2:0] ra3, input [1:0] s2, input c, input r, input [2:0] wa,\n"
    "            input signed [7:0] sa,\n"
    "            output [7:0] y, output [7:0] y1, output [3:0] y4, output [3:0] y5,\n"
    "            output [7:0] q);\n";
enum { HEAD_LINES = 5 };

static char g_text[TEXT_MAX];

/* The pass's arguments and the text of t.v. */
typedef struct mem_src {
    const char *args;
    const char *text;
} mem_src;

static void read_with(const mem_src *src) {
    et_src file = {"t.v", src->text};
    et_req req = {&file, 1, NULL, NULL, NULL};
    et_setup(&req, &rs);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_set_keep_ast(rs.design, ODIN3_KEEP_AST_NONE));
    rs.status = odin3_pass_run(rs.design, "read_verilog", odin3_bytes_cstr(src->args));
}

/* head + body + endmodule, read through the whole pass, with --nomem2reg when asked. */
static void read_body_with(bool nomem2reg, const char *body) {
    int got = snprintf(g_text, sizeof g_text, "%s%s%s", k_head, body, "endmodule\n");
    TEST_ASSERT_TRUE(got > 0 && (size_t)got < sizeof g_text);
    mem_src src = {nomem2reg ? "--nomem2reg t.v" : "t.v", g_text};
    read_with(&src);
}

static void read_body(const char *body) {
    read_body_with(false, body);
}

static void expect_ir_ok(void) {
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_check_design(rs.design, opts), et_log(&rs));
}

static uint32_t diag_errors(void) {
    return rs.errors - et_count_with(&rs, ODIN3_LOG_ERROR, "pass read_verilog: failed");
}

/* The warnings of 2D: not the IR check's (the wide head leaves outputs undriven). */
static uint32_t soft_warnings(void) {
    return rs.warnings - et_count_with(&rs, ODIN3_LOG_WARN, "check: ");
}

static void read_clean(const char *body) {
    read_body(body);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, diag_errors(), et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, soft_warnings(), et_log(&rs));
    expect_ir_ok();
}

/* A failed stage 2 leaves valid, incomplete modules (EL-2). */
static void read_failing(const char *body) {
    read_body(body);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_PARSE, rs.status, et_log(&rs));
    expect_ir_ok();
}

/* A read that succeeds (warnings allowed) with a valid IR. */
static void read_ok(const char *body) {
    read_body(body);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    expect_ir_ok();
}

/* "t.v:<line>:<col>: <level>: <msg>" (body line numbers count from the head's end). */
typedef struct at_loc {
    uint32_t line, col;
} at_loc;

static const char *diag_line(at_loc at, const char *level, const char *msg) {
    static char buf[768];
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

/* The "replacing memory" warning: name and the places ("t.v:L" joined by ", "). */
static const char *replacing(const char *name, const char *places) {
    static char buf[256];
    TEST_ASSERT_TRUE(snprintf(buf, sizeof buf,
                              "replacing memory '%s' with list of registers; see %s", name,
                              places) > 0);
    return buf;
}

/* "t.v:<line>" for a body line. */
static const char *place(uint32_t line) {
    static char buf[4][32];
    static unsigned next;
    char *out = buf[next++ % 4U];
    TEST_ASSERT_TRUE(snprintf(out, 32, "t.v:%u", (unsigned)(HEAD_LINES + line)) > 0);
    return out;
}

static const char *places2(uint32_t first, uint32_t second) {
    static char buf[96];
    TEST_ASSERT_TRUE(snprintf(buf, sizeof buf, "t.v:%u, t.v:%u", (unsigned)(HEAD_LINES + first),
                              (unsigned)(HEAD_LINES + second)) > 0);
    return buf;
}

static uint32_t warnings_with(const char *needle) {
    return et_count_with(&rs, ODIN3_LOG_WARN, needle);
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

static odin3_wire_id find_wire(const char *name) {
    uint32_t id = 0;
    if (!odin3_strtab_find(odin3_design_strtab(rs.design), odin3_bytes_cstr(name), &id)) {
        return (odin3_wire_id){0};
    }
    return odin3_module_find_wire(top(), id);
}

static odin3_wire_id wire_of(const char *name) {
    odin3_wire_id wire = find_wire(name);
    TEST_ASSERT_NOT_EQUAL_UINT32_MESSAGE(0, wire.v, name);
    return wire;
}

static odin3_net_id wnet(const char *name, uint32_t bit) {
    odin3_net_id net = odin3_wire_net(top(), wire_of(name), bit);
    TEST_ASSERT_NOT_EQUAL_UINT32_MESSAGE(0, net.v, name);
    return net;
}

static const odin3_celltype_def *def_of(odin3_node_id node) {
    return odin3_celltype_get(rs.design, odin3_node_type(top(), node));
}

static odin3_node_id nth_node(const char *type, uint32_t nth) {
    const odin3_module *module = top();
    uint32_t seen = 0;
    for (uint32_t id = 1; id < odin3_module_node_end(module); id++) {
        odin3_node_id node = {id};
        if (odin3_node_live(module, node) && strcmp(def_of(node)->name, type) == 0 &&
            seen++ == nth) {
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

static odin3_node_id only_node(const char *type) {
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, count_of(type), type);
    return nth_node(type, 0);
}

static const odin3_value *value_of(odin3_node_id node, const char *name) {
    const odin3_celltype_def *def = def_of(node);
    for (uint32_t index = 0; index < def->n_params; index++) {
        if (strcmp(def->params[index].name, name) == 0) {
            const odin3_value *value = odin3_node_param(top(), node, index);
            TEST_ASSERT_NOT_NULL(value);
            return value;
        }
    }
    TEST_FAIL_MESSAGE(name);
    return NULL;
}

static int64_t param_of(odin3_node_id node, const char *name) {
    return value_of(node, name)->i;
}

/* A BITS parameter as text, LSB first ('0', '1', 'x', 'z'). */
static const char *bits_of_param(odin3_node_id node, const char *name) {
    static char buf[64];
    const odin3_value *value = value_of(node, name);
    TEST_ASSERT_TRUE(value->len < sizeof buf);
    for (uint32_t k = 0; k < value->len; k++) {
        buf[k] = "01xz"[value->bits[k]];
    }
    buf[value->len] = '\0';
    return buf;
}

static void expect_memid(odin3_node_id node, const char *want) {
    const odin3_value *value = value_of(node, "MEMID");
    TEST_ASSERT_EQUAL_INT(ODIN3_VAL_STRING, value->kind);
    TEST_ASSERT_EQUAL_STRING(want, odin3_strtab_get(odin3_design_strtab(rs.design), value->str));
}

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

static uint32_t port_width(odin3_node_id node, const char *port) {
    const odin3_celltype_def *def = def_of(node);
    for (uint32_t index = 0; index < def->n_ports; index++) {
        if (strcmp(def->ports[index].name, port) == 0) {
            return odin3_node_port(top(), node, index).count;
        }
    }
    TEST_FAIL_MESSAGE(port);
    return 0;
}

static odin3_net_id const_net_of(odin3_bit bit) {
    static const char *const k_names[4] = {"$_CONST0_", "$_CONST1_", "$_CONSTX_", "$_CONSTZ_"};
    odin3_node_id node = nth_node(k_names[bit], 0);
    TEST_ASSERT_NOT_EQUAL_UINT32(0, node.v);
    return pin_net(node, "Y", 0);
}

typedef struct bits_of {
    const char *name;
    uint32_t first;
} bits_of;

/* Pins [pin_first, pin_first + count) of port are the nets of wire bits [src.first, ...). */
typedef struct pins_at {
    const char *port;
    uint32_t pin_first;
} pins_at;

static void expect_pins_are(odin3_node_id node, pins_at at, bits_of src, uint32_t count) {
    for (uint32_t k = 0; k < count; k++) {
        char why[96];
        TEST_ASSERT_TRUE(snprintf(why, sizeof why, "%s pin %u vs %s[%u]", at.port,
                                  (unsigned)(at.pin_first + k), src.name,
                                  (unsigned)(src.first + k)) > 0);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(wnet(src.name, src.first + k).v,
                                         pin_net(node, at.port, at.pin_first + k).v, why);
    }
}

/* Pins [first, first + count) of port on the constant `want`. */
static void expect_pins_const(odin3_node_id node, pins_at at, uint32_t count, odin3_bit want) {
    for (uint32_t k = 0; k < count; k++) {
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(const_net_of(want).v,
                                         pin_net(node, at.port, at.pin_first + k).v, at.port);
    }
}

/* The constant a run of address pins holds: LSB first, only 0 and 1 nets. */
static uint64_t pins_value(odin3_node_id node, pins_at at, uint32_t count) {
    uint64_t value = 0;
    for (uint32_t k = 0; k < count; k++) {
        odin3_net_id net = pin_net(node, at.port, at.pin_first + k);
        if (net.v == const_net_of(ODIN3_BIT_1).v) {
            value |= UINT64_C(1) << k;
        } else {
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(const_net_of(ODIN3_BIT_0).v, net.v, at.port);
        }
    }
    return value;
}

/* --- rule 1: forced ------------------------------------------------------------------------- */

/* A net array is registers, silently: wires w[0] .. w[3], no $mem (rule 1, N11). */
static void test_rule1_net_array_words(void) {
    read_clean("  wire [3:0] w [0:3];\n  assign w[0] = a4;\n  assign y4 = w[0];\n");
    for (uint32_t k = 0; k < 4; k++) {
        char name[16];
        TEST_ASSERT_TRUE(snprintf(name, sizeof name, "w[%u]", (unsigned)k) > 0);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(4, odin3_wire_width(top(), wire_of(name)), name);
    }
    TEST_ASSERT_EQUAL_UINT32(0, count_of("$mem"));
}

/* The words are named by the declared index: [1:4] gives w[1] .. w[4] and no w[0]. */
static void test_rule1_declared_index_names(void) {
    read_clean("  wire [3:0] w [1:4];\n  assign w[4] = a4;\n  assign y4 = w[4];\n");
    for (uint32_t k = 1; k <= 4; k++) {
        char name[16];
        TEST_ASSERT_TRUE(snprintf(name, sizeof name, "w[%u]", (unsigned)k) > 0);
        TEST_ASSERT_NOT_EQUAL_UINT32_MESSAGE(0, find_wire(name).v, name);
    }
    TEST_ASSERT_EQUAL_UINT32(0, find_wire("w[0]").v);
    TEST_ASSERT_EQUAL_UINT32(0, count_of("$mem"));
}

/* `(* mem2reg *)` on the array: registers, silently (a reg array read-only would be a memory). */
static void test_rule1_mem2reg_attribute(void) {
    read_clean("  (* mem2reg *) reg [7:0] m [0:2];\n");
    TEST_ASSERT_NOT_EQUAL_UINT32(0, find_wire("m[0]").v);
    TEST_ASSERT_NOT_EQUAL_UINT32(0, find_wire("m[2]").v);
    TEST_ASSERT_EQUAL_UINT32(0, count_of("$mem"));
}

/* `(* mem2reg *)` on the module forces every array of it. */
static void test_rule1_mem2reg_module_attribute(void) {
    static const char k_text[] = "(* mem2reg *)\nmodule top (input [3:0] a4, output [3:0] y4);\n"
                                 "  reg [3:0] m [0:1];\n  assign y4 = a4;\nendmodule\n";
    mem_src src = {"t.v", k_text};
    read_with(&src);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    TEST_ASSERT_NOT_EQUAL_UINT32(0, find_wire("m[1]").v);
    TEST_ASSERT_EQUAL_UINT32(0, count_of("$mem"));
}

/* A reg array nothing forces stays a memory (read-only: no write at all, so not rule 5). */
static void test_read_only_reg_array_is_memory(void) {
    read_clean("  reg [7:0] m [0:3];\n  assign y = m[ra];\n");
    TEST_ASSERT_EQUAL_UINT32(1, count_of("$mem"));
    TEST_ASSERT_EQUAL_UINT32(0, find_wire("m[0]").v);
}

/* --- rules 2 to 5 --------------------------------------------------------------------------- */

/* Rule 2: written in always @*: registers, with the warning naming the write's line. */
static void test_rule2_always_star(void) {
    read_body("  reg [7:0] m [0:3];\n  always @*\n    m[1] = a;\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    expect_warning((at_loc){1, 13}, replacing("m", place(3)));
    TEST_ASSERT_NOT_EQUAL_UINT32(0, find_wire("m[3]").v);
    TEST_ASSERT_EQUAL_UINT32(0, count_of("$mem"));
    TEST_ASSERT_EQUAL_UINT32(1, count_of("$proc")); /* the write of the word is a process now */
}

/* Rule 2: an asynchronous reset is two edge events, so not a single-edge block. */
static void test_rule2_async_reset(void) {
    read_ok("  reg [7:0] m [0:3];\n  always @(posedge c or posedge r)\n    m[wa] <= a;\n");
    expect_warning((at_loc){1, 13}, replacing("m", place(3)));
    TEST_ASSERT_NOT_EQUAL_UINT32(0, find_wire("m[0]").v);
}

/* One edge event with a variable-index write: a memory, no warning. */
static void test_rule2_single_edge_stays_memory(void) {
    read_ok("  reg [7:0] m [0:3];\n  always @(posedge c)\n    m[wa] <= a;\n"
            "  assign y = m[ra];\n");
    TEST_ASSERT_EQUAL_UINT32(0, warnings_with("replacing memory"));
    TEST_ASSERT_EQUAL_UINT32(1, count_of("$mem"));
    TEST_ASSERT_EQUAL_UINT32(0, find_wire("m[0]").v);
}

/* Rule 3: read after a blocking write in the same block. Yosys's places: each block's first
 * blocking write and the read, newest first (a hashlib pool iterates in reverse). */
static void test_rule3_read_after_blocking_write(void) {
    read_ok("  reg [7:0] m [0:3];\n  reg [7:0] t;\n  always @(posedge c) begin\n"
            "    m[wa] = a;\n    t = m[1];\n  end\n");
    expect_warning((at_loc){1, 13}, replacing("m", places2(5, 4)));
    TEST_ASSERT_NOT_EQUAL_UINT32(0, find_wire("m[2]").v);
}

/* A read in another block than the blocking write is no rule 3. */
static void test_rule3_other_block_stays_memory(void) {
    read_ok("  reg [7:0] m [0:3];\n  reg [7:0] t;\n  always @(posedge c)\n    m[wa] = a;\n"
            "  always @(posedge c)\n    t = m[1];\n");
    TEST_ASSERT_EQUAL_UINT32(0, warnings_with("replacing memory"));
}

/* Rule 4: the array inside the index of its own write. */
static void test_rule4_nested_index(void) {
    read_ok("  reg [7:0] m [0:3];\n  always @(posedge c)\n    m[m[0]] <= a;\n");
    expect_warning((at_loc){1, 13}, replacing("m", place(3)));
    TEST_ASSERT_NOT_EQUAL_UINT32(0, find_wire("m[1]").v);
}

/* An array in a concatenation lvalue is no complex lvalue: Yosys splits the concatenation first,
 * so the part is judged as any write. Verified on 0.55:
 *   yosys -p "read_verilog t.v; proc; stat"  with  {m[wa], t} <= {a, a};  and  assign y = m[ra];
 * keeps one memory and prints no "replacing memory" warning. */
static void test_rule4_concat_lvalue(void) {
    read_ok("  reg [7:0] m [0:3];\n  reg [7:0] t;\n  always @(posedge c)\n"
            "    {m[wa], t} <= {a, a};\n");
    TEST_ASSERT_EQUAL_UINT32(0, warnings_with("replacing memory"));
}

/* Rule 5: every write outside initial blocks has a constant index. */
static void test_rule5_constant_writes_only(void) {
    read_body("  reg [7:0] m [0:3];\n  always @(posedge c)\n    m[1] <= a;\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    expect_warning((at_loc){1, 13}, replacing("m", place(3)));
    TEST_ASSERT_NOT_EQUAL_UINT32(0, find_wire("m[3]").v);
    TEST_ASSERT_EQUAL_UINT32(1, count_of("$proc"));
}

/* A for loop whose variable starts and steps by constants under a constant condition is unrolled
 * by Yosys before mem2reg: its indices are constants (a reset loop makes registers: rule 5). */
static void test_rule5_unrolled_loop(void) {
    read_ok("  reg [3:0] m [0:3];\n  integer i;\n  always @(posedge c)\n"
            "    for (i = 0; i < 4; i = i + 1)\n      m[i + 1] <= a4;\n");
    expect_warning((at_loc){1, 13}, replacing("m", place(5)));
}

/* A loop whose condition depends on an input cannot be unrolled: the index stays variable. */
static void test_rule5_loop_with_variable_bound(void) {
    read_failing("  reg [3:0] m [0:3];\n  integer i;\n  always @(posedge c)\n"
                 "    for (i = 0; i < ra; i = i + 1)\n      m[i] <= a4;\n");
    TEST_ASSERT_EQUAL_UINT32(0, warnings_with("replacing memory"));
}

/* One variable-index write among constant ones keeps the memory. */
static void test_rule5_variable_write_keeps_memory(void) {
    read_ok("  reg [7:0] m [0:3];\n  always @(posedge c) begin\n    m[1] <= a;\n"
            "    m[wa] <= d;\n  end\n");
    TEST_ASSERT_EQUAL_UINT32(0, warnings_with("replacing memory"));
}

/* Writes in initial blocks do not count for rule 5 (read-only otherwise: a memory). */
static void test_initial_writes_are_not_rule5(void) {
    read_ok("  reg [7:0] m [0:3];\n  initial m[0] = 8'd1;\n  assign y = m[ra];\n");
    TEST_ASSERT_EQUAL_UINT32(0, warnings_with("replacing memory"));
    TEST_ASSERT_EQUAL_UINT32(1, count_of("$mem"));
}

/* The initial block's context ends with it: a continuous write after it is rule 5 again, and
 * Yosys's places are the initial write and the assignment, newest first. */
static void test_initial_context_ends(void) {
    read_ok("  reg [3:0] m [0:3];\n  initial m[0] = 4'd1;\n  assign m[2] = a4;\n"
            "  assign y4 = m[2];\n");
    expect_warning((at_loc){1, 13}, replacing("m", places2(3, 2)));
}

/* A constant continuous write is rule 5 too; the word is then a net of the assignment. */
static void test_rule5_continuous_constant_write(void) {
    read_body("  reg [3:0] m [0:3];\n  assign m[2] = a4;\n  assign y4 = m[2];\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    expect_warning((at_loc){1, 13}, replacing("m", place(2)));
    expect_warning((at_loc){2, 10},
                   "'m' is a variable assigned by a continuous assignment; treated as a net");
    TEST_ASSERT_EQUAL_UINT32(0, count_of("$mem"));
    for (uint32_t k = 0; k < 4; k++) {
        TEST_ASSERT_EQUAL_UINT32(wnet("a4", k).v, wnet("y4", k).v);
    }
    expect_ir_ok();
}

/* `(* nomem2reg *)` exempts the array from rules 2 to 5 (its write is a port: Task 24). */
static void test_nomem2reg_attribute_exempts(void) {
    read_ok("  (* nomem2reg *) reg [7:0] m [0:3];\n  always @(posedge c)\n    m[1] <= a;\n"
            "  assign y = m[ra];\n");
    TEST_ASSERT_EQUAL_UINT32(0, warnings_with("replacing memory"));
    TEST_ASSERT_EQUAL_UINT32(1, count_of("$mem"));
}

/* `(* nomem2reg *)` does not undo rule 1: a net array still has to be registers. */
static void test_nomem2reg_attribute_keeps_rule1(void) {
    read_clean("  (* nomem2reg *) wire [3:0] w [0:1];\n");
    TEST_ASSERT_NOT_EQUAL_UINT32(0, find_wire("w[1]").v);
    TEST_ASSERT_EQUAL_UINT32(0, count_of("$mem"));
}

/* --- --nomem2reg ---------------------------------------------------------------------------- */

#define NOMEM_ERROR                                                                                \
    "array 'w' cannot be a memory (it is written in a combinational block) and "                   \
    "--nomem2reg is set"

/* A net array assigned by `assign`: nothing can make it a memory. */
static void test_nomem2reg_option_net_array_assign(void) {
    read_body_with(true, "  wire [3:0] w [0:3];\n  assign w[1] = a4;\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_PARSE, rs.status, et_log(&rs));
    expect_error((at_loc){2, 10}, NOMEM_ERROR);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, diag_errors(), et_log(&rs));
    expect_ir_ok();
}

/* --nomem2reg disables rule 1 too: a read-only net array is a memory. */
static void test_nomem2reg_option_net_array_is_memory(void) {
    read_body_with(true, "  wire [3:0] w [0:3];\n  assign y4 = w[ra3[1:0]];\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32(1, count_of("$mem"));
    TEST_ASSERT_EQUAL_UINT32(0, find_wire("w[0]").v);
}

/* --nomem2reg: a write in a block without exactly one edge event is the same error. */
static void test_nomem2reg_option_async_write(void) {
    read_body_with(true, "  reg [3:0] w [0:3];\n  always @*\n    w[1] = a4;\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_PARSE, rs.status, et_log(&rs));
    expect_error((at_loc){3, 5}, NOMEM_ERROR);
    TEST_ASSERT_EQUAL_UINT32(0, warnings_with("replacing memory"));
}

/* --nomem2reg: rules 2 to 5 do not apply, so a clocked constant write stays a memory. */
static void test_nomem2reg_option_skips_rules(void) {
    read_body_with(true, "  reg [7:0] m [0:3];\n  always @(posedge c)\n    m[1] <= a;\n"
                         "  assign y = m[ra];\n");
    TEST_ASSERT_EQUAL_UINT32(0, warnings_with("replacing memory"));
    TEST_ASSERT_EQUAL_UINT32(1, count_of("$mem"));
}

/* --- the $mem cell and its read ports -------------------------------------------------------- */

/* The addressing of a read-only [1:4] array: the raw index on ra's own nets. */
static void test_read_port_cell(void) {
    read_clean("  reg [7:0] m [1:4];\n  assign y = m[ra];\n");
    odin3_node_id mem = only_node("$mem");
    expect_memid(mem, "m");
    TEST_ASSERT_EQUAL_INT64(8, param_of(mem, "WIDTH"));
    TEST_ASSERT_EQUAL_INT64(4, param_of(mem, "SIZE"));
    TEST_ASSERT_EQUAL_INT64(1, param_of(mem, "OFFSET"));
    TEST_ASSERT_EQUAL_INT64(4, param_of(mem, "ABITS"));
    TEST_ASSERT_EQUAL_INT64(1, param_of(mem, "RD_PORTS"));
    TEST_ASSERT_EQUAL_INT64(0, param_of(mem, "WR_PORTS"));
    TEST_ASSERT_EQUAL_STRING("0", bits_of_param(mem, "RD_CLK_ENABLE"));
    TEST_ASSERT_EQUAL_STRING("0", bits_of_param(mem, "RD_TRANSPARENT"));
    TEST_ASSERT_EQUAL_STRING("0", bits_of_param(mem, "RD_CLK_POLARITY"));
    TEST_ASSERT_EQUAL_STRING("", bits_of_param(mem, "WR_CLK_ENABLE"));
    TEST_ASSERT_EQUAL_STRING("", bits_of_param(mem, "INIT"));
    expect_pins_const(mem, (pins_at){"RD_EN", 0}, 1, ODIN3_BIT_1);
    expect_pins_const(mem, (pins_at){"RD_CLK", 0}, 1, ODIN3_BIT_X);
    expect_pins_are(mem, (pins_at){"RD_ADDR", 0}, (bits_of){"ra", 0}, 4);
    expect_pins_are(mem, (pins_at){"RD_DATA", 0}, (bits_of){"y", 0}, 8);
    TEST_ASSERT_EQUAL_UINT32(0, count_of("$sub"));
    TEST_ASSERT_EQUAL_UINT32(0, count_of("$add"));
}

/* [4:1] and [-2:1] arrays: OFFSET is the lowest declared index, the address stays raw. */
static void test_offsets(void) {
    read_clean("  reg [7:0] m [4:1];\n  reg [3:0] n [-2:1];\n  assign y = m[ra];\n"
               "  assign y4 = n[ra];\n");
    TEST_ASSERT_EQUAL_UINT32(2, count_of("$mem"));
    odin3_node_id first = nth_node("$mem", 0);
    odin3_node_id second = nth_node("$mem", 1);
    expect_memid(first, "m");
    TEST_ASSERT_EQUAL_INT64(1, param_of(first, "OFFSET"));
    TEST_ASSERT_EQUAL_INT64(4, param_of(first, "SIZE"));
    expect_memid(second, "n");
    TEST_ASSERT_EQUAL_INT64(-2, param_of(second, "OFFSET"));
    TEST_ASSERT_EQUAL_INT64(4, param_of(second, "SIZE"));
    expect_pins_are(first, (pins_at){"RD_ADDR", 0}, (bits_of){"ra", 0}, 4);
    expect_pins_are(second, (pins_at){"RD_ADDR", 0}, (bits_of){"ra", 0}, 4);
}

/* Two reads: two ports in source order; ABITS is the widest address, narrower ones are
 * zero-extended (Yosys's memory_collect). */
static void test_two_read_ports(void) {
    read_clean("  reg [7:0] m [0:7];\n  assign y = m[ra3];\n  assign y1 = m[ra];\n");
    odin3_node_id mem = only_node("$mem");
    TEST_ASSERT_EQUAL_INT64(2, param_of(mem, "RD_PORTS"));
    TEST_ASSERT_EQUAL_INT64(4, param_of(mem, "ABITS"));
    TEST_ASSERT_EQUAL_STRING("00", bits_of_param(mem, "RD_CLK_ENABLE"));
    TEST_ASSERT_EQUAL_UINT32(8, port_width(mem, "RD_ADDR"));
    expect_pins_are(mem, (pins_at){"RD_ADDR", 0}, (bits_of){"ra3", 0}, 3);
    expect_pins_const(mem, (pins_at){"RD_ADDR", 3}, 1, ODIN3_BIT_0);
    expect_pins_are(mem, (pins_at){"RD_ADDR", 4}, (bits_of){"ra", 0}, 4);
    expect_pins_are(mem, (pins_at){"RD_DATA", 0}, (bits_of){"y", 0}, 8);
    expect_pins_are(mem, (pins_at){"RD_DATA", 8}, (bits_of){"y1", 0}, 8);
    expect_pins_const(mem, (pins_at){"RD_EN", 0}, 2, ODIN3_BIT_1);
    expect_pins_const(mem, (pins_at){"RD_CLK", 0}, 2, ODIN3_BIT_X);
}

/* A constant index is generated at its self-determined width (an unsized 2 is 32 bits). */
static void test_constant_index_read(void) {
    read_clean("  reg [7:0] m [0:3];\n  assign y = m[2];\n");
    odin3_node_id mem = only_node("$mem");
    TEST_ASSERT_EQUAL_INT64(ABITS_32, param_of(mem, "ABITS"));
    TEST_ASSERT_EQUAL_UINT64(2, pins_value(mem, (pins_at){"RD_ADDR", 0}, ABITS_32));
}

/* A word read followed by a part select (N13): the low four bits of the port's data. */
static void test_word_then_part_select(void) {
    read_clean("  reg [7:0] m [0:3];\n  assign y4 = m[ra][3:0];\n");
    odin3_node_id mem = only_node("$mem");
    TEST_ASSERT_EQUAL_INT64(4, param_of(mem, "ABITS"));
    expect_pins_are(mem, (pins_at){"RD_DATA", 0}, (bits_of){"y4", 0}, 4);
}

/* A multi-dimensional array is flattened row-major: (i - 0) * 2 + (j - 1) as Yosys builds it
 * (no $sub for a low bound of 0), OFFSET 0 and SIZE 8. */
static void test_flat_address_cells(void) {
    read_clean("  reg [7:0] m2 [0:3][1:2];\n  assign y = m2[s2][c];\n");
    odin3_node_id mem = only_node("$mem");
    expect_memid(mem, "m2");
    TEST_ASSERT_EQUAL_INT64(8, param_of(mem, "SIZE"));
    TEST_ASSERT_EQUAL_INT64(0, param_of(mem, "OFFSET"));
    TEST_ASSERT_EQUAL_INT64(ABITS_32, param_of(mem, "ABITS"));
    TEST_ASSERT_EQUAL_UINT32(1, count_of("$mul"));
    TEST_ASSERT_EQUAL_UINT32(1, count_of("$sub"));
    TEST_ASSERT_EQUAL_UINT32(1, count_of("$add"));
    odin3_node_id mul = only_node("$mul");
    TEST_ASSERT_EQUAL_INT64(2, param_of(mul, "A_WIDTH"));
    TEST_ASSERT_EQUAL_INT64(ABITS_32, param_of(mul, "B_WIDTH"));
    TEST_ASSERT_EQUAL_INT64(ABITS_32, param_of(mul, "Y_WIDTH"));
    TEST_ASSERT_EQUAL_INT64(0, param_of(mul, "A_SIGNED"));
    odin3_node_id sub = only_node("$sub");
    TEST_ASSERT_EQUAL_INT64(1, param_of(sub, "A_WIDTH"));
    TEST_ASSERT_EQUAL_INT64(ABITS_32, param_of(sub, "B_WIDTH"));
    odin3_node_id add = only_node("$add");
    for (uint32_t k = 0; k < ABITS_32; k++) {
        TEST_ASSERT_EQUAL_UINT32(pin_net(add, "Y", k).v, pin_net(mem, "RD_ADDR", k).v);
    }
}

/* A descending dimension counts from its left bound: (3 - i) * 2 + j. */
static void test_flat_address_descending(void) {
    read_clean("  reg [7:0] m3 [3:0][0:1];\n  assign y = m3[s2][c];\n");
    odin3_node_id sub = only_node("$sub");
    TEST_ASSERT_EQUAL_INT64(ABITS_32, param_of(sub, "A_WIDTH"));
    TEST_ASSERT_EQUAL_INT64(2, param_of(sub, "B_WIDTH"));
    TEST_ASSERT_EQUAL_UINT32(1, count_of("$mul"));
    TEST_ASSERT_EQUAL_UINT32(1, count_of("$add"));
}

/* Constant indices fold to a constant address: [1][2] of [0:3][1:2] is 1 * 2 + 1 = 3. */
static void test_flat_address_constant(void) {
    read_clean("  reg [7:0] m2 [0:3][1:2];\n  assign y = m2[1][2];\n");
    odin3_node_id mem = only_node("$mem");
    TEST_ASSERT_EQUAL_UINT64(3, pins_value(mem, (pins_at){"RD_ADDR", 0}, ABITS_32));
    TEST_ASSERT_EQUAL_UINT32(0, count_of("$mul"));
    TEST_ASSERT_EQUAL_UINT32(0, count_of("$add"));
}

/* A signed index: every cell of the address takes the whole expression's sign (Yosys: A_SIGNED
 * and B_SIGNED 1 on $mul, $sub and $add when every leaf is signed, the constants being signed). */
static void test_flat_address_signed(void) {
    read_clean("  reg [7:0] m2 [0:3][1:2];\n  assign y = m2[sa][$signed(c)];\n");
    odin3_node_id mul = only_node("$mul");
    TEST_ASSERT_EQUAL_INT64(1, param_of(mul, "A_SIGNED"));
    TEST_ASSERT_EQUAL_INT64(1, param_of(mul, "B_SIGNED"));
    TEST_ASSERT_EQUAL_INT64(8, param_of(mul, "A_WIDTH"));
    TEST_ASSERT_EQUAL_INT64(1, param_of(only_node("$sub"), "A_SIGNED"));
    TEST_ASSERT_EQUAL_INT64(1, param_of(only_node("$add"), "B_SIGNED"));
}

/* A later merge of the index's net into another (`assign t = ra;` after the read) leaves the
 * port on the surviving net: the address is followed through the merges. */
static void test_address_follows_merges(void) {
    read_clean("  reg [7:0] m [0:15];\n  wire [3:0] t;\n  assign y = m[t];\n  assign t = ra;\n");
    odin3_node_id mem = only_node("$mem");
    expect_pins_are(mem, (pins_at){"RD_ADDR", 0}, (bits_of){"ra", 0}, 4);
    expect_pins_are(mem, (pins_at){"RD_ADDR", 0}, (bits_of){"t", 0}, 4);
}

/* An array in a generate block is named by its scope path, and reached by a hierarchical name. */
static void test_generate_scope_arrays(void) {
    read_clean("  genvar g;\n  generate for (g = 0; g < 2; g = g + 1) begin : gb\n"
               "    reg [7:0] m [0:3];\n    wire [7:0] r;\n    assign r = m[ra];\n"
               "  end endgenerate\n  assign y = gb[0].r;\n  assign y1 = gb[1].m[ra];\n");
    TEST_ASSERT_EQUAL_UINT32(2, count_of("$mem"));
    expect_memid(nth_node("$mem", 0), "gb[0].m");
    expect_memid(nth_node("$mem", 1), "gb[1].m");
    TEST_ASSERT_EQUAL_INT64(1, param_of(nth_node("$mem", 0), "RD_PORTS"));
    TEST_ASSERT_EQUAL_INT64(2, param_of(nth_node("$mem", 1), "RD_PORTS"));
}

/* An array that is never accessed makes no cell. */
static void test_unused_memory_makes_no_cell(void) {
    read_clean("  reg [7:0] m [0:3];\n");
    TEST_ASSERT_EQUAL_UINT32(0, count_of("$mem"));
}

/* A whole array is no value. */
static void test_whole_array_read(void) {
    read_failing("  reg [7:0] m [0:3];\n  assign y = m;\n");
    expect_error((at_loc){2, 14}, "'m' is a memory; select a word");
}

/* --- registers per word --------------------------------------------------------------------- */

static void test_word_assign_and_read(void) {
    read_clean("  wire [3:0] w [0:3];\n  assign w[1] = a4;\n  assign y4 = w[1];\n");
    for (uint32_t k = 0; k < 4; k++) {
        TEST_ASSERT_EQUAL_UINT32(wnet("a4", k).v, wnet("y4", k).v);
        TEST_ASSERT_EQUAL_UINT32(wnet("a4", k).v, wnet("w[1]", k).v);
    }
}

/* A word of a multi-dimensional array: row-major position 1 * 2 + 0 names it w2[2]. */
static void test_flat_word_names(void) {
    read_clean("  wire [3:0] w2 [0:1][0:1];\n  assign w2[1][0] = a4;\n  assign y4 = w2[1][0];\n");
    TEST_ASSERT_NOT_EQUAL_UINT32(0, find_wire("w2[3]").v);
    for (uint32_t k = 0; k < 4; k++) {
        TEST_ASSERT_EQUAL_UINT32(wnet("a4", k).v, wnet("w2[2]", k).v);
    }
}

/* An assignment to a part of a word. */
static void test_word_part_select_target(void) {
    read_clean("  wire [3:0] w [0:1];\n  assign w[1][2:1] = a4[1:0];\n  assign y5 = w[1];\n");
    TEST_ASSERT_EQUAL_UINT32(wnet("a4", 0).v, wnet("w[1]", 1).v);
    TEST_ASSERT_EQUAL_UINT32(wnet("a4", 1).v, wnet("w[1]", 2).v);
}

/* A constant index past the array reads x with a warning. */
static void test_word_read_out_of_range(void) {
    read_body("  wire [3:0] w [0:3];\n  assign y4 = w[7];\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    expect_warning((at_loc){2, 15}, "word select [7] of 'w' is out of bounds; the word is x");
    for (uint32_t k = 0; k < 4; k++) {
        TEST_ASSERT_EQUAL_UINT32(const_net_of(ODIN3_BIT_X).v, wnet("y4", k).v);
    }
    expect_ir_ok();
}

/* A constant index past the array in an assignment target changes nothing. */
static void test_word_write_out_of_range(void) {
    read_body("  wire [3:0] w [0:3];\n  assign w[9] = a4;\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    expect_warning((at_loc){2, 10}, "assignment to word [9] outside array 'w' is ignored");
    expect_ir_ok();
}

/* A variable index into registers on read is a case over the words (Task 23): a `$proc` of its
 * own (the combinational read process); the full dump is test_elab_stage2_call.c's. */
static void test_word_variable_read(void) {
    read_ok("  wire [3:0] w [0:3];\n  assign y4 = w[s2];\n  assign y5 = a4;\n");
    TEST_ASSERT_EQUAL_UINT32(1, count_of("$proc"));
    TEST_ASSERT_EQUAL_UINT32(wnet("a4", 0).v, wnet("y5", 0).v);
}

static void test_word_variable_write_error(void) {
    read_failing("  wire [3:0] w [0:3];\n  assign w[s2] = a4;\n");
    expect_error((at_loc){2, 10},
                 "continuous assignment to 'w' with a non-constant index is not supported");
}

/* --- caps ----------------------------------------------------------------------------------- */

static void memory_bits_1024(odin3_elab_limits *limits) {
    limits->max_memory_bits = 1024;
}

static void set_limits(void (*lower)(odin3_elab_limits *)) {
    odin3_elab_limits limits = *odin3_elab_limits_get();
    lower(&limits);
    odin3_elab_test_set_limits(&limits);
}

/* The memory-bits cap is fatal and located at the declarator. */
static void test_memory_bits_cap(void) {
    set_limits(memory_bits_1024);
    read_failing("  reg [7:0] m [0:255];\n  assign y = m[ra];\n");
    expect_error((at_loc){1, 13}, "memory 'm' of 2048 bits exceeds the limit of 1024 bits");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, diag_errors(), et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32(0, count_of("$mem"));
}

/* An array exactly at the cap is fine. */
static void test_memory_bits_at_cap(void) {
    set_limits(memory_bits_1024);
    read_clean("  reg [7:0] m [0:127];\n  assign y = m[ra3];\n");
    TEST_ASSERT_EQUAL_UINT32(1, count_of("$mem"));
}

/* --- the write port API (used from Task 24) -------------------------------------------------- */

static odin3_sig g_wr_addr, g_wr_data, g_wr_en;
static odin3_net_id g_wr_clk;

/* A constant address (1, 3 bits wide), data of alternating 1 and x, every enable 1. */
static odin3_status make_write_signals(odin3_elab2 *e2) {
    odin3_status st = odin3_elab2_sig_new(e2, 3, &g_wr_addr);
    st = st != ODIN3_OK ? st : odin3_elab2_sig_new(e2, 8, &g_wr_data);
    st = st != ODIN3_OK ? st : odin3_elab2_sig_new(e2, 8, &g_wr_en);
    st = st != ODIN3_OK ? st : odin3_elab2_const_net(e2, ODIN3_BIT_0, &g_wr_clk);
    if (st != ODIN3_OK) {
        return st;
    }
    g_wr_addr.bits[0] = odin3_sb_of_bit(ODIN3_BIT_1);
    for (uint32_t k = 0; k < 8; k++) {
        g_wr_data.bits[k] = odin3_sb_of_bit(k % 2 == 0 ? ODIN3_BIT_1 : ODIN3_BIT_X);
        g_wr_en.bits[k] = odin3_sb_of_bit(ODIN3_BIT_1);
    }
    return ODIN3_OK;
}

/* Adds two clocked write ports (a rising and a falling edge) to memory 0. strict: every call must
 * succeed; else (an OOM sweep) it may run out of memory. */
static void write_two_ports(odin3_elab2 *e2, bool strict) {
    odin3_status st = make_write_signals(e2);
    odin3_elab2_mem_wr wr = {0, &g_wr_addr, &g_wr_data, &g_wr_en, g_wr_clk, true, {0}};
    st = st != ODIN3_OK ? st : odin3_elab2_mem_write(e2, &wr);
    wr.posedge = false;
    st = st != ODIN3_OK ? st : odin3_elab2_mem_write(e2, &wr);
    if (strict) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    } else {
        TEST_ASSERT_TRUE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY);
    }
}

static void add_write_port(void *user, odin3_elab2 *e2, odin3_ast_id module_node) {
    (void)user;
    (void)module_node;
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)e2->mems.len);
    write_two_ports(e2, true);
    /* a malformed port is refused, not made: the data is not as wide as the word */
    odin3_sig narrow = g_wr_data;
    narrow.width = 4;
    odin3_elab2_mem_wr bad = {0, &g_wr_addr, &narrow, &g_wr_en, g_wr_clk, true, {0}};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_elab2_mem_write(e2, &bad));
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)odin3_elab2_mem_get(e2, 0)->wr_ports.len);
}

/* The same without asserting, for the OOM sweep. */
static void add_write_port_sweep(void *user, odin3_elab2 *e2, odin3_ast_id module_node) {
    (void)user;
    (void)module_node;
    write_two_ports(e2, false);
}

/* Two write ports with constant addr and data: clocked, in order, polarity per port, the address
 * zero-extended to ABITS (the widest of all ports). */
static void test_write_ports(void) {
    odin3_elab_test_hooks hooks = {.after_prescan = add_write_port};
    odin3_elab_test_set_hooks(&hooks);
    read_body("  (* nomem2reg *) reg [7:0] m [0:7];\n  assign y = m[ra3];\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    et_expect(&rs, ODIN3_LOG_ERROR, "odin3_elab2_mem_write: not a memory, or a malformed port");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, diag_errors(), et_log(&rs));
    expect_ir_ok();
    odin3_node_id mem = only_node("$mem");
    TEST_ASSERT_EQUAL_INT64(2, param_of(mem, "WR_PORTS"));
    TEST_ASSERT_EQUAL_INT64(1, param_of(mem, "RD_PORTS"));
    TEST_ASSERT_EQUAL_INT64(3, param_of(mem, "ABITS"));
    TEST_ASSERT_EQUAL_STRING("11", bits_of_param(mem, "WR_CLK_ENABLE"));
    TEST_ASSERT_EQUAL_STRING("10", bits_of_param(mem, "WR_CLK_POLARITY"));
    TEST_ASSERT_EQUAL_UINT32(16, port_width(mem, "WR_EN"));
    TEST_ASSERT_EQUAL_UINT32(6, port_width(mem, "WR_ADDR"));
    TEST_ASSERT_EQUAL_UINT64(1, pins_value(mem, (pins_at){"WR_ADDR", 0}, 3));
    TEST_ASSERT_EQUAL_UINT64(1, pins_value(mem, (pins_at){"WR_ADDR", 3}, 3));
    expect_pins_const(mem, (pins_at){"WR_EN", 0}, 16, ODIN3_BIT_1);
    TEST_ASSERT_EQUAL_UINT32(const_net_of(ODIN3_BIT_X).v, pin_net(mem, "WR_DATA", 1).v);
    TEST_ASSERT_EQUAL_UINT32(const_net_of(ODIN3_BIT_1).v, pin_net(mem, "WR_DATA", 8).v);
    TEST_ASSERT_EQUAL_UINT32(const_net_of(ODIN3_BIT_0).v, pin_net(mem, "WR_CLK", 1).v);
}

/* --- RTLIL and determinism ------------------------------------------------------------------- */

static void slurp(const char *path, char *out, size_t cap) {
    FILE *file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(file, path);
    size_t got = fread(out, 1, cap - 1, file);
    out[got] = '\0';
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static const char k_rich[] =
    "  reg [7:0] m [1:4];\n  reg [3:0] n [-2:1];\n  reg [7:0] m2 [0:3][1:2];\n"
    "  wire [3:0] w [0:2];\n  assign w[1] = a4;\n  assign y = m[ra];\n  assign y4 = n[ra];\n"
    "  assign y1 = m2[s2][c];\n  assign y5 = w[1];\n";

static void rtlil_of_rich(char *out, size_t cap) {
    read_body(k_rich);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_pass_run(rs.design, "write_rtlil", odin3_bytes_cstr("rich.il")));
    slurp("rich.il", out, cap);
    TEST_ASSERT_EQUAL_INT(0, remove("rich.il"));
}

/* Two runs write the same RTLIL; the $mem cells are in it with their raw-index ports. */
static void test_deterministic(void) {
    static char first[TEXT_MAX];
    static char second[TEXT_MAX];
    rtlil_of_rich(first, sizeof first);
    et_free(&rs);
    memset(&rs, 0, sizeof rs);
    rtlil_of_rich(second, sizeof second);
    TEST_ASSERT_NOT_NULL(strstr(first, "cell $mem"));
    TEST_ASSERT_NOT_NULL(strstr(first, "parameter \\OFFSET -2"));
    TEST_ASSERT_EQUAL_STRING(first, second);
}

/* --- out of memory --------------------------------------------------------------------------- */

static const char k_sweep_write[] =
    "module top (input [2:0] ra3, output [7:0] y);\n"
    "  (* nomem2reg *) reg [7:0] m [0:7];\n  assign y = m[ra3];\nendmodule\n";

static const char k_sweep[] =
    "module top (input [3:0] ra, input [1:0] s2, input c, input [3:0] a4,\n"
    "            output [7:0] y, output [7:0] y1, output [3:0] y4);\n"
    "  reg [7:0] m [1:4];\n  reg [7:0] m2 [0:3][1:2];\n  wire [3:0] w [0:2];\n"
    "  reg [3:0] m3 [0:1];\n  assign m3[1] = a4;\n"
    "  assign w[1] = a4;\n  assign y = m[ra];\n  assign y1 = m2[s2][c];\n  assign y4 = w[1];\n"
    "endmodule\n";

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

/* Every allocation of the read of `text` fails in turn: NO_MEMORY, the IR check passes, no leak
 * (ASan), until the read completes OOM_SETTLE runs in a row. */
static void oom_sweep(const char *text, uint32_t min_failures) {
    et_src src = {"t.v", text};
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
    TEST_ASSERT_GREATER_THAN_UINT32(min_failures, failures);
}

static void test_oom_sweep(void) {
    oom_sweep(k_sweep, 300);
}

/* The write port API under allocation failures (the hook adds the ports, or fails with them). */
static void test_oom_sweep_write_ports(void) {
    odin3_elab_test_hooks hooks = {.after_prescan = add_write_port_sweep};
    odin3_elab_test_set_hooks(&hooks);
    oom_sweep(k_sweep_write, 100);
}

#define ALL_TESTS(X)                                                                               \
    X(test_rule1_net_array_words)                                                                  \
    X(test_rule1_declared_index_names)                                                             \
    X(test_rule1_mem2reg_attribute)                                                                \
    X(test_rule1_mem2reg_module_attribute)                                                         \
    X(test_read_only_reg_array_is_memory)                                                          \
    X(test_rule2_always_star)                                                                      \
    X(test_rule2_async_reset)                                                                      \
    X(test_rule2_single_edge_stays_memory)                                                         \
    X(test_rule3_read_after_blocking_write)                                                        \
    X(test_rule3_other_block_stays_memory)                                                         \
    X(test_rule4_nested_index)                                                                     \
    X(test_rule4_concat_lvalue)                                                                    \
    X(test_rule5_constant_writes_only)                                                             \
    X(test_rule5_unrolled_loop)                                                                    \
    X(test_rule5_loop_with_variable_bound)                                                         \
    X(test_rule5_variable_write_keeps_memory)                                                      \
    X(test_initial_writes_are_not_rule5)                                                           \
    X(test_initial_context_ends)                                                                   \
    X(test_rule5_continuous_constant_write)                                                        \
    X(test_nomem2reg_attribute_exempts)                                                            \
    X(test_nomem2reg_attribute_keeps_rule1)                                                        \
    X(test_nomem2reg_option_net_array_assign)                                                      \
    X(test_nomem2reg_option_net_array_is_memory)                                                   \
    X(test_nomem2reg_option_async_write)                                                           \
    X(test_nomem2reg_option_skips_rules)                                                           \
    X(test_read_port_cell)                                                                         \
    X(test_offsets)                                                                                \
    X(test_two_read_ports)                                                                         \
    X(test_constant_index_read)                                                                    \
    X(test_word_then_part_select)                                                                  \
    X(test_flat_address_cells)                                                                     \
    X(test_flat_address_descending)                                                                \
    X(test_flat_address_constant)                                                                  \
    X(test_flat_address_signed)                                                                    \
    X(test_address_follows_merges)                                                                 \
    X(test_generate_scope_arrays)                                                                  \
    X(test_unused_memory_makes_no_cell)                                                            \
    X(test_whole_array_read)                                                                       \
    X(test_word_assign_and_read)                                                                   \
    X(test_flat_word_names)                                                                        \
    X(test_word_part_select_target)                                                                \
    X(test_word_read_out_of_range)                                                                 \
    X(test_word_write_out_of_range)                                                                \
    X(test_word_variable_read)                                                                     \
    X(test_word_variable_write_error)                                                              \
    X(test_memory_bits_cap)                                                                        \
    X(test_memory_bits_at_cap)                                                                     \
    X(test_write_ports)                                                                            \
    X(test_deterministic)                                                                          \
    X(test_oom_sweep)                                                                              \
    X(test_oom_sweep_write_ports)

#define RUN_ONE(name) RUN_TEST(name);

int main(void) {
    UNITY_BEGIN();
    ALL_TESTS(RUN_ONE)
    return UNITY_END();
}
