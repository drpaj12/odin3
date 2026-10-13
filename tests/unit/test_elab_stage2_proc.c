/*
 * test_elab_stage2_proc.c — 2D Task 21: the process generator core (EL-33 emission, EL-34 syncs,
 * EL-35 steps 1-7). Each `$proc` body is asserted exactly through a text dump of the node: the pins
 * as I<k> / O<k> in net-ID order, every case, action and sync in IR-19's canonical layout. The
 * whole read_verilog pass runs through the pass manager (elab_test.h); the IR is read back through
 * the module API and every located diagnostic is asserted as the whole line.
 *
 * Yosys 0.55 (git sha1 60f126cd0) is the oracle; the commands behind the expected values are in
 * the task report:
 *   yosys -q -p "read_verilog t.v; write_rtlil"                (the process text, the chunks)
 *   yosys -q -p "read_verilog t.v; proc; write_rtlil"          (the lowered registers)
 *   yosys -p "read_verilog w.v"                                (the net-in-process warning)
 */
#include "elab_test.h"
/* elab_test_tree.h (hand-built stores) defines its own et_free: rename it for this file. */
#define et_free et_tree_free
#include "elab_test_tree.h"
#undef et_free

#include "ast/ast.h"
#include "ast/kinds.h"
#include "frontends/elab/elab.h"
#include "frontends/elab/elab_internal.h"
#include "frontends/elab/elab_test.h"
#include "frontends/elab/pg_internal.h"
#include "ir/celltype.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/module.h"
#include "ir/proc.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <fcntl.h>
#include <spawn.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

enum { OOM_LIMIT = 400000, OOM_SETTLE = 32, TEXT_MAX = 1 << 16, DUMP_MAX = 1 << 13 };

extern char **environ;

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

/* The pass's arguments and the text of t.v. */
typedef struct pass_src {
    const char *args;
    const char *text;
} pass_src;

static void read_with(const pass_src *src) {
    et_src file = {"t.v", src->text};
    et_req req = {&file, 1, NULL, NULL, NULL};
    et_setup(&req, &rs);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_set_keep_ast(rs.design, ODIN3_KEEP_AST_NONE));
    rs.status = odin3_pass_run(rs.design, "read_verilog", odin3_bytes_cstr(src->args));
}

static void read_src(const char *text) {
    pass_src src = {"t.v", text};
    read_with(&src);
}

static void expect_ir_ok(void) {
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_check_design(rs.design, opts), et_log(&rs));
}

/* Errors that are located diagnostics: not the pass manager's own "failed" line. */
static uint32_t diag_errors(void) {
    return rs.errors - et_count_with(&rs, ODIN3_LOG_ERROR, "pass read_verilog: failed");
}

/* The warnings of 2D: not the IR check's. */
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

/* A failed stage 2 leaves valid, incomplete modules (EL-2). */
static void read_failing(const char *text) {
    read_src(text);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_ERR_PARSE, rs.status, et_log(&rs));
    expect_ir_ok();
}

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

static const char *str_of(uint32_t id) {
    return odin3_strtab_get(odin3_design_strtab(rs.design), id);
}

static odin3_module *top(void) {
    uint32_t id = id_of("t");
    for (uint32_t index = 1; index < odin3_design_module_end(rs.design); index++) {
        odin3_module *module = odin3_module_get(rs.design, (odin3_module_id){index});
        if (odin3_module_name(module) == id) {
            return module;
        }
    }
    TEST_FAIL_MESSAGE("no module t");
    return NULL;
}

static odin3_net_id wnet(const char *name, uint32_t bit) {
    odin3_wire_id wire = odin3_module_find_wire(top(), id_of(name));
    TEST_ASSERT_NOT_EQUAL_UINT32_MESSAGE(0, wire.v, name);
    odin3_net_id net = odin3_wire_net(top(), wire, bit);
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

static odin3_node_id proc_node(uint32_t nth) {
    odin3_node_id node = nth_node("$proc", nth);
    TEST_ASSERT_NOT_EQUAL_UINT32(0, node.v);
    return node;
}

/* The net on pin `index` of a port of a $proc node. */
static odin3_net_id slice_net(odin3_pinslice slice, uint32_t index) {
    TEST_ASSERT_TRUE(index < slice.count);
    return odin3_pin_net(top(), (odin3_pin_id){slice.first.v + index});
}

static odin3_net_id in_net(odin3_node_id node, uint32_t index) {
    return slice_net(odin3_node_port(top(), node, ODIN3_PROC_PORT_I), index);
}

static odin3_net_id out_net(odin3_node_id node, uint32_t index) {
    return slice_net(odin3_node_port(top(), node, ODIN3_PROC_PORT_O), index);
}

static const odin3_proc_body *body_of(odin3_node_id node, odin3_proc_body *storage) {
    const odin3_value *value = odin3_node_param(top(), node, ODIN3_PROC_P_BODY);
    TEST_ASSERT_NOT_NULL(value);
    TEST_ASSERT_TRUE(odin3_proc_body_view(value, storage));
    return storage;
}

/* --- the dump of a body: pins in net-ID order, cases, switches and syncs ---------------------- */

static void put_pbit(odin3_strbuf *out, odin3_pbit pbit) {
    if (pbit == ODIN3_PBIT_ANY) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, odin3_bytes_cstr("-")));
        return;
    }
    char tag = (pbit & ~ODIN3_PBIT_INDEX_MASK) == ODIN3_PBIT_OUT ? 'O' : 'I';
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_strbuf_appendf(out, "%c%u", tag, (unsigned)(pbit & ODIN3_PBIT_INDEX_MASK)));
}

/* A span as a list: consecutive pins of one port as a range (O8..O15). */
static void put_span(odin3_strbuf *out, const odin3_proc_body *body, odin3_pspan span) {
    for (uint32_t i = 0; i < span.len;) {
        odin3_pbit first = body->pbits[span.first + i];
        uint32_t j = i;
        while (first != ODIN3_PBIT_ANY && j + 1 < span.len &&
               body->pbits[span.first + j + 1] == first + (j + 1 - i)) {
            j++;
        }
        if (i > 0) {
            TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, odin3_bytes_cstr(",")));
        }
        put_pbit(out, first);
        if (j > i) {
            TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, odin3_bytes_cstr("..")));
            put_pbit(out, body->pbits[span.first + j]);
        }
        i = j + 1;
    }
}

static void put_actions(odin3_strbuf *out, const odin3_proc_body *body, uint32_t first,
                        uint32_t n) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, odin3_bytes_cstr("[")));
    for (uint32_t i = first; i < first + n; i++) {
        if (i > first) {
            TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, odin3_bytes_cstr(" ")));
        }
        put_span(out, body, body->actions[i].lhs);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, odin3_bytes_cstr("<-")));
        put_span(out, body, body->actions[i].rhs);
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, odin3_bytes_cstr("]")));
}

static void put_case(odin3_strbuf *out, const odin3_proc_body *body, uint32_t index) {
    const odin3_proc_case *cs = &body->cases[index];
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_appendf(out, "C%u", (unsigned)index));
    if (cs->n_compares > 0) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, odin3_bytes_cstr(" cmp(")));
        for (uint32_t k = 0; k < cs->n_compares; k++) {
            if (k > 0) {
                TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, odin3_bytes_cstr("|")));
            }
            put_span(out, body, body->compares[cs->first_compare + k]);
        }
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, odin3_bytes_cstr(")")));
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, odin3_bytes_cstr(" ")));
    put_actions(out, body, cs->first_action, cs->n_actions);
    if (cs->n_switches > 0) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_strbuf_appendf(out, " sw%u+%u", (unsigned)cs->first_switch,
                                                   (unsigned)cs->n_switches));
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, odin3_bytes_cstr("\n")));
}

static void put_sync(odin3_strbuf *out, const odin3_proc_body *body, uint32_t index) {
    static const char *const k_names[] = {"?", "posedge", "negedge", "low", "high", "always"};
    const odin3_proc_sync *sync = &body->syncs[index];
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_appendf(out, "S %s", k_names[sync->type]));
    if (sync->type != ODIN3_SYNC_ALWAYS) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, odin3_bytes_cstr(" ")));
        put_pbit(out, sync->signal);
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, odin3_bytes_cstr(" ")));
    put_actions(out, body, sync->first_action, sync->n_actions);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, odin3_bytes_cstr("\n")));
}

/* "I<w> O<w>", a line per case, switch and sync. */
static const char *dump(odin3_node_id node) {
    static char text[DUMP_MAX];
    odin3_proc_body storage;
    const odin3_proc_body *body = body_of(node, &storage);
    odin3_strbuf out;
    odin3_strbuf_init(&out);
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK,
        odin3_strbuf_appendf(&out, "I%u O%u\n",
                             (unsigned)odin3_node_port(top(), node, ODIN3_PROC_PORT_I).count,
                             (unsigned)odin3_node_port(top(), node, ODIN3_PROC_PORT_O).count));
    for (uint32_t i = 0; i < body->n_cases; i++) {
        put_case(&out, body, i);
    }
    for (uint32_t j = 0; j < body->n_switches; j++) {
        const odin3_proc_switch *sw = &body->switches[j];
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_appendf(&out, "W%u (", (unsigned)j));
        put_span(&out, body, sw->signal);
        TEST_ASSERT_EQUAL_INT(
            ODIN3_OK, odin3_strbuf_appendf(&out, ") f%u C%u+%u\n", (unsigned)sw->flags,
                                           (unsigned)sw->first_case, (unsigned)sw->n_cases));
    }
    for (uint32_t k = 0; k < body->n_syncs; k++) {
        put_sync(&out, body, k);
    }
    TEST_ASSERT_TRUE(out.len < sizeof text);
    memcpy(text, out.data, out.len);
    text[out.len] = '\0';
    odin3_strbuf_free(&out);
    return text;
}

static void expect_dump(uint32_t nth, const char *want) {
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want, dump(proc_node(nth)), et_log(&rs));
}

static void expect_in(odin3_node_id node, uint32_t index, odin3_net_id want) {
    TEST_ASSERT_EQUAL_UINT32(want.v, in_net(node, index).v);
}

static void expect_out(odin3_node_id node, uint32_t index, odin3_net_id want) {
    TEST_ASSERT_EQUAL_UINT32(want.v, out_net(node, index).v);
}

/* The constant net of bit (the shared $_CONST*_ node's Y). */
static odin3_net_id const_net_of(odin3_bit bit) {
    static const char *const k_names[4] = {"$_CONST0_", "$_CONST1_", "$_CONSTX_", "$_CONSTZ_"};
    odin3_node_id node = nth_node(k_names[bit], 0);
    TEST_ASSERT_NOT_EQUAL_UINT32(0, node.v);
    odin3_pinslice slice = odin3_node_port(top(), node, 0);
    return odin3_pin_net(top(), slice.first);
}

/* --- the cases of the brief: T7, T8, held values, defaults ------------------------------------ */

/* T7 (Review Focus 3): a blocking temporary read by the next statement. */
static const char k_t7[] = "module t(input clk, a, b, output reg r);\n  reg t;\n"
                           "  always @(posedge clk) begin t = a & b; r <= t; end\nendmodule\n";

static void test_t7_blocking_then_nonblocking(void) {
    read_clean(k_t7);
    TEST_ASSERT_EQUAL_UINT32(1, count_of("$proc"));
    odin3_node_id proc = proc_node(0);
    expect_dump(0, "I2 O4\nC0 [O3<-I1 O2<-I1]\nS posedge I0 [O0<-O2 O1<-O3]\n");
    expect_in(proc, 0, wnet("clk", 0));
    TEST_ASSERT_EQUAL_UINT32(1, count_of("$and"));
    odin3_node_id and_cell = nth_node("$and", 0);
    odin3_pinslice y_pins = odin3_node_port(top(), and_cell, 2);
    expect_in(proc, 1, odin3_pin_net(top(), y_pins.first));
    expect_out(proc, 0, wnet("r", 0));
    expect_out(proc, 1, wnet("t", 0));
    TEST_ASSERT_TRUE(out_net(proc, 2).v > wnet("t", 0).v &&
                     out_net(proc, 3).v > out_net(proc, 2).v);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_module_proc_count(top()));
}

/* T8: a non-blocking assignment's value is the one at the block's start. */
static void test_t8_nonblocking_reads_held_value(void) {
    read_clean("module t(input clk, a, output reg r1, r2);\n"
               "  always @(posedge clk) begin r1 <= a; r2 <= r1; end\nendmodule\n");
    odin3_node_id proc = proc_node(0);
    expect_dump(0, "I2 O4\nC0 [O2<-I1 O3<-O0]\nS posedge I0 [O0<-O2 O1<-O3]\n");
    expect_in(proc, 0, wnet("clk", 0));
    expect_in(proc, 1, wnet("a", 0));
    expect_out(proc, 0, wnet("r1", 0));
    expect_out(proc, 1, wnet("r2", 0));
}

/* The held value: `q <= q + 1` reads the register (the $add reads q's net, the process holds
 * the sum), and the sync updates q from its temporary. */
static void test_held_value(void) {
    read_clean("module t(input clk, output reg q);\n"
               "  always @(posedge clk) q <= q + 1;\nendmodule\n");
    odin3_node_id proc = proc_node(0);
    expect_dump(0, "I2 O2\nC0 [O1<-I1]\nS posedge I0 [O0<-O1]\n");
    odin3_node_id add = nth_node("$add", 0);
    TEST_ASSERT_NOT_EQUAL_UINT32(0, add.v);
    odin3_pinslice a_in = odin3_node_port(top(), add, 0);
    TEST_ASSERT_EQUAL_UINT32(wnet("q", 0).v, odin3_pin_net(top(), a_in.first).v);
    odin3_pinslice y_out = odin3_node_port(top(), add, 2);
    expect_in(proc, 1, odin3_pin_net(top(), y_out.first));
}

/* Combinational default mode: one ALWAYS sync, the root default replaced by the assignment. */
static void test_combinational_default_mode(void) {
    read_clean("module t(input a, output reg y);\n  always @* y = a;\nendmodule\n");
    expect_dump(0, "I1 O2\nC0 [O1<-I0]\nS always [O0<-O1]\n");
}

/* --nolatches: no root action; an output reads x until it is assigned (EL-35 step 3). */
static void test_nolatches_reads_x(void) {
    pass_src nolatches = {"--nolatches t.v", "module t(input a, output reg y, z);\n"
                                             "  always @* begin z = y; y = a; end\nendmodule\n"};
    read_with(&nolatches);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    expect_ir_ok();
    odin3_node_id proc = proc_node(0);
    expect_dump(0, "I2 O4\nC0 [O3<-I1 O2<-I0]\nS always [O0<-O2 O1<-O3]\n");
    expect_in(proc, 0, wnet("a", 0));
    expect_in(proc, 1, const_net_of(ODIN3_BIT_X));
    et_free(&rs);
    memset(&rs, 0, sizeof rs);
    read_clean("module t(input a, output reg y, z);\n"
               "  always @* begin z = y; y = a; end\nendmodule\n");
    expect_dump(0, "I1 O4\nC0 [O3<-O0 O2<-I0]\nS always [O0<-O2 O1<-O3]\n");
}

/* --nolatches keeps the root defaults of a block with an edge. */
static void test_nolatches_keeps_clocked_defaults(void) {
    pass_src clocked = {"--nolatches t.v", "module t(input c, a, output reg y, z);\n"
                                           "  always @(posedge c) y <= a;\nendmodule\n"};
    read_with(&clocked);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    expect_dump(0, "I2 O2\nC0 [O1<-I1]\nS posedge I0 [O0<-O1]\n");
}

/* --- chunks (v4.2) --------------------------------------------------------------------------- */

static void test_chunks_concat_lvalue(void) {
    read_clean("module t(input c, x, y, output reg a, b);\n"
               "  always @(posedge c) {a, b} <= {x, y};\nendmodule\n");
    odin3_node_id proc = proc_node(0);
    /* two sync actions and two t0 groups, one per variable; the user assignment is split in
     * lvalue order: t0(a) <- x first, then t0(b) <- y */
    expect_dump(0, "I3 O4\nC0 [O2<-I1 O3<-I2]\nS posedge I0 [O0<-O2 O1<-O3]\n");
    expect_out(proc, 0, wnet("a", 0));
    expect_out(proc, 1, wnet("b", 0));
}

static void test_chunks_reversed_concat(void) {
    read_clean("module t(input c, x, y, output reg a, b);\n"
               "  always @(posedge c) {b, a} <= {x, y};\nendmodule\n");
    /* sorted: a first (root and sync); the assignment in lvalue order: t0(b) <- x, t0(a) <- y */
    expect_dump(0, "I3 O4\nC0 [O3<-I1 O2<-I2]\nS posedge I0 [O0<-O2 O1<-O3]\n");
}

static void test_chunks_vector_is_one_action(void) {
    read_clean("module t(input c, input [7:0] d8, output reg [7:0] q);\n"
               "  always @(posedge c) q <= d8;\nendmodule\n");
    expect_dump(0, "I9 O16\nC0 [O8..O15<-I1..I8]\nS posedge I0 [O0..O7<-O8..O15]\n");
}

static void test_chunks_partial_write(void) {
    read_clean("module t(input c, input [3:0] d4, output reg [7:0] q);\n"
               "  always @(posedge c) q[3:0] <= d4;\nendmodule\n");
    odin3_node_id proc = proc_node(0);
    /* only the chunk q[3:0]: four outputs, four temporaries */
    expect_dump(0, "I5 O8\nC0 [O4..O7<-I1..I4]\nS posedge I0 [O0..O3<-O4..O7]\n");
    for (uint32_t k = 0; k < 4; k++) {
        expect_out(proc, k, wnet("q", k));
    }
}

static void test_chunks_split_at_the_chunk_boundary(void) {
    /* q[1] and q[3] are written: two chunks of one bit each (a gap in the bit positions) */
    read_clean("module t(input c, x, y, output reg [3:0] q);\n"
               "  always @(posedge c) begin q[1] <= x; q[3] <= y; end\nendmodule\n");
    expect_dump(0, "I3 O4\nC0 [O2<-I1 O3<-I2]\nS posedge I0 [O0<-O2 O1<-O3]\n");
}

static void test_chunks_overwrite_one_run(void) {
    /* a vector written whole after a bit: the earlier action is removed, one 4-bit action stays */
    read_clean("module t(input c, input [3:0] d, input x, output reg [3:0] q);\n"
               "  always @(posedge c) begin q[0] <= x; q <= d; end\nendmodule\n");
    expect_dump(0, "I5 O8\nC0 [O4..O7<-I1..I4]\nS posedge I0 [O0..O3<-O4..O7]\n");
}

static void test_partial_overwrite_keeps_the_rest(void) {
    read_clean("module t(input c, input [3:0] d, input x, output reg [3:0] q);\n"
               "  always @(posedge c) begin q <= d; q[1] <= x; end\nendmodule\n");
    /* the first action keeps its bits but q[1] (d[1] is no longer read: not an I pin) */
    expect_dump(0, "I5 O8\nC0 [O4,O6..O7<-I1..I3 O5<-I4]\nS posedge I0 [O0..O3<-O4..O7]\n");
}

/* --- constants ------------------------------------------------------------------------------- */

static void test_constant_pin_in_net_order(void) {
    /* the constant 0 net exists (assign) before the $xor's Y, so it sits before it among the
     * I pins; the second constant 0 of the block reuses its pin */
    read_clean("module t(input c, a, b, output reg r, s, output z);\n"
               "  assign z = 1'b0;\n"
               "  always @(posedge c) begin r <= 1'b0; s <= a ^ b; end\nendmodule\n");
    odin3_node_id proc = proc_node(0);
    expect_dump(0, "I3 O4\nC0 [O2<-I1 O3<-I2]\nS posedge I0 [O0<-O2 O1<-O3]\n");
    expect_in(proc, 1, const_net_of(ODIN3_BIT_0));
    odin3_node_id xor_cell = nth_node("$xor", 0);
    odin3_pinslice y_pins = odin3_node_port(top(), xor_cell, 2);
    expect_in(proc, 2, odin3_pin_net(top(), y_pins.first));
}

static void test_constant_shared_pin(void) {
    read_clean("module t(input c, output reg r, s);\n"
               "  always @(posedge c) begin r <= 1'b0; s <= 1'b0; end\nendmodule\n");
    odin3_node_id proc = proc_node(0);
    expect_dump(0, "I2 O4\nC0 [O2<-I1 O3<-I1]\nS posedge I0 [O0<-O2 O1<-O3]\n");
    expect_in(proc, 1, const_net_of(ODIN3_BIT_0));
    TEST_ASSERT_EQUAL_UINT32(1, count_of("$_CONST0_"));
}

/* --- event lists (EL-34) ---------------------------------------------------------------------- */

static void test_two_edges(void) {
    read_clean("module t(input a, b, d, output reg q);\n"
               "  always @(posedge a or negedge b) q <= d;\nendmodule\n");
    odin3_node_id proc = proc_node(0);
    expect_dump(0, "I3 O2\nC0 [O1<-I2]\nS posedge I0 [O0<-O1]\nS negedge I1 [O0<-O1]\n");
    expect_in(proc, 0, wnet("a", 0));
    expect_in(proc, 1, wnet("b", 0));
}

static void test_level_lists_are_one_always(void) {
    static const char *const k_events[] = {"@(a or b)", "@(a, b)", "@*", "@(*)"};
    for (size_t i = 0; i < sizeof k_events / sizeof k_events[0]; i++) {
        char text[256];
        TEST_ASSERT_TRUE(snprintf(text, sizeof text,
                                  "module t(input a, b, output reg y);\n  always %s y = a & b;\n"
                                  "endmodule\n",
                                  k_events[i]) > 0);
        read_clean(text);
        expect_dump(0, "I1 O2\nC0 [O1<-I0]\nS always [O0<-O1]\n");
        et_free(&rs);
        memset(&rs, 0, sizeof rs);
    }
    read_clean("module t(input a, output reg y);\n  always @a y = ~a;\nendmodule\n");
    expect_dump(0, "I1 O2\nC0 [O1<-I0]\nS always [O0<-O1]\n");
}

static void test_edge_on_wide_signal(void) {
    read_failing("module t(input [3:0] w4, input d, output reg q);\n"
                 "  always @(posedge w4) q <= d;\nendmodule\n");
    expect_error((at_loc){2, 12}, "posedge on 'w4', which is 4 bits; a 1-bit signal is needed");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, diag_errors(), et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32(0, count_of("$proc"));
    et_free(&rs);
    memset(&rs, 0, sizeof rs);
    read_failing("module t(input [3:0] w4, input d, output reg q);\n"
                 "  always @(negedge w4[2:0]) q <= d;\nendmodule\n");
    expect_error((at_loc){2, 12}, "negedge on 'w4', which is 3 bits; a 1-bit signal is needed");
}

static void test_edge_on_expression(void) {
    read_clean("module t(input a, b, d, output reg q);\n"
               "  always @(posedge a & b) q <= d;\nendmodule\n");
    odin3_node_id proc = proc_node(0);
    odin3_node_id and_cell = nth_node("$and", 0);
    odin3_pinslice y_pins = odin3_node_port(top(), and_cell, 2);
    TEST_ASSERT_EQUAL_UINT32(1, count_of("$and"));
    expect_dump(0, "I2 O2\nC0 [O1<-I0]\nS posedge I1 [O0<-O1]\n");
    expect_in(proc, 1, odin3_pin_net(top(), y_pins.first));
}

/* T3: an incomplete level list warns once, naming the first missing signal by name. */
static void test_incomplete_sensitivity_list(void) {
    read_src("module t(input a, b, c, output reg y);\n  always @(a) y = a & b & c;\nendmodule\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    expect_warning((at_loc){2, 3},
                   "incomplete sensitivity list: 'b' is read but not listed; the block is built "
                   "as @*");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, warnings_with("incomplete sensitivity list"), et_log(&rs));
    expect_dump(0, "I1 O2\nC0 [O1<-I0]\nS always [O0<-O1]\n");
    et_free(&rs);
    memset(&rs, 0, sizeof rs);
    /* @* and a complete list never warn; a value the block assigned first is no read */
    read_clean("module t(input a, b, output reg y, z, w);\n"
               "  always @(a or b) begin z = a; y = z & b; end\n"
               "  always @* w = a;\nendmodule\n");
}

/* Point (h): an edge on a net the block itself assigns. */
static void test_edge_on_own_output(void) {
    read_clean("module t(output reg a);\n  always @(posedge a) a <= ~a;\nendmodule\n");
    expect_dump(0, "I1 O2\nC0 [O1<-I0]\nS posedge O0 [O0<-O1]\n");
}

/* --- diagnostics, attributes, provenance ----------------------------------------------------- */

static void test_net_in_process_warning(void) {
    read_src("module t(input a, output y);\n  wire w;\n  always @* w = a;\n  assign y = w;\n"
             "endmodule\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    expect_warning((at_loc){3, 13},
                   "'w' is a net assigned in an always block; treated as a variable");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, warnings_with("is a net assigned in an always block"),
                                     et_log(&rs));
    expect_dump(0, "I1 O2\nC0 [O1<-I0]\nS always [O0<-O1]\n");
    expect_ir_ok();
}

static void test_attributes_and_provenance(void) {
    read_clean("module t(input a, output reg y);\n  (* keep *) always @* y = a;\nendmodule\n");
    odin3_node_id proc = proc_node(0);
    const odin3_value *keep =
        odin3_attr_get(top(), (odin3_objref){ODIN3_OBJ_NODE, proc.v}, id_of("keep"));
    TEST_ASSERT_NOT_NULL(keep);
    TEST_ASSERT_EQUAL_INT(ODIN3_VAL_INT, keep->kind);
    TEST_ASSERT_EQUAL_INT64(1, keep->i);
    /* the node's SOURCE record is its ALWAYS (the attribute starts the construct's span) */
    const odin3_prov_record *rec = odin3_prov_get(rs.design, odin3_node_prov(top(), proc));
    TEST_ASSERT_NOT_NULL(rec);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_SOURCE, rec->kind);
    TEST_ASSERT_EQUAL_UINT32(2, rec->locs[0].line);
    TEST_ASSERT_EQUAL_STRING("t", str_of(rec->hier));
    /* the root action records its statement `y = a` */
    odin3_proc_body storage;
    const odin3_proc_body *body = body_of(proc, &storage);
    const odin3_prov_record *act = odin3_prov_get(rs.design, body->actions[0].prov);
    TEST_ASSERT_NOT_NULL(act);
    TEST_ASSERT_EQUAL_UINT32(2, act->locs[0].line);
    TEST_ASSERT_EQUAL_UINT32(24, act->locs[0].col);
}

/* --- no node, other bits, pending ------------------------------------------------------------ */

static void test_empty_block_makes_no_node(void) {
    read_clean("module t(input a, output y);\n  assign y = a;\n  always @* ;\n"
               "  always @* begin end\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32(0, count_of("$proc"));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_module_proc_count(top()));
}

static void test_different_bits_two_nodes(void) {
    read_clean("module t(input c, x, y, output reg [1:0] r);\n"
               "  always @(posedge c) r[0] <= x;\n  always @(posedge c) r[1] <= y;\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32(2, count_of("$proc"));
    expect_dump(0, "I2 O2\nC0 [O1<-I1]\nS posedge I0 [O0<-O1]\n");
    expect_dump(1, "I2 O2\nC0 [O1<-I1]\nS posedge I0 [O0<-O1]\n");
    TEST_ASSERT_EQUAL_UINT32(wnet("r", 0).v, out_net(proc_node(0), 0).v);
    TEST_ASSERT_EQUAL_UINT32(wnet("r", 1).v, out_net(proc_node(1), 0).v);
}

static void test_pending_if(void) {
    read_failing("module t(input c, s, a, output reg y);\n"
                 "  always @* begin if (s) y = a; end\nendmodule\n");
    expect_error((at_loc){2, 19}, "elaboration is not available yet (2D): 'IF'");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, diag_errors(), et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32(0, count_of("$proc"));
}

static void test_pending_variable_index_target(void) {
    read_failing("module t(input c, input [1:0] s, input d, output reg [3:0] y);\n"
                 "  always @(posedge c) y[s] <= d;\nendmodule\n");
    expect_error((at_loc){2, 23},
                 "elaboration is not available yet (2D): 'variable index in a procedural "
                 "assignment target'");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, diag_errors(), et_log(&rs));
}

/* A word of an array that stayed a memory: the memory write is pending (Task 24), not dropped. */
static void test_pending_memory_write(void) {
    read_failing("module t(input c, input [1:0] i, input [7:0] d, output [7:0] y);\n"
                 "  reg [7:0] m [0:3];\n  assign y = m[i];\n"
                 "  always @(posedge c) m[i] <= d;\nendmodule\n");
    expect_error((at_loc){4, 23}, "elaboration is not available yet (2D): 'memory write'");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, diag_errors(), et_log(&rs));
}

/* --- drivers: processes beside continuous drivers and each other (carried from Tasks 19, 20) -- */

static void test_process_after_assign(void) {
    read_failing(
        "module t(input a, b, output reg o);\n  assign o = a;\n  always @* o = b;\nendmodule\n");
    expect_error((at_loc){3, 13}, "'o' has two drivers (also driven at t.v:2:10)");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, diag_errors(), et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32(0, count_of("$proc")); /* the conflicting actions were dropped */
}

static void test_two_processes_one_bit(void) {
    read_failing("module t(input c, a, b, output reg r);\n"
                 "  always @(posedge c) r <= a;\n  always @(posedge c) r <= b;\nendmodule\n");
    expect_error((at_loc){3, 23}, "'r' is assigned in two always blocks (also at t.v:2:23)");
    TEST_ASSERT_EQUAL_UINT32(1, count_of("$proc"));
}

/* T10 per bit: the bit named in the message, the other bits of the variable are no conflict. */
static void test_two_processes_one_bit_of_a_vector(void) {
    read_failing("module t(input c, a, b, output reg [3:0] r);\n"
                 "  always @(posedge c) r[2] <= a;\n  always @(posedge c) r[2:1] <= {b, a};\n"
                 "endmodule\n");
    expect_error((at_loc){3, 23}, "'r[2]' is assigned in two always blocks (also at t.v:2:23)");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, diag_errors(), et_log(&rs));
}

/* T9: `=` and `<=` on one bit, in one block or in two. */
static void test_blocking_and_nonblocking(void) {
    read_failing("module t(input c, a, b, output reg r);\n"
                 "  always @(posedge c) begin\n    r = a;\n    r <= b;\n  end\nendmodule\n");
    expect_error((at_loc){4, 5}, "'r' is assigned with both '=' and '<=' (also at t.v:3:5)");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, diag_errors(), et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32(0, count_of("$proc"));
    et_free(&rs);
    memset(&rs, 0, sizeof rs);
    read_failing("module t(input c, a, b, output reg [1:0] r);\n"
                 "  always @(posedge c) r[0] <= a;\n  always @* r[1] = b;\n"
                 "  always @(posedge c) r[1] <= a;\nendmodule\n");
    expect_error((at_loc){4, 23}, "'r' is assigned with both '=' and '<=' (also at t.v:3:13)");
}

/* Different variables, or the same kind everywhere, are no conflict. */
static void test_same_kind_is_fine(void) {
    read_clean("module t(input c, a, b, output reg p, q);\n"
               "  always @(posedge c) begin p = a; q <= p; p = b; end\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32(1, count_of("$proc"));
}

static void test_conflict_drops_only_the_conflicting_chunk(void) {
    read_failing("module t(input c, a, b, output reg p, q);\n  assign p = a;\n"
                 "  always @(posedge c) begin p <= b; q <= b; end\nendmodule\n");
    expect_error((at_loc){3, 29}, "'p' has two drivers (also driven at t.v:2:10)");
    TEST_ASSERT_EQUAL_UINT32(1, count_of("$proc"));
    odin3_node_id proc = proc_node(0);
    expect_dump(0, "I2 O2\nC0 [O1<-I1]\nS posedge I0 [O0<-O1]\n");
    expect_out(proc, 0, wnet("q", 0));
}

static void test_wired_net_is_not_driven(void) {
    read_failing("module t(input a, output y);\n  wand w;\n  always @* w = a;\n"
                 "  assign y = w;\nendmodule\n");
    expect_error((at_loc){3, 13},
                 "'w' is a wired net (wand) and cannot be assigned in an always block");
    TEST_ASSERT_EQUAL_UINT32(0, count_of("$proc"));
}

/* Register-array words are made after the pre-scan: their wires are marked by the second walk. */
typedef struct word_probe {
    bool ran;
    bool word[4]; /* every bit of the wire of word k is marked */
} word_probe;

static word_probe g_probe;

/* Whether the pre-scan marked the first and the last bit of the wire of word `word` of `m`. */
static bool word_marked(odin3_elab2 *e2, uint32_t word) {
    char name[16];
    uint32_t id = 0;
    TEST_ASSERT_TRUE(snprintf(name, sizeof name, "m[%u]", (unsigned)word) > 0);
    TEST_ASSERT_TRUE(
        odin3_strtab_find(odin3_design_strtab(e2->design), odin3_bytes_cstr(name), &id));
    odin3_wire_id wire = odin3_module_find_wire(e2->mod, id);
    return odin3_elab2_always_assigned_word(e2, wire, 0) &&
           odin3_elab2_always_assigned_word(e2, wire, 3);
}

static void probe_words(void *user, odin3_elab2 *e2, odin3_ast_id module_node) {
    (void)user;
    (void)module_node;
    g_probe.ran = true;
    for (uint32_t word = 0; word < 4; word++) {
        g_probe.word[word] = word_marked(e2, word);
    }
}

static void expect_marked_words(const bool *want) {
    TEST_ASSERT_TRUE(g_probe.ran);
    for (uint32_t word = 0; word < 4; word++) {
        TEST_ASSERT_EQUAL_INT(want[word], g_probe.word[word]);
    }
}

static void test_register_words_are_always_assigned(void) {
    memset(&g_probe, 0, sizeof g_probe);
    odin3_elab_test_hooks hooks = {.after_prescan = probe_words};
    odin3_elab_test_set_hooks(&hooks);
    read_src(
        "module t(input c, input [3:0] d, output [3:0] y);\n  reg [3:0] m [0:3];\n"
        "  always @* begin m[1] = d; m[3][2:1] = d[1:0]; end\n  assign y = m[2];\nendmodule\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    expect_warning((at_loc){2, 13}, "replacing memory 'm' with list of registers; see t.v:3");
    expect_ir_ok();
    /* a part select marks the whole word's wire */
    expect_marked_words((const bool[4]){false, true, false, true});
    expect_dump(0, "I4 O12\nC0 [O6..O9<-I0..I3 O10..O11<-I0..I1]\n"
                   "S always [O0..O3<-O6..O9 O4..O5<-O10..O11]\n");
}

/* A write to a register-array word beside the array's other words (constant index). */
static void test_register_word_write(void) {
    read_src("module t(input [3:0] d, output [3:0] y);\n  reg [3:0] m [0:3];\n"
             "  always @* m[1] = d;\n  assign y = m[1];\nendmodule\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    expect_ir_ok();
    TEST_ASSERT_EQUAL_UINT32(1, count_of("$proc"));
    odin3_node_id proc = proc_node(0);
    odin3_wire_id word = odin3_module_find_wire(top(), id_of("m[1]"));
    for (uint32_t k = 0; k < 4; k++) {
        expect_out(proc, k, odin3_wire_net(top(), word, k));
    }
    expect_dump(0, "I4 O8\nC0 [O4..O7<-I0..I3]\nS always [O0..O3<-O4..O7]\n");
}

/* --- the builder: the canonical layout of nested switches (white-box) --------------------------
 */

typedef struct layout_probe {
    bool ran;
    odin3_status status;
    odin3_node_id node;
} layout_probe;

static layout_probe g_layout;

static odin3_net_id fresh_net(odin3_elab2 *e2) {
    odin3_net_id net = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(e2->mod, 0, odin3_module_prov(e2->mod), &net));
    return net;
}

static odin3_sig sig_of(odin3_elab2 *e2, const odin3_sigbit *bits, uint32_t n) {
    odin3_sig sig;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_elab2_sig_new(e2, n, &sig));
    for (uint32_t i = 0; i < n; i++) {
        sig.bits[i] = bits[i];
    }
    return sig;
}

static odin3_status add_action(odin3_pg *pg, odin3_sigbit lhs, odin3_sigbit rhs) {
    pg_act_req req = {pg->cur, 0, &lhs, &rhs, 1, pg->prov};
    return pg_act_add(pg, &req);
}

/* The nets and signals the tree is built from. */
typedef struct tree_ctx {
    odin3_pg *pg;
    odin3_net_id out[8];
    odin3_net_id in;
    odin3_sig sel[4]; /* the switch signals */
    odin3_sig cmp[3]; /* compares 1, 0 and any */
} tree_ctx;

enum { OP_ACT, OP_SWITCH, OP_CASE, OP_END };
enum { V_IN, V_ONE, V_ZERO };
enum { K_ONE, K_ZERO, K_ANY, K_DEFAULT };

typedef struct tree_step {
    int op;
    int arg;   /* ACT: the output; SWITCH: the switch; CASE: the compare kind */
    int value; /* ACT: V_* */
} tree_step;

static const uint16_t k_switch_flags[4] = {0, ODIN3_SWITCH_FULL_CASE, 0,
                                           ODIN3_SWITCH_PARALLEL_CASE};

static odin3_status run_step(tree_ctx *tree, tree_step step) {
    switch (step.op) {
    case OP_ACT: {
        odin3_sigbit rhs[3] = {tree->in.v, odin3_sb_of_bit(ODIN3_BIT_1),
                               odin3_sb_of_bit(ODIN3_BIT_0)};
        return add_action(tree->pg, tree->out[step.arg].v, rhs[step.value]);
    }
    case OP_SWITCH: {
        odin3_pg_switch_req req = {{0}, &tree->sel[step.arg], k_switch_flags[step.arg]};
        return odin3_pg_switch(tree->pg, &req);
    }
    case OP_CASE: {
        odin3_pg_case_req req = {{0},
                                 step.arg == K_DEFAULT ? NULL : &tree->cmp[step.arg],
                                 step.arg == K_DEFAULT ? 0U : 1U};
        return odin3_pg_case(tree->pg, &req);
    }
    default:
        odin3_pg_case_end(tree->pg);
        return ODIN3_OK;
    }
}

/* The tree: root [a0], switches S0 (on s0) and S1 (on s1);
 *   S0: C1 cmp(1) [a1] with switch S2 (on s2): C4 cmp(1) [a4], C5 default [a5];
 *       C2 cmp(0) [a2]; C3 default [a3] with switch S3 (on s3): C6 default [a6];
 *   S1: C7 cmp(ANY) [a7]. */
static const tree_step k_tree[] = {
    {OP_ACT, 0, V_IN},    {OP_SWITCH, 0, 0},   {OP_CASE, K_ONE, 0},     /* C1 */
    {OP_ACT, 1, V_IN},    {OP_SWITCH, 2, 0},   {OP_CASE, K_ONE, 0},     /* C4 */
    {OP_ACT, 4, V_IN},    {OP_END, 0, 0},      {OP_CASE, K_DEFAULT, 0}, /* C5 */
    {OP_ACT, 5, V_ONE},   {OP_END, 0, 0},      {OP_END, 0, 0},          /* back in the root */
    {OP_CASE, K_ZERO, 0},                                               /* C2 */
    {OP_ACT, 2, V_ZERO},  {OP_END, 0, 0},      {OP_CASE, K_DEFAULT, 0}, /* C3 */
    {OP_ACT, 3, V_IN},    {OP_SWITCH, 3, 0},   {OP_CASE, K_DEFAULT, 0}, /* C6 */
    {OP_ACT, 6, V_IN},    {OP_END, 0, 0},      {OP_END, 0, 0},
    {OP_SWITCH, 1, 0},    {OP_CASE, K_ANY, 0}, /* C7 */
    {OP_ACT, 7, V_IN},    {OP_END, 0, 0},
};

static void tree_nets(odin3_elab2 *e2, tree_ctx *tree) {
    odin3_sigbit one = odin3_sb_of_bit(ODIN3_BIT_1);
    odin3_sigbit zero = odin3_sb_of_bit(ODIN3_BIT_0);
    for (uint32_t i = 0; i < 8; i++) {
        tree->out[i] = fresh_net(e2);
    }
    for (uint32_t i = 0; i < 4; i++) {
        odin3_sigbit signal = fresh_net(e2).v;
        tree->sel[i] = sig_of(e2, &signal, 1);
    }
    tree->in = fresh_net(e2);
    tree->cmp[K_ONE] = sig_of(e2, &one, 1);
    tree->cmp[K_ZERO] = sig_of(e2, &zero, 1);
    tree->cmp[K_ANY] = sig_of(e2, &(odin3_sigbit){ODIN3_SB_ANY}, 1);
}

static void build_nested(odin3_elab2 *e2, odin3_ast_id module_node) {
    tree_ctx tree = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pg_create(e2, &tree.pg));
    tree_nets(e2, &tree);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pg_block_reset(tree.pg));
    for (size_t i = 0; i < sizeof k_tree / sizeof k_tree[0]; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_step(&tree, k_tree[i]));
    }
    g_layout.status = odin3_pg_emit(tree.pg, module_node, &g_layout.node);
    g_layout.ran = true;
    odin3_pg_destroy(tree.pg);
}

static void layout_hook(void *user, odin3_elab2 *e2, odin3_ast_id module_node) {
    (void)user;
    build_nested(e2, module_node);
}

/* The renumbering is breadth first and the IR's node verify accepts it (IR-19). */
static void test_canonical_layout_of_nested_switches(void) {
    odin3_elab_test_hooks hooks = {.after_prescan = layout_hook};
    odin3_elab_test_set_hooks(&hooks);
    memset(&g_layout, 0, sizeof g_layout);
    read_src("module t(input a);\nendmodule\n");
    TEST_ASSERT_TRUE(g_layout.ran);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, g_layout.status, et_log(&rs));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    expect_ir_ok();
    /* nets: o0..o7 are O0..O7, s0..s3 I0..I3, in I4, the constants 1 and 0 I5 and I6; the cases
     * are numbered breadth first, the cases of a switch together */
    expect_dump(0, "I7 O8\n"
                   "C0 [O0<-I4] sw0+2\n"
                   "C1 cmp(I5) [O1<-I4] sw2+1\n"
                   "C2 cmp(I6) [O2<-I6]\n"
                   "C3 [O3<-I4] sw3+1\n"
                   "C4 cmp(-) [O7<-I4]\n"
                   "C5 cmp(I5) [O4<-I4]\n"
                   "C6 [O5<-I5]\n"
                   "C7 [O6<-I4]\n"
                   "W0 (I0) f0 C1+3\n"
                   "W1 (I1) f1 C4+1\n"
                   "W2 (I2) f0 C5+2\n"
                   "W3 (I3) f2 C7+1\n");
}

/* --- save and restore of lv and rv, temporaries (white-box) --------------------------------- */

static bool g_maps_ok;

/* rv of net is `want` (a sigbit), or absent when want is 0. */
static void expect_rv(const odin3_pg *pg, odin3_net_id net, odin3_sigbit want) {
    odin3_sigbit bit = 0;
    TEST_ASSERT_EQUAL_INT(want != 0, pg_rv_get(pg, net, &bit));
    TEST_ASSERT_EQUAL_UINT32(want, want != 0 ? bit : 0);
}

static void expect_lv(const odin3_pg *pg, odin3_net_id net, uint32_t want) {
    odin3_net_id got = {0};
    TEST_ASSERT_TRUE(pg_lv_get(pg, net, &got));
    TEST_ASSERT_EQUAL_UINT32(want, got.v);
}

/* The undo log reverts exactly the changes since a mark. */
static void check_undo_log(odin3_pg *pg, odin3_elab2 *e2) {
    odin3_net_id left = fresh_net(e2);
    odin3_net_id first = fresh_net(e2);
    odin3_net_id second = fresh_net(e2);
    odin3_sigbit one = odin3_sb_of_bit(ODIN3_BIT_1);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pg_lv_set(pg, left, first));
    uint32_t outer = odin3_pg_save(pg);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pg_lv_set(pg, left, second));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pg_rv_set(pg, left, one));
    uint32_t inner = odin3_pg_save(pg);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pg_rv_set(pg, left, odin3_sb_of_bit(ODIN3_BIT_X)));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pg_rv_set(pg, first, second.v));
    odin3_pg_restore(pg, inner); /* undoes the last two only */
    expect_rv(pg, left, one);
    expect_rv(pg, first, 0);
    expect_lv(pg, left, second.v);
    odin3_pg_restore(pg, outer); /* the first change stays */
    expect_rv(pg, left, 0);
    expect_lv(pg, left, first.v);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_pg_save(pg));
}

static uint32_t group_of(const odin3_pg *pg, odin3_net_id net, uint32_t pos) {
    pg_grp group = {0, 0};
    TEST_ASSERT_TRUE(pg_group_of(pg, net, &group));
    TEST_ASSERT_EQUAL_UINT32(pos, group.pos);
    return group.group;
}

/* Temporaries: consecutive nets of one group (positions 0, 1, 2), the next call a new group. */
static void check_temporaries(odin3_pg *pg, odin3_ast_id module_node) {
    odin3_net_id first = {0};
    odin3_net_id second = {0};
    pg_grp none = {0, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pg_temp(pg, 3, module_node, &first));
    uint32_t base = group_of(pg, first, 0);
    TEST_ASSERT_EQUAL_UINT32(base, group_of(pg, (odin3_net_id){first.v + 1U}, 1));
    TEST_ASSERT_EQUAL_UINT32(base, group_of(pg, (odin3_net_id){first.v + 2U}, 2));
    TEST_ASSERT_FALSE(pg_group_of(pg, (odin3_net_id){first.v - 1U}, &none));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pg_temp(pg, 1, module_node, &second));
    TEST_ASSERT_EQUAL_UINT32(first.v + 3, second.v);
    TEST_ASSERT_NOT_EQUAL_UINT32(base, group_of(pg, second, 0));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pg_temp(pg, 0, module_node, &first));
}

static void maps_hook(void *user, odin3_elab2 *e2, odin3_ast_id module_node) {
    (void)user;
    odin3_pg *pg = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pg_create(e2, &pg));
    check_undo_log(pg, e2);
    check_temporaries(pg, module_node);
    g_maps_ok = true;
    odin3_pg_destroy(pg);
}

static void test_maps_save_restore_and_temporaries(void) {
    odin3_elab_test_hooks hooks = {.after_prescan = maps_hook};
    odin3_elab_test_set_hooks(&hooks);
    g_maps_ok = false;
    read_src("module t(input a);\nendmodule\n");
    TEST_ASSERT_TRUE_MESSAGE(g_maps_ok, et_log(&rs));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
}

/* --- caps, scopes, hierarchy ------------------------------------------------------------------ */

/* The net-bits cap counts the temporaries (point (aa)): fatal, located at the always. */
static void test_net_bits_cap_counts_temporaries(void) {
    odin3_elab_limits limits = *odin3_elab_limits_get();
    limits.max_net_bits = 10; /* 1 + 4 + 4 wire bits, then 4 temporaries */
    odin3_elab_test_set_limits(&limits);
    read_failing("module t(input c, input [3:0] d, output reg [3:0] q);\n"
                 "  always @(posedge c) q <= d;\nendmodule\n");
    expect_error((at_loc){2, 3}, "elaboration needs more than 10 net bits");
    TEST_ASSERT_EQUAL_UINT32(0, count_of("$proc"));
}

/* Point (aa): the pins of one process are capped like a cell's. */
static void test_pins_cap(void) {
    odin3_elab_limits limits = *odin3_elab_limits_get();
    limits.max_cell_pins = 12; /* the process has 5 + 8 pins */
    odin3_elab_test_set_limits(&limits);
    read_failing("module t(input c, input [3:0] d, output reg [3:0] q);\n"
                 "  always @(posedge c) q <= d;\nendmodule\n");
    expect_error((at_loc){2, 3}, "cell of 13 pins exceeds the limit of 12 pins");
    TEST_ASSERT_EQUAL_UINT32(0, count_of("$proc"));
}

/* An always block in a generate block: its records carry the scope path. */
static void test_generate_scopes(void) {
    read_clean("module t(input [1:0] a, output reg [1:0] y);\n  genvar i;\n"
               "  for (i = 0; i < 2; i = i + 1) begin : g\n    always @* y[i] = a[i];\n  end\n"
               "endmodule\n");
    TEST_ASSERT_EQUAL_UINT32(2, count_of("$proc"));
    for (uint32_t k = 0; k < 2; k++) {
        const odin3_prov_record *rec =
            odin3_prov_get(rs.design, odin3_node_prov(top(), proc_node(k)));
        TEST_ASSERT_NOT_NULL(rec);
        char want[16];
        TEST_ASSERT_TRUE(snprintf(want, sizeof want, "t/g[%u]", (unsigned)k) > 0);
        TEST_ASSERT_EQUAL_STRING(want, str_of(rec->hier));
        TEST_ASSERT_EQUAL_UINT32(wnet("y", k).v, out_net(proc_node(k), 0).v);
    }
}

/* Two modules, both with processes: the generator is made again for the second. */
static void test_two_modules(void) {
    read_clean("module sub(input c, a, output reg q);\n  always @(posedge c) q <= a;\nendmodule\n"
               "module t(input c, a, output q);\n  sub u(c, a, q);\nendmodule\n");
    odin3_design *design = rs.design;
    uint32_t procs = 0;
    for (uint32_t index = 1; index < odin3_design_module_end(design); index++) {
        procs += odin3_module_proc_count(odin3_module_get(design, (odin3_module_id){index}));
    }
    TEST_ASSERT_EQUAL_UINT32(1, procs);
}

/* --- the hand-built store (review I10) ---------------------------------------------------------
 */

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

/* A wire port, or (flags != 0) a port with the given DT and no net kind. */
static odin3_ast_id hand_port(et_tree *tree, uint8_t dir, uint16_t flags, const char *name) {
    eb_decl decl = {.sub = dir, .flags = flags != 0 ? flags : eb_wire_port(), .name = name};
    return eb_port(tree, &decl);
}

/* module t(input clk, a, b, output reg r); reg t;
 * always @(posedge clk) begin t = a & b; r <= t; end endmodule, built by hand. */
static void test_hand_built_store(void) {
    read_clean(k_t7);
    char parsed[256];
    TEST_ASSERT_TRUE(snprintf(parsed, sizeof parsed, "%s", dump(proc_node(0))) > 0);
    et_free(&rs);
    memset(&rs, 0, sizeof rs);

    et_req req = {NULL, 0, NULL, NULL, NULL};
    et_setup(&req, &rs);
    et_tree tree;
    open_held_store(&tree, rs.design);
    uint16_t reg = (uint16_t)(ODIN3_AST_DT_REG << ODIN3_AST_F_PORT_DECL_DT_SHIFT);
    odin3_ast_id ports[4] = {hand_port(&tree, ODIN3_AST_PORT_DECL_INPUT, 0, "clk"),
                             hand_port(&tree, ODIN3_AST_PORT_DECL_INPUT, 0, "a"),
                             hand_port(&tree, ODIN3_AST_PORT_DECL_INPUT, 0, "b"),
                             hand_port(&tree, ODIN3_AST_PORT_DECL_OUTPUT, reg, "r")};
    eb_decl tdecl = {.sub = ODIN3_AST_VAR_DECL_REG, .name = "t"};
    odin3_ast_id stmts[2] = {
        eb_assign(
            &tree, ODIN3_AST_BLOCKING_ASSIGN, et_ident(&tree, "t"),
            et_binary(&tree, ODIN3_AST_BINARY_AND, et_ident(&tree, "a"), et_ident(&tree, "b"))),
        eb_assign(&tree, ODIN3_AST_NONBLOCKING_ASSIGN, et_ident(&tree, "r"), et_ident(&tree, "t"))};
    odin3_ast_id events[1] = {
        eb_event(&tree, ODIN3_AST_EVENT_EXPR_POSEDGE, et_ident(&tree, "clk"))};
    odin3_ast_id items[2] = {eb_var(&tree, &tdecl),
                             eb_always(&tree, events, 1, eb_begin(&tree, stmts, 2))};
    eb_mod mod = {"t", {0}, eb_list(&tree, ports, 4), items, 2};
    odin3_ast_id module = eb_module(&tree, &mod);
    (void)eb_unit(&tree, &module, 1);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, odin3_ast_elaborate(rs.design, tree.ast, NULL),
                                  et_log(&rs));
    TEST_ASSERT_EQUAL_STRING(parsed, dump(proc_node(0)));
    expect_ir_ok();
}

/* --- determinism and the round trip ----------------------------------------------------------- */

static void slurp(const char *path, char *out, size_t cap) {
    FILE *file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(file, path);
    size_t got = fread(out, 1, cap - 1, file);
    out[got] = '\0';
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static const char k_rich[] =
    "module t(input c, d, e, input [3:0] a4, output reg [3:0] q, r, output reg p, s);\n"
    "  reg u;\n  always @(posedge c) begin q <= a4; u = e & d; p <= u; end\n"
    "  always @* begin r = q; s = ~r[0]; end\nendmodule\n";

static void write_rich(const char *path) {
    read_clean(k_rich);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_pass_run(rs.design, "write_rtlil", odin3_bytes_cstr(path)));
}

static void rtlil_of_rich(char *out, size_t cap, const char *path) {
    write_rich(path);
    slurp(path, out, cap);
    TEST_ASSERT_EQUAL_INT(0, remove(path));
}

static void test_deterministic_and_check(void) {
    static char first[TEXT_MAX];
    static char second[TEXT_MAX];
    rtlil_of_rich(first, sizeof first, "rich.il");
    et_free(&rs);
    memset(&rs, 0, sizeof rs);
    rtlil_of_rich(second, sizeof second, "rich2.il");
    TEST_ASSERT_NOT_NULL(strstr(first, "process $proc"));
    TEST_ASSERT_NOT_NULL(strstr(first, "sync posedge \\c"));
    TEST_ASSERT_EQUAL_STRING(first, second);
}

/* Runs argv with its output discarded; its exit status. */
static int run_quiet(char *const argv[]) {
    posix_spawn_file_actions_t actions;
    pid_t pid = 0;
    int status = 0;
    TEST_ASSERT_EQUAL_INT(0, posix_spawn_file_actions_init(&actions));
    TEST_ASSERT_EQUAL_INT(
        0, posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0));
    TEST_ASSERT_EQUAL_INT(0,
                          posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO));
    TEST_ASSERT_EQUAL_INT(0, posix_spawn(&pid, argv[0], &actions, NULL, argv, environ));
    TEST_ASSERT_EQUAL_INT(0, posix_spawn_file_actions_destroy(&actions));
    TEST_ASSERT_EQUAL_INT((int)pid, (int)waitpid(pid, &status, 0));
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* write_rtlil -> Yosys 0.55 read_rtlil; proc exits 0 (the oracle, plan point (p)). */
static void test_yosys_reads_and_procs(void) {
    const char *yosys = getenv("ODIN3_YOSYS055");
    if (yosys == NULL || yosys[0] == '\0') {
        TEST_IGNORE_MESSAGE("no Yosys 0.55 (ODIN3_YOSYS055)");
    }
    write_rich("rich.il");
    char *exe = strdup(yosys);
    TEST_ASSERT_NOT_NULL(exe);
    char *const argv[] = {exe, "-q", "-p", "read_rtlil rich.il; proc", NULL};
    TEST_ASSERT_EQUAL_INT(0, run_quiet(argv));
    free(exe);
    TEST_ASSERT_EQUAL_INT(0, remove("rich.il"));
}

/* --- out of memory ------------------------------------------------------------------------------
 */

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

/* One run of the whole read with the limit-th allocation failing. */
static odin3_status read_failing_at(long limit) {
    fresh_quiet_design();
    g_saw_missing_file = false;
    g_saw_error = false;
    odin3_util_set_alloc_fail_after(limit);
    odin3_status st = odin3_pass_run(rs.design, "read_verilog", odin3_bytes_cstr("t.v"));
    odin3_util_set_alloc_fail_after(-1);
    /* an injected allocation failure is NO_MEMORY, never a user error and never a missing file;
     * an OK read is complete and logged no error */
    TEST_ASSERT_TRUE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY);
    TEST_ASSERT_FALSE(g_saw_missing_file);
    check_not_swallowed(st);
    odin3_check_opts full = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(rs.design, full));
    return st;
}

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

/* Every allocation of the processes fails in turn: NO_MEMORY, a valid IR, no leak (ASan). */
static void test_oom_sweep(void) {
    oom_sweep(k_rich, 300);
}

#define ALL_TESTS(X)                                                                               \
    X(test_t7_blocking_then_nonblocking)                                                           \
    X(test_t8_nonblocking_reads_held_value)                                                        \
    X(test_held_value)                                                                             \
    X(test_combinational_default_mode)                                                             \
    X(test_nolatches_reads_x)                                                                      \
    X(test_nolatches_keeps_clocked_defaults)                                                       \
    X(test_chunks_concat_lvalue)                                                                   \
    X(test_chunks_reversed_concat)                                                                 \
    X(test_chunks_vector_is_one_action)                                                            \
    X(test_chunks_partial_write)                                                                   \
    X(test_chunks_split_at_the_chunk_boundary)                                                     \
    X(test_chunks_overwrite_one_run)                                                               \
    X(test_partial_overwrite_keeps_the_rest)                                                       \
    X(test_constant_pin_in_net_order)                                                              \
    X(test_constant_shared_pin)                                                                    \
    X(test_two_edges)                                                                              \
    X(test_level_lists_are_one_always)                                                             \
    X(test_edge_on_wide_signal)                                                                    \
    X(test_edge_on_expression)                                                                     \
    X(test_incomplete_sensitivity_list)                                                            \
    X(test_edge_on_own_output)                                                                     \
    X(test_net_in_process_warning)                                                                 \
    X(test_attributes_and_provenance)                                                              \
    X(test_empty_block_makes_no_node)                                                              \
    X(test_different_bits_two_nodes)                                                               \
    X(test_pending_if)                                                                             \
    X(test_pending_variable_index_target)                                                          \
    X(test_pending_memory_write)                                                                   \
    X(test_process_after_assign)                                                                   \
    X(test_two_processes_one_bit)                                                                  \
    X(test_two_processes_one_bit_of_a_vector)                                                      \
    X(test_blocking_and_nonblocking)                                                               \
    X(test_same_kind_is_fine)                                                                      \
    X(test_conflict_drops_only_the_conflicting_chunk)                                              \
    X(test_wired_net_is_not_driven)                                                                \
    X(test_register_words_are_always_assigned)                                                     \
    X(test_register_word_write)                                                                    \
    X(test_canonical_layout_of_nested_switches)                                                    \
    X(test_maps_save_restore_and_temporaries)                                                      \
    X(test_net_bits_cap_counts_temporaries)                                                        \
    X(test_pins_cap)                                                                               \
    X(test_generate_scopes)                                                                        \
    X(test_two_modules)                                                                            \
    X(test_hand_built_store)                                                                       \
    X(test_deterministic_and_check)                                                                \
    X(test_yosys_reads_and_procs)                                                                  \
    X(test_oom_sweep)

#define RUN_ONE(name) RUN_TEST(name);

int main(void) {
    UNITY_BEGIN();
    ALL_TESTS(RUN_ONE)
    return UNITY_END();
}
