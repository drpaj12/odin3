/*
 * test_elab_stage2_case.c — 2D Task 22: `if`, `case`, `casez`, `casex`, the case temporaries and
 * the switch flags of EL-36, and the case method for a non-constant lvalue select (EL-32 writes).
 * Each `$proc` body is asserted exactly through a text dump of the node (the pins as I<k> / O<k>
 * in net-ID order; every case, switch and sync in IR-19's canonical layout; a compare lists its
 * bits least significant first, `-` is ANY), the whole read_verilog pass runs through the pass
 * manager (elab_test.h), and every located diagnostic is asserted as the whole line.
 *
 * Yosys 0.55 (git sha1 60f126cd0) is the oracle; the commands behind the expected values are in
 * the task report (read_verilog; write_rtlil for the process text, proc for the lowering), and the
 * CTest elab_case_bridge runs elab-equiv on the sources under tests/verilog/case-bridge.
 */
#include "elab_test.h"

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
#include "odin3/odin3.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { OOM_LIMIT = 400000, OOM_SETTLE = 32, DUMP_MAX = 1 << 13 };

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

/* --- if / else (EL-36) ------------------------------------------------------------------------ */

/* `if (s) y = a; else y = b;`: t1 for y, a switch on s, case 1'1 and the default (the brief). */
static void test_if_else_default_mode(void) {
    read_clean("module t(input s, a, b, output reg y);\n"
               "  always @* if (s) y = a; else y = b;\nendmodule\n");
    odin3_node_id proc = proc_node(0);
    expect_dump(0, "I4 O3\n"
                   "C0 [O1<-O2] sw0+1\n"
                   "C1 cmp(I3) [O2<-I1]\n"
                   "C2 [O2<-I2]\n"
                   "W0 (I0) f0 C1+2\n"
                   "S always [O0<-O1]\n");
    expect_in(proc, 0, wnet("s", 0));
    expect_in(proc, 1, wnet("a", 0));
    expect_in(proc, 2, wnet("b", 0));
    expect_in(proc, 3, const_net_of(ODIN3_BIT_1));
    expect_out(proc, 0, wnet("y", 0));
    TEST_ASSERT_TRUE(out_net(proc, 1).v > wnet("y", 0).v &&
                     out_net(proc, 2).v > out_net(proc, 1).v);
}

/* Latch mode: the added default holds the output (a latch path); the root default is replaced. */
static void test_if_latch_default_mode(void) {
    read_clean("module t(input s, a, output reg y);\n  always @* if (s) y = a;\nendmodule\n");
    expect_dump(0, "I3 O3\n"
                   "C0 [O1<-O2] sw0+1\n"
                   "C1 cmp(I2) [O2<-I1]\n"
                   "C2 [O2<-O0]\n"
                   "W0 (I0) f0 C1+2\n"
                   "S always [O0<-O1]\n");
}

/* --nolatches: no root default; y reads x until assigned, so the added default holds 1'x. */
static void test_if_latch_nolatches(void) {
    pass_src nolatches = {"--nolatches t.v", "module t(input s, a, output reg y);\n"
                                             "  always @* if (s) y = a;\nendmodule\n"};
    read_with(&nolatches);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    expect_ir_ok();
    odin3_node_id proc = proc_node(0);
    expect_dump(0, "I4 O3\n"
                   "C0 [O1<-O2] sw0+1\n"
                   "C1 cmp(I2) [O2<-I1]\n"
                   "C2 [O2<-I3]\n"
                   "W0 (I0) f0 C1+2\n"
                   "S always [O0<-O1]\n");
    expect_in(proc, 2, const_net_of(ODIN3_BIT_1));
    expect_in(proc, 3, const_net_of(ODIN3_BIT_X));
}

/* A condition wider than a bit is $reduce_bool(c). */
static void test_if_wide_condition(void) {
    read_clean("module t(input [2:0] s, input a, output reg y);\n"
               "  always @* if (s) y = a;\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32(1, count_of("$reduce_bool"));
    odin3_node_id reduce = nth_node("$reduce_bool", 0);
    odin3_pinslice y_pins = odin3_node_port(top(), reduce, 1);
    odin3_node_id proc = proc_node(0);
    expect_dump(0, "I3 O3\n"
                   "C0 [O1<-O2] sw0+1\n"
                   "C1 cmp(I2) [O2<-I0]\n"
                   "C2 [O2<-O0]\n"
                   "W0 (I1) f0 C1+2\n"
                   "S always [O0<-O1]\n");
    expect_in(proc, 0, wnet("a", 0));
    expect_in(proc, 1, odin3_pin_net(top(), y_pins.first));
}

/* Non-blocking assignments write the root temporary: no t1, no added default. */
static void test_nonblocking_branch_has_no_case_temporary(void) {
    read_clean("module t(input c, e, d, output reg q);\n"
               "  always @(posedge c) if (e) q <= d;\nendmodule\n");
    expect_dump(0, "I4 O2\n"
                   "C0 [O1<-O0] sw0+1\n"
                   "C1 cmp(I3) [O1<-I2]\n"
                   "W0 (I1) f0 C1+1\n"
                   "S posedge I0 [O0<-O1]\n");
}

/* --- case, casez, casex (EL-36) --------------------------------------------------------------- */

static const char k_case_items[] =
    "module t(input [1:0] sel, input a, b, c, output reg y);\n"
    "  always @* case (sel) 2'd0: y = a; 2'd1, 2'd2: y = b; default: y = c; endcase\n"
    "endmodule\n";

/* The items in source order, one case per item with its compares, the default last. */
static void test_case_items_and_default(void) {
    read_clean(k_case_items);
    expect_dump(0, "I7 O3\n"
                   "C0 [O1<-O2] sw0+1\n"
                   "C1 cmp(I5,I5) [O2<-I2]\n"
                   "C2 cmp(I6,I5|I5..I6) [O2<-I3]\n"
                   "C3 [O2<-I4]\n"
                   "W0 (I0..I1) f0 C1+3\n"
                   "S always [O0<-O1]\n");
}

/* The default written first is still the last case (C6). */
static void test_case_default_first_is_last(void) {
    read_clean("module t(input [1:0] sel, input a, b, c, output reg y);\n"
               "  always @* case (sel) default: y = c; 2'd0: y = a; 2'd1, 2'd2: y = b; endcase\n"
               "endmodule\n");
    expect_dump(0, "I7 O3\n"
                   "C0 [O1<-O2] sw0+1\n"
                   "C1 cmp(I5,I5) [O2<-I2]\n"
                   "C2 cmp(I6,I5|I5..I6) [O2<-I3]\n"
                   "C3 [O2<-I4]\n"
                   "W0 (I0..I1) f0 C1+3\n"
                   "S always [O0<-O1]\n");
}

/* C8 (review M10): `case (1'b1)` with non-constant items compares on the items' own pins. */
static void test_case_non_constant_items(void) {
    read_clean("module t(input a, b, output reg y);\n"
               "  always @* case (1'b1) a: y = 1'b1; b: y = 1'b0; endcase\nendmodule\n");
    odin3_node_id proc = proc_node(0);
    expect_dump(0, "I4 O3\n"
                   "C0 [O1<-O2] sw0+1\n"
                   "C1 cmp(I0) [O2<-I2]\n"
                   "C2 cmp(I1) [O2<-I3]\n"
                   "C3 [O2<-O0]\n"
                   "W0 (I2) f0 C1+3\n"
                   "S always [O0<-O1]\n");
    expect_in(proc, 0, wnet("a", 0));
    expect_in(proc, 1, wnet("b", 0));
    expect_in(proc, 2, const_net_of(ODIN3_BIT_1));
    expect_in(proc, 3, const_net_of(ODIN3_BIT_0));
}

/* Wildcards: casez `?`, casex `x`; the plain case keeps x as a bit (C9). Bits least significant
 * first, so `2'b1?` is [ANY, 1]. */
static void test_wildcards(void) {
    read_clean("module t(input [1:0] sel, input a, output reg y);\n"
               "  always @* casez (sel) 2'b1?: y = a; default: y = 1'b0; endcase\nendmodule\n");
    expect_dump(0, "I5 O3\n"
                   "C0 [O1<-O2] sw0+1\n"
                   "C1 cmp(-,I3) [O2<-I2]\n"
                   "C2 [O2<-I4]\n"
                   "W0 (I0..I1) f0 C1+2\n"
                   "S always [O0<-O1]\n");
}

static void test_casex_wildcard(void) {
    read_clean("module t(input [1:0] sel, input a, output reg y);\n"
               "  always @* casex (sel) 2'bx1: y = a; default: y = 1'b0; endcase\nendmodule\n");
    expect_dump(0, "I5 O3\n"
                   "C0 [O1<-O2] sw0+1\n"
                   "C1 cmp(I3,-) [O2<-I2]\n"
                   "C2 [O2<-I4]\n"
                   "W0 (I0..I1) f0 C1+2\n"
                   "S always [O0<-O1]\n");
}

static void test_plain_case_keeps_x(void) {
    read_clean("module t(input [1:0] sel, input a, output reg y);\n"
               "  always @* case (sel) 2'bx1: y = a; default: y = 1'b0; endcase\nendmodule\n");
    odin3_node_id proc = proc_node(0);
    expect_dump(0, "I6 O3\n"
                   "C0 [O1<-O2] sw0+1\n"
                   "C1 cmp(I3..I4) [O2<-I2]\n"
                   "C2 [O2<-I5]\n"
                   "W0 (I0..I1) f0 C1+2\n"
                   "S always [O0<-O1]\n");
    expect_in(proc, 3, const_net_of(ODIN3_BIT_1));
    expect_in(proc, 4, const_net_of(ODIN3_BIT_X));
    expect_in(proc, 5, const_net_of(ODIN3_BIT_0));
}

/* `case (a4) 3'd1:` compares at the widest width: 4 bits. */
static void test_case_width(void) {
    read_clean("module t(input [3:0] a4, input a, output reg y);\n"
               "  always @* case (a4) 3'd1: y = a; endcase\nendmodule\n");
    expect_dump(0, "I7 O3\n"
                   "C0 [O1<-O2] sw0+1\n"
                   "C1 cmp(I5..I6,I6,I6) [O2<-I4]\n"
                   "C2 [O2<-O0]\n"
                   "W0 (I0..I3) f0 C1+2\n"
                   "S always [O0<-O1]\n");
}

/* v4.2 chunks: an 8-bit y is one 8-bit action in every case, the added default and the exit. */
static void test_chunks_case_default_item(void) {
    read_clean("module t(input [1:0] s, input [7:0] a8, b8, c8, output reg [7:0] y);\n"
               "  always @* case (s) 2'd0: y = a8; 2'd1: y = b8; default: y = c8; endcase\n"
               "endmodule\n");
    expect_dump(0, "I28 O24\n"
                   "C0 [O8..O15<-O16..O23] sw0+1\n"
                   "C1 cmp(I26,I26) [O16..O23<-I2..I9]\n"
                   "C2 cmp(I27,I26) [O16..O23<-I10..I17]\n"
                   "C3 [O16..O23<-I18..I25]\n"
                   "W0 (I0..I1) f0 C1+3\n"
                   "S always [O0..O7<-O8..O15]\n");
}

static void test_chunks_case_added_default(void) {
    read_clean("module t(input [1:0] s, input [7:0] a8, b8, output reg [7:0] y);\n"
               "  always @* case (s) 2'd0: y = a8; 2'd1: y = b8; endcase\nendmodule\n");
    expect_dump(0, "I20 O24\n"
                   "C0 [O8..O15<-O16..O23] sw0+1\n"
                   "C1 cmp(I18,I18) [O16..O23<-I2..I9]\n"
                   "C2 cmp(I19,I18) [O16..O23<-I10..I17]\n"
                   "C3 [O16..O23<-O0..O7]\n"
                   "W0 (I0..I1) f0 C1+3\n"
                   "S always [O0..O7<-O8..O15]\n");
}

/* --- nesting (internal risk 16 #3) ------------------------------------------------------------ */

/* Each switch has its own t1 and the outer state is restored after the inner one. */
static void test_nested_if(void) {
    read_clean("module t(input a, b, output reg y);\n"
               "  always @* if (a) begin y = 1'b1; if (b) y = 1'b0; end\nendmodule\n");
    odin3_node_id proc = proc_node(0);
    expect_dump(0, "I4 O4\n"
                   "C0 [O1<-O2] sw0+1\n"
                   "C1 cmp(I2) [O2<-O3] sw1+1\n"
                   "C2 [O2<-O0]\n"
                   "C3 cmp(I2) [O3<-I3]\n"
                   "C4 [O3<-I2]\n"
                   "W0 (I0) f0 C1+2\n"
                   "W1 (I1) f0 C3+2\n"
                   "S always [O0<-O1]\n");
    expect_in(proc, 2, const_net_of(ODIN3_BIT_1));
    expect_in(proc, 3, const_net_of(ODIN3_BIT_0));
}

/* A blocking read after the switch sees t1 (rv): `z = y` takes the switch's temporary. */
static void test_read_after_switch_sees_t1(void) {
    read_clean("module t(input s, a, b, output reg y, z);\n"
               "  always @* begin if (s) y = a; else y = b; z = y; end\nendmodule\n");
    expect_dump(0, "I4 O5\n"
                   "C0 [O2<-O4 O3<-O4] sw0+1\n"
                   "C1 cmp(I3) [O4<-I1]\n"
                   "C2 [O4<-I2]\n"
                   "W0 (I0) f0 C1+2\n"
                   "S always [O0<-O2 O1<-O3]\n");
}

/* --- full_case, parallel_case (C10, D1) --------------------------------------------------------
 */

static const char k_full_case[] = "module t(input s, a, output reg y);\n"
                                  "  always @* (* full_case *) case (s) 1'b0: y = a; endcase\n"
                                  "endmodule\n";

/* The flag, the added default holding 1'x, and D1's warning (2D emits it, not 2E). */
static void test_full_case_attribute(void) {
    read_src(k_full_case);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    expect_ir_ok();
    expect_dump(0, "I4 O3\n"
                   "C0 [O1<-O2] sw0+1\n"
                   "C1 cmp(I2) [O2<-I1]\n"
                   "C2 [O2<-I3]\n"
                   "W0 (I0) f1 C1+2\n"
                   "S always [O0<-O1]\n");
    expect_warning((at_loc){2, 16}, "'full_case' is honoured: case values no item lists assign x");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, soft_warnings(), et_log(&rs));
}

/* The metacomment form: both flags, both warnings. */
static void test_synopsys_metacomment_both_flags(void) {
    read_src("module t(input s, a, output reg y);\n"
             "  always @* case (s) // synopsys full_case parallel_case\n"
             "    1'b0: y = a;\n  endcase\nendmodule\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    expect_ir_ok();
    expect_dump(0, "I4 O3\n"
                   "C0 [O1<-O2] sw0+1\n"
                   "C1 cmp(I2) [O2<-I1]\n"
                   "C2 [O2<-I3]\n"
                   "W0 (I0) f3 C1+2\n"
                   "S always [O0<-O1]\n");
    expect_warning((at_loc){2, 22}, "'full_case' is honoured: case values no item lists assign x");
    expect_warning((at_loc){2, 22}, "'parallel_case' is honoured: case items are not prioritised");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(2, soft_warnings(), et_log(&rs));
}

/* parallel_case alone sets its flag only; `= 0` is no flag at all. */
static void test_parallel_case_and_false_attribute(void) {
    read_src("module t(input s, a, output reg y);\n"
             "  always @* (* parallel_case *) case (s) 1'b0: y = a; endcase\nendmodule\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    expect_dump(0, "I3 O3\n"
                   "C0 [O1<-O2] sw0+1\n"
                   "C1 cmp(I2) [O2<-I1]\n"
                   "C2 [O2<-O0]\n"
                   "W0 (I0) f2 C1+2\n"
                   "S always [O0<-O1]\n");
    TEST_ASSERT_EQUAL_UINT32(1, warnings_with("'parallel_case' is honoured"));
    et_free(&rs);
    memset(&rs, 0, sizeof rs);
    read_clean("module t(input s, a, output reg y);\n"
               "  always @* (* full_case = 0 *) case (s) 1'b0: y = a; endcase\nendmodule\n");
    expect_dump(0, "I3 O3\n"
                   "C0 [O1<-O2] sw0+1\n"
                   "C1 cmp(I2) [O2<-I1]\n"
                   "C2 [O2<-O0]\n"
                   "W0 (I0) f0 C1+2\n"
                   "S always [O0<-O1]\n");
}

/* --- constants fold (the brief; Yosys folds only what the AST makes constant) ---------------- */

static void test_constant_if_folds(void) {
    read_clean("module t(input a, b, output reg y);\n"
               "  always @* if (1) y = a; else y = b;\nendmodule\n");
    expect_dump(0, "I1 O2\nC0 [O1<-I0]\nS always [O0<-O1]\n");
    expect_in(proc_node(0), 0, wnet("a", 0));
    et_free(&rs);
    memset(&rs, 0, sizeof rs);
    read_clean("module t(input a, b, output reg y);\n"
               "  always @* if (1'b0) y = a; else y = b;\nendmodule\n");
    expect_dump(0, "I1 O2\nC0 [O1<-I0]\nS always [O0<-O1]\n");
    expect_in(proc_node(0), 0, wnet("b", 0));
}

/* A parameter is constant; the dead branch's output is no output (Yosys simplifies it away). */
static void test_constant_parameter_and_dead_output(void) {
    read_clean("module t(input a, b, output reg y, z);\n  parameter P = 2;\n"
               "  always @* if (P > 1) y = a; else z = b;\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32(1, count_of("$proc"));
    expect_dump(0, "I1 O2\nC0 [O1<-I0]\nS always [O0<-O1]\n");
    expect_out(proc_node(0), 0, wnet("y", 0));
}

static void test_constant_case_folds(void) {
    read_clean("module t(input a, b, c, output reg y);\n"
               "  always @* case (2'd1) 2'd0: y = a; 2'd1: y = b; default: y = c; endcase\n"
               "endmodule\n");
    expect_dump(0, "I1 O2\nC0 [O1<-I0]\nS always [O0<-O1]\n");
    expect_in(proc_node(0), 0, wnet("b", 0));
    et_free(&rs);
    memset(&rs, 0, sizeof rs);
    read_clean("module t(input a, b, c, output reg y);\n"
               "  always @* case (2'd3) 2'd0: y = a; 2'd1: y = b; default: y = c; endcase\n"
               "endmodule\n");
    expect_in(proc_node(0), 0, wnet("c", 0));
}

/* No item matches and there is no default: nothing runs, so the block has no output. */
static void test_constant_case_no_match_makes_no_node(void) {
    read_clean("module t(input a, output reg y);\n"
               "  always @* case (2'd3) 2'd0: y = a; endcase\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32(0, count_of("$proc"));
}

/* An x condition is not folded: proc decides (the switch is kept on the x constant). */
static void test_x_condition_is_not_folded(void) {
    read_clean("module t(input a, b, output reg y);\n"
               "  always @* if (1'bx) y = a; else y = b;\nendmodule\n");
    odin3_node_id proc = proc_node(0);
    expect_dump(0, "I4 O3\n"
                   "C0 [O1<-O2] sw0+1\n"
                   "C1 cmp(I2) [O2<-I0]\n"
                   "C2 [O2<-I1]\n"
                   "W0 (I3) f0 C1+2\n"
                   "S always [O0<-O1]\n");
    expect_in(proc, 3, const_net_of(ODIN3_BIT_X));
}

/* A variable that holds a constant is not constant by the AST: the switch stays. */
static void test_variable_holding_a_constant_is_not_folded(void) {
    read_clean("module t(input a, b, output reg y);\n  reg s;\n"
               "  always @* begin s = 1'b1; if (s) y = a; else y = b; end\nendmodule\n");
    TEST_ASSERT_EQUAL_UINT32(1, count_of("$proc"));
    TEST_ASSERT_TRUE(strstr(dump(proc_node(0)), "sw") != NULL);
}

/* --- nesting depth: no recursion ---------------------------------------------------------------
 */

enum { CHAIN_LENGTH = 400, CHAIN_TEXT = 1 << 15 };

/* An else-if chain of CHAIN_LENGTH tests nests that many switches; frames, not the C stack. */
static void test_deep_else_if_chain(void) {
    static char text[CHAIN_TEXT];
    size_t len = 0;
    len += (size_t)snprintf(text + len, sizeof text - len,
                            "module t(input [15:0] s, output reg [15:0] y);\n  always @* begin\n"
                            "    if (s == 16'd0) y = 16'd0;\n");
    for (unsigned k = 1; k < CHAIN_LENGTH; k++) {
        len += (size_t)snprintf(text + len, sizeof text - len,
                                "    else if (s == 16'd%u) y = 16'd%u;\n", k, k);
        TEST_ASSERT_TRUE(len < sizeof text);
    }
    len +=
        (size_t)snprintf(text + len, sizeof text - len, "    else y = 16'd7;\n  end\nendmodule\n");
    TEST_ASSERT_TRUE(len < sizeof text);
    read_clean(text);
    TEST_ASSERT_EQUAL_UINT32(1, count_of("$proc"));
    odin3_proc_body storage;
    const odin3_proc_body *body = body_of(proc_node(0), &storage);
    TEST_ASSERT_EQUAL_UINT32(CHAIN_LENGTH, body->n_switches);
    TEST_ASSERT_EQUAL_UINT32(2U * CHAIN_LENGTH + 1U, body->n_cases);
}

/* --- non-constant lvalue selects: the case method (EL-32 writes) -------------------------------
 */

static void test_variable_index_write(void) {
    read_clean("module t(input c, input [1:0] s, input d, output reg [3:0] y);\n"
               "  always @(posedge c) y[s] <= d;\nendmodule\n");
    odin3_node_id proc = proc_node(0);
    expect_dump(0, "I6 O10\n"
                   "C0 [O4..O7<-O0..O3 O8..O9<-I1..I2] sw0+1\n"
                   "C1 cmp(I4,I4) [O4<-I3]\n"
                   "C2 cmp(I5,I4) [O5<-I3]\n"
                   "C3 cmp(I4..I5) [O6<-I3]\n"
                   "C4 cmp(I5,I5) [O7<-I3]\n"
                   "W0 (O8..O9) f0 C1+4\n"
                   "S posedge I0 [O0..O3<-O4..O7]\n");
    expect_in(proc, 3, wnet("d", 0));
    for (uint32_t k = 0; k < 4; k++) {
        expect_out(proc, k, wnet("y", k));
    }
}

static void test_indexed_part_select_write(void) {
    read_clean("module t(input c, input [1:0] b, input [1:0] d2, output reg [3:0] y);\n"
               "  always @(posedge c) y[b +: 2] <= d2;\nendmodule\n");
    expect_dump(0, "I7 O10\n"
                   "C0 [O4..O7<-O0..O3 O8..O9<-I1..I2] sw0+1\n"
                   "C1 cmp(I5,I5) [O4..O5<-I3..I4]\n"
                   "C2 cmp(I6,I5) [O5..O6<-I3..I4]\n"
                   "C3 cmp(I5..I6) [O6..O7<-I3..I4]\n"
                   "W0 (O8..O9) f0 C1+3\n"
                   "S posedge I0 [O0..O3<-O4..O7]\n");
}

/* [b -: w] covers b, b-1 .. b-w+1: positions 1..3 of a [3:0] target for a 2-bit base. */
static void test_indexed_part_select_minus_write(void) {
    read_clean("module t(input c, input [1:0] b, input [1:0] d2, output reg [3:0] y);\n"
               "  always @(posedge c) y[b -: 2] <= d2;\nendmodule\n");
    expect_dump(0, "I7 O10\n"
                   "C0 [O4..O7<-O0..O3 O8..O9<-I1..I2] sw0+1\n"
                   "C1 cmp(I5..I6) [O4..O5<-I3..I4]\n"
                   "C2 cmp(I6,I5) [O5..O6<-I3..I4]\n"
                   "C3 cmp(I5,I5) [O6..O7<-I3..I4]\n"
                   "W0 (O8..O9) f0 C1+3\n"
                   "S posedge I0 [O0..O3<-O4..O7]\n");
}

/* An ascending target maps the declared index to the bit position by EL-23. */
static void test_ascending_target(void) {
    read_clean("module t(input c, input [1:0] s, input d, output reg [0:3] y);\n"
               "  always @(posedge c) y[s] <= d;\nendmodule\n");
    expect_dump(0, "I6 O10\n"
                   "C0 [O4..O7<-O0..O3 O8..O9<-I1..I2] sw0+1\n"
                   "C1 cmp(I4,I4) [O7<-I3]\n"
                   "C2 cmp(I5,I4) [O6<-I3]\n"
                   "C3 cmp(I4..I5) [O5<-I3]\n"
                   "C4 cmp(I5,I5) [O4<-I3]\n"
                   "W0 (O8..O9) f0 C1+4\n"
                   "S posedge I0 [O0..O3<-O4..O7]\n");
}

/* A signed 2-bit index holds 0 and 1 of a [3:0] target: two cases, and only y[1:0] is an output
 * (Yosys 0.55 writes the same two items). */
static void test_signed_index_limits_the_positions(void) {
    read_clean("module t(input c, input signed [1:0] s, input d, output reg [3:0] y);\n"
               "  always @(posedge c) y[s] <= d;\nendmodule\n");
    expect_dump(0, "I6 O6\n"
                   "C0 [O2..O3<-O0..O1 O4..O5<-I1..I2] sw0+1\n"
                   "C1 cmp(I4,I4) [O2<-I3]\n"
                   "C2 cmp(I5,I4) [O3<-I3]\n"
                   "W0 (O4..O5) f0 C1+2\n"
                   "S posedge I0 [O0..O1<-O2..O3]\n");
}

/* A blocking write is the switch of EL-36: case temporaries for every bit a position writes, and
 * the default that holds them (a latch path in default mode). */
static void test_blocking_variable_index_write(void) {
    read_clean("module t(input [1:0] s, input d, output reg [3:0] y);\n"
               "  always @* y[s] = d;\nendmodule\n");
    expect_dump(0, "I5 O14\n"
                   "C0 [O8..O9<-I0..I1 O4..O7<-O10..O13] sw0+1\n"
                   "C1 cmp(I3,I3) [O11..O13<-O1..O3 O10<-I2]\n"
                   "C2 cmp(I4,I3) [O10,O12..O13<-O0,O2..O3 O11<-I2]\n"
                   "C3 cmp(I3..I4) [O10..O11,O13<-O0..O1,O3 O12<-I2]\n"
                   "C4 cmp(I4,I4) [O10..O12<-O0..O2 O13<-I2]\n"
                   "C5 [O10..O13<-O0..O3]\n"
                   "W0 (O8..O9) f0 C1+5\n"
                   "S always [O0..O3<-O4..O7]\n");
}

/* A select that can reach nothing inside its target writes nothing: warned, no output. */
static void test_index_that_selects_nothing(void) {
    read_src("module t(input c, input [1:0] s, input d, output reg [7:4] y);\n"
             "  always @(posedge c) y[s] <= d;\nendmodule\n");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    expect_warning((at_loc){2, 23}, "assignment to bits outside 'y' [7:4] is ignored");
    TEST_ASSERT_EQUAL_UINT32(0, count_of("$proc"));
}

/* A non-constant select inside a concatenation: Yosys 0.55 silently drops the write (spec 17). */
static void test_variable_index_in_concatenation_is_an_error(void) {
    read_failing("module t(input c, input [1:0] s, d2, output reg [3:0] y, output reg z);\n"
                 "  always @(posedge c) {y[s], z} <= d2;\nendmodule\n");
    expect_error((at_loc){2, 24},
                 "non-constant index into 'y' is not supported in a concatenation target");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, diag_errors(), et_log(&rs));
}

/* --- what is still pending (point (l)) ---------------------------------------------------------
 */

/* A memory write inside an if arm and inside a case arm is reported, not dropped (Task 24). */
static void test_memory_write_in_if_arm_is_pending(void) {
    read_failing("module t(input c, e, input [1:0] i, input [7:0] d, output [7:0] y);\n"
                 "  reg [7:0] m [0:3];\n  assign y = m[i];\n"
                 "  always @(posedge c) if (e) m[i] <= d;\nendmodule\n");
    expect_error((at_loc){4, 30}, "elaboration is not available yet (2D): 'memory write'");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, diag_errors(), et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32(0, count_of("$proc"));
}

static void test_memory_write_in_case_arm_is_pending(void) {
    read_failing("module t(input c, e, input [1:0] i, input [7:0] d, output [7:0] y);\n"
                 "  reg [7:0] m [0:3];\n  assign y = m[i];\n"
                 "  always @(posedge c) case (e) 1'b1: m[i] <= d; endcase\nendmodule\n");
    expect_error((at_loc){4, 38}, "elaboration is not available yet (2D): 'memory write'");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, diag_errors(), et_log(&rs));
}

/* Loops are Task 23's: the located line, from inside an if arm too. */
static void test_loop_in_if_arm_is_pending(void) {
    read_failing("module t(input e, output reg y);\n"
                 "  always @* if (e) repeat (2) y = 1'b1;\nendmodule\n");
    expect_error((at_loc){2, 20}, "elaboration is not available yet (2D): 'REPEAT'");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, diag_errors(), et_log(&rs));
}

/* A word of a register array with a variable index is EL-27's case over the words (Task 23). */
static void test_register_word_variable_index_is_pending(void) {
    read_failing("module t(input [1:0] s, input [3:0] d, output [3:0] y);\n  reg [3:0] w [0:3];\n"
                 "  assign y = w[0];\n  always @* w[s] = d;\nendmodule\n");
    expect_error((at_loc){4, 13},
                 "elaboration is not available yet (2D): 'variable index into a register array "
                 "in a procedural assignment target'");
    TEST_ASSERT_EQUAL_UINT32(1, warnings_with("replacing memory 'w' with list of registers"));
}

/* --- out of memory ----------------------------------------------------------------------------
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

/* if, nested if, case with temporaries, a variable-index write and a blocking one. */
static const char k_rich[] =
    "module t(input c, input [1:0] s, input [3:0] a, b, output reg [3:0] y, z, w, v);\n"
    "  always @(posedge c) begin\n"
    "    if (a[0]) begin y = a; if (b[1]) y = b; end else y = 4'd0;\n"
    "    case (s) 2'd0: z <= a; 2'd1, 2'd2: z <= b; default: z <= 4'd1; endcase\n"
    "    w[s] <= a[0];\n"
    "    v[b[1:0] +: 2] <= a[1:0];\n"
    "  end\n"
    "endmodule\n";

/* Flags, wildcards, folding and a blocking select inside a branch. */
static const char k_rich_flags[] =
    "module t(input [1:0] s, input [3:0] a, b, input e, output reg [3:0] y, z);\n"
    "  parameter P = 1;\n"
    "  always @* begin\n"
    "    (* full_case, parallel_case *) casez (s) 2'b1?: y = a; 2'b01: y = b; endcase\n"
    "    if (P) z = a; else z = b;\n"
    "    if (e) z[s] = b[0];\n"
    "  end\n"
    "endmodule\n";

/* Every allocation of the processes fails in turn: NO_MEMORY, a valid IR, no leak (ASan). */
static void test_oom_sweep(void) {
    oom_sweep(k_rich, 300);
}

static void test_oom_sweep_flags_and_folding(void) {
    oom_sweep(k_rich_flags, 300);
}

/* --- the builder primitives under allocation failure (white-box; review M2 of Task 21) ---------
 */

typedef struct wb_probe {
    bool ran;
    uint32_t failures;
    uint32_t settled;
    odin3_ast_id lhs; /* the declarator of `y` */
} wb_probe;

static wb_probe g_wb;

static odin3_net_id wb_net(odin3_elab2 *e2) {
    odin3_net_id net = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_create(e2->mod, 0, odin3_module_prov(e2->mod), &net));
    return net;
}

/* The DECLARATOR named y under the module. */
static odin3_ast_id wb_declarator(odin3_elab2 *e2, odin3_ast_id module_node) {
    odin3_ast_cursor cur;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_cursor_init(&cur, e2->elab, module_node));
    odin3_ast_event ev;
    odin3_ast_id node;
    odin3_ast_id found = {0};
    while (found.v == 0 && odin3_ast_cursor_next(&cur, &ev, &node)) {
        bool declarator =
            ev == ODIN3_AST_ENTER && odin3_ast_kind_of(e2->elab, node) == ODIN3_AST_DECLARATOR;
        found = declarator && strcmp(odin3_ast_name_str(e2->elab, node), "y") == 0 ? node : found;
    }
    odin3_ast_cursor_free(&cur);
    TEST_ASSERT_NOT_EQUAL_UINT32(0, found.v);
    return found;
}

/* The primitives in use: a temporary, a switch on it, a case with a compare in which an
 * assignment to y[1:0] lands (lv set up before), the case ended, a default case. */
static odin3_status wb_sequence(odin3_pg *pg, odin3_ast_id module_node) {
    odin3_net_id first = {0};
    odin3_status st = odin3_pg_temp(pg, 2, module_node, &first);
    odin3_sigbit signal_bit = first.v;
    odin3_sig signal = {&signal_bit, 1, false};
    odin3_pg_switch_req sw = {module_node, &signal, 0};
    st = st != ODIN3_OK ? st : odin3_pg_switch(pg, &sw);
    odin3_sigbit one = odin3_sb_of_bit(ODIN3_BIT_1);
    odin3_sig compare = {&one, 1, false};
    odin3_pg_case_req item = {module_node, &compare, 1};
    st = st != ODIN3_OK ? st : odin3_pg_case(pg, &item);
    odin3_sigbit value[2] = {odin3_sb_of_bit(ODIN3_BIT_0), odin3_sb_of_bit(ODIN3_BIT_1)};
    odin3_sig rhs = {value, 2, false};
    odin3_pg_assign_req assign = {module_node, g_wb.lhs, &rhs, true};
    st = st != ODIN3_OK ? st : odin3_pg_assign(pg, &assign);
    odin3_pg_case_end(pg);
    odin3_pg_case_req dflt = {module_node, NULL, 0};
    st = st != ODIN3_OK ? st : odin3_pg_case(pg, &dflt);
    odin3_pg_case_end(pg);
    return st;
}

/* One iteration: a fresh generator, the sequence with the limit-th allocation failing, then the
 * emission (failure-free) when the sequence went through. A failed sequence is NO_MEMORY and
 * leaves no node. */
static odin3_status wb_run(odin3_elab2 *e2, odin3_ast_id module_node, long limit) {
    odin3_pg *pg = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pg_create(e2, &pg));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pg_block_reset(pg));
    odin3_wire_id wire = odin3_module_find_wire(e2->mod, id_of("y"));
    for (uint32_t k = 0; k < 2; k++) { /* lv(y[k]) = a fresh net, so the assignment makes actions */
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              pg_lv_set(pg, odin3_wire_net(e2->mod, wire, k), wb_net(e2)));
    }
    uint32_t nodes = odin3_module_node_end(e2->mod);
    odin3_util_set_alloc_fail_after(limit);
    odin3_status st = wb_sequence(pg, module_node);
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_TRUE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY);
    if (st == ODIN3_OK) {
        odin3_node_id node = {0};
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pg_emit(pg, module_node, &node));
        TEST_ASSERT_NOT_EQUAL_UINT32(0, node.v);
    }
    odin3_pg_destroy(pg);
    TEST_ASSERT_TRUE(st == ODIN3_OK || odin3_module_node_end(e2->mod) == nodes);
    return st;
}

static void wb_hook(void *user, odin3_elab2 *e2, odin3_ast_id module_node) {
    (void)user;
    g_wb.lhs = wb_declarator(e2, module_node);
    uint32_t settled = 0;
    for (long limit = 0; limit < OOM_LIMIT && settled < OOM_SETTLE; limit++) {
        odin3_status st = wb_run(e2, module_node, limit);
        g_wb.failures += st == ODIN3_ERR_NO_MEMORY ? 1U : 0U;
        settled = st == ODIN3_OK ? settled + 1 : 0;
    }
    g_wb.settled = settled;
    g_wb.ran = true;
}

/* odin3_pg_temp, _switch, _case and _assign: every failing point is NO_MEMORY with no node made,
 * and the sequence settles to OK with an emittable body. */
static void test_oom_sweep_builder_primitives(void) {
    odin3_elab_test_hooks hooks = {.after_prescan = wb_hook};
    odin3_elab_test_set_hooks(&hooks);
    memset(&g_wb, 0, sizeof g_wb);
    read_src("module t(output reg [1:0] y);\nendmodule\n");
    TEST_ASSERT_TRUE_MESSAGE(g_wb.ran, et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32(OOM_SETTLE, g_wb.settled);
    TEST_ASSERT_GREATER_THAN_UINT32(10, g_wb.failures);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ODIN3_OK, rs.status, et_log(&rs));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, diag_errors(), et_log(&rs));
    expect_ir_ok();
}

/* --- the statement entry points and the select write refuse what they are not (white-box) ------
 */

static void misuse_hook(void *user, odin3_elab2 *e2, odin3_ast_id module_node) {
    (void)user;
    odin3_pg *pg = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pg_create(e2, &pg));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pg_block_reset(pg));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pg_if(pg, module_node));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pg_case_stmt(pg, module_node));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pg_if(pg, (odin3_ast_id){0}));
    odin3_sigbit bit = odin3_sb_of_bit(ODIN3_BIT_0);
    odin3_sig rhs = {&bit, 1, false};
    /* the target is a plain name: no select to write through the case method */
    odin3_pg_assign_req req = {module_node, wb_declarator(e2, module_node), &rhs, false};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pg_select_write(pg, &req));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pg_select_write(pg, NULL));
    g_wb.ran = true;
    odin3_pg_destroy(pg);
}

static void test_public_entry_points_refuse_misuse(void) {
    odin3_elab_test_hooks hooks = {.after_prescan = misuse_hook};
    odin3_elab_test_set_hooks(&hooks);
    memset(&g_wb, 0, sizeof g_wb);
    read_src("module t(output reg y);\nendmodule\n");
    TEST_ASSERT_TRUE_MESSAGE(g_wb.ran, et_log(&rs));
}

#define ALL_TESTS(X)                                                                               \
    X(test_if_else_default_mode)                                                                   \
    X(test_if_latch_default_mode)                                                                  \
    X(test_if_latch_nolatches)                                                                     \
    X(test_if_wide_condition)                                                                      \
    X(test_nonblocking_branch_has_no_case_temporary)                                               \
    X(test_case_items_and_default)                                                                 \
    X(test_case_default_first_is_last)                                                             \
    X(test_case_non_constant_items)                                                                \
    X(test_wildcards)                                                                              \
    X(test_casex_wildcard)                                                                         \
    X(test_plain_case_keeps_x)                                                                     \
    X(test_case_width)                                                                             \
    X(test_chunks_case_default_item)                                                               \
    X(test_chunks_case_added_default)                                                              \
    X(test_nested_if)                                                                              \
    X(test_read_after_switch_sees_t1)                                                              \
    X(test_full_case_attribute)                                                                    \
    X(test_synopsys_metacomment_both_flags)                                                        \
    X(test_parallel_case_and_false_attribute)                                                      \
    X(test_constant_if_folds)                                                                      \
    X(test_constant_parameter_and_dead_output)                                                     \
    X(test_constant_case_folds)                                                                    \
    X(test_constant_case_no_match_makes_no_node)                                                   \
    X(test_x_condition_is_not_folded)                                                              \
    X(test_variable_holding_a_constant_is_not_folded)                                              \
    X(test_deep_else_if_chain)                                                                     \
    X(test_variable_index_write)                                                                   \
    X(test_indexed_part_select_write)                                                              \
    X(test_indexed_part_select_minus_write)                                                        \
    X(test_ascending_target)                                                                       \
    X(test_signed_index_limits_the_positions)                                                      \
    X(test_blocking_variable_index_write)                                                          \
    X(test_index_that_selects_nothing)                                                             \
    X(test_variable_index_in_concatenation_is_an_error)                                            \
    X(test_memory_write_in_if_arm_is_pending)                                                      \
    X(test_memory_write_in_case_arm_is_pending)                                                    \
    X(test_loop_in_if_arm_is_pending)                                                              \
    X(test_register_word_variable_index_is_pending)                                                \
    X(test_public_entry_points_refuse_misuse)                                                      \
    X(test_oom_sweep_builder_primitives)                                                           \
    X(test_oom_sweep)                                                                              \
    X(test_oom_sweep_flags_and_folding)

#define RUN_ONE(name) RUN_TEST(name);

int main(void) {
    UNITY_BEGIN();
    ALL_TESTS(RUN_ONE)
    return UNITY_END();
}
